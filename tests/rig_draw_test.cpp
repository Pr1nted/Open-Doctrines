// What the rig actually PUTS ON THE SCREEN.
//
// WHY THIS EXISTS, WHICH IS A DIFFERENT REASON FROM rig_test.cpp.
//
// RigTest checks the solver: where the bones are, what the poses interpolate
// to, how the cloth hangs. Every one of those is a number, and a number that
// goes wrong can be caught by comparing it to another number.
//
// The drawing fails in a way no number can see. A hand is built out of filled
// triangles, and raylib's DrawTriangle BACKFACE-CULLS: a triangle handed to it
// in the wrong winding order is discarded, silently, with no error and no
// warning. The palm's fill was wound the wrong way from the day it was
// written, so it was thrown away at every angle, every facing and both
// chiralities -- and the hand still drew, still posed, still solved, still
// passed every check in RigTest. It was simply hollow: a wire loop where the
// comment above it promised "a solid tapered mass". It survived because the
// only thing that ever looked at the drawing was a person looking at a
// screenshot, and a missing fill reads as a deliberate outline style.
//
// What it cost was the THUMB, which is what got reported. A thumb held palm-on
// comes to lie alongside the palm's edge -- it has nowhere else to go, because
// the axis it would cross in front on projects to exactly zero when the palm
// is square to the viewer. Alongside a solid mass that reads as a thumb;
// alongside a wire outline it reads as a loose stroke that has come off the
// hand. The bug was in the palm and the symptom was in the thumb, which is
// most of why it went unfound.
//
// The fingers went the same way, and for the same reason: the splay that fans
// them apart was signed backwards, so the index swung toward the little finger
// and the little finger toward the index until all four met at a point. The
// hand read as a flame. Nothing in RigTest could see it -- a converging fan is
// a perfectly good set of numbers -- and nothing here could either, until a
// person looked at the menu and said the hands were wrong.
//
// So the checks are the properties a person would otherwise have to look at a
// picture to confirm, made countable:
//
//   the hand has a BODY   -- filled pixels exist, at every orientation, and
//                            there are enough of them to be a mass
//   the fingers FAN       -- four of them are separately visible palm-on,
//                            rather than merging into one shape
//   the thumb REACHES     -- to about the index knuckle, so it reads as a
//                            digit rather than as a lump on the palm
//   the hand is ONE PIECE -- everything drawn is connected to everything else,
//                            so no part of it can drift off on its own
//
// All are read back off a rendered texture, so they test the pixels rather than
// the intent. None hardcodes a proportion from RigDraw: they are ratios and
// orderings, self-calibrated against the render, so re-tuning the hand does not
// break them and losing the fill still does.
//
// Every one of these was found by a person looking at the menu and saying the
// hands were wrong, after the checks that already existed had passed. That is
// the measure of how much of a drawing this kind of test actually covers.
//
// This one needs a GL context, unlike every other test in the suite. Where
// there is no display it says so and skips -- a check that cannot run is a
// skip, not a failure. tests/run_all.sh puts it under xvfb where it can.

#include "raylib.h"
#include "rig/RigDraw.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;
static void ok(bool c, const char* what) {
    g_checks++;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what);
    if (!c) g_failed++;
}
static void section(const char* s) { printf("\n=== %s ===\n", s); }

namespace {

constexpr int kCanvas = 340;

/// What one rendered hand is made of. Drawn white on black, so the weight a
/// pixel came out at says which part of the hand drew it: a stroke lands at
/// full brightness, a fill at its own alpha, and nothing at all stays black.
struct Shot {
    int body = 0;        ///< filled area -- the palm's mass and the thenar
    int strokes = 0;     ///< bones, outlines, joints: drawn at full weight
    int pieces = 0;      ///< connected components of everything lit
    /**
     * The most separate pieces of hand any one horizontal scanline crosses.
     *
     * Four fingers that fan apart are four crossings; four that converge to a
     * point are one. Counted over EVERY row, so it does not depend on which
     * way up the texture readback happens to come out -- which is a mistake
     * worth not making twice: a first pass at this scanned "the finger half"
     * of a bottom-up readback, measured the palm, and reported the bug and the
     * fix as identical.
     */
    int digits = 0;
};

Shot shoot(RenderTexture2D& rt, const char* cls, float facing, float chir,
           float size, float angle) {
    BeginTextureMode(rt);
    ClearBackground({0, 0, 0, 255});
    rig::drawHandCard({kCanvas * 0.5f, kCanvas * 0.5f}, angle, size, cls, facing,
                      chir, {255, 255, 255, 255}, 1.0f, /*labels=*/false);
    EndTextureMode();

    Image im = LoadImageFromTexture(rt.texture);
    const Color* p = (const Color*)im.data;
    const int n = kCanvas * kCanvas;

    Shot s;
    std::vector<char> lit(n, 0);
    for (int i = 0; i < n; ++i) {
        const int v = p[i].r;
        if (v < 40) continue;                  // background
        lit[i] = 1;
        if (v >= 200) ++s.strokes; else ++s.body;
    }
    UnloadImage(im);

    for (int y = 0; y < kCanvas; ++y) {
        int runs = 0;
        bool in = false;
        for (int x = 0; x < kCanvas; ++x) {
            const bool on = lit[y * kCanvas + x] != 0;
            if (on && !in) ++runs;
            in = on;
        }
        s.digits = std::max(s.digits, runs);
    }

    // Connected components, eight-way. A stroke drawn one pixel clear of the
    // rest of the hand is the whole failure being looked for, so diagonal
    // touching counts as joined -- anything less would report an antialiasing
    // artefact as a hand falling apart.
    std::vector<char> seen(n, 0);
    std::vector<int> stack;
    for (int i = 0; i < n; ++i) {
        if (seen[i] || !lit[i]) continue;
        stack.assign(1, i);
        seen[i] = 1;
        int area = 0;
        while (!stack.empty()) {
            const int c = stack.back(); stack.pop_back();
            ++area;
            const int cx = c % kCanvas, cy = c / kCanvas;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = cx + dx, ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= kCanvas || ny >= kCanvas) continue;
                    const int ni = ny * kCanvas + nx;
                    if (!seen[ni] && lit[ni]) { seen[ni] = 1; stack.push_back(ni); }
                }
        }
        // A handful of pixels is a corner, not a piece of hand.
        if (area > 12) ++s.pieces;
    }
    return s;
}

/**
 * How far up the hand the thumb reaches, as a fraction of the whole hand's
 * reach from the wrist. Zero means it never got far enough off the axis to be
 * told apart from the palm at all.
 *
 * A thumb tip lands about level with the INDEX KNUCKLE -- a little past half
 * way up the hand. One that stops short of that stops reading as a digit and
 * starts reading as a lump on the side of the palm, which is the state it was
 * reported in twice. It had been given a finger's length (0.46) for a chain
 * with one more bone in it, and the shortfall was invisible to every other
 * check here: the hand was still one piece, still filled, still had four
 * fingers that fanned.
 *
 * DRAWN POINTING +X, so the along-axis is x and a bottom-up readback cannot
 * turn the measurement upside down -- see the note on Shot::digits for why
 * that is worth being deliberate about.
 *
 * SELF-CALIBRATING: the palm's half-width is measured off the render near the
 * wrist rather than copied from RigDraw, so re-proportioning the hand moves
 * both sides of the comparison together. The 1.65 is the one judgement in it:
 * at its tip the thumb stands about 1.9 palm-halves off the axis where the
 * fanned index finger reaches about 1.4, so the threshold sits between them
 * and, being a ratio, holds at every size.
 */
float thumbReach(RenderTexture2D& rt, float facing, float chir, float size) {
    const int wx = 40;
    const int wy = kCanvas / 2;
    BeginTextureMode(rt);
    ClearBackground({0, 0, 0, 255});
    rig::drawHandCard({(float)wx, (float)wy}, 0.0f, size, "open", facing, chir,
                      {255, 255, 255, 255}, 1.0f, /*labels=*/false);
    EndTextureMode();

    Image im = LoadImageFromTexture(rt.texture);
    const Color* p = (const Color*)im.data;
    std::vector<int> ext(kCanvas, -1);
    int reach = 0;
    for (int x = 0; x < kCanvas; ++x) {
        int e = -1;
        for (int y = 0; y < kCanvas; ++y)
            if (p[y * kCanvas + x].r > 24) e = std::max(e, std::abs(y - wy));
        ext[x] = e;
        if (e >= 0 && x > wx) reach = x - wx;
    }
    UnloadImage(im);
    if (reach <= 0) return 0.0f;

    const int palmRef = ext[wx + std::max(1, reach / 10)];
    if (palmRef <= 0) return 0.0f;
    int thumb = 0;
    for (int a = 1; a < reach; ++a)
        if ((float)ext[wx + a] > palmRef * 1.65f) thumb = a;
    return (float)thumb / (float)reach;
}

}  // namespace

int main() {
    // raylib cannot open a window without a window server, and on Linux it
    // takes one down with a fatal log rather than a return value -- so this is
    // asked before anything is initialised rather than after.
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
        printf("  skip  no DISPLAY or WAYLAND_DISPLAY: nothing can be drawn here\n");
        printf("        (run it under xvfb-run to check the drawing on a headless box)\n");
        return 0;
    }
#endif

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(kCanvas, kCanvas, "rig drawing");
    if (!IsWindowReady()) {
        printf("  skip  no GL context here: the drawing cannot be checked\n");
        return 0;
    }
    RenderTexture2D rt = LoadRenderTexture(kCanvas, kCanvas);

    const char* CLASSES[] = {"relaxed", "open", "fist", "point"};

    section("the palm is a filled mass, not a wire outline");
    {
        // THE REGRESSION THIS FILE WAS WRITTEN FOR. With the palm's triangles
        // wound the way raylib discards, every one of these came back zero.
        int worstBody = 1 << 30;
        const char* worstAt = "";
        for (const char* cls : CLASSES)
            for (int fi = 0; fi <= 8; ++fi)
                for (float chir : {1.0f, -1.0f})
                    for (float ang : {-90.0f, -20.0f, 60.0f, 175.0f}) {
                        const float f = -1.0f + fi * 0.25f;
                        const Shot s = shoot(rt, cls, f, chir, 101.0f, ang);
                        if (s.body < worstBody) { worstBody = s.body; worstAt = cls; }
                    }
        printf("        the thinnest hand anywhere in the sweep has %d filled pixels (%s)\n",
               worstBody, worstAt);
        ok(worstBody > 0, "every hand has some body, at every facing and angle");

        // Not merely non-zero: a mass. Held square to the viewer a palm is the
        // biggest single area in the hand, so its fill is a good fraction of
        // everything drawn -- stated against the stroke count rather than a
        // pixel figure, so re-proportioning the hand does not break it.
        const Shot flat = shoot(rt, "open", 1.0f, 1.0f, 101.0f, -90.0f);
        printf("        palm-on: %d filled pixels against %d of stroke\n",
               flat.body, flat.strokes);
        ok(flat.body > flat.strokes / 4,
           "held palm-on the fill is a substantial part of the hand");
    }

    section("the body narrows as the hand turns edge-on");
    {
        // The palm keeps a little thickness when its width has projected away
        // -- see palmDeep -- so this is not a check that it vanishes. It is a
        // check that the fill is answering `facing` at all, which a fill drawn
        // at a constant size would not.
        const Shot square = shoot(rt, "open", 1.0f, 1.0f, 101.0f, -90.0f);
        const Shot edge = shoot(rt, "open", 0.0f, 1.0f, 101.0f, -90.0f);
        printf("        square-on %d filled, edge-on %d\n", square.body, edge.body);
        ok(edge.body > 0, "edge-on a palm still has its thickness");
        ok(square.body > edge.body * 3, "and square-on it is far broader than that");
    }

    section("an open hand's fingers fan apart");
    {
        // The splay was signed the wrong way round: the index swung across
        // toward the little finger and the little finger back toward the
        // index, so four fingers that left the knuckles 34 pixels apart
        // arrived 13 apart at the tips. They met at a point and the hand read
        // as a flame rather than as a hand -- at every facing, in every class,
        // for as long as there had been fingers.
        //
        // COUNTED, NOT MEASURED, because width is the wrong question. The
        // widest part of an open hand is across the thumb, and that hardly
        // moved: 62 pixels against 65. Nothing about the silhouette's extent
        // says whether you can see four fingers. Crossings do.
        int worst = 99;
        for (float size : {40.0f, 101.0f, 210.0f})
            for (float f : {-1.0f, 1.0f})
                for (float chir : {1.0f, -1.0f})
                    worst = std::min(worst, shoot(rt, "open", f, chir, size, -90.0f).digits);
        printf("        palm-on, the thinnest showing is %d separate digits across\n", worst);
        // Converging fingers never manage more than three, at any size. Four
        // is the fan; the fifth, where the thumb is crossed too, comes and goes
        // with the size and is not worth pinning.
        ok(worst >= 4, "four fingers are separately visible, not merged to a point");
    }

    section("the thumb reaches the index knuckle");
    {
        // It reached 70% of the way up the palm and read as a lump. Nothing
        // else in this file could see that: a short thumb is still connected,
        // still filled, and the fingers still fan.
        float worst = 1.0f;
        for (float size : {60.0f, 101.0f, 140.0f})
            for (float f : {-1.0f, 1.0f})
                for (float chir : {1.0f, -1.0f})
                    worst = std::min(worst, thumbReach(rt, f, chir, size));
        printf("        the thumb reaches %.0f%% of the way up the hand\n", worst * 100.0f);
        // A little over half is where the knuckle is. The length it was cut
        // back to never cleared the palm at all and measures a flat zero here,
        // so the margin either side of 45% is the whole of the defect.
        ok(worst > 0.45f, "the thumb is a digit's length, not a stub");
    }

    section("a hand is drawn in one piece");
    {
        // The reported symptom: a thumb that has come away from the hand. It
        // never actually had -- but nothing was watching, and the thumb is the
        // part most able to, being the only one that leaves the palm sideways.
        int broken = 0, checked = 0;
        std::string firstBad;
        for (const char* cls : CLASSES)
            for (int fi = 0; fi <= 8; ++fi)
                for (float chir : {1.0f, -1.0f})
                    for (float size : {40.0f, 101.0f})
                        for (float ang : {-90.0f, 20.0f, 130.0f}) {
                            const float f = -1.0f + fi * 0.25f;
                            const Shot s = shoot(rt, cls, f, chir, size, ang);
                            ++checked;
                            if (s.pieces > 1) {
                                ++broken;
                                if (firstBad.empty())
                                    firstBad = TextFormat("%s facing %+.2f %s size %.0f angle %.0f -> %d pieces",
                                                          cls, f, chir > 0 ? "R" : "L",
                                                          size, ang, s.pieces);
                            }
                        }
        printf("        %d hands drawn; %d came out in more than one piece\n",
               checked, broken);
        if (!firstBad.empty()) printf("        first: %s\n", firstBad.c_str());
        ok(broken == 0, "no hand falls into pieces, at any size or angle");
    }

    UnloadRenderTexture(rt);
    CloseWindow();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
