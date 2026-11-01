#pragma once
#include "raylib.h"

/**
 * High contrast: every colour the interface draws, pushed to the ends.
 *
 * THE PROBLEM. The interface has no theme. Its colours are written out where
 * each screen draws -- about 3,500 literals across 44 files -- so there is no
 * table to swap for a high-contrast one, and making one by hand would be an
 * edit to every screen in the game.
 *
 * THE TRICK IS THE ONE UiScale.h ALREADY USES. Rather than teach every call
 * site about a theme, raylib's drawing functions are shadowed here and the
 * colour is decided in one place, odHc() below. This header is force-included
 * into every C++ file of the game (CMakeLists.txt), so no screen has to know.
 * Text does not go through these shadows; it goes through odText::drawText
 * (src/i18n/Text.h), which applies the same transform.
 *
 * WHAT THE TRANSFORM DOES, measured against WCAG's contrast ratio:
 *
 *   greys        three levels. Dark (backgrounds) become black, light (text,
 *                outlines) become white -- 21:1 between them. Mid (button
 *                faces, hover states, separators) become a dark grey that is
 *                still a visible step up from black, so a hovered button
 *                stays distinguishable from the panel it sits on.
 *   colours      keep their hue -- war is still red and an alliance still
 *                green, so the colour-blind palettes compose with this --
 *                but are fully saturated and lifted until they read against
 *                black (relative luminance >= 0.30, about 7:1).
 *   translucency a dark panel that let the map show through becomes opaque,
 *                because map detail behind text is the contrast problem.
 *
 * IDEMPOTENT ON PURPOSE: text can reach raylib through both odText and the
 * shadows, and a colour transformed twice must come out the same as once.
 * tests/theme_test.cpp holds it to that, and to the ratios above.
 */
namespace odTheme {

inline bool g_highContrast = false;
inline bool highContrast() { return g_highContrast; }
inline void setHighContrast(bool on) { g_highContrast = on; }

namespace detail {
inline float lin(unsigned char c) {
    const float s = c / 255.0f;
    return s <= 0.04045f ? s / 12.92f : __builtin_powf((s + 0.055f) / 1.055f, 2.4f);
}
inline float luminance(Color c) {
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}
}  // namespace detail

/// The level a mid grey becomes: a step above black that still holds 7:1
/// against white text.
constexpr unsigned char kMid = 58;

/// One application of the transform, on or off regardless. hc() below runs
/// it to its fixed point.
inline Color hcOnce(Color c) {
    const int mx = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    const int mn = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
    const float sat = mx == 0 ? 0.0f : (float)(mx - mn) / (float)mx;
    const float L = detail::luminance(c);
    Color o = c;
    if (sat < 0.30f || mx < 70) {
        // A grey, or too dark for its hue to matter: three levels.
        unsigned char v = L < 0.03f ? 0 : (L < 0.22f ? kMid : 255);
        if (c.r == kMid && c.g == kMid && c.b == kMid) v = kMid;   // idempotent
        o.r = o.g = o.b = v;
        // A dark backdrop that let the map through: opaque now.
        if (v == 0 && c.a >= 96) o.a = 255;
        if (v == kMid && c.a >= 96) o.a = 255;
    } else {
        // A colour: full saturation at its hue, then lifted toward white
        // until it reads against black. The lift is the same affine step on
        // every channel, so the hue survives it and a second pass lands here
        // again.
        auto full = [&](int v) { return (unsigned char)((v - mn) * 255 / (mx - mn)); };
        Color s{full(c.r), full(c.g), full(c.b), c.a};
        auto lift = [](unsigned char v) { return (unsigned char)(v + (255 - v) / 4); };
        for (int i = 0; i < 16 && detail::luminance(s) < 0.30f; ++i) {
            s.r = lift(s.r); s.g = lift(s.g); s.b = lift(s.b);
        }
        o = s;
    }
    return o;
}

/// Largest fill (in both directions) still treated as a mark rather than a
/// backdrop: a legend swatch, a status dot, a flag chip.
constexpr float kMarkSize = 24.0f;

/// One application of the fill transform: greys as hcOnce; a colour keeps its
/// hue but is DARKENED, every channel scaled down together, until white text
/// on it holds 7:1 (relative luminance <= 0.10).
inline Color hcFillOnce(Color c) {
    const int mx = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    const int mn = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
    const float sat = mx == 0 ? 0.0f : (float)(mx - mn) / (float)mx;
    if (sat < 0.30f || mx < 70) return hcOnce(c);
    // Already a backdrop colour: fully saturated (no white in it) and dark
    // enough. Every output below is exactly that, so leaving such a colour
    // alone is what makes a second pass change nothing.
    if (mn == 0 && detail::luminance(c) <= 0.10f) {
        Color o = c;
        if (c.a >= 96) o.a = 255;
        return o;
    }
    Color s{(unsigned char)((c.r - mn) * 255 / (mx - mn)), (unsigned char)((c.g - mn) * 255 / (mx - mn)),
            (unsigned char)((c.b - mn) * 255 / (mx - mn)), c.a};
    for (int i = 0; i < 24 && detail::luminance(s) > 0.10f; ++i) {
        s.r = (unsigned char)(s.r * 7 / 8); s.g = (unsigned char)(s.g * 7 / 8); s.b = (unsigned char)(s.b * 7 / 8);
    }
    if (c.a >= 96) s.a = 255;          // a coloured backdrop is opaque too
    return s;
}

namespace detail {
/// Run `step` to the smallest member of the cycle it settles into; see hc().
template <class F> inline Color settle(Color c, F step) {
    auto key = [](Color x) {
        return ((unsigned)x.r << 24) | ((unsigned)x.g << 16) | ((unsigned)x.b << 8) | x.a;
    };
    unsigned seen[24];
    int n = 0;
    Color cur = step(c);
    for (;;) {
        const unsigned k = key(cur);
        for (int i = 0; i < n; ++i)
            if (seen[i] == k) {
                unsigned best = seen[i];
                for (int j = i; j < n; ++j) best = seen[j] < best ? seen[j] : best;
                return Color{(unsigned char)(best >> 24), (unsigned char)(best >> 16),
                             (unsigned char)(best >> 8), (unsigned char)best};
            }
        if (n == 24) return cur;
        seen[n++] = k;
        cur = step(cur);
    }
}
}  // namespace detail

/// The transform. Identity when high contrast is off.
///
/// Run to a FIXED POINT, not once: integer rounding in the lift means a
/// second pass can land a step or two away from the first (918 of 5,232
/// colours did, measured), and a few colours bounce between two values for
/// ever. So the step is repeated until a value comes round again, and the
/// answer is the smallest value of that cycle -- a fixed point is a cycle of
/// one. Starting from any member of the cycle finds the same cycle and the
/// same smallest member, which is what makes hc(hc(c)) == hc(c) exact.
inline Color hc(Color c) {
    if (!g_highContrast) return c;
    return detail::settle(c, hcOnce);
}

/**
 * The transform for a FILLED shape of w x h.
 *
 * A colour cannot say whether it is a backdrop or a mark, but the shape it is
 * drawn as can. A button face or a banner has text on it, so its colour is
 * darkened -- the first version lifted every colour, and Process Turn came
 * out bright green under white text, worse than before. A legend swatch or a
 * status dot has nothing on it and sits on a black panel, so it is lifted like
 * text, or it would vanish into the panel.
 */
inline Color hcFill(Color c, float w, float h) {
    if (!g_highContrast) return c;
    if (w <= kMarkSize && h <= kMarkSize) return hc(c);
    return hcFillOnce(c);
}

}  // namespace odTheme

// ── The shadows. Wrappers first, then the names that reach them. ──
// Global, and prefixed, so that `::DrawRectangle(...)` still compiles.
inline void odDrawRectangle(int x, int y, int w, int h, Color c) { ::DrawRectangle(x, y, w, h, odTheme::hcFill(c, (float)w, (float)h)); }
inline void odDrawRectangleRec(Rectangle r, Color c) { ::DrawRectangleRec(r, odTheme::hcFill(c, r.width, r.height)); }
inline void odDrawRectangleV(Vector2 p, Vector2 s, Color c) { ::DrawRectangleV(p, s, odTheme::hcFill(c, s.x, s.y)); }
inline void odDrawRectanglePro(Rectangle r, Vector2 o, float rot, Color c) { ::DrawRectanglePro(r, o, rot, odTheme::hcFill(c, r.width, r.height)); }
inline void odDrawRectangleLines(int x, int y, int w, int h, Color c) { ::DrawRectangleLines(x, y, w, h, odTheme::hc(c)); }
inline void odDrawRectangleLinesEx(Rectangle r, float t, Color c) { ::DrawRectangleLinesEx(r, t, odTheme::hc(c)); }
inline void odDrawRectangleRounded(Rectangle r, float rd, int seg, Color c) { ::DrawRectangleRounded(r, rd, seg, odTheme::hcFill(c, r.width, r.height)); }
inline void odDrawRectangleRoundedLines(Rectangle r, float rd, int seg, Color c) { ::DrawRectangleRoundedLines(r, rd, seg, odTheme::hc(c)); }
inline void odDrawRectangleRoundedLinesEx(Rectangle r, float rd, int seg, float t, Color c) { ::DrawRectangleRoundedLinesEx(r, rd, seg, t, odTheme::hc(c)); }
inline void odDrawRectangleGradientV(int x, int y, int w, int h, Color a, Color b) { ::DrawRectangleGradientV(x, y, w, h, odTheme::hcFill(a, (float)w, (float)h), odTheme::hcFill(b, (float)w, (float)h)); }
inline void odDrawRectangleGradientH(int x, int y, int w, int h, Color a, Color b) { ::DrawRectangleGradientH(x, y, w, h, odTheme::hcFill(a, (float)w, (float)h), odTheme::hcFill(b, (float)w, (float)h)); }
inline void odDrawLine(int x1, int y1, int x2, int y2, Color c) { ::DrawLine(x1, y1, x2, y2, odTheme::hc(c)); }
inline void odDrawLineV(Vector2 a, Vector2 b, Color c) { ::DrawLineV(a, b, odTheme::hc(c)); }
inline void odDrawLineEx(Vector2 a, Vector2 b, float t, Color c) { ::DrawLineEx(a, b, t, odTheme::hc(c)); }
inline void odDrawCircle(int x, int y, float r, Color c) { ::DrawCircle(x, y, r, odTheme::hcFill(c, 2 * r, 2 * r)); }
inline void odDrawCircleV(Vector2 p, float r, Color c) { ::DrawCircleV(p, r, odTheme::hcFill(c, 2 * r, 2 * r)); }
inline void odDrawCircleLines(int x, int y, float r, Color c) { ::DrawCircleLines(x, y, r, odTheme::hc(c)); }
inline void odDrawRing(Vector2 p, float ri, float ro, float a0, float a1, int seg, Color c) { ::DrawRing(p, ri, ro, a0, a1, seg, odTheme::hc(c)); }
inline void odDrawTriangle(Vector2 a, Vector2 b, Vector2 cc, Color c) { ::DrawTriangle(a, b, cc, odTheme::hc(c)); }
inline void odDrawTextEx(Font f, const char* t, Vector2 p, float s, float sp, Color c) { ::DrawTextEx(f, t, p, s, sp, odTheme::hc(c)); }
inline void odDrawTextPro(Font f, const char* t, Vector2 p, Vector2 o, float r, float s, float sp, Color c) { ::DrawTextPro(f, t, p, o, r, s, sp, odTheme::hc(c)); }
inline void odDrawTextCodepoint(Font f, int cp, Vector2 p, float s, Color c) { ::DrawTextCodepoint(f, cp, p, s, odTheme::hc(c)); }

#define DrawRectangle(...)              odDrawRectangle(__VA_ARGS__)
#define DrawRectangleRec(...)           odDrawRectangleRec(__VA_ARGS__)
#define DrawRectangleV(...)             odDrawRectangleV(__VA_ARGS__)
#define DrawRectanglePro(...)           odDrawRectanglePro(__VA_ARGS__)
#define DrawRectangleLines(...)         odDrawRectangleLines(__VA_ARGS__)
#define DrawRectangleLinesEx(...)       odDrawRectangleLinesEx(__VA_ARGS__)
#define DrawRectangleRounded(...)       odDrawRectangleRounded(__VA_ARGS__)
#define DrawRectangleRoundedLines(...)  odDrawRectangleRoundedLines(__VA_ARGS__)
#define DrawRectangleRoundedLinesEx(...) odDrawRectangleRoundedLinesEx(__VA_ARGS__)
#define DrawRectangleGradientV(...)     odDrawRectangleGradientV(__VA_ARGS__)
#define DrawRectangleGradientH(...)     odDrawRectangleGradientH(__VA_ARGS__)
#define DrawLine(...)                   odDrawLine(__VA_ARGS__)
#define DrawLineV(...)                  odDrawLineV(__VA_ARGS__)
#define DrawLineEx(...)                 odDrawLineEx(__VA_ARGS__)
#define DrawCircle(...)                 odDrawCircle(__VA_ARGS__)
#define DrawCircleV(...)                odDrawCircleV(__VA_ARGS__)
#define DrawCircleLines(...)            odDrawCircleLines(__VA_ARGS__)
#define DrawRing(...)                   odDrawRing(__VA_ARGS__)
#define DrawTriangle(...)               odDrawTriangle(__VA_ARGS__)
#define DrawTextEx(...)                 odDrawTextEx(__VA_ARGS__)
#define DrawTextPro(...)                odDrawTextPro(__VA_ARGS__)
#define DrawTextCodepoint(...)          odDrawTextCodepoint(__VA_ARGS__)
