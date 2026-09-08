// Districts: the budget is a share, the ground is a share, and the ratio is
// what the resolver multiplies pacification by.
//
// The property everything else rests on is that this feature COSTS NOTHING
// until it is used. An undivided country, and a district drawing exactly its
// own share of the ground, must both come out at 1.0 -- otherwise adding
// districts silently rebalances every existing save and every AI country that
// never opens the tab.
//
// Replicated rather than linked, like the other rule tests: reaching the real
// one means building a world. If the rule changes, change it here in the same
// commit.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
static void ok(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { printf("  ok    %s\n", what.c_str()); return; }
    ++g_fails; printf("  FAIL  %s\n", what.c_str());
}
static void section(const char* t) { printf("\n== %s ==\n", t); }

struct D { int provinces; int sharePct; };

static double factor(const std::vector<D>& ds, int which, int heldTotal) {
    if (ds.empty()) return 1.0;
    if (heldTotal <= 0) return 1.0;
    const D& d = ds[(size_t)which];
    if (d.provinces <= 0) return 0.0;
    int shareTotal = 0;
    for (const auto& x : ds) shareTotal += std::max(0, x.sharePct);
    if (shareTotal <= 0) return 0.0;
    const double budgetShare = (double)std::max(0, d.sharePct) / (double)shareTotal;
    const double groundShare = (double)d.provinces / (double)heldTotal;
    return std::min(6.0, budgetShare / groundShare);
}

static void equalise(std::vector<D>& ds) {
    const int n = (int)ds.size();
    const int each = 100 / n;
    for (auto& d : ds) d.sharePct = each;
    int left = 100 - each * n;
    for (int i = 0; i < n && left > 0; ++i, --left) ds[(size_t)i].sharePct += 1;
}

int main() {
    section("an undivided country is the game as it was");
    {
        ok(factor({}, 0, 40) == 1.0, "no districts at all multiplies by one");
        std::vector<D> whole = {{40, 100}};
        ok(factor(whole, 0, 40) == 1.0, "one district holding everything multiplies by one");
    }

    section("a district drawing its own share of the ground changes nothing");
    {
        // THE INERTNESS PROPERTY. Four districts, each a quarter of the country
        // and a quarter of the money: every province is policed exactly as the
        // single old slider policed it.
        std::vector<D> even = {{10, 25}, {10, 25}, {10, 25}, {10, 25}};
        for (int i = 0; i < 4; ++i)
            ok(std::abs(factor(even, i, 40) - 1.0) < 1e-9,
               "a quarter of the ground on a quarter of the budget is unchanged");

        // And proportional-but-unequal is the same statement.
        std::vector<D> prop = {{30, 75}, {10, 25}};
        ok(std::abs(factor(prop, 0, 40) - 1.0) < 1e-9, "three quarters of each is unchanged");
        ok(std::abs(factor(prop, 1, 40) - 1.0) < 1e-9, "and so is the other quarter");
    }

    section("concentrating the budget is the whole point");
    {
        std::vector<D> hot = {{4, 50}, {36, 50}};
        ok(factor(hot, 0, 40) == 5.0, "a tenth of the ground on half the money is policed 5x");
        ok(std::abs(factor(hot, 1, 40) - (0.5 / 0.9)) < 1e-9, "and the rest pays for it");

        std::vector<D> starved = {{4, 0}, {36, 100}};
        ok(factor(starved, 0, 40) == 0.0, "a district given nothing is policed not at all");
    }

    section("the cap, so rebellion stays possible");
    {
        // Without it a single-province district holding the whole budget of a
        // large country multiplies suppression by the province count, and no
        // amount of grievance can ever reach the threshold.
        std::vector<D> pin = {{1, 100}, {199, 0}};
        ok(factor(pin, 0, 200) == 6.0, "200x is capped to 6x");
    }

    section("shares add to one hundred, and equally means equally");
    {
        std::vector<D> three = {{5, 0}, {5, 0}, {5, 0}};
        equalise(three);
        const int sum = three[0].sharePct + three[1].sharePct + three[2].sharePct;
        ok(sum == 100, "three districts still sum to 100");
        ok(three[0].sharePct == 34 && three[1].sharePct == 33 && three[2].sharePct == 33,
           "and the remainder is handed out, not dropped");

        std::vector<D> four = {{1,0},{1,0},{1,0},{1,0}};
        equalise(four);
        ok(four[0].sharePct == 25 && four[3].sharePct == 25, "four divide exactly");

        std::vector<D> one = {{9, 7}};
        equalise(one);
        ok(one[0].sharePct == 100, "one district owns the whole budget");
    }

    section("degenerate shapes do not divide by zero");
    {
        std::vector<D> empty = {{0, 50}, {10, 50}};
        ok(factor(empty, 0, 10) == 0.0, "a district with no ground is policed not at all");
        std::vector<D> nomoney = {{5, 0}, {5, 0}};
        ok(factor(nomoney, 0, 10) == 0.0, "no budget anywhere is zero, not a division by zero");
        ok(factor({{5, 50}}, 0, 0) == 1.0, "a country holding nothing does not divide by zero");
    }

    section("ground with no district joins the NEAREST one");
    {
        // Game::reconcileDistricts, replicated. What it guards: conquer a
        // province on your eastern border and it used to land in whichever
        // district happened to be first in the list, usually on the far side of
        // the country. Adjacency wins outright over distance, because a border
        // is a better answer than a centre-to-centre measurement -- and an
        // island touches nothing, so distance has to catch what adjacency
        // cannot.
        struct P { double x, y; };
        auto nearest = [](const std::vector<std::vector<int>>& ds,
                          const std::vector<P>& at, int orphan,
                          const std::vector<std::pair<int,int>>& adj) {
            size_t best = 0; double bestD = 1e18;
            for (size_t d = 0; d < ds.size(); ++d) {
                for (int other : ds[d]) {
                    bool touching = false;
                    for (auto& e : adj)
                        if ((e.first == orphan && e.second == other) ||
                            (e.second == orphan && e.first == other)) touching = true;
                    if (touching) return d;
                    const double dx = at[(size_t)orphan].x - at[(size_t)other].x;
                    const double dy = at[(size_t)orphan].y - at[(size_t)other].y;
                    const double d2 = dx * dx + dy * dy;
                    if (d2 < bestD) { bestD = d2; best = d; }
                }
            }
            return best;
        };
        //            0      1      2      3       4        5
        std::vector<P> at = {{0,0},{0,0},{1,0},{10,0},{11,0},{100,0}};
        std::vector<std::vector<int>> ds = {{1, 2}, {3, 4}};   // west, east

        ok(nearest(ds, at, 5, {}) == 1, "far ground joins the district it is closest to");
        at[5] = {2, 0};
        ok(nearest(ds, at, 5, {}) == 0, "and the other one when it is closer to that");

        // Adjacency beats distance: province 5 sits nearer the western centres
        // but shares a border with the east.
        at[5] = {3, 0};
        ok(nearest(ds, at, 5, {{5, 3}}) == 1,
           "a district it actually touches wins over a nearer one it does not");

        // An island touches nothing and still has to land somewhere.
        at[5] = {50, 0};
        ok(nearest(ds, at, 5, {}) == 1, "an island is not left in no district at all");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
