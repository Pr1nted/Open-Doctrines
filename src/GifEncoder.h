#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

// Minimal self-contained GIF89a writer (animated, looping).
//
// Vendored rather than shelling out to ffmpeg so timelapse export works on any
// machine that can run the game, with no external tools installed.
//
// Streaming: frames are palettised and written out as they arrive, so a long
// export at 1920x960 doesn't have to hold every frame in memory. GIF needs its
// colour table up front, so callers feed a few representative frames to
// addPaletteSample() first; if none are given, the first written frame is used.
//
//   GifEncoder g;
//   g.begin(path, w, h, delayCentiseconds);
//   for (sample : someFrames) g.addPaletteSample(sample);
//   for (frame  : allFrames)  g.writeFrame(frame);
//   g.end();
class GifEncoder {
public:
    ~GifEncoder() { end(); }

    // delayCs is the per-frame delay in centiseconds (100ths of a second).
    bool begin(const std::string& path, int width, int height, int delayCs);

    // rgba = width*height*4 bytes; alpha ignored. Only feeds the histogram.
    void addPaletteSample(const uint8_t* rgba);

    // Palettises and writes one frame immediately.
    bool writeFrame(const uint8_t* rgba);

    // Writes the trailer and closes. Safe to call twice.
    bool end();

    bool ok() const { return m_ok; }
    int frameCount() const { return m_frameCount; }

private:
    struct Rgb { uint8_t r, g, b; };

    void finalizePalette();          // build table + emit header
    uint8_t nearestIndex(uint8_t r, uint8_t g, uint8_t b);
    void lzwCompress(const std::vector<uint8_t>& indices);

    void bitsInit();
    void bitsWrite(int code, int codeLen);
    void bitsFlush();

    std::string m_path;
    FILE* m_fp = nullptr;
    int m_w = 0, m_h = 0, m_delayCs = 4;
    bool m_ok = false;
    bool m_done = false;
    bool m_headerWritten = false;
    int m_frameCount = 0;

    std::unordered_map<uint16_t, uint32_t> m_hist; // RGB555 -> count
    std::vector<Rgb> m_palette;
    std::vector<int16_t> m_exactCache;
    std::vector<uint8_t> m_indices;

    std::vector<uint8_t> m_block;
    uint32_t m_bitAcc = 0;
    int m_bitCount = 0;
};
