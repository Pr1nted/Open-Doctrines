#include "achievements/GrantVerify.h"

#include "util/Ed25519.h"
#include "json.hpp"

#if __has_include("AccountIssuer.h")
#include "AccountIssuer.h"
#endif
#ifndef OD_ACHIEVEMENT_KEYS_BAKED
#define OD_ACHIEVEMENT_KEYS_BAKED ""
#endif

#include <cstdlib>

namespace odach {

std::vector<uint8_t> b64urlDecode(const std::string& in) {
    std::vector<uint8_t> out;
    unsigned buf = 0;
    int bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else if (c == '=') break;
        else return {};
        buf = (buf << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)((buf >> bits) & 0xFF)); }
    }
    return out;
}

std::vector<std::vector<uint8_t>> parseGrantKeys(const std::string& raw) {
    std::vector<std::vector<uint8_t>> out;
    size_t start = 0;
    while (start <= raw.size()) {
        const size_t comma = raw.find(',', start);
        std::string part = raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        while (!part.empty() && (part.back() == ' ' || part.back() == '\n')) part.pop_back();
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        auto k = b64urlDecode(part);
        if (k.size() == 32) out.push_back(k);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

std::vector<std::vector<uint8_t>> grantPublicKeys() {
    if (const char* e = std::getenv("OD_ACHIEVEMENT_KEYS")) return parseGrantKeys(e);
    return parseGrantKeys(OD_ACHIEVEMENT_KEYS_BAKED);
}

bool verifyGrantToken(const std::string& token, const std::string& issuer,
                      const std::vector<std::vector<uint8_t>>& keys, GrantFields& out) {
    if (token.size() > 4096) return false;
    const size_t a = token.find('.');
    if (a == std::string::npos) return false;
    const size_t b = token.find('.', a + 1);
    if (b == std::string::npos || token.find('.', b + 1) != std::string::npos) return false;
    if (token.compare(0, a, "oda1") != 0) return false;

    const std::vector<uint8_t> sig = b64urlDecode(token.substr(b + 1));
    if (sig.size() != 64) return false;
    // The signature covers the text exactly as transmitted: nothing here
    // re-serialises JSON, which is where signature bugs usually live.
    const std::string signedPart = token.substr(0, b);
    bool ok = false;
    for (const auto& key : keys) {
        if (key.size() != 32) continue;
        if (ed25519Verify(sig.data(), (const uint8_t*)signedPart.data(), signedPart.size(), key.data())) {
            ok = true;
            break;
        }
    }
    if (!ok) return false;

    const std::vector<uint8_t> payload = b64urlDecode(token.substr(a + 1, b - a - 1));
    nlohmann::json j = nlohmann::json::parse(std::string(payload.begin(), payload.end()), nullptr, false);
    if (!j.is_object()) return false;
    if (!j.contains("aud") || !j["aud"].is_string() || j["aud"].get<std::string>() != "od-achievement") return false;
    GrantFields f;
    if (j.contains("iss") && j["iss"].is_string()) f.iss = j["iss"].get<std::string>();
    // Without a configured issuer, the key list alone says who may sign.
    if (!issuer.empty() && f.iss != issuer) return false;
    if (!j.contains("sub") || !j["sub"].is_string() || !j.contains("ach") || !j["ach"].is_string()) return false;
    f.sub = j["sub"].get<std::string>();
    f.ach = j["ach"].get<std::string>();
    if (j.contains("iat") && j["iat"].is_number_integer()) f.iat = j["iat"].get<long long>();
    f.modded = j.contains("mod") && j["mod"].is_number_integer() && j["mod"].get<int>() == 1;
    out = f;
    return true;
}

}  // namespace odach
