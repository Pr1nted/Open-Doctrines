#include "GifEncoder.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>

// ─── Setup ───────────────────────────────────────────────

bool GifEncoder::begin(const std::string& path, int width, int height, int delayCs) {
    m_path = path;
    m_w = width;
    m_h = height;
    m_delayCs = delayCs < 2 ? 2 : delayCs; // viewers clamp anything faster anyway
    m_done = false;
    m_headerWritten = false;
    m_frameCount = 0;
    m_hist.clear();
    m_palette.clear();
    m_err.clear();
    m_ok = true;

    if (width <= 0 || height <= 0) {
        fail("size must be positive");
        return false;
    }
    // The logical screen descriptor and every image descriptor store width and
    // height as two bytes each. Above 65535 the low 16 bits were written and
    // the rest thrown away, so 70000 became 4464: the encoder then wrote
    // 70000-pixel rows into a frame it had declared 4464 wide, which overruns
    // the frame on the first row and decodes as a corrupt LZW stream -- and
    // end() still returned true. Refuse the size instead of writing a file
    // that cannot be right.
    if (width > MAX_DIMENSION || height > MAX_DIMENSION) {
        fail("GIF cannot be larger than " + std::to_string(MAX_DIMENSION) +
             " pixels on a side (asked for " + std::to_string(width) + "x" +
             std::to_string(height) + ")");
        return false;
    }

    // Opened here rather than on the first frame, so an unwritable path is
    // reported before several minutes of rendering. The caller's "could not
    // create GIF at that path" message was unreachable while this was lazy:
    // begin() said yes to any path at all.
    m_fp = fopen(m_path.c_str(), "wb");
    if (!m_fp) {
        fail("could not open " + m_path + ": " + strerror(errno));
        return false;
    }
    return m_ok;
}

// ─── Checked output ──────────────────────────────────────

void GifEncoder::fail(const std::string& why) {
    if (m_err.empty()) m_err = why;   // first cause, not last symptom
    m_ok = false;
}

bool GifEncoder::put(int byte) {
    if (!m_ok || !m_fp) return false;
    if (fputc(byte, m_fp) == EOF) {
        fail("write to " + m_path + " failed: " + strerror(errno));
        return false;
    }
    return true;
}

bool GifEncoder::put(const void* data, size_t n) {
    if (!m_ok || !m_fp) return false;
    if (n && fwrite(data, 1, n, m_fp) != n) {
        fail("write to " + m_path + " failed: " + strerror(errno));
        return false;
    }
    return true;
}

void GifEncoder::addPaletteSample(const uint8_t* rgba) {
    if (!m_ok || m_headerWritten || !rgba) return;
    // Sampling every 3rd pixel keeps this cheap on large frames without
    // meaningfully changing which colours dominate.
    size_t n = (size_t)m_w * m_h * 4;
    for (size_t i = 0; i + 3 < n; i += 4 * 3) {
        uint16_t key = (uint16_t)(((rgba[i] >> 3) << 10) | ((rgba[i + 1] >> 3) << 5) | (rgba[i + 2] >> 3));
        m_hist[key]++;
    }
}

// ─── Palette ─────────────────────────────────────────────

void GifEncoder::finalizePalette() {
    // Median cut over the sampled histogram.
    //
    // This replaced "the 256 most frequent colours", which is only a quantiser
    // when the histogram has peaks. A smooth gradient does not: every colour
    // appears once or twice, so the top 256 were an arbitrary 256 and everything
    // else mapped to whichever of them happened to be nearest -- up to 206 out
    // of 255 wrong on one channel, measured. That matters here specifically
    // because the thing being exported is political.png, which has the border
    // gradient baked into every country.
    //
    // Median cut instead splits the colour space where the colours actually are:
    // repeatedly take the box with the largest spread, sort its colours on that
    // axis, and cut at the weighted median until there are 256 boxes.
    struct Box {
        std::vector<size_t> items;      // indices into `cols`
        uint8_t lo[3], hi[3];
        uint64_t weight = 0;
        int longest = 0;                // axis with the widest spread
        int spread = 0;
    };

    struct Col { uint8_t c[3]; uint32_t count; };
    std::vector<Col> cols;
    cols.reserve(m_hist.size());
    for (const auto& [key, count] : m_hist) {
        uint8_t r = (uint8_t)(((key >> 10) & 0x1F) << 3); r |= r >> 5;
        uint8_t g = (uint8_t)(((key >> 5) & 0x1F) << 3);  g |= g >> 5;
        uint8_t b = (uint8_t)((key & 0x1F) << 3);         b |= b >> 5;
        cols.push_back({{r, g, b}, count});
    }

    auto measure = [&cols](Box& box) {
        for (int a = 0; a < 3; ++a) { box.lo[a] = 255; box.hi[a] = 0; }
        box.weight = 0;
        for (size_t i : box.items) {
            for (int a = 0; a < 3; ++a) {
                box.lo[a] = std::min(box.lo[a], cols[i].c[a]);
                box.hi[a] = std::max(box.hi[a], cols[i].c[a]);
            }
            box.weight += cols[i].count;
        }
        box.longest = 0; box.spread = 0;
        for (int a = 0; a < 3; ++a) {
            int d = box.hi[a] - box.lo[a];
            if (d > box.spread) { box.spread = d; box.longest = a; }
        }
    };

    m_palette.clear();
    if (!cols.empty()) {
        std::vector<Box> boxes(1);
        boxes[0].items.resize(cols.size());
        for (size_t i = 0; i < cols.size(); ++i) boxes[0].items[i] = i;
        measure(boxes[0]);

        while (boxes.size() < 256) {
            // Split the box that is worst to represent with one colour: widest
            // spread, and among equals the one covering the most pixels.
            int pick = -1;
            for (size_t i = 0; i < boxes.size(); ++i) {
                if (boxes[i].items.size() < 2 || boxes[i].spread == 0) continue;
                if (pick < 0 || boxes[i].spread > boxes[pick].spread ||
                    (boxes[i].spread == boxes[pick].spread &&
                     boxes[i].weight > boxes[pick].weight)) {
                    pick = (int)i;
                }
            }
            if (pick < 0) break;        // every box is a single colour

            Box& src = boxes[pick];
            const int axis = src.longest;
            std::sort(src.items.begin(), src.items.end(),
                      [&cols, axis](size_t a, size_t b) {
                          if (cols[a].c[axis] != cols[b].c[axis])
                              return cols[a].c[axis] < cols[b].c[axis];
                          return a < b;      // stable, so output is reproducible
                      });
            // Cut at the weighted median so both halves carry similar pixel
            // counts -- but the cut MUST leave both sides non-empty.
            //
            // The scan below stops before the last item, so it never counts that
            // item's weight. When a single colour holds more than half the box
            // and happens to sort last, `run` therefore never reaches `half`,
            // `cut` runs off to size-1, and the high half comes out empty. The
            // box then never actually splits: the loop appends an empty box,
            // picks the same box again, and repeats until it has 256 boxes of
            // which only a handful hold anything.
            //
            // That is not a corner case here. A political map is ~70% sea in one
            // flat colour, so it happened on the first frame of every timelapse
            // and the palette came out with five entries instead of a hundred.
            uint64_t half = src.weight / 2, run = 0;
            size_t cut = 0;
            bool reached = false;
            for (; cut + 1 < src.items.size(); ++cut) {
                run += cols[src.items[cut]].count;
                if (run >= half) { reached = true; break; }
            }
            if (!reached) {
                // One dominant colour at the far end. Peel it off on its own,
                // which is what a median cut should do with it anyway.
                cut = src.items.size() - 2;
            }
            Box hiBox;
            hiBox.items.assign(src.items.begin() + (long)cut + 1, src.items.end());
            src.items.resize(cut + 1);
            measure(src);
            measure(hiBox);
            boxes.push_back(std::move(hiBox));
        }

        for (const Box& box : boxes) {
            if (box.items.empty()) continue;
            // Pixel-weighted mean, so a box holding one dominant colour and a
            // few strays lands on the dominant one.
            uint64_t acc[3] = {0, 0, 0}, w = 0;
            for (size_t i : box.items) {
                for (int a = 0; a < 3; ++a)
                    acc[a] += (uint64_t)cols[i].c[a] * cols[i].count;
                w += cols[i].count;
            }
            if (!w) continue;
            m_palette.push_back({(uint8_t)(acc[0] / w), (uint8_t)(acc[1] / w),
                                 (uint8_t)(acc[2] / w)});
        }
    }
    if (m_palette.empty()) m_palette.push_back({0, 0, 0});

    m_exactCache.assign(32768, -1);
    m_hist.clear();

    // Header + logical screen descriptor
    put("GIF89a", 6);
    put(m_w & 0xFF); put((m_w >> 8) & 0xFF);
    put(m_h & 0xFF); put((m_h >> 8) & 0xFF);
    put(0xF7); // global table present, 8 bits/pixel, 256 entries
    put(0x00); // background colour index
    put(0x00); // pixel aspect ratio

    for (int i = 0; i < 256; ++i) {
        Rgb c = (i < (int)m_palette.size()) ? m_palette[i] : Rgb{0, 0, 0};
        put(c.r); put(c.g); put(c.b);
    }

    // Netscape extension: loop forever
    put(0x21); put(0xFF); put(0x0B);
    put("NETSCAPE2.0", 11);
    put(0x03); put(0x01);
    put(0x00); put(0x00);
    put(0x00);

    m_headerWritten = m_ok;
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

void GifEncoder::bitsInit() { m_block.clear(); m_bitAcc = 0; m_bitCount = 0; }

void GifEncoder::bitsWrite(int code, int codeLen) {
    m_bitAcc |= (uint32_t)code << m_bitCount;
    m_bitCount += codeLen;
    while (m_bitCount >= 8) {
        m_block.push_back((uint8_t)(m_bitAcc & 0xFF));
        m_bitAcc >>= 8;
        m_bitCount -= 8;
        if (m_block.size() == 255) {
            put(255);
            put(m_block.data(), 255);
            m_block.clear();
        }
    }
}

void GifEncoder::bitsFlush() {
    if (m_bitCount > 0) {
        m_block.push_back((uint8_t)(m_bitAcc & 0xFF));
        m_bitAcc = 0; m_bitCount = 0;
    }
    if (!m_block.empty()) {
        put((int)m_block.size());
        put(m_block.data(), m_block.size());
        m_block.clear();
    }
    put(0); // block terminator
}

// Standard GIF variable-width LZW.
void GifEncoder::lzwCompress(const std::vector<uint8_t>& indices) {
    const int minCodeSize = 8;
    const int clearCode = 1 << minCodeSize; // 256
    const int endCode = clearCode + 1;      // 257

    put(minCodeSize);
    bitsInit();

    static std::vector<int32_t> dict; // dict[prefix*256 + byte] -> code
    dict.assign((size_t)4096 * 256, -1);
    int next = endCode + 1;
    int codeSize = minCodeSize + 1;

    bitsWrite(clearCode, codeSize);
    if (indices.empty()) { bitsWrite(endCode, codeSize); bitsFlush(); return; }

    int prefix = indices[0];
    for (size_t i = 1; i < indices.size(); ++i) {
        int k = indices[i];
        int32_t& slot = dict[(size_t)prefix * 256 + k];
        if (slot >= 0) { prefix = slot; continue; }
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

// ─── Frame writing ───────────────────────────────────────

bool GifEncoder::writeFrame(const uint8_t* rgba) {
    if (!m_ok || m_done || !m_fp || !rgba) return false;

    if (!m_headerWritten) {
        // No explicit samples given — derive the palette from this frame.
        if (m_hist.empty()) addPaletteSample(rgba);
        finalizePalette();
        if (!m_ok) return false;
    }

    m_indices.resize((size_t)m_w * m_h);
    for (size_t p = 0; p < m_indices.size(); ++p)
        m_indices[p] = nearestIndex(rgba[p * 4], rgba[p * 4 + 1], rgba[p * 4 + 2]);

    // Graphic control extension (per-frame delay, no disposal)
    put(0x21); put(0xF9); put(0x04);
    put(0x04);
    put(m_delayCs & 0xFF); put((m_delayCs >> 8) & 0xFF);
    put(0x00); put(0x00);

    // Image descriptor (full frame, global palette)
    put(0x2C);
    put(0); put(0);
    put(0); put(0);
    put(m_w & 0xFF); put((m_w >> 8) & 0xFF);
    put(m_h & 0xFF); put((m_h >> 8) & 0xFF);
    put(0x00);

    lzwCompress(m_indices);
    if (!m_ok) return false;
    m_frameCount++;
    return true;
}

bool GifEncoder::end() {
    if (m_done) return m_ok;
    m_done = true;
    if (!m_fp) {
        if (m_err.empty()) fail("nothing was written");
        return false;
    }
    if (!m_headerWritten) fail("no frames were written");

    put(0x3B); // trailer

    // fclose does the last flush, and that is where a full disk usually
    // surfaces: the frames before it fitted in the stdio buffer. Checking only
    // the fputc calls would have missed it.
    if (m_ok && fflush(m_fp) != 0)
        fail("flushing " + m_path + " failed: " + strerror(errno));
    if (m_ok && ferror(m_fp))
        fail("write to " + m_path + " failed");
    const bool closed = (fclose(m_fp) == 0);
    m_fp = nullptr;
    if (m_ok && !closed)
        fail("closing " + m_path + " failed: " + strerror(errno));

    // A half-written GIF is indistinguishable from a whole one until something
    // tries to decode it, so leaving it behind invites the next script to pick
    // it up. Remove it and say so; on a full disk this also gives the space
    // back.
    if (!m_ok) {
        remove(m_path.c_str());
        if (m_headerWritten) m_err += " (incomplete file removed)";
    }
    return m_ok;
}
