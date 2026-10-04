#pragma once

// Drawing a rig with no art on it.
//
// Separate from Rig.cpp because that file is deliberately free of raylib -- the
// parser and the solver are the parts with edge cases and they are tested
// without a window. This is the half that needs a screen.

#include "Rig.h"

#include "raylib.h"

#include <string>

namespace rig {

/**
 * The test view: every bone its own colour, every joint a dot.
 *
 * This is how a skeleton gets built and checked before anybody draws a
 * character to hang on it -- and afterwards, it is how you find out why an
 * elbow is in the wrong place. Hands draw as a box and a facing needle, heads
 * as a circle with blinking eyes, so the parts with no geometry of their own
 * are still visible.
 */
void drawDebug(const Rig& rig, Vector2 origin, float scale, bool labels,
               float alpha = 1.0f, bool flip = false);

/**
 * The character as a PERSON rather than as a diagram.
 *
 * drawDebug answers "is that elbow where I meant it". This answers "does that
 * look like the drawing", which is a different question and needs different
 * pictures: filled shapes, one ink colour, a coat, a hat, a face.
 *
 * There is no art here and none is loaded. Everything is built from geometry
 * the rig already has -- the coat is the two cloth chains with the space
 * between them filled in, so it flaps because the cloth solver made it flap;
 * the face rides the head bone's frame, so it tilts when the head does; the
 * hands are the same cards drawHandCard draws, in skin rather than in a bone
 * colour. What it needs from the file is Skeleton::look: about nine colours.
 *
 * A rig with no `look` block falls back to drawDebug, so this can be turned on
 * one character at a time.
 */
void drawSkin(const Rig& rig, Vector2 origin, float scale, float alpha = 1.0f,
              bool flip = false);

/**
 * One hand, on its own, from the wrist.
 *
 * Exposed because a hand is the part of the rig that cannot be judged in
 * place: it is thirty pixels across in a screenshot of the whole figure, it has
 * to read correctly at every facing rather than at the one the current pose
 * happens to hold, and both chiralities have to be checked against each other
 * or a left hand on a right arm goes unnoticed. tools/rig_preview.cpp draws the
 * whole matrix of class against facing from this, which is the only way to see
 * all of it at once. It is also where the art atlas will hook in.
 *
 * `worldAngleDeg` points wrist -> knuckles. `facing` is -1 back of the hand to
 * the viewer, 0 edge-on, +1 palm. `chir` is +1 for a right hand, -1 a left.
 */
void drawHandCard(Vector2 wrist, float worldAngleDeg, float size,
                  const std::string& cls, float facing, float chir,
                  Color col, float alpha = 1.0f, bool labels = false);

}  // namespace rig
