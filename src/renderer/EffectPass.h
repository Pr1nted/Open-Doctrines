#pragma once

#include "raylib.h"

#include "../ModEffectLayer.h"

/**
 * The one shader that draws every mod screen effect.
 *
 * ONE PASS, HOST-WRITTEN, MEASURED ONCE. A per-pixel overlay shader doing four
 * id lookups and eight table lookups once cut this game from ~1000 fps to ~350
 * (2026-09-21), which is the reason effects are a fixed list with parameters
 * rather than something a mod supplies: somebody writes the pass once, somebody
 * measures it once, and every mod gets the measured one.
 *
 * Every effect is a function of the pixel under consideration and nothing else
 * -- no neighbour sampling, no second target, no read of the previous frame. So
 * it composes into a single fragment shader bound around draws that were going
 * to happen anyway, and costs no extra geometry, no render target and no
 * resolve.
 */
namespace odeffect {

/**
 * Bind the pass for a target, if that target is doing anything.
 *
 * `area` is the rectangle the effect is being applied over, in screen pixels:
 * it is what `x` and `y` mean to a vignette, and it is the only geometry the
 * shader is given. Returns false when there is nothing to draw, and in that
 * case nothing was bound and endPass() must NOT be called -- the caller draws
 * exactly as it did before effects existed, which is every session without an
 * effect mod.
 */
bool beginPass(const Layer& layer, Target target, Rectangle area);

/** Unbind. Only after a beginPass() that returned true. */
void endPass();

/**
 * Release the shader.
 *
 * Called on shutdown. The shader is compiled on first use rather than at
 * startup, so a build that never loads an effect mod never asks the driver for
 * one -- and a driver that refuses it leaves the game drawing normally instead
 * of failing to start.
 */
void unloadPass();

}  // namespace odeffect
