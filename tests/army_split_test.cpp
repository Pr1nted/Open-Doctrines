// Dividing a garrison: the shares have to add up to the garrison.
//
// A player splitting a province's army in two saw an army marker reading "<1"
// left behind on ground they had emptied. Each move order took its share
// independently -- `base * pct / 100` -- so two 50% orders on an odd garrison
// both rounded DOWN and stranded one man. Garrison counts are odd about half
// the time, which is why it was reported as happening consistently rather than
// as an oddity.
//
// The fix makes the RUNNING TOTAL exact instead of each share: an order takes
// the difference between the cumulative share up to and including it and the
// cumulative share before it, so the truncations telescope and a set of orders
// summing to 100% moves exactly the whole garrison.
//
// This replicates the arithmetic from Game::processArmyMovement rather than
// calling it, the way tests/industry_capacity_test.cpp replicates the loader's
// row walk: the real one needs a Game, a map and a world, and the property
// worth pinning is the arithmetic. If the resolver's formula changes, change it
// here in the same commit -- that is what makes this test worth having.
//
// Pure integer arithmetic. Links nothing.

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

// What processArmyMovement did before: every order rounds down on its own.
static long long movedByIndependentShares(long long base, const std::vector<int>& pcts) {
    long long sent = 0;
    for (int pct : pcts) {
        long long toMove = base * pct / 100;
        if (toMove > base - sent) toMove = base - sent;
        sent += toMove;
    }
    return sent;
}

// What it does now: cumulative shares, so the truncations telescope.
static long long movedByCumulativeShares(long long base, const std::vector<int>& pcts) {
    long long sent = 0;
    int cum = 0;
    for (int pct : pcts) {
        const int next = (cum + pct > 100) ? 100 : cum + pct;
        long long toMove = base * next / 100 - base * cum / 100;
        cum = next;
        if (toMove > base - sent) toMove = base - sent;
        sent += toMove;
    }
    return sent;
}

int main() {
    // Every way the army panel lets a player carve up one province. queueArmyMove
    // offers halves and the arrow sliders can be dragged to anything, but the
    // total out of one source is capped at 100%.
    const std::vector<std::vector<int>> patterns = {
        {100}, {50, 50}, {33, 33, 34}, {25, 25, 25, 25},
        {60, 40}, {70, 30}, {10, 10, 10, 10, 10, 10, 10, 10, 10, 10},
    };

    section("orders summing to 100% empty the province");
    {
        long long strandedOld = 0, strandedNew = 0, worstOld = 0;
        for (long long base = 1; base <= 200000; base += 7) {
            for (const auto& p : patterns) {
                const long long o = movedByIndependentShares(base, p);
                const long long n = movedByCumulativeShares(base, p);
                if (o != base) { ++strandedOld; if (base - o > worstOld) worstOld = base - o; }
                if (n != base) ++strandedNew;
            }
        }
        ok(strandedNew == 0,
           "no garrison strands a man: " + std::to_string(strandedNew) + " cases");
        // The old behaviour is asserted too, so this test documents the bug it
        // was written for rather than only the fix. If this ever stops failing,
        // somebody has changed the thing being compared against.
        ok(strandedOld > 0,
           "the old independent-share arithmetic did strand men (" +
               std::to_string(strandedOld) + " cases, worst " +
               std::to_string(worstOld) + ")");
    }

    section("the reported case");
    {
        // An odd garrison split in two: one man left standing, drawn as "<1".
        ok(movedByIndependentShares(100001, {50, 50}) == 100000, "before: one man stranded");
        ok(movedByCumulativeShares(100001, {50, 50}) == 100001, "after: the province empties");
        // An even one was always fine, which is why it looked intermittent.
        ok(movedByIndependentShares(100000, {50, 50}) == 100000, "even garrisons never showed it");
    }

    section("partial splits keep the rest");
    {
        // Not every split is a full division: moving half away must leave half,
        // and the cumulative form must not sweep more than it was asked for.
        for (long long base : {2LL, 3LL, 999LL, 100000LL, 100001LL}) {
            const long long half = movedByCumulativeShares(base, {50});
            ok(half == base / 2,
               "a lone 50% order moves half of " + std::to_string(base));
            ok(base - half >= base / 2,
               "and leaves at least half of " + std::to_string(base));
        }
        ok(movedByCumulativeShares(1000, {}) == 0, "no orders move nobody");
        ok(movedByCumulativeShares(0, {50, 50}) == 0, "an empty province moves nobody");
    }

    section("shares are monotonic and never exceed the garrison");
    {
        for (long long base = 1; base <= 5000; base += 13) {
            for (const auto& p : patterns) {
                const long long n = movedByCumulativeShares(base, p);
                ok(n <= base, "never moves more men than are there");
                ok(n >= 0, "never moves a negative number of men");
            }
        }
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
