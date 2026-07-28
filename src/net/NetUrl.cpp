#include "NetUrl.h"

#include <cctype>
#include <cstdlib>

// There is deliberately no userinfo ("wss://user@evil.example/") handling: the
// safe treatment of userinfo is to refuse it outright, because it is the oldest
// trick for making a URL appear to point somewhere it does not.
bool NetUrl::parse(const std::string& url, NetUrl& out) {
    NetUrl u;

    size_t at = 0;
    if (url.compare(0, 6, "wss://") == 0)        { u.secure = true;  u.port = 443; at = 6; }
    else if (url.compare(0, 8, "https://") == 0) { u.secure = true;  u.port = 443; at = 8; }
    else if (url.compare(0, 5, "ws://") == 0)    { u.secure = false; u.port = 80;  at = 5; }
    else if (url.compare(0, 7, "http://") == 0)  { u.secure = false; u.port = 80;  at = 7; }
    else return false;

    const size_t slash = url.find('/', at);
    std::string authority = slash == std::string::npos
        ? url.substr(at) : url.substr(at, slash - at);
    u.path = slash == std::string::npos ? "/" : url.substr(slash);

    if (authority.empty()) return false;
    if (authority.find('@') != std::string::npos) return false;   // userinfo

    // A bracketed IPv6 literal is the one authority form with a colon that is
    // not a port separator.
    if (authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        u.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return false;
            authority = authority.substr(close + 2);
        } else {
            authority.clear();
        }
    } else {
        const size_t colon = authority.find(':');
        if (colon == std::string::npos) { u.host = authority; authority.clear(); }
        else { u.host = authority.substr(0, colon); authority = authority.substr(colon + 1); }
    }

    if (!authority.empty()) {
        for (char c : authority) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        const long port = std::strtol(authority.c_str(), nullptr, 10);
        if (port <= 0 || port > 65535) return false;
        u.port = static_cast<uint16_t>(port);
    }

    if (u.host.empty() || u.host.size() > 253) return false;
    for (char c : u.host) {
        const bool allowed = std::isalnum(static_cast<unsigned char>(c)) ||
                             c == '.' || c == '-' || c == ':';   // ':' for IPv6
        if (!allowed) return false;
    }
    // A path is sent verbatim in the request line, so a control character or a
    // space in it would let a caller inject a second HTTP header.
    for (char c : u.path) {
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) return false;
    }

    out = u;
    return true;
}
