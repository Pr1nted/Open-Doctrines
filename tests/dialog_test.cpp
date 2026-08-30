// The dialogue markup and where it puts its line breaks.
//
// WHY THESE EXIST. Both halves fail quietly. A tag typo does not crash, it just
// silently loses the emphasis somebody wrote, and nobody notices until the
// tutorial ships reading flat. A line breaker does not crash either; it puts a
// comma at the start of a line, or splits "1914" from "offensive", and the text
// merely looks slightly wrong in a way that is hard to attribute.
//
// Both are pure text handling, so neither needs a window, a font, or a map --
// advances are handed in, and a fixed width per glyph makes every expectation
// here countable by hand.
#include "dialog/DialogScript.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;

static void ok(bool cond, const char* what) {
    g_checks++;
    printf(cond ? "  ok    %s\n" : "  FAIL  %s\n", what);
    if (!cond) g_failed++;
}

static void section(const char* s) { printf("\n=== %s ===\n", s); }

// Every glyph one unit wide, so "a line of 10" means ten characters.
static std::vector<int> cps(const std::string& s) {
    std::vector<int> v;
    for (unsigned char c : s) v.push_back(c);
    return v;
}
static std::vector<float> flat(size_t n, float w = 1.0f) { return std::vector<float>(n, w); }

// The text of line `l`, for expectations that read like the prose does.
static std::string lineText(const std::string& src, const std::vector<int>& starts, int l) {
    const int a = starts[l];
    const int b = (l + 1 < (int)starts.size()) ? starts[l + 1] : (int)src.size();
    std::string out = src.substr(a, b - a);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    return out;
}

static bool spanHas(const dlg::Page& p, const char* text, uint32_t fx) {
    for (const auto& s : p.spans)
        if (s.text.find(text) != std::string::npos && (s.style.fx & fx) == fx) return true;
    return false;
}

int main() {
    // ── The markup ────────────────────────────────────────────────────────
    section("markdown emphasis");
    {
        dlg::Script s = dlg::parse("@X\nplain **bold** /ital/ *act* __under__\n");
        ok(s.pages.size() == 1, "one page");
        ok(s.pages[0].speaker == "X", "the speaker is read off the @ line");
        ok(spanHas(s.pages[0], "bold", dlg::FX_BOLD), "**bold** is bold");
        ok(spanHas(s.pages[0], "ital", dlg::FX_ITALIC), "/ital/ is italic");
        // Single * is a STAGE DIRECTION here, not italic: dialogue needs
        // *dialed in* far more often than it needs a second emphasis.
        ok(spanHas(s.pages[0], "act", dlg::FX_ACTION), "*act* is an action");
        ok(spanHas(s.pages[0], "under", dlg::FX_UNDERLINE), "__under__ is underlined");
        ok(spanHas(s.pages[0], "plain", 0) && !spanHas(s.pages[0], "plain", dlg::FX_BOLD),
           "the text around them is not");
    }

    section("effects combine");
    {
        // The whole reason the style is a bitmask.
        dlg::Script s = dlg::parse("{shake}{rainbow}**doom**{/rainbow}{/shake}\n");
        ok(spanHas(s.pages[0], "doom", dlg::FX_SHAKE | dlg::FX_RAINBOW | dlg::FX_BOLD),
           "shake + rainbow + bold on one run");
    }

    section("colour");
    {
        dlg::Script s = dlg::parse("{color=#ff8040}a{/color}{color=red}b{/color}"
                                   "{accent}c{/accent}{accent=60}d{/accent}\n");
        ok(s.warnings.empty(), "no warnings for valid colours");
        bool hex = false, named = false, acc = false, tone = false;
        for (const auto& sp : s.pages[0].spans) {
            if (sp.text == "a") hex = sp.style.hasColor && sp.style.color.r == 0xFF &&
                                      sp.style.color.g == 0x80 && sp.style.color.b == 0x40;
            if (sp.text == "b") named = sp.style.hasColor;
            if (sp.text == "c") acc = sp.style.useAccent && sp.style.accentTone == 1.0f;
            if (sp.text == "d") tone = sp.style.useAccent && sp.style.accentTone > 0.55f &&
                                       sp.style.accentTone < 0.65f;
        }
        ok(hex, "#rrggbb");
        ok(named, "a named colour");
        ok(acc, "{accent} follows the player's palette");
        ok(tone, "{accent=60} is a tone of it, not a second colour");
    }

    section("a bad tag is reported, not silently dropped");
    {
        dlg::Script s = dlg::parse("{wobble}x{/wobble}\n");
        ok(!s.warnings.empty(), "an unknown tag warns");
        dlg::Script c = dlg::parse("{color=nonsense}x{/color}\n");
        ok(!c.warnings.empty(), "an unknown colour warns");
        dlg::Script u = dlg::parse("{shake}never closed\n");
        ok(!u.warnings.empty(), "an unclosed tag warns");
    }

    section("escapes and pages");
    {
        dlg::Script s = dlg::parse("a \\{not a tag\\} b\n---\nsecond\n");
        ok(s.pages.size() == 2, "--- splits pages");
        ok(s.pages[0].spans[0].text.find("{not a tag}") != std::string::npos,
           "\\{ is a literal brace");
    }

    section("directives");
    {
        dlg::Script s = dlg::parse(":: anchor=center size=28 speed=12 enter=fade pad=40\n"
                                   "hello\n---\nstill centred\n");
        ok(s.pages[0].style.anchor == dlg::Anchor::Center, "anchor=center");
        ok(s.pages[0].style.size == 28, "size");
        ok(s.pages[0].style.speed == 12.0f, "speed");
        ok(s.pages[0].style.enter == dlg::Enter::Fade, "enter=fade");
        ok(s.pages[0].style.padding == 40, "pad");
        ok(s.pages[1].style.anchor == dlg::Anchor::Center,
           "directives carry to the next page until changed");
        ok(s.pages[1].speaker == s.pages[0].speaker, "so does the speaker");
    }

    // ── Line breaking ─────────────────────────────────────────────────────
    section("words are never split");
    {
        const std::string t = "the quick brown fox jumps";
        auto st = dlg::breakLines(cps(t), flat(t.size()), 10.0f);
        bool clean = true;
        for (size_t l = 0; l < st.size(); ++l) {
            const std::string s = lineText(t, st, (int)l);
            if (!s.empty() && s.front() == ' ') clean = false;
            // Every line must be whole words.
            if (st[l] > 0 && t[st[l] - 1] != ' ') clean = false;
        }
        ok(clean, "every break is at a space");
        ok(st.size() >= 2, "and it did wrap");
    }

    section("punctuation stays where it belongs");
    {
        ok(dlg::isClosingPunct(',') && dlg::isClosingPunct('.') && dlg::isClosingPunct(')'),
           "closing punctuation is known");
        ok(dlg::isOpeningPunct('(') && dlg::isOpeningPunct('['),
           "opening punctuation is known");
        // A line may not begin with the comma that ends the previous clause.
        const std::string t = "aaaa bbbb , cccc";
        auto st = dlg::breakLines(cps(t), flat(t.size()), 10.0f);
        bool leadsWithPunct = false;
        for (size_t l = 1; l < st.size(); ++l)
            if (dlg::isClosingPunct(t[st[l]])) leadsWithPunct = true;
        ok(!leadsWithPunct, "no line starts with closing punctuation");
    }

    section("an opening bracket is not stranded");
    {
        const std::string t = "aaaa bbbb ( cccc dddd";
        auto st = dlg::breakLines(cps(t), flat(t.size()), 12.0f);
        bool stranded = false;
        for (size_t l = 0; l + 1 < st.size(); ++l) {
            const std::string s = lineText(t, st, (int)l);
            if (!s.empty() && dlg::isOpeningPunct((unsigned char)s.back())) stranded = true;
        }
        ok(!stranded, "no line ends on an opening bracket");
    }

    section("a number keeps its noun, an abbreviation keeps its name");
    {
        const std::string t = "in 1914 everybody";
        auto st = dlg::breakLines(cps(t), flat(t.size()), 12.0f);
        bool split = false;
        for (size_t l = 1; l < st.size(); ++l)
            if (t.compare(st[l], 9, "everybody") == 0) split = true;
        ok(!split, "1914 is not parted from what it counts");

        const std::string m = "told Mr. Chamberlain today";
        auto ms = dlg::breakLines(cps(m), flat(m.size()), 14.0f);
        bool cut = false;
        for (size_t l = 1; l < ms.size(); ++l)
            if (m.compare(ms[l], 12, "Chamberlain ") == 0) cut = true;
        ok(!cut, "Mr. keeps its Chamberlain");
    }

    section("explicit newlines are obeyed");
    {
        const std::string t = "one\ntwo";
        auto st = dlg::breakLines(cps(t), flat(t.size()), 100.0f);
        ok(st.size() == 2, "a newline breaks even when the line is nowhere near full");
    }

    section("no widow");
    {
        // Sized so the naive break leaves exactly one short word alone.
        const std::string t = "aaaa bbbb cccc dd";
        auto st = dlg::breakLines(cps(t), flat(t.size()), 14.0f);
        const std::string last = lineText(t, st, (int)st.size() - 1);
        ok(last.find(' ') != std::string::npos || st.size() == 1,
           "the last line is never a single word on its own");
    }

    section("a word longer than the line is cut rather than overflowing");
    {
        const std::string t = "aaaaaaaaaaaaaaaaaaaaaaaa";
        auto st = dlg::breakLines(cps(t), flat(t.size()), 8.0f);
        ok(st.size() >= 3, "it is broken up");
        bool fits = true;
        for (size_t l = 0; l + 1 < st.size(); ++l)
            if (st[l + 1] - st[l] > 8) fits = false;
        ok(fits, "and every piece fits the width");
    }

    section("degenerate input does not crash");
    {
        ok(dlg::parse("").pages.empty(), "an empty file is an empty script");
        ok(dlg::breakLines({}, {}, 10.0f).empty(), "no glyphs, no lines");
        ok(dlg::breakLines(cps("abc"), flat(3), 0.0f).size() == 1, "zero width still answers");
        dlg::parse("{/nothing}\n");                       // must not crash
        dlg::parse("{unterminated\n");
        ok(true, "stray and unterminated tags survive parsing");
    }

    // ── the short delimiters, and the prose they must not eat ──
    //
    // These are the cases the tightness rule exists for. Apostrophes and
    // hyphens are everywhere in dialogue; if they open effects, a single
    // contraction silently shakes the rest of the paragraph.
    section("punctuation inside a word is left alone");
    {
        auto fxOf = [](const std::string& src, uint32_t bit) {
            auto sc = dlg::parse(src);
            if (sc.pages.empty()) return false;
            for (const auto& sp : sc.pages[0].spans)
                if (sp.style.fx & bit) return true;
            return false;
        };
        auto textOf = [](const std::string& src) {
            auto sc = dlg::parse(src);
            std::string out;
            if (!sc.pages.empty())
                for (const auto& sp : sc.pages[0].spans) out += sp.text;
            while (!out.empty() && out.back() == '\n') out.pop_back();
            return out;
        };
        ok(!fxOf("I don't have enough to do it\n", dlg::FX_SHAKE),
           "an apostrophe in \"don't\" does not start a shake");
        ok(textOf("I don't have enough\n") == "I don't have enough",
           "and the apostrophe is still in the text");
        ok(!fxOf("a well-known problem\n", dlg::FX_STRIKE),
           "a hyphen inside \"well-known\" does not strike");
        ok(!fxOf("and/or, either way\n", dlg::FX_ITALIC),
           "a slash inside \"and/or\" does not italicise");
        ok(textOf("a well-known problem\n") == "a well-known problem",
           "the hyphen survives too");
    }

    section("the short delimiters do work where they are meant to");
    {
        auto spanFx = [](const std::string& src, const std::string& want) {
            auto sc = dlg::parse(src);
            if (sc.pages.empty()) return 0u;
            for (const auto& sp : sc.pages[0].spans)
                if (sp.text.find(want) != std::string::npos) return sp.style.fx;
            return 0u;
        };
        ok(spanFx("Come 'on'!\n", "on") & dlg::FX_SHAKE, "'on' shakes");
        ok(spanFx("a /tiny little favour,/ could you\n", "tiny") & dlg::FX_ITALIC,
           "/italic/ italicises");
        ok(spanFx("*dialed in* Hello\n", "dialed") & dlg::FX_ACTION,
           "*action* is an action");
        ok(spanFx("**never ran a country**\n", "never") & dlg::FX_BOLD, "**bold** is bold");
        ok(spanFx("_underlined_ word\n", "underlined") & dlg::FX_UNDERLINE,
           "_underline_ underlines");
        ok(spanFx("-gone- now\n", "gone") & dlg::FX_STRIKE, "-strike- strikes");
        ok(spanFx("<wave> ok\n", "wave") & dlg::FX_WAVE, "<wave> waves");
        ok(spanFx("|rainbow| ok\n", "rainbow") & dlg::FX_RAINBOW, "|rainbow| is a rainbow");
    }

    section("the accent sign toggles the accent colour");
    {
        auto sc = dlg::parse("What \u00a7**Pr1nted**\u00a7?\n");
        bool sawAccent = false, sawPlain = false;
        for (const auto& sp : sc.pages[0].spans) {
            if (sp.text.find("Pr1nted") != std::string::npos && sp.style.useAccent) sawAccent = true;
            if (sp.text.find("What") != std::string::npos && !sp.style.useAccent) sawPlain = true;
        }
        ok(sawAccent, "the name is in the accent colour");
        ok(sawPlain, "and the words around it are not");
    }

    section("the accent sign works in a script written without spaces");
    {
        // JAPANESE AND CHINESE HAVE NO SPACES, and their punctuation is three
        // bytes wide. The delimiter rule asked for a space or ASCII
        // punctuation beside the marker and read ONE BYTE of 。 to decide, so §
        // never closed: the section signs were drawn as text and the accent
        // colour ran on to the end of the page. Written out per position,
        // because the bug only showed at the ones English never produces --
        // against a full stop, against a comma, and against a bare kana.
        auto sc = dlg::parse("\u3002\u00a7**\u307b\u304b**\u00a7\u3001\u4eca\n");
        std::string accented, plain;
        for (const auto& sp : sc.pages[0].spans)
            (sp.style.useAccent ? accented : plain) += sp.text;
        ok(accented == "\u307b\u304b", "the emphasised run is the accented one");
        ok(plain == "\u3002\u3001\u4eca", "and the section signs are not drawn");

        // The closing marker against a kana, which is the commonest position
        // of all in Japanese and the one that left the accent stuck on.
        auto sc2 = dlg::parse("\u00a7**\u53d6\u308a**\u00a7\u306b\n");
        std::string plain2;
        for (const auto& sp : sc2.pages[0].spans)
            if (!sp.style.useAccent) plain2 += sp.text;
        ok(plain2 == "\u306b", "the accent stops where the closing sign is");

        // And the ASCII case still behaves: the rule was relaxed for § only,
        // so nothing that used to work may stop working.
        auto sc3 = dlg::parse("What \u00a7**Pr1nted**\u00a7?\n");
        std::string plain3;
        for (const auto& sp : sc3.pages[0].spans)
            if (!sp.style.useAccent) plain3 += sp.text;
        ok(plain3 == "What ?", "English is unchanged");
    }

    section("a run of > lines makes the page a choice");
    {
        dlg::Script s = dlg::parse(
            "@P\nSo which is it?\n> Show me the basics -> basics\n> Talk specifics\n");
        ok(s.pages.size() == 1, "still one page");
        ok(s.pages[0].choices.size() == 2, "two options");
        ok(s.pages[0].choices[0].label == "Show me the basics", "the label is the prose");
        ok(s.pages[0].choices[0].key == "basics", "and -> gives the key");
        ok(s.pages[0].choices[1].key == "Talk specifics",
           "without ->, the label doubles as the key");
        bool prose = false;
        for (const auto& sp : s.pages[0].spans)
            if (sp.text.find("which is it") != std::string::npos) prose = true;
        ok(prose, "the page keeps its own text");
    }

    section("a page can point at a named piece of the interface");
    {
        dlg::Script s = dlg::parse(
            "@M\n:: point=tab.economy gate=1\nLook here.\n---\n@M\nAnd now anywhere.\n");
        ok(s.pages.size() == 2, "two pages");
        ok(s.pages[0].pointAt == "tab.economy", "the name is read");
        ok(s.pages[0].gate, "and the gate with it");
        // Carrying a pointer forward would ring the economy tab through a
        // conversation about something else entirely.
        ok(s.pages[1].pointAt.empty(), "a pointer does not carry to the next page");
        ok(!s.pages[1].gate, "and neither does the gate");
        ok(s.warnings.empty(), "no warnings");
    }

    section("a page can wait for the game, and ask it to do something");
    {
        dlg::Script s = dlg::parse(
            "@M\n:: until=destroyed:KES act=rebellion\nTake them.\n---\n@M\nDone.\n");
        ok(s.pages.size() == 2, "two pages");
        ok(s.pages[0].until == "destroyed:KES", "the condition is read verbatim");
        ok(s.pages[0].act == "rebellion", "and so is the action");
        // A condition that outlives its page stops the next one for a reason
        // nobody wrote down.
        ok(s.pages[1].until.empty(), "neither carries to the next page");
        ok(s.pages[1].act.empty(), "the action does not carry either");
        ok(s.warnings.empty(), "no warnings");
    }

    section("a delay is a beat, not a setting");
    {
        dlg::Script s = dlg::parse("@M\n:: delay=1.1\nOne.\n---\n@M\nTwo.\n---\n@M\nThree.\n");
        ok(s.pages.size() == 3, "three pages");
        ok(s.pages[0].style.delay > 1.0f, "the page it was written on waits");
        // Carried, it would put a silence in front of every page for the rest
        // of the conversation.
        ok(s.pages[1].style.delay == 0.0f, "the next page does not");
        ok(s.pages[2].style.delay == 0.0f, "nor the one after that");
        // ...while an ordinary setting still carries, which is the contrast.
        dlg::Script t = dlg::parse("@M\n:: size=30\nOne.\n---\n@M\nTwo.\n");
        ok(t.pages[1].style.size == 30, "size, being a setting, still carries");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
