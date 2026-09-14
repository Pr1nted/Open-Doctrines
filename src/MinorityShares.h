#pragma once
// The two rules that keep an ethnic breakdown a breakdown.
//
// Minority shares PARTITION a province: every group in the list is a fraction
// of the same province population, the majority included -- the name is
// misleading. Migration is the only thing that moves people between provinces,
// and it has to leave that partition intact at both ends.
//
// It did not. The recalculation subtracted a move from one group's head count
// and then divided EVERY group by the shrunken province, which balances only
// while the shares still add up -- and moveCount was capped against the
// PROVINCE (srcPop/10, or srcPop/2 under a refugee surge), never against the
// group actually leaving. Move more people than a group has and its head count
// goes negative, it is dropped as empty, and its share leaves the numerator
// while the denominator keeps the hole. Everyone left behind is inflated, and
// nothing bounded the result, so the error compounded every turn.
//
// Replayed over the 116 turns of a conquer-the-world save the shares in one
// province reached 6,111,403%, and the Ethnic Management panel multiplied a
// legitimate province population by them: 9.4e20 Belarusians, with the titular
// Poles -- the group that kept emigrating -- smaller than its own minorities.
//
// These live in a header, templated on the group type, for one reason: the
// test has to exercise the code the game runs. Written as lambdas inside
// processPopulation they were unreachable from anywhere else, and a test that
// re-typed the arithmetic would have gone on passing while the game's own copy
// drifted. Only `.name` and `.pct` are touched, so tests/minority_share_test.cpp
// instantiates them on its own struct without dragging in raylib or the game.
//
// See tests/minority_share_test.cpp, which replays the save above.

#include <algorithm>
#include <string>
#include <vector>

namespace od {

// How many people of `name` this province actually holds.
//
// Absent means zero, not "unconstrained". That distinction IS the fix: the
// caller clamps a move against this, and a group that is not here cannot
// emigrate from here.
template <typename Group>
long long groupPopAt(const std::vector<Group>& groups, long long provPop,
                     const std::string& name) {
    for (const auto& g : groups)
        if (g.name == name) return (long long)((double)provPop * g.pct / 100.0);
    return 0;
}

// Drop the emptied groups, and hold the total at no more than `maxTotal`.
//
// A CAP, never a rescale UP -- and that asymmetry is deliberate. The map data
// is not uniformly a clean 100: measured across the 1939 world the share totals
// run from 85% to a median of exactly 100%. Normalising every province this
// touches onto 100 would quietly manufacture people in each province whose
// author left the total short, which is a bigger edit than the bug being fixed
// and would show up as population appearing wherever anyone migrated. Arrivals
// legitimately push a destination over 100, and that is what gets clamped here.
//
// Summed in double: the shares are floats, and adding a long list of them in
// float drifts on its own over a few hundred turns.
template <typename Group>
void renormaliseShares(std::vector<Group>& groups, double maxTotal = 100.0) {
    groups.erase(std::remove_if(groups.begin(), groups.end(),
        [](const Group& g) { return g.pct <= 0.0f; }), groups.end());
    double total = 0.0;
    for (const auto& g : groups) total += g.pct;
    if (total <= 0.0) { groups.clear(); return; }   // nobody left; the caller erases the entry
    if (total <= maxTotal) return;                  // short totals are the map's, not ours to fix
    for (auto& g : groups) g.pct = (float)(g.pct / total * maxTotal);
}

}  // namespace od
