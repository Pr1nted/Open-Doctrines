#include "Socks5.h"

#include <atomic>

namespace socks5 {

namespace {
std::atomic<bool> g_all{false};
std::atomic<int>  g_port{0};

bool endsWith(const std::string& s, const char* suffix) {
    const std::string t(suffix);
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}
}  // namespace

void setRouteAll(bool all) { g_all.store(all); }
bool routeAll() { return g_all.load(); }
void setPort(int p) { g_port.store(p > 0 && p < 65536 ? p : 0); }
int  port() { return g_port.load(); }

bool wanted(const std::string& hostIn) {
    std::string host = hostIn;
    for (char& c : host) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (endsWith(host, ".onion")) return true;
    if (host == "localhost" || host == "127.0.0.1" || host == "::1" ||
        endsWith(host, ".localhost"))
        return false;
    return g_all.load();
}

std::vector<uint8_t> greeting() { return {0x05, 0x01, 0x00}; }

std::vector<uint8_t> connectRequest(const std::string& host, uint16_t p) {
    std::vector<uint8_t> out = {0x05, 0x01, 0x00, 0x03};
    const size_t n = host.size() > 255 ? 255 : host.size();
    out.push_back((uint8_t)n);
    out.insert(out.end(), host.begin(), host.begin() + (long)n);
    out.push_back((uint8_t)(p >> 8));
    out.push_back((uint8_t)(p & 0xFF));
    return out;
}

int replyRemainder(const uint8_t r[5]) {
    // VER REP RSV ATYP, then the bound address and port. The fifth byte is the
    // first byte of the address: the length itself, for a domain name.
    switch (r[3]) {
        case 0x01: return 4 - 1 + 2;          // IPv4
        case 0x04: return 16 - 1 + 2;         // IPv6
        case 0x03: return r[4] + 2;           // name: length byte already read
        default:   return -1;
    }
}

std::string replyText(uint8_t code) {
    switch (code) {
        case 0x00: return "connected";
        case 0x01: return "Tor could not make the connection";
        case 0x02: return "Tor's settings forbid this connection";
        case 0x03: return "the network is unreachable through Tor";
        case 0x04: return "that host is unreachable through Tor (it may be offline)";
        case 0x05: return "the server refused the connection";
        case 0x06: return "Tor gave up waiting (TTL expired)";
        case 0x07: return "Tor does not support that request";
        case 0x08: return "Tor does not support that address type";
        // Tor's own extensions (proposal 304), for onion services.
        case 0xF0: return "that onion service could not be found -- check the address";
        case 0xF1: return "that onion service's descriptor is invalid";
        case 0xF2: return "could not reach that onion service's introduction points";
        case 0xF3: return "that onion service's rendezvous failed";
        case 0xF4: return "that onion service needs client authorisation";
        case 0xF5: return "that onion service's client authorisation was refused";
        case 0xF6: return "that is not a valid onion address";
        case 0xF7: return "that onion address is malformed";
    }
    return "Tor refused the connection (code " + std::to_string(code) + ")";
}

}  // namespace socks5
