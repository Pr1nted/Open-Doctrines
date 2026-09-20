#pragma once

/**
 * Redirects the game's input calls, so the editor's view can be played in.
 *
 * HOW IT IS APPLIED. Not by including it: CMake force-includes this file into
 * every translation unit of the game when OD_DEV_INPUT is on, so no source
 * file has to know it exists and nothing has to be edited to turn it off. It
 * includes raylib.h FIRST and defines the macros after, so the declarations it
 * is renaming are already in scope -- the other order would rename raylib's
 * own prototypes and nothing would link.
 *
 * WHY MACROS, WHICH ARE NOT NICE. The game asks about input in roughly three
 * hundred places across forty files. Rewriting those call sites would be a far
 * bigger and more permanent change than a compile flag that can be turned off,
 * and an event queue has nowhere to deliver to when every reader calls raylib
 * directly. This is the smallest change that makes the view playable.
 *
 * WHAT IT COSTS WHEN ON. One branch per input question: `driving()` is false
 * unless an editor is actually sending, and every function falls straight
 * through to raylib when it is. A build with this on and nothing connected
 * behaves exactly like one without it.
 *
 * NOT IN A SHIPPING BUILD. OD_DEV_INPUT is off by default and the release
 * build does not set it.
 */
#include "raylib.h"
#include "DevLink.h"

#define IsKeyDown(k) ::devinput::keyDown(k)
#define IsKeyPressed(k) ::devinput::keyPressed(k)
#define IsKeyReleased(k) ::devinput::keyReleased(k)
#define IsMouseButtonDown(b) ::devinput::mouseDown(b)
#define IsMouseButtonPressed(b) ::devinput::mousePressed(b)
#define IsMouseButtonReleased(b) ::devinput::mouseReleased(b)
#define GetMousePosition() ::devinput::mousePosition()
#define GetMouseWheelMove() ::devinput::wheelMove()
#define GetCharPressed() ::devinput::charPressed()
