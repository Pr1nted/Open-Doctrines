#include "DialogScript.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace dlg {
namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool startsWith(const std::string& s, const char* p) {
    return s.compare(0, strlen(p), p) == 0;
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// #rgb and #rrggbb, because a script author will type both.
bool parseHex(const std::string& s, Rgb& out) {
    if (s.empty() || s[0] != '#') return false;
    const std::string h = s.substr(1);
    auto pair = [&](int i) { return hexNibble(h[i]) * 16 + hexNibble(h[i + 1]); };
    if (h.size() == 6) {
        for (char c : h) if (hexNibble(c) < 0) return false;
        out = {(uint8_t)pair(0), (uint8_t)pair(2), (uint8_t)pair(4)};
        return true;
    }
    if (h.size() == 3) {
        for (char c : h) if (hexNibble(c) < 0) return false;
        auto one = [&](int i) { const int v = hexNibble(h[i]); return (uint8_t)(v * 16 + v); };
        out = {one(0), one(1), one(2)};
        return true;
    }
    return false;
}

// A handful of names, because "{color=red}" is what somebody writing dialogue
// reaches for first and being told to look up a hex code is friction in the
// wrong place.
bool parseNamedColor(const std::string& n, Rgb& out) {
    struct Named { const char* name; Rgb rgb; };
    static const Named kNamed[] = {
        {"white",  {235, 235, 240}}, {"grey",   {150, 150, 160}},
        {"gray",   {150, 150, 160}}, {"black",  { 20,  20,  25}},
        {"red",    {225,  75,  75}}, {"green",  {110, 200, 120}},
        {"blue",   {100, 150, 235}}, {"yellow", {235, 205, 100}},
        {"orange", {235, 150,  80}}, {"purple", {180, 120, 220}},
        {"pink",   {235, 140, 190}}, {"cyan",   {110, 210, 220}},
    };
    for (const auto& e : kNamed)
        if (n == e.name) { out = e.rgb; return true; }
    return false;
}

}  // namespace

// ── Parsing ───────────────────────────────────────────────────────────────

namespace {

/// Whitespace, or off the end of the line -- both count as "nothing there".
inline bool blankAt(const std::string& s, size_t i) {
    return i >= s.size() || s[i] == ' ' || s[i] == '\t';
}

/// May a delimiter of `len` bytes at `i` OPEN a span? It must have space (or
/// the line start) behind it and something other than space in front, so the
/// apostrophe in "don't" -- letters on both sides -- never qualifies.
inline bool canOpen(const std::string& s, size_t i, size_t len) {
    const bool spaceBefore = (i == 0) || blankAt(s, i - 1) || isOpeningPunct((unsigned char)s[i - 1]);
    return spaceBefore && !blankAt(s, i + len);
}

/// May it CLOSE one? The mirror: something other than space behind it, and
/// space, the end of the line, or closing punctuation in front -- which is
/// what lets "Come 'on'!" close on the apostrophe before the "!".
inline bool canClose(const std::string& s, size_t i, size_t len) {
    if (i == 0 || blankAt(s, i - 1)) return false;
    return blankAt(s, i + len) || isClosingPunct((unsigned char)s[i + len]);
}

}  // namespace

Script parse(const std::string& source) {
    Script out;
    Page page;
    PageStyle carried;               // directives persist across page breaks
    std::string carriedPose;
    bool  carriedHasPlace = false;
    float carriedX = 0.5f, carriedY = 0.5f, carriedScale = 1.0f;
    bool  carriedFlip = false;
    bool pageHasContent = false;
    int lineNo = 0;

    auto warn = [&](const std::string& what) {
        out.warnings.push_back("line " + std::to_string(lineNo) + ": " + what);
    };

    // The style stack. Every open tag pushes, every close pops, and the style a
    // glyph gets is whatever is on top -- which is what makes effects combine
    // without the parser having to know which combinations are legal.
    std::vector<Style> stack{Style{}};
    std::vector<std::string> openTags;

    std::string pending;             // text accumulated under the current style

    auto flush = [&]() {
        if (pending.empty()) return;
        page.spans.push_back({pending, stack.back()});
        pending.clear();
        pageHasContent = true;
    };

    auto endPage = [&]() {
        flush();
        if (!pageHasContent && page.speaker.empty()) return;
        page.style = carried;
        // Every other page directive carries -- a size or an anchor is a
        // setting. A delay is a BEAT: it belongs to the one line it was
        // written under, and carrying it puts a silence in front of every
        // page after it for the rest of the conversation.
        carried.delay = 0.0f;
        page.pose = carriedPose;
        page.hasPlacement = carriedHasPlace;
        page.atX = carriedX; page.atY = carriedY;
        page.atScale = carriedScale; page.flip = carriedFlip;
        out.pages.push_back(page);
        page = Page{};
        page.speaker = out.pages.back().speaker;   // a speaker keeps talking
        pageHasContent = false;
    };

    auto applyTag = [&](const std::string& raw) {
        std::string name = raw, value;
        const size_t eq = raw.find('=');
        if (eq != std::string::npos) { name = raw.substr(0, eq); value = raw.substr(eq + 1); }
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return (char)tolower(c); });

        Style s = stack.back();
        if      (name == "b" || name == "bold")      s.fx |= FX_BOLD;
        else if (name == "i" || name == "italic")    s.fx |= FX_ITALIC;
        else if (name == "u" || name == "underline") s.fx |= FX_UNDERLINE;
        else if (name == "shake")                    s.fx |= FX_SHAKE;
        else if (name == "wave")                     s.fx |= FX_WAVE;
        else if (name == "rainbow")                  s.fx |= FX_RAINBOW;
        else if (name == "obf" || name == "obfuscate") s.fx |= FX_OBFUSCATE;
        else if (name == "strike" || name == "s")    s.fx |= FX_STRIKE;
        else if (name == "action")                   s.fx |= FX_ACTION;
        else if (name == "key") {
            if (value.empty()) { warn("{key=} with no action"); return false; }
            flush();
            Span k; k.style = stack.back(); k.keyAction = value;
            page.spans.push_back(k);
            pageHasContent = true;
            return true;                       // self-closing, nothing to push
        }
        else if (name == "choice") {
            // Not a style: a hole in the text for the box to fill. Emitted as
            // its own span so it keeps whatever styling is open around it.
            flush();
            Span echo; echo.style = stack.back(); echo.echoChoice = true;
            page.spans.push_back(echo);
            pageHasContent = true;
            return true;                       // consumed; nothing to push
        }
        else if (name == "accent") {
            s.useAccent = true;
            s.hasColor = false;
            // "{accent=60}" is 60% of the accent's brightness -- a tone of the
            // player's colour rather than a second colour.
            s.accentTone = value.empty() ? 1.0f
                                         : std::max(0.0f, (float)atof(value.c_str()) / 100.0f);
        } else if (name == "color" || name == "colour") {
            Rgb rgb;
            if (parseHex(value, rgb) || parseNamedColor(value, rgb)) {
                s.color = rgb; s.hasColor = true; s.useAccent = false;
            } else {
                warn("unknown colour '" + value + "'");
                return false;
            }
        } else if (name == "size") {
            // Relative, so a script never has to know the base size.
            s.sizeDelta = atoi(value.c_str());
        } else {
            warn("unknown tag '" + name + "'");
            return false;
        }
        flush();
        stack.push_back(s);
        openTags.push_back(name);
        return true;
    };

    auto closeTag = [&](const std::string& raw) {
        if (openTags.empty()) { warn("closing tag with nothing open"); return; }
        std::string name = raw;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return (char)tolower(c); });
        // "{/}" closes the innermost, which is what a writer means when they
        // have three tags open and do not want to spell out which is which.
        if (!name.empty() && name != openTags.back()) {
            // Aliases, so {b}...{/bold} works.
            const bool alias =
                (name == "bold" && openTags.back() == "b") ||
                (name == "b" && openTags.back() == "bold") ||
                (name == "italic" && openTags.back() == "i") ||
                (name == "i" && openTags.back() == "italic") ||
                (name == "underline" && openTags.back() == "u") ||
                (name == "u" && openTags.back() == "underline") ||
                (name == "obfuscate" && openTags.back() == "obf") ||
                (name == "obf" && openTags.back() == "obfuscate") ||
                (name == "colour" && openTags.back() == "color") ||
                (name == "color" && openTags.back() == "colour");
            if (!alias) warn("'{/" + name + "}' closes '" + openTags.back() + "'");
        }
        flush();
        stack.pop_back();
        openTags.pop_back();
        if (stack.empty()) stack.push_back(Style{});
    };

    // Markdown emphasis toggles rather than nests, the way markdown does.
    auto toggleFx = [&](uint32_t bit) {
        Style s = stack.back();
        s.fx ^= bit;
        flush();
        stack.back() = s;
    };

    auto toggleAccent = [&]() {
        Style s = stack.back();
        s.useAccent = !s.useAccent;
        s.accentTone = 1.0f;
        flush();
        stack.back() = s;
    };

    std::string line;
    size_t pos = 0;
    while (pos <= source.size()) {
        const size_t nl = source.find('\n', pos);
        line = source.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? source.size() + 1 : nl + 1;
        lineNo++;

        const std::string t = trim(line);
        if (startsWith(t, "//")) continue;

        if (t == "---") { endPage(); continue; }

        if (startsWith(t, "@")) {
            flush();
            page.speaker = trim(t.substr(1));
            continue;
        }

        if (startsWith(t, "::")) {
            std::string rest = trim(t.substr(2));
            size_t p = 0;
            while (p < rest.size()) {
                const size_t sp = rest.find(' ', p);
                std::string kv = rest.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
                p = (sp == std::string::npos) ? rest.size() : sp + 1;
                if (kv.empty()) continue;
                const size_t eq = kv.find('=');
                if (eq == std::string::npos) { warn("directive '" + kv + "' has no value"); continue; }
                const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
                if (k == "anchor") {
                    if (v == "left") carried.anchor = Anchor::Left;
                    else if (v == "center" || v == "centre") carried.anchor = Anchor::Center;
                    else if (v == "right") carried.anchor = Anchor::Right;
                    else warn("unknown anchor '" + v + "'");
                } else if (k == "size")    carried.size = std::max(6, atoi(v.c_str()));
                else if (k == "spacing")   carried.letterSpacing = (float)atof(v.c_str());
                else if (k == "line")      carried.lineSpacing = std::max(0.5f, (float)atof(v.c_str()));
                else if (k == "pad")       carried.padding = std::max(0, atoi(v.c_str()));
                else if (k == "speed")     carried.speed = std::max(1.0f, (float)atof(v.c_str()));
                else if (k == "delay")     carried.delay = std::max(0.0f, (float)atof(v.c_str()));
                else if (k == "enter") {
                    if (v == "type" || v == "typewriter") carried.enter = Enter::Typewriter;
                    else if (v == "fade")    carried.enter = Enter::Fade;
                    else if (v == "rise")    carried.enter = Enter::Rise;
                    else if (v == "instant" || v == "none") carried.enter = Enter::Instant;
                    else warn("unknown enter '" + v + "'");
                } else if (k == "pose") carriedPose = v;
                else if (k == "at") {
                    // "at=0.25,0.8" -- normalised, so it means the same thing
                    // on every screen.
                    const size_t comma = v.find(',');
                    if (comma == std::string::npos) warn("at needs x,y");
                    else {
                        carriedX = (float)atof(v.substr(0, comma).c_str());
                        carriedY = (float)atof(v.substr(comma + 1).c_str());
                        carriedHasPlace = true;
                    }
                } else if (k == "scale") {
                    carriedScale = std::max(0.05f, (float)atof(v.c_str()));
                    carriedHasPlace = true;
                } else if (k == "point") {
                    // NOT carried across pages: a pointer that outlives the
                    // page that asked for it ends up ringing the economy tab
                    // through a conversation about the navy.
                    page.pointAt = (v == "none" || v == "-") ? "" : v;
                } else if (k == "round") {
                    page.pointRound = (v == "1" || v == "true" || v == "yes");
                } else if (k == "until") {
                    // Not carried: a condition that outlives its page stops
                    // the NEXT one for a reason nobody wrote down.
                    page.until = (v == "none" || v == "-") ? "" : v;
                } else if (k == "act") {
                    page.act = v;
                } else if (k == "gate") {
                    page.gate = (v == "1" || v == "true" || v == "yes");
                } else if (k == "flip") {
                    carriedFlip = (v == "1" || v == "true" || v == "yes");
                    carriedHasPlace = true;
                }
                else warn("unknown directive '" + k + "'");
            }
            continue;
        }

        // An option line. Checked before the inline pass, because '>' at the
        // start of a line is a choice and '>' inside one closes a <wave>.
        if (t.size() > 1 && t[0] == '>' && t[1] == ' ') {
            flush();
            std::string body = t.substr(2);
            std::string key;
            const size_t arrow = body.find("->");
            if (arrow != std::string::npos) {
                key = body.substr(arrow + 2);
                body = body.substr(0, arrow);
                while (!key.empty() && key.front() == ' ') key.erase(key.begin());
                while (!key.empty() && key.back() == ' ') key.pop_back();
            }
            while (!body.empty() && body.back() == ' ') body.pop_back();
            if (body.empty()) warn("an option with no label");
            else {
                // No key given: the label doubles as one. Fine for a script
                // nobody branches on, and it keeps simple menus short.
                page.choices.push_back({body, key.empty() ? body : key});
                pageHasContent = true;
            }
            continue;
        }

        // A blank line inside a page is a paragraph break the author asked for.
        if (t.empty()) {
            if (pageHasContent || !pending.empty()) pending += '\n';
            continue;
        }

        // ── Inline pass ──
        for (size_t i = 0; i < line.size(); ) {
            const char c = line[i];

            if (c == '\\' && i + 1 < line.size()) { pending += line[i + 1]; i += 2; continue; }

            if (c == '{') {
                const size_t end = line.find('}', i);
                if (end == std::string::npos) { pending += c; i++; continue; }
                const std::string inner = line.substr(i + 1, end - i - 1);
                if (!inner.empty() && inner[0] == '/') closeTag(inner.substr(1));
                else if (!applyTag(inner)) { /* warned; the text is dropped, not shown raw */ }
                i = end + 1;
                continue;
            }

            if (c == '*' && i + 1 < line.size() && line[i + 1] == '*') {
                toggleFx(FX_BOLD); i += 2; continue;
            }
            if (c == '_' && i + 1 < line.size() && line[i + 1] == '_') {
                toggleFx(FX_UNDERLINE); i += 2; continue;
            }

            // The short delimiters. `on` says whether this effect is already
            // running, because that decides whether we are looking for an
            // opener or a closer -- and they have opposite tightness rules.
            {
                const uint32_t nowFx = stack.back().fx;
                bool handled = false;
                const struct { char ch; uint32_t bit; } kShort[] = {
                    {'*',  FX_ACTION},      // **bold** was taken above
                    {'/',  FX_ITALIC},
                    {'_',  FX_UNDERLINE},
                    {'-',  FX_STRIKE},
                    {'<',  FX_WAVE},        // closed by '>', handled below
                    {'|',  FX_RAINBOW},
                    {'\'', FX_SHAKE},
                };
                for (const auto& d : kShort) {
                    if (c != d.ch) continue;
                    // <wave> is the one pair that is not the same character
                    // at both ends, so its closer is '>' and never '<'.
                    if (d.ch == '<' && (nowFx & FX_WAVE)) break;
                    const bool on = (nowFx & d.bit) != 0;
                    if (on ? canClose(line, i, 1) : canOpen(line, i, 1)) {
                        toggleFx(d.bit); i += 1; handled = true;
                    }
                    break;
                }
                if (handled) continue;
                if (c == '>' && (stack.back().fx & FX_WAVE) && canClose(line, i, 1)) {
                    toggleFx(FX_WAVE); i += 1; continue;
                }
                // U+00A7 SECTION SIGN, two bytes in UTF-8.
                //
                // NO BOUNDARY TEST ON THIS ONE, unlike '*' and '_'.
                //
                // canOpen/canClose ask for a space or ASCII punctuation beside
                // the marker, which is what stops the apostrophe in "it's" and
                // the star in "2*3" from being read as markup. Japanese and
                // Chinese have neither: they are written without spaces and
                // their punctuation is 。、「」, three bytes each, so both tests
                // looked at one byte of a multi-byte character and said no.
                // The result was that § opened only at the very start of a
                // line and never closed at all -- "§**ほかはすべて灰色になり**§、"
                // drew both section signs as text and left the accent colour
                // running to the end of the page.
                //
                // The boundary test is not needed here anyway: § is not a
                // character any of these twenty languages writes, so there is
                // nothing for it to be confused with, and tools/dialog_check.py
                // fails a script whose § do not pair up. '*' and '_' keep the
                // test, because they DO occur inside ordinary words.
                if ((unsigned char)c == 0xC2 && i + 1 < line.size() &&
                    (unsigned char)line[i + 1] == 0xA7) {
                    toggleAccent(); i += 2; continue;
                }
            }

            pending += c;
            i++;
        }
        // A newline in the source is a newline on screen. Wrapping handles the
        // rest; this is only for where the author insisted.
        pending += '\n';
    }

    if (!openTags.empty()) warn("'" + openTags.back() + "' was never closed");
    endPage();

    // A trailing newline on every page, from the loop above, would draw an
    // empty last line in every box.
    for (auto& p : out.pages) {
        while (!p.spans.empty() && !p.spans.back().text.empty() &&
               p.spans.back().text.back() == '\n') {
            p.spans.back().text.pop_back();
            if (p.spans.back().text.empty()) p.spans.pop_back();
        }
    }
    return out;
}

// ── Line breaking ─────────────────────────────────────────────────────────

bool isClosingPunct(int cp) {
    switch (cp) {
        case ',': case '.': case ';': case ':': case '!': case '?':
        case ')': case ']': case '}': case '%':
        case '>':     // closes <wave>, and never starts a line
        case 0x2019:  // ’
        case 0x201D:  // ”
        case 0x00BB:  // »
        case 0x2026:  // …
            return true;
        default: return false;
    }
}

bool isOpeningPunct(int cp) {
    switch (cp) {
        case '(': case '[': case '{':
        case '<':     // opens <wave>, and never ends a line. Also what lets
                      // markup nest straight after it: an opener needs space
                      // or an opening bracket behind it, and "<*Sigh*>" has
                      // neither unless '<' counts as one.
        case 0x2018:  // ‘
        case 0x201C:  // “
        case 0x00AB:  // «
            return true;
        default: return false;
    }
}

namespace {

bool isDigit(int cp) { return cp >= '0' && cp <= '9'; }

// Would breaking BEFORE index i read badly, ignoring width?
//
// The space at i-1 is the break; what matters is what sits either side of it.
bool badBreakBefore(const std::vector<int>& cps, int i) {
    if (i <= 0 || i >= (int)cps.size()) return true;
    if (cps[i - 1] != ' ') return true;             // only spaces are opportunities

    // Rule 2: nothing opens at the end of a line, nothing closes at the start.
    if (i >= 2 && isOpeningPunct(cps[i - 2])) return true;
    if (isClosingPunct(cps[i])) return true;

    // Rule 3: a number keeps its unit, and an abbreviation keeps its noun.
    // "1914 " followed by a word, or "Mr. " -- both read as one thing.
    {
        int j = i - 2;
        bool sawDigit = false;
        while (j >= 0 && isDigit(cps[j])) { sawDigit = true; j--; }
        if (sawDigit && (j < 0 || cps[j] == ' ')) return true;
    }
    if (i >= 2 && cps[i - 2] == '.') {
        // A full stop ends a sentence -- an excellent place to break. An
        // abbreviation does not: it is short and capitalised.
        int j = i - 3, len = 0;
        while (j >= 0 && cps[j] != ' ') { j--; len++; }
        if (len > 0 && len <= 3) return true;       // "Mr.", "St.", "No."
    }
    return false;
}

}  // namespace

std::vector<int> breakLines(const std::vector<int>& cps,
                            const std::vector<float>& advances,
                            float maxWidth) {
    std::vector<int> starts;
    const int n = (int)cps.size();
    if (n == 0) return starts;
    starts.push_back(0);
    if (maxWidth <= 0.0f) return starts;

    int lineStart = 0;
    float width = 0.0f;
    int lastGood = -1;              // where we could break, if we had to

    for (int i = 0; i < n; ++i) {
        if (cps[i] == '\n') {       // the author insisted
            starts.push_back(i + 1);
            lineStart = i + 1;
            width = 0.0f;
            lastGood = -1;
            continue;
        }
        if (i > lineStart && !badBreakBefore(cps, i)) lastGood = i;

        width += (i < (int)advances.size()) ? advances[i] : 0.0f;
        if (width <= maxWidth || i == lineStart) continue;

        // Over. Fall back to the last legal break; if there was none the word
        // is longer than the line and gets cut, because the alternative is
        // drawing off the edge of the box.
        const int br = (lastGood > lineStart) ? lastGood : i;
        starts.push_back(br);
        lineStart = br;
        lastGood = -1;
        width = 0.0f;
        for (int k = br; k <= i; ++k) width += (k < (int)advances.size()) ? advances[k] : 0.0f;
    }

    // Rule 4: no widow. A last line carrying one short word looks like a
    // mistake, and moving a word down from the line above costs nothing.
    if (starts.size() >= 2) {
        const int lastStart = starts.back();
        int words = 0;
        for (int i = lastStart; i < n; ++i)
            if (cps[i] == ' ' && i + 1 < n && cps[i + 1] != ' ') words++;
        if (words == 0) {                       // one word on the last line
            const int prevStart = starts[starts.size() - 2];
            // The break before it, i.e. the start of the last word of the
            // previous line. Only if that leaves the previous line non-empty.
            int cand = -1;
            for (int i = lastStart - 1; i > prevStart; --i)
                if (!badBreakBefore(cps, i)) { cand = i; break; }
            if (cand > prevStart) {
                float w = 0.0f;
                for (int i = cand; i < n; ++i)
                    w += (i < (int)advances.size()) ? advances[i] : 0.0f;
                if (w <= maxWidth) starts.back() = cand;
            }
        }
    }
    return starts;
}

}  // namespace dlg
