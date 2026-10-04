#pragma once

// Checking an achievement grant, and nothing else.
//
// Pure: a token, a list of public keys, an expected issuer in; a yes or no
// out. No network, no files, no game -- which is what lets the test link it
// alone against vectors the Worker's own crypto produced, and what lets the
// launcher (Unifico) carry the identical code. See net/src/achievements/grant.ts
// for the format and src/achievements/Achievements.h for what a grant means.

#include <cstdint>
#include <string>
#include <vector>

namespace odach {

struct GrantFields {
    std::string sub;      // account id
    std::string ach;      // achievement id
    std::string iss;
    long long   iat = 0;
    bool        modded = false;
};

/** base64url (or base64) to bytes. Empty on any invalid character. */
std::vector<uint8_t> b64urlDecode(const std::string& in);

/**
 * The keys grants may be signed with: OD_ACHIEVEMENT_KEYS from the environment
 * if set, else the list baked into the build (-DOD_ACHIEVEMENT_KEYS). Base64url
 * raw Ed25519 keys, comma-separated. A LIST so that a key change never orphans
 * grants already issued.
 */
std::vector<std::vector<uint8_t>> grantPublicKeys();

/** Parse "k1,k2,..." into 32-byte keys, skipping anything malformed. */
std::vector<std::vector<uint8_t>> parseGrantKeys(const std::string& csv);

/**
 * True when `token` is "oda1.<payload>.<sig>", the signature verifies under one
 * of `keys`, the audience is od-achievement, and -- when `issuer` is non-empty
 * -- the issuer matches. Fills `out` only on success.
 */
bool verifyGrantToken(const std::string& token, const std::string& issuer,
                      const std::vector<std::vector<uint8_t>>& keys, GrantFields& out);

}  // namespace odach
