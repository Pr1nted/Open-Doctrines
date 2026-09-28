// The monument catalogue and its arithmetic. See Monuments.h for the design.

#include "Monuments.h"

#include <algorithm>
#include <cmath>

namespace odmon {
namespace {

/**
 * THE CATALOGUE, in Kind order, and the only place a monument's numbers live.
 *
 * Read the columns as: how far it can be upgraded, what the first level costs,
 * what each further level costs, how many province steps it reaches at level
 * one, whether it can be moved, whether it needs a port, whether ordnance
 * destroys it, and the band its effect runs between.
 *
 * The bands are mostly 20%-50%, which is what was asked for, and the ones that
 * are not are the ones whose effect is not a percentage of anything: the silo's
 * band is unused (its reach is in kilometres, see siloRangeKm) and the air
 * defence's is the share of incoming ordnance it stops.
 */
const Spec kSpecs[kKindCount] = {
    // kind                     max  build  upgr  rad  move   port   fragile  min    max
    {Kind::University,            3,  150.f, 120.f,  0, false, false, false,  0.20f, 0.50f},
    {Kind::Megacity,              3,  200.f, 160.f,  1, false, false, false,  0.20f, 0.50f},
    // The dear one, on purpose: "around 200 to upgrade from level 1 to level 2".
    // Level 3 is 400 on top of that, and what it buys is the whole planet.
    {Kind::MissileSilo,           3,  180.f, 200.f,  0, false, false, false,  0.00f, 0.00f},
    // Movable, and gone if anything heavier than heavy artillery finds it.
    {Kind::DefenceCorporation,    3,  220.f, 180.f,  1, true,  false, true,   0.15f, 0.40f},
    {Kind::FactoryConglomerate,   3,  180.f, 150.f,  1, false, false, false,  0.20f, 0.50f},
    // Its band is the share of incoming ordnance stopped, not a bonus.
    {Kind::AirDefence,            3,  200.f, 170.f,  1, false, false, false,  0.35f, 0.85f},
    {Kind::StrategicReserve,      3,  160.f, 140.f,  2, false, false, false,  0.25f, 0.60f},
    {Kind::GrandExchange,         3,  200.f, 170.f,  0, false, true,  false,  0.15f, 0.40f},
    {Kind::AdmiraltyYard,         3,  220.f, 180.f,  2, false, true,  false,  0.20f, 0.50f},
    {Kind::MinistryOfEnlightenment, 3, 170.f, 140.f, 1, false, false, false,  0.20f, 0.50f},
    // The second movable one, and as fragile as the first for the same reason.
    {Kind::SignalsDirectorate,    3,  190.f, 160.f,  2, true,  false, true,   0.20f, 0.50f},
};

static_assert(sizeof(kSpecs) / sizeof(kSpecs[0]) == (size_t)kKindCount,
              "every Kind needs a row in the catalogue");

bool valid(Kind k) { return (int)k >= 0 && (int)k < kKindCount; }

}  // namespace

const char* kindKey(Kind k) {
    switch (k) {
        case Kind::University:              return "university";
        case Kind::Megacity:                return "megacity";
        case Kind::MissileSilo:             return "missile_silo";
        case Kind::DefenceCorporation:      return "defence_corporation";
        case Kind::FactoryConglomerate:     return "factory_conglomerate";
        case Kind::AirDefence:              return "air_defence";
        case Kind::StrategicReserve:        return "strategic_reserve";
        case Kind::GrandExchange:           return "grand_exchange";
        case Kind::AdmiraltyYard:           return "admiralty_yard";
        case Kind::MinistryOfEnlightenment: return "ministry_of_enlightenment";
        case Kind::SignalsDirectorate:      return "signals_directorate";
        default:                            return "";
    }
}

// NOT T() here: this file is linked by tests that have no translation layer,
// and a monument's name is drawn in about four places. The callers translate.
const char* kindName(Kind k) {
    switch (k) {
        case Kind::University:              return "University";
        case Kind::Megacity:                return "Megacity";
        case Kind::MissileSilo:             return "Missile Silo";
        case Kind::DefenceCorporation:      return "Defence Corporation";
        case Kind::FactoryConglomerate:     return "Factory Conglomerate";
        case Kind::AirDefence:              return "Air Defence System";
        case Kind::StrategicReserve:        return "Strategic Reserve";
        case Kind::GrandExchange:           return "Grand Exchange";
        case Kind::AdmiraltyYard:           return "Admiralty Yard";
        case Kind::MinistryOfEnlightenment: return "Ministry of Enlightenment";
        case Kind::SignalsDirectorate:      return "Signals Directorate";
        default:                            return "";
    }
}

const char* kindBlurb(Kind k) {
    switch (k) {
        case Kind::University:
            return "Raises what the whole country gets from its research budget.";
        case Kind::Megacity:
            return "People move here, and migrants choose it if your government "
                   "is friendly to them.";
        case Kind::MissileSilo:
            return "Fires any ordnance you have researched, across the planet at "
                   "the highest level.";
        case Kind::DefenceCorporation:
            return "Troops attacking and defending nearby survive more. Can be "
                   "moved, and heavy ordnance destroys it.";
        case Kind::FactoryConglomerate:
            return "Factories here and next door produce more.";
        case Kind::AirDefence:
            return "Stops ordnance landing on it and the provinces around it.";
        case Kind::StrategicReserve:
            return "Holds more of everything, and keeps armies in range supplied.";
        case Kind::GrandExchange:
            return "You sell for more and buy for less. Must stand on a port.";
        case Kind::AdmiraltyYard:
            return "Repairs ships in range every turn. Must stand on a port.";
        case Kind::MinistryOfEnlightenment:
            return "Minorities nearby come round faster -- unless you are at war "
                   "with their kin.";
        case Kind::SignalsDirectorate:
            return "Shows you what the enemy has nearby and spoils their aim. Can "
                   "be moved, and heavy ordnance destroys it.";
        default: return "";
    }
}

std::string unlockNode(Kind k) { return std::string("mon_") + kindKey(k); }

const Spec& spec(Kind k) {
    static const Spec fallback{};
    return valid(k) ? kSpecs[(int)k] : fallback;
}

// ── What one looks like ─────────────────────────────────────────────────────

FigureBox figureBox(int level, float zoom, bool onGlobe, float face) {
    const float lv = (float)std::clamp(level, 1, 99);
    // Big enough to find, small enough not to cover the province. Level is in
    // HERE, once, and must not be applied again by the caller.
    const float base = std::clamp(11.0f + 4.0f * lv, 11.0f, 26.0f) *
                       std::clamp(zoom * 1.4f, 0.6f, 1.8f);
    if (!onGlobe) return {base * 1.15f, base * 1.15f};
    // Standing up, and a little taller than wide because it is standing. `face`
    // is the only thing that may change the proportion.
    return {base, base * 1.35f * std::clamp(face, 0.0f, 1.0f)};
}

std::vector<Part> silhouette(Kind k) {
    using S = PartShape;
    switch (k) {
        // A colonnade under a pediment: the one building everybody draws when
        // they mean "a place of learning".
        case Kind::University:
            return {{S::Box,      0.06f, 0.88f, 0.00f, 0.14f},   // stylobate
                    {S::Box,      0.14f, 0.09f, 0.14f, 0.46f},   // columns
                    {S::Box,      0.31f, 0.09f, 0.14f, 0.46f},
                    {S::Box,      0.48f, 0.09f, 0.14f, 0.46f},
                    {S::Box,      0.65f, 0.09f, 0.14f, 0.46f},
                    {S::Box,      0.10f, 0.80f, 0.60f, 0.10f},   // architrave
                    {S::Pediment, 0.06f, 0.88f, 0.70f, 0.30f}};

        // A skyline: three towers, none the same height. Read as a city and
        // not as a factory because nothing on it is horizontal.
        case Kind::Megacity:
            return {{S::Box, 0.04f, 0.26f, 0.00f, 0.62f},
                    {S::Box, 0.36f, 0.28f, 0.00f, 0.88f},
                    {S::Box, 0.70f, 0.26f, 0.00f, 0.78f},
                    // The mast is inside the box like everything else: the
                    // tallest tower gives up its last 12% to make room, rather
                    // than the figure growing a part that sticks out of its
                    // own bounds and gets clipped by whatever draws it.
                    {S::Needle, 0.48f, 0.04f, 0.88f, 0.12f}};

        // A hardened cap with the nose of the thing itself showing.
        case Kind::MissileSilo:
            // The GROUND WORKS are most of it, with the missile coming out --
            // a big rocket on a small mound is a launch pad, which is a
            // different building.
            return {{S::Box,    0.00f, 1.00f, 0.00f, 0.26f},      // apron
                    {S::Box,    0.10f, 0.34f, 0.26f, 0.16f},      // the rolled-back lid
                    {S::Slot,   0.52f, 0.34f, 0.17f, 0.09f},      // the open mouth
                    {S::Box,    0.61f, 0.16f, 0.26f, 0.43f},      // the missile, out of it
                    {S::Needle, 0.61f, 0.16f, 0.69f, 0.16f}};

        // A casemate: a wide sloped revetment with a low turret on it. The
        // first version put a PEDIMENT on a box, which is a house -- the one
        // silhouette a bunker must not have.
        case Kind::DefenceCorporation:
            // FLAT AND SQUARE, with the embrasure cut into it. A bevel that
            // wide read as a hill; what makes a bunker a bunker is that it is
            // a slab with a slot in it.
            return {{S::Box,   0.00f, 1.00f, 0.00f, 0.34f},       // the slab
                    {S::Slot,  0.14f, 0.50f, 0.14f, 0.09f},       // the embrasure
                    {S::Bevel, 0.10f, 0.60f, 0.34f, 0.14f},       // the sloped roof
                    {S::Box,   0.38f, 0.18f, 0.48f, 0.10f},       // a cupola
                    {S::Dome,  0.38f, 0.18f, 0.58f, 0.07f}};

        // Sheds and chimneys. The two chimneys are what say "industry" at
        // twelve pixels; the saw-tooth roof is the detail that survives zoom.
        case Kind::FactoryConglomerate:
            return {{S::Box,      0.02f, 0.96f, 0.00f, 0.44f},
                    {S::Pediment, 0.06f, 0.30f, 0.44f, 0.18f},
                    {S::Pediment, 0.38f, 0.30f, 0.44f, 0.18f},
                    {S::Box,      0.72f, 0.10f, 0.44f, 0.52f},    // chimney
                    {S::Box,      0.86f, 0.10f, 0.44f, 0.40f}};   // and a shorter one

        // A dish on a mast: a trapezoid opening upward, which reads as a dish
        // at twelve pixels. Cutting a bowl out of an ellipse gave a crescent
        // moon, which is what it looked like and not what it was.
        case Kind::AirDefence:
            return {{S::Box,   0.12f, 0.60f, 0.00f, 0.16f},       // the vehicle
                    // The mount sits under the panel's LOW CORNER, which a
                    // leaning shape puts at 0.45 of its width, not at its
                    // left edge -- so a centred mount leaves the dish hanging
                    // in the air beside it.
                    {S::Box,   0.44f, 0.16f, 0.16f, 0.28f},       // the mount
                    // A PANEL, LEANING. The trapezoid version came out as a
                    // wine glass: wide bowl, thin stem, foot. A tilted flat
                    // face on a stubby mount is what a radar looks like from
                    // any distance, and it is the only leaning thing here.
                    {S::Panel, 0.26f, 0.68f, 0.40f, 0.50f}};

        // Tanks: squat cylinders with domed caps and a gap between them, plus
        // the pipe gantry that says "storage" rather than "towers". They were
        // tall and touching before, which read as two thumbs.
        case Kind::StrategicReserve:
            // TWO SEPARATE TANKS AND NO PIPE. The pipe joined their tops and
            // the pair came out as an arch -- the one shape that says
            // "bridge". Squat, because a tall cylinder is a tower.
            // WIDER THAN TALL, with a shallow cap and a LADDER up one side.
            // Tanks as tall as they were wide came out as headstones; a seam
            // drawn the full width across them came out as a pair of burgers.
            // A vertical mark says cylinder, a horizontal one says stack.
            return {{S::Box,  0.00f, 1.00f, 0.00f, 0.10f},        // hardstanding
                    {S::Box,  0.02f, 0.44f, 0.10f, 0.26f},        // tank
                    {S::Dome, 0.02f, 0.44f, 0.36f, 0.07f},
                    {S::Slot, 0.07f, 0.05f, 0.12f, 0.22f},        // its side ladder
                    {S::Box,  0.54f, 0.44f, 0.10f, 0.38f},        // and a fuller one
                    {S::Dome, 0.54f, 0.44f, 0.48f, 0.07f},
                    {S::Slot, 0.59f, 0.05f, 0.12f, 0.34f}};

        // A rotunda: one dome on a broad hall, which is what an exchange has
        // looked like since the eighteenth century.
        case Kind::GrandExchange:
            return {{S::Box,      0.02f, 0.96f, 0.00f, 0.34f},
                    {S::Box,      0.14f, 0.72f, 0.34f, 0.16f},
                    {S::Dome,     0.22f, 0.56f, 0.50f, 0.38f},
                    {S::Needle,   0.48f, 0.04f, 0.88f, 0.12f}};

        // A gantry crane over a slipway. The jib is the whole identity.
        case Kind::AdmiraltyYard:
            return {{S::Box, 0.02f, 0.96f, 0.00f, 0.16f},         // quay
                    {S::Box, 0.14f, 0.12f, 0.16f, 0.68f},         // mast
                    {S::Jib, 0.14f, 0.76f, 0.76f, 0.09f},         // the arm
                    {S::Box, 0.66f, 0.05f, 0.40f, 0.36f},         // the hoist line
                    {S::Box, 0.60f, 0.17f, 0.30f, 0.10f}};        // and its load

        // A tower with a lantern: a beacon, which is the oldest picture there
        // is of a state telling people things.
        case Kind::MinistryOfEnlightenment:
            // A HALL WITH A BELL TOWER OFF TO ONE SIDE. A tower with a dome
            // on it, centred, came out as a fire hydrant -- and symmetry is
            // what did it, so this one is deliberately lopsided.
            return {{S::Box,      0.00f, 0.62f, 0.00f, 0.40f},    // the hall
                    {S::Slot,     0.22f, 0.16f, 0.00f, 0.22f},    // its door
                    {S::Pediment, 0.00f, 0.62f, 0.40f, 0.16f},    // its roof
                    {S::Box,      0.62f, 0.26f, 0.00f, 0.74f},    // the tower
                    {S::Box,      0.56f, 0.38f, 0.74f, 0.08f},    // its gallery
                    {S::Pediment, 0.60f, 0.30f, 0.82f, 0.18f}};   // and its spire

        // A mast with arms hung ALTERNATELY off each side. Two symmetrical
        // arms made a crucifix; three centred ones made a Christmas tree.
        case Kind::SignalsDirectorate:
            return {{S::Box,    0.34f, 0.32f, 0.00f, 0.08f},      // the pad
                    {S::Box,    0.44f, 0.12f, 0.08f, 0.76f},      // the mast
                    // ALTERNATING, not centred. Arms of falling length on a
                    // vertical make a fir tree; the same arms hung off one
                    // side and then the other make a transmission mast.
                    {S::Jib,    0.14f, 0.48f, 0.30f, 0.06f},
                    {S::Jib,    0.40f, 0.46f, 0.46f, 0.05f},
                    {S::Jib,    0.20f, 0.38f, 0.60f, 0.05f},
                    {S::Jib,    0.44f, 0.34f, 0.72f, 0.05f},
                    {S::Needle, 0.44f, 0.12f, 0.84f, 0.16f}};

        default:
            // A kind with no drawing is a plain block rather than nothing at
            // all: a monument that is invisible on the map is worse than one
            // that is unrecognisable, because the second can be clicked.
            return {{S::Box, 0.15f, 0.70f, 0.00f, 0.70f}};
    }
}

// ── Money ───────────────────────────────────────────────────────────────────

float slotCost(int n) {
    if (n < 1) return 0.0f;
    // 50 + 25 * T(n-1), T the triangular numbers: 50, 75, 125, 200, 300, 425.
    const float t = (float)(n - 1) * (float)n * 0.5f;
    return 50.0f + 25.0f * t;
}

float slotUpkeep(int active) {
    float sum = 0.0f;
    for (int i = 1; i <= active; ++i) sum += slotCost(i);
    return sum;
}

float levelCost(Kind kind, int level) {
    if (!valid(kind)) return 0.0f;
    const Spec& s = spec(kind);
    if (level <= 0) return s.buildCost;
    if (level >= s.maxLevel) return 0.0f;   // nothing left to buy
    // Multiplied by the level being bought, so the last one is the dear one.
    return s.upgradeCost * (float)level;
}

float moveCost(Kind kind, int level) {
    if (!valid(kind) || !spec(kind).movable) return 0.0f;
    // Half of what it cost to get here, so moving a level 3 is a campaign
    // decision and moving a fresh one is merely expensive.
    float paid = spec(kind).buildCost;
    for (int l = 1; l < level; ++l) paid += levelCost(kind, l);
    return paid * 0.5f;
}

// ── Effects ─────────────────────────────────────────────────────────────────

float scaledEffect(Kind kind, float popShare, int level) {
    if (!valid(kind) || level <= 0) return 0.0f;
    const Spec& s = spec(kind);
    // SATURATES AT A THIRD. A province holding a third of its country is
    // already that country's centre; past that the curve would only be
    // rewarding maps where one province holds everything.
    const float t = std::clamp(popShare, 0.0f, 1.0f / 3.0f) * 3.0f;
    const float base = s.effectMin + (s.effectMax - s.effectMin) * t;
    // A quarter of the base again per level above the first.
    return base * (1.0f + 0.25f * (float)(level - 1));
}

float stackedEffect(Kind kind, float popShare, int level, int rank) {
    if (rank < 1) return 0.0f;
    return scaledEffect(kind, popShare, level) / (float)rank;
}

int radius(Kind kind, int level) {
    if (!valid(kind) || level <= 0) return 0;
    return spec(kind).baseRadius + (level - 1);
}

float siloRangeKm(int level) {
    switch (level) {
        case 1:  return 1500.0f;    // a theatre: neighbours and their neighbours
        case 2:  return 6000.0f;    // an ocean
        // Larger than half the Earth's circumference (about 20,015 km), so at
        // the top level the curve stops mattering and the answer is always yes.
        // That is what the level is for, and it is cheaper to explain than a
        // range that reaches almost everywhere.
        default: return level >= 3 ? 21000.0f : 0.0f;
    }
}

std::vector<Holding> chargeOrder(const std::vector<Holding>& all) {
    std::vector<Holding> out = all;
    std::sort(out.begin(), out.end(), [](const Holding& a, const Holding& b) {
        // Active first: an inactive one costs nothing and must not take a
        // cheap slot away from one that is running.
        if (a.active != b.active) return a.active;
        // Then the bigger investment, so the cheapest slot goes to the
        // monument the country has put the most into.
        if (a.level != b.level) return a.level > b.level;
        // Then a total tie-break that cannot tie: a province id. Without it
        // two level-2 universities would order by whatever the map happened to
        // hand over, and a replay on another machine would charge them
        // differently.
        if (a.kind != b.kind) return (int)a.kind < (int)b.kind;
        return a.provinceId < b.provinceId;
    });
    return out;
}

int activeCount(const std::vector<Holding>& all) {
    int n = 0;
    for (const Holding& h : all) if (h.active) ++n;
    return n;
}

}  // namespace odmon
