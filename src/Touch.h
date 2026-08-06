#pragma once
#include "raylib.h"

/**
 * Touchscreen input, for a game whose every screen was written for a mouse.
 *
 * THE SAME TRICK odPad USES, for the same reason. Two hundred and thirty call
 * sites ask raylib whether a mouse button is down, and the position they test
 * comes from Game::getMouse(). Rewriting those into abstract gestures is months
 * of work, so touch does not get its own path through the UI either: it drives
 * the SAME virtual cursor and the SAME buttons, and the screens never learn
 * that anything changed. See PadInput.h for the shims.
 *
 * WHY A CURSOR AND NOT DIRECT TAPS. Tapping where you want to press is the
 * obvious design and it breaks this game. Half the interface is HOVER-driven --
 * the ship action overlay computes its target from wherever the pointer rests,
 * tooltips appear under it, the artillery wheel previews from it -- and a
 * finger that is only present at the instant of contact has no hover state at
 * all. A cursor that persists between touches keeps every one of those working,
 * and it has the side benefit that your finger is not covering the thing you
 * are aiming at, which on a province two millimetres wide matters.
 *
 * So the gestures are trackpad-shaped rather than touchscreen-shaped:
 *
 *   drag one finger   moves the cursor, at a gain, so crossing the screen does
 *                     not need a screen-wide swipe
 *   tap               left click, where the cursor already is
 *   long press        right click, same place -- the context menus have no
 *                     other way in
 *   pinch two fingers the mouse wheel, which is map zoom at 42 call sites
 *
 * WHO IS DRIVING. On desktop this arbitrates with the mouse exactly as odPad
 * does. On Android it must go further: raylib's own core maps touch point zero
 * onto the mouse, so GetMousePosition() and IsMouseButtonDown(LEFT) already
 * follow the finger. Left as an OR, the real mouse and this cursor would fight
 * over every frame -- the finger would press one thing and the cursor another.
 * While touch is driving, the shims therefore IGNORE raylib's mouse rather than
 * OR with it, which is what suppressesMouse() is for.
 */
namespace odTouch {

/** Poll the screen. Call once per frame, before anything reads input. */
void update(float dt, int screenW, int screenH);

/** Whether touch is currently the thing driving the cursor. */
bool active();

/**
 * Whether raylib's own mouse must be ignored this frame.
 *
 * True whenever touch is driving, because on Android raylib is already
 * synthesising mouse events from the same finger -- see the note above.
 */
bool suppressesMouse();

/** Where the virtual cursor is, in screen pixels. */
Vector2 cursor();

/** Put the cursor somewhere (used when a screen opens, so it starts sensibly). */
void placeCursor(Vector2 p);

/** This frame's contribution to a mouse button, in raylib's vocabulary. */
bool mouseDown(int button);
bool mousePressed(int button);
bool mouseReleased(int button);

/** Wheel notches this frame, from a pinch. */
float wheel();

/** Whether the device has reported any touch at all, for the settings screen. */
bool present();

}  // namespace odTouch
