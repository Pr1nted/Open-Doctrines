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
 *   BeginDrawing      pushes a scale matrix, so every draw call after it is
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

inline void odBeginDrawingScaled() {
    BeginDrawing();
    const float s = odUi::scale();
    if (s != 1.0f) { rlPushMatrix(); rlScalef(s, s, 1.0f); }
}

inline void odEndDrawingScaled() {
    if (odUi::scale() != 1.0f) rlPopMatrix();
    EndDrawing();
}

// The logical canvas the game lays itself out against.
inline int odScreenW() { return (int)((float)GetScreenWidth()  / odUi::scale()); }
inline int odScreenH() { return (int)((float)GetScreenHeight() / odUi::scale()); }

#define BeginDrawing    odBeginDrawingScaled
#define EndDrawing      odEndDrawingScaled
#define GetScreenWidth  odScreenW
#define GetScreenHeight odScreenH
