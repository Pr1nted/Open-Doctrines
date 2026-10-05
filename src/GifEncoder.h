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
//   if (!g.begin(path, w, h, delayCentiseconds)) fail(g.error());
//   for (sample : someFrames) g.addPaletteSample(sample);
//   for (frame  : allFrames)  if (!g.writeFrame(frame)) break;
//   if (!g.end()) fail(g.error());
//
// Those return values matter. A GIF carries no length and no checksum, so a
// write that fails partway -- a full disk is the usual one -- leaves a file
// that opens, plays, and then dies on a half-written LZW code. Ignoring them
// is how --export-timelapse came to print "Saved 721 frames" and exit 0 about
// a truncated GIF.
class GifEncoder {
public:
    ~GifEncoder() { end(); }

    // delayCs is the per-frame delay in centiseconds (100ths of a second).
    bool begin(const std::string& path, int width, int height, int delayCs);

    // rgba = width*height*4 bytes; alpha ignored. Only feeds the histogram.
    void addPaletteSample(const uint8_t* rgba);

    // Palettises and writes one frame immediately.
    bool writeFrame(const uint8_t* rgba);

    // Writes the trailer and closes. Safe to call twice. False if ANY write
    // along the way failed, in which case the incomplete file is removed and
    // error() says why.
    bool end();

    bool ok() const { return m_ok; }
    int frameCount() const { return m_frameCount; }

    // Why the encode failed, empty while it is going well. Set by the first
    // failure only, so it names the cause rather than the last symptom.
    const std::string& error() const { return m_err; }

    // GIF stores width and height in 16 bits. Anything larger cannot be
    // described by the format at all.
    static constexpr int MAX_DIMENSION = 65535;

private:
    struct Rgb { uint8_t r, g, b; };

    void finalizePalette();          // build table + emit header
    uint8_t nearestIndex(uint8_t r, uint8_t g, uint8_t b);
    void lzwCompress(const std::vector<uint8_t>& indices);

    // Every byte leaves through one of these two, and both check the result.
    // A GIF is a stream with no length fields and no checksum, so a dropped
    // tail -- a full disk, an unmounted share -- is not detectable from the
    // file: it decodes as far as it got and then dies on a partial LZW code.
    // Unchecked fputc/fwrite meant the encoder reported success about exactly
    // that file, and --export-timelapse exited 0 on a truncated GIF.
    bool put(int byte);
    bool put(const void* data, size_t n);
    void fail(const std::string& why);

    void bitsInit();
    void bitsWrite(int code, int codeLen);
    void bitsFlush();

    std::string m_path;
    FILE* m_fp = nullptr;
    int m_w = 0, m_h = 0, m_delayCs = 4;
    bool m_ok = false;
    bool m_done = false;
    std::string m_err;
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
