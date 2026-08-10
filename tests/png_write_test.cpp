// Indexed-PNG writer round-trip test.
//
// src/util/PngWrite.cpp encodes the map layers a .odmap ships. Its whole claim
// is that the bytes it writes decode back to the pixels that went in -- that is
// what makes shrinking a map lossless rather than merely smaller -- and a
// bitstream writer cannot check its own deflate. So this decodes with the
// decoder the game actually reads maps through (stb_image, which is what
// raylib's LoadImageFromMemory calls) and compares pixel for pixel. Then
// tests/png_write_check.py decodes the same files with Pillow, because two
// independent decoders agreeing is the only real evidence: a malformed PNG
// often still opens in the library that wrote it.
//
//   PngWriteTest <out-dir>
//
// writes <out-dir>/depth<N>.png for each palette size and a manifest naming
// what each one should contain.

#include "../src/util/PngWrite.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  %-58s %s\n", what.c_str(), ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

/**
 * A blocky image using exactly `colours` distinct RGBA values.
 *
 * Blocky rather than random on purpose: a map layer is broad flat regions, so
 * this exercises the run-length behaviour the encoder is tuned for. Colour 0 is
 * fully transparent, which is what puts the tRNS chunk under test -- land_sea
 * layers really do carry a transparent entry.
 */
std::vector<unsigned char> makeImage(int colours, int w, int h) {
    std::vector<unsigned char> px((size_t)w * h * 4);
    for (long i = 0; i < (long)w * h; ++i) {
        int c = (int)((i / 7 + i / ((long)w * 3)) % colours);
        px[i * 4 + 0] = (unsigned char)(c * 7 + 1);
        px[i * 4 + 1] = (unsigned char)(c * 13 + 2);
        px[i * 4 + 2] = (unsigned char)(c * 29 + 3);
        px[i * 4 + 3] = (c == 0) ? 0 : 255;
    }
    return px;
}

// PNG layout: 8-byte signature, then IHDR's length+type (8 bytes), then
// width(4) height(4), so the bit-depth byte is at 24.
int bitDepthOf(const std::vector<uint8_t>& png) { return png.size() > 24 ? png[24] : -1; }

void roundTrip(int colours, int expectDepth, const std::string& outDir) {
    const int w = 512, h = 256;
    std::vector<unsigned char> src = makeImage(colours, w, h);
    std::vector<uint8_t> png = pngw::encodeIndexedRGBA(src.data(), w, h);

    char label[96];
    snprintf(label, sizeof(label), "%d colours -> %d-bit indexed", colours, expectDepth);
    if (png.empty()) {
        check(false, std::string(label) + " (encoder returned nothing)");
        return;
    }
    check(bitDepthOf(png) == expectDepth, label);

    char path[512];
    snprintf(path, sizeof(path), "%s/depth%d.png", outDir.c_str(), colours);
    FILE* f = fopen(path, "wb");
    if (!f) { check(false, std::string("write ") + path); return; }
    fwrite(png.data(), 1, png.size(), f);
    fclose(f);

    int rw = 0, rh = 0, rn = 0;
    unsigned char* back = stbi_load(path, &rw, &rh, &rn, 4);
    if (!back) {
        check(false, std::string(label) + ": stb_image refused it");
        return;
    }
    bool same = (rw == w && rh == h);
    for (long i = 0; same && i < (long)w * h * 4; ++i)
        if (src[i] != back[i]) same = false;
    stbi_image_free(back);
    check(same, std::string(label) + ": decodes to the same pixels");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string outDir = argc > 1 ? argv[1] : ".";
    printf("PngWrite round-trip\n");

    // One case per bit depth the encoder can choose, and the boundaries where
    // it steps up: 2/4/16/256 are the last palette size each depth can hold.
    roundTrip(2, 1, outDir);
    roundTrip(3, 2, outDir);
    roundTrip(4, 2, outDir);
    roundTrip(5, 4, outDir);
    roundTrip(16, 4, outDir);
    roundTrip(17, 8, outDir);
    roundTrip(256, 8, outDir);

    // Past the palette limit the answer must be "not mine" rather than a
    // quantised approximation -- the caller falls back to truecolour, and a
    // lossy result here would silently corrupt province ids.
    {
        std::vector<unsigned char> src = makeImage(257, 512, 256);
        check(pngw::encodeIndexedRGBA(src.data(), 512, 256).empty(),
              "257 colours -> declined, so the caller can fall back");
    }

    // Degenerate inputs must not reach the encoder's packing loop.
    check(pngw::encodeIndexedRGBA(nullptr, 8, 8).empty(), "null pixels -> empty");
    {
        std::vector<unsigned char> one = makeImage(1, 4, 4);
        check(pngw::encodeIndexedRGBA(one.data(), 0, 4).empty(), "zero width -> empty");
    }

    // A single-colour image: the smallest palette a real layer can have (an
    // all-sea map), and the case an off-by-one in bit packing loses first.
    {
        std::vector<unsigned char> one = makeImage(1, 64, 32);
        std::vector<uint8_t> png = pngw::encodeIndexedRGBA(one.data(), 64, 32);
        check(!png.empty() && bitDepthOf(png) == 1, "1 colour -> 1-bit indexed");
    }

    char manifest[512];
    snprintf(manifest, sizeof(manifest), "%s/manifest.txt", outDir.c_str());
    if (FILE* mf = fopen(manifest, "w")) {
        fprintf(mf, "# file colours width height\n");
        for (int c : {2, 3, 4, 5, 16, 17, 256}) fprintf(mf, "depth%d.png %d 512 256\n", c, c);
        fclose(mf);
    }

    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
