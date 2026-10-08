// symbol-fill: how much of its own canvas does a symbol SVG actually cover?
//
// WHY THIS IS C++ AND NOT PART OF THE PYTHON TOOLS
//
// The question "how big is this symbol" only has one answer that matters: the
// one the game gets. The game rasterises symbols with nanosvg
// (src/renderer/nanosvg.h, via FlagRenderer::rasterizeSVG), and nanosvg is not
// a conformant SVG renderer -- it ignores <use>, gradients and clip paths, and
// approximates arcs its own way. A bounding box computed by parsing path data
// in Python would be the RIGHT answer to the WRONG question, and would drift
// from the renderer the moment either changed.
//
// So this links the same header the game does and measures the alpha bounding
// box of the same raster, using the same scale-to-fit convention. What it
// prints is what a flag will show.
//
//     symbol-fill data/symbols/*.svg              # human-readable table
//     symbol-fill --json data/symbols/*.svg       # one JSON object per line
//
// Fractions are of the SVG's own canvas, so 1.0 means "touches the edge".
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

// Big enough that a one-pixel antialiasing fringe is under 0.1% of the answer,
// small enough to stay instant across the whole symbol set.
static const int RASTER = 1024;

int main(int argc, char** argv) {
    bool json = false;
    int first = 1;
    for (; first < argc; ++first) {
        if (strcmp(argv[first], "--json") == 0) json = true;
        else break;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: symbol-fill [--json] FILE.svg...\n");
        return 2;
    }

    if (!json)
        printf("%-24s %8s %8s %8s %8s\n", "file", "fillX", "fillY", "fill", "ink");

    int failures = 0;
    for (int i = first; i < argc; ++i) {
        const char* path = argv[i];
        const char* base = strrchr(path, '/');
        base = base ? base + 1 : path;

        NSVGimage* image = nsvgParseFromFile(path, "px", 96.0f);
        if (!image || image->width <= 0 || image->height <= 0) {
            if (json) printf("{\"file\":\"%s\",\"error\":\"parse failed\"}\n", path);
            else fprintf(stderr, "%-24s PARSE FAILED\n", base);
            if (image) nsvgDelete(image);
            ++failures;
            continue;
        }

        const int w = RASTER, h = RASTER;
        unsigned char* rgba = (unsigned char*)calloc((size_t)w * h * 4, 1);
        NSVGrasterizer* rast = nsvgCreateRasterizer();
        if (!rgba || !rast) { fprintf(stderr, "out of memory\n"); return 1; }

        // Identical to FlagRenderer::rasterizeSVG: preserve aspect, centre,
        // pad with transparency.
        const float sx = (float)w / image->width, sy = (float)h / image->height;
        const float scale = sx < sy ? sx : sy;
        const float tx = ((float)w - image->width * scale) * 0.5f;
        const float ty = ((float)h - image->height * scale) * 0.5f;
        nsvgRasterize(rast, image, tx, ty, scale, rgba, w, h, w * 4);
        nsvgDeleteRasterizer(rast);

        int minx = w, miny = h, maxx = -1, maxy = -1;
        double ink = 0.0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const unsigned char a = rgba[((size_t)y * w + x) * 4 + 3];
                if (!a) continue;
                ink += a / 255.0;
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        free(rgba);

        if (maxx < 0) {
            // Not a warning to bury: a symbol that rasterises to nothing is
            // how star_4 shipped broken, and nanosvg fails quietly.
            if (json) printf("{\"file\":\"%s\",\"error\":\"rasterises to nothing\"}\n", path);
            else fprintf(stderr, "%-24s EMPTY -- rasterises to nothing\n", base);
            nsvgDelete(image);
            ++failures;
            continue;
        }

        const float cw = image->width * scale, ch = image->height * scale;
        const float x0 = ((float)minx - tx) / cw, x1 = ((float)maxx + 1 - tx) / cw;
        const float y0 = ((float)miny - ty) / ch, y1 = ((float)maxy + 1 - ty) / ch;
        const float fillX = x1 - x0, fillY = y1 - y0;
        const float fill = std::max(fillX, fillY);
        const double inkFrac = ink / ((double)cw * ch);

        if (json)
            printf("{\"file\":\"%s\",\"canvas\":[%.4f,%.4f],\"bbox\":[%.6f,%.6f,%.6f,%.6f],"
                   "\"fill\":%.6f,\"ink\":%.6f}\n",
                   path, image->width, image->height, x0, y0, x1, y1, fill, inkFrac);
        else
            printf("%-24s %7.1f%% %7.1f%% %7.1f%% %7.1f%%\n",
                   base, fillX * 100, fillY * 100, fill * 100, inkFrac * 100);

        nsvgDelete(image);
    }
    return failures ? 1 : 0;
}
