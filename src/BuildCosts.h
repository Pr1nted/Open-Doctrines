#pragma once

// What a building costs and how long it takes -- once, for everybody.
//
// WHY THIS FILE EXISTS
//
// These four tables lived in Game_Render.cpp, and a second, identical copy
// lived in AISystem.cpp under AI_ names. The note left on the originals says
// exactly what goes wrong with that -- "which is fine while it was the only
// caller and is exactly how a second caller ends up with its own slightly
// different copy" -- and then it happened anyway, one file over.
//
// The two copies never diverged in VALUE. What diverged was what was done with
// them: the panel multiplied every price by a research cost modifier and the AI
// did not, so with the industry tree finished a player built at half price and
// the AI paid full, forever, while its economy module learned from the result.
// A shared table does not prevent that on its own, which is why buildCostMod()
// and conscriptionCostMod() are here too: the modifier is part of the price, so
// it lives beside the price.
//
// THE RULE
//
// A cost is (table entry) x (modifier). Anything that charges for a build calls
// both, or it is charging a different game than the one next to it.

#include <algorithm>

/**
 * Industry price by the level being built, and turns to build it.
 *
 * Indexed by the TARGET level, so IND_COST[3] is what it costs to reach level
 * 3, and index 0 is unused padding to keep that reading true.
 */
inline constexpr int IND_COST[]   = {0, 1, 10, 15, 25, 50, 75, 100, 150, 200, 300};
inline constexpr int IND_TURNS[]  = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

/** Fortification, same indexing. Forts are quick and get expensive fast. */
inline constexpr int FORT_COST[]  = {0, 20, 30, 50, 100, 200};
inline constexpr int FORT_TURNS[] = {0, 1, 1, 1, 1, 1};

inline constexpr int IND_MAX_LEVEL  = (int)(sizeof(IND_COST) / sizeof(IND_COST[0])) - 1;
inline constexpr int FORT_MAX_LEVEL = (int)(sizeof(FORT_COST) / sizeof(FORT_COST[0])) - 1;
inline constexpr int PORT_MAX_LEVEL = 3;

/** A new port, or the next level of one, and how long it takes. */
inline constexpr float PORT_COST_PER_LEVEL = 60.0f;
inline constexpr int   PORT_TURNS = 3;

/** Warships: price and the port level needed to lay one down. */
inline constexpr float DESTROYER_COST = 15.0f;
inline constexpr float CARRIER_COST   = 40.0f;
inline constexpr int   DESTROYER_PORT = 2;
inline constexpr int   CARRIER_PORT   = 3;
inline constexpr int   SHIP_TURNS     = 3;

/** Specialising a province costs this much of its CURRENT industry level. */
inline constexpr float SPECIALIZE_COST_MULT = 1.5f;
inline constexpr int   SPECIALIZE_TURNS = 3;

/**
 * Research discount on anything built with industry, as a multiplier.
 *
 * `effectPct` is what getTotalEffect("industryCostPct") returns. A DISCOUNT is
 * registered as a POSITIVE number -- "industry cost -50%" is stored as 50 --
 * so this subtracts, and the floor at zero stops a future stack of effects
 * going past free into negative prices.
 */
inline float buildCostMod(float effectPct) {
    return std::max(0.0f, 1.0f - effectPct / 100.0f);
}

/**
 * The same, for recruitment, from getTotalEffect("conscriptionCostPct").
 *
 * A separate function only so the call site names which tree it is reading.
 * The effect name matters more than it looks: "armyCostPct" is not a real one,
 * and code that asked for it got a silent zero and therefore no discount at
 * all -- which has now been written twice, in two different files.
 */
inline float conscriptionCostMod(float effectPct) { return buildCostMod(effectPct); }
