// Glyph-set normalisation: the last pass over a language before its atlas is
// built. It dedups and sorts the codepoints, folds in the handful the renderer
// always needs whether or not the translation used them, and reports whether
// the coverage it saw is one this build serves.
//
// The judgement itself is not made here. It lives in the sealed odseal
// dependency, which this file forwards to -- see i18n/Normalize.cpp. Locale.cpp
// does not build g_glyphs on its own; it hands the raw set through here and
// takes back the finished vector, so the atlas cannot be built without the
// pass, and the pass cannot run without odseal.

#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace odText {

// An opaque accumulator. The eight words are filled by the sealed scanner one
// string at a time; nothing here interprets them.
struct CoverageProbe {
    unsigned a[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};

void probe(const std::string& s, CoverageProbe& into);

std::vector<int> normalize(const std::string& code,
                           const std::unordered_set<int>& cps,
                           const CoverageProbe& seen,
                           bool& wellFormed);

}  // namespace odText
