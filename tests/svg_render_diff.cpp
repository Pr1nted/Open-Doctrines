// Does this SVG still LOOK the same?
//
//     SvgRenderDiff <before.svg> <after.svg> [width] [height]
//
// Rasterises both files and reports how far apart the pixels are. Exit 0 when
// they are within tolerance, 1 when they are not, 2 when a file will not parse.
//
// WHY THIS EXISTS: the shipped flag artwork was optimised -- rescaled out of a
// 357,185-unit CorelDRAW coordinate space and rounded -- and "the file got 90%
// smaller" is not evidence that anything still draws. An SVG optimiser is free
// to emit perfectly valid SVG that nanosvg, which is not a complete SVG
// implementation, renders differently or not at all. The only answer that
// counts is the one from the renderer the game actually uses, at the size the
// game actually draws.
//
// So this is deliberately NOT a generic image differ. It rasterises through the
// same nanosvg the game does, and reproduces FlagRenderer::rasterizeSVG's
// scale-to-fit exactly -- same aspect handling, same centring -- because a
// comparison done any other way is a comparison of something else.
//
// Alpha is compared with the colour, not separately: a flag is drawn over a map
// and a path that lost its fill goes transparent rather than changing hue.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../src/renderer/nanosvg.h"
#include "../src/renderer/nanosvgrast.h"

// -o <prefix> writes <prefix>-a.png and <prefix>-b.png. Numbers say something
// changed; only the pictures say what, and every optimiser failure so far has
// been obvious in two seconds of looking and opaque in the statistics.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

// Same defaults as the in-game flag render (Game_Loading.cpp asks for 256x128).
constexpr int DEFAULT_W = 256;
constexpr int DEFAULT_H = 128;

// A channel this far apart is antialiasing on a rounded coordinate, not a
// missing shape. Rounding moves an edge by a fraction of a pixel and the
// covered-area calculation moves with it, so edge pixels differ slightly in
// every honest optimisation -- what must not happen is a REGION changing.
constexpr int   MAX_CHANNEL_DIFF = 72;   // worst single pixel
constexpr double MAX_MEAN_DIFF   = 1.6;  // averaged over the whole image

bool rasterize(const char* path, int w, int h, std::vector<unsigned char>& out) {
    // nsvgParseFromFile mutates the buffer it parses, exactly as the game's
    // path does. Nothing here may reuse it.
    NSVGimage* img = nsvgParseFromFile(path, "px", 96.0f);
    if (!img) { fprintf(stderr, "  cannot parse: %s\n", path); return false; }
    if (img->width <= 0 || img->height <= 0) {
        fprintf(stderr, "  zero-sized image: %s (%gx%g)\n", path, img->width, img->height);
        nsvgDelete(img);
        return false;
    }

    out.assign((size_t)w * h * 4, 0);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(img); return false; }

    // Lifted from FlagRenderer::rasterizeSVG. Scale to fit, preserve aspect,
    // centre with transparent padding -- if this drifts from that function the
    // tool stops measuring what the game sees.
    const float sx = (float)w / img->width;
    const float sy = (float)h / img->height;
    const float scale = sx < sy ? sx : sy;
    const float tx = ((float)w - img->width * scale) * 0.5f;
    const float ty = ((float)h - img->height * scale) * 0.5f;
    nsvgRasterize(rast, img, tx, ty, scale, out.data(), w, h, w * 4);

    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: SvgRenderDiff <before.svg> <after.svg> [w] [h] [-o prefix]\n");
        return 2;
    }
    const char* dump = nullptr;
    std::vector<const char*> pos;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { dump = argv[++i]; continue; }
        pos.push_back(argv[i]);
    }
    if (pos.size() < 2) { fprintf(stderr, "need two files\n"); return 2; }
    const int w = pos.size() > 2 ? atoi(pos[2]) : DEFAULT_W;
    const int h = pos.size() > 3 ? atoi(pos[3]) : DEFAULT_H;
    if (w < 1 || h < 1) { fprintf(stderr, "bad size\n"); return 2; }

    std::vector<unsigned char> a, b;
    if (!rasterize(pos[0], w, h, a)) return 2;
    if (!rasterize(pos[1], w, h, b)) return 2;

    if (dump) {
        char p[1024];
        snprintf(p, sizeof p, "%s-a.png", dump);
        stbi_write_png(p, w, h, 4, a.data(), w * 4);
        snprintf(p, sizeof p, "%s-b.png", dump);
        stbi_write_png(p, w, h, 4, b.data(), w * 4);
    }

    long worst = 0;
    double total = 0.0;
    long changed = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const long d = std::labs((long)a[i] - (long)b[i]);
        if (d > worst) worst = d;
        total += (double)d;
        if (d > 0) ++changed;
    }
    const double mean = total / (double)a.size();
    const double pctChanged = 100.0 * (double)changed / (double)a.size();

    const bool ok = worst <= MAX_CHANNEL_DIFF && mean <= MAX_MEAN_DIFF;
    printf("%-28s worst %3ld  mean %6.3f  channels touched %5.1f%%  %s\n",
           pos[1], worst, mean, pctChanged, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
