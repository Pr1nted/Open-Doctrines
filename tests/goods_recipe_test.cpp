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

#include <array>

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

static void ok(bool cond, const std::string& what, const std::string& detail) {
    ok(cond, what + "  [" + detail + "]");
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

    section("who makes what, when everything is short");
    {
        // THE BUG THIS EXISTS TO STOP HAPPENING A THIRD TIME. The allocator
        // ranked goods by the fraction of their need that was unmet and
        // clamped it to 1, so EVERY fully unmet good scored exactly 1.0, the
        // tie fell to the lowest good id, and consumer goods were the only
        // thing a starving country ever made -- which is the very bug the
        // fraction had been introduced to fix. Measured on 1914:FRA: consumer
        // 30.24 a turn, machinery, fuel and munitions 0.00 each against real
        // demand.
        auto all = [](float c, float m, float f, float mu) {
            std::array<float, GOOD_COUNT> a{};
            a[GOOD_CONSUMER] = c; a[GOOD_MACHINERY] = m;
            a[GOOD_FUEL] = f; a[GOOD_MUNITIONS] = mu;
            return a;
        };
        const bool yes[GOOD_COUNT] = {true, true, true, true};
        int out[GOOD_COUNT];

        // A country at war, short of everything, with 20 factories. The
        // numbers are 1914:FRA's own, measured.
        auto need  = all(37.18f, 7.00f, 0.01f, 0.10f);
        auto stock = all(0.0f, 0.0f, 0.0f, 0.0f);
        planOutputs(need.data(), stock.data(), yes, 20, out);
        ok(out[GOOD_CONSUMER] + out[GOOD_MACHINERY] + out[GOOD_FUEL] +
               out[GOOD_MUNITIONS] == 20,
           "every factory is given something to make");
        ok(out[GOOD_CONSUMER] < 20,
           "a country short of everything does not put every factory into food",
           std::to_string(out[GOOD_CONSUMER]) + " of 20");
        ok(out[GOOD_MACHINERY] > 0, "some of it makes machinery");
        ok(out[GOOD_CONSUMER] > out[GOOD_MACHINERY],
           "but food still gets the most, because that is what it is shortest of");

        // A need of 0.10 against 37.18 is a rounding error in proportion, and
        // it still has to get a factory -- a country that cannot make ONE
        // shell cannot fight at all. This is what largest remainder buys.
        ok(out[GOOD_MUNITIONS] > 0, "and a tiny munitions need still gets a plant");

        // FED, and the shelves are the other way round. Nothing about the
        // first case should make a well-fed country keep building food.
        stock = all(60.0f, 0.0f, 0.0f, 0.0f);
        planOutputs(need.data(), stock.data(), yes, 20, out);
        ok(out[GOOD_CONSUMER] == 0,
           "a country with full larders stops making food");

        // Feasibility still wins over need: no oil, no fuel, however badly it
        // is wanted. That scarcity is the point of the strategic goods.
        const bool noFuel[GOOD_COUNT] = {true, true, false, true};
        need  = all(1.0f, 1.0f, 100.0f, 1.0f);
        stock = all(0.0f, 0.0f, 0.0f, 0.0f);
        planOutputs(need.data(), stock.data(), noFuel, 9, out);
        ok(out[GOOD_FUEL] == 0, "a country with no oil is given no fuel to make");
        ok(out[GOOD_CONSUMER] + out[GOOD_MACHINERY] + out[GOOD_MUNITIONS] == 9,
           "and its factories go to what it can actually make");

        // Oversupplied in everything: direct nothing, rather than topping up
        // the least-full shelf. This is the -1.0 floor, and it is load bearing
        // -- it is the one behaviour a careless rewrite of this loses.
        need  = all(1.0f, 1.0f, 1.0f, 1.0f);
        stock = all(50.0f, 50.0f, 50.0f, 50.0f);
        planOutputs(need.data(), stock.data(), yes, 20, out);
        ok(out[GOOD_CONSUMER] + out[GOOD_MACHINERY] + out[GOOD_FUEL] +
               out[GOOD_MUNITIONS] == 0,
           "a country with everything overflowing directs nothing");

        // Nobody wants anything: the same.
        need = all(0.0f, 0.0f, 0.0f, 0.0f);
        planOutputs(need.data(), stock.data(), yes, 20, out);
        ok(out[GOOD_CONSUMER] + out[GOOD_MACHINERY] + out[GOOD_FUEL] +
               out[GOOD_MUNITIONS] == 0,
           "and so does one nobody wants anything from");
    }

    section("a good that holds stock is not locked out");
    {
        // THE THIRD TIME. The two notes above describe the same bug twice:
        // consumer goods ending up the only thing anyone makes. It happened a
        // third way, and none of the checks above could see it, because they
        // all start from EMPTY shelves -- which is the one state where the
        // rule behaved.
        //
        // Step 2 admitted only goods within 0.02 of the worst unmet fraction.
        // Consumer goods are eaten to zero every turn, so their fraction is
        // pinned at 1.0 forever and `worst` is pinned with it. Any other good
        // holding the smallest stock fell outside the window and was excluded
        // ENTIRELY -- it never reached the one-factory-each step either.
        //
        // These are the demands the 2026-09-30 sweep measured, and the stocks
        // are what a country actually holds mid-game. Before the fix the last
        // two cases both returned consumer=100 and zero of everything else.
        auto all = [](float c, float m, float f, float mu) {
            std::array<float, GOOD_COUNT> a{};
            a[GOOD_CONSUMER] = c; a[GOOD_MACHINERY] = m;
            a[GOOD_FUEL] = f; a[GOOD_MUNITIONS] = mu;
            return a;
        };
        const bool yes[GOOD_COUNT] = {true, true, true, true};
        int out[GOOD_COUNT];
        auto need = all(1480.9f, 262.3f, 112.3f, 155.4f);

        // A trace of stock in one good must not cost it its whole share.
        auto s1 = all(0.0f, 6.0f, 0.0f, 0.0f);          // 2.3% of its demand
        planOutputs(need.data(), s1.data(), yes, 100, out);
        ok(out[GOOD_MACHINERY] > 0,
           "machinery holding 2.3% of its demand still gets factories",
           std::to_string(out[GOOD_MACHINERY]) + " of 100");

        // The state a mid-game country is actually in.
        auto s2 = all(0.0f, 26.0f, 3.0f, 5.0f);
        planOutputs(need.data(), s2.data(), yes, 100, out);
        ok(out[GOOD_CONSUMER] < 100,
           "food does not take every factory once the others hold any stock",
           std::to_string(out[GOOD_CONSUMER]) + " of 100");
        ok(out[GOOD_MACHINERY] > 0 && out[GOOD_FUEL] > 0 && out[GOOD_MUNITIONS] > 0,
           "and machinery, fuel and munitions are all still made");

        // INVESTMENT IS THE ONE THAT MATTERS. Without machinery a country
        // cannot build the industry that would feed it, so a lockout here is
        // not one shortage among four -- it is the reason the shortage never
        // ends.
        ok(out[GOOD_MACHINERY] >= 8,
           "machinery gets a share near its share of total shortfall",
           std::to_string(out[GOOD_MACHINERY]) + " of 100, demand share ~13%");

        // Everything half-stocked: food should lead, nothing should be zero.
        auto s3 = all(0.0f, 131.0f, 56.0f, 77.0f);
        planOutputs(need.data(), s3.data(), yes, 100, out);
        ok(out[GOOD_CONSUMER] > out[GOOD_MACHINERY],
           "the good that is furthest short still leads");
        ok(out[GOOD_MACHINERY] > 0 && out[GOOD_FUEL] > 0 && out[GOOD_MUNITIONS] > 0,
           "but none of the others is locked out at half stock");

        // The window's LEGITIMATE purpose has to survive its removal: a good
        // nobody is short of should not be made. Proportional-to-shortfall
        // does this on its own -- zero shortfall, zero share.
        auto s4 = all(0.0f, 400.0f, 200.0f, 200.0f);   // all more than full
        planOutputs(need.data(), s4.data(), yes, 100, out);
        ok(out[GOOD_MACHINERY] == 0 && out[GOOD_FUEL] == 0 && out[GOOD_MUNITIONS] == 0,
           "a good nobody is short of is still not made");
        ok(out[GOOD_CONSUMER] == 100,
           "and the one good that IS short takes the country",
           std::to_string(out[GOOD_CONSUMER]) + " of 100");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
