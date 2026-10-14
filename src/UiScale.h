#pragma once
#include "raylib.h"
#include "rlgl.h"

/**
 * One knob that makes the whole interface bigger, for screens held at arm's
 * length rather than sat at.
 *
 * THE PROBLEM. Every panel, button and label in this game is laid out in raw
 * pixels -- 1294 DrawText/MeasureText calls, panels at fixed offsets, 14 and 16
 * point type. On a 1920x1080 desktop window that is correct. On a phone with
 * the same pixel count across six inches it is unreadable, and there is no
 * per-site fix that is not 1294 edits.
 *
 * THE TRICK IS THE ONE PadInput.h ALREADY USES. Rather than teach every call
 * site about a scale factor, raylib's own functions are shadowed here and the
 * scale is applied in one place:
 *
 *   BeginDrawing      scales the projection, so every draw call after it is
 *                     magnified without knowing it
 *   GetScreenWidth    reports the LOGICAL size -- the physical size divided by
 *   GetScreenHeight   the scale -- so layout still fills the screen instead of
 *                     running off the right edge at 1.5x
 *
 * A screen that asks "how wide am I" and then draws to that width therefore
 * lays out in a smaller coordinate space that is then magnified, which is
 * exactly what is wanted and requires no edit to the screen.
 *
 * The cost is that text is magnified rather than re-rasterised, so it is
 * slightly soft. That is the standard trade for a mobile port of a desktop UI
 * and it is far better than type nobody can read.
 *
 * 1.0 everywhere except Android, so desktop and web are untouched.
 */
namespace odUi {

/** The active factor. 1.0 means every shadow below is a no-op. */
float scale();

/** Set it. Called once at startup from the platform's own reasoning. */
void setScale(float s);

}  // namespace odUi

/**
 * Where a pointer really is, in the space the game lays itself out in.
 *
 * `raw` is what the platform reports, `dpi` converts a window point to a
 * framebuffer pixel, and `ui` is the magnification the whole frame is drawn
 * through (odBeginDrawingScaled below). Dividing by `ui` is what nobody can
 * skip: EVERYTHING is drawn inside that matrix, the map included, so a
 * pointer left in physical pixels lands `ui` times away from what it is over.
 *
 * ONE FUNCTION because there were two copies and they disagreed: Game::getMouse
 * divided by the scale and MapRenderer::getMouse did not, so on a phone -- the
 * one platform where the scale is never 1 -- every panel saw the finger in one
 * place and the map saw it somewhere else, and a drag moved the camera by the
 * wrong distance.
 */
inline Vector2 odPointerIn(Vector2 raw, float dpi, float ui) {
    const float u = (ui > 0.0f) ? ui : 1.0f;
    return { raw.x * dpi / u, raw.y * dpi / u };
}

// ── THE SCALE LIVES IN THE PROJECTION ──
//
// It used to be one modelview matrix pushed at BeginDrawing, and that broke in
// two ways (GitHub issue #17):
//
//   - BeginMode2D/EndMode2D load identity into the modelview, so the first map
//     drawn in a frame wiped the scale: the map and every panel after it came
//     out unscaled in the top-left 1/scale of the window while the layout used
//     the logical (smaller) size.
//   - Pushing in modelview mode moves rlgl onto its CPU-side transform, which
//     DrawMesh multiplies into every model. The globe was scaled up around its
//     own centre until the camera sat inside it: stars and nothing else, at any
//     scale but 1 -- on Android, always.
//
// The projection has neither problem. 2D cameras only touch the modelview,
// BeginMode3D pushes the projection and EndMode3D pops it back, and a render
// texture sets its own unscaled one. Only EndTextureMode, which rebuilds the
// screen projection from scratch, has to have the scale put back. Scissor
// rectangles, which raylib takes in framebuffer pixels, are scaled the same
// way, except inside a render texture.
namespace odUi {
bool inTexture();
void setInTexture(bool on);
bool inFrame();
void setInFrame(bool on);
Matrix& screenProjection();  // the one BeginDrawing found, restored at EndDrawing
}

inline void odApplyScaleToProjection() {
    const float s = odUi::scale();
    if (s == 1.0f) return;
    rlMatrixMode(RL_PROJECTION);
    rlScalef(s, s, 1.0f);
    rlMatrixMode(RL_MODELVIEW);
}
inline void odBeginTextureModeScaled(RenderTexture2D t) {
    odUi::setInTexture(true);
    BeginTextureMode(t);
}
inline void odEndTextureModeScaled() {
    EndTextureMode();
    odUi::setInTexture(false);
    // A texture drawn before BeginDrawing must not leave the projection scaled
    // for BeginDrawing to save and scale a second time.
    if (odUi::inFrame()) odApplyScaleToProjection();
}
inline void odBeginScissorModeScaled(int x, int y, int w, int h) {
    const float s = odUi::inTexture() ? 1.0f : odUi::scale();
    if (s == 1.0f) { BeginScissorMode(x, y, w, h); return; }
    BeginScissorMode((int)(x * s), (int)(y * s), (int)(w * s + 0.5f), (int)(h * s + 0.5f));
}

inline void odBeginDrawingScaled() {
    BeginDrawing();
    odUi::setInFrame(true);
    if (odUi::scale() != 1.0f) {
        rlDrawRenderBatchActive();
        odUi::screenProjection() = rlGetMatrixProjection();
        odApplyScaleToProjection();
    }
}

inline void odEndDrawingScaled() {
    if (odUi::scale() != 1.0f) {
        // Flush under the scaled projection, then restore the saved one exactly
        // rather than dividing back out, which would drift a little each frame.
        rlDrawRenderBatchActive();
        rlSetMatrixProjection(odUi::screenProjection());
    }
    odUi::setInFrame(false);
    EndDrawing();
}

// The logical canvas the game lays itself out against.
inline int odScreenW() { return (int)((float)GetScreenWidth()  / odUi::scale()); }
inline int odScreenH() { return (int)((float)GetScreenHeight() / odUi::scale()); }

#define BeginDrawing    odBeginDrawingScaled
#define BeginTextureMode odBeginTextureModeScaled
#define EndTextureMode  odEndTextureModeScaled
#define BeginScissorMode odBeginScissorModeScaled
#define EndDrawing      odEndDrawingScaled
#define GetScreenWidth  odScreenW
#define GetScreenHeight odScreenH
