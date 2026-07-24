#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Minimal self-contained GIF89a writer (animated, looping).
//
// Vendored rather than shelling out to ffmpeg so timelapse export works on any
// machine that can run the game, with no external tools installed.
//
// Palette: GIF is limited to 256 colours per frame, so the encoder builds one
// global palette from a histogram of the frames it is given. Map renders are
// mostly flat country fills, which quantise very well this way.
//
// Usage:
//   GifEncoder g;
//   g.begin(path, w, h, delayCentiseconds);
//   for (frame : frames) g.addFrame(rgba);   // w*h*4 bytes, RGBA
//   g.end();
class GifEncoder {
public:
    ~GifEncoder() { end(); }

    // delayCs is the per-frame delay in centiseconds (100ths of a second).
    bool begin(const std::string& path, int width, int height, int delayCs);

    // rgba must be width*height*4 bytes. Alpha is ignored.
    // Frames are buffered so a single global palette can be built from all of
    // them; nothing is written until end().
    bool addFrame(const uint8_t* rgba);

    // Writes the file and closes it. Safe to call twice.
    bool end();

    bool ok() const { return m_ok; }
    int frameCount() const { return (int)m_frames.size(); }

private:
    struct Rgb { uint8_t r, g, b; };

    void buildPalette();
    uint8_t nearestIndex(uint8_t r, uint8_t g, uint8_t b);
    void writeFrameIndices(const std::vector<uint8_t>& indices, bool first);
    void lzwCompress(const std::vector<uint8_t>& indices);

    // Bit-level output for LZW, packed LSB-first into 255-byte sub-blocks.
    void bitsInit();
    void bitsWrite(int code, int codeLen);
    void bitsFlush();

    std::string m_path;
    FILE* m_fp = nullptr;
    int m_w = 0, m_h = 0, m_delayCs = 4;
    bool m_ok = false;
    bool m_done = false;

    std::vector<std::vector<uint8_t>> m_frames; // RGBA frames, buffered
    std::vector<Rgb> m_palette;                 // <=256 entries
    std::vector<int16_t> m_exactCache;          // RGB555 -> palette index, -1 unknown

    // LZW bit buffer
    std::vector<uint8_t> m_block;
    uint32_t m_bitAcc = 0;
    int m_bitCount = 0;
};
