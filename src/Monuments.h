#pragma once

/**
 * Monuments: one great work in a province, and what it does to the map around it.
 *
 * ── WHAT A MONUMENT IS ──
 *
 * A single building in a single province, of one of the kinds in the catalogue
 * below, at a level. Not a second industry track: industry is a number every
 * province has, and a monument is a thing a country decides to have ONE of,
 * somewhere specific, at a price that rises the more of them it runs.
 *
 * ── THE SLOT IS THE COST, AND IT IS THE WHOLE ECONOMY OF THIS ──
 *
 * Building one is cheap. RUNNING one takes a slot, and slots are priced so the
 * fifth hurts: 50, 75, 125, 200, 300, 425 a turn. That is a triangular series
 * and it is written as a formula rather than a table, because the next person
 * to want a sixth number will otherwise guess at it -- see slotCost().
 *
 * A monument can be switched INACTIVE, which frees its slot and turns its
 * effect off. That is the dial: a country with eleven monuments and money for
 * four decides which four, every turn, and can change its mind when a war
 * starts. Dismantling is separate and costs money, because a decision you can
 * unmake for free is not a decision.
 *
 * ── WHY THE EFFECTS ARE SHAPED THE WAY THEY ARE ──
 *
 * Every effect that scales, scales on the PROVINCE'S SHARE OF ITS COUNTRY'S
 * POPULATION, not on an absolute number. A university in a country's one big
 * city is a national institution; the same building in an empty province is a
 * college. Absolute thresholds would make every monument a question about which
 * map you are on, and the same monument would be worthless in 1914 China and
 * decisive in 1914 Norway.
 *
 * And STACKING DECAYS, harmonically: the second monument of a kind is worth
 * half, the third a third. Asked for outright -- "if there are more
 * universities, we should somehow nerf that people would be able to produce
 * ridiculous amounts of RP" -- and it is the right shape for all of them, not
 * just that one, because a linear stack turns every monument into "build as
 * many as the slots allow" and there is no decision left in it.
 *
 * ── FOUR CONSUMERS, ONE SET OF ANSWERS ──
 *
 * The panel, the map, a MAP SCRIPT and a MOD all have to be able to ask what a
 * province has and what it gives, and they must get the same answer. So the
 * game layer exposes flat accessors -- monumentAt, monumentLevel,
 * monumentActive, monumentEffectAt -- and all four go through them:
 *
 *   the UI            src/Game_Monuments.cpp and the panel
 *   a map script      province.<id>.monument / .monument_level /
 *                     .monument_active, in ScriptEngine::resolveRef
 *   a mod             gearbox:gamestate.read.province_monument* and the
 *                     write calls, in sdk/abi.json
 *   the AI            the same accessors, from a reflex
 *
 * The one thing none of them may do is work an effect out for itself from the
 * catalogue. An effect re-derived at a call site is a second copy of the rule,
 * and the two stop agreeing the first time a constant here moves.
 *
 * Pure: no Game, no raylib, no map. Every rule here is tested on its own in
 * tests/monuments_test.cpp, and the game half lives in src/Game_Monuments.cpp.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace odmon {

/** What a monument is. The order is the catalogue order and is saved, so
 *  entries are APPENDED and never reordered -- see kindKey(). */
enum class Kind : uint8_t {
    University = 0,      ///< research output
    Megacity,            ///< population gravity, and where migrants choose
    MissileSilo,         ///< artillery range, over the curve of the planet
    DefenceCorporation,  ///< survivability in an area; movable; fragile
    FactoryConglomerate, ///< factory output here and next door
    AirDefence,          ///< stops ordnance landing in its radius
    StrategicReserve,    ///< stockpiles, and supply for armies in range
    GrandExchange,       ///< what you get selling and pay buying; needs a port
    AdmiraltyYard,       ///< repairs ships in range; unlocks a hull class
    MinistryOfEnlightenment, ///< minority alignment, and its opposite in war
    SignalsDirectorate,  ///< sees and jams; the second movable one
    Count
};

/** How many there are, as a plain int for loops. */
inline constexpr int kKindCount = (int)Kind::Count;

/**
 * Stable, lowercase, never translated: saves, mods, the bench and the research
 * node ids are all built from these. Changing one breaks every save that has
 * the monument in it.
 */
const char* kindKey(Kind k);

/** Display name, translated at the point of drawing. */
const char* kindName(Kind k);

/** One line, for the panel and the research node. */
const char* kindBlurb(Kind k);

/** The research node that unlocks it: "mon_university" and so on. */
std::string unlockNode(Kind k);

// ── What one kind of monument is like ───────────────────────────────────────

struct Spec {
    Kind  kind = Kind::University;
    int   maxLevel = 3;
    /** Money to put the first level down. */
    float buildCost = 120.0f;
    /**
     * Money per level after the first, multiplied by the level being bought.
     *
     * The silo is the expensive one on purpose -- "around 200 to upgrade from
     * level 1 to level 2" -- and it buys the largest single thing in the game,
     * which is the ability to hit anywhere at all.
     */
    float upgradeCost = 100.0f;
    /**
     * How far its effect reaches, in PROVINCE STEPS, at level 1; each further
     * level adds one. 0 means the province it stands in and nothing else.
     */
    int   baseRadius = 1;
    /** Can it be picked up and put down somewhere else, for a price? */
    bool  movable = false;
    /** Does it need a coast, because what it does happens at sea? */
    bool  needsPort = false;
    /**
     * Is it destroyed outright by ordnance heavier than heavy artillery?
     *
     * The trade the movable ones make: they go where they are needed and they
     * do not survive being found.
     */
    bool  fragile = false;
    /** The low and high end of what it gives, as a fraction. See scaledEffect. */
    float effectMin = 0.20f;
    float effectMax = 0.50f;
};

/** The catalogue. Indexed by Kind. */
const Spec& spec(Kind k);

// ── Money ───────────────────────────────────────────────────────────────────

/**
 * What the n-th ACTIVE monument costs per turn, n counting from 1.
 *
 * 50, 75, 125, 200, 300, 425, 575 -- the asked-for series, which is 50 plus a
 * triangular number of 25s. Written as the formula it is, because a table
 * stops at whatever number somebody typed last and the eighth slot then costs
 * nothing.
 */
float slotCost(int n);

/** What running `active` monuments costs per turn, all slots summed. */
float slotUpkeep(int active);

/** Money to raise `kind` from `level` to `level + 1`. Level 0 means building it. */
float levelCost(Kind kind, int level);

/** Taking one down. Flat, and the same for every kind: it is a decision, not a sale. */
inline constexpr float kDismantleCost = 50.0f;

/**
 * Moving one of the movable ones.
 *
 * Dear enough that it is a campaign decision rather than a repositioning every
 * turn, and it scales with level because a bigger one is more to move.
 */
float moveCost(Kind kind, int level);

// ── Effects ─────────────────────────────────────────────────────────────────

/**
 * What one monument gives, before stacking.
 *
 * `popShare` is the province's population as a fraction of its country's, and
 * it walks the effect from effectMin to effectMax. It saturates at a THIRD:
 * one province holding a third of a country is already that country's centre,
 * and past that the curve would only reward a map where one province holds
 * everything. Level adds a further 25% of the base per level above the first.
 */
float scaledEffect(Kind kind, float popShare, int level);

/**
 * The same, with the harmonic decay a country's n-th monument of a kind takes.
 *
 * `rank` counts from 1 for the largest contributor. The second is worth a
 * half, the third a third. So five universities are worth about 2.28 of one
 * rather than five, and the answer to "build more" stops being "always".
 */
float stackedEffect(Kind kind, float popShare, int level, int rank);

/**
 * How far this one reaches, in province steps.
 *
 * The silo is the exception the caller has to know about: its range is not in
 * steps at all but in kilometres over the sphere (see siloRangeKm), because
 * "intercontinental" is not a number of borders.
 */
int radius(Kind kind, int level);

/**
 * The silo's reach, in kilometres, at a level.
 *
 * Level 1 is theatre range, level 2 crosses an ocean, level 3 reaches anywhere
 * on the planet. The last is deliberately larger than half the Earth's
 * circumference (about 20,000 km), so at level 3 the curve stops mattering and
 * the answer is always yes -- which is what the level is FOR, and is cheaper to
 * explain than a range that reaches most of the world.
 */
float siloRangeKm(int level);

// ── What one LOOKS like ─────────────────────────────────────────────────────
//
// ONE SILHOUETTE PER KIND, AND TWO RENDERINGS OF IT. A university and a missile
// silo have to be told apart on the map, and the first version drew every kind
// as the same stepped block -- which is a placeholder wearing eleven names.
//
// The shape is DATA rather than eleven drawing functions, because it is drawn
// twice: flat, as an icon standing in the province, and on the globe, as a
// solid extruded out of the ground with a lit face and a shaded one. Two
// hand-written drawing routines per kind would be twenty-two chances for the
// icon and the model to stop being the same building.
//
// Coordinates are FRACTIONS of the figure's own box: x and w across it with
// 0.5 the centre line, base and h up it with 0 the ground. So the same
// description works at 12 pixels on a world map and at 60 on a close globe.

enum class PartShape : uint8_t {
    Box = 0,   ///< a wall, a hall, a tower
    Pediment,  ///< a triangle sitting on its slot: a classical roof, a gable
    Dome,      ///< the TOP HALF of an ellipse, sitting on its base line
    Needle,    ///< a spike: a missile, a spire, an antenna
    Dish,      ///< a trapezoid opening upward: a radar face on its mount
    Jib,       ///< a horizontal arm off a mast: a crane
    Bevel,     ///< a trapezoid narrowing upward: a bunker, a revetment
    Panel,     ///< a parallelogram leaning right: a radar face, a solar array
    Slot,      ///< a DARK recess cut into the mass: an embrasure, a doorway
};

/**
 * HOW A FIGURE IS SHADED, which is a property of the FIGURE and not of a part.
 *
 * The first version shaded the right-hand fifth of every box, including
 * columns and chimneys, so a university came out as eight separate blocks with
 * eight separate light sources -- "broken parts" is exactly what that looks
 * like. One light, one silhouette: the renderer outlines the whole figure,
 * fills it, and darkens the right edge of its WIDEST ground-standing part
 * only. Everything else is flat, which is what makes the mass read as one
 * building.
 */

struct Part {
    PartShape shape = PartShape::Box;
    float x = 0.0f;     ///< left edge, 0..1 across the figure
    float w = 1.0f;     ///< width, as a fraction of the figure
    float base = 0.0f;  ///< bottom, 0..1 up the figure; 0 is the ground
    float h = 1.0f;     ///< height, as a fraction of the figure
};

/**
 * The parts of one kind, in DRAWING ORDER -- back to front, bottom to top.
 *
 * Every kind has at least one part standing on the ground (base == 0), or the
 * figure floats; tests/monuments_test.cpp asserts that, that every part is
 * inside the box, and -- the point of the whole thing -- that no two kinds
 * share a silhouette.
 */
std::vector<Part> silhouette(Kind k);

/**
 * Everything the caller must be able to ask without knowing the catalogue.
 *
 * A monument the player has: where it is, what it is, how big, and whether it
 * is currently costing a slot.
 */
struct Holding {
    int  provinceId = 0;
    Kind kind = Kind::University;
    int  level = 1;
    bool active = true;
};

/**
 * The holdings of one country, ordered as the slots are charged.
 *
 * ACTIVE ONES FIRST, then by what they give, then by province id. The order
 * decides which monument is the cheap slot and which is the dear one, and it
 * has to be the same on every machine and in every replay -- so it is total,
 * and the last key is an id rather than anything that can tie.
 */
std::vector<Holding> chargeOrder(const std::vector<Holding>& all);

/** How many of `all` are active, which is how many slots are being paid for. */
int activeCount(const std::vector<Holding>& all);

}  // namespace odmon
