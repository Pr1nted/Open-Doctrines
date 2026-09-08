// Drawing a voyage that crosses the antimeridian.
//
// The overlay used to draw a straight line from a hull to its destination --
// the one path a ship never sails, because the router goes around land. It now
// draws the route itself, and that turned a one-line draw into a POLYLINE,
// which brought back a problem the mover had already been bitten by once.
//
// Game::worldToScreen picks the map copy nearest the camera FOR EACH POINT.
// That is right for a province marker and wrong for a line: a voyage from 179E
// to 179W has its two ends land on opposite copies of the world, and the
// segment between them draws as a stripe straight across the map. The mover hit
// the same class of bug in reverse -- an unwrapped dLon sailed a hull from 179E
// to "182E", off the map, and it measured as eleven hulls beached in an eval
// that had never reported one.
//
// So the overlay builds the polyline in CONTINUOUS world pixels: each point's x
// is the previous x plus lonDelta(prev, next) scaled to pixels, allowed to run
// outside the map, with the camera's wrap offset taken ONCE from the hull. This
// pins that arithmetic.
//
// It replicates the accumulation rather than calling it, the way
// tests/army_split_test.cpp replicates the resolver's split: the real one needs
// a Game, a camera and a raylib context, and the property worth pinning is the
// geometry. If Game::drawShipRoutePath's accumulation changes, change it here
// in the same commit -- that is what makes this test worth having.
//
// Pure arithmetic. Links nothing.

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

// Game::lonDelta, verbatim.
static double lonDelta(double a, double b) {
    double d = b - a;
    while (d >  180.0) d -= 360.0;
    while (d < -180.0) d += 360.0;
    return d;
}

// The polyline Game::drawShipRoutePath builds: continuous x in world pixels,
// starting at the hull. Returns x for the hull and each waypoint.
static std::vector<double> continuousX(double shipLon,
                                       const std::vector<double>& routeLons,
                                       double mapWidthPx) {
    const double pxPerDeg = mapWidthPx / 360.0;
    std::vector<double> xs;
    // The hull's own pixel column, as lonLatToPixel would give it.
    double cx = (shipLon + 180.0) * pxPerDeg;
    xs.push_back(cx);
    double prev = shipLon;
    for (double lon : routeLons) {
        cx += lonDelta(prev, lon) * pxPerDeg;
        xs.push_back(cx);
        prev = lon;
    }
    return xs;
}

int main() {
    const double MW = 8192.0;             // the shipped maps are 8192 wide
    const double PX = MW / 360.0;

    section("lonDelta takes the short way round");
    {
        ok(std::abs(lonDelta(179.0, -179.0) - 2.0) < 1e-9,
           "179E to 179W is 2 degrees east, not 358 west");
        ok(std::abs(lonDelta(-179.0, 179.0) + 2.0) < 1e-9,
           "179W to 179E is 2 degrees west");
        ok(std::abs(lonDelta(10.0, 20.0) - 10.0) < 1e-9, "an ordinary leg is itself");
        ok(std::abs(lonDelta(0.0, 180.0) - 180.0) < 1e-9, "exactly half way is east");
        ok(std::abs(lonDelta(5.0, 5.0)) < 1e-9, "no movement is no delta");
    }

    section("a voyage across the antimeridian draws as a short line");
    {
        // Tokyo-ish out into the Pacific and over the dateline.
        const std::vector<double> route = {175.0, 179.0, -177.0, -173.0};
        const std::vector<double> xs = continuousX(170.0, route, MW);

        // Every SEGMENT must be short. The bug this pins would make one of them
        // most of the width of the world.
        double worst = 0.0;
        for (size_t i = 0; i + 1 < xs.size(); ++i)
            worst = std::max(worst, std::abs(xs[i + 1] - xs[i]));
        ok(worst < MW * 0.5,
           "no segment jumps across the map (worst " + std::to_string(worst) + "px)");

        // And the whole voyage is 17 degrees of longitude, wherever it crosses.
        const double span = xs.back() - xs.front();
        ok(std::abs(span - 17.0 * PX) < 1e-6,
           "the voyage spans 17 degrees eastward");
        // Monotonic east, because every leg of this route goes east.
        bool mono = true;
        for (size_t i = 0; i + 1 < xs.size(); ++i) mono = mono && xs[i + 1] > xs[i];
        ok(mono, "an eastward voyage never doubles back on screen");
    }

    section("x is allowed to leave the map, and that is the point");
    {
        // A hull at 179E heading to 179W: the second point is off the right
        // edge in world pixels. Clamping or wrapping it is what tore the line.
        const std::vector<double> xs = continuousX(179.0, {-179.0}, MW);
        ok(xs[1] > MW, "the far end sits past the map's right edge, uncorrected");
        ok(std::abs((xs[1] - xs[0]) - 2.0 * PX) < 1e-6, "and is 2 degrees away");
    }

    section("westward crossings work the same way");
    {
        const std::vector<double> xs = continuousX(-175.0, {-179.0, 178.0, 174.0}, MW);
        double worst = 0.0;
        for (size_t i = 0; i + 1 < xs.size(); ++i)
            worst = std::max(worst, std::abs(xs[i + 1] - xs[i]));
        ok(worst < MW * 0.5, "no segment jumps across the map going west");
        ok(xs.back() < xs.front(), "a westward voyage runs left on screen");
        ok(std::abs((xs.back() - xs.front()) + 11.0 * PX) < 1e-6,
           "and spans 11 degrees westward");
    }

    section("a voyage that never goes near the dateline is untouched");
    {
        const std::vector<double> xs = continuousX(-10.0, {-5.0, 0.0, 5.0}, MW);
        ok(std::abs(xs[0] - (170.0 * PX)) < 1e-6, "the hull sits where the map puts it");
        ok(std::abs(xs.back() - (185.0 * PX)) < 1e-6, "and so does the destination");
    }

    section("the whole world is not a special case");
    {
        // Halfway round, taken as a sequence of legs each under 180 degrees.
        // Every leg is short, so the accumulation must add up to the long way.
        const std::vector<double> xs = continuousX(0.0, {90.0, 179.0, -91.0, -1.0}, MW);
        ok(std::abs((xs.back() - xs.front()) - 359.0 * PX) < 1e-6,
           "four eastward legs total 359 degrees, not -1");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
