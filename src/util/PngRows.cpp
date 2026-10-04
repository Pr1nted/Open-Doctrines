#include "PngRows.h"

#include "miniz.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

bool fail(std::string* why, const char* text) {
    if (why) *why = text;
    return false;
}

}  // namespace

bool pngForEachRow(const uint8_t* data, size_t size,
                   const std::function<bool(int, int)>& onSize,
                   const std::function<void(int, const uint8_t*)>& row,
                   std::string* why) {
    static const uint8_t kSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!data || size < 8 + 25 || std::memcmp(data, kSig, 8) != 0)
        return fail(why, "not a PNG");

    // ── the chunks: header, palette, transparency, and the compressed data ──
    uint32_t width = 0, height = 0;
    int depth = 0, colorType = -1, interlace = 0;
    std::vector<uint8_t> palette;      // RGBA, 256 entries once read
    std::vector<uint8_t> idat;
    bool haveTrnsKey = false;
    uint16_t trnsKey[3] = {0, 0, 0};
    for (size_t at = 8; at + 12 <= size;) {
        const uint32_t len = be32(data + at);
        if (len > size - at - 12) return fail(why, "a chunk runs past the end");
        const uint8_t* type = data + at + 4;
        const uint8_t* body = data + at + 8;
        if (!std::memcmp(type, "IHDR", 4)) {
            if (len < 13) return fail(why, "short IHDR");
            width = be32(body);
            height = be32(body + 4);
            depth = body[8];
            colorType = body[9];
            interlace = body[12];
        } else if (!std::memcmp(type, "PLTE", 4)) {
            palette.assign(256 * 4, 0);
            for (uint32_t i = 0; i < 256; ++i) palette[i * 4 + 3] = 255;
            for (uint32_t i = 0; i * 3 + 2 < len && i < 256; ++i) {
                palette[i * 4 + 0] = body[i * 3 + 0];
                palette[i * 4 + 1] = body[i * 3 + 1];
                palette[i * 4 + 2] = body[i * 3 + 2];
            }
        } else if (!std::memcmp(type, "tRNS", 4)) {
            if (colorType == 3 && !palette.empty()) {
                for (uint32_t i = 0; i < len && i < 256; ++i) palette[i * 4 + 3] = body[i];
            } else if (colorType == 0 && len >= 2) {
                haveTrnsKey = true;
                trnsKey[0] = (uint16_t)((body[0] << 8) | body[1]);
            } else if (colorType == 2 && len >= 6) {
                haveTrnsKey = true;
                for (int c = 0; c < 3; ++c)
                    trnsKey[c] = (uint16_t)((body[c * 2] << 8) | body[c * 2 + 1]);
            }
        } else if (!std::memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), body, body + len);
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        at += 12 + (size_t)len;
    }

    if (width == 0 || height == 0 || width > 65535 || height > 65535)
        return fail(why, "no usable size");
    if (interlace != 0) return fail(why, "interlaced PNG");
    int channels = 0;
    switch (colorType) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return fail(why, "unknown colour type");
    }
    const bool depthOk = depth == 8 || depth == 16 ||
                         ((colorType == 0 || colorType == 3) && (depth == 1 || depth == 2 || depth == 4));
    if (!depthOk) return fail(why, "unsupported bit depth");
    if (colorType == 3 && palette.empty()) return fail(why, "palette image with no palette");
    if (idat.empty()) return fail(why, "no image data");
    if (onSize && !onSize((int)width, (int)height)) return true;

    const size_t bitsPerPixel = (size_t)channels * depth;
    const size_t stride = (width * bitsPerPixel + 7) / 8;
    const size_t bpp = std::max<size_t>(1, bitsPerPixel / 8);   // filter unit

    // ── inflate, one scanline at a time ──
    //
    // tinfl writes into a circular window it also reads back-references from,
    // so the window must be the full 32 KB dictionary. Bytes leave it into the
    // scanline being assembled as they are produced.
    tinfl_decompressor inflator;
    tinfl_init(&inflator);
    std::vector<uint8_t> window(TINFL_LZ_DICT_SIZE);
    size_t windowAt = 0;
    std::vector<uint8_t> cur(stride + 1), prev(stride + 1, 0);
    size_t filled = 0;
    std::vector<uint8_t> rgba((size_t)width * 4);
    uint32_t y = 0;
    size_t inAt = 0;
    tinfl_status status = TINFL_STATUS_NEEDS_MORE_INPUT;

    auto emitRow = [&]() {
        // Undo the filter; byte 0 of `cur` is the filter type, as in `prev`.
        uint8_t* c = cur.data() + 1;
        const uint8_t* p = prev.data() + 1;
        const uint8_t f = cur[0];
        for (size_t i = 0; i < stride; ++i) {
            const int a = i >= bpp ? c[i - bpp] : 0;
            const int b = p[i];
            const int d = i >= bpp ? p[i - bpp] : 0;
            switch (f) {
                case 0: break;
                case 1: c[i] = (uint8_t)(c[i] + a); break;
                case 2: c[i] = (uint8_t)(c[i] + b); break;
                case 3: c[i] = (uint8_t)(c[i] + ((a + b) >> 1)); break;
                case 4: c[i] = (uint8_t)(c[i] + paeth(a, b, d)); break;
                default: return false;
            }
        }
        // To RGBA8, the way stb_image converts with four channels requested.
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* o = &rgba[(size_t)x * 4];
            if (depth < 8) {
                const size_t bit = (size_t)x * depth;
                const int shift = 8 - depth - (int)(bit & 7);
                const int mask = (1 << depth) - 1;
                const int v = (c[bit >> 3] >> shift) & mask;
                if (colorType == 3) {
                    std::memcpy(o, &palette[(size_t)v * 4], 4);
                } else {
                    // Grey below 8 bits is scaled to fill the byte.
                    static const int scale[9] = {0, 0xFF, 0x55, 0, 0x11, 0, 0, 0, 0x01};
                    const uint8_t g = (uint8_t)(v * scale[depth]);
                    o[0] = o[1] = o[2] = g;
                    o[3] = (haveTrnsKey && v == trnsKey[0]) ? 0 : 255;
                }
                continue;
            }
            const size_t sampleBytes = depth / 8;
            auto sample = [&](int ch) -> uint16_t {
                const uint8_t* s = c + ((size_t)x * channels + ch) * sampleBytes;
                return sampleBytes == 2 ? (uint16_t)((s[0] << 8) | s[1]) : s[0];
            };
            auto to8 = [&](uint16_t v) -> uint8_t {
                return sampleBytes == 2 ? (uint8_t)(v >> 8) : (uint8_t)v;
            };
            switch (colorType) {
                case 0: {
                    const uint16_t g = sample(0);
                    o[0] = o[1] = o[2] = to8(g);
                    o[3] = (haveTrnsKey && g == trnsKey[0]) ? 0 : 255;
                    break;
                }
                case 2: {
                    const uint16_t r = sample(0), g = sample(1), b = sample(2);
                    o[0] = to8(r); o[1] = to8(g); o[2] = to8(b);
                    o[3] = (haveTrnsKey && r == trnsKey[0] && g == trnsKey[1] && b == trnsKey[2])
                               ? 0 : 255;
                    break;
                }
                case 3:
                    std::memcpy(o, &palette[(size_t)c[x] * 4], 4);
                    break;
                case 4:
                    o[0] = o[1] = o[2] = to8(sample(0));
                    o[3] = to8(sample(1));
                    break;
                case 6:
                    o[0] = to8(sample(0)); o[1] = to8(sample(1));
                    o[2] = to8(sample(2)); o[3] = to8(sample(3));
                    break;
            }
        }
        row((int)y, rgba.data());
        cur.swap(prev);
        ++y;
        filled = 0;
        return true;
    };

    while (y < height) {
        size_t inLeft = idat.size() - inAt;
        size_t outLeft = window.size() - windowAt;
        const mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER |
                                (inAt + inLeft < idat.size() ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        status = tinfl_decompress(&inflator, idat.data() + inAt, &inLeft,
                                  window.data(), window.data() + windowAt, &outLeft, flags);
        inAt += inLeft;
        // Move what came out into scanlines.
        for (size_t i = 0; i < outLeft && y < height; ++i) {
            cur[filled++] = window[windowAt + i];
            if (filled == stride + 1 && !emitRow()) return fail(why, "bad filter type");
        }
        windowAt = (windowAt + outLeft) & (window.size() - 1);
        if (status == TINFL_STATUS_DONE) break;
        if (status < TINFL_STATUS_DONE) return fail(why, "corrupt image data");
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT && inAt >= idat.size())
            return fail(why, "image data ends early");
    }
    if (y < height) return fail(why, "image data ends early");
    return true;
}
