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

inline bool odMouseDown(int b)     { return IsMouseButtonDown(b)     || odPad::mouseDown(b); }
inline bool odMousePressed(int b)  { return IsMouseButtonPressed(b)  || odPad::mousePressed(b); }
inline bool odMouseReleased(int b) { return IsMouseButtonReleased(b) || odPad::mouseReleased(b); }
inline bool odMouseUp(int b)       { return !odMouseDown(b); }
inline float odMouseWheel()        { return GetMouseWheelMove() + odPad::wheel(); }

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
