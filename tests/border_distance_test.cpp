// The border distance field, against the loop it replaced.
//
// odBorderChamfer splits each chamfer pass into a vectorised row step and a
// sixteen-pixel prefix scan, with a separate implementation per instruction set
// (NEON, SSE2, WASM SIMD, scalar). The claim is that every one of them writes
// exactly the bytes the original one-pixel-at-a-time loop wrote. That is a
// claim about all inputs, so it is checked here on many: random seeds, random
// border lines, arbitrary pre-filled values, every width around a multiple of
// sixteen, and a stride wider than the row (the incremental rebuild's window).
//
// Integer work only, so a pass on one CPU is a pass on all of them -- but run
// it on each anyway; that is what the per-architecture CI jobs are for.

#include "map/BorderDistance.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

// THE ORIGINAL, verbatim but for the signature: Game_Loading.cpp's passes as
// they stood before the split.
static void referencePasses(std::vector<uint8_t>& out, int w, int h) {
    auto relax = [&out](size_t i, size_t from, uint8_t step) {
        const uint8_t d = out[from];
        if (d >= 60) return;                 // the BFS did not expand from here
        const uint8_t nd = (uint8_t)(d + step);
        if (nd < out[i]) out[i] = nd;
    };
    for (int y = 0; y < h; ++y) {
        const size_t row = (size_t)y * w;
        for (int x = 0; x < w; ++x) {
            const size_t i = row + x;
            if (y > 0) {
                relax(i, i - w, 2);
                if (x > 0)     relax(i, i - w - 1, 3);
                if (x < w - 1) relax(i, i - w + 1, 3);
            }
            if (x > 0) relax(i, i - 1, 2);
        }
    }
    for (int y = h - 1; y >= 0; --y) {
        const size_t row = (size_t)y * w;
        for (int x = w - 1; x >= 0; --x) {
            const size_t i = row + x;
            if (y < h - 1) {
                relax(i, i + w, 2);
                if (x > 0)     relax(i, i + w - 1, 3);
                if (x < w - 1) relax(i, i + w + 1, 3);
            }
            if (x < w - 1) relax(i, i + 1, 2);
        }
    }
}

static std::vector<uint8_t> makeField(int w, int h, std::mt19937& rng, int kind) {
    std::vector<uint8_t> v((size_t)w * h, 255);
    auto pct = [&]() { return (int)(rng() % 1000); };
    if (kind == 0) {                         // scattered seeds
        for (auto& b : v) if (pct() < 8) b = 0;
    } else {                                 // wandering border lines
        const int lines = std::max(1, (w + h) / 16);
        for (int k = 0; k < lines; ++k) {
            int x = (int)(rng() % w), y = (int)(rng() % h);
            int dx = (int)(rng() % 3) - 1, dy = (int)(rng() % 3) - 1;
            for (int s = 0; s < w + h; ++s) {
                if (x < 0 || y < 0 || x >= w || y >= h) break;
                v[(size_t)y * w + x] = 0;
                x += dx; y += dy;
                if (pct() < 200) { dx = (int)(rng() % 3) - 1; dy = (int)(rng() % 3) - 1; }
            }
        }
    }
    if (kind == 2)                           // anything at all, not just 0/255
        for (auto& b : v) if (pct() < 40) b = (uint8_t)rng();
    return v;
}

int main() {
    std::mt19937 rng(20261101);
    int cases = 0, bad = 0;
    auto check = [&](int w, int h, int kind, int pad) {
        std::vector<uint8_t> want = makeField(w, h, rng, kind);
        // The same field inside a wider buffer, with junk in the padding that
        // must neither be read nor written.
        const int stride = w + pad;
        std::vector<uint8_t> got((size_t)stride * h, 0xA5);
        for (int y = 0; y < h; ++y)
            std::memcpy(&got[(size_t)y * stride], &want[(size_t)y * w], (size_t)w);
        referencePasses(want, w, h);
        odBorderChamfer(got.data(), w, h, (size_t)stride);
        ++cases;
        for (int y = 0; y < h; ++y) {
            const bool rowOk = std::memcmp(&got[(size_t)y * stride], &want[(size_t)y * w], (size_t)w) == 0;
            bool padOk = true;
            for (int p = 0; p < pad; ++p) padOk &= got[(size_t)y * stride + w + p] == 0xA5;
            if (!rowOk || !padOk) {
                if (++bad <= 10)
                    printf("FAIL  %dx%d kind %d pad %d: row %d %s\n", w, h, kind, pad, y,
                           rowOk ? "padding written" : "differs");
                return;
            }
        }
    };
    const int widths[] = {1, 2, 3, 15, 16, 17, 31, 32, 33, 47, 48, 64, 100, 257};
    const int heights[] = {1, 2, 3, 17, 64};
    for (int rep = 0; rep < 6; ++rep)
        for (int w : widths)
            for (int h : heights)
                for (int kind = 0; kind < 3; ++kind)
                    check(w, h, kind, (rep & 1) ? 13 : 0);
    for (int rep = 0; rep < 4; ++rep) check(1024 + rep * 7, 512, rep % 3, rep * 5);

    printf("border distance (%s): %d cases, %d failed\n", odBorderChamferPath(), cases, bad);
    return bad ? 1 : 0;
}
