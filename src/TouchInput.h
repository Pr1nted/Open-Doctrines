#pragma once

/**
 * Sends the game's two text-input questions through the on-screen keyboard.
 *
 * HOW IT IS APPLIED. Force-included into every C++ translation unit of the
 * game by CMake, the same way src/DevInput.h is and for the same reason: the
 * game asks for typed characters in seventy-odd places across twenty files,
 * and each of them should keep reading input the way it always has. raylib.h
 * comes first so the declarations being renamed are already in scope.
 *
 * WHAT IT COSTS WHEN THERE IS NOTHING TO DO. One branch. `osk::charPressed`
 * and `osk::keyPressed` fall straight through to raylib when no key has been
 * tapped, and on a platform where the keyboard is switched off they do nothing
 * else at all.
 *
 * WHY ONLY TWO CALLS. Those two are what a text field reads: characters, and
 * backspace/enter. Everything else the keyboard needs -- where the finger is,
 * whether it lifted -- it asks raylib for itself, because it is drawing the
 * keys rather than pretending to be one.
 */
#include "raylib.h"
#include "TouchKeyboard.h"

#define GetCharPressed() ::osk::charPressed()
#define IsKeyPressed(k) ::osk::keyPressed(k)
