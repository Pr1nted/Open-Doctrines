// High contrast (src/Theme.h): the transform keeps the promises its header
// makes, for every colour, not for the handful a screenshot happens to show.
//
//   idempotent    text reaches raylib through odText AND the shadows, so a
//                 colour transformed twice must come out as it did once
//   contrast      white on black is 21:1, every lifted colour reads against
//                 black at 7:1 or better, and the mid grey still holds 7:1
//                 under white text
//   meaning       a hue survives: war stays red, an alliance stays green
//   off is off    with high contrast off, every colour is left alone
#include <cmath>
#include <cstdio>
#include "Theme.h"

static int fails = 0, checks = 0;
static void check(const char* what, bool ok) {
    ++checks;
    if (!ok) { ++fails; std::printf("  FAIL  %s\n", what); }
}

static float ratio(Color a, Color b) {
    float la = odTheme::detail::luminance(a), lb = odTheme::detail::luminance(b);
    if (la < lb) { float t = la; la = lb; lb = t; }
    return (la + 0.05f) / (lb + 0.05f);
}

static float hue(Color c) {
    float r = c.r / 255.f, g = c.g / 255.f, b = c.b / 255.f;
    float mx = std::fmax(r, std::fmax(g, b)), mn = std::fmin(r, std::fmin(g, b)), d = mx - mn;
    if (d <= 0) return -1;
    float h = mx == r ? std::fmod((g - b) / d, 6.f) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
    h *= 60;
    return h < 0 ? h + 360 : h;
}

static bool same(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

int main() {
    std::printf("=== high contrast ===\n");

    odTheme::setHighContrast(false);
    bool untouched = true;
    for (int v = 0; v < 256; v += 17)
        for (int a = 0; a < 256; a += 85) {
            Color c{(unsigned char)v, (unsigned char)(255 - v), (unsigned char)(v / 2), (unsigned char)a};
            untouched = untouched && same(odTheme::hc(c), c);
        }
    check("off leaves every colour alone", untouched);

    odTheme::setHighContrast(true);
    const Color black{0, 0, 0, 255}, white{255, 255, 255, 255};
    check("white text on a black panel is 21:1", std::fabs(ratio(white, black) - 21.0f) < 0.01f);
    const Color mid{odTheme::kMid, odTheme::kMid, odTheme::kMid, 255};
    check("white text on the mid grey holds 7:1", ratio(white, mid) >= 7.0f);
    check("the mid grey is a visible step above black", ratio(mid, black) >= 1.5f);

    int notIdem = 0, dim = 0, hueLost = 0, colours = 0;
    for (int r = 0; r < 256; r += 15)
        for (int g = 0; g < 256; g += 15)
            for (int b = 0; b < 256; b += 15) {
                const Color c{(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
                const Color once = odTheme::hc(c), twice = odTheme::hc(once);
                if (!same(once, twice)) ++notIdem;
                const bool grey = once.r == once.g && once.g == once.b;
                if (!grey) {
                    ++colours;
                    if (ratio(once, black) < 7.0f) ++dim;
                    const float h0 = hue(c), h1 = hue(once);
                    float dh = std::fabs(h0 - h1);
                    if (dh > 180) dh = 360 - dh;
                    if (h0 >= 0 && dh > 12.0f) ++hueLost;
                }
            }
    std::printf("  %d colours lifted, %d not idempotent, %d under 7:1, %d hue moved\n",
                colours, notIdem, dim, hueLost);
    check("transforming twice is transforming once", notIdem == 0);
    check("every lifted colour reads against black at 7:1", dim == 0);
    check("a colour keeps its hue (within 12 degrees)", hueLost == 0);

    // Fills: a backdrop darkens under white text, a mark lifts on black.
    int fillDim = 0, fillNotIdem = 0;
    for (int r = 0; r < 256; r += 15)
        for (int g = 0; g < 256; g += 15)
            for (int b = 0; b < 256; b += 15) {
                const Color c{(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
                const Color f = odTheme::hcFill(c, 200, 40);
                if (!same(odTheme::hcFill(f, 200, 40), f)) ++fillNotIdem;
                if (!(f.r == f.g && f.g == f.b) && ratio(white, f) < 7.0f) ++fillDim;
            }
    std::printf("  fills: %d not idempotent, %d under 7:1 against white text\n", fillNotIdem, fillDim);
    check("a button face transformed twice is transformed once", fillNotIdem == 0);
    check("white text on every coloured button face holds 7:1", fillDim == 0);
    const Color go{40, 160, 60, 255}, goFace = odTheme::hcFill(go, 140, 32);
    check("Process Turn stays green, and dark enough for its label",
          goFace.g > goFace.r && goFace.g > goFace.b && ratio(white, goFace) >= 7.0f);
    const Color swatch = odTheme::hcFill(Color{220, 40, 40, 255}, 10, 10);
    check("a small swatch is lifted like text, not darkened", ratio(swatch, black) >= 7.0f);

    const Color war{220, 40, 40, 255}, alliance{54, 205, 64, 255};
    const Color w = odTheme::hc(war), al = odTheme::hc(alliance);
    check("war is still red", w.r > w.g && w.r > w.b);
    check("an alliance is still green", al.g > al.r && al.g > al.b);

    const Color panel{20, 22, 30, 200}, p = odTheme::hc(panel);
    check("a translucent dark panel becomes opaque black", same(p, black));
    const Color faint{200, 200, 210, 60};
    check("faint text keeps its own alpha", odTheme::hc(faint).a == 60);

    std::printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
