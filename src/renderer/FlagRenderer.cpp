#include "FlagRenderer.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <cstring>
#include "nanosvg.h"
#include "nanosvgrast.h"

// ══════════════════════════════════════════════════════
//  Color helper
// ══════════════════════════════════════════════════════
static Color getColor(const std::vector<Color>& colors, int index, Color fallback) {
    return (index < (int)colors.size()) ? colors[index] : fallback;
}

// ══════════════════════════════════════════════════════
//  Pixel-level primitives
// ══════════════════════════════════════════════════════
void FlagRenderer::fillImage(Image* img, Color c) {
    for (int y = 0; y < img->height; ++y)
        for (int x = 0; x < img->width; ++x)
            ImageDrawPixel(img, x, y, c);
}

void FlagRenderer::fillCircle(Image* img, int cx, int cy, int r, Color c) {
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            if (dx * dx + dy * dy <= r * r) {
                int px = cx + dx, py = cy + dy;
                if (px >= 0 && px < img->width && py >= 0 && py < img->height)
                    ImageDrawPixel(img, px, py, c);
            }
}

void FlagRenderer::fillRect(Image* img, int x, int y, int w, int h, Color c) {
    for (int dy = 0; dy < h; ++dy)
        for (int dx = 0; dx < w; ++dx) {
            int px = x + dx, py = y + dy;
            if (px >= 0 && px < img->width && py >= 0 && py < img->height)
                ImageDrawPixel(img, px, py, c);
        }
}

void FlagRenderer::fillTriangle(Image* img, int x1, int y1, int x2, int y2, int x3, int y3, Color c) {
    // Rasterize via barycentric
    int minX = std::max(0, std::min({x1, x2, x3}));
    int maxX = std::min(img->width - 1, std::max({x1, x2, x3}));
    int minY = std::max(0, std::min({y1, y2, y3}));
    int maxY = std::min(img->height - 1, std::max({y1, y2, y3}));
    auto orient = [](int ax, int ay, int bx, int by, int cx, int cy) {
        return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    };
    int o1 = orient(x1, y1, x2, y2, x3, y3);
    if (o1 == 0) return;
    for (int py = minY; py <= maxY; ++py)
        for (int px = minX; px <= maxX; ++px) {
            int o2 = orient(x2, y2, x3, y3, px, py);
            int o3 = orient(x3, y3, x1, y1, px, py);
            if ((o1 > 0 && o2 >= 0 && o3 >= 0) || (o1 < 0 && o2 <= 0 && o3 <= 0))
                ImageDrawPixel(img, px, py, c);
        }
}

void FlagRenderer::drawLine(Image* img, int x1, int y1, int x2, int y2, int thick, Color c) {
    // Bresenham-style thick line using small rectangles
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2;
    int px = x1, py = y1;
    while (true) {
        for (int t = -thick/2; t <= thick/2; ++t)
            for (int tt = -thick/2; tt <= thick/2; ++tt) {
                int tx = px + t, ty = py + tt;
                if (tx >= 0 && tx < img->width && ty >= 0 && ty < img->height)
                    ImageDrawPixel(img, tx, ty, c);
            }
        if (px == x2 && py == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; px += sx; }
        if (e2 <= dx) { err += dx; py += sy; }
    }
}

void FlagRenderer::fillRing(Image* img, int cx, int cy, int rOuter, int rInner, Color c) {
    if (rInner < 0) rInner = 0;
    for (int dy = -rOuter; dy <= rOuter; ++dy)
        for (int dx = -rOuter; dx <= rOuter; ++dx) {
            const int d2 = dx * dx + dy * dy;
            if (d2 > rOuter * rOuter || d2 < rInner * rInner) continue;
            const int px = cx + dx, py = cy + dy;
            if (px >= 0 && px < img->width && py >= 0 && py < img->height)
                ImageDrawPixel(img, px, py, c);
        }
}

// ══════════════════════════════════════════════════════
//  Reading the field a generated emblem is about to land on
// ══════════════════════════════════════════════════════

static float luminanceOf(Color c) {
    return (c.r * 0.2126f + c.g * 0.7152f + c.b * 0.0722f) / 255.0f;
}

// Above this, the best square placeSymbol could find -- at any size the
// arrangement will shrink to -- is still not somewhere an emblem can sit, and
// an EmblemField::AUTO_ symbol gets a field of its own instead.
//
// Measured against the shipped flags rather than guessed, at committed
// intensity. France, Italy, Sweden and Brazil come out at 0.000 -- they have a
// plain band or a plain field an emblem sits in whole -- and Japan, Turkey,
// Poland and Switzerland reach 0.026 at worst. The Union Jack starts at 0.033
// and the American flag at 0.189, because neither has a plain patch anywhere on
// it, and Nepal starts at 0.074 because a pennon's diagonal cuts every square.
// The two populations do not overlap and this sits in the gap.
//
// Radical intensities of otherwise-clean flags land above it where their larger
// arrangements cannot fit anywhere whole -- Sweden 0.150, Brazil 0.117 -- and
// those are exactly the ones that looked wrong straddling a cross arm or a
// globe, so they get a field too.
static constexpr float EMBLEM_FIELD_THRESHOLD = 0.030f;

/**
 * How light, and how BUSY, the square an emblem would occupy is.
 *
 * Variance is the useful half. A flag has plenty of room on it and almost none
 * of it is uniform: the mean alone would happily drop a star onto the middle of
 * the Union Jack, where the average of navy, white and red is a perfectly
 * reasonable mid-grey and the picture underneath is a mess.
 */
FlagRenderer::FieldStats FlagRenderer::measureField(const Image& img, int cx, int cy, int r) {
    return measureRegion(img, cx - r, cy - r, 2 * r + 1, 2 * r + 1);
}

FlagRenderer::FieldStats FlagRenderer::measureRegion(const Image& img, int rx, int ry, int rw, int rh) {
    FieldStats out;
    constexpr int BINS = 32;
    int hist[BINS] = {0};
    double sum = 0.0, sumSq = 0.0;
    int n = 0, opaque = 0;
    // Every third pixel: this runs for a handful of candidates per flag and the
    // answer does not change at full density.
    const int step = std::max(1, std::min(rw, rh) / 24);
    for (int y = ry; y < ry + rh; y += step)
        for (int x = rx; x < rx + rw; x += step) {
            ++n;
            if (x < 0 || x >= img.width || y < 0 || y >= img.height) continue;
            const Color c = GetImageColor(img, x, y);
            if (c.a < 8) continue;              // letterbox padding: not flag
            const float l = luminanceOf(c);
            sum += l; sumSq += l * l; ++opaque;
            int b = (int)(l * BINS);
            hist[b < 0 ? 0 : (b >= BINS ? BINS - 1 : b)]++;
        }
    if (n == 0 || opaque == 0) return out;      // coverage 0: nothing to land on
    out.mean     = (float)(sum / opaque);
    out.variance = (float)std::max(0.0, sumSq / opaque - out.mean * (double)out.mean);
    out.coverage = (float)opaque / (float)n;

    // How much of the square is ONE colour.
    //
    // This is the number that matters and variance is not a substitute for it.
    // An emblem looks wrong when it STRADDLES something -- the arm of a cross,
    // the edge of a device -- and a straddle is two flat colours in one square,
    // which is a shape variance barely registers: sixty per cent of Sweden's
    // maroon field against forty of its red cross is a variance of 0.016, well
    // under a threshold calibrated on the Union Jack's five-way mess. As a
    // fraction it is forty per cent foreign, which is exactly what it looks
    // like. Japan's disc, meanwhile, is a device an emblem sits happily inside,
    // and it is pure -- so this rejects what looks bad and permits what does
    // not, where "busy-ness" rejected both or neither.
    int best = 0, bestIdx = 0;
    for (int b = 0; b < BINS; ++b)
        if (hist[b] > best) { best = hist[b]; bestIdx = b; }
    int nearMode = 0;
    for (int b = std::max(0, bestIdx - 1); b <= std::min(BINS - 1, bestIdx + 1); ++b)
        nearMode += hist[b];
    out.purity = (float)nearMode / (float)opaque;
    return out;
}

/**
 * Where on this particular flag the emblem should actually go.
 *
 * The pattern's own (x, y) is candidate one and wins ties, so an authored
 * position is only overruled when it is measurably bad. The rest are the places
 * a flag conventionally carries a device -- the two cantons, the two lower
 * quarters, the centre.
 *
 * Scored on two things, because either alone picks badly. Plainness alone puts
 * a white star on the white half of Poland, which is where it was and which is
 * nowhere. Contrast alone puts it in the middle of the Union Jack, where a
 * white star and the average of navy-white-red are satisfyingly far apart and
 * the picture underneath is a mess. Wanting both finds the red band, which is
 * where a person would have put it.
 */
FlagRenderer::FlagBounds FlagRenderer::flagBounds(const Image& img) {
    int x0 = img.width, y0 = img.height, x1 = -1, y1 = -1;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
            if (GetImageColor(img, x, y).a >= 8) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < 0) return {0, 0, img.width, img.height};      // nothing drawn yet
    return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

/** fillRect, but only where there is already flag to paint on. */
void FlagRenderer::fillRectMasked(Image* img, int x, int y, int w, int h, Color c) {
    for (int dy = 0; dy < h; ++dy)
        for (int dx = 0; dx < w; ++dx) {
            const int px = x + dx, py = y + dy;
            if (px < 0 || px >= img->width || py < 0 || py >= img->height) continue;
            if (GetImageColor(*img, px, py).a < 8) continue;   // outside the flag's shape
            ImageDrawPixel(img, px, py, c);
        }
}

float FlagRenderer::placeSymbol(const Image& img, const FlagSymbol& sym, const FlagBounds& b,
                                 int r, Color symbolColor, int* cx, int* cy) {
    const float symLum = luminanceOf(symbolColor);
    // A grid rather than a handful of named spots. Five candidates was not
    // enough on Japan: every corner of the hinomaru flag is white, so no
    // candidate both contrasted and sat clear of the disc, and the emblem stayed
    // in the canton clipping the disc's edge. A sweep finds the inside of the
    // disc, which is plain, contrasts, and is where a device on that flag would
    // actually go.
    static const float XS[] = {0.18f, 0.32f, 0.50f, 0.68f, 0.82f};
    static const float YS[] = {0.24f, 0.50f, 0.76f};

    float bestScore = 0.0f, bestQuality = 0.0f;
    int   bestX = *cx, bestY = *cy;
    bool  haveBest = false;

    // Fractions are of the FLAG, not of the canvas. On Switzerland, which is
    // square and sits in the left half of its slot, an 0.82 candidate was in
    // the empty right half; on Nepal every candidate past the pennon's diagonal
    // was outside the flag altogether.
    auto consider = [&](float fx, float fy) {
        int px = b.x + (int)(fx * b.w);
        int py = b.y + (int)(fy * b.h);
        // Same clamp the drawing does, so a candidate is scored where it would
        // actually be drawn rather than where it was asked for.
        if (px - r < b.x)         px = b.x + r;
        if (py - r < b.y)         py = b.y + r;
        if (px + r > b.x + b.w)   px = b.x + b.w - r;
        if (py + r > b.y + b.h)   py = b.y + b.h - r;

        const FieldStats st = measureField(img, px, py, r);
        const float shortfall = std::max(0.0f, 0.32f - std::fabs(symLum - st.mean));
        // How far this is from the position the pattern asked for, as a
        // fraction of the flag. It is a tie-breaker, not a driver: the canton
        // keeps the emblem unless somewhere else is measurably better for it.
        const float dx = (fx - sym.x), dy = (fy - sym.y);
        const float pull = std::sqrt(dx * dx + dy * dy);
        // Hanging off the edge of a letterboxed flag into the transparent
        // padding is disqualifying, not merely a bit worse.
        const float offFlag = (1.0f - st.coverage) * 2.0f;
        // Two numbers, deliberately. `quality` is how good this spot is for the
        // emblem and is what gets RETURNED, because the caller uses it to
        // decide whether the flag has anywhere at all. `score` adds the
        // distance from the canton, which only ever picks between spots and
        // must not make a perfectly good far-away one look bad.
        const float quality = (1.0f - st.purity) + shortfall * 0.25f + offFlag;
        const float score   = quality + pull * 0.05f;
        if (!haveBest || score < bestScore) {
            bestScore = score; bestQuality = quality; bestX = px; bestY = py; haveBest = true;
        }
    };

    consider(sym.x, sym.y);                    // what the pattern asked for
    for (float fx : XS)
        for (float fy : YS)
            consider(fx, fy);

    // Then walk downhill from whichever cell won, halving the step each pass.
    // The grid gets the emblem onto the right FEATURE; this centres it on that
    // feature. Without it the star landed on the nearest grid point to the
    // hinomaru rather than on the hinomaru, and hung over the edge.
    float step = 0.06f;
    for (int pass = 0; pass < 3; ++pass) {
        const float bx = (float)(bestX - b.x) / (float)b.w;
        const float by = (float)(bestY - b.y) / (float)b.h;
        for (int oy = -1; oy <= 1; ++oy)
            for (int ox = -1; ox <= 1; ++ox)
                if (ox || oy) consider(bx + ox * step, by + oy * step);
        step *= 0.5f;
    }

    *cx = bestX;
    *cy = bestY;
    return bestQuality;
}

// ══════════════════════════════════════════════════════
//  Ideological recolouring
// ══════════════════════════════════════════════════════

namespace {

struct Hsl { float h, s, l; };

Hsl toHsl(Color c) {
    const float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
    const float mx = std::fmax(r, std::fmax(g, b));
    const float mn = std::fmin(r, std::fmin(g, b));
    Hsl o{0.0f, 0.0f, (mx + mn) * 0.5f};
    const float d = mx - mn;
    if (d < 1e-6f) return o;                      // grey: hue is undefined
    o.s = d / (1.0f - std::fabs(2.0f * o.l - 1.0f));
    if      (mx == r) o.h = 60.0f * std::fmod((g - b) / d, 6.0f);
    else if (mx == g) o.h = 60.0f * ((b - r) / d + 2.0f);
    else              o.h = 60.0f * ((r - g) / d + 4.0f);
    if (o.h < 0.0f) o.h += 360.0f;
    return o;
}

Color toRgb(Hsl v, unsigned char alpha) {
    const float c = (1.0f - std::fabs(2.0f * v.l - 1.0f)) * v.s;
    const float x = c * (1.0f - std::fabs(std::fmod(v.h / 60.0f, 2.0f) - 1.0f));
    const float m = v.l - c * 0.5f;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if      (v.h <  60.0f) { r = c; g = x; }
    else if (v.h < 120.0f) { r = x; g = c; }
    else if (v.h < 180.0f) { g = c; b = x; }
    else if (v.h < 240.0f) { g = x; b = c; }
    else if (v.h < 300.0f) { r = x; b = c; }
    else                   { r = c; b = x; }
    auto to8 = [&](float v8) {
        const float f = (v8 + m) * 255.0f;
        return (unsigned char)(f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f));
    };
    return { to8(r), to8(g), to8(b), alpha };
}

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

}  // namespace

// A pixel that carries the flag's politics rather than its structure. White,
// black and near-greys are left EXACTLY alone -- Poland's white half, the white
// stripes of the United States, a tricolour's white band are what the country
// still looks like afterwards, and tinting them is what makes a restyled flag
// look dirty rather than changed.
static bool isChromatic(const Hsl& v) {
    return v.s >= 0.14f && v.l <= 0.88f && v.l >= 0.10f;
}

/**
 * One colour, restyled. `t` is where it ranks among the flag's own chromatic
 * colours, 0 for the darkest and 1 for the lightest; see FlagRecolor.
 */
static Color restyleOne(Color c, const FlagRecolor& rc, float t) {
    Hsl v = toHsl(c);
    if (!isChromatic(v)) return c;

    const float tgtSat   = lerpf(rc.shadeSat,   rc.sat,   t);
    const float tgtLight = lerpf(rc.shadeLight, rc.light, t);

    if (rc.shiftHue) v.h = rc.hue;      // replaced, never interpolated
    v.s = lerpf(v.s, tgtSat,   rc.weight);
    v.l = lerpf(v.l, tgtLight, rc.weight);
    return toRgb(v, c.a);
}

/**
 * The whole flag, restyled -- raster included.
 *
 * This is the part that makes an ideological change FUNDAMENTAL rather than a
 * sticker. Nearly every country flies an image, so recolouring only the
 * pattern palette meant the overwhelming majority of restyles were an emblem
 * dropped on an otherwise untouched flag. The layout, proportions and white are
 * all preserved, so the country is still recognisable at a glance on the map --
 * what changes is what it is made of.
 *
 * Two passes: the first learns how dark and how light this particular flag's
 * colours are, the second maps them onto the ideology's band. See FlagRecolor
 * for why the range has to come from the flag rather than from a constant.
 */
void FlagRenderer::recolorFlag(Image* img, const FlagRecolor& rc) {
    if (!rc.active) return;

    // The flag's ACTUAL colours, found as the populated bins of a histogram.
    //
    // Neither the literal range nor a percentile of it works, and both were
    // tried. A raster flag's edges are antialiased, so between a navy union and
    // a red stripe there is a continuum of in-between pixels: on the American
    // flag, with thirteen stripe boundaries, those fringes are nearly a fifth
    // of every chromatic pixel on it. The range reached almost white, the 92nd
    // percentile still landed in the fringe, and both put the stripes a third
    // of the way up a band whose top they should have owned -- so they came out
    // olive, which is the exact fault the banding exists to fix.
    //
    // A real colour occupies ONE bin and holds a large share of the flag. A
    // fringe is spread thinly across many. Ignoring bins below a floor keeps
    // the first and discards the second.
    constexpr int BINS = 64;
    constexpr int HUE_BINS = 24;
    int hist[BINS] = {0};
    int hueHist[HUE_BINS] = {0};
    int total = 0;
    for (int y = 0; y < img->height; ++y)
        for (int x = 0; x < img->width; ++x) {
            const Color c = GetImageColor(*img, x, y);
            if (c.a == 0) continue;
            const Hsl v = toHsl(c);
            if (!isChromatic(v)) continue;
            int b = (int)(v.l * BINS);
            if (b < 0) b = 0;
            if (b >= BINS) b = BINS - 1;
            ++hist[b];
            int hb = (int)(v.h * HUE_BINS / 360.0f);
            hueHist[hb < 0 ? 0 : (hb >= HUE_BINS ? HUE_BINS - 1 : hb)]++;
            ++total;
        }

    // The flag's own colours, in order, so a colour can be placed by its RANK
    // rather than by where its lightness happens to fall.
    //
    // Brazil is why. Its green sits at lightness 0.30 and the blue of its globe
    // at 0.23, against a yellow at 0.50 -- so on a straight lightness map the
    // field and the globe both landed within a few per cent of the dark end of
    // the band and came out the same colour, and the globe stopped being a
    // device on a field and became a smudge in it. Ranked, three colours become
    // three colours: shade, middle, and the ideology's own.
    std::vector<int> tones;
    float lo = 0.0f, hi = 1.0f;
    if (total > 0) {
        // 6% of the flag's chromatic area. At 2.5% the Union Jack's red-to-white
        // antialiasing formed a 3% plateau one bin above its red, which stretched
        // the top of the band past the colour that should have owned it and put
        // the whole flag back in the olive it was rescued from. A colour a flag
        // is actually MADE of is never this rare.
        const int floorCount = std::max(1, total / 16);
        int loBin = -1, hiBin = -1;
        for (int b = 0; b < BINS; ++b)
            if (hist[b] >= floorCount) {
                if (loBin < 0) loBin = b;
                hiBin = b;
                // Adjacent bins are one antialiased colour, not two.
                if (tones.empty() || b - tones.back() > 1) tones.push_back(b);
            }
        if (loBin < 0) {
            // Nothing concentrated enough -- a flag that is mostly gradient, a
            // painted coat of arms. Fall back to the range of what is there.
            for (int b = 0; b < BINS; ++b)
                if (hist[b] > 0) { if (loBin < 0) loBin = b; hiBin = b; }
        }
        if (loBin >= 0) {
            lo = (loBin + 0.5f) / BINS;
            hi = (hiBin + 0.5f) / BINS;
        }
    }

    // The flag's own dominant hue, for the target that does not have one.
    //
    // Nationalism darkens rather than recolours, and the first version of that
    // kept every colour's own hue -- which on a three-colour flag produced three
    // muted hues, and three muted hues is the definition of mud: Brazil came out
    // a grey-green field, an olive diamond and a blue-grey globe, all at once.
    // Pulling everything to the hue the flag ALREADY mostly is keeps it
    // coherent, and keeps it recognisably that country's flag, without turning
    // every nationalist flag on the map the same charcoal.
    FlagRecolor rcx = rc;
    if (!rc.shiftHue) {
        int bestHue = 0, bestCount = -1;
        for (int i = 0; i < HUE_BINS; ++i)
            if (hueHist[i] > bestCount) { bestCount = hueHist[i]; bestHue = i; }
        rcx.shiftHue = true;
        rcx.hue = (bestHue + 0.5f) * 360.0f / HUE_BINS;
    }

    // A flag with one chromatic colour -- Poland, Japan, Turkey -- has no range
    // to rank, and its single colour should become the ideology's colour
    // outright rather than something halfway down the band.
    const bool ranked = (hi - lo) > 0.10f;

    for (int y = 0; y < img->height; ++y)
        for (int x = 0; x < img->width; ++x) {
            const Color c = GetImageColor(*img, x, y);
            if (c.a == 0) continue;
            float t = 1.0f;
            if (ranked) {
                const Hsl v = toHsl(c);
                if (tones.size() >= 3) {
                    // Placed by rank: nearest tone wins, and the tones are
                    // spread evenly across the band whatever their spacing.
                    size_t nearest = 0;
                    float bestD = 1e9f;
                    for (size_t i = 0; i < tones.size(); ++i) {
                        const float d = std::fabs(v.l - (tones[i] + 0.5f) / BINS);
                        if (d < bestD) { bestD = d; nearest = i; }
                    }
                    t = (float)nearest / (float)(tones.size() - 1);
                } else {
                    t = (v.l - lo) / (hi - lo);
                    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                }
            }
            ImageDrawPixel(img, x, y, restyleOne(c, rcx, t));
        }
}

void FlagRenderer::imagePixelate(Image* img, int blockSize) {
    for (int y = 0; y < img->height; y += blockSize)
        for (int x = 0; x < img->width; x += blockSize) {
            int r = 0, g = 0, b = 0, a = 0, count = 0;
            for (int dy = 0; dy < blockSize && y + dy < img->height; ++dy)
                for (int dx = 0; dx < blockSize && x + dx < img->width; ++dx) {
                    Color c = GetImageColor(*img, x + dx, y + dy);
                    r += c.r; g += c.g; b += c.b; a += c.a; count++;
                }
            r /= count; g /= count; b /= count; a /= count;
            Color avg = {(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a};
            for (int dy = 0; dy < blockSize && y + dy < img->height; ++dy)
                for (int dx = 0; dx < blockSize && x + dx < img->width; ++dx)
                    ImageDrawPixel(img, x + dx, y + dy, avg);
        }
}

void FlagRenderer::imageBlur(Image* img, int radius) {
    if (radius < 1 || img->width < 1 || img->height < 1) return;

    // Block-average mosaic, which is what `FlagPattern::censored` documents
    // itself as doing ("pixelate to obscure hate symbols") and what a censored
    // flag is expected to look like.
    //
    // It replaces a separable Gaussian that did not work. The kernel was
    // normalised in TWO dimensions -- all (2r+1)^2 weights summed to 1 -- and
    // then applied as two one-dimensional passes, each reading a single row of
    // it. Each pass therefore multiplied the image by the sum of one row, about
    // 0.30 at radius 4, and two passes left roughly 9% of the original in both
    // colour AND alpha. The result was not a blurred flag, it was a nearly
    // transparent one, which looked like the censored flag failing to render
    // at all.
    //
    // A mosaic also obscures better than a blur: a swastika at 4px of blur is
    // still legible in outline.
    //
    // But 12px blocks did NOT make it illegible either, which is what this used
    // to compute -- `radius * 3` at the radius 4 every caller passes. Rendered
    // and looked at, the shipped German flag came through a censoring pass with
    // its emblem perfectly readable: a symbol drawn at a quarter of the flag's
    // width survives being averaged into blocks a twentieth of it. The block
    // has to scale with the FLAG, not with a constant, because that is what the
    // symbol on it scales with.
    const int block = std::max({2, radius * 3, std::min(img->width, img->height) / 6});
    for (int by = 0; by < img->height; by += block) {
        for (int bx = 0; bx < img->width; bx += block) {
            const int x1 = std::min(bx + block, img->width);
            const int y1 = std::min(by + block, img->height);
            unsigned long long r = 0, g = 0, b = 0, a = 0;
            int n = 0;
            for (int y = by; y < y1; ++y) {
                for (int x = bx; x < x1; ++x) {
                    Color c = GetImageColor(*img, x, y);
                    // Weight colour by alpha so a transparent margin does not
                    // drag an opaque block toward black.
                    r += (unsigned long long)c.r * c.a;
                    g += (unsigned long long)c.g * c.a;
                    b += (unsigned long long)c.b * c.a;
                    a += c.a;
                    ++n;
                }
            }
            if (n == 0) continue;
            Color avg;
            if (a == 0) {
                avg = BLANK;
            } else {
                avg = { (unsigned char)(r / a), (unsigned char)(g / a),
                        (unsigned char)(b / a), (unsigned char)(a / n) };
            }
            for (int y = by; y < y1; ++y)
                for (int x = bx; x < x1; ++x)
                    ImageDrawPixel(img, x, y, avg);
        }
    }
}

// ══════════════════════════════════════════════════════
//  Text block (for non-ASCII text placeholder)
// ══════════════════════════════════════════════════════
void FlagRenderer::drawTextBlock(Image* img, int cx, int cy, int w, int h, Color color) {
    if (w < 2 || h < 2) { if (cx >= 0 && cx < img->width && cy >= 0 && cy < img->height) ImageDrawPixel(img, cx, cy, color); return; }
    Color bg = { (unsigned char)(color.r * 0.4f), (unsigned char)(color.g * 0.4f),
                 (unsigned char)(color.b * 0.4f), color.a };
    fillRect(img, cx - w/2, cy - h/2, w, h, bg);
    int lineH = (h > 6) ? 2 : 1;
    int gap = (h > 6) ? (h - 3 * lineH) / 4 : 1;
    for (int i = 0; i < 3 && (i * (lineH + gap) + lineH) <= h; ++i) {
        int ly = cy - h/2 + gap + i * (lineH + gap);
        fillRect(img, cx - w/2 + 2, ly, w - 4, lineH, color);
    }
}

void FlagRenderer::drawCensorBar(Image* img, int cx, int cy, int rad) {
    if (rad < 2) { if (cx >= 0 && cx < img->width && cy >= 0 && cy < img->height) ImageDrawPixel(img, cx, cy, BLACK); return; }
    fillCircle(img, cx, cy, rad, BLACK);
    if (rad > 3) {
        int d = rad / 2;
        ImageDrawLine(img, cx - d, cy - d, cx + d, cy + d, WHITE);
        ImageDrawLine(img, cx + d, cy - d, cx - d, cy + d, WHITE);
    }
}

// ══════════════════════════════════════════════════════
//  BACKGROUND PATTERNS
// ══════════════════════════════════════════════════════

void FlagRenderer::drawSolid(Image* img, int w, int h, const std::vector<Color>& colors) {
    fillImage(img, getColor(colors, 0, DARKGRAY));
}

void FlagRenderer::drawStripesH(Image* img, int w, int h, const std::vector<Color>& colors) {
    int n = (int)colors.size();
    if (n == 0) { fillImage(img, DARKGRAY); return; }
    int bandH = h / n;
    for (int i = 0; i < n; ++i) {
        Color c = getColor(colors, i, DARKGRAY);
        ImageDrawRectangle(img, 0, i * bandH, w, (i == n - 1) ? h - i * bandH : bandH, c);
    }
}

void FlagRenderer::drawStripesV(Image* img, int w, int h, const std::vector<Color>& colors) {
    int n = (int)colors.size();
    if (n == 0) { fillImage(img, DARKGRAY); return; }
    int bandW = w / n;
    for (int i = 0; i < n; ++i) {
        Color c = getColor(colors, i, DARKGRAY);
        ImageDrawRectangle(img, i * bandW, 0, (i == n - 1) ? w - i * bandW : bandW, h, c);
    }
}

void FlagRenderer::drawDiagonal(Image* img, int w, int h, bool rightDown, const std::vector<Color>& colors) {
    Color c1 = getColor(colors, 0, DARKGRAY);
    Color c2 = getColor(colors, 1, LIGHTGRAY);
    // Simple two-color diagonal split
    for (int y = 0; y < h; ++y) {
        float t = (float)y / h;
        int splitX = rightDown ? (int)(t * w) : (int)((1.0f - t) * w);
        if (splitX < 0) splitX = 0;
        if (splitX > w) splitX = w;
        for (int x = 0; x < splitX; ++x)
            ImageDrawPixel(img, x, y, c1);
        for (int x = splitX; x < w; ++x)
            ImageDrawPixel(img, x, y, c2);
    }
}

void FlagRenderer::drawTriangle(Image* img, int w, int h, bool doubleTri, const std::vector<Color>& colors) {
    Color bg = getColor(colors, 0, DARKGRAY);
    Color tri = getColor(colors, 1, WHITE);
    fillImage(img, bg);
    // Triangle from left edge to center
    for (int y = 0; y < h; ++y) {
        int triW = (int)((float)y / h * w * 0.45f + (float)(h - y) / h * w * 0.45f);
        if (triW < 0) triW = 0;
        for (int x = 0; x < triW && x < w; ++x)
            ImageDrawPixel(img, x, y, tri);
    }
    if (doubleTri && colors.size() >= 3) {
        // Second triangle (smaller, different color) inset
        Color tri2 = getColor(colors, 2, WHITE);
        for (int y = h / 4; y < 3 * h / 4; ++y) {
            float t = (float)(y - h / 4) / (h / 2);
            int triW = (int)((1.0f - fabs(t - 0.5f) * 2.0f) * w * 0.2f);
            for (int x = 0; x < triW && x < w; ++x)
                ImageDrawPixel(img, x, y, tri2);
        }
    }
}

void FlagRenderer::drawQuartered(Image* img, int w, int h, const std::vector<Color>& colors) {
    Color q1 = getColor(colors, 0, DARKGRAY);
    Color q2 = getColor(colors, 1, WHITE);
    Color q3 = getColor(colors, 2, q2);
    Color q4 = getColor(colors, 3, q1);
    int hw = w / 2, hh = h / 2;
    fillRect(img, 0, 0, hw, hh, q1);
    fillRect(img, hw, 0, w - hw, hh, q2);
    fillRect(img, 0, hh, hw, h - hh, q3);
    fillRect(img, hw, hh, w - hw, h - hh, q4);
}

void FlagRenderer::drawSaltir(Image* img, int w, int h, const std::vector<Color>& colors) {
    // X cross splitting flag into 4 triangles
    Color c1 = getColor(colors, 0, DARKGRAY);
    Color c2 = getColor(colors, 1, WHITE);
    Color c3 = getColor(colors, 2, c1);
    Color c4 = getColor(colors, 3, c2);
    int cx = w / 2, cy = h / 2;
    // Top, right, bottom, left triangles
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // Which side of the X?  Sign of (x - cx) and (y - cy) determines
            if (abs(x - cx) > abs(y - cy)) {
                // Left/right of X — check left triangle or right
                ImageDrawPixel(img, x, y, (x < cx) ? c1 : c2);
            } else {
                ImageDrawPixel(img, x, y, (y < cy) ? c3 : c4);
            }
        }
    // Draw the X stripes
    int bw = std::max(1, w / 14);
    for (int t = -bw/2; t <= bw/2; ++t) {
        for (int y = 0; y < h; ++y) {
            int xx1 = y + t;
            int xx2 = (h - y) + t;
            if (xx1 >= 0 && xx1 < w) ImageDrawPixel(img, xx1, y, c3);
            if (xx2 >= 0 && xx2 < w) ImageDrawPixel(img, xx2, y, c4);
        }
    }
}

void FlagRenderer::drawCanton(Image* img, int w, int h, const std::vector<Color>& colors) {
    // Colors[0] = flag background, colors[1] = canton background
    Color bg = getColor(colors, 0, DARKGRAY);
    Color canton = getColor(colors, 1, BLUE);
    fillImage(img, bg);
    int cw = w * 2 / 5, ch = h * 2 / 5;
    fillRect(img, 0, 0, cw, ch, canton);
}

void FlagRenderer::drawPale(Image* img, int w, int h, const std::vector<Color>& colors) {
    Color left = getColor(colors, 0, DARKGRAY);
    Color center = getColor(colors, 1, WHITE);
    Color right = getColor(colors, 2, left);
    int cw = w / 3;
    int rw = w - cw * 2;
    fillRect(img, 0, 0, cw, h, left);
    fillRect(img, cw, 0, cw, h, center);
    fillRect(img, cw * 2, 0, rw, h, right);
}

void FlagRenderer::drawFess(Image* img, int w, int h, const std::vector<Color>& colors) {
    Color top = getColor(colors, 0, DARKGRAY);
    Color center = getColor(colors, 1, WHITE);
    Color bot = getColor(colors, 2, top);
    int ch = h / 3;
    int rh = h - ch * 2;
    fillRect(img, 0, 0, w, ch, top);
    fillRect(img, 0, ch, w, ch, center);
    fillRect(img, 0, ch * 2, w, rh, bot);
}

void FlagRenderer::drawNordicCross(Image* img, int w, int h, const std::vector<Color>& colors) {
    Color bg = getColor(colors, 0, DARKGRAY);
    Color cross = getColor(colors, 1, WHITE);
    fillImage(img, bg);
    int vw = std::max(1, w / 8);        // vertical bar width
    int hh = std::max(1, h / 6);        // horizontal bar height
    int vx = w / 4;                     // offset to hoist
    int hy = h / 2 - hh / 2;
    fillRect(img, vx, 0, vw, h, cross);
    fillRect(img, 0, hy, w, hh, cross);
    // If 3+ colors, draw inner cross (like Norway/Sweden)
    if (colors.size() >= 3) {
        Color inner = getColor(colors, 2, RED);
        int ivw = std::max(1, vw / 3);
        int ihh = std::max(1, hh / 3);
        fillRect(img, vx + (vw - ivw) / 2, 0, ivw, h, inner);
        fillRect(img, 0, hy + (hh - ihh) / 2, w, ihh, inner);
    }
}

void FlagRenderer::drawGreekCross(Image* img, int w, int h, const std::vector<Color>& colors) {
    Color bg = getColor(colors, 0, DARKGRAY);
    Color cross = getColor(colors, 1, WHITE);
    fillImage(img, bg);
    int vw = std::max(1, w / 5);
    int hh = std::max(1, h / 5);
    int cx = w / 2 - vw / 2;
    int cy = h / 2 - hh / 2;
    fillRect(img, cx, 0, vw, h, cross);
    fillRect(img, 0, cy, w, hh, cross);
}

void FlagRenderer::drawBorder(Image* img, int w, int h, const std::vector<Color>& colors) {
    Color bg = getColor(colors, 0, DARKGRAY);
    Color border = getColor(colors, 1, WHITE);
    fillImage(img, bg);
    int bw = std::max(1, w / 20);
    fillRect(img, 0, 0, w, bw, border);
    fillRect(img, 0, h - bw, w, bw, border);
    fillRect(img, 0, 0, bw, h, border);
    fillRect(img, w - bw, 0, bw, h, border);
}

void FlagRenderer::drawSunburst(Image* img, int w, int h, const std::vector<Color>& colors) {
    Color bg = getColor(colors, 0, DARKGRAY);
    fillImage(img, bg);

    // Wedges, not hairlines. The previous version walked each ray outwards
    // plotting a single pixel per radius step, so the rays stayed one pixel
    // wide while the gaps between them grew -- at flag size that is a flat
    // field with a few faint threads on it, not a sunburst. It also read only
    // two colours, so a three-colour sunburst silently lost its third.
    std::vector<Color> rays;
    for (size_t i = 1; i < colors.size(); ++i) rays.push_back(colors[i]);
    if (rays.empty()) rays.push_back(WHITE);

    const int   kWedges = 12;
    const float kTwoPi  = 6.28318530f;
    const float cx = w * 0.5f, cy = h * 0.5f;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float a = atan2f((float)y - cy, (float)x - cx);
            if (a < 0.0f) a += kTwoPi;
            int idx = (int)(a / kTwoPi * kWedges) % kWedges;
            if (rays.size() == 1) {
                // One ray colour: alternate it with the field, or the whole
                // flag would come out a single flat colour.
                if (idx & 1) continue;
                ImageDrawPixel(img, x, y, rays[0]);
            } else {
                ImageDrawPixel(img, x, y, rays[idx % rays.size()]);
            }
        }
    }
}

// ══════════════════════════════════════════════════════
//  SYMBOL DRAWING — STARS
// ══════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════
//  CRESCENT & SUN
// ══════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════
//  CROSSES
// ══════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════
//  GEAR, SWORDS, MOUNTAIN, DIAMOND
// ══════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════
//  MAIN RENDER ENTRY POINT
// ══════════════════════════════════════════════════════

void FlagRenderer::drawFlagBackground(Image* img, const FlagPattern& pattern, int width, int height) {
    switch (pattern.type) {
        case FlagType::SOLID:         drawSolid(img, width, height, pattern.colors); break;
        case FlagType::HSTRIPES_2:
        case FlagType::HSTRIPES_3:
        case FlagType::HSTRIPES_N:    drawStripesH(img, width, height, pattern.colors); break;
        case FlagType::VSTRIPES_2:
        case FlagType::VSTRIPES_3:
        case FlagType::VSTRIPES_N:    drawStripesV(img, width, height, pattern.colors); break;
        case FlagType::DIAGONAL_L:    drawDiagonal(img, width, height, false, pattern.colors); break;
        case FlagType::DIAGONAL_R:    drawDiagonal(img, width, height, true, pattern.colors); break;
        case FlagType::TRIANGLE:      drawTriangle(img, width, height, false, pattern.colors); break;
        case FlagType::TRIANGLE_DOUBLE: drawTriangle(img, width, height, true, pattern.colors); break;
        case FlagType::QUARTERED:     drawQuartered(img, width, height, pattern.colors); break;
        case FlagType::SALTIR:        drawSaltir(img, width, height, pattern.colors); break;
        case FlagType::CANTON:        drawCanton(img, width, height, pattern.colors); break;
        case FlagType::PALE:          drawPale(img, width, height, pattern.colors); break;
        case FlagType::FESS:          drawFess(img, width, height, pattern.colors); break;
        case FlagType::CROSS_NORDIC:  drawNordicCross(img, width, height, pattern.colors); break;
        case FlagType::CROSS_GREEK:   drawGreekCross(img, width, height, pattern.colors); break;
        case FlagType::STRIPED_EDGE:  drawBorder(img, width, height, pattern.colors); break;
        case FlagType::SUNBURST:      drawSunburst(img, width, height, pattern.colors); break;
    }
}

// ══════════════════════════════════════════════════════
//  Arrangements
// ══════════════════════════════════════════════════════

// How far a whole arrangement reaches from its centre, so that the placement
// search measures -- and the flag bounds clamp -- the GROUP rather than the one
// emblem at the middle of it.
// Every constant below is paired with one in layoutInstances(); they are the
// same geometry read two ways, and changing one without the other either clips
// the arrangement at the flag's edge or reserves space it does not use.
namespace {
constexpr float ROW_R      = 0.45f, ROW_STEP  = 2.30f;   // step is in units of ROW_R
constexpr float ARC_R      = 0.30f, ARC_RAD   = 0.90f, ARC_SPAN = 1.80f;
constexpr float CIRCLE_R   = 0.32f, CIRCLE_RAD = 0.72f;
constexpr float ESC_MAIN_R = 0.78f, ESC_SAT_R = 0.20f, ESC_RAD = 1.25f, ESC_SPAN = 1.50f;
}  // namespace

int FlagRenderer::groupExtent(const FlagSymbol& sym, int r) {
    const int n = std::max(1, sym.count);
    switch (sym.layout) {
        case SymbolLayout::ROW:
            return (int)(r * (ROW_R * ROW_STEP * (n - 1) * 0.5f + ROW_R)) + 1;
        case SymbolLayout::ARC:      return (int)(r * (ARC_RAD + ARC_R)) + 1;
        case SymbolLayout::CIRCLE:   return (int)(r * (CIRCLE_RAD + CIRCLE_R)) + 1;
        case SymbolLayout::GRID:     return (int)(r * 1.10f) + 1;
        case SymbolLayout::ESCORTED: return (int)(r * (ESC_RAD + ESC_SAT_R)) + 1;
        default:                     return r;
    }
}

void FlagRenderer::layoutInstances(const FlagSymbol& sym, int cx, int cy, int r,
                                    std::vector<SymbolInstance>& out) {
    const int n = std::max(1, sym.count);
    switch (sym.layout) {
        case SymbolLayout::SINGLE:
            out.push_back({sym.type, cx, cy, r});
            break;

        case SymbolLayout::ROW: {
            const int rs = std::max(2, (int)(r * ROW_R));
            const int step = (int)(rs * ROW_STEP);
            const int x0 = cx - step * (n - 1) / 2;
            for (int i = 0; i < n; ++i) out.push_back({sym.type, x0 + step * i, cy, rs});
            break;
        }
        case SymbolLayout::ARC: {
            // A shallow arc of the kind that sits above a charge on a great
            // many real flags. The stars are small and the arc is wide: at the
            // first sizes tried they were half the primary's radius on a
            // three-quarter-radius arc, which put their centres closer together
            // than their own diameters and fused them into one spiked blob.
            const int rs = std::max(2, (int)(r * ARC_R));
            const float radius = r * ARC_RAD;
            const float start = -1.5708f - ARC_SPAN * 0.5f;   // centred on straight up
            for (int i = 0; i < n; ++i) {
                const float t = (n == 1) ? 0.5f : (float)i / (float)(n - 1);
                const float a = start + ARC_SPAN * t;
                out.push_back({sym.type,
                               cx + (int)(std::cos(a) * radius),
                               cy + (int)(std::sin(a) * radius),
                               rs});
            }
            break;
        }
        case SymbolLayout::CIRCLE: {
            const int rs = std::max(2, (int)(r * CIRCLE_R));
            const float radius = r * CIRCLE_RAD;
            for (int i = 0; i < n; ++i) {
                const float a = -1.5708f + 6.28319f * (float)i / (float)n;
                out.push_back({sym.type,
                               cx + (int)(std::cos(a) * radius),
                               cy + (int)(std::sin(a) * radius),
                               rs});
            }
            break;
        }
        case SymbolLayout::GRID: {
            // Rows and columns, as wide as it is tall or one wider, which is
            // how a canton full of stars is actually laid out.
            int cols = 1;
            while (cols * cols < n) ++cols;
            const int rows = (n + cols - 1) / cols;
            const float span = r * 2.0f;
            const float stepX = span / (float)(cols + 1);
            const float stepY = span / (float)(rows + 1);
            const int rs = std::max(2, (int)(std::min(stepX, stepY) * 0.46f));
            for (int i = 0; i < n; ++i) {
                const int col = i % cols, row = i / cols;
                out.push_back({sym.type,
                               cx - (int)(span * 0.5f) + (int)(stepX * (col + 1)),
                               cy - (int)(span * 0.5f) + (int)(stepY * (row + 1)),
                               rs});
            }
            break;
        }
        case SymbolLayout::ESCORTED: {
            // One charge with a train of small stars beside it. The satellites
            // are always five-pointed stars whatever the primary is, which is
            // the convention every flag using this arrangement follows.
            out.push_back({sym.type, cx, cy, (int)(r * ESC_MAIN_R)});
            const int rs = std::max(2, (int)(r * ESC_SAT_R));
            const float radius = r * ESC_RAD;
            const float start = -1.25f;
            for (int i = 0; i < n; ++i) {
                const float t = (n == 1) ? 0.5f : (float)i / (float)(n - 1);
                const float a = start + ESC_SPAN * t;
                out.push_back({SymbolType::STAR_5,
                               cx + (int)(std::cos(a) * radius),
                               cy + (int)(std::sin(a) * radius),
                               rs});
            }
            break;
        }
    }
}

void FlagRenderer::drawSymbol(Image* img, const FlagSymbol& sym, int width, int height, const std::string& baseDir,
                               const std::unordered_map<std::string, std::string>* odmData) {
    int cx = (int)(sym.x * width);
    int cy = (int)(sym.y * height);
    int r = (int)(sym.size * std::min(width, height));
    if (r < 1) r = 1;

    Color sc = getColor(sym.colors, 0, WHITE);

    // ── The two legacy "several stars" types, in the terms that now exist ──
    //
    // STARS_CIRCLE and STARS_GRID predate SymbolLayout and had neither an SVG
    // nor a procedural fallback, so a flag asking for either drew nothing at
    // all. They are an arrangement of stars, which is exactly what a layout is.
    FlagSymbol effective = sym;
    if (sym.type == SymbolType::STARS_CIRCLE || sym.type == SymbolType::STARS_GRID) {
        effective.type   = SymbolType::STAR_5;
        effective.layout = (sym.type == SymbolType::STARS_CIRCLE) ? SymbolLayout::CIRCLE
                                                                  : SymbolLayout::GRID;
        if (effective.count <= 0) effective.count = 12;
    }
    const FlagSymbol& s = effective;

    // Where the flag actually is on this canvas. Everything below reasons in
    // these bounds; see FlagBounds.
    const FlagBounds b = flagBounds(*img);
    if (s.autoPlace) { cx = b.x + (int)(s.x * b.w); cy = b.y + (int)(s.y * b.h); }
    if (s.autoPlace || s.field != EmblemField::NONE)
        r = (int)(s.size * std::min(b.w, b.h));
    if (r < 1) r = 1;

    // ── Generated emblems: find somewhere to put it, and make it readable ──
    //
    // All three of these are opt-in, so an authored flag is drawn exactly where
    // and how its author said. Only PoliticalIdentity::applyFlag sets them.
    int extent = groupExtent(s, r);
    float placementScore = 0.0f;
    if (s.autoPlace) {
        // Try the arrangement at full size, then smaller, and keep the LARGEST
        // that finds somewhere good. A flag can have a perfectly good home for
        // an emblem and no room for a five-star escort at the same size --
        // Poland's red band is 55 pixels tall and the full group is 96 -- and
        // shrinking to fit is a better answer there than covering the flag with
        // a field it did not need. The field is for flags where NO size works.
        // Not far, though. Shrinking to 0.60 let an emblem squeeze between two
        // American stripes and score well, which is why half the United States
        // plates ended up with a small charge sitting on the flag instead of
        // the field they needed: an emblem small enough to fit anywhere is an
        // emblem too small to be the country's. A Nordic cross divides its flag
        // into four quarters barely fifty pixels tall, though, and an escorted
        // arrangement at 0.70 still straddles the arms; 0.58 fits inside one,
        // which is where a cross flag has always carried a badge.
        const float SCALES[] = {1.00f, 0.85f, 0.70f, 0.58f};
        bool have = false;
        int bestR = r, bestX = cx, bestY = cy, bestExtent = extent;
        for (float scale : SCALES) {
            const int rr = std::max(4, (int)(r * scale));
            const int ex = groupExtent(s, rr);
            int px = cx, py = cy;
            const float q = placeSymbol(*img, s, b, ex, sc, &px, &py);
            if (!have || q < placementScore) {
                placementScore = q; bestR = rr; bestX = px; bestY = py; bestExtent = ex;
                have = true;
            }
            if (q <= EMBLEM_FIELD_THRESHOLD) break;   // good enough, and the biggest that is
        }
        r = bestR; cx = bestX; cy = bestY; extent = bestExtent;
    }

    // ── ...and where there is nowhere for it to go, give it somewhere ──
    //
    // The United States is stripes edge to edge with a union full of stars, and
    // the Union Jack is crossed into every corner: the search returns the least
    // bad square on a flag that has no good one, and an emblem there sits ON
    // the flag rather than in it. A plain field of its own is what a revolution
    // marking an old flag has always resorted to, and it is only drawn when the
    // flag leaves no alternative -- Poland, Japan and Sweden never see one.
    EmblemField field = s.field;
    if (field == EmblemField::AUTO_CANTON || field == EmblemField::AUTO_HOIST) {
        const bool needed = s.autoPlace && placementScore > EMBLEM_FIELD_THRESHOLD;
        field = !needed ? EmblemField::NONE
                        : (s.field == EmblemField::AUTO_HOIST ? EmblemField::HOIST
                                                                : EmblemField::CANTON);
    }
    if (field == EmblemField::CANTON || field == EmblemField::HOIST) {
        // 0.42 for both, not a narrower band for the hoist. An American canton
        // reaches 0.40 across, so a 0.30 band left a strip of the old union
        // standing beside the new one -- which reads as a rendering fault
        // rather than as a flag. A device's field has to finish what it starts.
        // Sized and positioned in the FLAG's bounds, and painted only where
        // there is flag under it.
        //
        // Both halves of that matter and neither was true. In canvas terms the
        // Swiss flag is the left half of its slot, so a field 0.42 of the CANVAS
        // wide was 0.84 of the flag and swallowed the cross whole; and Nepal is
        // a pennon, so a rectangle drawn over it filled in the notch and the
        // diagonal and left a plain red rectangle where the only
        // non-quadrilateral flag in the world had been.
        const int bandW   = (int)(b.w * 0.42f);
        const int cantonH = (int)(b.h * 0.55f);

        // WHICH corner, and whether a band at all.
        //
        // The hoist is where a device conventionally goes, so it is tried
        // first -- but a flag whose OWN device is at the hoist gets it cut in
        // half, and Turkey is exactly that: a full-height band over the left of
        // it covered the crescent and left the star stranded on the edge. So
        // every reasonable position is measured and the one covering the
        // PLAINEST part of the flag wins, with the conventional one favoured on
        // anything close. Purity is the right measure again: a region that is
        // one flat colour is the part of a flag carrying nothing.
        struct Slot { int x, y, w, h; float bias; };
        const bool wantBand = (field == EmblemField::HOIST);
        const Slot slots[] = {
            {b.x,                  b.y,                  bandW, b.h,     wantBand ? 0.00f : 0.07f},
            {b.x + b.w - bandW,    b.y,                  bandW, b.h,     wantBand ? 0.05f : 0.10f},
            {b.x,                  b.y,                  bandW, cantonH, wantBand ? 0.07f : 0.00f},
            {b.x + b.w - bandW,    b.y,                  bandW, cantonH, wantBand ? 0.10f : 0.05f},
            {b.x,                  b.y + b.h - cantonH,  bandW, cantonH, wantBand ? 0.10f : 0.06f},
        };
        const Slot* best = &slots[0];
        FieldStats under = measureRegion(*img, best->x, best->y, best->w, best->h);
        float bestCost = (1.0f - under.purity) + best->bias;
        for (const Slot& sl : slots) {
            const FieldStats st = measureRegion(*img, sl.x, sl.y, sl.w, sl.h);
            const float cost = (1.0f - st.purity) + sl.bias;
            if (cost < bestCost) { bestCost = cost; best = &sl; under = st; }
        }
        const int fx = best->x, fy = best->y, fw = best->w, fh = best->h;

        fillRectMasked(img, fx, fy, fw, fh, s.fieldColor);

        // Where it does not separate itself, fimbriate it -- the same answer as
        // for an emblem that will not show against its ground, and the reason
        // real flags outline a canton at all.
        if (std::fabs(luminanceOf(s.fieldColor) - under.mean) < 0.16f) {
            const Color edge = (luminanceOf(s.fieldColor) > 0.5f) ? Color{20, 22, 28, 255}
                                                                    : Color{245, 245, 242, 255};
            const int t = std::max(2, b.h / 48);
            // Only the edges that face the rest of the flag: an edge on the
            // flag's own border is a line drawn on nothing.
            if (fx > b.x)              fillRectMasked(img, fx, fy, t, fh, edge);
            if (fx + fw < b.x + b.w)   fillRectMasked(img, fx + fw - t, fy, t, fh, edge);
            if (fy > b.y)              fillRectMasked(img, fx, fy, fw, t, edge);
            if (fy + fh < b.y + b.h)   fillRectMasked(img, fx, fy + fh - t, fw, t, edge);
        }

        cx = fx + fw / 2;
        cy = fy + fh / 2;
        // Refit the arrangement to the field it now lives in.
        const int fit = (int)(std::min(fw, fh) * 0.46f);
        if (extent > fit) {
            r = std::max(2, r * fit / std::max(1, extent));
            extent = groupExtent(s, r);
        }
    }

    int outlineWidth = 0;
    Color outlineColor = BLANK;
    if (s.autoContrast) {
        // A white star on Poland's white half is not a subtle problem, it is an
        // invisible emblem. The fix is the one vexillology already uses for two
        // colours that will not sit next to each other: fimbriate it. The
        // emblem keeps the colour its identity gives it -- white for committed,
        // gold for radical -- and gains an outline in whichever extreme the
        // field is furthest from. Measured AFTER any field is drawn, so an
        // emblem on its own field is judged against that field.
        const FieldStats stats = measureField(*img, cx, cy, extent);
        const float symLum = luminanceOf(sc);
        if (std::fabs(symLum - stats.mean) < 0.30f || stats.variance > 0.02f) {
            outlineColor = (stats.mean > 0.5f) ? Color{20, 22, 28, 255}
                                               : Color{245, 245, 242, 255};
            // ...unless the outline would be the emblem's own colour, in which
            // case the pair reads as one shape and nothing has been gained.
            if (std::fabs(luminanceOf(outlineColor) - symLum) < 0.30f)
                outlineColor = (symLum > 0.5f) ? Color{20, 22, 28, 255}
                                               : Color{245, 245, 242, 255};
            outlineWidth = std::max(2, r / 9);
        }
    }

    std::vector<SymbolInstance> instances;
    layoutInstances(s, cx, cy, r, instances);
    for (const SymbolInstance& in : instances)
        drawOneSymbol(img, s, in.type, in.cx, in.cy, in.r, sc,
                      outlineWidth, outlineColor, baseDir, odmData);
}

void FlagRenderer::drawOneSymbol(Image* img, const FlagSymbol& sym, SymbolType type,
                                  int cx, int cy, int r, Color sc,
                                  int outlineWidth, Color outlineColor,
                                  const std::string& baseDir,
                                  const std::unordered_map<std::string, std::string>* odmData) {
    const int width = img->width, height = img->height;

    // The outline is sized from the arrangement, but a satellite star is a
    // fraction of the primary and would be swallowed by an outline scaled to
    // it. Every instance gets one proportional to itself.
    if (outlineWidth > 0) outlineWidth = std::max(1, std::min(outlineWidth, r / 5));

    // ── SVG redirect: map old procedural types to SVG_FILE ──
    SymbolType effectiveType = type;
    std::string svgPath;
    auto mapToSVG = [&](SymbolType t, const char* svgName) {
        if (effectiveType == t) {
            // Symbol SVGs live in <data>/symbols/ (data/flags/ holds country flags)
            svgPath = std::string("symbols/") + svgName;
            effectiveType = SymbolType::SVG_FILE;
        }
    };
    // Unconditionally, NOT `if (!baseDir.empty())`.
    //
    // An empty baseDir is not "no data directory", it is what Android uses on
    // purpose (Game.cpp: m_dataDir = "" so that "symbols/star5.svg" reaches the
    // APK asset through android_fopen). Gating the mapping on it meant every
    // symbol fell through to the procedural switch below, which has fallbacks
    // for five of the twenty-odd types, so on Android a flag's star simply did
    // not appear. rasterizeSVG already tries the archive and then the path, and
    // reports a miss once, so there is nothing for the guard to protect.
    {
        mapToSVG(SymbolType::STAR_5,        "star5.svg");
        mapToSVG(SymbolType::STAR_6,        "star6.svg");
        mapToSVG(SymbolType::STAR_7,        "star7.svg");
        mapToSVG(SymbolType::CRESCENT,      "crescent.svg");
        mapToSVG(SymbolType::CRESCENT_STAR, "crescent_star.svg");
        mapToSVG(SymbolType::SUN,           "sun.svg");
        mapToSVG(SymbolType::SUN_RAYS,      "sun_wavy.svg");
        mapToSVG(SymbolType::CROSS_LATIN,   "cross_latin.svg");
        mapToSVG(SymbolType::CROSS_SALTIR,  "cross_saltir.svg");
        mapToSVG(SymbolType::CROSS_MALTESE, "cross_maltese.svg");
        mapToSVG(SymbolType::DIAMOND,       "diamond.svg");
        mapToSVG(SymbolType::GEAR,          "gear.svg");
        mapToSVG(SymbolType::HAMMER_SICKLE, "hammer_sickle.svg");
        mapToSVG(SymbolType::SWASTIKA,      "swastika.svg");
        mapToSVG(SymbolType::SWORD,         "sword.svg");
        mapToSVG(SymbolType::CROSSED_SWORDS,"crossed_swords.svg");
        mapToSVG(SymbolType::MOUNTAIN,      "mountain.svg");
        mapToSVG(SymbolType::TREE,          "tree.svg");
        // Ten SVGs shipped in data/symbols/ that no SymbolType could name, and
        // so could never appear on a flag. eagle_nazi.svg is deliberately still
        // not among them, for the same reason applyFlag will not generate a
        // swastika: an authored historical flag may carry one, nothing else
        // should be able to reach for it by accident.
        mapToSVG(SymbolType::ANCHOR,        "anchor.svg");
        mapToSVG(SymbolType::TORCH,         "torch.svg");
        mapToSVG(SymbolType::ROSE,          "rose.svg");
        mapToSVG(SymbolType::FASCES,        "fasces.svg");
        mapToSVG(SymbolType::CROSS_PATTEE,  "cross_pattee.svg");
        mapToSVG(SymbolType::STAR_4,        "star_4.svg");
        mapToSVG(SymbolType::STAR_OF_DAVID, "star_of_david.svg");
        mapToSVG(SymbolType::WREATH,         "wreath.svg");
        mapToSVG(SymbolType::HAMMER,         "hammer.svg");
        mapToSVG(SymbolType::LIGHTNING,      "lightning.svg");
        mapToSVG(SymbolType::SUN_SPLENDOUR,  "sun_splendour.svg");
    }

    switch (effectiveType) {
        case SymbolType::SVG_FILE: {
            const std::string& svgName = (type == SymbolType::SVG_FILE) ? sym.text : svgPath;
            if (!svgName.empty()) {
                                // Clamp position so symbol stays within flag bounds
                int halfRaster = r;  // raster is r*2, half is r
                if (cx - halfRaster < 0) cx = halfRaster;
                if (cy - halfRaster < 0) cy = halfRaster;
                if (cx + halfRaster > width) cx = width - halfRaster;
                if (cy + halfRaster > height) cy = height - halfRaster;
                if (cx < 0) cx = halfRaster;
                if (cy < 0) cy = halfRaster;
Image svgImg = rasterizeSVG(svgName, r * 2, r * 2, baseDir, odmData);
                if (svgImg.data != nullptr) {
                    // Recolor SVG to match symbol color
                    const Color targetColor = sc;
                    if (!(targetColor.r == 255 && targetColor.g == 255 && targetColor.b == 255)) {
                        Image recolored = recolorImage(svgImg, targetColor);
                        UnloadImage(svgImg);
                        svgImg = recolored;
                    }
                    // Composite onto flag at (cx, cy) centered
                    int hw = svgImg.width / 2;
                    int hh = svgImg.height / 2;
                    // Fimbriation first, as a dilation of the symbol's own
                    // silhouette: every opaque pixel stamps a disc of outline
                    // colour, and the symbol is then drawn over the middle of
                    // it. Following the shape rather than boxing it is what
                    // makes it read as part of the emblem.
                    if (outlineWidth > 0) {
                        for (int sy = 0; sy < svgImg.height; ++sy)
                            for (int sx = 0; sx < svgImg.width; ++sx) {
                                if (GetImageColor(svgImg, sx, sy).a < 100) continue;
                                fillCircle(img, cx + sx - hw, cy + sy - hh,
                                           outlineWidth, outlineColor);
                            }
                    }
                    for (int sy = 0; sy < svgImg.height; ++sy) {
                        for (int sx = 0; sx < svgImg.width; ++sx) {
                            Color pc = GetImageColor(svgImg, sx, sy);
                            if (pc.a > 0) {
                                int px = cx + sx - hw;
                                int py = cy + sy - hh;
                                if (px >= 0 && px < img->width && py >= 0 && py < img->height)
                                    ImageDrawPixel(img, px, py, pc);
                            }
                        }
                    }
                    UnloadImage(svgImg);
                }
            }
            break;
        }

        // ── Procedural fallbacks (for symbols without SVG) ──
        case SymbolType::CIRCLE:
            if (outlineWidth > 0) fillCircle(img, cx, cy, r + outlineWidth, outlineColor);
            fillCircle(img, cx, cy, r, sc);
            break;
        case SymbolType::DISC: {
            // An "outlined circle" is an annulus, and the flag shows through
            // the middle of it. It used to be a filled disc with a second disc
            // of DARKGRAY stamped in the centre, which on a flag carrying one
            // colour -- every generated emblem does -- drew a grey dot inside a
            // white blob and read as a rendering fault rather than a device.
            const int rInner = r * 3 / 5;
            if (sym.colors.size() > 1) {
                fillCircle(img, cx, cy, r, sc);
                fillCircle(img, cx, cy, rInner, sym.colors[1]);
                break;
            }
            if (outlineWidth > 0) {
                fillRing(img, cx, cy, r + outlineWidth, rInner - outlineWidth, outlineColor);
            }
            fillRing(img, cx, cy, r, rInner, sc);
            break;
        }
        case SymbolType::TRIANGLE: {
            int hh = r, hw = r;
            if (outlineWidth > 0) {
                const int o = outlineWidth;
                fillTriangle(img, cx, cy - hh - o * 2, cx - hw - o, cy + hh + o,
                             cx + hw + o, cy + hh + o, outlineColor);
            }
            fillTriangle(img, cx, cy - hh, cx - hw, cy + hh, cx + hw, cy + hh, sc);
            break;
        }
        case SymbolType::TEXT_BLOCK:
            drawTextBlock(img, cx, cy, r * 2, r, sc);
            break;
        case SymbolType::CENSOR_BAR:
            drawCensorBar(img, cx, cy, r);
            break;
        default: break;
    }
}

Texture2D FlagRenderer::render(const FlagPattern& pattern, int width, int height, const std::string& baseDir,
                                 const std::unordered_map<std::string, std::string>* odmData) {
    if (!pattern.imagePath.empty()) {
        std::string fileExt;
        std::string::size_type dot = pattern.imagePath.rfind('.');
        if (dot != std::string::npos)
            fileExt = pattern.imagePath.substr(dot);
        if (fileExt == ".svg") {
            Image svgImg = rasterizeSVG(pattern.imagePath, width, height, baseDir, odmData);
            if (svgImg.data != nullptr) {
                Image dst;
                if (svgImg.width != width || svgImg.height != height) {
                    dst = GenImageColor(width, height, BLANK);
                    for (int y = 0; y < height; ++y)
                        for (int x = 0; x < width; ++x) {
                            int sx = x * svgImg.width / width;
                            int sy = y * svgImg.height / height;
                            if (sx >= svgImg.width) sx = svgImg.width - 1;
                            if (sy >= svgImg.height) sy = svgImg.height - 1;
                            Color c = GetImageColor(svgImg, sx, sy);
                            if (c.a > 0) ImageDrawPixel(&dst, x, y, c);
                        }
                    UnloadImage(svgImg);
                } else {
                    dst = svgImg;
                }
                Texture2D tex = compose(&dst, pattern, width, height, baseDir, odmData);
                UnloadImage(dst);
                return tex;
            }
        } else {
            std::string fullPath = baseDir.empty() ? pattern.imagePath : baseDir + "/" + pattern.imagePath;
            Image src = {};
            // Try loading from archive first
            if (odmData) {
                auto it = odmData->find(pattern.imagePath);
                if (it != odmData->end()) {
                    src = LoadImageFromMemory(fileExt.c_str(), 
                        reinterpret_cast<const unsigned char*>(it->second.data()), 
                        static_cast<int>(it->second.size()));
                }
            }
            // Fall back to disk if not in archive
            if (src.data == nullptr) {
                src = LoadImage(fullPath.c_str());
            }
            if (src.data != nullptr) {
                ImageResize(&src, width, height);
                Texture2D tex = compose(&src, pattern, width, height, baseDir, odmData);
                UnloadImage(src);
                return tex;
            }
        }
    }

    Image img = GenImageColor(width, height, BLANK);
    drawFlagBackground(&img, pattern, width, height);
    Texture2D tex = compose(&img, pattern, width, height, baseDir, odmData);
    UnloadImage(img);
    return tex;
}

/**
 * Everything that happens to a flag after its field is on the canvas, wherever
 * that canvas came from.
 *
 * The three branches of render() -- SVG, raster, generated pattern -- used to
 * each carry their own copy of this tail, which is how the SVG one came to
 * return early and skip symbols entirely while the other two drew them. There
 * is one copy now, and the order it imposes is the order the result depends on:
 * the recolour must not touch the emblem, and the contrast measurement behind
 * the emblem must see the recoloured flag.
 */
Texture2D FlagRenderer::compose(Image* img, const FlagPattern& pattern, int width, int height,
                                 const std::string& baseDir,
                                 const std::unordered_map<std::string, std::string>* odmData) {
    recolorFlag(img, pattern.recolor);

    // Symbols go ON TOP of a real flag image, they are not skipped by it. The
    // SVG branch used to return the loaded image untouched, so a pattern that
    // carried symbols alongside an imagePath silently lost them -- and since
    // almost every real country's flag IS an image, the political-identity
    // overlay drew on nobody. The British Empire became the "British Free
    // Republic" flying an unaltered Union Jack.
    for (const auto& sym : pattern.symbols)
        drawSymbol(img, sym, width, height, baseDir, odmData);

    if (pattern.censored) imageBlur(img, 4);
    return LoadTextureFromImage(*img);
}

// ══════════════════════════════════════════════════════
//  SVG rasterization with cache
// ══════════════════════════════════════════════════════

Image FlagRenderer::rasterizeSVG(const std::string& filePath, int width, int height, const std::string& baseDir,
                                  const std::unordered_map<std::string, std::string>* odmData) {
    std::string fullPath = baseDir.empty() ? filePath : baseDir + "/" + filePath;
    // Keyed on the SIZE as well as the path.
    //
    // It was keyed on the path alone, which was survivable only while every
    // symbol on a flag was the same size: the first rasterisation won and every
    // later request for that file got it back at whatever size that was,
    // because the compositor in drawOneSymbol sizes itself from the image it is
    // given rather than the size it asked for. The moment one flag wanted a
    // star at two sizes -- a charge with a train of small stars beside it --
    // the small ones came back as full-size ones and the arrangement fused into
    // a single blob.
    const std::string cacheKey = fullPath + "@" + std::to_string(width) + "x" + std::to_string(height);
    static std::unordered_map<std::string, Image> s_cache;
    // Sizes are chosen per flag now, so the key space is no longer tiny. This
    // is a cache, not a registry: when it stops being cheap, drop it.
    if (s_cache.size() > 512) {
        for (auto& e : s_cache) UnloadImage(e.second);
        s_cache.clear();
    }
    auto cit = s_cache.find(cacheKey);
    if (cit != s_cache.end()) {
        Image copy = GenImageColor(cit->second.width, cit->second.height, BLANK);
        for (int y = 0; y < cit->second.height; ++y)
            for (int x = 0; x < cit->second.width; ++x)
                ImageDrawPixel(&copy, x, y, GetImageColor(cit->second, x, y));
        return copy;
    }

    NSVGimage* svgImage = nullptr;
    if (odmData) {
        auto it = odmData->find(filePath);
        if (it != odmData->end()) {
            std::string copy = it->second;
            svgImage = nsvgParse(&copy[0], "px", 96.0f);
        }
        if (!svgImage) {
            // Try with "symbols/" prefix — SVGs may be stored there in the archive
            std::string symPath = "symbols/" + filePath;
            auto sit = odmData->find(symPath);
            if (sit != odmData->end()) {
                std::string copy = sit->second;
                svgImage = nsvgParse(&copy[0], "px", 96.0f);
            }
        }
    }
    if (!svgImage) {
        svgImage = nsvgParseFromFile(fullPath.c_str(), "px", 96.0f);
    }
    if (!svgImage) {
        // Once per flag per load is fine; once per REBEL flag on a training
        // run that creates hundreds of them is not, and the message is the
        // same every time.
        static int reported = 0;
        if (reported < 20) { printf("[SVG] Failed to parse: %s\n", fullPath.c_str()); ++reported; }
        return {0};
    }

    int w = width;
    int h = height;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    unsigned char* rgba = (unsigned char*)malloc(w * h * 4);
    if (!rgba) { nsvgDelete(svgImage); return {0}; }
    memset(rgba, 0, w * h * 4);

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) { free(rgba); nsvgDelete(svgImage); return {0}; }
    // Scale-to-fit: preserve aspect ratio, center within (w, h) with transparent padding.
    // This fixes clipping for non-2:1 SVGs (e.g., 3:2, 1:1, Nepal 5:6) that nanosvg would
    // otherwise clip vertically when using width-only scaling.
    float sx = (float)w / svgImage->width;
    float sy = (float)h / svgImage->height;
    float scale = sx < sy ? sx : sy;
    float tx = ((float)w - svgImage->width * scale) * 0.5f;
    float ty = ((float)h - svgImage->height * scale) * 0.5f;
    nsvgRasterize(rast, svgImage, tx, ty, scale, rgba, w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(svgImage);

    Image img = GenImageColor(w, h, BLANK);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int idx = (y * w + x) * 4;
            unsigned char rr = rgba[idx], gg = rgba[idx+1], bb = rgba[idx+2], aa = rgba[idx+3];
            ImageDrawPixel(&img, x, y, {rr, gg, bb, aa});
        }
    }
    free(rgba);

    Image cached = ImageCopy(img);
    s_cache[cacheKey] = cached;

    return img;
}

Image FlagRenderer::recolorImage(const Image& src, Color newColor) {
    Image dst = GenImageColor(src.width, src.height, BLANK);
    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            Color c = GetImageColor(src, x, y);
            if (c.a > 0) {
                // Preserve luminance for shading detail, replace hue with newColor
                float luma = (c.r * 0.2126f + c.g * 0.7152f + c.b * 0.0722f) / 255.0f;
                unsigned char nr = (unsigned char)std::min(255.0f, newColor.r * luma);
                unsigned char ng = (unsigned char)std::min(255.0f, newColor.g * luma);
                unsigned char nb = (unsigned char)std::min(255.0f, newColor.b * luma);
                ImageDrawPixel(&dst, x, y, {nr, ng, nb, c.a});
            }
        }
    }
    return dst;
}
