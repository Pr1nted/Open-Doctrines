// What a province will carry, and the two ways that answer can go wrong.
//
// industryCapacity() decides where a factory may stand. It replaced a flat cap
// of ten everywhere, which let an uninhabited rock hold the same industry as
// the Ruhr -- the complaint that started it was an island "with a population of
// 0, 84 square kilometres of space, and basically no resources" sitting at
// level 10.
//
// A formula like that fails in two distinct ways, so this checks both.
//
//   THE SHAPE. More people and more ore must never LOWER the ceiling; land is
//   deliberately not in that list, and the section below says why. It must be
//   bounded at both ends, density must outweigh area (or the term is decorative
//   -- it was, once), and it must put the specific provinces people argued
//   about where they belong. The constants
//   are pinned here as well: they were fitted against the four shipped maps by
//   asking how often the rule disagrees with what a human map author actually
//   built, and a silent edit to any of them is a balance change to every map at
//   once. If you MEAN to move them, move the expectations here in the same
//   commit and say so -- that is the point of pinning them.
//
//   THE AREA WALK. Capacity reads cos(latitude)-weighted area, accumulated in
//   the load pass by advancing a row weight as a flat pixel index crosses each
//   row boundary (see Game::buildPopulationLookups). That loop is the kind of
//   thing that is off by one row for a year without anyone noticing, because
//   every province would simply be slightly wrong. It is replicated here and
//   checked against the direct sum it is an optimisation of.
//
// Pure arithmetic: no window, no map, no game. Links nothing.

#include "BuildCosts.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;

static void ok(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    ++g_failed;
    printf("  FAIL  %s\n", what.c_str());
}

static void section(const char* name) { printf("\n== %s ==\n", name); }

int main() {
    section("bounds");
    // Nowhere is barren. A capacity of zero would make a province's first
    // factory unbuildable for ever, which is a wall rather than a slope.
    ok(industryCapacity(0, 0.0f, 0.0f) == 1, "empty province floors at 1");
    ok(industryCapacity(-5, -5.0f, -5.0f) == 1, "negative inputs floor at 1");
    ok(industryCapacity(10000000000LL, 1e9f, 1e6f) == IND_MAX_LEVEL,
       "absurd inputs cap at IND_MAX_LEVEL");
    // Division by area happens inside; area 0 must not become an infinite
    // density and therefore an instant level 10.
    ok(industryCapacity(1000000, 0.0f, 0.0f) <= IND_MAX_LEVEL,
       "population with no area does not divide by zero");

    section("the province that started it");
    // Pop 0, a couple of raster pixels, nothing buried. This is the case the
    // player reported at level 10.
    ok(industryCapacity(0, 2.0f, 0.0f) == 1, "uninhabited rock supports level 1");
    ok(industryCapacity(0, 40.0f, 0.0f) == 1, "uninhabited and slightly larger: still 1");

    section("monotonic where monotonicity is actually meant");
    // Not "usually rises" -- never falls. A player who settles a province or
    // finds ore in it must never be punished for that with a lower ceiling.
    //
    // AREA IS DELIBERATELY NOT IN THIS LIST, and the first version of this test
    // asserted that it was, which was wrong about the design rather than about
    // the code. At FIXED population, more land means the same people spread
    // thinner -- more rural, less industrial -- and the density term is
    // supposed to fall. Asserting "more land never lowers capacity" demands
    // that density do nothing, which is exactly the degenerate formula the
    // constants were re-fitted to escape.
    //
    // The honest invariant for land is the scaling one below: a province with
    // proportionally more of everything is at least as capable.
    for (long long pop : {0LL, 1000LL, 50000LL, 500000LL, 5000000LL, 40000000LL}) {
        for (float area : {1.0f, 50.0f, 500.0f, 5000.0f, 40000.0f}) {
            for (float res : {0.0f, 20.0f, 90.0f, 250.0f}) {
                const int base = industryCapacity(pop, area, res);
                ok(industryCapacity(pop * 2 + 1, area, res) >= base,
                   "more people never lowers capacity");
                ok(industryCapacity(pop, area, res + 25.0f) >= base,
                   "more resources never lowers capacity");
                // Same density, more of it: a bigger province holding
                // proportionally more people is never worth less.
                ok(industryCapacity(pop * 4, area * 4.0f, res) >= base,
                   "scaling land and people together never lowers capacity");
            }
        }
    }

    section("density is what lets a small province carry heavy industry");
    // Without a density term the rule is "big population wins", and it gets the
    // shipped maps wrong in a specific, repeatable place: Belgium and the
    // French industrial north are small, dense, resource-poor and heavily
    // industrialised by hand. Same population, same resources, an order of
    // magnitude apart in land -- the dense one must rank higher.
    {
        const int dense  = industryCapacity(500000, 340.0f, 9.0f);
        const int sparse = industryCapacity(500000, 20000.0f, 9.0f);
        ok(dense > sparse, "dense province outranks the sprawling one at equal population");
    }

    section("resources carry an empty province some of the way");
    {
        const int barren = industryCapacity(2000, 800.0f, 0.0f);
        const int rich   = industryCapacity(2000, 800.0f, 260.0f);
        ok(rich > barren, "a rich seam is worth building on");
        ok(rich < IND_MAX_LEVEL, "but ore alone is not an industrial heartland");
    }

    section("pinned constants (fitted against the four shipped maps)");
    // Representative provinces from data/STDmaps, with the capacities the
    // fitted constants produce. These are the numbers the calibration was
    // accepted on; if they move, the balance of every map moved with them.
    struct Case { long long pop; float area; float res; int want; const char* what; };
    const Case cases[] = {
        {       0,      2.0f,   0.0f,  1, "uninhabited islet" },
        {    1819,    320.0f,   0.0f,  1, "near-empty province" },
        {  463538,   2392.0f,  16.4f,  4, "median shipped province" },
        {  489618,    341.0f,   9.1f,  5, "Belgium: small, dense, no ore" },
        { 2370615,   2839.0f,  12.0f,  6, "French industrial north" },
        {74724692,  14589.0f, 104.6f, 10, "largest province on the 1939 map" },
    };
    for (const Case& c : cases) {
        const int got = industryCapacity(c.pop, c.area, c.res);
        ok(got == c.want,
           std::string(c.what) + ": expected " + std::to_string(c.want) +
               ", got " + std::to_string(got));
    }

    section("the cos(latitude) area walk");
    // Replicates Game::buildPopulationLookups' row advance exactly, then checks
    // it against the direct per-pixel sum it stands in for. Small raster, odd
    // height, so an off-by-one at either pole shows up.
    {
        const int w = 7, h = 5;
        std::vector<float> rowWeight((size_t)h, 0.0f);
        for (int y = 0; y < h; ++y) {
            const double lat = 90.0 - ((double)y + 0.5) * (180.0 / (double)h);
            rowWeight[(size_t)y] = (float)std::cos(lat * 3.14159265358979323846 / 180.0);
        }

        // The walk, as the loader runs it.
        int areaRow = 0, areaRowEnd = w;
        float areaRowW = rowWeight[0];
        std::vector<float> walked((size_t)h, 0.0f);
        for (int i = 0; i < w * h; ++i) {
            if (i >= areaRowEnd && areaRow + 1 < h) {
                ++areaRow;
                areaRowEnd += w;
                areaRowW = rowWeight[(size_t)areaRow];
            }
            // Attribute every pixel to its own row, so a row that was credited
            // with a neighbour's weight is visible rather than averaged away.
            walked[(size_t)(i / w)] += areaRowW;
        }

        bool allRowsRight = true;
        for (int y = 0; y < h; ++y) {
            const float want = rowWeight[(size_t)y] * (float)w;
            if (std::fabs(walked[(size_t)y] - want) > 1e-4f) allRowsRight = false;
        }
        ok(allRowsRight, "each row is credited its own cos(latitude) weight");

        // And the property the weighting exists for: a row near the pole must
        // count for less ground than the same pixels at the equator.
        const int mid = h / 2;
        ok(rowWeight[0] < rowWeight[(size_t)mid],
           "a polar row weighs less than an equatorial one");
        ok(rowWeight[0] > 0.0f, "no row weighs zero or less");
    }

    section("what a hull costs to keep");
    {
        // The AI carried its own copy of these numbers in two places and was
        // left behind when they were repriced -- believing a scrapped carrier
        // closed a 25-point hole when it closed a 4-point one. shipUpkeep is
        // the single place it is worked out; these pin what it answers.
        ok(shipUpkeep("carrier", 0) == SHIP_UPKEEP_CARRIER, "a carrier costs its constant");
        ok(shipUpkeep("destroyer", 0) == SHIP_UPKEEP_DESTROYER, "and a destroyer its own");
        ok(shipUpkeep("carrier", 0) > shipUpkeep("destroyer", 0),
           "a carrier is dearer than a destroyer");
        ok(shipUpkeep("boat", 0) == 0.0f, "an empty transport is free to keep");
        ok(shipUpkeep("nonsense", 0) == 0.0f, "and an unknown hull costs nothing");
        ok(shipUpkeep("boat", 10000) > 0.0f, "a crewed transport is not free");

        // THE ORDERING THAT KEEPS THE AI HONEST. Scrapping is worth doing at
        // all only while a hull costs more than the men on it; if that ever
        // inverts, the austerity loop scraps warships to save nothing.
        ok(shipUpkeep("destroyer", 0) > shipUpkeep("boat", 10000),
           "a warship costs more than a transport's crew");

        // Bit-identity with the accumulation this replaced. The old loop added
        // the hull and the crew as two separate `+=`, and float addition is not
        // associative -- so this is only a pure refactor because one of the two
        // terms is always exactly zero. The loader enforces that: every hull
        // that is not a "boat" has its crew set to 0.
        ok(shipUpkeep("carrier", 0) == 0.0f + SHIP_UPKEEP_CARRIER + 0.0f,
           "a warship's crew term is exactly zero, so the order cannot matter");
        const float crewOnly = (7500 / 10000.0f) * SHIP_UPKEEP_PER_10K_CREW;
        ok(shipUpkeep("boat", 7500) == 0.0f + crewOnly,
           "and a transport's hull term is exactly zero");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
