#include "DialogBox.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace dlg {
namespace {

// UTF-8 in, codepoints out. The same decoder shape as Game::drawHybridText,
// which is where the hybrid-font idea comes from.
std::vector<int> decodeUtf8(const std::string& s) {
    std::vector<int> out;
    for (size_t i = 0; i < s.size(); ) {
        const unsigned char c = (unsigned char)s[i];
        int cp = c, len = 1;
        if (c >= 0xF0 && i + 3 < s.size()) {
            cp = ((c & 0x07) << 18) | ((s[i+1] & 0x3F) << 12) |
                 ((s[i+2] & 0x3F) << 6) | (s[i+3] & 0x3F); len = 4;
        } else if (c >= 0xE0 && i + 2 < s.size()) {
            cp = ((c & 0x0F) << 12) | ((s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F); len = 3;
        } else if (c >= 0xC0 && i + 1 < s.size()) {
            cp = ((c & 0x1F) << 6) | (s[i+1] & 0x3F); len = 2;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

// Deterministic per-glyph noise. Not rand(): the same glyph must jitter the
// same way on every machine in a multiplayer session and on every frame of a
// recorded timelapse, and it must not consume the simulation's random stream.
float hashNoise(int index, int salt, float t) {
    const float a = std::sin((float)(index * 12.9898 + salt * 78.233) + t) * 43758.5453f;
    return (a - std::floor(a)) * 2.0f - 1.0f;
}

Color hueShift(float h) {
    // h in [0,1). A cheap HSV->RGB at full saturation and value; the dialogue
    // box is the only caller and it always wants a vivid one.
    const float r = std::fabs(h * 6.0f - 3.0f) - 1.0f;
    const float g = 2.0f - std::fabs(h * 6.0f - 2.0f);
    const float b = 2.0f - std::fabs(h * 6.0f - 4.0f);
    auto ch = [](float v) { return (unsigned char)(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    return {ch(r), ch(g), ch(b), 255};
}

// What an obfuscated glyph shows instead of itself. Restricted to shapes that
// exist in the pixel font and read as "noise" rather than as another language.
int obfuscatedGlyph(int index, float t) {
    static const char* kSoup = "#@$%&*?!/\\|=+<>~^";
    const int n = (int)strlen(kSoup);
    // Re-rolled a few times a second, so it churns without strobing.
    const int step = (int)(t * 9.0f);
    const unsigned h = (unsigned)(index * 2654435761u) ^ (unsigned)(step * 40503u);
    return kSoup[h % (unsigned)n];
}

Color tone(Color c, float k) {
    auto ch = [&](unsigned char v) {
        return (unsigned char)std::clamp((int)std::lround(v * k), 0, 255);
    };
    return {ch(c.r), ch(c.g), ch(c.b), c.a};
}

}  // namespace

// ── Loading ───────────────────────────────────────────────────────────────

bool Box::open(const std::string& dialogDir, const std::string& name,
               const std::string& language) {
    // A LANGUAGE IS A DIRECTORY, and English is the fallback.
    //
    // Not one file with every translation in it: dialogue is the thing most
    // likely to be contributed by somebody who is not a programmer, and asking
    // them to edit a file that also contains eleven other languages is asking
    // for a merge conflict. One tree per language, same filenames throughout.
    auto tryLoad = [&](const std::string& lang) -> bool {
        const std::string path = dialogDir + "/" + lang + "/" + name + ".oddlg";
        std::ifstream f(path);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        m_script = parse(ss.str());
        m_script.language = lang;
        return true;
    };

    if (!tryLoad(language) && !(language != "en" && tryLoad("en"))) {
        m_open = false;
        return false;
    }
    if (m_script.pages.empty()) { m_open = false; return false; }

    m_page = 0;
    refreshPageScript();   // the page decides which font the whole line uses
    m_open = true;
    m_dirty = true;
    m_revealed = 0.0f;
    m_pageAge = 0.0f;
    m_pickedKey.clear();      // see close(): a new script inherits no answer
    return true;
}

void Box::openScript(Script script) {
    m_script = std::move(script);
    m_page = 0;
    refreshPageScript();   // the page decides which font the whole line uses
    m_open = !m_script.pages.empty();
    m_dirty = true;
    m_revealed = 0.0f;
    m_pageAge = 0.0f;
    m_pickedKey.clear();
}

void Box::jumpTo(int page) {
    if (m_script.pages.empty()) return;
    m_page = std::clamp(page, 0, (int)m_script.pages.size() - 1);
    refreshPageScript();   // the page decides which font the whole line uses
    m_revealed = 0.0f;
    m_pageAge = 0.0f;
    m_dirty = true;
    m_open = true;
}

void Box::setBounds(Rectangle r) {
    if (r.x == m_bounds.x && r.y == m_bounds.y &&
        r.width == m_bounds.width && r.height == m_bounds.height) return;
    m_bounds = r;
    m_dirty = true;
}

Anchor Box::anchor() const {
    if (m_page < 0 || m_page >= (int)m_script.pages.size()) return Anchor::Left;
    return m_script.pages[m_page].style.anchor;
}

// ── Layout ────────────────────────────────────────────────────────────────

Font Box::fontFor(int codepoint) const {
    // ONE FONT PER PAGE, not one per glyph, once a page needs the atlas.
    //
    // Switching per codepoint means a Ukrainian sentence draws its letters from
    // the atlas and its punctuation from the pixel font -- two faces with two
    // sets of metrics -- so every "?" and "," in the line sits a pixel or two
    // off the baseline the words are on. On an all-ASCII page there is nothing
    // to switch between and this changes nothing, which is why the English
    // rendering is untouched.
    if (m_pageNeedsAtlas && m_unicode.texture.id > 0) return m_unicode;
    const bool ascii = (codepoint >= 32 && codepoint <= 126);
    if (!ascii && m_unicode.texture.id > 0) return m_unicode;
    return m_ascii.texture.id > 0 ? m_ascii : GetFontDefault();
}

// Does the page now showing contain anything the pixel font cannot draw?
void Box::refreshPageScript() {
    m_pageNeedsAtlas = false;
    if (m_page < 0 || m_page >= (int)m_script.pages.size()) return;
    for (const auto& sp : m_script.pages[m_page].spans)
        if (!isAsciiOnly(sp.text)) { m_pageNeedsAtlas = true; return; }
}

// THE NAME PLATE AND THE CHOICES ARE WHOLE STRINGS, not codepoints.
//
// fontFor() above answers per glyph, which is what the body text needs because
// it walks codepoints. The plate and the choice rows draw a string in one call,
// and both drew it in a font that has ASCII and nothing else -- fine while the
// only speakers were "Pr1nted" and the only choices were English, and five tofu
// boxes the moment either was translated.
//
// A pure-ASCII string still takes exactly the path it took before, so nothing
// about the English rendering moves.
bool Box::isAsciiOnly(const std::string& s) {
    for (unsigned char c : s)
        if (c > 126 || (c < 32 && c != '\t')) return false;
    return true;
}

float Box::glyphAdvanceFor(int codepoint, int size) const {
    const Font f = fontFor(codepoint);
    const int gi = GetGlyphIndex(f, codepoint);
    if (gi >= 0 && gi < f.glyphCount) {
        float adv = (float)f.glyphs[gi].advanceX;
        if (adv > 0) return adv * size / (float)f.baseSize;
        return f.recs[gi].width * size / (float)f.baseSize;
    }
    return size * 0.6f;
}

void Box::layout() {
    m_glyphs.clear();
    m_lineCount = 0;
    m_textHeight = 0;
    m_dirty = false;
    if (m_page < 0 || m_page >= (int)m_script.pages.size()) return;

    const Page& page = m_script.pages[m_page];
    const PageStyle& ps = page.style;

    // Flatten the spans to glyphs, each carrying its own style. From here on
    // the effects can be per glyph, which is the whole reason for doing it.
    std::vector<int> cps;
    std::vector<float> advances;
    std::vector<Style> styles;
    std::vector<int> sizes;
    for (const Span& sp : page.spans) {
        const int size = std::max(6, ps.size + sp.style.sizeDelta);
        // {choice} carries no words of its own: it stands for whatever the
        // player last picked, which is only known now.
        std::string resolved;
        if (!sp.keyAction.empty())
            resolved = m_keyResolver ? m_keyResolver(sp.keyAction) : ("[" + sp.keyAction + "]");
        const std::string& text = !sp.keyAction.empty() ? resolved
                                : sp.echoChoice        ? m_pickedLabel
                                                       : sp.text;
        for (int cp : decodeUtf8(text)) {
            cps.push_back(cp);
            styles.push_back(sp.style);
            sizes.push_back(size);
            // Letter spacing is part of the advance so the line breaker, which
            // knows nothing about fonts, still measures what will be drawn.
            //
            // AND SO IS THE ROOM THE FAKED WEIGHTS TAKE. Bold is the glyph
            // struck a second time one pixel over, so its ink is a pixel wider
            // than the font says -- charge for it, or every bold word is drawn
            // a pixel tighter per letter than the text around it and reads as
            // cramped. Which it did.
            float adv = (cp == '\n') ? 0.0f
                                      : glyphAdvanceFor(cp, size) + ps.letterSpacing;
            if (cp != '\n' && (sp.style.fx & FX_BOLD)) adv += 1.0f;
            advances.push_back(adv);
        }
    }
    if (cps.empty()) return;

    const float innerW = std::max(16.0f, m_bounds.width - ps.padding * 2.0f);
    const std::vector<int> starts = breakLines(cps, advances, innerW);

    // Line heights come from the largest glyph on each line: a {size=+8} word
    // must not overlap the line above it.
    const int lines = (int)starts.size();
    std::vector<float> lineH(lines, (float)ps.size);
    std::vector<float> lineW(lines, 0.0f);
    for (int l = 0; l < lines; ++l) {
        const int a = starts[l];
        const int b = (l + 1 < lines) ? starts[l + 1] : (int)cps.size();
        for (int i = a; i < b; ++i) {
            if (cps[i] == '\n') continue;
            lineH[l] = std::max(lineH[l], (float)sizes[i]);
            lineW[l] += advances[i];
        }
        // A trailing space should not push a centred line off centre.
        for (int i = b - 1; i >= a && cps[i] == ' '; --i) lineW[l] -= advances[i];
    }

    float y = 0.0f;
    int order = 0;
    for (int l = 0; l < lines; ++l) {
        const int a = starts[l];
        const int b = (l + 1 < lines) ? starts[l + 1] : (int)cps.size();
        float x = 0.0f;
        if (ps.anchor == Anchor::Center)      x = (innerW - lineW[l]) * 0.5f;
        else if (ps.anchor == Anchor::Right)  x = innerW - lineW[l];

        for (int i = a; i < b; ++i) {
            if (cps[i] == '\n') continue;
            Placed g;
            g.codepoint = cps[i];
            g.style = styles[i];
            g.size = sizes[i];
            g.advance = advances[i];
            g.x = x;
            // Sit every glyph on the line's baseline, so mixed sizes align at
            // the bottom rather than at the top.
            g.y = y + (lineH[l] - (float)sizes[i]);
            g.line = l;
            g.indexInPage = order++;
            m_glyphs.push_back(g);
            x += advances[i];
        }
        y += lineH[l] * ps.lineSpacing;
    }
    m_lineCount = lines;
    m_textHeight = y;
}

// ── Update ────────────────────────────────────────────────────────────────

bool Box::pageComplete() const {
    if (m_page < 0 || m_page >= (int)m_script.pages.size()) return true;
    if (m_script.pages[m_page].style.enter != Enter::Typewriter) return m_pageAge > 0.0f;
    return m_revealed >= (float)m_glyphs.size();
}

bool Box::awaitingCondition() const {
    if (!m_open || m_page >= (int)m_script.pages.size()) return false;
    return !m_script.pages[m_page].until.empty() && !m_conditionMet && pageComplete();
}

bool Box::awaitingChoice() const {
    if (!m_open || m_page >= (int)m_script.pages.size()) return false;
    return !m_script.pages[m_page].choices.empty() && pageComplete();
}

void Box::moveSelection(int delta) {
    if (!awaitingChoice()) return;
    const int n = (int)m_script.pages[m_page].choices.size();
    m_sel = ((m_sel + delta) % n + n) % n;
}

void Box::nextPage() {
    if (m_page + 1 < (int)m_script.pages.size()) {
        m_page++;
        m_revealed = 0.0f;
        m_pageAge = 0.0f;
        m_dirty = true;
        m_sel = 0;
        m_conditionMet = false;      // the next page answers for itself
        m_autoAdvance = 0.0f;
    } else {
        m_open = false;
    }
}

void Box::commitChoice() {
    if (!awaitingChoice()) return;
    const auto& cs = m_script.pages[m_page].choices;
    const int i = std::clamp(m_sel, 0, (int)cs.size() - 1);
    m_pickedKey = cs[i].key;
    m_pickedLabel = cs[i].label;
    nextPage();
}

void Box::update(float dt, bool clicked, Vector2 mouse) {
    if (!m_open) return;
    if (m_dirty) layout();

    m_time += dt;
    m_pageAge += dt;

    const PageStyle& ps = m_script.pages[m_page].style;
    // The beat before the page speaks. Only the REVEAL waits -- the click
    // below still lands, because a pause for effect that cannot be skipped is
    // a lock, and the player who has read the line already is the one most
    // likely to be pressing.
    if (m_pageAge >= ps.delay) {
        if (ps.enter == Enter::Typewriter) m_revealed += ps.speed * dt;
        else m_revealed = (float)m_glyphs.size();
    }

    // DOING THE THING IS THE ANSWER.
    //
    // A page that says "open the economy" and waits for it should turn when
    // the economy opens -- not sit there afterwards wanting a click as well.
    // Asking the player to both obey and acknowledge makes the obeying feel
    // like it did not register.
    //
    // Not instant: a short beat so the result is visible before the words
    // change, and it only starts once the page has finished typing, or a
    // condition that is already true would skip the page unread.
    {
        const Page& pg = m_script.pages[m_page];
        if (!pg.until.empty() && pg.choices.empty()) {
            if (m_conditionMet && pageComplete()) {
                m_autoAdvance += dt;
                if (m_autoAdvance >= 0.5f) { nextPage(); return; }
            } else {
                m_autoAdvance = 0.0f;
            }
        }
    }

    // While options are up, the pointer moves the highlight and a click takes
    // it. Falling through to the page-turn below would skip the question.
    if (awaitingChoice()) {
        for (int i = 0; i < (int)m_choiceRects.size(); ++i)
            if (CheckCollisionPointRec(mouse, m_choiceRects[i])) m_sel = i;
        if (clicked) {
            // Only a click ON an option takes it. Clicking the box elsewhere
            // is how a player dismisses the typewriter, and it must not pick
            // whatever happened to be highlighted.
            for (int i = 0; i < (int)m_choiceRects.size(); ++i)
                if (CheckCollisionPointRec(mouse, m_choiceRects[i])) { m_sel = i; commitChoice(); break; }
        }
        return;
    }

    if (!clicked) return;

    // THE CLASSIC CONTRACT. A click while it is still typing means "I read
    // faster than that", not "next" -- getting this backwards is the single
    // most irritating thing a textbox can do, because it skips a line the
    // player never saw.
    if (!pageComplete()) {
        m_revealed = (float)m_glyphs.size();
        m_pageAge = 10.0f;              // finishes the fade/rise too
        return;
    }
    // Waiting on the world. The click above still finished the typewriter --
    // a player who cannot skip the text is a player who reads it once and
    // then stares at it -- but the page itself does not turn.
    if (awaitingCondition()) return;

    nextPage();
}

// ── Drawing ───────────────────────────────────────────────────────────────

void Box::draw(Color accent) const {
    if (!m_open || m_page >= (int)m_script.pages.size()) return;

    const Page& page = m_script.pages[m_page];
    const PageStyle& ps = page.style;

    // The frame.
    DrawRectangleRounded(m_bounds, 0.06f, 8, Color{12, 12, 18, 235});
    DrawRectangleRoundedLines(m_bounds, 0.06f, 8, ColorAlpha(accent, 0.55f));

    // The nameplate sits ON the top edge rather than inside the box, so the
    // first line of text starts where the box starts and a long name cannot
    // push the prose down.
    if (!page.speaker.empty()) {
        const Label lb = m_labelResolver ? m_labelResolver(page.speaker)
                                         : Label{page.speaker, "", ""};
        const std::string name = lb.name.empty() ? page.speaker : lb.name;
        const int nf = 18;
        const int rf = 14;
        const std::string role = lb.role.empty() ? "" : " - " + lb.role;
        const bool nameAscii = isAsciiOnly(name);
        const bool roleAscii = isAsciiOnly(role);
        const int nw = nameAscii ? MeasureText(name.c_str(), nf)
                                 : (int)MeasureTextEx(m_unicode, name.c_str(),
                                                      (float)nf, 0.0f).x;
        const int rw = role.empty() ? 0
                     : (roleAscii ? MeasureText(role.c_str(), rf)
                                  : (int)MeasureTextEx(m_unicode, role.c_str(),
                                                       (float)rf, 0.0f).x);
        const int tw = 0;   // the short tag is not shown; see below
        const Rectangle plate{m_bounds.x + ps.padding - 10, m_bounds.y - 15.0f,
                              (float)(nw + rw + tw) + 24.0f, 30.0f};
        DrawRectangleRounded(plate, 0.4f, 8, Color{18, 18, 26, 245});
        DrawRectangleRoundedLines(plate, 0.4f, 8, ColorAlpha(accent, 0.8f));
        float x = plate.x + 12;
        if (nameAscii)
            DrawText(name.c_str(), (int)x, (int)(plate.y + 7), nf, accent);
        else
            DrawTextEx(m_unicode, name.c_str(), {x, plate.y + 7.0f},
                       (float)nf, 0.0f, accent);
        x += nw;
        if (!role.empty()) {
            // Dimmer and smaller: a caption to the name, not a second name.
            if (roleAscii)
                DrawText(role.c_str(), (int)x, (int)(plate.y + 10), rf,
                         ColorAlpha(accent, 0.6f));
            else
                DrawTextEx(m_unicode, role.c_str(), {x, plate.y + 10.0f},
                           (float)rf, 0.0f, ColorAlpha(accent, 0.6f));
            x += rw;
        }
        // No short tag. "Mia - lazy advisor MIA" says the name twice, and the
        // second one is an abbreviation of a word the reader has just read.
    }

    const float originX = m_bounds.x + ps.padding;
    const float originY = m_bounds.y + ps.padding;

    // Entrance animations that act on the page as a whole. The typewriter is
    // not here because it is per glyph, below.
    float pageAlpha = 1.0f, pageRise = 0.0f;
    if (ps.enter == Enter::Fade) pageAlpha = std::clamp(m_pageAge / 0.35f, 0.0f, 1.0f);
    else if (ps.enter == Enter::Rise) {
        const float t = std::clamp(m_pageAge / 0.4f, 0.0f, 1.0f);
        pageAlpha = t;
        pageRise = (1.0f - t) * 14.0f;
    }

    for (const Placed& g : m_glyphs) {
        // ── Typewriter ──
        // The last glyph fades in over its own moment rather than snapping,
        // which is the difference between typing and flickering.
        float a = pageAlpha;
        if (ps.enter == Enter::Typewriter) {
            const float d = m_revealed - (float)g.indexInPage;
            if (d <= 0.0f) continue;
            a *= std::min(1.0f, d);
        }

        float px = originX + g.x;
        float py = originY + g.y + pageRise;

        // ── Motion ──
        if (g.style.fx & FX_WAVE)
            py += std::sin(m_time * 5.0f + g.indexInPage * 0.45f) * (g.size * 0.14f);
        if (g.style.fx & FX_SHAKE) {
            px += hashNoise(g.indexInPage, 1, m_time * 26.0f) * (g.size * 0.09f);
            py += hashNoise(g.indexInPage, 2, m_time * 26.0f) * (g.size * 0.09f);
        }

        // ── Colour ──
        Color col{225, 225, 232, 255};
        if (g.style.useAccent)      col = tone(accent, g.style.accentTone);
        else if (g.style.hasColor)  col = {g.style.color.r, g.style.color.g, g.style.color.b, 255};
        if (g.style.fx & FX_RAINBOW)
            col = hueShift(std::fmod(m_time * 0.35f + g.indexInPage * 0.035f, 1.0f));
        // A stage direction is not speech, and reads as an aside rather than
        // as a line: same colour, stepped back.
        if (g.style.fx & FX_ACTION)
            col = Color{(unsigned char)(col.r * 0.72f), (unsigned char)(col.g * 0.72f),
                        (unsigned char)(col.b * 0.74f), col.a};
        col = ColorAlpha(col, a);

        const int cp = (g.style.fx & FX_OBFUSCATE)
                     ? obfuscatedGlyph(g.indexInPage, m_time)
                     : g.codepoint;
        const Font f = fontFor(cp);

        // ── Weight and slant ──
        //
        // The game's font is a pixel face with no bold and no italic cut, so
        // both are faked -- which for a pixel font is the honest answer rather
        // than a compromise. Bold is the glyph struck twice a pixel apart.
        // Italic is the glyph sliced horizontally and each slice nudged, which
        // is a real shear and costs a few draws on a handful of letters.
        if (g.style.fx & (FX_ITALIC | FX_ACTION)) {
            const int gi = GetGlyphIndex(f, cp);
            if (gi >= 0 && gi < f.glyphCount) {
                const Rectangle src = f.recs[gi];
                const float scale = (float)g.size / (float)f.baseSize;
                const int slices = std::max(2, (int)src.height / 2);
                const float sh = src.height / (float)slices;
                const float lean = g.size * 0.22f;
                for (int s = 0; s < slices; ++s) {
                    const Rectangle ss{src.x, src.y + s * sh, src.width, sh};
                    const float up = 1.0f - (s + 0.5f) / (float)slices;   // top leans most
                    const Rectangle dd{
                        px + f.glyphs[gi].offsetX * scale + lean * up,
                        py + f.glyphs[gi].offsetY * scale + s * sh * scale,
                        src.width * scale, sh * scale};
                    DrawTexturePro(f.texture, ss, dd, {0, 0}, 0.0f, col);
                    if (g.style.fx & FX_BOLD)
                        DrawTexturePro(f.texture, ss, {dd.x + 1, dd.y, dd.width, dd.height},
                                       {0, 0}, 0.0f, col);
                }
            }
        } else {
            DrawTextCodepoint(f, cp, {px, py}, (float)g.size, col);
            if (g.style.fx & FX_BOLD)
                DrawTextCodepoint(f, cp, {px + 1.0f, py}, (float)g.size, col);
        }

    }

    // ── Underline, drawn as a RULE and not as a stack of little rectangles ──
    //
    // It was one rectangle per glyph, skipping spaces, which gave "how many
    // turns you have" an underline that stopped dead at every gap between
    // words -- four rules where there should be one. Underlining is a mark
    // made under a PHRASE; the spaces inside it are part of the phrase.
    //
    // Per-glyph rectangles were also a seam risk in their own right: each was
    // ceil()ed to whole pixels independently, so a run could show hairline
    // gaps between letters purely from rounding.
    //
    // A separate pass because it has to reach across glyphs. A run ends when
    // the underline stops, the line changes, the colour changes, or the
    // typewriter has not got there yet -- an underline must not run ahead of
    // the letters it belongs to.
    //
    // Strike-through is the same mark at a different height, so it runs the
    // same pass rather than a copy of it: a rule under a phrase and a rule
    // through one have identical rules about where they may stop.
    const struct { uint32_t bit; float yFrac; } kRules[] = {
        { FX_UNDERLINE, 0.98f },
        { FX_STRIKE,    0.55f },
    };
    for (const auto& rule : kRules) {
        int i = 0;
        const int n = (int)m_glyphs.size();
        while (i < n) {
            const Placed& g0 = m_glyphs[i];
            if (!(g0.style.fx & rule.bit)) { ++i; continue; }
            if (ps.enter == Enter::Typewriter && m_revealed - (float)g0.indexInPage <= 0.0f) {
                ++i; continue;
            }
            int j = i;
            float endX = 0.0f;
            float alpha = pageAlpha;
            while (j < n) {
                const Placed& g = m_glyphs[j];
                if (!(g.style.fx & rule.bit) || g.line != g0.line) break;
                if (ps.enter == Enter::Typewriter) {
                    const float d = m_revealed - (float)g.indexInPage;
                    if (d <= 0.0f) break;
                    alpha = std::min(alpha, pageAlpha * std::min(1.0f, d));
                }
                endX = g.x + g.advance;
                ++j;
            }
            if (j > i) {
                Color col{225, 225, 232, 255};
                if (g0.style.useAccent)     col = tone(accent, g0.style.accentTone);
                else if (g0.style.hasColor) col = {g0.style.color.r, g0.style.color.g,
                                                   g0.style.color.b, 255};
                if (g0.style.fx & FX_RAINBOW)
                    col = hueShift(std::fmod(m_time * 0.35f + g0.indexInPage * 0.035f, 1.0f));
                // A trailing space at the end of the run is not part of the
                // phrase, so the rule stops at the last visible letter.
                int last = j - 1;
                while (last > i && m_glyphs[last].codepoint == ' ') {
                    endX = m_glyphs[last].x;
                    last--;
                }
                DrawRectangle((int)std::floor(originX + g0.x),
                              (int)(originY + g0.y + pageRise + g0.size * rule.yFrac),
                              (int)std::ceil(endX - g0.x),
                              std::max(1, g0.size / 14), ColorAlpha(col, alpha));
            }
            i = (j > i) ? j : i + 1;
        }
    }

    // ── Options ──────────────────────────────────────────────────────────
    //
    // Laid out from the BOTTOM of the box upward, so adding an option pushes
    // the list up rather than pushing it off the bottom edge, and so the
    // prose above keeps the position it was typed at.
    //
    // The rectangles are stored here, in draw, because this is the only place
    // that knows the final geometry -- and update() needs exactly these to
    // decide what the pointer is over. draw() is const, so they are mutable:
    // it is a cache of what was drawn, not state of its own.
    if (!page.choices.empty() && pageComplete()) {
        const int cs = std::max(12, ps.size - 2);
        const float rowH = cs * 1.9f;
        const float x0 = m_bounds.x + ps.padding;
        const float w  = m_bounds.width - ps.padding * 2.0f;

        // BELOW the prose, not simply at the bottom of the box.
        //
        // Anchoring the list to the bottom edge works until the question is
        // two lines long and there are five answers, and then the options are
        // drawn straight through the text -- which is what five topics did.
        // The list starts under the last line and, if that would push it off
        // the bottom, it is the BOX that has to have been made taller: see
        // dialogueBounds, which grows for a page carrying choices.
        float textBottom = m_bounds.y + ps.padding;
        for (const Placed& g : m_glyphs)
            textBottom = std::max(textBottom, originY + g.y + g.size);
        const float listH = rowH * page.choices.size();
        float y = std::max(textBottom + cs * 0.9f,
                           m_bounds.y + m_bounds.height - ps.padding - listH);

        m_choiceRects.assign(page.choices.size(), Rectangle{});
        for (size_t i = 0; i < page.choices.size(); ++i) {
            const Rectangle r{x0, y, w, rowH - 4.0f};
            m_choiceRects[i] = r;
            const bool on = ((int)i == m_sel);

            if (on) {
                // A bar down the left rather than a filled row: the box is
                // over live map, and a slab of colour there reads as a panel
                // that has appeared rather than as a highlight.
                const float pulse = 0.75f + 0.25f * std::sin(m_time * 4.0f);
                DrawRectangleRec({r.x, r.y + 3.0f, 3.0f, r.height - 6.0f},
                                 ColorAlpha(accent, pulse));
                DrawRectangleRec({r.x, r.y + 3.0f, r.width, r.height - 6.0f},
                                 ColorAlpha(accent, 0.10f));
            }
            const Color col = on ? Color{240, 242, 248, 255} : Color{170, 176, 190, 235};
            const Font& cf = isAsciiOnly(page.choices[i].label) ? m_ascii : m_unicode;
            DrawTextEx(cf, page.choices[i].label.c_str(),
                       {r.x + 14.0f, r.y + (r.height - cs) * 0.5f},
                       (float)cs, ps.letterSpacing, ColorAlpha(col, pageAlpha));
            y += rowH;
        }
        return;      // the ▼ prompt would be a lie: this page does not advance
    }
    m_choiceRects.clear();

    // Waiting on the player to DO something rather than to click. A different
    // mark, because the ▼ means "there is more when you are ready" and here
    // there is not -- pressing on will not help, and a prompt that says it
    // will is a prompt that gets pressed twenty times.
    if (awaitingCondition()) {
        drawPrompt(Prompt::Waiting, 15, 0.35f + 0.35f * std::sin(m_time * 2.2f), accent);
        return;
    }

    // The prompt: only once the page is finished, and pulsing, because a
    // static one reads as decoration rather than as "your turn".
    if (pageComplete()) {
        const Prompt kind = (m_page + 1 < (int)m_script.pages.size()) ? Prompt::More
                                                                      : Prompt::End;
        drawPrompt(kind, 16, 0.55f + 0.45f * std::sin(m_time * 3.4f), accent);
    }
}

// The prompt marks are DRAWN, not typed.
//
// They started as the glyphs ▼ ● ○ through DrawText, which uses raylib's
// default font -- an ASCII face that has none of them, so every one of them
// rendered as a "?" in the corner of the box. Moving to the game's Unifont
// fallback did not help either: it is loaded with 617 glyphs, the ones the
// game's languages need, and geometric shapes are not among them.
//
// Three primitives always work, are not a font question at all, and look
// better at this size than a glyph would.
void Box::drawPrompt(Prompt kind, int size, float alpha, Color accent) const {
    const PageStyle& ps = m_script.pages[m_page].style;
    const float r = size * 0.42f;
    const Vector2 c{m_bounds.x + m_bounds.width - ps.padding - r,
                    m_bounds.y + m_bounds.height - ps.padding - r};
    const Color col = ColorAlpha(accent, alpha);
    switch (kind) {
        case Prompt::More:                 // a triangle: there is more, click on
            DrawTriangle({c.x - r, c.y - r * 0.7f}, {c.x, c.y + r * 0.8f},
                         {c.x + r, c.y - r * 0.7f}, col);
            break;
        case Prompt::End:                  // a full stop
            DrawCircleV(c, r * 0.8f, col);
            break;
        case Prompt::Waiting:              // an open ring: your move, not mine
            DrawRing(c, r * 0.62f, r * 0.9f, 0.0f, 360.0f, 24, col);
            break;
    }
}


}  // namespace dlg
