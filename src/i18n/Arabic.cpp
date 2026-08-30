// ─────────────────────────────────────────────────────────────────────────────
// Arabic: joining forms, and the direction it is read in.
//
// WHY THIS FILE EXISTS. Every other language here can be drawn by walking the
// string and putting one glyph after another. Arabic cannot, for two reasons,
// and getting either wrong produces text that an Arabic reader will call
// broken rather than badly translated:
//
//   1. LETTERS CHANGE SHAPE BY POSITION. Every Arabic letter has up to four
//      forms -- isolated, initial, medial, final -- and which one is correct
//      depends on the letters on both sides of it. Drawing the codepoints as
//      typed gives the isolated form every time, which reads like English
//      written i n  s e p a r a t e  l e t t e r s, except worse, because the
//      connecting strokes are what the eye reads a word by.
//
//   2. IT IS READ RIGHT TO LEFT. Numbers and Latin inside it are still read
//      left to right, so a line is not simply reversed.
//
// The font already carries the answer to (1): Unifont ships all 144 glyphs of
// Arabic Presentation Forms-B, which ARE the shaped forms. So the work is to
// choose the right one and substitute it -- no ligature engine required.
//
// WHAT THIS IS NOT. It is not a bidirectional algorithm. UAX #9 handles nested
// direction changes, neutral runs resolving by context, and paragraph levels;
// this handles one level -- Arabic runs reversed, non-Arabic runs left alone --
// which is right for labels and sentences and would be wrong for a paragraph
// mixing quotations of both. That limit is deliberate and worth knowing.
// ─────────────────────────────────────────────────────────────────────────────

#include "i18n/Arabic.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace odText {
namespace {

// How a letter connects. A DUAL letter joins on both sides; a RIGHT letter
// joins only to the letter before it and refuses to connect forward, which is
// why "دد" stays two separate shapes. Everything else joins nothing.
enum class Join { None, Right, Dual };

struct Forms {
    Join join;
    unsigned isolated, final, initial, medial;   // 0 where the form does not exist
};

/// base codepoint -> its four presentation forms.
const std::unordered_map<unsigned, Forms>& table() {
    static const std::unordered_map<unsigned, Forms> t = {
        {0x0621, {Join::None,  0xFE80, 0,      0,      0     }},  // hamza
        {0x0622, {Join::Right, 0xFE81, 0xFE82, 0,      0     }},  // alef madda
        {0x0623, {Join::Right, 0xFE83, 0xFE84, 0,      0     }},  // alef hamza above
        {0x0624, {Join::Right, 0xFE85, 0xFE86, 0,      0     }},  // waw hamza
        {0x0625, {Join::Right, 0xFE87, 0xFE88, 0,      0     }},  // alef hamza below
        {0x0626, {Join::Dual,  0xFE89, 0xFE8A, 0xFE8B, 0xFE8C}},  // yeh hamza
        {0x0627, {Join::Right, 0xFE8D, 0xFE8E, 0,      0     }},  // alef
        {0x0628, {Join::Dual,  0xFE8F, 0xFE90, 0xFE91, 0xFE92}},  // beh
        {0x0629, {Join::Right, 0xFE93, 0xFE94, 0,      0     }},  // teh marbuta
        {0x062A, {Join::Dual,  0xFE95, 0xFE96, 0xFE97, 0xFE98}},  // teh
        {0x062B, {Join::Dual,  0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}},  // theh
        {0x062C, {Join::Dual,  0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0}},  // jeem
        {0x062D, {Join::Dual,  0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}},  // hah
        {0x062E, {Join::Dual,  0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}},  // khah
        {0x062F, {Join::Right, 0xFEA9, 0xFEAA, 0,      0     }},  // dal
        {0x0630, {Join::Right, 0xFEAB, 0xFEAC, 0,      0     }},  // thal
        {0x0631, {Join::Right, 0xFEAD, 0xFEAE, 0,      0     }},  // reh
        {0x0632, {Join::Right, 0xFEAF, 0xFEB0, 0,      0     }},  // zain
        {0x0633, {Join::Dual,  0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}},  // seen
        {0x0634, {Join::Dual,  0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}},  // sheen
        {0x0635, {Join::Dual,  0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC}},  // sad
        {0x0636, {Join::Dual,  0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}},  // dad
        {0x0637, {Join::Dual,  0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}},  // tah
        {0x0638, {Join::Dual,  0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8}},  // zah
        {0x0639, {Join::Dual,  0xFEC9, 0xFECA, 0xFECB, 0xFECC}},  // ain
        {0x063A, {Join::Dual,  0xFECD, 0xFECE, 0xFECF, 0xFED0}},  // ghain
        {0x0641, {Join::Dual,  0xFED1, 0xFED2, 0xFED3, 0xFED4}},  // feh
        {0x0642, {Join::Dual,  0xFED5, 0xFED6, 0xFED7, 0xFED8}},  // qaf
        {0x0643, {Join::Dual,  0xFED9, 0xFEDA, 0xFEDB, 0xFEDC}},  // kaf
        {0x0644, {Join::Dual,  0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0}},  // lam
        {0x0645, {Join::Dual,  0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}},  // meem
        {0x0646, {Join::Dual,  0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8}},  // noon
        {0x0647, {Join::Dual,  0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC}},  // heh
        {0x0648, {Join::Right, 0xFEED, 0xFEEE, 0,      0     }},  // waw
        {0x0649, {Join::Right, 0xFEEF, 0xFEF0, 0,      0     }},  // alef maksura
        {0x064A, {Join::Dual,  0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4}},  // yeh
    };
    return t;
}

// LAM followed by an ALEF is not two letters. It is one, and writing it as two
// is the single most obvious way to look like a machine wrote the page.
unsigned lamAlef(unsigned alef, bool joinsPrev) {
    switch (alef) {
        case 0x0622: return joinsPrev ? 0xFEF6 : 0xFEF5;
        case 0x0623: return joinsPrev ? 0xFEF8 : 0xFEF7;
        case 0x0625: return joinsPrev ? 0xFEFA : 0xFEF9;
        case 0x0627: return joinsPrev ? 0xFEFC : 0xFEFB;
        default:     return 0;
    }
}

Join joinOf(unsigned cp) {
    auto it = table().find(cp);
    return (it == table().end()) ? Join::None : it->second.join;
}

/// Marks sit on the letter before them and must not break a join.
bool isTransparent(unsigned cp) {
    return (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670 ||
           (cp >= 0x06D6 && cp <= 0x06ED);
}

}  // namespace

bool isArabic(unsigned cp) {
    return (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0xFE70 && cp <= 0xFEFF);
}

bool hasArabic(const std::vector<unsigned>& text) {
    for (unsigned cp : text)
        if (isArabic(cp)) return true;
    return false;
}

std::vector<unsigned> shapeArabic(const std::vector<unsigned>& in) {
    std::vector<unsigned> out;
    out.reserve(in.size());

    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned cp = in[i];
        auto it = table().find(cp);
        if (it == table().end()) { out.push_back(cp); continue; }

        // The letters either side, skipping the marks that sit on top of them.
        auto prevLetter = [&]() -> unsigned {
            for (size_t k = i; k-- > 0;)
                if (!isTransparent(in[k])) return in[k];
            return 0;
        };
        auto nextLetter = [&](size_t from) -> unsigned {
            for (size_t k = from; k < in.size(); ++k)
                if (!isTransparent(in[k])) return in[k];
            return 0;
        };

        const unsigned prev = prevLetter();
        const unsigned next = nextLetter(i + 1);

        // Joined to what came before only if THAT letter connects forward.
        const bool joinsPrev = (joinOf(prev) == Join::Dual);
        // Joined to what comes next only if this letter connects forward and
        // there is a letter there able to take the connection.
        const bool joinsNext = (it->second.join == Join::Dual) && (joinOf(next) != Join::None);

        if (cp == 0x0644) {
            if (const unsigned lig = lamAlef(next, joinsPrev)) {
                out.push_back(lig);
                // The alef has been consumed by the ligature; step over it and
                // any marks between them.
                size_t k = i + 1;
                while (k < in.size() && isTransparent(in[k])) ++k;
                i = k;
                continue;
            }
        }

        const Forms& f = it->second;
        unsigned form = f.isolated;
        if (joinsPrev && joinsNext && f.medial)      form = f.medial;
        else if (joinsPrev && f.final)               form = f.final;
        else if (joinsNext && f.initial)             form = f.initial;
        out.push_back(form ? form : cp);
    }
    return out;
}

std::vector<unsigned> reorderForDisplay(const std::vector<unsigned>& in) {
    // One level, run by run: an Arabic run is reversed, everything else keeps
    // its order, and the runs themselves come out back to front. Digits and
    // Latin embedded in an Arabic sentence therefore still read forwards,
    // which is the case that actually occurs in this interface -- "%d" in the
    // middle of a translated label.
    std::vector<unsigned> out;
    out.reserve(in.size());

    std::vector<std::pair<size_t, size_t>> runs;   // [begin, end)
    std::vector<bool> rtl;
    size_t i = 0;
    while (i < in.size()) {
        const bool r = isArabic(in[i]);
        size_t j = i;
        while (j < in.size() && isArabic(in[j]) == r) ++j;
        runs.emplace_back(i, j);
        rtl.push_back(r);
        i = j;
    }

    for (size_t k = runs.size(); k-- > 0;) {
        const auto [b, e] = runs[k];
        if (rtl[k]) {
            for (size_t m = e; m-- > b;) out.push_back(in[m]);
        } else {
            // A neutral run between two Arabic runs belongs between them in
            // reading order; its own contents stay forwards.
            for (size_t m = b; m < e; ++m) out.push_back(in[m]);
        }
    }
    return out;
}

}  // namespace odText
