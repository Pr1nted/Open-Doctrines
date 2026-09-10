#include "RelayLink.h"

#include <algorithm>
#include <cctype>

namespace netrelay {
namespace {

// The service's own alphabet and shape, from net/src/util/crypto.ts and
// net/src/lobby/session.ts. No I, L, O, 0 or 1: the point is that a code read
// aloud or off a stream cannot be ambiguous.
const char* kAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
constexpr size_t kHalf = 4;             // SESSION_CODE_CHARS / 2

bool inAlphabet(char c) {
    for (const char* p = kAlphabet; *p; ++p) if (*p == c) return true;
    return false;
}

}  // namespace

bool isSessionCode(const std::string& code) {
    if (code.size() != kHalf * 2 + 1) return false;
    if (code[kHalf] != '-') return false;
    for (size_t i = 0; i < code.size(); ++i) {
        if (i == kHalf) continue;
        if (!inAlphabet(code[i])) return false;
    }
    return true;
}

std::string normaliseCode(const std::string& typed) {
    std::string body;
    for (unsigned char c : typed) {
        if (std::isspace(c) || c == '-' || c == '_') continue;
        const char up = (char)std::toupper(c);
        // Refused rather than repaired: see the header. A character outside the
        // alphabet means this is not a code, not that it is a code with a typo.
        if (!inAlphabet(up)) return {};
        body += up;
        if (body.size() > kHalf * 2) return {};
    }
    if (body.size() != kHalf * 2) return {};
    const std::string out = body.substr(0, kHalf) + "-" + body.substr(kHalf);
    return isSessionCode(out) ? out : std::string();
}

bool decodeToHost(const uint8_t* data, size_t len, Inbound& out) {
    if (!data || len < 3) return false;
    const uint8_t kind = data[0];
    if (kind != (uint8_t)ToHost::Data &&
        kind != (uint8_t)ToHost::PeerJoined &&
        kind != (uint8_t)ToHost::PeerLeft) {
        // A tag this build does not know is a newer relay, not an attack. The
        // caller drops the frame; it does not close the session over it.
        return false;
    }
    out.kind = (ToHost)kind;
    // LITTLE-ENDIAN, matching toHost() in LobbyDO.ts. Getting this backwards
    // would not drop a connection -- it would deliver one player's turn to
    // another, which is why it is written once, here, and tested.
    out.peerId = (uint16_t)(data[1] | (data[2] << 8));
    out.payload.assign(data + 3, data + len);
    return true;
}

std::vector<uint8_t> encodeFromHost(FromHost kind, uint16_t peerId,
                                    const uint8_t* payload, size_t len) {
    std::vector<uint8_t> f;
    f.reserve(3 + len);
    f.push_back((uint8_t)kind);
    f.push_back((uint8_t)(peerId & 0xFF));
    f.push_back((uint8_t)((peerId >> 8) & 0xFF));
    if (payload && len) f.insert(f.end(), payload, payload + len);
    return f;
}

std::string helloFrame(const std::string& ticket) {
    // Built by hand rather than through a JSON writer: the only field is a
    // ticket, which is base64url and therefore has nothing in it to escape.
    std::string safe;
    safe.reserve(ticket.size());
    for (char c : ticket) {
        const bool ok = std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.';
        if (ok) safe += c;
    }
    return "{\"ticket\":\"" + safe + "\"}";
}

bool parseHelloReply(const std::string& text, uint16_t& peerId, std::string& role) {
    if (text.find("\"ok\"") == std::string::npos) return false;
    if (text.find("true") == std::string::npos) return false;

    const size_t at = text.find("\"peerId\"");
    if (at == std::string::npos) return false;
    const size_t colon = text.find(':', at);
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    while (i < text.size() && std::isspace((unsigned char)text[i])) ++i;
    if (i >= text.size() || !std::isdigit((unsigned char)text[i])) return false;
    unsigned long v = 0;
    for (; i < text.size() && std::isdigit((unsigned char)text[i]); ++i) {
        v = v * 10 + (unsigned long)(text[i] - '0');
        if (v > 0xFFFF) return false;
    }
    peerId = (uint16_t)v;

    role.clear();
    const size_t r = text.find("\"role\"");
    if (r != std::string::npos) {
        const size_t q1 = text.find('"', text.find(':', r) + 1);
        const size_t q2 = (q1 == std::string::npos) ? std::string::npos : text.find('"', q1 + 1);
        if (q1 != std::string::npos && q2 != std::string::npos && q2 - q1 < 32)
            role = text.substr(q1 + 1, q2 - q1 - 1);
    }
    return true;
}

std::string relayUrl(const std::string& issuer, const std::string& code,
                     const char* role) {
    if (!isSessionCode(code)) return {};
    if (!role || !*role) return {};
    for (const char* p = role; *p; ++p)
        if (!std::isalpha((unsigned char)*p)) return {};

    std::string base = issuer;
    while (!base.empty() && base.back() == '/') base.pop_back();

    std::string scheme;
    if (base.rfind("https://", 0) == 0)      scheme = "wss://",  base = base.substr(8);
    else if (base.rfind("http://", 0) == 0)  scheme = "ws://",   base = base.substr(7);
    else return {};                       // not a URL this understands
    if (base.empty()) return {};
    // Nothing that could turn one URL into another: a host with a query, a
    // fragment or whitespace in it did not come from a setting a person typed.
    if (base.find_first_of(" \t\r\n?#\\") != std::string::npos) return {};

    return scheme + base + "/session/" + code + "/ws?role=" + role;
}

}  // namespace netrelay
