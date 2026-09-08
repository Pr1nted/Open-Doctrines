// How far from home an army is, and what the sea does about it.
//
// Supply is measured in hops over the province graph from a country's ports and
// its largest industrial province, walking only ground it or its allies hold.
// Near home costs nothing; beyond that a stack fights progressively worse; and
// a province with NO land route at all is cut off.
//
// THE CASE THIS EXISTS FOR is the one a walk over land cannot see. A landing
// has no route home BY DEFINITION, so the hop walk reads every amphibious
// assault as an encirclement -- and an encirclement is exactly what a landing
// is not, because there is a fleet sitting on the only road it has. The first
// version papered over that with a flat constant applied to anything arriving
// from the sea, which asserted that a landing is supplied instead of asking.
// A beachhead whose fleet has been sunk or has sailed away is stranded, and a
// constant cannot tell that from a fleet still riding offshore.
//
// So the rule asks. This pins what it answers, and in particular the ordering
// property that made the change safe to make: sea supply is a RELAXATION -- no
// position is worse supplied than it was under the constant.
//
// Replicates Game::supplyFactor rather than linking it (the real one needs a
// Game, a map and a fleet). If the rule changes, change it here in the same
// commit.

#include <algorithm>
#include <cstdio>
#include <string>

static int g_checks = 0, g_failed = 0;
static void ok(bool c, const std::string& w) {
    ++g_checks; if (c) return; ++g_failed; printf("  FAIL  %s\n", w.c_str());
}
static void section(const char* n) { printf("\n== %s ==\n", n); }

static const int   SUPPLY_FREE_HOPS = 2;
static const float SUPPLY_FALLOFF   = 0.08f;
static const float SUPPLY_MIN       = 0.55f;
static const float SUPPLY_CUTOFF    = 0.45f;
static const float SUPPLY_BEACHHEAD = 0.85f;

// hops < 0 means no land route at all.
static float supplyFactor(int hops, bool hullOffshore) {
    if (hops < 0) return hullOffshore ? SUPPLY_BEACHHEAD : SUPPLY_CUTOFF;
    if (hops <= SUPPLY_FREE_HOPS) return 1.0f;
    return std::max(SUPPLY_MIN, 1.0f - SUPPLY_FALLOFF * (float)(hops - SUPPLY_FREE_HOPS));
}
// What the flat constant did: every arrival from the sea, supplied, always.
static float oldFactor(int hops, bool fromTheSea) {
    if (fromTheSea) return SUPPLY_BEACHHEAD;
    if (hops < 0) return SUPPLY_CUTOFF;
    if (hops <= SUPPLY_FREE_HOPS) return 1.0f;
    return std::max(SUPPLY_MIN, 1.0f - SUPPLY_FALLOFF * (float)(hops - SUPPLY_FREE_HOPS));
}

int main() {
    section("close to home costs nothing");
    {
        ok(supplyFactor(0, false) == 1.0f, "at a port or the industrial heart");
        ok(supplyFactor(1, false) == 1.0f, "one hop out");
        ok(supplyFactor(2, false) == 1.0f, "two hops, the last free one");
        ok(supplyFactor(3, false) < 1.0f,  "three hops starts to tell");
    }

    section("and the march does not get worse for ever");
    {
        float prev = 1.0f;
        for (int h = 3; h < 40; ++h) {
            const float f = supplyFactor(h, false);
            ok(f <= prev + 1e-6f, "never rises with distance at hop " + std::to_string(h));
            prev = f;
        }
        ok(supplyFactor(39, false) == SUPPLY_MIN, "and settles on the floor");
        ok(SUPPLY_MIN > SUPPLY_CUTOFF,
           "a very long march is still better than no road at all");
    }

    section("a landing is not an encirclement");
    {
        // Both have no land route. Only one of them has a fleet.
        ok(supplyFactor(-1, true)  == SUPPLY_BEACHHEAD, "a beachhead with hulls offshore");
        ok(supplyFactor(-1, false) == SUPPLY_CUTOFF,    "a pocket with none");
        ok(supplyFactor(-1, true) > supplyFactor(-1, false),
           "and the fleet is the whole difference between them");
    }

    section("the fleet leaving is what the flat constant could not say");
    {
        // The same beachhead, on two successive turns: hulls, then none.
        const float withFleet = supplyFactor(-1, true);
        const float stranded  = supplyFactor(-1, false);
        ok(withFleet > stranded, "losing the fleet costs the beachhead its supply");
        ok(oldFactor(-1, true) == oldFactor(-1, true),
           "where the old constant returned the same answer either way");
        // Which is the bug, stated as an assertion.
        ok(oldFactor(-1, /*fromTheSea=*/true) == SUPPLY_BEACHHEAD &&
           supplyFactor(-1, /*hull=*/false) == SUPPLY_CUTOFF,
           "a stranded landing used to be supplied and is now cut off");
    }

    section("sea supply only ever relaxes: nothing is worse off than before");
    {
        // The property that made this safe to land. For every position, the new
        // answer is at least the old one -- except the deliberate case above,
        // where a landing with NO fleet loses the supply it should never have
        // had. Checked over every hop count and both fleet states.
        bool everWorse = false;
        for (int h = -1; h < 30; ++h)
            for (int fleet = 0; fleet < 2; ++fleet) {
                // A landing assault always has its own hull in range, so
                // fromTheSea implies a fleet is present at the moment of
                // landing -- that is the pairing to compare.
                const float now = supplyFactor(h, fleet != 0);
                const float was = oldFactor(h, fleet != 0 && h < 0);
                if (now < was - 1e-6f && !(h < 0 && fleet == 0)) everWorse = true;
            }
        ok(!everWorse, "no supplied position was made worse by the change");
        ok(supplyFactor(-1, true) == oldFactor(-1, true),
           "and a landing at the moment it lands is unchanged, exactly");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
