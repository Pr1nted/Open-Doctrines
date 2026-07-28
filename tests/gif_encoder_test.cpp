// GifEncoder round-trip test.
//
// Writes GIFs with known content so tests/gif_encoder_check.py can decode them
// with an independent decoder (Pillow) and compare. Two implementations
// disagreeing is the only reliable way to catch a bug in a bitstream writer:
// the encoder cannot check its own LZW, and a corrupt GIF often still opens.
//
//   GifEncoderTest <out-dir>
//
// writes <out-dir>/{gradient,motion,noise,single,wide}.gif and a manifest
// describing what each should contain.

#include "../src/GifEncoder.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  %-56s %s\n", what.c_str(), ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

std::vector<uint8_t> frameGradient(int w, int h, int t) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t i = ((size_t)y * w + x) * 4;
            px[i + 0] = (uint8_t)((x * 255) / (w - 1));
            px[i + 1] = (uint8_t)((y * 255) / (h - 1));
            px[i + 2] = (uint8_t)(t * 40);
            px[i + 3] = 255;
        }
    return px;
}

// A block that moves. Two flat colours, so the palette is tiny and any
// mis-sized LZW code shows up as a displaced or torn block.
std::vector<uint8_t> frameMotion(int w, int h, int t) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t i = ((size_t)y * w + x) * 4;
            bool in = x >= t * 8 && x < t * 8 + 16 && y >= 4 && y < 20;
            px[i + 0] = in ? 220 : 20;
            px[i + 1] = in ? 30 : 40;
            px[i + 2] = in ? 30 : 90;
            px[i + 3] = 255;
        }
    return px;
}

// Deterministic pseudo-noise. This is the case that pushes the LZW dictionary
// past 512 entries and so past the first code-size increase, which is exactly
// where an off-by-one in the increment condition corrupts the stream.
std::vector<uint8_t> frameNoise(int w, int h, int seed) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    uint32_t s = 0x9E3779B9u ^ (uint32_t)seed;
    for (size_t p = 0; p < (size_t)w * h; ++p) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        px[p * 4 + 0] = (uint8_t)(s >> 24);
        px[p * 4 + 1] = (uint8_t)(s >> 16);
        px[p * 4 + 2] = (uint8_t)(s >> 8);
        px[p * 4 + 3] = 255;
    }
    return px;
}

bool writeGif(const std::string& path, int w, int h, int frames, int delay,
              std::vector<uint8_t> (*gen)(int, int, int)) {
    GifEncoder g;
    if (!g.begin(path, w, h, delay)) return false;
    // Sample EVERY frame. Sampling only the endpoints leaves the frames between
    // them with colours that have no palette entry at all, and the resulting
    // error is the test's fault rather than the encoder's -- which is how this
    // was first misread as corruption. exportHistoryGif() samples keyframes and
    // mid-transition frames for the same reason.
    for (int t = 0; t < frames; ++t) {
        auto f = gen(w, h, t);
        g.addPaletteSample(f.data());
    }
    for (int t = 0; t < frames; ++t) {
        auto f = gen(w, h, t);
        if (!g.writeFrame(f.data())) return false;
    }
    if (!g.end()) return false;
    return g.frameCount() == frames;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: GifEncoderTest <out-dir>\n"); return 2; }
    std::string dir = argv[1];

    printf("=== GifEncoder ===\n");

    check(writeGif(dir + "/gradient.gif", 64, 32, 6, 8, frameGradient),
          "gradient, 64x32, 6 frames");
    check(writeGif(dir + "/motion.gif", 96, 24, 8, 5, frameMotion),
          "moving block, 96x24, 8 frames");
    check(writeGif(dir + "/noise.gif", 80, 40, 3, 10, frameNoise),
          "pseudo-noise, 80x40, 3 frames (exercises code-size growth)");
    check(writeGif(dir + "/single.gif", 40, 40, 1, 100, frameGradient),
          "single frame");
    check(writeGif(dir + "/wide.gif", 300, 8, 4, 4, frameMotion),
          "wide and short, 300x8");

    // Many FLAT colours, one dominating.
    //
    // This is what a political map is: a large sea area plus a hundred-odd flat
    // country colours. It is the case the timelapse export actually feeds the
    // encoder, and the case a frequency-based palette and a careless median cut
    // both get wrong -- the first because the histogram has no peaks worth
    // keeping, the second if it stops splitting too early. A gradient does not
    // catch either.
    {
        const int w = 160, h = 120, bands = 120;
        std::vector<uint8_t> px((size_t)w * h * 4, 255);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                size_t i = ((size_t)y * w + x) * 4;
                // Left 60% is one dominant colour, the rest is `bands` distinct
                // flat colours -- the same shape as sea plus countries.
                if (x < w * 6 / 10) {
                    px[i + 0] = 8; px[i + 1] = 8; px[i + 2] = 41;
                } else {
                    // Coprime with the sampler's stride of 3. `(y*7 + x) % 120`
                    // aliased with it -- 160 is 1 mod 3, so sampling every third
                    // pixel only ever saw b == 0 mod 3, i.e. 40 of the 120
                    // colours. The test then "failed" for its own reasons.
                    int b = ((y * 11 + x * 7) % bands);
                    px[i + 0] = (uint8_t)(40 + (b * 17) % 200);
                    px[i + 1] = (uint8_t)(40 + (b * 53) % 200);
                    px[i + 2] = (uint8_t)(40 + (b * 97) % 200);
                }
            }
        GifEncoder g;
        bool ok = g.begin(dir + "/flatmany.gif", w, h, 10);
        g.addPaletteSample(px.data());
        ok = ok && g.writeFrame(px.data());
        ok = ok && g.end();
        check(ok, "120 flat colours over a dominant background");
    }

    // A frame that is nothing but one colour: the LZW run is maximal and every
    // code repeats, which is the other end of the dictionary behaviour.
    {
        GifEncoder g;
        std::vector<uint8_t> flat((size_t)50 * 50 * 4, 0);
        for (size_t p = 0; p < 50 * 50; ++p) {
            flat[p * 4 + 0] = 17; flat[p * 4 + 1] = 99;
            flat[p * 4 + 2] = 200; flat[p * 4 + 3] = 255;
        }
        bool ok = g.begin(dir + "/flat.gif", 50, 50, 20);
        ok = ok && g.writeFrame(flat.data());
        ok = ok && g.writeFrame(flat.data());
        ok = ok && g.end();
        check(ok && g.frameCount() == 2, "flat colour, 2 frames (maximal runs)");
    }

    // Misuse should fail cleanly rather than write a broken file.
    {
        GifEncoder g;
        check(!g.begin(dir + "/bad.gif", 0, 10, 4), "begin() rejects zero width");
        GifEncoder h;
        h.begin(dir + "/empty.gif", 8, 8, 4);
        check(!h.end(), "end() without a frame reports failure");
    }

    FILE* man = fopen((dir + "/manifest.txt").c_str(), "w");
    if (man) {
        fprintf(man, "gradient.gif 64 32 6 8\n");
        fprintf(man, "motion.gif 96 24 8 5\n");
        fprintf(man, "noise.gif 80 40 3 10\n");
        fprintf(man, "single.gif 40 40 1 100\n");
        fprintf(man, "wide.gif 300 8 4 4\n");
        fprintf(man, "flat.gif 50 50 2 20\n");
        fprintf(man, "flatmany.gif 160 120 1 10\n");
        fclose(man);
    } else {
        check(false, "write manifest");
    }

    printf("\n%s\n", failures ? "SOMETHING FAILED" : "encoder side ok");
    return failures ? 1 : 0;
}
