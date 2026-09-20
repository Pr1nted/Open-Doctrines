// Industry in state hands.
//
//   NationalisationTest
//
// WHY THIS TEST EXISTS
//
// The mechanic is a ramp and three multipliers hanging off it, and the property
// that makes it a mechanic rather than a bonus is that the three cannot come
// apart. If the output ever climbs ahead of the cost, the strategy is to
// nationalise, build cheaply, collect, and privatise -- and the whole design
// exists to refuse that. So the test that matters is not "does the multiplier
// come out right" but "is there any ramp at which the benefit is large and the
// bill is not".
//
// The second is the ideology cap, where the failure would be silent: a cap that
// is off by one at a boundary hands a centrist country a speciality it should
// not have, and nothing on the screen says which number was wrong.

#include "../src/Nationalisation.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int checks = 0, fails = 0;
void ok(bool c, const std::string& what) {
    ++checks;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!c) ++fails;
}
void section(const char* t) { printf("\n== %s ==\n", t); }

}  // namespace

int main() {
    printf("Nationalisation\n");

    section("THE IDEOLOGY DECIDES HOW MANY");
    {
        // -100 is the command end, +100 the market end.
        ok(odnat::capFor(-100.0f) == 5, "a command economy may take all five");
        ok(odnat::capFor(100.0f) == 0, "a free market may take none");
        ok(odnat::capFor(0.0f) == 2, "a centrist country holds two");

        // Monotone across the whole axis. A dip anywhere would mean a country
        // could gain room by moving RIGHT, which is the wrong way round and is
        // the kind of thing a single boundary test misses.
        bool monotone = true;
        int last = odnat::capFor(-100.0f);
        for (float e = -100.0f; e <= 100.0f; e += 0.5f) {
            const int c = odnat::capFor(e);
            if (c > last) monotone = false;
            last = c;
        }
        ok(monotone, "and the room only ever shrinks as the country moves right");

        // Nothing outside 0..5, including from a compass that should not exist.
        ok(odnat::capFor(-1e9f) == 5 && odnat::capFor(1e9f) == 0,
           "an impossible compass is clamped rather than trusted");
        const float nan = std::nanf("");
        const int nc = odnat::capFor(nan);
        ok(nc >= 0 && nc <= 5, "and a NaN compass yields a cap in range, not INT_MIN");
    }

    section("THE RAMP CLIMBS AND FALLS AT THE SAME RATE");
    {
        float r = 0.0f;
        for (int i = 0; i < odnat::kRampTurns; ++i) r = odnat::step(r, true);
        ok(r > 0.999f, "held for the full term, it reaches 1.0");
        ok(odnat::step(r, true) <= 1.0f, "and does not climb past it");

        for (int i = 0; i < odnat::kRampTurns; ++i) r = odnat::step(r, false);
        ok(r < 0.001f, "released for the same term, it is back to nothing");
        ok(odnat::step(r, false) >= 0.0f, "and does not fall below it");

        // Half the term gives about half the effect -- the point of a ramp.
        float h = 0.0f;
        for (int i = 0; i < odnat::kRampTurns / 2; ++i) h = odnat::step(h, true);
        ok(h > 0.45f && h < 0.55f, "half the term is about half the way");
    }

    section("THE BENEFIT NEVER ARRIVES BEFORE THE BILL");
    {
        // THE ONE THE DESIGN EXISTS FOR. Walked across the whole ramp rather
        // than sampled at the ends: a crossover in the middle is exactly what
        // an exploit would be, and the endpoints would not show it.
        bool billLeads = true;
        float worstRamp = -1.0f;
        for (int i = 0; i <= 100; ++i) {
            const float ramp = (float)i / 100.0f;
            const float gain = odnat::outputMul(ramp) - 1.0f;
            const float bill = (odnat::buildCostMul(ramp) - 1.0f)
                             + (odnat::upkeepMul(ramp) - 1.0f);
            if (gain > bill) { billLeads = false; worstRamp = ramp; break; }
        }
        ok(billLeads, "at no ramp does the output gain exceed what it costs" +
           (billLeads ? std::string() : " (at ramp " + std::to_string(worstRamp) + ")"));

        // And every one of them is zero at zero, so declaring it and doing
        // nothing else changes nothing at all.
        ok(odnat::buildCostMul(0.0f) == 1.0f, "a fresh nationalisation costs nothing extra");
        ok(odnat::upkeepMul(0.0f) == 1.0f, "runs at the same price");
        ok(odnat::outputMul(0.0f) == 1.0f, "and produces no more");
        ok(odnat::unrestPct(0.0f) == 0.0f, "and nobody minds yet");

        // All four move the same way and reach their ceiling together.
        ok(odnat::buildCostMul(1.0f) > odnat::buildCostMul(0.5f), "cost climbs with the ramp");
        ok(odnat::outputMul(1.0f) > odnat::outputMul(0.5f), "so does output");
        ok(odnat::upkeepMul(1.0f) > odnat::upkeepMul(0.5f), "so does upkeep");
        ok(odnat::unrestPct(1.0f) > odnat::unrestPct(0.5f), "and so does the resentment");

        // A ramp outside [0,1] cannot produce an effect outside the ceiling.
        ok(odnat::outputMul(5.0f) == odnat::outputMul(1.0f),
           "a ramp past 1 is clamped, not extrapolated");
        ok(odnat::buildCostMul(-5.0f) == 1.0f, "and one below 0 is too");
    }

    section("losing the room releases the least invested first");
    {
        std::vector<odnat::Holding> hs = {
            {"Oil", 0.9f, true}, {"Gold", 0.1f, true}, {"Metal", 0.5f, true},
        };
        const auto drop = odnat::overCap(hs, 2);
        ok(drop.size() == 1, "one too many means one goes");
        ok(!drop.empty() && drop[0] == "Gold", "and it is the one held the shortest");

        const auto dropTwo = odnat::overCap(hs, 1);
        ok(dropTwo.size() == 2, "a cap of one drops two");
        ok(dropTwo.size() == 2 && dropTwo[0] == "Gold" && dropTwo[1] == "Metal",
           "in order of how little was invested");

        ok(odnat::overCap(hs, 3).empty(), "a cap that fits drops nothing");
        ok(odnat::overCap(hs, 9).empty(), "and a generous one drops nothing");
        ok(odnat::overCap(hs, 0).size() == 3, "a cap of zero drops everything");

        // Ties broken by name, so a drifting compass gives the same answer on
        // every machine and in a replay of the same game.
        std::vector<odnat::Holding> level = {
            {"Rubber", 0.4f, true}, {"Gold", 0.4f, true},
        };
        const auto tie = odnat::overCap(level, 1);
        ok(tie.size() == 1 && tie[0] == "Gold", "equal investment breaks by name");

        // An entry already released is not holding a slot.
        std::vector<odnat::Holding> decaying = {
            {"Oil", 0.9f, true}, {"Gold", 0.3f, false},
        };
        ok(odnat::overCap(decaying, 1).empty(),
           "one still decaying does not count against the cap");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
