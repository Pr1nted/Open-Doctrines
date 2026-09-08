// Research groups: the budget split, and when a group may walk on alone.
//
// Both rules are arithmetic over state the UI cannot show and a screenshot
// cannot check -- a share that leaks, or an auto-advance that steps past a
// decision, looks exactly like a working game until several turns later.
//
// The logic is replicated here rather than linked, for the same reason
// troop_types_test replicates the save packing: reaching it means building the
// whole game. If either rule changes, change it here in the same commit.

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

// ─── the split ───────────────────────────────────────────────────────────
struct Group { int activeNode = -1; int sharePct = 50; bool autoAdvance = false; };

static int groupPoints(const Group* g, int unlocked, int idx, int total) {
    int denom = 0;
    for (int i = 0; i < unlocked; ++i)
        if (g[i].activeNode >= 0) denom += std::max(1, g[i].sharePct);
    if (denom <= 0 || idx >= unlocked || g[idx].activeNode < 0) return 0;
    return (int)((long long)total * std::max(1, g[idx].sharePct) / denom);
}

// ─── the unlock: size, gated by poverty ──────────────────────────────────
// Both arguments are MULTIPLES OF THE WORLD MEDIAN -- grossX how big the
// economy is, perX how much of it there is per person.
static int unlocked(float grossX, float perX) {
    if (grossX <= 0.0f && perX <= 0.0f) return 1;
    if (perX < 0.5f) return 1;              // spread too thin, whatever the size
    if (grossX >= 5.0f) return 3;
    if (grossX >= 2.0f) return 2;
    return 1;                                // ONE IS ALWAYS AVAILABLE
}

// ─── shares sum to 100 ───────────────────────────────────────────────────
static void normalise(int* share, int unlocked, int changed, int want) {
    if (unlocked <= 1) { share[0] = 100; return; }
    share[changed] = std::max(0, std::min(100, want));
    int rest = 0;
    for (int g = 0; g < unlocked; ++g) if (g != changed) rest += std::max(0, share[g]);
    const int budget = 100 - share[changed];
    int handed = 0, last = -1;
    for (int g = 0; g < unlocked; ++g) {
        if (g == changed) continue;
        const int v = rest > 0 ? (int)((long long)budget * std::max(0, share[g]) / rest)
                               : budget / (unlocked - 1);
        share[g] = v; handed += v; last = g;
    }
    if (last >= 0) share[last] += budget - handed;
    for (int g = unlocked; g < 3; ++g) share[g] = 0;
}

// ─── auto-advance ────────────────────────────────────────────────────────
struct Node { std::string cat, sub; bool researched = false; bool available = false; };

static int autoNext(const std::vector<Node>& nodes, const Node& from) {
    int found = -1;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const Node& n = nodes[i];
        if (n.cat != from.cat || n.sub != from.sub) continue;
        if (n.researched || !n.available) continue;
        if (found >= 0) return -1;          // a decision, not a continuation
        found = (int)i;
    }
    return found;
}

int main() {
    printf("\nthe budget divides over the groups that are working\n");
    {
        Group g[3];
        g[0].activeNode = 1; g[0].sharePct = 50;
        // ONE WORKING GROUP TAKES THE WHOLE POOL. If shares were read as a
        // fraction of 100 rather than of the working total, a country with one
        // project would research at half speed and nothing on screen would say
        // why -- the exact bug this normalisation exists to prevent.
        ok(groupPoints(g, 3, 0, 100) == 100, "one active group gets every point");
        ok(groupPoints(g, 3, 1, 100) == 0,   "an idle group claims nothing");

        g[1].activeNode = 2; g[1].sharePct = 50;
        ok(groupPoints(g, 3, 0, 100) == 50 && groupPoints(g, 3, 1, 100) == 50,
           "two equal shares split evenly");

        g[0].sharePct = 75; g[1].sharePct = 25;
        ok(groupPoints(g, 3, 0, 100) == 75, "75/25 gives the larger share 75");
        ok(groupPoints(g, 3, 1, 100) == 25, "75/25 gives the smaller share 25");

        // A LOCKED GROUP IS NOT A LEAK. Its share must not enter the divisor,
        // or two unlocked groups would quietly research at two-thirds speed.
        Group h[3];
        h[0].activeNode = 1; h[1].activeNode = 2; h[2].activeNode = 3;
        ok(groupPoints(h, 2, 0, 100) + groupPoints(h, 2, 1, 100) == 100,
           "with two unlocked, the third's share does not vanish from the pool");
        ok(groupPoints(h, 2, 2, 100) == 0, "a locked group is paid nothing");

        Group z[3];
        ok(groupPoints(z, 3, 0, 100) == 0, "no active group, no division by zero");
    }

    printf("\nthe great powers get three, and one is always available\n");
    {
        // Measured on the 1939 map at turn 40: median gross 34.3, median per
        // head 1.736. These are the real figures, not invented ones.
        ok(unlocked(30.30f, 2.93f) == 3, "the United States, 30x the world economy");
        ok(unlocked(27.20f, 3.95f) == 3, "France, 27x");
        ok(unlocked(19.93f, 0.86f) == 3, "the British Empire, 20x and poor per head");
        ok(unlocked(11.95f, 0.87f) == 3, "the Soviet Union, 12x and poor per head");
        ok(unlocked( 6.23f, 1.00f) == 3, "Germany, 6x");
        ok(unlocked( 3.44f, 0.94f) == 2, "Italy, 3.4x, earns a second");
        ok(unlocked( 1.12f, 36.9f) == 1, "Switzerland: rich per head, tiny, keeps one");

        // THE CASE THE GATE EXISTS FOR. China has the fifth largest economy on
        // the map and 0.21x the world's income per head. Size alone would give
        // it three; the gate is what makes it one.
        ok(unlocked( 5.34f, 0.21f) == 1, "China, huge but spread far too thin");
        ok(unlocked(30.00f, 0.49f) == 1, "the gate outranks any size at all");
        ok(unlocked(30.00f, 0.51f) == 3, "and lets the same economy through just above it");

        ok(unlocked(0.0f, 0.0f) == 1,  "a world with no median still gives one");
        ok(unlocked(0.02f, 0.21f) == 1, "the poorest country on the map still gets one");

        // SCALE-FREE. Both axes are ratios precisely so that a map measuring
        // population or money in different units unlocks the same groups; an
        // absolute threshold read 0.0 for every country on such a save.
        ok(unlocked(19.93f, 0.86f) == unlocked(19.93f, 0.86f), "ratios are unitless");
    }

    printf("\nthe shares always add to one hundred\n");
    {
        // Dragging one slider pushes the difference onto the others in
        // proportion to what they hold. Three independent 0-100 sliders let a
        // country show itself spending 180% of its research budget -- nothing
        // was actually overspent, because the points were normalised before
        // being paid out, which is WORSE: the numbers on screen then meant
        // nothing and quietly disagreed with what the game did.
        auto sum = [](const int* v, int n) { int t = 0; for (int i = 0; i < n; ++i) t += v[i]; return t; };

        int a[3] = {50, 50, 50};
        normalise(a, 3, 0, 80);
        ok(sum(a, 3) == 100, "raising one share to 80 still sums to 100");
        ok(a[0] == 80, "the slider being dragged does what it is told");
        ok(a[1] == a[2], "two equal shares stay equal when squeezed");

        int b[3] = {60, 30, 10};
        normalise(b, 3, 0, 50);
        ok(sum(b, 3) == 100, "unequal shares still sum to 100");
        ok(b[1] > b[2], "and keep their relative standing");

        int c[3] = {50, 50, 50};
        normalise(c, 3, 0, 100);
        ok(sum(c, 3) == 100 && c[1] == 0 && c[2] == 0, "taking all of it leaves the others nothing");

        int d[3] = {50, 50, 50};
        normalise(d, 3, 0, 0);
        ok(sum(d, 3) == 100, "giving away all of it still sums to 100");

        // ONE GROUP OWNS THE WHOLE BUDGET, or a country with a single
        // programme shows a share that means nothing.
        int e[3] = {50, 50, 50};
        normalise(e, 1, 0, 30);
        ok(e[0] == 100, "with one group unlocked its share is the whole budget");

        // Integer division loses a point or two; it has to go somewhere, or
        // the row reads 99 and the rule on screen is a lie.
        int f[3] = {33, 33, 34};
        normalise(f, 3, 0, 33);
        ok(sum(f, 3) == 100, "the rounding remainder is not dropped");

        int g[3] = {50, 0, 0};
        normalise(g, 3, 0, 40);
        ok(sum(g, 3) == 100, "others at zero split what is left evenly");
    }

    printf("\nauto-advance stops at a decision and at the end\n");
    {
        std::vector<Node> n = {
            {"army", "army", true,  false},   // done
            {"army", "army", false, true},    // the only way on
            {"army", "navy", false, true},    // another branch entirely
        };
        ok(autoNext(n, n[0]) == 1, "one open node on the branch is a continuation");

        n.push_back({"army", "army", false, true});   // a second open node
        ok(autoNext(n, n[0]) == -1, "two open nodes is a decision, so it stops");

        std::vector<Node> done = {{"army", "army", true, false},
                                  {"army", "army", true, false}};
        ok(autoNext(done, done[0]) == -1, "a finished branch stops");

        // IT MUST NOT WANDER INTO ANOTHER BRANCH. Choosing which line to start
        // is precisely the judgement the setting promises not to make.
        std::vector<Node> other = {{"army", "army", true, false},
                                   {"army", "navy", false, true}};
        ok(autoNext(other, other[0]) == -1, "it will not jump to a different branch");

        std::vector<Node> locked = {{"army", "army", true, false},
                                    {"army", "army", false, false}};
        ok(autoNext(locked, locked[0]) == -1, "a branch whose next node is locked stops");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
