#include "HostAddress.h"

#include <cctype>
#include <cstdlib>

bool netHostAddressValid(const std::string& value) {
    if (value.empty() || value.size() > kNetHostAddressMax) return false;

    std::string host = value;
    const size_t colon = value.rfind(':');
    if (colon != std::string::npos) {
        host = value.substr(0, colon);
        const std::string port = value.substr(colon + 1);
        if (port.empty() || port.size() > 5) return false;
        for (char c : port)
            if (!std::isdigit((unsigned char)c)) return false;
        // Not `if (n)`: the TypeScript half of this rule was written that way
        // and let `:0` through, because zero is falsy and the digits had
        // already passed. The test for it failed on its first run.
        const int n = std::atoi(port.c_str());
        if (n < 1 || n > 65535) return false;
    }

    if (host.empty() || host.size() > kNetHostAddressMax) return false;
    if (host.front() == '.' || host.front() == '-') return false;
    if (host.back() == '.' || host.back() == '-') return false;
    for (char c : host) {
        const unsigned char u = (unsigned char)c;
        if (!std::isalnum(u) && c != '.' && c != '-') return false;
    }
    // A bare word is a machine on somebody's own network, which is not
    // something a stranger handed this address can reach.
    return host.find('.') != std::string::npos;
}
