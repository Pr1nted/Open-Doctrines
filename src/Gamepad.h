#pragma once
#include "raylib.h"

/**
 * Controller input, for a game whose every screen was written for a mouse.
 *
 * THE PROBLEM THIS SOLVES. Two hundred and thirty call sites across nineteen
 * screens ask raylib `IsMouseButtonPressed/Released/Down`, and the position
 * they test against comes from `Game::getMouse()`. Rewriting those into an
 * abstract "activate" event is months of work and would touch every screen in
 * the game. So the controller does not get its own path through the UI: it
 * drives the SAME cursor and the SAME buttons, and the screens never learn that
 * anything changed.
 *
 * Two halves, per the chosen scheme:
 *
 *   a virtual cursor   The left stick moves a pointer, A presses whatever is
 *                      under it. This is what makes the map, the province
 *                      panel, the economy screen and the map editor usable at
 *                      all, none of which have any notion of a "current
 *                      widget" to move between.
 *   menu navigation    The main menu, the pause menu and settings already walk
 *                      their items with Up/Down and act on Enter. The d-pad is
 *                      fed into that existing path rather than a new one, so
 *                      those screens feel like a console game rather than a
 *                      pointer being dragged down a list.
 *
 * WHO IS DRIVING. A mouse and a pad must not fight over the cursor. The pad
 * takes it the moment a stick or button moves, and gives it back the moment the
 * real mouse does -- so a desktop player who bumps a controller does not lose
 * their pointer, and a controller player is not dragged around by a resting
 * mouse.
 */
namespace odPad {

/** Poll the pad. Call once per frame, before anything reads input. */
void update(float dt, int screenW, int screenH);

/** Whether the pad is currently the thing driving the cursor. */
bool active();

/** Where the virtual cursor is, in screen pixels. */
Vector2 cursor();

/** Put the cursor somewhere (used when a screen opens, so it starts sensibly). */
void placeCursor(Vector2 p);

/**
 * The pad's contribution to a mouse button, in raylib's own vocabulary.
 *
 * These are OR-ed with the real mouse by the shims in GameInternals.h; nothing
 * calls them directly.
 */
bool mouseDown(int button);
bool mousePressed(int button);
bool mouseReleased(int button);

/** Wheel notches this frame, from the right stick or the shoulder buttons. */
float wheel();

/** Menu walking: one event per press, repeating while held. */
bool navUp();
bool navDown();
bool navLeft();
bool navRight();
bool navActivate();      /**< A */
bool navBack();          /**< B */

/**
 * Tell the pad what the keyboard actions are bound to.
 *
 * Several mechanics are not mouse gestures at all -- the artillery wheel is a
 * key HELD over a province, ships move on another held key, box-select on a
 * third -- so a pad that only pushed a cursor and clicked could aim armies and
 * nothing else. The pad presses the ACTION, and looks up whichever key that
 * action currently carries, so a rebind moves the pad button with it.
 *
 * Call whenever config.keybinds changes.
 */
void setBindings(const int* keybinds, int count);

/** Whether a pad button is standing in for this keyboard key right now. */
bool keyDown(int key);
bool keyPressed(int key);

/** Whether a pad is plugged in at all, for the settings screen to report. */
bool present();

/** The pad's name, or nullptr. */
const char* name();

/**
 * The pad button standing in for an ACTION -- "Y", "LT" -- or nullptr.
 *
 * The keybinds screen asks rather than listing the mapping itself, so moving a
 * button in PAD_ACTIONS moves what the screen says with it. A settings page
 * that has to be edited alongside the thing it describes eventually describes
 * something else.
 */
const char* buttonName(int action);

}  // namespace odPad
