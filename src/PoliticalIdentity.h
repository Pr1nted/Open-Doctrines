#pragma once

#include <string>

#include "renderer/FlagRenderer.h"

/**
 * What a country calls itself, and what it flies, given where its government
 * stands.
 *
 * WHY THIS EXISTS
 *
 * Doctrines move the government's compass every turn they are implementing or
 * active (Game_Policies.cpp: shiftCountryCompass), so a country that spends
 * fifty turns enacting collectivisation genuinely ends up somewhere its
 * founding name and flag no longer describe. Nothing reflected that: Poland
 * with a fully communist government was still "Poland" under the 1939 flag.
 *
 * WHAT IT DOES NOT DO, DELIBERATELY
 *
 * It does not invent a country. The name keeps its root and the flag keeps its
 * colours and layout -- "Poland" becomes "People's Republic of Poland" and its
 * flag gains a red field bias and a canton symbol, rather than becoming a
 * generic communist flag belonging to nobody. A player who has spent forty
 * turns looking at a shape on a map must still recognise it afterwards, and a
 * country that swings back must come back to EXACTLY what it was, which is why
 * the original of both is stored rather than recomputed.
 *
 * THE TWO THINGS THAT MAKE IT USABLE RATHER THAN ANNOYING
 *
 * Hysteresis. The compass moves continuously, so a country parked on a
 * threshold would rename itself every single turn. An identity is entered at
 * one radius and left at a smaller one, and cannot change again for
 * MIN_DWELL_TURNS.
 *
 * Only the committed. Everything inside ENTER_RADIUS keeps its historical
 * identity. A France that leans mildly left is still France; this is for
 * governments that have gone somewhere, not for every wobble.
 */

/** Which vocabulary a government's position draws on. */
enum class IdeologyQuadrant {
    None = 0,        // near the centre: no expressed ideology
    Communist,       // left + authoritarian
    Socialist,       // left + libertarian
    Nationalist,     // right + authoritarian
    Liberal,         // right + libertarian
    // A government can be a long way from the centre on ONE axis only, and
    // calling that communist or nationalist on the sign of a near-zero economy
    // is how Tibet at econ=-0 and Yemen at econ=-5 ended up with opposite
    // labels for the same politics. When one axis dominates, the identity is
    // that axis and says nothing about the other.
    Authoritarian,   // authoritarian, economically undecided
    Libertarian,     // libertarian, economically undecided
    Collectivist,    // economically left, socially undecided
    Capitalist,      // economically right, socially undecided
};

/** How far it has gone, which decides how loud the name and flag get. */
enum class IdeologyIntensity {
    None = 0,
    Committed,       // past ENTER_RADIUS: renamed, flag restyled
    Radical,         // past RADICAL_RADIUS: stronger wording and symbol
};

struct PoliticalIdentity {
    IdeologyQuadrant  quadrant  = IdeologyQuadrant::None;
    IdeologyIntensity intensity = IdeologyIntensity::None;

    bool operator==(const PoliticalIdentity& o) const {
        return quadrant == o.quadrant && intensity == o.intensity;
    }
    bool operator!=(const PoliticalIdentity& o) const { return !(*this == o); }
    bool expressed() const { return quadrant != IdeologyQuadrant::None; }
};

namespace politid {

// Radii on the (economic, social) compass, each -100..100.
//
// ENTER at 45 and EXIT at 32 rather than one number: a government hovering
// around a single threshold would otherwise rename itself, restyle its flag
// and notify every player, every turn, forever. The gap is what makes the
// change an event rather than a flicker.
inline constexpr float ENTER_RADIUS   = 45.0f;
inline constexpr float EXIT_RADIUS    = 32.0f;
inline constexpr float RADICAL_RADIUS = 72.0f;

// And a floor in turns, because a doctrine can cross the gap above in one step
// if its shift is large enough.
//
// 25, not 12. The radius pair stops a government that is PARKED on a threshold
// from flickering, but it cannot help one whose compass genuinely swings --
// and the AI enacts and repeals doctrines continually, so a measured 150-turn
// run had countries rebranding every sixteen turns with both hysteresis bands
// already in place. A country may change what it calls itself about once a
// generation; more often than that is a government in farce, not in flux.
inline constexpr int MIN_DWELL_TURNS = 25;

// How far along an axis a government must be for that axis to count toward a
// two-axis label. Below it the axis is "undecided" and the identity falls back
// to whichever axis it HAS committed to.
inline constexpr float AXIS_MIN = 25.0f;

// ...and the same hysteresis the radius gets, for the same reason.
//
// Without it AXIS_MIN is a second threshold with no memory: a government whose
// economy sits near 25 flips between "communist" and "authoritarian" every time
// it drifts across, which in a 150-turn run had countries changing identity ten
// times. An axis that has already been counted keeps counting until it falls
// below this.
inline constexpr float AXIS_MIN_EXIT = 16.0f;

/**
 * The identity a compass position implies, given what it is already showing.
 *
 * `current` is passed in because this is hysteretic: staying takes less than
 * arriving did. Pure -- no RNG, no clock -- so a host and its clients compute
 * the same answer from the same state, which is what lets this run on both
 * sides of a multiplayer game without being replicated.
 */
PoliticalIdentity classify(float economic, float social, const PoliticalIdentity& current);

/** e.g. "People's Republic of Poland" from ("Poland", Communist, Radical). */
std::string applyName(const std::string& rootName, const PoliticalIdentity& id);

/**
 * The country's own flag, restyled.
 *
 * Takes the ORIGINAL pattern every time rather than the current one, so the
 * transform never compounds and reverting is exact.
 */
FlagPattern applyFlag(const FlagPattern& originalFlag, const PoliticalIdentity& id);

/**
 * The geographic core of a country name: "Kingdom of Italy" -> "Italy".
 *
 * Shared with the breakaway namer, which must not build "Democratic Alliance of
 * Kingdom of Italy" out of a parent that already carries a form.
 */
std::string geographicCoreOf(const std::string& name);

/** For notifications: "communist", "nationalist", ... */
const char* quadrantName(IdeologyQuadrant q);

}  // namespace politid
