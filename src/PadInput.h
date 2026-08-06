#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Controller: the mouse, as far as every screen is concerned
// ─────────────────────────────────────────────────────────────────────────────
//
// The pad drives a virtual cursor and presses virtual buttons (see Gamepad.h).
// Rather than teach two hundred and thirty call sites about it -- across the
// map, the panels, the menus, the map editor -- raylib's four mouse queries are
// SHADOWED here, and the shadows OR the real mouse with the pad.
//
// A macro over a library function is a heavy hammer, and it is used because the
// alternative is worse: an edit to every call site is an edit that can be
// applied to 229 of them, and the one that is missed is a button that works
// with a mouse and not with a pad, found by a player rather than by a compiler.
// This way there is exactly one place where "is the button down" is answered,
// and it cannot drift.
//
// This lives in its own header rather than in GameInternals.h because the map
// renderer needs it and is not part of Game: it is a separate class with its own
// mouse call sites, and it is where a controller player does the actual aiming.
// Sharing the pointer but not the buttons is how the pad ended up able to hover
// a province and unable to select one.
//
// Only files that include this are covered. MapEditor.cpp has its own 91 call
// sites and its own class; it is mouse-only until it includes this too.

#include "raylib.h"
#include "Gamepad.h"
#include "Touch.h"
#include "UiScale.h"

// Touch REPLACES the real mouse rather than ORing with it. raylib's Android
// backend already synthesises mouse events from touch point zero, so leaving
// the real query in would have the finger pressing one thing while the virtual
// cursor sits on another. See odTouch::suppressesMouse.
inline bool odMouseDown(int b) {
    if (odTouch::suppressesMouse()) return odTouch::mouseDown(b);
    return IsMouseButtonDown(b) || odPad::mouseDown(b);
}
inline bool odMousePressed(int b) {
    if (odTouch::suppressesMouse()) return odTouch::mousePressed(b);
    return IsMouseButtonPressed(b) || odPad::mousePressed(b);
}
inline bool odMouseReleased(int b) {
    if (odTouch::suppressesMouse()) return odTouch::mouseReleased(b);
    return IsMouseButtonReleased(b) || odPad::mouseReleased(b);
}
inline bool odMouseUp(int b)       { return !odMouseDown(b); }
inline float odMouseWheel() {
    if (odTouch::suppressesMouse()) return odTouch::wheel();
    return GetMouseWheelMove() + odPad::wheel();
}

// The same treatment for the keyboard, and for the same reason: the artillery
// wheel, ship orders and box-select are keys HELD while the pointer aims, and a
// pad with no way to press them can move armies and nothing else.
inline bool odKeyDown(int k)    { return IsKeyDown(k)    || odPad::keyDown(k); }
inline bool odKeyPressed(int k) { return IsKeyPressed(k) || odPad::keyPressed(k); }

#define IsKeyDown             odKeyDown
#define IsKeyPressed          odKeyPressed
#define IsMouseButtonDown     odMouseDown
#define IsMouseButtonPressed  odMousePressed
#define IsMouseButtonReleased odMouseReleased
#define IsMouseButtonUp       odMouseUp
#define GetMouseWheelMove     odMouseWheel
