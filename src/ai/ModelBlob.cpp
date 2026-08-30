#include "ModelBlob.h"

#include <cstring>

#include "miniz.h"

namespace modelblob {
namespace {

// "ODAZ", then a format byte, a reserved byte, and the uncompressed length.
// The length is stored rather than guessed at because inflating needs the
// output size up front and a model has no other record of it.
constexpr char MAGIC[4] = {'O', 'D', 'A', 'Z'};
constexpr uint8_t FORMAT = 1;
constexpr size_t HEADER = 14;

// A model is ~9 MB. The cap is three orders of magnitude of headroom and its
// only job is to stop a corrupt length field from asking for a 16 EB
// allocation before anything has had a chance to notice the file is damaged.
constexpr uint64_t MAX_RAW = 512ull << 20;

/**
 * Deinterleave the four byte positions of each float into four planes.
 *
 * Byte 3 of an IEEE float is its sign and most of its exponent, and a layer of
 * trained weights is thousands of values of similar magnitude -- so that plane
 * is long runs of a handful of distinct bytes. Byte 0 is the low mantissa and
 * is close to noise. Interleaved, deflate sees noise every fourth byte and its
 * matches die at three bytes; separated, the compressible planes compress.
 *
 * The tail (size % 4 bytes, two of them in practice -- the ODAI header is six
 * bytes long) is copied through unchanged, so this is defined for any length.
 */
void deinterleave(const uint8_t* in, size_t n, uint8_t* out) {
    const size_t groups = n / 4;
    for (size_t k = 0; k < 4; ++k)
        for (size_t i = 0; i < groups; ++i) out[k * groups + i] = in[i * 4 + k];
    memcpy(out + groups * 4, in + groups * 4, n - groups * 4);
}

void interleave(const uint8_t* in, size_t n, uint8_t* out) {
    const size_t groups = n / 4;
    for (size_t k = 0; k < 4; ++k)
        for (size_t i = 0; i < groups; ++i) out[i * 4 + k] = in[k * groups + i];
    memcpy(out + groups * 4, in + groups * 4, n - groups * 4);
}

void putU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back((uint8_t)(v >> (i * 8)));
}

uint64_t getU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

}  // namespace

bool isPacked(const uint8_t* data, size_t size) {
    return data && size >= HEADER && memcmp(data, MAGIC, 4) == 0 && data[4] == FORMAT;
}

std::vector<uint8_t> pack(const std::vector<uint8_t>& plain, int level) {
    std::vector<uint8_t> out;
    if (plain.empty() || plain.size() > MAX_RAW) return out;

    std::vector<uint8_t> planes(plain.size());
    deinterleave(plain.data(), plain.size(), planes.data());

    mz_ulong bound = mz_compressBound((mz_ulong)planes.size());
    std::vector<uint8_t> body(bound);
    if (mz_compress2(body.data(), &bound, planes.data(), (mz_ulong)planes.size(), level) != MZ_OK)
        return out;

    out.reserve(HEADER + bound);
    out.insert(out.end(), MAGIC, MAGIC + 4);
    out.push_back(FORMAT);
    out.push_back(0);  // reserved
    putU64(out, (uint64_t)plain.size());
    out.insert(out.end(), body.begin(), body.begin() + bound);
    return out;
}

bool unpack(std::vector<uint8_t>& bytes) {
    if (!isPacked(bytes.data(), bytes.size())) return true;  // already plain

    const uint64_t raw = getU64(bytes.data() + 6);
    if (raw == 0 || raw > MAX_RAW) return false;

    std::vector<uint8_t> planes((size_t)raw);
    mz_ulong got = (mz_ulong)raw;
    if (mz_uncompress(planes.data(), &got, bytes.data() + HEADER,
                      (mz_ulong)(bytes.size() - HEADER)) != MZ_OK)
        return false;
    if (got != raw) return false;

    std::vector<uint8_t> plain((size_t)raw);
    interleave(planes.data(), (size_t)raw, plain.data());
    bytes.swap(plain);
    return true;
}

}  // namespace modelblob
