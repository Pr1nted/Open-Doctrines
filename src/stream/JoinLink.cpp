#include "JoinLink.h"

#include <cctype>

namespace joinlink {

std::string codeFrom(const std::string& url) {
    static const std::string kPrefix = "opendoctrines://join/";
    if (url.size() <= kPrefix.size()) return {};
    if (url.compare(0, kPrefix.size(), kPrefix) != 0) return {};

    std::string code = url.substr(kPrefix.size());
    // A chat client may add a trailing slash, and a browser may append a query
    // or a fragment. Everything from the first of those is not the code.
    const size_t cut = code.find_first_of("/?#");
    if (cut != std::string::npos) code.erase(cut);

    if (code.empty() || code.size() > 32) return {};
    // Letters, digits, dash and underscore. Anything else -- a dot, a slash
    // that survived, a percent escape -- is refused rather than decoded: there
    // is no session code containing one, and decoding is where this kind of
    // handler goes wrong.
    for (unsigned char c : code)
        if (!std::isalnum(c) && c != '-' && c != '_') return {};
    return code;
}

}  // namespace joinlink
