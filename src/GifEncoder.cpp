#include "GifEncoder.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>

// ─── Setup ───────────────────────────────────────────────

bool GifEncoder::begin(const std::string& path, int width, int height, int delayCs) {
    m_path = path;
    m_w = width;
    m_h = height;
    m_delayCs = delayCs < 2 ? 2 : delayCs; // browsers clamp anything faster anyway
    m_frames.clear();
    m_done = false;
    m_ok = (width > 0 && height > 0);
    return m_ok;
}

bool GifEncoder::addFrame(const uint8_t* rgba) {
    if (!m_ok || m_done || !rgba) return false;
    m_frames.emplace_back(rgba, rgba + (size_t)m_w * m_h * 4);
    return true;
}

// ─── Palette ─────────────────────────────────────────────

// Histogram in RGB555 space, then keep the most common colours. Flat map fills
// collapse to very few distinct entries, so this beats a fixed colour cube.
void GifEncoder::buildPalette() {
    std::unordered_map<uint16_t, uint32_t> hist;
    hist.reserve(4096);
    for (auto& f : m_frames) {
        // Sampling every 3rd pixel keeps this cheap on big frames without
        // meaningfully changing which colours dominate.
        for (size_t i = 0; i + 3 < f.size(); i += 4 * 3) {
            uint16_t key = (uint16_t)(((f[i] >> 3) << 10) | ((f[i + 1] >> 3) << 5) | (f[i + 2] >> 3));
            hist[key]++;
        }
    }
    std::vector<std::pair<uint16_t, uint32_t>> sorted(hist.begin(), hist.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    m_palette.clear();
    for (size_t i = 0; i < sorted.size() && m_palette.size() < 256; ++i) {
        uint16_t k = sorted[i].first;
        // Expand 5-bit channels back to 8-bit, replicating high bits so white
        // stays white (0x1F -> 0xFF rather than 0xF8).
        uint8_t r = (uint8_t)(((k >> 10) & 0x1F) << 3); r |= r >> 5;
        uint8_t g = (uint8_t)(((k >> 5) & 0x1F) << 3);  g |= g >> 5;
        uint8_t b = (uint8_t)((k & 0x1F) << 3);         b |= b >> 5;
        m_palette.push_back({r, g, b});
    }
    if (m_palette.empty()) m_palette.push_back({0, 0, 0});

    m_exactCache.assign(32768, -1);
}

uint8_t GifEncoder::nearestIndex(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t key = (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
    int16_t cached = m_exactCache[key];
    if (cached >= 0) return (uint8_t)cached;

    int best = 0, bestDist = INT32_MAX;
    for (size_t i = 0; i < m_palette.size(); ++i) {
        int dr = (int)r - m_palette[i].r;
        int dg = (int)g - m_palette[i].g;
        int db = (int)b - m_palette[i].b;
        int d = dr * dr + dg * dg + db * db;
        if (d < bestDist) { bestDist = d; best = (int)i; if (!d) break; }
    }
    m_exactCache[key] = (int16_t)best;
    return (uint8_t)best;
}

// ─── LZW bit packing ─────────────────────────────────────

void GifEncoder::bitsInit() {
    m_block.clear();
    m_bitAcc = 0;
    m_bitCount = 0;
}

void GifEncoder::bitsWrite(int code, int codeLen) {
    m_bitAcc |= (uint32_t)code << m_bitCount;
    m_bitCount += codeLen;
    while (m_bitCount >= 8) {
        m_block.push_back((uint8_t)(m_bitAcc & 0xFF));
        m_bitAcc >>= 8;
        m_bitCount -= 8;
        if (m_block.size() == 255) {
            fputc(255, m_fp);
            fwrite(m_block.data(), 1, 255, m_fp);
            m_block.clear();
        }
    }
}

void GifEncoder::bitsFlush() {
    if (m_bitCount > 0) {
        m_block.push_back((uint8_t)(m_bitAcc & 0xFF));
        m_bitAcc = 0;
        m_bitCount = 0;
    }
    if (!m_block.empty()) {
        fputc((int)m_block.size(), m_fp);
        fwrite(m_block.data(), 1, m_block.size(), m_fp);
        m_block.clear();
    }
    fputc(0, m_fp); // block terminator
}

// Standard GIF variable-width LZW.
void GifEncoder::lzwCompress(const std::vector<uint8_t>& indices) {
    const int minCodeSize = 8;
    const int clearCode = 1 << minCodeSize; // 256
    const int endCode = clearCode + 1;      // 257

    fputc(minCodeSize, m_fp);
    bitsInit();

    // dict[prefix * 256 + nextByte] -> code
    std::vector<int32_t> dict((size_t)4096 * 256, -1);
    int next = endCode + 1;
    int codeSize = minCodeSize + 1;

    bitsWrite(clearCode, codeSize);

    if (indices.empty()) {
        bitsWrite(endCode, codeSize);
        bitsFlush();
        return;
    }

    int prefix = indices[0];
    for (size_t i = 1; i < indices.size(); ++i) {
        int k = indices[i];
        int32_t& slot = dict[(size_t)prefix * 256 + k];
        if (slot >= 0) {
            prefix = slot;
            continue;
        }
        bitsWrite(prefix, codeSize);
        slot = next++;
        if (next > (1 << codeSize)) {
            if (codeSize < 12) {
                codeSize++;
            } else {
                bitsWrite(clearCode, codeSize);
                std::fill(dict.begin(), dict.end(), -1);
                next = endCode + 1;
                codeSize = minCodeSize + 1;
            }
        }
        prefix = k;
    }
    bitsWrite(prefix, codeSize);
    bitsWrite(endCode, codeSize);
    bitsFlush();
}

// ─── Frame + file writing ────────────────────────────────

void GifEncoder::writeFrameIndices(const std::vector<uint8_t>& indices, bool /*first*/) {
    // Graphic control extension (per-frame delay)
    fputc(0x21, m_fp); fputc(0xF9, m_fp); fputc(0x04, m_fp);
    fputc(0x04, m_fp);                                   // disposal: do not dispose
    fputc(m_delayCs & 0xFF, m_fp); fputc((m_delayCs >> 8) & 0xFF, m_fp);
    fputc(0x00, m_fp);                                   // no transparent index
    fputc(0x00, m_fp);

    // Image descriptor (full frame, uses the global palette)
    fputc(0x2C, m_fp);
    fputc(0, m_fp); fputc(0, m_fp);                      // left
    fputc(0, m_fp); fputc(0, m_fp);                      // top
    fputc(m_w & 0xFF, m_fp); fputc((m_w >> 8) & 0xFF, m_fp);
    fputc(m_h & 0xFF, m_fp); fputc((m_h >> 8) & 0xFF, m_fp);
    fputc(0x00, m_fp);                                   // no local colour table

    lzwCompress(indices);
}

bool GifEncoder::end() {
    if (m_done) return m_ok;
    m_done = true;
    if (!m_ok || m_frames.empty()) { m_ok = false; return false; }

    buildPalette();

    m_fp = fopen(m_path.c_str(), "wb");
    if (!m_fp) { m_ok = false; return false; }

    // Header + logical screen descriptor
    fwrite("GIF89a", 1, 6, m_fp);
    fputc(m_w & 0xFF, m_fp); fputc((m_w >> 8) & 0xFF, m_fp);
    fputc(m_h & 0xFF, m_fp); fputc((m_h >> 8) & 0xFF, m_fp);
    // Global colour table present, 8 bits/pixel, table size 256 (2^(7+1))
    fputc(0xF7, m_fp);
    fputc(0x00, m_fp); // background colour index
    fputc(0x00, m_fp); // pixel aspect ratio

    // Global colour table, always padded to a full 256 entries
    for (int i = 0; i < 256; ++i) {
        Rgb c = (i < (int)m_palette.size()) ? m_palette[i] : Rgb{0, 0, 0};
        fputc(c.r, m_fp); fputc(c.g, m_fp); fputc(c.b, m_fp);
    }

    // Netscape extension: loop forever
    fputc(0x21, m_fp); fputc(0xFF, m_fp); fputc(0x0B, m_fp);
    fwrite("NETSCAPE2.0", 1, 11, m_fp);
    fputc(0x03, m_fp); fputc(0x01, m_fp);
    fputc(0x00, m_fp); fputc(0x00, m_fp);
    fputc(0x00, m_fp);

    std::vector<uint8_t> indices((size_t)m_w * m_h);
    for (size_t f = 0; f < m_frames.size(); ++f) {
        const auto& src = m_frames[f];
        for (size_t p = 0; p < indices.size(); ++p)
            indices[p] = nearestIndex(src[p * 4], src[p * 4 + 1], src[p * 4 + 2]);
        writeFrameIndices(indices, f == 0);
    }

    fputc(0x3B, m_fp); // trailer
    fclose(m_fp);
    m_fp = nullptr;
    m_frames.clear();
    m_frames.shrink_to_fit();
    return m_ok;
}
