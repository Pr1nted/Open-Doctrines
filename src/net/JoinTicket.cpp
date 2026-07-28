#include "JoinTicket.h"

#include "HttpClient.h"
#include "util/Ed25519.h"

#include <cstring>

namespace {

// Must match TOKEN_PREFIX in net/src/auth/token.ts. The algorithm is NOT a
// field in this format, which is the point: every historic JWT vulnerability is
// downstream of a verifier trusting an `alg` header sent by the attacker.
const char kTokenPrefix[] = "od1";

// A ticket is a few hundred bytes. This is a bound on work done before
// anything has been authenticated, not a format limit.
constexpr size_t kMaxTokenBytes = 8 * 1024;

// The signed claims come from the issuer, so these are defence in depth rather
// than parsing limits: a bug at the issuer must not become unbounded memory
// here.
constexpr size_t kMaxPsid = 64;
constexpr size_t kMaxName = 64;
constexpr size_t kMaxNonce = 64;
constexpr size_t kMaxJti = 64;
constexpr size_t kMaxIssuer = 256;
constexpr size_t kMaxAudience = 128;
constexpr size_t kMaxBadges = 8;

/** base64url decode, no padding required. Empty on any invalid input. */
std::vector<uint8_t> b64urlDecode(const std::string& in) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };

    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4 + 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;            // tolerated, though we never emit it
        const int v = value(c);
        if (v < 0) return {};           // standard base64 or junk: not ours
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    // Leftover bits must be zero padding, not a truncated byte.
    if (bits >= 6) return {};
    if (bits && (acc & ((1u << bits) - 1))) return {};
    return out;
}

}  // namespace

// ------------------------------------------------------------------ keys ----

std::vector<NetIssuerKey> netParseIssuerKeys(const std::string& json) {
    std::vector<NetIssuerKey> keys;
    if (json.empty() || json.size() > 64 * 1024) return keys;

    // Walk the objects inside the "keys" array by brace depth. A JWK set is
    // small and fixed in shape, so this is enough without a real parser -- but
    // it must not be fooled by a "keys" that is a VALUE rather than a name,
    // which is what httpJsonScope is careful about.
    const size_t at = httpJsonScope(json, "keys");
    if (at == std::string::npos) return keys;

    const size_t arrayStart = json.find('[', at);
    if (arrayStart == std::string::npos) return keys;

    size_t i = arrayStart + 1;
    int depth = 0;
    size_t objectStart = std::string::npos;
    bool inString = false, escaped = false;

    for (; i < json.size(); i++) {
        const char c = json[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{') { if (depth++ == 0) objectStart = i; continue; }
        if (c == '}') {
            if (--depth == 0 && objectStart != std::string::npos) {
                const std::string object = json.substr(objectStart, i - objectStart + 1);

                // Only OKP/Ed25519 keys. Anything else in the set is for some
                // other purpose and must not be used to verify a ticket.
                if (httpJsonString(object, "kty", 16) == "OKP" &&
                    httpJsonString(object, "crv", 16) == "Ed25519") {
                    const std::vector<uint8_t> x =
                        b64urlDecode(httpJsonString(object, "x", 128));
                    if (x.size() == 32) {
                        NetIssuerKey k;
                        std::memcpy(k.bytes, x.data(), 32);
                        keys.push_back(k);
                    }
                }
                objectStart = std::string::npos;
            }
            continue;
        }
        if (c == ']' && depth == 0) break;
    }
    return keys;
}

// ---------------------------------------------------------------- verify ----

bool netVerifyJoinTicket(const std::string& token,
                         const std::vector<NetIssuerKey>& keys,
                         const NetTicketCheck& expect,
                         NetJoinTicket& out) {
    // No keys means we cannot verify anyone. That is a refusal, not a pass.
    if (keys.empty()) return false;
    if (token.empty() || token.size() > kMaxTokenBytes) return false;
    if (expect.issuer.empty() || expect.audience.empty() || expect.nonce.empty())
        return false;

    // "od1.<claims>.<signature>", exactly three parts.
    const size_t firstDot = token.find('.');
    if (firstDot == std::string::npos) return false;
    const size_t secondDot = token.find('.', firstDot + 1);
    if (secondDot == std::string::npos) return false;
    if (token.find('.', secondDot + 1) != std::string::npos) return false;

    if (token.compare(0, firstDot, kTokenPrefix) != 0) return false;

    const std::string claimsPart = token.substr(firstDot + 1, secondDot - firstDot - 1);
    const std::string sigPart    = token.substr(secondDot + 1);
    if (claimsPart.empty() || sigPart.empty()) return false;

    const std::vector<uint8_t> signature = b64urlDecode(sigPart);
    if (signature.size() != 64) return false;

    // Signed bytes are the first two parts including the dot, exactly as
    // token.ts composes them.
    const std::string signed_ = token.substr(0, secondDot);

    bool signatureHeld = false;
    for (const NetIssuerKey& k : keys) {
        if (ed25519Verify(signature.data(),
                          reinterpret_cast<const uint8_t*>(signed_.data()),
                          signed_.size(), k.bytes)) {
            signatureHeld = true;
            break;
        }
    }
    if (!signatureHeld) return false;

    const std::vector<uint8_t> claimBytes = b64urlDecode(claimsPart);
    if (claimBytes.empty() || claimBytes.size() > kMaxTokenBytes) return false;
    const std::string claims(claimBytes.begin(), claimBytes.end());

    NetJoinTicket t;
    t.issuer   = httpJsonString(claims, "iss", kMaxIssuer);
    t.audience = httpJsonString(claims, "aud", kMaxAudience);
    t.psid     = httpJsonString(claims, "psid", kMaxPsid);
    t.name     = httpJsonString(claims, "name", kMaxName);
    t.nonce    = httpJsonString(claims, "nonce", kMaxNonce);
    t.jti      = httpJsonString(claims, "jti", kMaxJti);
    t.issuedAt = httpJsonNumber(claims, "iat", 0);
    t.expires  = httpJsonNumber(claims, "exp", 0);
    t.badges   = httpJsonStringArray(claims, "badges", kMaxBadges);

    // Exact matches, never prefixes. A ticket for session "ABCD" must not
    // satisfy a host checking for "ABC".
    if (t.issuer != expect.issuer) return false;
    if (t.audience != expect.audience) return false;
    if (t.nonce != expect.nonce) return false;

    // A ticket with no pseudonym or no jti cannot be seated or burned, so it is
    // unusable whatever else it says.
    if (t.psid.empty() || t.jti.empty()) return false;

    const long long skew = expect.skewSeconds < 0 ? 0 : expect.skewSeconds;
    if (t.expires <= 0 || t.expires + skew <= expect.now) return false;
    // Issued implausibly far in the future: our clock or theirs is wrong enough
    // that the freshness window means nothing.
    if (t.issuedAt > 0 && t.issuedAt - skew > expect.now) return false;

    out = std::move(t);
    return true;
}

// ----------------------------------------------------------- issuer clock ----

void NetIssuerClock::observe(long long serverTime, long long localTime) {
    // 0 means the reply carried no usable Date. Keep whatever we knew before
    // rather than resetting to "our clock is right", which is the assumption
    // this class exists to avoid.
    if (serverTime <= 0) return;
    m_offset = serverTime - localTime;
    m_known = true;
}

// ------------------------------------------------------------- jti reuse ----

bool NetTicketReplayGuard::useOnce(const std::string& jti, long long expires,
                                   long long now) {
    if (jti.empty()) return false;
    sweep(now);
    for (const Used& u : m_seen)
        if (u.jti == jti) return false;
    m_seen.push_back(Used{jti, expires});
    return true;
}

void NetTicketReplayGuard::sweep(long long now) {
    for (size_t i = m_seen.size(); i-- > 0;)
        if (m_seen[i].until <= now)
            m_seen.erase(m_seen.begin() + static_cast<long>(i));
}
