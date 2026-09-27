#include "JoinLink.h"

#include "net/HostAddress.h"

#include <cctype>

namespace joinlink {
namespace {

/** The value of `at=` in a query string, or empty. Nothing else is read. */
std::string atValue(const std::string& query) {
    size_t at = 0;
    while (at < query.size()) {
        size_t end = query.find('&', at);
        if (end == std::string::npos) end = query.size();
        const std::string pair = query.substr(at, end - at);
        const size_t eq = pair.find('=');
        // ONE KEY. Everything else in the query is ignored rather than
        // collected: a handler that gathers whatever it is given is a handler
        // that grows a parameter nobody meant to add.
        if (eq != std::string::npos && pair.compare(0, eq, "at") == 0)
            return pair.substr(eq + 1);
        at = end + 1;
    }
    return {};
}

}  // namespace

Invite parse(const std::string& url) {
    Invite out;
    static const std::string kPrefix = "opendoctrines://join/";
    if (url.size() <= kPrefix.size()) return out;
    if (url.compare(0, kPrefix.size(), kPrefix) != 0) return out;

    std::string rest = url.substr(kPrefix.size());

    // The fragment belongs to nobody here; drop it before anything else reads
    // the string, so `?at=x#y` does not put a `#y` inside the address.
    const size_t hash = rest.find('#');
    if (hash != std::string::npos) rest.erase(hash);

    std::string query;
    const size_t q = rest.find('?');
    if (q != std::string::npos) {
        query = rest.substr(q + 1);
        rest.erase(q);
    }

    std::string code = rest;
    // A chat client may add a trailing slash. Everything from it is not the code.
    const size_t cut = code.find('/');
    if (cut != std::string::npos) code.erase(cut);

    if (code.empty() || code.size() > 32) return out;
    // Letters, digits, dash and underscore. Anything else -- a dot, a slash
    // that survived, a percent escape -- is refused rather than decoded: there
    // is no session code containing one, and decoding is where this kind of
    // handler goes wrong.
    for (unsigned char c : code)
        if (!std::isalnum(c) && c != '-' && c != '_') return out;
    out.code = code;

    // The address, on the same terms the board's is on. NOT percent-decoded,
    // for the reason above: a `:` needs no escaping in a query value, nothing
    // that writes these links emits one, and decoding here would be a second
    // way to spell every address this is supposed to be checking.
    const std::string address = atValue(query);
    if (!address.empty() && netHostAddressValid(address)) out.address = address;
    return out;
}

std::string codeFrom(const std::string& url) { return parse(url).code; }

}  // namespace joinlink
