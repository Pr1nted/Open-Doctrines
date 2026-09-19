#pragma once

// What a building costs and how long it takes -- once, for everybody.
//
// WHY THIS FILE EXISTS
//
// These four tables lived in Game_Render.cpp, and a second, identical copy
// lived in AISystem.cpp under AI_ names. The note left on the originals says
// exactly what goes wrong with that -- "which is fine while it was the only
// caller and is exactly how a second caller ends up with its own slightly
// different copy" -- and then it happened anyway, one file over.
//
// The two copies never diverged in VALUE. What diverged was what was done with
// them: the panel multiplied every price by a research cost modifier and the AI
// did not, so with the industry tree finished a player built at half price and
// the AI paid full, forever, while its economy module learned from the result.
// A shared table does not prevent that on its own, which is why buildCostMod()
// and conscriptionCostMod() are here too: the modifier is part of the price, so
// it lives beside the price.
//
// THE RULE
//
// A cost is (table entry) x (modifier). Anything that charges for a build calls
// both, or it is charging a different game than the one next to it.

#include <algorithm>
#include <cmath>
#include <string>

/**
 * WHAT A PROVINCE CAN PHYSICALLY SUPPORT, and why the cap is not a constant.
 *
 * THE PROBLEM THIS ANSWERS. Industry was capped at IND_MAX_LEVEL everywhere on
 * the map, and a level earned `level * 2` wherever it stood. So an uninhabited
 * rock earned exactly what the Ruhr earned. A player put it plainly: an island
 * "with a population of 0, 84 square kilometres of space, and basically no
 * resources should not be able to have level 10 industry". They are right, and
 * the tutorial already agreed with them in writing -- tut_economy tells the
 * player "a factory in an empty province is an expensive shed, the population
 * is the input, not the building", which was simply not true of the game.
 *
 * THE FOUR TERMS, and why each is there rather than just population:
 *
 *   population  the input. Logarithmic, because provinces span 0 to 75 million
 *               and a linear term would make the top ten provinces the only
 *               ones that existed.
 *   density     the reason a SMALL province can carry heavy industry. Without
 *               it the formula is "big population wins" and a sprawling rural
 *               province ranks level with an industrial city. This term is what
 *               reconciles the rule with the shipped maps: Belgium and the
 *               French north are dense, small, resource-poor and heavily
 *               industrialised by the map author, and they are exactly the
 *               provinces a density-blind formula gets wrong.
 *   area        room to build. Capped LOW, and deliberately lower than the
 *               density term -- land helps, but Siberia is not the Ruhr.
 *   resources   what makes an empty but oil-rich province still worth a
 *               refinery, and keeps prospecting worth doing.
 *
 * WHY AREA IS CAPPED BELOW DENSITY, stated because getting this wrong is silent.
 * Area and density pull in opposite directions at fixed population: spreading
 * the same people over more ground raises the area term and lowers the density
 * one. The first fit of these constants gave both a ceiling of 1.5, and the two
 * cancelled almost exactly -- Belgium and a province with the same population
 * over sixty times the land came out at the SAME capacity, so the density term
 * was contributing nothing net while appearing to work. It still scored well,
 * because it happened to agree with the maps for unrelated reasons. Density
 * must out-weigh area or this whole paragraph is a description of code that is
 * not running; tests/industry_capacity_test.cpp asserts the comparison directly
 * so it cannot quietly stop being true again.
 *
 * AREA IS TRUE AREA, NOT PIXELS. The maps are equirectangular, so a raster
 * pixel at 70N covers about a third of the ground a pixel at the equator does.
 * Counting pixels would hand every arctic province a bonus it has not got. The
 * caller weights each row by cos(latitude); see Game::buildPopulationLookups.
 *
 * CALIBRATION. Fitted against the four shipped maps (map, 1914, 1918, 1939) by
 * one test: how often does this rule disagree with what a human map author
 * actually built? Out of 1247-1642 provinces per map, it disagrees on 2 to 4 --
 * eleven provinces across all four maps together -- and they are the same
 * historically-industrial French and Belgian ones every time, which is a
 * category the rule is not expected to capture and grandfathering covers.
 * Median capacity is 4-5, level 10 is reachable in 6 to 10 provinces a map, and
 * roughly 200 empty provinces per map cap at 1 -- the class the player was
 * complaining about.
 *
 * GRANDFATHERED PROVINCES ARE LEGAL. A province built above its own capacity is
 * a state the rules cannot produce but saves can contain, because the cap binds
 * new builds only and nobody's cities shrink because they updated. Every caller
 * must therefore tolerate `level > capacity`: refuse the NEXT level, never
 * clamp the one that is there.
 */
inline constexpr float INDCAP_POP_FLOOR_LOG  = 3.0f;  ///< under 1k people, no contribution
inline constexpr float INDCAP_POP_SCALE      = 1.35f;
inline constexpr float INDCAP_DENS_FLOOR_LOG = 2.0f;  ///< people per unit area
inline constexpr float INDCAP_DENS_SCALE     = 1.1f;
inline constexpr float INDCAP_DENS_MAX       = 1.8f;
inline constexpr float INDCAP_AREA_FLOOR_LOG = 2.5f;
inline constexpr float INDCAP_AREA_SCALE     = 0.4f;
inline constexpr float INDCAP_AREA_MAX       = 0.6f;
inline constexpr float INDCAP_RES_DIV        = 65.0f; ///< sum of the five, each 0-100
inline constexpr float INDCAP_RES_MAX        = 2.5f;

/**
 * The capacity itself, as a pure function of the three things a province is.
 *
 * Here rather than in Game so that there is exactly one of it. The note at the
 * top of this file exists because the build-cost tables were duplicated into
 * the AI and silently diverged in what they were multiplied by; a capacity rule
 * that the panel and the AI each computed for themselves would go the same way,
 * and the failure would be invisible -- the player and the AI would simply be
 * playing two different games about where a factory may stand.
 *
 * `area` is cos(latitude)-weighted raster area; `resourceSum` is the five
 * deposit amounts added, each on the map's own 0-100 scale.
 */
inline int industryCapacity(long long population, float area, float resourceSum) {
    const double pop = (double)std::max(0LL, population);
    const double a   = std::max(0.0, (double)area);
    double t = std::max(0.0, (std::log10(pop + 1.0) - INDCAP_POP_FLOOR_LOG) * INDCAP_POP_SCALE);
    // Density over TRUE area, floored at 1 so a province rounded to no area at
    // all cannot divide by zero and arrive at an infinite capacity.
    const double dens = pop / std::max(1.0, a);
    t += std::min((double)INDCAP_DENS_MAX,
                  std::max(0.0, (std::log10(dens + 1.0) - INDCAP_DENS_FLOOR_LOG) * INDCAP_DENS_SCALE));
    t += std::min((double)INDCAP_AREA_MAX,
                  std::max(0.0, (std::log10(a + 1.0) - INDCAP_AREA_FLOOR_LOG) * INDCAP_AREA_SCALE));
    t += std::min((double)INDCAP_RES_MAX,
                  (double)std::max(0.0f, resourceSum) / (double)INDCAP_RES_DIV);
    // Floored at 1, never 0: nowhere on the map is permanently barren, and a
    // capacity of zero would make a province's first factory unbuildable
    // forever, which is a wall rather than a slope.
    const int lvl = (int)t;
    return lvl < 1 ? 1 : (lvl > 10 ? 10 : lvl);
}

/**
 * Industry price by the level being built, and turns to build it.
 *
 * Indexed by the TARGET level, so IND_COST[3] is what it costs to reach level
 * 3, and index 0 is unused padding to keep that reading true.
 */
// LEVEL 1 WAS 1 GOLD, and yielded 2 a turn -- it repaid itself in HALF A TURN,
// at any scale, which is what made buying the first level in every province the
// dominant opening. 8 still makes it the best step in the table (four turns to
// repay, against five for the second level and a hundred and fifty for the
// tenth); it is simply no longer free money.
inline constexpr int IND_COST[]   = {0, 8, 10, 15, 25, 50, 75, 100, 150, 200, 300};
inline constexpr int IND_TURNS[]  = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

/** Fortification, same indexing. Forts are quick and get expensive fast. */
inline constexpr int FORT_COST[]  = {0, 20, 30, 50, 100, 200};
inline constexpr int FORT_TURNS[] = {0, 1, 1, 1, 1, 1};

/**
 * What a country's industry costs to RUN, per turn, as a fraction of what it
 * earns -- and the reason the fraction depends on how much of it there is.
 *
 * THE PROBLEM THIS ANSWERS. Industry had no upkeep at all: once built it was
 * pure profit for ever, and since a province's price never depended on what
 * the country already owned, the cheapest strategy in the game was to buy the
 * first level everywhere. A player reported it as "the industrialization
 * snowball is particularly effective in low-industrial environs", which is
 * exactly right -- the more level-0 provinces you hold, the better it gets.
 *
 * WHY UPKEEP AND NOT A HIGHER PRICE. A price is paid once, out of a treasury.
 * AI countries run at or near zero treasury, so a lump-sum gate on an action
 * is not a tax on it, it is a PROHIBITION -- raise the price of industry far
 * enough and the AI simply stops industrialising, which makes the opponent
 * worse in a game whose players already say the AI is too easy. Upkeep comes
 * out of income instead, so the AI keeps building and keeps paying, and the
 * brake applies to whoever is winning hardest.
 *
 * WHY IT NEVER TAKES MORE THAN IT GIVES. The fraction is capped, so net
 * industry income still RISES with every level built -- just by less each
 * time. A curve that eventually made new industry a loss would be a wall the
 * player hits and stops playing at; this one is a slope they keep climbing.
 *
 *   T = total industry levels held    eaten    net of 2/level
 *      20  (a small country)            3%          39
 *     120  (a solid power)             20%         192
 *     250  (a large empire)            42%         292
 *     500+                             45% cap     550
 */
inline constexpr float IND_UPKEEP_SCALE = 600.0f;  ///< levels at which the cap is met
inline constexpr float IND_UPKEEP_MAX   = 0.45f;   ///< never eats more than this

/**
 * `reductionPct` is getTotalEffect("industryUpkeepPct") -- the Industrial
 * Efficiency branch. A REDUCTION is registered positive, like industryCostPct,
 * so this subtracts. Floored at zero rather than allowed to pay a country for
 * owning factories, and applied to the RATE so it compounds correctly with the
 * cap instead of fighting it.
 */
inline float industryUpkeep(int totalLevels, float grossIndustryIncome,
                            float reductionPct = 0.0f) {
    if (totalLevels <= 0 || grossIndustryIncome <= 0.0f) return 0.0f;
    float f = std::min(IND_UPKEEP_MAX, (float)totalLevels / IND_UPKEEP_SCALE);
    f *= std::max(0.0f, 1.0f - reductionPct / 100.0f);
    return grossIndustryIncome * f;
}

inline constexpr int IND_MAX_LEVEL  = (int)(sizeof(IND_COST) / sizeof(IND_COST[0])) - 1;
// industryCapacity() is defined above IND_COST -- it is a rule about provinces,
// not a price -- so it clamps to a literal 10. This is the wire between the
// two: extend IND_COST and the ceiling moves, and the compiler says so here
// rather than the capacity rule silently keeping the old roof.
static_assert(IND_MAX_LEVEL == 10,
              "industryCapacity() clamps to a literal 10; update it with IND_COST");
inline constexpr int FORT_MAX_LEVEL = (int)(sizeof(FORT_COST) / sizeof(FORT_COST[0])) - 1;
inline constexpr int PORT_MAX_LEVEL = 3;

/** A new port, or the next level of one, and how long it takes. */
/**
 * WHAT A HULL COSTS TO KEEP AT SEA, PER TURN.
 *
 * These were 25 and 10, written inline in the income loop, and they made the
 * navy unaffordable in a way nothing in the game said out loud. Measured on
 * the 1914 map at turn 60, across 42 countries:
 *
 *     navy    9.0% of gross income on average, 47.7% for the worst country
 *     army    0.43% on average
 *
 * One carrier at 25 a turn cost the upkeep of TWENTY-FIVE MILLION SOLDIERS,
 * because army upkeep is 0.01 per ten thousand men. That is not a fleet being
 * expensive, it is a unit of account that never got compared with the other
 * one.
 *
 * The consequence was measured from the other end by the AI session: no
 * trained model buys a ship or a port on any seat -- 0 of 6,020 ship offers
 * across a full game -- and a reflex that forces the purchases costs 40.6
 * points of the map at 400 turns, because the drain compounds every turn. Both
 * halves agree, which is what makes this a price and not a policy defect: the
 * models are playing correctly and the navy is priced above what it returns.
 *
 * At roughly a sixth of the old figures the mean falls to about 1.4% of gross,
 * a little above the army -- a fleet should cost more than infantry -- and a
 * naval power's fleet stays the largest line in its budget without being half
 * of it. The carrier:destroyer ratio is held at about 2.7:1, so the CHOICE
 * between hulls is unchanged and only the scale moves.
 */
inline constexpr float SHIP_UPKEEP_CARRIER      = 4.0f;
inline constexpr float SHIP_UPKEEP_DESTROYER    = 1.5f;
inline constexpr float SHIP_UPKEEP_PER_10K_CREW = 0.2f;   ///< unchanged

/**
 * What one hull costs its owner per turn. THE ONLY PLACE THIS IS WORKED OUT.
 *
 * It exists because it did not, and the cost of that was found the hard way:
 * the AI's two scrap-a-warship sites each carried their own `carrier ? 25 :
 * destroyer ? 10 : 0`, re-derived rather than read. Repricing the constants
 * above left both copies behind, and they were WRONG rather than merely stale
 * -- the AI's austerity loop stops cutting once `treasury + net + scrapSaving
 * >= 0`, so it believed scrapping a carrier closed a 25-point hole when it
 * closed a 4-point one, and would have stopped cutting while still insolvent.
 *
 * A re-derived rule is a silent second copy, and it stays silent until
 * somebody changes the original. Call this; do not read the constants.
 */
inline float shipUpkeep(const std::string& type, int crew) {
    float n = 0.0f;
    if (type == "carrier")        n = SHIP_UPKEEP_CARRIER;
    else if (type == "destroyer") n = SHIP_UPKEEP_DESTROYER;
    // Transports are free to keep and are the reason this takes crew at all:
    // the hull costs nothing and the men aboard are the whole of the bill.
    return n + (crew / 10000.0f) * SHIP_UPKEEP_PER_10K_CREW;
}

inline constexpr float PORT_COST_PER_LEVEL = 60.0f;
inline constexpr int   PORT_TURNS = 3;

/** Warships: price and the port level needed to lay one down. */
inline constexpr float DESTROYER_COST = 15.0f;
inline constexpr float CARRIER_COST   = 40.0f;
inline constexpr int   DESTROYER_PORT = 2;
inline constexpr int   CARRIER_PORT   = 3;
inline constexpr int   SHIP_TURNS     = 3;

/** Specialising a province costs this much of its CURRENT industry level. */
inline constexpr float SPECIALIZE_COST_MULT = 1.5f;
inline constexpr int   SPECIALIZE_TURNS = 3;

// ═══ WHAT AN ARMY IS MADE OF ═══════════════════════════════════════════════
//
// An army was one integer. `struct ArmyUnit { int countryId; int count; }` was
// the whole type, which is most of why the game reads as Victoria rather than
// Hearts of Iron: there was nothing to decide about an army except how big it
// was, and nothing for a war to be about except who brought more.
//
// THIS TABLE COPIES ARTY_COSTS DELIBERATELY. Ammunition is already typed,
// already researched, already priced per type in three currencies, and already
// read by the panel, the AI, the resolver and the refund path from ONE table --
// see the note below it, which records that the price used to live in six
// places and that adding currencies to six copies "is not a risk of divergence,
// it is a plan for it". Troops are the same kind of thing and get the same
// treatment rather than a second invention.
//
// WHAT DISTINGUISHES A TYPE, and the choice matters more than the numbers: each
// field MODULATES A TERM Game::weighAssault ALREADY COMPUTES. No new combat
// formula, nothing to keep in step, and every one of these already prints a
// counter in the eval:
//
//   frontage  x combatWidth   -- how much of the line one man occupies
//   atk / def x armyAtkPct / armyDefPct, which are already per-side
//   fuel      x ARMY_FUEL_PER_10K and supplyFactor
//
// The frontage column is the one that changes the game. Measured on 2026-09-05,
// numbers win TWICE: depthFactor rewards a deeper stack in the comparison, and
// the attrition race rewards surplus, so an attacker fighting at 0.73 power with
// a 0.74 exchange still wins by having brought more. Forts and supply only delay
// that arithmetic. A type that uses the frontage efficiently CHANGES it -- and
// with manpower priced per type, "bring more men" finally has to compete with
// "bring better men" for the same pool.
//
// LINE INFANTRY IS EXACTLY NEUTRAL, 1.0 in every column, and that is load
// bearing. Every soldier in every existing save and every world today is line
// infantry, so wiring these multipliers in changes nothing at all until another
// type exists -- which is what makes the first stage of this provable rather
// than hopeful: a byte-identical eval.
enum TroopType : uint8_t {
    TROOP_LINE = 0,     ///< the default, and what every old save is made of
    TROOP_MILITIA,      ///< cheap, defensive, poor at using a frontage
    TROOP_ASSAULT,      ///< expensive in men, takes ground
    TROOP_MECHANISED,   ///< expensive in everything, thirsty, best on a frontage
    TROOP_TYPE_COUNT
};

struct TroopCost {
    const char* id;
    const char* name;
    float money;      ///< multiplier on the recruit price
    float munitions;  ///< multiplier on RECRUIT_MUNITIONS_PER_10K
    float manpower;   ///< pool drawn PER MAN -- the scarcity that makes this a choice
    float frontage;   ///< share of combat width one man occupies; lower is better
    float atk;        ///< multiplier on attack power
    float def;        ///< multiplier on defence power
    float fuel;       ///< multiplier on ARMY_FUEL_PER_10K
};

/**
 * Every troop type, once.
 *
 * Four, and the number is a UI constraint as much as a design one: each has a
 * clearly different answer in every column, and four is the most that stays
 * readable on a province marker. The table makes a fifth cheap if the game wants
 * one.
 *
 * The numbers here are a FIRST DRAFT and are meant to be measured, not trusted.
 *
 * MILITIA HAS ALREADY BEEN CORRECTED ONCE, and the error is worth keeping
 * because it is easy to repeat. It was drafted at frontage 1.20 on the idea
 * that an untrained rabble "uses a frontage badly" -- which sounds right and is
 * arithmetically fatal. Above the frontage every count cancels and power
 * reduces to `width x stat/frontage`, so the only numbers that matter in a
 * width-bound fight are ATTACK PER METRE and DEFENCE PER METRE:
 *
 *              atk/m   def/m
 *   line       1.000   1.000
 *   militia    0.583   0.917   <- worse than line at BOTH. Dominated.
 *   assault    1.688   1.187
 *   mech       2.083   1.917
 *
 * Militia was strictly worse than line infantry at the one job it exists for,
 * in exactly the fights that decide provinces -- a quarter of all assaults are
 * width-bound. Measured by the AI session: a rule that bought the best defence
 * per unit of money picked militia and cost 34 and 28 rating points on two
 * models, with the floor falling 110 to 26.
 *
 * The fix is that FRONTAGE IS FOOTPRINT, NOT SKILL. A militiaman occupies a
 * man's width like anybody else; what he lacks is training, which is the attack
 * column. So militia is 1.00 frontage with 1.15 defence: better than line at
 * holding, plainly worst at taking ground, at half the money and 0.6 of the
 * people. Mechanised keeps 0.60 because concentrated firepower genuinely does
 * deliver more per metre of line, which is a different claim.
 */
inline constexpr TroopCost TROOP_TYPES[] = {
    // id            name           money  muni   manp  front   atk    def   fuel
    {"line",     "Line Infantry",   1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},
    {"militia",  "Militia",         0.50f, 0.50f, 0.60f, 1.00f, 0.70f, 1.15f, 0.80f},
    {"assault",  "Assault Infantry",2.00f, 2.00f, 2.50f, 0.80f, 1.35f, 0.95f, 1.20f},
    {"mech",     "Mechanised",      3.50f, 3.00f, 4.00f, 0.60f, 1.25f, 1.15f, 3.00f},
};
static_assert(sizeof(TROOP_TYPES) / sizeof(TROOP_TYPES[0]) == (size_t)TROOP_TYPE_COUNT,
              "TROOP_TYPES and TroopType have drifted apart");

/** The entry for a type. Never a silent zero: an unknown type is line infantry. */
inline const TroopCost& troopCost(TroopType t) {
    const int i = (int)t;
    return TROOP_TYPES[(i >= 0 && i < (int)TROOP_TYPE_COUNT) ? i : 0];
}
/** The type an id names, or TROOP_LINE. Ids are stable and never translated. */
inline TroopType troopTypeFromId(const char* id) {
    if (!id) return TROOP_LINE;
    for (int i = 0; i < (int)TROOP_TYPE_COUNT; ++i) {
        const char* a = TROOP_TYPES[i].id;
        const char* b = id;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == 0 && *b == 0) return (TroopType)i;
    }
    return TROOP_LINE;
}

// ═══ WHAT A WAR COSTS IN THINGS RATHER THAN MONEY ═══════════════════════════
//
// Phase 4 of the production economy: an army is raised with munitions, kept
// with fuel, and its artillery fires both. Every one of these is ZERO unless
// the goods economy is switched on for the world -- see Game::m_goodsEconomy --
// so a money-economy game is priced exactly as it always was.
//
// WHY THE ARTILLERY TABLE MOVED HERE. Its money cost lived in SIX places: an
// ALL_ARTY table in the province panel, an ARTY_COST beside the firing code, a
// second ARTY_COST beside the naval bombardment code, a CANCEL_ARTY_COST for
// refunds, and two Ammo tables in the AI. They all agreed -- today. Adding two
// more currencies to six copies is not a risk of divergence, it is a plan for
// it, and this file exists because exactly that happened to the build-cost
// tables once already. One table, six callers.
//
// A REFUND MUST READ THE SAME TABLE AS THE CHARGE. The cancel path had its own
// copy, which is the single most dangerous duplicate in the set: if the two
// ever disagreed, cancelling an order would pay back more than it cost and the
// treasury would be a money printer.
struct ArtyCost {
    const char* id;
    float money;
    float fuel;       ///< aircraft burn it; guns barely do
    float munitions;  ///< the shell itself
};

/**
 * Every ammunition type, once.
 *
 * Fuel is weighted toward the types delivered by aircraft (napalm, carpet) and
 * munitions toward the types that are mostly warhead (chemical, nuclear,
 * biological). A mortar is cheap in everything, which is what keeps it the
 * thing a poor country can still fire.
 */
inline constexpr ArtyCost ARTY_COSTS[] = {
    // id             money  fuel  munitions
    {"mortar",          5.0f, 0.2f,  1.0f},
    {"light",          10.0f, 0.4f,  2.0f},
    {"heavy",          20.0f, 0.8f,  4.0f},
    {"napalm",         30.0f, 3.0f,  3.0f},   // aircraft
    {"carpet",         25.0f, 3.5f,  2.5f},   // aircraft
    {"chemical",       40.0f, 1.0f,  8.0f},
    {"nuclear",        80.0f, 2.0f, 16.0f},
    {"biological",     60.0f, 1.0f, 12.0f},
};
inline constexpr int ARTY_COST_COUNT = (int)(sizeof(ARTY_COSTS) / sizeof(ARTY_COSTS[0]));

/** The entry for an ammunition id, or nullptr. Never a silent zero-cost hit. */
inline const ArtyCost* artyCost(const char* id) {
    if (!id) return nullptr;
    for (int i = 0; i < ARTY_COST_COUNT; ++i) {
        const char* a = ARTY_COSTS[i].id;
        const char* b = id;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == 0 && *b == 0) return &ARTY_COSTS[i];
    }
    return nullptr;
}

/** Money only, which is what every existing caller asked for. 0 if unknown. */
inline float artyMoneyCost(const char* id) {
    const ArtyCost* c = artyCost(id);
    return c ? c->money : 0.0f;
}

/**
 * Munitions to raise ten thousand men, and fuel to keep them for a turn.
 *
 * RAISING AND KEEPING ARE PRICED IN DIFFERENT THINGS ON PURPOSE, and it is the
 * same distinction the mobilisation doctrines already draw in money: a levy is
 * cheap to raise and expensive to keep. Now a country can be rich and unable to
 * arm anybody, or well armed and unable to move.
 *
 * Sized against what a country can actually produce. A country's total goods
 * output at full industrialisation is about 1.6x its own population's consumer
 * demand (see GOOD_OUTPUT_SCALE), so a mid-sized power making munitions with a
 * third of its factories has a few units a turn: recruiting a hundred thousand
 * men at 1.5 munitions is a real decision, and a standing army burning fuel
 * every turn is a bill that competes with its own artillery.
 *
 * UNSWEPT, AND CHOSEN ON DESIGN GROUNDS RATHER THAN MEASUREMENT. Three values
 * were tried on one map and one seed -- 0.004, 0.01, 0.02 -- and the results
 * were NOT MONOTONIC (living standards 0.76 / 0.56 / 0.61, survival 54.7% /
 * 45.3% / 52.8%). A single run cannot separate them, and reading that ordering
 * as signal would have been fitting noise.
 *
 * So the value is picked on the one thing a single run CAN show: whether the
 * mechanic exists at all. At 0.004, world fuel demand was 7 against a stock of
 * 677 -- provably inert, an army that never noticed fuel. At 0.02, production
 * tracks demand turn by turn (29.8 against 28.8) -- provably live. 0.02 is
 * therefore the smallest value tested at which the phase does what it is for.
 * It wants a proper multi-seed sweep before anybody treats it as balanced.
 */
inline constexpr float RECRUIT_MUNITIONS_PER_10K = 0.15f;
inline constexpr float ARMY_FUEL_PER_10K         = 0.02f;

/**
 * What a country pays, per unit, for fuel it needed and did not have.
 *
 * AN ARMY WITHOUT FUEL DOES NOT STOP FIGHTING IN THIS PHASE -- it is bought in
 * at a premium instead, and the premium is the punishment. That is deliberately
 * a money problem rather than a combat one: making a shortage degrade combat
 * power would move resolveAssault, which is the function the amphibious work is
 * being measured on, and one variable at a time is worth more than one clever
 * change. Degrading combat is the obvious next step and it is out of scope here.
 *
 * Well above RAW_FLOOR_PRICE, because buying under duress is not the same
 * transaction as selling a surplus.
 */
inline constexpr float FUEL_SHORTFALL_PRICE = 3.0f;

/**
 * Standing reserves a country wants on hand, which is what tells the allocator
 * to make these at all.
 *
 * Without a reserve the demand for munitions and machinery would be whatever is
 * queued this turn, and a country with an empty stockpile could never queue
 * anything -- it would need munitions to recruit, and nothing would make
 * munitions because nothing was recruiting. A standing want breaks that circle
 * the way a real quartermaster does: you hold stock because you expect to need
 * it, not because an order has arrived.
 */
inline constexpr float MUNITIONS_RESERVE_PER_10K_ARMY = 0.02f;
inline constexpr float MACHINERY_RESERVE_PER_LEVEL    = 0.05f;

/**
 * Research discount on anything built with industry, as a multiplier.
 *
 * `effectPct` is what getTotalEffect("industryCostPct") returns. A DISCOUNT is
 * registered as a POSITIVE number -- "industry cost -50%" is stored as 50 --
 * so this subtracts, and the floor at zero stops a future stack of effects
 * going past free into negative prices.
 */
inline float buildCostMod(float effectPct) {
    return std::max(0.0f, 1.0f - effectPct / 100.0f);
}

/**
 * The same, for recruitment, from getTotalEffect("conscriptionCostPct").
 *
 * A separate function only so the call site names which tree it is reading.
 * The effect name matters more than it looks: "armyCostPct" is not a real one,
 * and code that asked for it got a silent zero and therefore no discount at
 * all -- which has now been written twice, in two different files.
 */
inline float conscriptionCostMod(float effectPct) { return buildCostMod(effectPct); }

/**
 * What a navy costs, from getTotalEffect("navyCostPct").
 *
 * THIS WAS SUMMED AND NEVER SPENT, exactly as maintenanceCostPct was below.
 * Four research nodes grant navyCostPct and five doctrines advertise it, and
 * nothing read the total -- so a player who researched "Advanced Shipbuilding"
 * for its stated -10% ship cost got nothing, and reported it:
 *
 *   "I researched Advanced Shipbuilding, which says it reduces ship cost by
 *    10%. Yet when I completed it, the cost to build ships like destroyers and
 *    carriers was not reduced, nor was the Navy Cost listed in the Local
 *    Economy part of the Economy section reduced."
 *
 * They were right on both counts, and the second is the one that could be
 * fixed: DESTROYER_COST and CARRIER_COST are only ever read to value a fleet
 * for national accounting -- a ship is never CHARGED up front. Its whole price
 * is the berth while it is building and the upkeep once it floats, which is
 * the "Navy Cost" line they checked. So that is what the discount applies to,
 * and the advertised number becomes true against the figure on screen.
 *
 * Same sign convention as every other modifier here: a REDUCTION is positive.
 */
inline float navyCostMod(float effectPct) { return buildCostMod(effectPct); }

/**
 * What it costs to KEEP an army, as against raising one.
 *
 * `effectPct` is getTotalEffect("maintenanceCostPct"). Same sign convention as
 * every other modifier here: a REDUCTION is stored positive.
 *
 * THIS WAS SUMMED AND NEVER SPENT. maintenanceCostPct was added up correctly by
 * getTotalEffect -- four research nodes grant it and four doctrines advertise it
 * (Demobilisation says army maintenance -25%, Austerity -20%, Privatisation and
 * Autarky -10%) -- and then NOTHING read the total. Army upkeep was a flat
 * (men / 10000) * 0.01 with no modifier anywhere in it, so every one of those
 * eight promises was decoration from the day it was written.
 *
 * That is the same failure the doctrine levers had, one step further along: the
 * levers were parsed into nothing and summed nowhere, and the note on
 * Policy::levers records fixing that. These were summed correctly and applied
 * nowhere, which is harder to notice because the number looks right everywhere
 * you can see it.
 *
 * A separate function from conscriptionCostMod for the same reason that one is
 * separate from buildCostMod: the call site should name which lever it is
 * reading, because "armyCostPct" is not a real effect name and code that asked
 * for it got a silent zero -- which this file already records happening twice.
 *
 * RAISING AND KEEPING ARE DIFFERENT DECISIONS, and that is why there are two of
 * these. A mass levy is cheap to raise and expensive to keep; a professional
 * army is the other way round. A doctrine that moved both together would just
 * be a discount.
 */
inline float maintenanceCostMod(float effectPct) { return buildCostMod(effectPct); }
