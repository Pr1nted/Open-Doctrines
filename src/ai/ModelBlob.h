#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * The container the AI model file is stored in: "ODAZ".
 *
 * WHY THIS EXISTS. A model is 8.9 MB and every byte of it is float32 -- weights
 * plus the two Adam moments that ride along so resumed training keeps its
 * momentum. Float arrays are not incompressible, they are compressed WRONG by a
 * plain deflate pass: consecutive weights have near-identical sign and exponent
 * bytes and unrelated mantissa bytes, so an LZ window that sees them
 * interleaved finds a match roughly never. Deinterleaving the four byte
 * positions into four planes first puts the repetitive bytes next to each
 * other, and deflate then does what it is good at.
 *
 *   plain                    8,974,598
 *   deflate 9                4,307,539   48.0%
 *   deinterleave + deflate 9 4,025,937   44.9%
 *
 * Nothing is approximated: every weight comes back bit for bit. The shipped
 * model, the APK asset and the web download are all this file, so this is 5 MB
 * off each of them.
 *
 * COMPATIBILITY. unpack() accepts a plain "ODAI" file unchanged, so a model
 * written by an older build -- or by a worker mid-run -- still loads. Only the
 * writer changed. An older build cannot read an ODAZ file, which is the honest
 * failure: it prints "Fresh model" rather than reading noise as weights.
 */
namespace modelblob {

/// Deflate level for files written by the game itself. See ModelBlob.cpp.
constexpr int SAVE_LEVEL = 9;

/// True when these bytes are an ODAZ container rather than a plain ODAI file.
bool isPacked(const uint8_t* data, size_t size);

/// Plain ODAI bytes -> ODAZ container. Empty on failure.
std::vector<uint8_t> pack(const std::vector<uint8_t>& plain, int level = SAVE_LEVEL);

/**
 * ODAZ -> plain ODAI, in place. A buffer that is not ODAZ is left untouched and
 * reported as success, so callers can hand any model file straight to this.
 * False means the container is present but corrupt.
 */
bool unpack(std::vector<uint8_t>& bytes);

}  // namespace modelblob
