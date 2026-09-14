// Ethnic migration has to conserve people.
//
// A player's conquer-the-world save reached turn 116 with 9.4e20 Belarusians in
// one Polish province -- more people than atoms in a room -- and the titular
// Poles, the group that had been emigrating the whole game, listed smaller than
// any of their own minorities. The number was not a display bug: the shares
// themselves had grown to 6,111,403%, and the panel dutifully multiplied a
// perfectly ordinary province population by them.
//
// The last case here is that save. It runs the real migration arithmetic over
// the same shape of turn loop, and it FAILS on the old code -- verified by
// reverting the clamp, which is the only way to know a regression test for a
// numeric bug is pointed at anything.

#include "MinorityShares.h"

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;

static void ok(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    ++g_failed;
    printf("  FAIL  %s\n", what.c_str());
}

// Only .name and .pct, which is all the header touches -- no raylib, no Game.
struct Group {
    std::string name;
    float pct;
};

static double total(const std::vector<Group>& g) {
    double t = 0.0;
    for (const auto& x : g) t += x.pct;
    return t;
}

static float shareOf(const std::vector<Group>& g, const std::string& n) {
    for (const auto& x : g) if (x.name == n) return x.pct;
    return 0.0f;
}

// ── the migration step, exactly as processPopulation runs it ────────────────
//
// Both call sites in Game_TurnLogic.cpp do this: clamp the move, move it, then
// recompute every share against the shrunken province and hand the list to
// renormaliseShares. `clampToGroup` is the fix under test; passing false is the
// old code, and the last case needs that arm to fail.
static void migrateOut(std::vector<Group>& groups, long long& provPop,
                       const std::string& who, long long want, bool clampToGroup) {
    long long move = want;
    if (clampToGroup) move = std::min(move, od::groupPopAt(groups, provPop, who));
    move = std::min(move, provPop / 10);          // the province-level cap
    if (move < 1) return;

    long long newPop = provPop - move;
    for (auto& g : groups) {
        long long abs = (long long)((double)provPop * g.pct / 100.0);
        if (g.name == who) abs -= move;
        g.pct = newPop > 0 ? (float)((double)abs / newPop * 100.0) : 0.0f;
    }
    od::renormaliseShares(groups);
    provPop = newPop;
}

int main() {
    printf("minority shares\n");

    // ── groupPopAt ──
    {
        std::vector<Group> g = {{"Pole", 63.4f}, {"Belarusian", 17.5f}, {"Ukrainian", 5.8f}};
        ok(od::groupPopAt(g, 1000000, "Pole") == 634000, "63.4% of a million is 634000");
        ok(od::groupPopAt(g, 1000000, "Belarusian") == 175000, "17.5% of a million");

        // The whole fix rests on this returning 0 rather than "no constraint".
        ok(od::groupPopAt(g, 1000000, "Latvian") == 0, "a group that is not here holds nobody");
        const std::vector<Group> none;
        ok(od::groupPopAt(none, 1000000, "Pole") == 0, "an empty province holds nobody");
    }

    // ── renormaliseShares caps, and only caps ──
    {
        std::vector<Group> g = {{"Pole", 60.0f}, {"Belarusian", 60.0f}};
        od::renormaliseShares(g);
        ok(std::fabs(total(g) - 100.0) < 0.01, "120% is brought back to 100");
        ok(std::fabs(g[0].pct - g[1].pct) < 0.01, "and the ratio between groups is kept");
    }
    {
        // A province the map author left short stays short. Rescaling it to 100
        // would invent people in every province anyone ever migrated through.
        std::vector<Group> g = {{"Pashtun", 50.0f}, {"Tajik", 35.0f}};
        od::renormaliseShares(g);
        ok(std::fabs(total(g) - 85.0) < 0.01, "an 85% province is left at 85%");
    }
    {
        std::vector<Group> g = {{"Pole", 99.0f}, {"Ghost", 0.0f}, {"Negative", -12.0f}};
        od::renormaliseShares(g);
        ok(g.size() == 1 && g[0].name == "Pole", "emptied and negative groups are dropped");
    }
    {
        std::vector<Group> g = {{"Ghost", 0.0f}};
        od::renormaliseShares(g);
        ok(g.empty(), "a province of nobody empties, rather than dividing by zero");
    }

    // ── one oversized move ──
    //
    // The province cap allows 10% of everyone; the Ukrainians are 5% of it. The
    // old code moved the province's allowance out of a group that could not
    // cover it, and the difference came out of everybody else's share.
    {
        std::vector<Group> g = {{"Pole", 80.0f}, {"Belarusian", 15.0f}, {"Ukrainian", 5.0f}};
        long long pop = 1000000;
        migrateOut(g, pop, "Ukrainian", 100000, true);
        ok(total(g) <= 100.01, "shares stay within 100 after an oversized move");
        ok(shareOf(g, "Pole") <= 84.3f, "the Poles are not inflated by someone else leaving");
        ok(pop >= 950000, "and no more people left than the group had");
    }

    // ── the save: 116 turns of the same province bleeding one group ──
    {
        std::vector<Group> g = {{"Pole", 63.4f}, {"Belarusian", 17.5f},
                                {"Ukrainian", 5.8f}, {"Lithuanian", 6.2f},
                                {"German", 1.3f}, {"Jewish", 5.8f}};
        long long pop = 2000000;
        for (int turn = 0; turn < 116; ++turn)
            migrateOut(g, pop, "Pole", pop / 10, true);   // ask for the whole allowance

        ok(total(g) <= 100.01, "116 turns later the shares still total 100 or less");
        ok(shareOf(g, "Belarusian") <= 100.0f, "no group exceeds the whole province");

        // The reported symptom, stated as the number that was on screen.
        long long belarusians = od::groupPopAt(g, pop, "Belarusian");
        ok(belarusians <= pop, "the Belarusians do not outnumber the province");
        ok(belarusians < 100000000LL, "and nowhere near the 9.4e20 that was reported");

        // BREAK IT ON PURPOSE. The same loop without the group clamp is the old
        // code; if this passes, the case above is not testing anything.
        std::vector<Group> broken = {{"Pole", 63.4f}, {"Belarusian", 17.5f},
                                     {"Ukrainian", 5.8f}, {"Lithuanian", 6.2f},
                                     {"German", 1.3f}, {"Jewish", 5.8f}};
        long long bpop = 2000000;
        for (int turn = 0; turn < 116; ++turn)
            migrateOut(broken, bpop, "Pole", bpop / 10, false);
        ok(shareOf(broken, "Belarusian") > shareOf(g, "Belarusian"),
           "the unclamped arithmetic really does inflate the survivors "
           "(control: this test would prove nothing if it did not)");
    }

    printf("%d check(s), %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
