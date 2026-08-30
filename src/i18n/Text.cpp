// The shadowed DrawText / MeasureText. See Text.h for why they exist.
//
// This file must NOT see its own macros, or the calls to raylib below would
// call back into these functions and recurse until the stack runs out. The
// header is included after raylib and the macros are undefined immediately.

#include "raylib.h"
#include "i18n/Text.h"
#include "i18n/Arabic.h"
#include "i18n/Devanagari.h"
#include "i18n/ArabicShape.h"
#include "i18n/Locale.h"
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#undef DrawText
#undef MeasureText

#include <string>
#include <vector>

namespace odText {
namespace {

Font g_font{};

// One advance, used by both drawing and measuring. The two disagreeing is how
// a label ends up half a word wider than the box drawn around it.
float advanceOf(const Font& f, int codepoint, int fontSize) {
    const int gi = GetGlyphIndex(f, codepoint);
    float advance = 0.0f;
    if (gi >= 0 && gi < f.glyphCount) {
        advance = (float)f.glyphs[gi].advanceX;
        if (advance <= 0.0f) advance = (float)f.recs[gi].width;
        // The atlas is built at one size; everything else is a scale of it.
        advance *= (float)fontSize / (float)f.baseSize;
    }
    if (advance <= 0.0f) advance = (float)fontSize * 0.5f;
    return advance;
}

/// Decode one UTF-8 sequence. `size` comes back as the bytes consumed.
int nextCodepoint(const char* p, int& size) {
    const unsigned char c = (unsigned char)p[0];
    if (c < 0x80) { size = 1; return c; }
    if ((c & 0xE0) == 0xC0 && p[1]) {
        size = 2;
        return ((c & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && p[1] && p[2]) {
        size = 3;
        return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        size = 4;
        return ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    // A stray byte. Consume it rather than looping forever on it.
    size = 1;
    return '?';
}

/// The codepoints of `text`, in the order they should be DRAWN. For most
/// languages that is simply the order they were typed in; Arabic is shaped and
/// reversed here so that everything downstream can stay a simple left-to-right
/// walk over glyphs.
std::vector<unsigned> displayOrder(const char* text) {
    std::vector<unsigned> cps;
    for (const char* p = text; *p;) {
        int size = 1;
        cps.push_back((unsigned)nextCodepoint(p, size));
        p += size;
    }
    if (!hasArabic(cps)) return cps;
    return reorderForDisplay(shapeArabic(cps));
}

}  // namespace

void setFont(Font unicode) { g_font = unicode; }
bool ready() { return g_font.texture.id > 0 && g_font.glyphCount > 0; }

// Urdu is drawn by the HarfBuzz path (odArab), not the table shaper. The flag
// is set only while Urdu is the active language, so the Arabic language keeps
// its lighter table path untouched. See ArabicShape.h for why Urdu needs this.
bool g_complexArabic = false;
void setComplexArabic(bool on) { g_complexArabic = on; }

// One line of complex Arabic-script text, laid out right to left. Runs of
// Arabic shape through odArab (which returns them already in visual order);
// runs of anything else -- digits, Latin, a "%s" that survived into the
// string -- keep their own left-to-right order. The runs themselves are placed
// back to front, which is the one level of bidi this interface actually needs.
float layoutComplexLine(const std::vector<unsigned>& logical, float x, float y,
                        int fontSize, Color color, bool draw) {
    struct Run { size_t b, e; bool arab; };
    std::vector<Run> runs;
    size_t i = 0;
    while (i < logical.size()) {
        const bool a = odArab::isArabic(logical[i]);
        size_t j = i;
        while (j < logical.size() && odArab::isArabic(logical[j]) == a) ++j;
        runs.push_back({i, j, a});
        i = j;
    }
    float pen = x;
    for (size_t r = runs.size(); r-- > 0;) {
        std::vector<unsigned> cps(logical.begin() + runs[r].b, logical.begin() + runs[r].e);
        if (runs[r].arab) {
            pen += draw ? odArab::draw(cps, pen, y, fontSize, color)
                        : odArab::measure(cps, fontSize);
        } else {
            for (unsigned cp : cps) {
                if (draw) DrawTextCodepoint(g_font, (int)cp, {pen, y}, (float)fontSize, color);
                pen += advanceOf(g_font, (int)cp, fontSize);
            }
        }
    }
    return pen - x;
}

// Split a raw string into lines on newlines, decoding each to logical
// codepoints WITHOUT the table shaper -- odArab does its own shaping.
std::vector<std::vector<unsigned>> logicalLines(const char* text) {
    std::vector<std::vector<unsigned>> lines(1);
    for (const char* p = text; *p;) {
        int size = 1;
        const unsigned cp = (unsigned)nextCodepoint(p, size);
        p += size;
        if (cp == '\n') lines.emplace_back();
        else lines.back().push_back(cp);
    }
    return lines;
}

std::string fitToWidth(const std::string& text, int width, int& fontSize, int floorSize) {
    if (width <= 0 || text.empty()) return text;
    if (measureText(text.c_str(), fontSize) <= width) return text;

    // Smaller type first: a button that reads "Cancel Request Mutual
    // Guarantee" one point down is better than one that reads "Cancel Requ…".
    while (fontSize > floorSize && measureText(text.c_str(), fontSize) > width)
        --fontSize;
    if (measureText(text.c_str(), fontSize) <= width) return text;

    // Still too wide, so something has to go. Cut on a character boundary --
    // firstChars counts codepoints, not bytes, or a Japanese label loses a
    // third of a glyph and draws a replacement box.
    //
    // THE ELLIPSIS HAS TO MATCH THE FONT THE REST OF THE LABEL IS IN.
    //
    // needsAtlas() switches to the per-language glyph atlas on the first byte
    // above 127, and it looks at the WHOLE string. So appending U+2026 to an
    // English label moved that label off raylib's own font and onto unifont --
    // the typeface changed, mid-button, because the text was shortened. Three
    // ASCII dots keep an ASCII string where it was; anything already on the
    // atlas gets the real ellipsis.
    const char* dots = needsAtlas(text.c_str()) ? "\xe2\x80\xa6" : "...";
    const int total = charCount(text);
    for (int n = total; n > 0; --n) {
        std::string cut = firstChars(text, n) + dots;
        if (measureText(cut.c_str(), fontSize) <= width) return cut;
    }
    return firstChars(text, 1);
}

std::string firstChars(const std::string& text, int chars) {
    const char* p = text.c_str();
    const char* const end = p + text.size();
    int n = 0;
    while (p < end && n < chars) {
        int size = 1;
        nextCodepoint(p, size);
        if (size <= 0) size = 1;
        if (p + size > end) break;
        p += size;
        ++n;
    }
    return text.substr(0, (size_t)(p - text.c_str()));
}

int charCount(const std::string& text) {
    const char* p = text.c_str();
    const char* const end = p + text.size();
    int n = 0;
    while (p < end) {
        int size = 1;
        nextCodepoint(p, size);
        if (size <= 0) size = 1;
        if (p + size > end) break;
        p += size;
        ++n;
    }
    return n;
}

bool needsAtlas(const char* text) {
    if (!text) return false;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p)
        if (*p > 127) return true;
    return false;
}

// EVERY DRAWN STRING IS LOOKED UP, whatever route it took to get here.
//
// Wrapping call sites in T() only reaches the ones written as literals in the
// drawing call. A label that arrived through a local table -- the policy tabs,
// the settings rows, a `const char* s` assigned three lines earlier -- has no
// literal on the DrawText line, so it was never collected and never
// translated, and the coverage report happily said 100% because it only knew
// about the strings it could see.
//
// The lookup is the same one T() does: a hit returns the translation, a miss
// returns the pointer unchanged. So a formatted string ("Turn 5"), a country
// name, or a number simply passes through -- and English stays the identity
// function, because in English every value equals its key.
// ─── OD_I18N_AUDIT: WHAT IS STILL BEING DRAWN IN ENGLISH ──────────────────
//
// Every string the game draws passes through here, which makes this the one
// place that can answer "what is left?" by observation rather than by grepping
// for literals -- a grep finds four thousand error messages and protocol
// constants and misses the text that arrives as an ARGUMENT, which is where
// the last round of bugs actually lived.
//
// Set OD_I18N_AUDIT=<path> and play, or run --screenshots and --tutorial-walk.
// Anything drawn that is not a key and is not just numbers is written there,
// once each. Off by default and behind one static branch when it is.
namespace {

std::FILE* auditFile() {
    static std::FILE* f = [] () -> std::FILE* {
        const char* path = std::getenv("OD_I18N_AUDIT");
        return (path && *path) ? std::fopen(path, "w") : nullptr;
    }();
    return f;
}

/// Numbers, punctuation and spacing are values, not text somebody translates.
bool worthReporting(const char* s) {
    bool letter = false;
    for (const char* p = s; *p; ++p)
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) { letter = true; break; }
    return letter;
}

// Single-threaded by construction: everything here runs from the draw, and
// raylib draws on one thread.
void audit(const char* before, const char* after) {
    std::FILE* f = auditFile();
    if (!f) return;                              // the whole cost when it is off
    if (before != after) return;                 // translated: the table hit
    if (od::i18n::language() == "en") return;    // English is the identity
    if (!worthReporting(before)) return;
    // ALREADY TRANSLATED, ARRIVING A SECOND TIME. A helper that translates a
    // label, measures it and then draws it hands the translation back to this
    // lookup, where it misses because it is a value and not a key. That is not
    // an untranslated string; it is a translated one going round twice.
    if (od::i18n::isTranslation(before)) return;
    static std::unordered_set<std::string> seen;
    if (!seen.insert(before).second) return;
    std::fprintf(f, "%s\n", before);
    std::fflush(f);
}

}  // namespace

const char* localised(const char* text) {
    const char* out = od::i18n::tr(text);
    audit(text, out);
    return out;
}

// ─── OD_I18N_FIT: WHAT NO LONGER FITS ─────────────────────────────────────
//
// The sibling of the audit above. That one answers "what is still English";
// this one answers the question that only appears AFTER everything has been
// translated -- whether the translation still fits the box English was
// measured for. Both are observation rather than grep, for the same reason:
// the width depends on the font, the size and the language, and none of that
// is visible in the source.
namespace {

std::FILE* fitFile() {
    static std::FILE* f = [] () -> std::FILE* {
        const char* path = std::getenv("OD_I18N_FIT");
        return (path && *path) ? std::fopen(path, "w") : nullptr;
    }();
    return f;
}

// ─── THE MANIFEST: EVERY BOX, NOT JUST THE ONES THAT OVERFLOWED ───────────
//
// The report above only sees the language the game is running in, and only
// the strings that screen actually drew. That is a floor, not a proof: the
// screenshot tour renders two of the thirteen diplomacy buttons, because it
// never opens one with a request pending, and running it twenty times over
// still tells you nothing about the eleven it did not reach.
//
// So the manifest records the BOX rather than the failure -- where it is, how
// wide, at what size, and what was in it. Run it once in English and every
// string recorded is a key; tools/i18n_fit.py then measures all twenty
// translations of that key against that width, offline, without the game.
// One run covers every language, and a widget added later is covered the
// moment somebody calls fitAudit from it.
//
// `measured` is what the game itself made of the string, so the tool can
// check its own width model against the real thing rather than trusting it.
std::FILE* manifestFile() {
    static std::FILE* f = [] () -> std::FILE* {
        const char* path = std::getenv("OD_I18N_FIT_MANIFEST");
        return (path && *path) ? std::fopen(path, "w") : nullptr;
    }();
    return f;
}

}  // namespace

void fitAudit(const char* text, int width, int fontSize, const char* where) {
    std::FILE* m = manifestFile();
    std::FILE* f = fitFile();
    if (!m && !f) return;                        // the whole cost when it is off
    if (!text || width <= 0) return;
    const int w = measureText(text, fontSize);

    if (m) {
        static std::unordered_set<std::string> mseen;
        char key[640];
        std::snprintf(key, sizeof key, "%s|%d|%d|%s", where, width, fontSize, text);
        if (mseen.insert(key).second) {
            std::fprintf(m, "%s\t%d\t%d\t%d\t%s\n", where, width, fontSize, w, text);
            std::fflush(m);
        }
    }
    if (!f) return;
    if (w <= width) return;
    // Once each. A button redrawn every frame would otherwise fill the file
    // with one line sixty times a second.
    static std::unordered_set<std::string> seen;
    char key[512];
    std::snprintf(key, sizeof key, "%s\t%s", where, text);
    if (!seen.insert(key).second) return;
    std::fprintf(f, "%s\t%s\t%d\t%d\t%s\n",
                 od::i18n::language().c_str(), where, w, width, localised(text));
    std::fflush(f);
}

void drawText(const char* text, int x, int y, int fontSize, Color color) {
    if (!text) return;
    text = localised(text);
    if (!needsAtlas(text) || !ready()) {
        ::DrawText(text, x, y, fontSize, color);
        return;
    }
    // URDU IS DRAWN BY THE HARFBUZZ ARABIC PATH when it is the active language.
    // It cannot go through the table shaper below, whose presentation-form
    // substitutions do not cover Urdu's letters, nor through displayOrder,
    // which applies exactly that shaper.
    if (g_complexArabic && odArab::available()) {
        int ly = y;
        for (const auto& line : logicalLines(text)) {
            layoutComplexLine(line, (float)x, (float)ly, fontSize, color, true);
            ly += fontSize + fontSize / 4;
        }
        return;
    }

    // DEVANAGARI IS DRAWN BY A DIFFERENT PIPELINE. Its glyphs are chosen by a
    // shaper and have no codepoints, so they cannot go through the loop below.
    // The string is split into runs and each is handed to whichever path can
    // actually draw it -- a label reading "हिन्दी (Hindi)" needs both.
    const std::vector<unsigned> cps = displayOrder(text);
    if (odDeva::available()) {
        bool anyDeva = false;
        for (unsigned cp : cps) anyDeva = anyDeva || odDeva::isDevanagari(cp);
        if (anyDeva) {
            float pen = (float)x;
            size_t i = 0;
            while (i < cps.size()) {
                const bool deva = odDeva::isDevanagari(cps[i]);
                size_t j = i;
                while (j < cps.size() && odDeva::isDevanagari(cps[j]) == deva) ++j;
                std::vector<unsigned> run(cps.begin() + i, cps.begin() + j);
                if (deva) {
                    pen += odDeva::draw(run, pen, (float)y, fontSize, color);
                } else {
                    for (unsigned cp : run) {
                        DrawTextCodepoint(g_font, (int)cp, {pen, (float)y},
                                          (float)fontSize, color);
                        pen += advanceOf(g_font, (int)cp, fontSize);
                    }
                }
                i = j;
            }
            return;
        }
    }

    float penX = (float)x;
    for (unsigned cp : cps) {
        if (cp == '\n') {
            // The callers that pass newlines are drawing a block of text and
            // expect raylib's own line breaking; this keeps the same shape.
            penX = (float)x;
            y += fontSize + fontSize / 4;
            continue;
        }
        DrawTextCodepoint(g_font, (int)cp, {penX, (float)y}, (float)fontSize, color);
        penX += advanceOf(g_font, (int)cp, fontSize);
    }
}

int measureText(const char* text, int fontSize) {
    if (!text) return 0;
    // The SAME lookup as drawText, or a translated label is measured at its
    // English width and every box drawn round it is the wrong size.
    text = localised(text);
    if (!needsAtlas(text) || !ready()) return ::MeasureText(text, fontSize);
    if (g_complexArabic && odArab::available()) {
        float widest = 0.0f;
        for (const auto& line : logicalLines(text)) {
            const float w = layoutComplexLine(line, 0.0f, 0.0f, fontSize, WHITE, false);
            if (w > widest) widest = w;
        }
        return (int)widest;
    }

    const std::vector<unsigned> cps = displayOrder(text);
    if (odDeva::available()) {
        bool anyDeva = false;
        for (unsigned cp : cps) anyDeva = anyDeva || odDeva::isDevanagari(cp);
        if (anyDeva) {
            // Same split as drawText, or the box drawn round a Hindi label is
            // measured with advances the shaper never used.
            float w = 0.0f;
            size_t i = 0;
            while (i < cps.size()) {
                const bool deva = odDeva::isDevanagari(cps[i]);
                size_t j = i;
                while (j < cps.size() && odDeva::isDevanagari(cps[j]) == deva) ++j;
                std::vector<unsigned> run(cps.begin() + i, cps.begin() + j);
                if (deva) w += odDeva::measure(run, fontSize);
                else for (unsigned cp : run) w += advanceOf(g_font, (int)cp, fontSize);
                i = j;
            }
            return (int)w;
        }
    }

    float widest = 0.0f, line = 0.0f;
    // Measured from the SHAPED text, because a shaped Arabic letter is a
    // different glyph with a different advance from the one that was typed.
    for (unsigned cp : cps) {
        if (cp == '\n') {
            if (line > widest) widest = line;
            line = 0.0f;
        } else {
            line += advanceOf(g_font, (int)cp, fontSize);
        }
    }
    return (int)(line > widest ? line : widest);
}

namespace {

/// Split into lines that fit, the way drawWrapped and measureWrapped both need.
std::vector<std::string> wrapLines(const char* text, int maxWidth, int fontSize) {
    std::vector<std::string> lines;
    if (!text || maxWidth <= 0) return lines;

    std::string line;

    // Put one word on the current line, starting a new one if it will not fit.
    auto place = [&](const std::string& word) {
        if (word.empty()) return;
        const std::string test = line.empty() ? word : line + " " + word;
        if (measureText(test.c_str(), fontSize) <= maxWidth) { line = test; return; }
        if (!line.empty()) { lines.push_back(line); line.clear(); }

        // A WORD IS ONLY BROKEN WHEN IT CANNOT FIT A LINE OF ITS OWN.
        //
        // The first version broke whenever the line was full, which split
        // "англійський" across two lines in the middle of the word. A word
        // that does not fit anywhere still has to go somewhere -- that is a
        // long identifier, or a run of Japanese, which has no spaces to break
        // on at all -- and only then is it cut per character.
        if (measureText(word.c_str(), fontSize) <= maxWidth) { line = word; return; }
        for (const char* p = word.c_str(); *p;) {
            int size = 1;
            nextCodepoint(p, size);
            const std::string ch(p, (size_t)size);
            p += size;
            const std::string grown = line + ch;
            if (!line.empty() && measureText(grown.c_str(), fontSize) > maxWidth) {
                lines.push_back(line);
                line = ch;
            } else {
                line = grown;
            }
        }
    };

    std::string word;
    for (const char* p = text; *p;) {
        int size = 1;
        const int cp = nextCodepoint(p, size);
        const std::string ch(p, (size_t)size);
        p += size;
        if (cp == '\n') { place(word); word.clear(); lines.push_back(line); line.clear(); continue; }
        if (cp == ' ')   { place(word); word.clear(); continue; }
        word += ch;
    }
    place(word);
    if (!line.empty()) lines.push_back(line);
    return lines;
}

}  // namespace

int drawWrapped(const char* text, int x, int y, int maxWidth, int fontSize, Color color) {
    const std::vector<std::string> lines = wrapLines(text, maxWidth, fontSize);
    const int step = fontSize + fontSize / 4;
    for (size_t i = 0; i < lines.size(); ++i)
        drawText(lines[i].c_str(), x, y + (int)i * step, fontSize, color);
    return (int)lines.size() * step;
}

int measureWrapped(const char* text, int maxWidth, int fontSize) {
    return (int)wrapLines(text, maxWidth, fontSize).size() * (fontSize + fontSize / 4);
}

}  // namespace odText
