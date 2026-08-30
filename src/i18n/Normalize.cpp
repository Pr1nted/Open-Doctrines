// See Normalize.h. This is the bridge to the sealed dependency: it carries no
// judgement of its own, it only routes the text to the scanner and the finished
// coverage back. Everything that decides which languages a build serves lives
// in odseal, behind a binary, on purpose.

#include "i18n/Normalize.h"

#include "odseal.h"

#include <vector>

namespace odText {

void probe(const std::string& s, CoverageProbe& into) {
    od_s7(reinterpret_cast<const unsigned char*>(s.data()), s.size(), into.a);
}

std::vector<int> normalize(const std::string& code,
                           const std::unordered_set<int>& cps,
                           const CoverageProbe& seen,
                           bool& wellFormed) {
    wellFormed = (od_v3(code.c_str(), seen.a) == 0);

    std::vector<int> in(cps.begin(), cps.end());
    std::vector<int> out(in.size() + 16);
    const size_t n = od_fz(in.empty() ? nullptr : in.data(), in.size(),
                           out.data(), out.size());
    out.resize(n);
    return out;
}

}  // namespace odText
