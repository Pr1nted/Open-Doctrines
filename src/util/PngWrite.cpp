#include "PngWrite.h"

#include <algorithm>
#include <cstring>

#include "miniz.h"

namespace pngw {
namespace {

constexpr int MAX_PALETTE = 256;

inline uint32_t rgbaKey(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((uint8_t)(v >> 24));
    out.push_back((uint8_t)(v >> 16));
    out.push_back((uint8_t)(v >> 8));
    out.push_back((uint8_t)v);
}

void putChunk(std::vector<uint8_t>& out, const char tag[4], const uint8_t* data, size_t len) {
    putU32(out, (uint32_t)len);
    size_t crcStart = out.size();
    out.insert(out.end(), tag, tag + 4);
    if (len) out.insert(out.end(), data, data + len);
    putU32(out, (uint32_t)mz_crc32(MZ_CRC32_INIT, out.data() + crcStart, 4 + len));
}

/**
 * Distinct RGBA values, sorted, or empty past MAX_PALETTE.
 *
 * Open addressing over a fixed 1024-slot table rather than a std::unordered_map:
 * this runs over 33 million pixels per layer, and the map's per-insert
 * allocation dominated everything else when it was written that way.
 */
bool collectPalette(const uint8_t* rgba, size_t pixels, std::vector<uint32_t>& palette) {
    constexpr size_t SLOTS = 1024;      // >= 4x MAX_PALETTE, so probes stay short
    constexpr size_t MASK = SLOTS - 1;
    std::vector<uint32_t> slotKey(SLOTS, 0);
    std::vector<uint8_t> slotUsed(SLOTS, 0);
    palette.clear();

    uint32_t lastKey = 0;
    bool haveLast = false;
    for (size_t i = 0; i < pixels; ++i) {
        uint32_t k = rgbaKey(rgba + i * 4);
        // Map layers are broad flat regions, so the previous pixel is almost
        // always the answer -- this short-circuits the hash for most of them.
        if (haveLast && k == lastKey) continue;
        lastKey = k;
        haveLast = true;

        size_t s = (k * 2654435761u) & MASK;
        while (slotUsed[s] && slotKey[s] != k) s = (s + 1) & MASK;
        if (slotUsed[s]) continue;
        if (palette.size() >= MAX_PALETTE) { palette.clear(); return false; }
        slotUsed[s] = 1;
        slotKey[s] = k;
        palette.push_back(k);
    }
    std::sort(palette.begin(), palette.end());
    return true;
}

}  // namespace

std::vector<uint8_t> encodeIndexedRGBA(const uint8_t* rgba, int w, int h) {
    std::vector<uint8_t> out;
    if (!rgba || w <= 0 || h <= 0) return out;
    const size_t pixels = (size_t)w * (size_t)h;

    std::vector<uint32_t> palette;
    if (!collectPalette(rgba, pixels, palette) || palette.empty()) return out;

    const size_t n = palette.size();
    const int bitDepth = n <= 2 ? 1 : n <= 4 ? 2 : n <= 16 ? 4 : 8;
    const int perByte = 8 / bitDepth;

    // key -> palette index, same table shape as the collection pass
    constexpr size_t SLOTS = 1024;
    constexpr size_t MASK = SLOTS - 1;
    std::vector<uint32_t> slotKey(SLOTS, 0);
    std::vector<uint8_t> slotIdx(SLOTS, 0);
    std::vector<uint8_t> slotUsed(SLOTS, 0);
    for (size_t i = 0; i < n; ++i) {
        size_t s = (palette[i] * 2654435761u) & MASK;
        while (slotUsed[s]) s = (s + 1) & MASK;
        slotUsed[s] = 1;
        slotKey[s] = palette[i];
        slotIdx[s] = (uint8_t)i;
    }

    // Filter 0 (None) on every row. The adaptive filters exist to make
    // neighbouring BYTES similar, which is what helps a photograph; on packed
    // indices they scramble a run of identical pixels into a run of deltas
    // that deflate handles no better and often worse.
    const size_t rowBytes = ((size_t)w + perByte - 1) / perByte;
    std::vector<uint8_t> raw((rowBytes + 1) * (size_t)h, 0);
    uint32_t lastKey = ~palette[0];  // anything the first pixel cannot match
    uint8_t lastIdx = 0;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = raw.data() + (size_t)y * (rowBytes + 1) + 1;  // +1 skips the filter byte
        const uint8_t* src = rgba + (size_t)y * (size_t)w * 4;
        for (int x = 0; x < w; ++x) {
            uint32_t k = rgbaKey(src + (size_t)x * 4);
            if (k != lastKey) {
                size_t s = (k * 2654435761u) & MASK;
                while (slotKey[s] != k) s = (s + 1) & MASK;
                lastKey = k;
                lastIdx = slotIdx[s];
            }
            const uint8_t idx = lastIdx;
            if (bitDepth == 8) {
                row[x] = idx;
            } else {
                // MSB first within each byte, as the PNG spec packs indices
                const int shift = 8 - bitDepth * (x % perByte + 1);
                row[x / perByte] |= (uint8_t)(idx << shift);
            }
        }
    }

    mz_ulong bound = mz_compressBound((mz_ulong)raw.size());
    std::vector<uint8_t> idat(bound);
    if (mz_compress2(idat.data(), &bound, raw.data(), (mz_ulong)raw.size(),
                     MZ_BEST_COMPRESSION) != MZ_OK)
        return {};
    idat.resize(bound);

    out.reserve(idat.size() + 1024);
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    out.insert(out.end(), SIG, SIG + 8);

    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);  ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);  ihdr[7] = (uint8_t)h;
    ihdr[8] = (uint8_t)bitDepth;
    ihdr[9] = 3;                       // colour type 3 = indexed
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;  // deflate / no filter set / no interlace
    putChunk(out, "IHDR", ihdr, sizeof(ihdr));

    std::vector<uint8_t> plte(n * 3);
    for (size_t i = 0; i < n; ++i) {
        plte[i * 3 + 0] = (uint8_t)(palette[i] >> 24);
        plte[i * 3 + 1] = (uint8_t)(palette[i] >> 16);
        plte[i * 3 + 2] = (uint8_t)(palette[i] >> 8);
    }
    putChunk(out, "PLTE", plte.data(), plte.size());

    // tRNS only when some colour is not fully opaque, and only as far as the
    // last such entry -- the spec lets the rest default to opaque.
    std::vector<uint8_t> trns(n);
    size_t lastAlpha = 0;
    bool anyAlpha = false;
    for (size_t i = 0; i < n; ++i) {
        trns[i] = (uint8_t)(palette[i] & 0xff);
        if (trns[i] != 255) { lastAlpha = i + 1; anyAlpha = true; }
    }
    if (anyAlpha) putChunk(out, "tRNS", trns.data(), lastAlpha);

    putChunk(out, "IDAT", idat.data(), idat.size());
    putChunk(out, "IEND", nullptr, 0);
    return out;
}

}  // namespace pngw
