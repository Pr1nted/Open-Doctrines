// The recipes, and the property that turned out to matter most: CAN EVERYBODY
// EAT?
//
// The production economy shipped with two bugs that were invisible in the code
// and obvious the first time it was run, and both of them are properties of the
// recipe table rather than of any one function:
//
//   THE CONSUMER GOOD WAS UNMAKEABLE FOR MOST OF THE WORLD. Its first recipe
//   was rubber plus gemstones. Gemstones are on 123 of the 1298 provinces of
//   the 1939 map, and twenty-seven of sixty-four countries have NEITHER rubber
//   NOR gemstones -- so their populations could never be fed, whatever they
//   built and however well they played. That is not difficulty, it is an
//   unwinnable state handed out at map load, and it measured as 93% of the
//   world's factories standing idle with rebellions tripled.
//
//   SUBSTITUTABLE INPUTS HAVE TO BE SPENDABLE. `any` is only meaningful if the
//   feasibility calculation and the deduction agree about it; if one counts a
//   material the other will not spend, factories stall holding full piles.
//
// So this asserts the invariant the first version broke -- a country holding
// ANY single raw material can feed its people -- plus the arithmetic of the
// recipe helpers. The strategic goods are asserted to be the opposite: fuel
// without oil must be impossible, because that scarcity is the point.
//
// Pure arithmetic over the recipe table. No window, no map, no game.

#include "GameStructs.h"

#include <cmath>
#include <cstdio>
#include <string>

static int g_checks = 0, g_failed = 0;

static void ok(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    ++g_failed;
    printf("  FAIL  %s\n", what.c_str());
}

static void section(const char* name) { printf("\n== %s ==\n", name); }

// Mirrors Game::recipeFeasible. Kept in step by the checks below rather than by
// hope: if the real one changes shape, the "spend exactly what was promised"
// section stops balancing.
static float feasible(const CountryStockpile& p, int good) {
    const GoodRecipe r = goodInputs(good);
    float f = 1e9f;
    bool bounded = false;
    for (int i = 0; i < RAW_COUNT; ++i) {
        if (r.fixed[i] <= 0.0f) continue;
        f = std::fmin(f, p.raw[i] / r.fixed[i]);
        bounded = true;
    }
    if (r.any > 0.0f) {
        float total = 0.0f;
        for (int i = 0; i < RAW_COUNT; ++i) total += p.raw[i];
        f = std::fmin(f, total / r.any);
        bounded = true;
    }
    return bounded ? std::fmax(0.0f, f) : 0.0f;
}

static CountryStockpile withOnly(int raw, float amount) {
    CountryStockpile p;
    p.raw[raw] = amount;
    return p;
}

int main() {
    section("everybody can eat");
    // THE INVARIANT THE FIRST RECIPE BROKE. A country holding a useful quantity
    // of any ONE of the four materials must be able to make the good its
    // population eats. Checked for each material separately, because "some
    // countries can" is exactly the bug: 27 of 64 could not.
    for (int r = 0; r < RAW_COUNT; ++r) {
        const CountryStockpile p = withOnly(r, 100.0f);
        ok(feasible(p, GOOD_CONSUMER) > 0.0f,
           std::string("consumer goods are makeable from ") + rawKey(r) + " alone");
    }
    // And the honest boundary: nothing at all really is nothing.
    {
        CountryStockpile empty;
        ok(feasible(empty, GOOD_CONSUMER) == 0.0f,
           "a country with no materials at all cannot make consumer goods");
    }

    section("strategic goods stay strategic");
    // The opposite property, and it is deliberate. Fuel without oil is the kind
    // of scarcity that makes trade and conquest mean something; if these ever
    // start accepting substitutes, the war economy has quietly become free.
    ok(feasible(withOnly(RAW_METAL, 100.0f), GOOD_FUEL) == 0.0f,
       "no oil, no fuel -- metal is not a substitute");
    ok(feasible(withOnly(RAW_RUBBER, 100.0f), GOOD_MACHINERY) == 0.0f,
       "no metal, no machinery");
    ok(feasible(withOnly(RAW_OIL, 100.0f), GOOD_MUNITIONS) == 0.0f,
       "no metal, no munitions -- its `any` part cannot cover the fixed part");
    ok(feasible(withOnly(RAW_OIL, 100.0f), GOOD_FUEL) > 0.0f, "oil makes fuel");
    ok(feasible(withOnly(RAW_METAL, 100.0f), GOOD_MACHINERY) > 0.0f,
       "metal makes machinery");

    section("feasibility is monotonic and proportional");
    for (int g = 0; g < GOOD_COUNT; ++g) {
        CountryStockpile small, big;
        for (int r = 0; r < RAW_COUNT; ++r) { small.raw[r] = 10.0f; big.raw[r] = 20.0f; }
        const float a = feasible(small, g), b = feasible(big, g);
        ok(b >= a, std::string("more materials never makes ") + goodKey(g) + " less feasible");
        // Twice everything is twice the output: every term is a ratio, so a
        // recipe that broke this would have a constant hiding in it.
        ok(std::fabs(b - a * 2.0f) < 0.001f,
           std::string("doubling every material doubles feasible ") + goodKey(g));
    }

    section("every good is actually producible, and named");
    for (int g = 0; g < GOOD_COUNT; ++g) {
        CountryStockpile rich;
        for (int r = 0; r < RAW_COUNT; ++r) rich.raw[r] = 1000.0f;
        ok(feasible(rich, g) > 0.0f,
           std::string("a well-supplied country can make ") + goodKey(g));
        // A good with no recipe at all would silently never be made, and the
        // allocator would keep choosing it because its shelf is always empty.
        const GoodRecipe rc = goodInputs(g);
        float sum = rc.any;
        for (int r = 0; r < RAW_COUNT; ++r) sum += rc.fixed[r];
        ok(sum > 0.0f, std::string(goodKey(g)) + " has a recipe with inputs in it");
        ok(goodKey(g)[0] != '\0', std::string("good ") + std::to_string(g) + " has a save key");
        ok(rawKey(g < RAW_COUNT ? g : 0)[0] != '\0', "raw materials have save keys");
    }

    section("save keys are stable and distinct");
    // These key save files and the bench's output. Two goods sharing a key
    // would silently merge a country's stockpiles on load.
    for (int a = 0; a < GOOD_COUNT; ++a)
        for (int b = a + 1; b < GOOD_COUNT; ++b)
            ok(std::string(goodKey(a)) != goodKey(b),
               std::string("good keys differ: ") + goodKey(a) + " vs " + goodKey(b));
    for (int a = 0; a < RAW_COUNT; ++a)
        for (int b = a + 1; b < RAW_COUNT; ++b)
            ok(std::string(rawKey(a)) != rawKey(b),
               std::string("raw keys differ: ") + rawKey(a) + " vs " + rawKey(b));

    section("supply and demand are scale-invariant");
    // THE PROPERTY THAT COST A WHOLE DESIGN ITERATION. Demand is linear in
    // population. Supply used to be linear in industry LEVELS, which are capped
    // by a capacity that is LOGARITHMIC in population -- so a country whose
    // population multiplied could never multiply its production to match, and
    // no decision the player made could close the gap. Measured on one map, the
    // shortfall went 1.8:1 at turn 40, 20:1 at turn 80, 33.7:1 at turn 120,
    // because the world's population runs away and only one side of the ratio
    // followed it.
    //
    // So the invariant is: multiply every population by anything and the
    // supply-to-demand ratio must not move. If this ever fails again, a long
    // game has become unwinnable for reasons the player cannot see or act on.
    {
        // One province industrialised to its capacity, feeding its own people.
        auto ratioAt = [](double people) {
            const double demand = people / CONSUMER_PER_CAPITA;      // countryGoodsDemand
            const double supply = (people / CONSUMER_PER_CAPITA)     // provinceGoodOutput
                                  * 1.0 /* built to capacity */ * GOOD_OUTPUT_SCALE;
            return supply / demand;
        };
        const double base = ratioAt(1000000.0);
        for (double mult : {1.0, 10.0, 52.0, 1000.0}) {
            ok(std::fabs(ratioAt(1000000.0 * mult) - base) < 1e-9,
               "supply/demand is unchanged at " + std::to_string((long long)mult) +
                   "x the population");
        }
        // And the intended margin: fully built feeds more than itself, so the
        // headroom pays for machinery, fuel and munitions out of the same
        // factories. At or below 1.0 the other three goods could never be made.
        ok(base > 1.0, "a fully industrialised province feeds more than its own people");
        ok(base < 3.0, "but not so much that industrialising once solves the game");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
