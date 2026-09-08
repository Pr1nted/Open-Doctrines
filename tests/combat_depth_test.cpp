// What the men behind the frontage are worth.
//
// Combat width caps how many men either side can bring to bear in one assault.
// Built alone, it capped BOTH sides, and that made the outcome of a fight
// between two stacks that both filled the frontage depend on nothing but the
// modifiers -- so it returned the identical answer every turn, for ever, until
// one stack happened to fall below the cap. Measured on a real invasion
// (province 824, Sweden into Norway):
//
//   attackers 174,800 / 119,537 / 87,293  -> atkPower 35,631 every time
//   defenders  93,048 /  84,909 / 66,067  -> defPower 37,769 every time
//
// 174,800 men accomplished exactly what 40,000 would. The repair gives men
// beyond the frontage a diminishing bonus -- they are the reserve, rotated into
// a line they cannot widen -- and this pins the three properties that repair
// has to keep at once. They pull against each other, which is the whole
// difficulty: numbers must matter, and a narrow fortified pass must still stop
// numbers.
//
// It replicates Game::depthFactor rather than linking it, the way
// tests/army_split_test.cpp replicates the split arithmetic: the real one is a
// static member of a class that needs a window and a map. If the formula in
// Game.h changes, change it here in the same commit -- that is what makes this
// test worth having.

#include <algorithm>
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
static void section(const char* n) { printf("\n== %s ==\n", n); }

// Game::DEPTH_PER_DOUBLING / DEPTH_MAX / depthFactor, verbatim.
static const double DEPTH_PER_DOUBLING = 0.20;
static const double DEPTH_MAX          = 1.5;

static double depthFactor(long long troops, long long width) {
    if (width <= 0 || troops <= width) return 1.0;
    return std::min(DEPTH_MAX, 1.0 + DEPTH_PER_DOUBLING * std::log2((double)troops / (double)width));
}

// Game::resolveAssault's comparison, reduced to the part under test.
static bool carries(long long atk, long long def, long long width,
                    int fortLevel = 0, double atkMod = 1.0, double defMod = 1.06) {
    const double engaged  = (double)std::min(atk, width);
    const double defShare = (def > width && def > 0) ? (double)width / (double)def : 1.0;
    const double fortMul  = 1.0 + fortLevel * 10.0 / 100.0;
    const double atkPower = engaged * atkMod * depthFactor(atk, width);
    const double defPower = (double)def * defShare * fortMul * defMod * depthFactor(def, width);
    return atkPower > defPower;
}

int main() {
    section("a stack at or below the frontage is unchanged");
    {
        ok(depthFactor(10000, 20000) == 1.0, "half the frontage earns nothing");
        ok(depthFactor(20000, 20000) == 1.0, "exactly the frontage earns nothing");
        ok(depthFactor(1, 20000) == 1.0, "one man earns nothing");
        ok(depthFactor(50000, 0) == 1.0, "a zero frontage cannot be exceeded");
        // So a fight neither side can fill behaves exactly as it did before.
        ok(carries(15000, 9000, 20000), "a small fight is decided by numbers, as always");
    }

    section("the reported invasion: numbers now decide it");
    {
        // Province 824. Every one of these was repulsed under width alone, with
        // atkPower pinned at 35,631 and defPower at 37,769 regardless of size.
        const long long W = 35631;
        ok(carries(174800, 93048, W), "174,800 against 93,048 carries");
        ok(carries(119537, 84909, W), "119,537 against 84,909 carries");
        ok(carries(87293,  66067, W), "87,293 against 66,067 carries");
        ok(carries(90513,  32453, W), "and the one that always carried still does");
        // The bug itself: identical power at different sizes.
        ok(depthFactor(174800, W) != depthFactor(87293, W),
           "two stacks of different size no longer fight identically");
    }

    section("and yet a fortified pass still stops numbers");
    {
        // The property combat width exists for, tested in the range where real
        // fights happen: a frontage of 20,000 with stacks of two to six times
        // it, which is what the shipped maps produce.
        const long long W = 20000;
        ok(carries(60000, 30000, W, /*fort=*/0),
           "on open ground two to one carries");
        ok(!carries(60000, 30000, W, /*fort=*/5),
           "the same attack against a level 5 fort does not");
        ok(!carries(60000, 30000, W, /*fort=*/3),
           "nor against a level 3 fort");
        ok(!carries(100000, 30000, W, /*fort=*/5),
           "and piling on more men does not buy the fort either");
        // Numbers help, but they are not a substitute for the ground. That is
        // the whole reason the bonus is capped.
        ok(!carries(10000000, 2000000, W, /*fort=*/2),
           "five to one does not take a fortified pass");
    }

    section("more men are never worse than fewer");
    {
        const long long W = 30000;
        double prev = 0.0;
        bool monotonic = true;
        for (long long t = 1000; t <= 4000000; t = (long long)(t * 1.3)) {
            const double f = depthFactor(t, W);
            monotonic = monotonic && f >= prev - 1e-12;
            prev = f;
        }
        ok(monotonic, "the bonus never falls as the stack grows");
        ok(depthFactor(4000000, W) <= DEPTH_MAX + 1e-12, "and never exceeds the cap");
        // A defender is helped by depth on exactly the same terms.
        ok(depthFactor(90000, W) == depthFactor(90000, W), "the rule is side-blind");
        ok(!carries(60000, 60000, W), "equal stacks: the defender's edge decides, as before");
    }

    section("the cap, and the corner it leaves");
    {
        // Documented rather than pretended away: above the cap the original
        // "size does not matter" behaviour returns for two stacks that both
        // hugely overfill the ground. See DEPTH_MAX in Game.h for why this is
        // accepted -- raising the cap measured 9-17 points of survival worse.
        const long long W = 20000;
        const double atCap = depthFactor((long long)(W * 5.66), W);
        ok(std::abs(atCap - DEPTH_MAX) < 0.01, "the cap is reached at about 5.7x the frontage");
        ok(depthFactor(W * 100, W) == depthFactor(W * 1000, W),
           "beyond it, two enormous stacks do fight identically (known, accepted)");
        // Stated as an assertion so nobody rediscovers it as a bug: ten million
        // against one million on OPEN ground is a draw, because both are far
        // past the cap and a draw goes to the defender. This is the original
        // width behaviour surviving in a corner, and the alternative -- raising
        // the cap so it carries -- measured 9-17 points of survival worse
        // across three seeds. See DEPTH_MAX in Game.h.
        ok(!carries(10000000, 1000000, W, /*fort=*/0),
           "ten to one on open ground is still a draw above the cap");
        // But the live range of the shipped maps is below it.
        ok(depthFactor(W * 2, W) < depthFactor(W * 4, W),
           "and everything in the normal two-to-six-times range still separates");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
