// The opening handshake, shared by the client and the server. See WsHandshake.h
// for why it is one unit rather than two.

#include "WsHandshake.h"
#include "Sha1.h"

#include <cctype>
#include <cstdlib>

namespace {

// RFC 6455 section 1.3. Every character matters: get one wrong and the accept
// digest is wrong, which means no handshake anywhere ever completes. The RFC's
// worked example is asserted in the tests precisely to pin this string.
const char kWsGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

constexpr size_t kMaxRequestBytes = 16 * 1024;
constexpr size_t kMaxHeaderValue = 512;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

}  // namespace

std::string wsBase64(const uint8_t* data, size_t n) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = i + 1 < n ? data[i + 1] : 0;
        const uint32_t c = i + 2 < n ? data[i + 2] : 0;
        const uint32_t v = (a << 16) | (b << 8) | c;
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += i + 1 < n ? T[(v >> 6) & 63] : '=';
        out += i + 2 < n ? T[v & 63] : '=';
    }
    return out;
}

std::string wsAcceptFor(const std::string& key) {
    if (key.empty()) return {};
    Sha1 sha;
    const std::string joined = key + kWsGuid;
    sha.update(reinterpret_cast<const uint8_t*>(joined.data()), joined.size());
    uint8_t digest[20];
    sha.finish(digest);
    return wsBase64(digest, sizeof(digest));
}

bool wsParseUpgrade(const std::string& request, WsUpgradeRequest& out) {
    if (request.empty() || request.size() > kMaxRequestBytes) return false;

    // The header block must be complete. A partial request is not a failed
    // one -- the caller should keep reading -- but it is not a success either,
    // and conflating those is how a server replies to half a handshake.
    const size_t headEnd = request.find("\r\n\r\n");
    if (headEnd == std::string::npos) return false;

    const size_t firstEol = request.find("\r\n");
    if (firstEol == std::string::npos) return false;
    const std::string requestLine = request.substr(0, firstEol);

    // "GET <path> HTTP/1.1". Anything else is not a client we serve.
    if (requestLine.compare(0, 4, "GET ") != 0) return false;
    const size_t pathEnd = requestLine.find(' ', 4);
    if (pathEnd == std::string::npos) return false;
    if (requestLine.find("HTTP/1.1", pathEnd) == std::string::npos) return false;

    WsUpgradeRequest r;
    r.path = requestLine.substr(4, pathEnd - 4);
    if (r.path.empty() || r.path[0] != '/' || r.path.size() > 512) return false;

    size_t at = firstEol + 2;
    while (at < headEnd) {
        size_t eol = request.find("\r\n", at);
        if (eol == std::string::npos || eol > headEnd) eol = headEnd;
        const std::string line = request.substr(at, eol - at);
        at = eol + 2;
        if (line.empty()) continue;

        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (value.size() > kMaxHeaderValue) return false;

        if (name == "upgrade") {
            r.upgrade = lower(value).find("websocket") != std::string::npos;
        } else if (name == "connection") {
            // Comma-separated and case-insensitive; a proxy may add tokens.
            r.connectionUpgrade = lower(value).find("upgrade") != std::string::npos;
        } else if (name == "sec-websocket-key") {
            // 16 random bytes, base64 -- so always 24 characters ending "==".
            // Checked because it is fed straight into the accept digest.
            if (value.size() != 24) return false;
            for (char c : value) {
                const bool ok = std::isalnum(static_cast<unsigned char>(c)) ||
                                c == '+' || c == '/' || c == '=';
                if (!ok) return false;
            }
            r.key = value;
        } else if (name == "sec-websocket-version") {
            r.version = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
        }
    }

    if (!r.valid()) return false;
    out = r;
    return true;
}

std::string wsUpgradeResponse(const WsUpgradeRequest& request) {
    if (!request.valid()) {
        // A plain 400 with no detail. Explaining which header was wrong helps a
        // real client not at all -- they are all mandatory -- and helps someone
        // probing the port rather more.
        return "HTTP/1.1 400 Bad Request\r\n"
               "Connection: close\r\n"
               "Content-Length: 0\r\n\r\n";
    }
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " + wsAcceptFor(request.key) + "\r\n\r\n";
}
