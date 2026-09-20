#pragma once

#include <string>
#include <vector>

/**
 * Industry in state hands, one resource speciality at a time.
 *
 * ── WHAT IT IS ──
 *
 * A country nationalises a SPECIALITY, not a province: "Oil is ours" applies to
 * every province of that country specialised in oil, and to every one it
 * specialises later. That is the unit the player already thinks in -- the
 * specialisation screen offers exactly these five -- and it keeps the state out
 * of per-province bookkeeping that a save would then have to carry.
 *
 * Nationalised industry is DEARER TO BUILD and DEARER TO RUN, and it PRODUCES
 * MORE. All three from one number.
 *
 * ── THE RAMP IS THE WHOLE MECHANIC ──
 *
 * Every speciality carries a ramp in [0,1] that climbs by 1/kRampTurns each
 * turn it is held and falls by the same each turn it is not. Every effect is
 * that ramp times a ceiling, so a nationalisation that has just been declared
 * does almost nothing, and one held for a generation does all of it.
 *
 * The three multipliers come from the SAME ramp on purpose. It is what stops
 * the obvious exploit: take the industry into state hands, build at the cheap
 * early rate, and collect the high output afterwards. You cannot -- the output
 * is only high once the ramp is high, and by then building is dear and, more to
 * the point, the UPKEEP is too. Cheaply-built nationalised industry still pays
 * the rising bill on everything it earns, so there is no window in which the
 * benefit is large and the cost is not.
 *
 * Releasing does not snap back either: the ramp decays at the rate it grew, so
 * a country that privatises to dodge the upkeep loses the output over the same
 * twenty turns. Flipping between the two is the one strategy this shape refuses
 * to reward.
 *
 * ── AND THE IDEOLOGY DECIDES HOW MUCH ──
 *
 * How many specialities a country may hold at once comes off the economic axis
 * of its compass, 5 at the far left and 0 at the far right. A liberal free
 * market cannot nationalise anything; a command economy can take all five. A
 * country that drifts right loses the room and has to let one go -- which the
 * ramp then unwinds rather than cancels.
 */
namespace odnat {

/// Turns to go from nothing to the full effect, and back.
inline constexpr int   kRampTurns  = 20;
/// The ceilings, every one reached only at ramp 1.0.
inline constexpr float kCostMax    = 0.60f;   ///< +60% to build
inline constexpr float kUpkeepMax  = 0.35f;   ///< +35% to run
inline constexpr float kOutputMax  = 0.45f;   ///< +45% out of the ground
inline constexpr float kUnrestMax  = 3.0f;    ///< percentage points, in those provinces

/**
 * How many specialities this country may hold, from the economic axis.
 *
 * -100 is the command end and +100 the market end, so this runs 5 down to 0.
 * Truncating rather than rounding, so the boundaries are where they look:
 * a centrist country at 0.0 holds two.
 */
int capFor(float economic);

/**
 * One turn of movement for one speciality's ramp.
 *
 * `held` is whether the country still has it in state hands. Clamped to [0,1]
 * at both ends, so the caller cannot accumulate a ramp past the ceiling by
 * holding something for a century.
 */
float step(float ramp, bool held);

/// What building industry costs, as a multiplier on the base price.
float buildCostMul(float ramp);
/// What running it costs.
float upkeepMul(float ramp);
/// What it produces.
float outputMul(float ramp);
/// Extra unrest, in percentage points, in a province it touches.
float unrestPct(float ramp);

/// One speciality a country holds, or is letting go of.
struct Holding {
    std::string resource;   ///< one of Game::SPEC_RESOURCES
    float ramp = 0.0f;
    bool  held = true;      ///< false once released; the entry stays to decay
};

/**
 * Which holdings to release when the cap has fallen below what is held.
 *
 * Returns the resources to let go of, LOWEST RAMP FIRST -- the least invested
 * is the cheapest to give up -- and by name when two are level, so a compass
 * that drifts produces the same answer on every machine and in every replay.
 */
std::vector<std::string> overCap(const std::vector<Holding>& holdings, int cap);

}  // namespace odnat
