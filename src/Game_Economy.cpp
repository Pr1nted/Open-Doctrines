#include "Game.h"
#include "Palette.h"
#include "GameInternals.h"
#include "Keybinds.h"
#include "raymath.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdio>
#include "BuildCosts.h"

// === specializationBoostPct / provinceResourceIncome ===
//
// See the note on the declarations: the boost is applied here, on the way out,
// rather than written into resourceIncome -- which stays the province's
// unspecialised base so that re-specialising cannot compound it and so that
// every save written before specialisation paid anything still reads right.
const char* const Game::SPEC_RESOURCES[5] = {"Oil", "Gold", "Metal", "Rubber", "Gemstones"};

float Game::specializationBoostPct(int pid) const {
    auto indIt = m_provinceIndustry.find(pid);
    if (indIt == m_provinceIndustry.end() || indIt->second.specialization.empty()) return 0.0f;
    auto resIt = m_provinceResources.find(pid);
    if (resIt == m_provinceResources.end()) return 0.0f;
    const std::string& s = indIt->second.specialization;
    const ProvinceResources& r = resIt->second;
    if (s == "Oil")       return r.oil.boost;
    if (s == "Gold")      return r.gold.boost;
    if (s == "Metal")     return r.metal.boost;
    if (s == "Rubber")    return r.rubber.boost;
    if (s == "Gemstones") return r.gemstones.boost;
    return 0.0f;
}

float Game::provinceResourceIncome(int pid) const {
    auto indIt = m_provinceIndustry.find(pid);
    if (indIt == m_provinceIndustry.end()) return 0.0f;
    return indIt->second.resourceIncome * (1.0f + specializationBoostPct(pid) / 100.0f);
}

const char* Game::bestSpecializationFor(int pid) const {
    auto resIt = m_provinceResources.find(pid);
    if (resIt == m_provinceResources.end()) return nullptr;
    const ProvinceResources& r = resIt->second;
    const float boosts[5] = {r.oil.boost, r.gold.boost, r.metal.boost,
                             r.rubber.boost, r.gemstones.boost};
    int best = -1;
    for (int i = 0; i < 5; ++i)
        if (boosts[i] > 0.0f && (best < 0 || boosts[i] > boosts[best])) best = i;
    return best < 0 ? nullptr : SPEC_RESOURCES[best];
}

// === provinceIndustryCapacity / provinceIndustryIncome ===
//
// See industryCapacity() in BuildCosts.h for the rule, the four terms and the
// calibration against the shipped maps. These are the wrappers that feed it the
// province's own numbers, and they are the ONLY way the rest of the game asks.
//
// Why a wrapper at all rather than callers reading the three inputs themselves:
// because that is precisely how the build-cost tables came to have two copies
// that agreed on the values and disagreed on what was done with them, one file
// over, for a whole release. A capacity every caller computed for itself would
// fail the same way and fail invisibly -- the panel and the AI would simply be
// playing two different games about where a factory may stand.
int Game::provinceIndustryCapacity(int pid) const {
    long long people = 0;
    if (pid > 0 && (size_t)pid < m_provincePopArray.size())
        people = m_provincePopArray[pid];
    else {
        auto it = m_provincePopulations.find(pid);
        if (it != m_provincePopulations.end()) people = it->second;
    }

    float resourceSum = 0.0f;
    auto res = m_provinceResources.find(pid);
    if (res != m_provinceResources.end()) {
        const ProvinceResources& r = res->second;
        resourceSum = r.oil.amount + r.gold.amount + r.metal.amount +
                      r.rubber.amount + r.gemstones.amount;
    }

    return industryCapacity(people, provinceArea(pid), resourceSum);
}

// WHAT A LEVEL EARNS, AND WHY IT IS NOT `level * 2` ANY MORE.
//
// The old line -- `ind.income = targetLevel * 2.0f; // Simplified income` --
// paid an empty rock exactly what it paid the Ruhr, which is the other half of
// the same complaint the capacity rule answers. Capping the LEVEL alone would
// have left every province that is under its cap earning a flat rate regardless
// of whether anybody lives there.
//
// MONOTONIC BY CONSTRUCTION. `level` only ever multiplies a positive rate, so
// the next level is always worth more than this one, everywhere. A curve that
// could make a build a loss would be a wall the player hits and stops playing
// at; this is a slope they keep climbing, just at different gradients in
// different places. That is the same reasoning industryUpkeep() is built on.
//
// THE RATE IS THE OLD RATE AT FULL CAPACITY. A province built to its own
// capacity earns 2 per level, exactly as before, so the well-sited provinces
// that carried an economy still carry it and this is not a stealth nerf to
// everybody. What changes is the badly-sited ones: a level-4 factory in a
// province that can support 8 earns half rate, because half the reason to build
// it is not there.
// ── ONE TABLE OF WEDGES ──
//
// Read by the economy screen's expense pie and by the country profile, which
// publishes the same breakdown to anyone who opens it. INDUSTRY UPKEEP IS IN
// HERE for a reason worth keeping: the denominator is `expenses`, which
// includes upkeep, so leaving it out did not merely omit a label -- it left the
// pie with a hole exactly the size of the upkeep, and the percentages did not
// sum to 100. Reported from a 67-level France whose four listed slices came to
// 79.2%, with the missing 20.8% sitting in the table beside the chart.
std::vector<Game::ExpenseSlice> Game::expenseSlices(const CountryIncomeSnapshot& s) {
    return {
        {s.industryUpkeep,   Color{230, 170, 110, 255}, T("Industry upkeep")},
        {s.armyExpenses,     Color{200,  80,  80, 255}, T("Army")},
        {s.navyExpenses,     Color{120,  80, 180, 255}, T("Navy")},
        {s.policyCosts,      Color{ 80, 120, 200, 255}, T("Doctrines")},
        {s.minorityCosts,    Color{200, 140,  80, 255}, T("Minority")},
        {s.researchCost,     Color{ 80, 200,  80, 255}, T("Research")},
        {s.pacificationCost, Color{ 80, 180, 220, 255}, T("Pacification")},
    };
}

float Game::provinceIndustryIncome(int pid, int level) const {
    if (level <= 0) return 0.0f;
    const int cap = provinceIndustryCapacity(pid);
    // Grandfathered provinces sit ABOVE their capacity and are legal; the ratio
    // is clamped at 1 so they earn the full rate rather than a bonus for being
    // overbuilt. See the grandfathering note in BuildCosts.h.
    const float fit = std::min(1.0f, (float)cap / (float)std::max(1, level));
    // Floored well above zero: a province that can support one level and has
    // been given six is badly run, not worthless, and an income that collapsed
    // to nothing would make conquering a small country a liability.
    const float rate = 2.0f * std::max(0.35f, fit);
    // ── AND THE REGIONAL LAW IN FORCE ON THIS GROUND ──
    //
    // Here rather than at the country total, because that is what these are:
    // a tax holiday in one district and company towns in another are two
    // different rates inside one country on the same turn. Zero for an
    // undivided country, so this line changes nothing until a law is passed.
    const Province* p = m_provinces.getProvinceById(pid);
    if (p && p->countryId > 0) {
        const float mod = 1.0f + districtLawsAt(p->countryId, pid).incomePct / 100.0f;
        return (float)level * rate * std::max(0.0f, mod);
    }
    return (float)level * rate;
}

void Game::countryIndustryCapacity(int countryId, int& used, int& total,
                                   int& overcap) const {
    used = total = overcap = 0;
    for (int pid : provincesOf(countryId)) {
        const int cap = provinceIndustryCapacity(pid);
        total += cap;
        auto ind = m_provinceIndustry.find(pid);
        const int lvl = (ind != m_provinceIndustry.end()) ? ind->second.level : 0;
        used += lvl;
        if (lvl > cap) ++overcap;
    }
}

// ═══ THE PRODUCTION ECONOMY ═════════════════════════════════════════════════
//
// See the declarations in Game.h and the note on GoodId in GameStructs.h.
//
// DETERMINISM IS A REQUIREMENT HERE, NOT AN ASPIRATION. This pass runs over
// every province of every country every turn, and players rely on a seed
// playing the same game twice -- one of them reported the same flag being
// generated for the same AI country in two separate worlds and read it,
// correctly, as the game being deterministic. So: iteration is over
// provincesOf(), which is an ordered index, never over an unordered_map, and
// nothing here reads wall-clock time or a shared RNG.
// tests/determinism_check.sh is what proves it.

/**
 * A province's factories at full supply: level x rate, scaled by how well the
 * province actually supports that level.
 *
 * Reuses the capacity fit rather than inventing a second one, so a factory
 * built beyond what its province can carry produces less for the same reason it
 * earns less. One rule about badly-sited industry, applied twice.
 */
float Game::provinceGoodOutput(int pid) const {
    auto ind = m_provinceIndustry.find(pid);
    if (ind == m_provinceIndustry.end()) return 0.0f;
    const int level = ind->second.level;
    if (level <= 0 || ind->second.output < 0 || ind->second.output >= GOOD_COUNT)
        return 0.0f;

    // A FACTORY'S OUTPUT IS ITS WORKERS, NOT ITS BUILDINGS.
    //
    // This was `level * GOOD_PER_LEVEL`, a flat rate per level, and it was
    // wrong in a way that only a long game showed. Demand is linear in
    // population; that supply was linear in industry LEVELS, and levels are
    // capped by provinceIndustryCapacity, which is LOGARITHMIC in population.
    // So a country whose population multiplied could not multiply its
    // production to match: ten times the people bought about one and a third
    // extra levels of capacity, and nothing else moved.
    //
    // Over a 120-turn run that gap is not a margin, it is the whole result.
    // Measured on one map: consumer demand outran production 1.8:1 at turn 40,
    // 20:1 at turn 80 and 33.7:1 at turn 120, while production itself barely
    // moved. Nothing a player did could close it, because the ceiling was not
    // theirs to raise.
    //
    // POPULATION IS BOTH SIDES NOW, so the ratio is scale-invariant: fifty-two
    // times the people is fifty-two times the demand AND fifty-two times the
    // potential output, and what decides whether anybody is fed is the fraction
    // of the country that is industrialised. That is a question the player
    // answers with decisions instead of one the arithmetic answers for them.
    //
    // It is also the model the game already claims: the tutorial says "the
    // population is the input, not the building", and now that is true of what
    // a factory MAKES as well as of where one may stand.
    long long people = 0;
    if (pid > 0 && (size_t)pid < m_provincePopArray.size())
        people = m_provincePopArray[pid];
    if (people <= 0) return 0.0f;

    // How far this province is industrialised, 0..1. Clamped at 1 so a
    // grandfathered province -- built above its own capacity before the cap
    // existed -- produces at full rate rather than earning a bonus for it.
    const int cap = provinceIndustryCapacity(pid);
    const float built = std::min(1.0f, (float)level / (float)std::max(1, cap));

    // Expressed in the SAME denominator as countryGoodsDemand, which is what
    // makes the two comparable at all. A province industrialised to its own
    // capacity produces GOOD_OUTPUT_SCALE times its own population's consumer
    // demand -- so a country feeds itself somewhat before it is fully built,
    // and the surplus capacity is what pays for machinery, fuel and munitions.
    return (float)((double)people / CONSUMER_PER_CAPITA) * built * GOOD_OUTPUT_SCALE;
}

// WHAT THE POPULATION WANTS.
//
// Only the consumer good is demanded by people; the other three are demanded by
// things the player builds, and those are charged where they are built rather
// than forecast here.
//
// THE COMPASS MOVES IT, which is the cheapest way to make ideology cost
// something concrete. A left or libertarian population expects more of its
// government than an authoritarian right one does, so the same production feeds
// one and not the other -- and a player who moves the compass finds out that
// promises have a bill attached.
float Game::countryGoodsDemand(int countryId, int good) const {
    if (good != GOOD_CONSUMER) return 0.0f;
    long long people = 0;
    for (int pid : provincesOf(countryId)) {
        if (pid > 0 && (size_t)pid < m_provincePopArray.size())
            people += m_provincePopArray[pid];
    }
    if (people <= 0) return 0.0f;
    float expectation = 1.0f;
    auto gov = m_countryCompass.find(countryId);
    if (gov != m_countryCompass.end()) {
        // Left of centre and libertarian both raise it, by at most a quarter
        // each. Bounded on purpose: an unbounded expectation would make the
        // compass a suicide button rather than a trade-off.
        expectation += std::max(0.0f, -gov->second.economic / 100.0f) * 0.25f;
        expectation += std::max(0.0f,  gov->second.social   / 100.0f) * 0.25f;
    }
    return (float)((double)people / CONSUMER_PER_CAPITA) * expectation;
}

float Game::livingStandards(int countryId) const {
    auto it = m_countryProduction.find(countryId);
    if (it == m_countryProduction.end()) return 1.0f;
    return it->second.livingStandards;
}

// === artilleryPrice / recruitPrice / payWarMaterials / refundWarMaterials ===
//
// See the declarations, and ARTY_COSTS in BuildCosts.h for the table.

Game::WarPrice Game::artilleryPrice(const std::string& ammoType, int countryId) const {
    const int cid = (countryId < 0) ? m_playerCountryId : countryId;
    WarPrice p;
    const ArtyCost* c = artyCost(ammoType.c_str());
    if (!c) return p;                       // unknown ammunition costs nothing
    p.money = c->money;
    if (!m_goodsEconomy) return p;          // money economy: priced as before
    // Arming is conscriptionCostPct; running is maintenanceCostPct. A shell is
    // both -- it is manufactured and it is moved -- so it reads one lever for
    // each currency rather than one lever for the pair.
    p.munitions = c->munitions * conscriptionCostMod(getTotalEffect("conscriptionCostPct", cid));
    p.fuel      = c->fuel      * maintenanceCostMod(getTotalEffect("maintenanceCostPct", cid));
    return p;
}

Game::WarPrice Game::recruitPrice(int count, int countryId, TroopType type) const {
    const int cid = (countryId < 0) ? m_playerCountryId : countryId;
    WarPrice p;
    if (count <= 0) return p;
    const float mod = conscriptionCostMod(getTotalEffect("conscriptionCostPct", cid));
    // What KIND of soldier, on top of the country's own cost research. Line
    // infantry is 1.0 in both columns, so a world with only line infantry is
    // priced exactly as it always was. See TROOP_TYPES.
    const TroopCost& tc = troopCost(type);
    // The money half is the rule the recruitment button has always used: one
    // per ten thousand men, floored at one so a token levy is not free.
    p.money = (count / 10000.0f) * mod * tc.money;
    if (p.money < 1.0f) p.money = 1.0f;
    if (!m_goodsEconomy) return p;
    p.munitions = (count / 10000.0f) * RECRUIT_MUNITIONS_PER_10K * mod * tc.munitions;
    return p;
}

bool Game::payWarMaterials(int countryId, const WarPrice& p) {
    if (!m_goodsEconomy) return true;
    if (p.fuel <= 0.0f && p.munitions <= 0.0f) return true;
    auto it = m_countryStockpiles.find(countryId);
    if (it == m_countryStockpiles.end()) return false;
    CountryStockpile& sp = it->second;
    if (sp.goods[GOOD_FUEL] < p.fuel || sp.goods[GOOD_MUNITIONS] < p.munitions)
        return false;
    sp.goods[GOOD_FUEL]      -= p.fuel;
    sp.goods[GOOD_MUNITIONS] -= p.munitions;
    return true;
}

bool Game::canAffordWarMaterials(int countryId, const WarPrice& p) const {
    if (!m_goodsEconomy) return true;
    if (p.fuel <= 0.0f && p.munitions <= 0.0f) return true;
    auto it = m_countryStockpiles.find(countryId);
    if (it == m_countryStockpiles.end()) return false;
    return it->second.goods[GOOD_FUEL] >= p.fuel &&
           it->second.goods[GOOD_MUNITIONS] >= p.munitions;
}

void Game::refundWarMaterials(int countryId, const WarPrice& p) {
    if (!m_goodsEconomy) return;
    CountryStockpile& sp = m_countryStockpiles[countryId];
    sp.goods[GOOD_FUEL]      = std::min(STOCKPILE_CAP, sp.goods[GOOD_FUEL] + p.fuel);
    sp.goods[GOOD_MUNITIONS] = std::min(STOCKPILE_CAP, sp.goods[GOOD_MUNITIONS] + p.munitions);
}

void Game::refundArtilleryOrder(int countryId, const std::string& ammoType,
                                double& treasury) {
    const WarPrice p = artilleryPrice(ammoType, countryId);
    treasury += p.money;
    refundWarMaterials(countryId, p);
}

std::string Game::artilleryPriceLabel(const char* ammoType, int troopKillPct) const {
    const WarPrice p = artilleryPrice(ammoType ? ammoType : "", m_playerCountryId);
    std::string out = TextFormat("$%.0f", p.money);
    // Only what is actually charged. In a money economy this reads exactly as
    // it always did, which is the point of showing it this way rather than
    // printing two zeroes.
    if (p.munitions > 0.005f) out += TextFormat(" %.1fmun", p.munitions);
    if (p.fuel > 0.005f)      out += TextFormat(" %.1ffuel", p.fuel);
    if (troopKillPct >= 0)    out += TextFormat(T(" %d%% troops"), troopKillPct);
    return out;
}

// === plannedShare / directableFactories / directedFactories / autoSellPctFor ===
//
// See the declarations. The economic compass is the dial; these read it.
float Game::plannedShare(int countryId) const {
    auto it = m_countryCompass.find(countryId);
    if (it == m_countryCompass.end()) return 0.5f;   // no compass: the middle
    // economic runs -100 (left) to +100 (right). Clamped through the same
    // bounded accessor everything else uses, because an out-of-range compass
    // has reached this game before and a share above 1 would let a country
    // direct more factories than it owns.
    const float econ = clampCompassAxis(it->second.economic);
    return std::clamp((100.0f - econ) / 200.0f, 0.0f, 1.0f);
}

int Game::directableFactories(int countryId) const {
    int factories = 0;
    for (int pid : provincesOf(countryId)) {
        auto ind = m_provinceIndustry.find(pid);
        if (ind != m_provinceIndustry.end() && ind->second.level > 0) ++factories;
    }
    // Rounded rather than truncated, so a small country in a mostly-planned
    // economy can still direct its one factory instead of being told its
    // economics are theoretical.
    return (int)std::lround((double)factories * (double)plannedShare(countryId));
}

int Game::directedFactories(int countryId) const {
    int n = 0;
    for (int pid : provincesOf(countryId)) {
        auto ind = m_provinceIndustry.find(pid);
        if (ind != m_provinceIndustry.end() && ind->second.level > 0 &&
            ind->second.directed) ++n;
    }
    return n;
}

int Game::autoSellPctFor(int countryId) const {
    // The world sets the ceiling; the government's economic system scales it.
    // A planned economy (share 1) sells a fifth of what a pure market does --
    // not nothing, because a state that could never dispose of a surplus would
    // simply be broken rather than ideological, and because trade is the
    // intended answer rather than the only one.
    const float planned = plannedShare(countryId);
    const float scale = 1.0f - 0.8f * planned;
    return std::clamp((int)std::lround((double)m_autoSellPct * (double)scale), 0, 100);
}

bool Game::setProvinceOutput(int pid, int good, int countryId) {
    const int cid = (countryId < 0) ? m_playerCountryId : countryId;
    const Province* p = m_provinces.getProvinceById(pid);
    if (!p || p->countryId != cid) return false;
    if (good < -1 || good >= GOOD_COUNT) return false;
    auto ind = m_provinceIndustry.find(pid);
    // A province with no factories has nothing to assign. Checked rather than
    // silently accepted, because the multiplayer host runs this against orders
    // from a client that may say anything.
    if (ind == m_provinceIndustry.end() || ind->second.level <= 0) return false;

    if (good < 0) {
        // Handing a factory back to the economy always costs nothing and is
        // always allowed -- a government may stop directing whenever it likes.
        ind->second.output = -1;
        ind->second.directed = false;
        return true;
    }

    // HOW MANY A GOVERNMENT MAY DIRECT IS ITS ECONOMIC SYSTEM.
    //
    // Checked here, in the rule, and not in the panel that draws the button:
    // this is the only path a player, the AI and the multiplayer host all go
    // through, and a limit written in the renderer would bind the local player
    // alone. Re-directing a factory the country ALREADY directs is free --
    // otherwise a country at its limit could never change its mind, which is a
    // trap rather than a constraint.
    if (!ind->second.directed &&
        directedFactories(cid) >= directableFactories(cid))
        return false;

    ind->second.output = good;
    ind->second.directed = true;
    return true;
}

// === recipeFeasible / consumeRecipe ===
//
// How many units of a good the pool could make, and taking the materials for
// them. Two functions rather than one so that the allocator can ASK without
// spending -- it has to compare four goods before choosing one.
//
// SUBSTITUTABLE INPUTS ARE DRAWN MOST-PLENTIFUL-FIRST, which is both the
// sensible reading (light industry uses what is to hand) and the one that
// keeps a scarce material for the goods that genuinely need it: spending the
// last of the metal on consumer goods when machinery has no substitute would
// be the wrong call, and greedy-on-the-largest-pile avoids it without needing
// a planner.
//
// Deterministic: the order is by amount with the raw id breaking ties, never
// by container order. See the determinism note above.
static void anyOrder(const CountryStockpile& pool, int out[RAW_COUNT]) {
    for (int i = 0; i < RAW_COUNT; ++i) out[i] = i;
    // Four elements: an insertion sort is the clearest correct thing here.
    for (int i = 1; i < RAW_COUNT; ++i) {
        for (int j = i; j > 0; --j) {
            const float a = pool.raw[out[j]], b = pool.raw[out[j - 1]];
            // Strictly greater, so equal piles keep ascending id order.
            if (a > b) std::swap(out[j], out[j - 1]); else break;
        }
    }
}

float Game::recipeFeasible(const CountryStockpile& pool, int good) const {
    const GoodRecipe r = goodInputs(good);
    float feasible = 1e9f;
    bool bounded = false;
    for (int i = 0; i < RAW_COUNT; ++i) {
        if (r.fixed[i] <= 0.0f) continue;
        feasible = std::min(feasible, pool.raw[i] / r.fixed[i]);
        bounded = true;
    }
    if (r.any > 0.0f) {
        float total = 0.0f;
        for (int i = 0; i < RAW_COUNT; ++i) total += pool.raw[i];
        feasible = std::min(feasible, total / r.any);
        bounded = true;
    }
    // A recipe with no inputs at all would otherwise report a billion. Nothing
    // ships like that today, but a mod or a later good might.
    return bounded ? std::max(0.0f, feasible) : 0.0f;
}

void Game::consumeRecipe(CountryStockpile& pool, int good, float units) const {
    if (units <= 0.0f) return;
    const GoodRecipe r = goodInputs(good);
    for (int i = 0; i < RAW_COUNT; ++i) {
        if (r.fixed[i] <= 0.0f) continue;
        pool.raw[i] = std::max(0.0f, pool.raw[i] - units * r.fixed[i]);
    }
    if (r.any <= 0.0f) return;
    float owed = units * r.any;
    int order[RAW_COUNT];
    anyOrder(pool, order);
    for (int k = 0; k < RAW_COUNT && owed > 0.0f; ++k) {
        const int i = order[k];
        const float take = std::min(pool.raw[i], owed);
        pool.raw[i] -= take;
        owed -= take;
    }
}

// === autoAssignOutputs ===
//
// WHO DECIDES WHAT A FACTORY MAKES, when nobody has said.
//
// A RESOLVER HEURISTIC AND NOT A NEURAL DECISION, deliberately. This runs for
// every undirected province of every country every turn; it has a clear
// objective (make what the country is shortest of) and it must be
// deterministic, which is three good reasons not to hand it to a policy net.
// The network's business is whether to DIRECT an economy at all and at what
// cost -- which is the planned-versus-market choice, and belongs with the
// compass -- not which of four goods a particular shed should make.
//
// It is also what stops the goods economy from being unplayable on arrival.
// Without it, switching the flag on gives every country a set of idle factories
// and a starving population on turn one, which would measure as a catastrophe
// and tell nobody anything.
//
// THE RULE: feed people first, then fill the emptiest shelf. Consumer goods are
// the only good with a population behind them, so an unfed country puts
// everything into food and clothing; a fed one spreads across the rest by
// what it holds least of. Ties break by good id, so the same world allocates
// the same way twice -- see the determinism note above.
void Game::autoAssignOutputs(int countryId, const CountryStockpile& pool) {
    const float consumerDemand = countryGoodsDemand(countryId, GOOD_CONSUMER);
    // LAST turn's demand, because this turn's is still being accumulated when
    // the allocator runs -- the army's fuel draw and the standing reserves are
    // recorded in steps 3b and 3c, after production. Using the previous turn is
    // a one-turn lag on a quantity that moves slowly, and it is honest; reading
    // a half-filled record would be neither.
    auto prevIt = m_countryProduction.find(countryId);
    const CountryProduction* lastDemand =
        (prevIt != m_countryProduction.end()) ? &prevIt->second : nullptr;
    for (int pid : provincesOf(countryId)) {
        auto ind = m_provinceIndustry.find(pid);
        if (ind == m_provinceIndustry.end()) continue;
        // A factory its government has taken charge of is not the economy's to
        // reallocate. Undirected ones are, every turn -- that is what makes a
        // market a market and a plan a plan.
        if (ind->second.level <= 0 || ind->second.directed) continue;

        // WHAT IT CAN MAKE, not just what is wanted. Assigning a factory to a
        // good whose materials the country does not hold is how 93% of the
        // world's factories ended up idle on the first run of this: the
        // allocator chose by need alone, every country chose consumer goods,
        // and the ones with no suitable deposits starved with full ore piles.
        // ── THE WORST-SUPPLIED SHELF, MEASURED IN PROPORTION ──
        //
        // "Feed people first, then fill the emptiest shelf" was an ALL-OR-
        // NOTHING rule, and it starved the war economy completely. Consumer
        // demand is chronically a little short in almost every country, so the
        // first branch always won: measured at turn 40, consumer goods were the
        // ONLY thing produced anywhere in the world -- machinery, fuel and
        // munitions all at 0.0 against real demand -- so armies bought every
        // drop of fuel at the penalty price and could not be reinforced at all.
        //
        // A country does not stop making shells because bread is 10% short. So
        // the four goods are ranked together by the FRACTION of their need that
        // is unmet, not the absolute gap: a small need that is entirely unmet
        // outranks a large one that is nearly covered, which fills the cheap
        // shortages quickly and then puts everything back into food.
        //
        // Proportional rather than absolute is what makes goods with very
        // different scales comparable at all -- consumer demand is hundreds and
        // fuel is single digits, so an absolute gap would rank consumer first
        // for ever and reproduce the bug in a subtler form.
        int want = -1;
        float worstFrac = -1.0f;
        for (int g = 0; g < GOOD_COUNT; ++g) {
            if (recipeFeasible(pool, g) <= 0.0f) continue;
            const float need = (g == GOOD_CONSUMER)
                                   ? consumerDemand
                                   : (lastDemand ? lastDemand->demand[g] : 0.0f);
            // NOT CLAMPED AT ZERO, and that is what stops a satisfied country
            // dumping every factory into one good for ever. Left clamped, every
            // met need scored 0, the tie fell to the lowest id, and consumer
            // goods piled up to seven times what anyone wanted (stock 8,121
            // against demand 1,147 at turn 120) while machinery sat at twice
            // its reserve. Letting the fraction go NEGATIVE ranks "three times
            // over-supplied" below "just barely covered", so surplus capacity
            // spreads across the shelves instead of drowning one.
            //
            // A good nothing wants at all ranks last outright rather than at
            // zero -- otherwise it would outrank every over-supplied good and
            // a country with no army would make munitions in preference to
            // anything it actually uses.
            float frac = -1e9f;
            if (need > 0.0001f)
                frac = std::min(1.0f, (need - pool.goods[g]) / need);
            // Strictly greater, so ties fall to the lower good id and the same
            // world allocates the same way twice. See the determinism note.
            if (frac > worstFrac) { worstFrac = frac; want = g; }
        }
        // Nothing is makeable from what this country holds. Left undirected on
        // purpose rather than parked on a good: the moment a trade or a
        // conquest brings materials in, it is allocated on the next turn
        // instead of sitting on a choice made when the cupboard was bare.
        if (want < 0) continue;
        ind->second.output = want;
    }
}

void Game::processProduction(int countryId) {
    if (!m_goodsEconomy) return;

    CountryStockpile& pool = m_countryStockpiles[countryId];
    CountryProduction prod{};   // a rate, rebuilt from nothing every turn

    // ── 1. EXTRACT ──
    // Deposits become materials. The specialisation bonus applies here for the
    // same reason it applies to resource income: it is what specialising buys,
    // and provinceResourceIncome is where that rule already lives.
    //
    // A DEPOSIT IS A RICHNESS, NOT A QUANTITY, AND PEOPLE ARE WHAT GET IT OUT.
    //
    // This used to be `amount * rate` -- a fixed tonnage per turn set entirely
    // by the map. That is the same failure provinceGoodOutput had, one layer
    // down: consumer demand grows with population, and a world whose ore output
    // is fixed by the map can never keep up with it however it plays. Fixing
    // output alone moved the shortfall from 33.7:1 to 24.2:1 and no further,
    // because the binding constraint had simply moved from factories to ore.
    //
    // So the deposit sets the RATE (amount/100, how rich the ground is) and the
    // province's people set the SCALE, in the same denominator as demand and
    // production. Everything in the chain now moves together, and what decides
    // whether a country is fed is how rich its ground is and how much of its
    // capacity it has built -- both decisions -- rather than how many people
    // happen to have been born.
    for (int pid : provincesOf(countryId)) {
        auto res = m_provinceResources.find(pid);
        if (res == m_provinceResources.end()) continue;
        long long people = 0;
        if (pid > 0 && (size_t)pid < m_provincePopArray.size())
            people = m_provincePopArray[pid];
        if (people <= 0) continue;   // nobody to work the seam
        const ProvinceResources& r = res->second;
        const float spec = 1.0f + specializationBoostPct(pid) / 100.0f;
        const float amounts[RAW_COUNT] = {
            r.oil.amount, r.metal.amount, r.rubber.amount, r.gemstones.amount
        };
        const float workers = (float)((double)people / CONSUMER_PER_CAPITA);
        for (int i = 0; i < RAW_COUNT; ++i)
            prod.extracted[i] += (amounts[i] / 100.0f) * workers * EXTRACT_SCALE * spec;
    }
    for (int i = 0; i < RAW_COUNT; ++i) pool.raw[i] += prod.extracted[i];

    // ── 1b. DIRECT WHATEVER NOBODY HAS DIRECTED ──
    // After extraction, so the allocator sees this turn's materials, and before
    // production, so a factory directed this turn works this turn.
    autoAssignOutputs(countryId, pool);

    // ── 2. PRODUCE ──
    // Each province converts pool materials into its assigned good, limited by
    // what the pool actually holds. Provinces are visited in the ordered index,
    // so which factory gets the last of a scarce material is decided the same
    // way every run -- see the determinism note above.
    for (int pid : provincesOf(countryId)) {
        auto ind = m_provinceIndustry.find(pid);
        if (ind == m_provinceIndustry.end() || ind->second.level <= 0) continue;
        ++prod.factoriesTotal;
        const int good = ind->second.output;
        if (good < 0 || good >= GOOD_COUNT) { ++prod.factoriesIdle; continue; }

        float want = provinceGoodOutput(pid);
        if (want <= 0.0f) { ++prod.factoriesIdle; continue; }

        const float feasible = std::min(want, recipeFeasible(pool, good));
        if (feasible <= 0.0001f) { ++prod.factoriesIdle; continue; }
        consumeRecipe(pool, good, feasible);
        pool.goods[good] += feasible;
        prod.produced[good] += feasible;
    }

    // ── 3. FEED ──
    // The population eats before anything is sold. See the ordering note on
    // processProduction: a country must not sell the bread it needed.
    prod.demand[GOOD_CONSUMER] = countryGoodsDemand(countryId, GOOD_CONSUMER);
    const float wantC = prod.demand[GOOD_CONSUMER];
    if (wantC > 0.0f) {
        const float ate = std::min(pool.goods[GOOD_CONSUMER], wantC);
        pool.goods[GOOD_CONSUMER] -= ate;
        prod.consumed[GOOD_CONSUMER] = ate;
        prod.livingStandards = ate / wantC;
    } else {
        // Nobody to feed is not a famine. A country with no population left is
        // in trouble for reasons this number should not also be reporting.
        prod.livingStandards = 1.0f;
    }

    // ── 3b. THE ARMY DRINKS ──
    //
    // A standing army burns fuel whether or not it is fighting. Charged here
    // rather than in the money accounts because it is a materials cost, and
    // taken AFTER the population has eaten: a country short of everything feeds
    // its people before it fuels its tanks, which is both the humane reading
    // and the one that keeps living standards meaning what it says.
    //
    // A SHORTFALL IS BOUGHT IN, AT A PREMIUM, RATHER THAN GROUNDING THE ARMY.
    // Making a fuel shortage degrade combat power is the obvious next step and
    // it is deliberately not taken here: it would move resolveAssault, which is
    // the function the amphibious work is being measured on. One variable at a
    // time. See FUEL_SHORTFALL_PRICE.
    {
        // Garrisons AND men in battles: see Game::countryTroops. A war does not
        // stop needing fuel because the men are in the middle of a fight.
        const long long troops = countryTroops(countryId);
        if (troops > 0) {
            const float want = (float)((double)troops / 10000.0) * ARMY_FUEL_PER_10K *
                               maintenanceCostMod(getTotalEffect("maintenanceCostPct", countryId));
            prod.demand[GOOD_FUEL] += want;
            const float have = std::min(pool.goods[GOOD_FUEL], want);
            pool.goods[GOOD_FUEL] -= have;
            prod.consumed[GOOD_FUEL] += have;
            const float shortfall = want - have;
            if (shortfall > 0.0f) {
                prod.fuelBought = shortfall;
                auto c = m_countries.getAll().find(countryId);
                if (c != m_countries.getAll().end())
                    c->second.treasury -= (double)shortfall * FUEL_SHORTFALL_PRICE;
            }
        }
    }

    // ── 3c. WHAT ELSE THE COUNTRY WANTS ON HAND ──
    //
    // Demand is what tells autoAssignOutputs to make a thing at all, so without
    // these three lines munitions and machinery would be produced only while an
    // order was already queued -- and a country with an empty stockpile could
    // never queue one, because recruiting needs munitions. A standing reserve
    // breaks that circle the way a quartermaster does. See
    // MUNITIONS_RESERVE_PER_10K_ARMY.
    {
        const long long troops = countryTroops(countryId);   // battles included
        prod.demand[GOOD_MUNITIONS] +=
            (float)((double)troops / 10000.0) * MUNITIONS_RESERVE_PER_10K_ARMY;
        int levels = 0;
        for (int p2 : provincesOf(countryId)) {
            auto ind2 = m_provinceIndustry.find(p2);
            if (ind2 != m_provinceIndustry.end()) levels += ind2->second.level;
        }
        prod.demand[GOOD_MACHINERY] += (float)levels * MACHINERY_RESERVE_PER_LEVEL;
    }

    // ── 4. SELL THE SURPLUS ──
    // See m_autoSellPct: one dial from "everything sells" to "nothing sells
    // itself". Raw materials only -- goods are what the country made on
    // purpose, and auto-selling those would undo step 2 the turn after it ran.
    // The country's own rate, not the world's: see autoSellPctFor.
    const int pct = autoSellPctFor(countryId);
    if (pct > 0) {
        float earned = 0.0f;
        for (int i = 0; i < RAW_COUNT; ++i) {
            const float sell = pool.raw[i] * (float)pct / 100.0f;
            if (sell <= 0.0f) continue;
            pool.raw[i] -= sell;
            earned += sell * RAW_FLOOR_PRICE[i];
        }
        prod.rawSold = earned;
        auto c = m_countries.getAll().find(countryId);
        if (c != m_countries.getAll().end()) c->second.treasury += earned;
    }

    // Stockpiles are finite. Without a ceiling a country that produces more
    // than it can use accumulates for the whole game and the number stops
    // meaning anything on the panel -- and at 3,000 turns it stops fitting in
    // a float's exact range too.
    for (int i = 0; i < RAW_COUNT; ++i)  pool.raw[i]   = std::min(pool.raw[i],   STOCKPILE_CAP);
    for (int i = 0; i < GOOD_COUNT; ++i) pool.goods[i] = std::min(pool.goods[i], STOCKPILE_CAP);

    m_countryProduction[countryId] = prod;
}

// === projectIncome ===
//
// See the declaration. Today's snapshot, walked forward over the build queues.
//
// EVERYTHING ELSE IS HELD STILL ON PURPOSE. A projection that guessed at
// conquest, population growth or what the neighbours will do would be a
// forecast, and a wrong one; this answers the narrower question a player
// answers by glancing at their build queue -- "of what I have already paid
// for, what has arrived by then, and what is it costing me?" That is the only
// part of the future this game is arithmetic about, and it is the part every
// multi-turn purchase decision actually turns on.
// ── THE INCOME LEVERS, AND THE THREE THAT WERE NEVER SPENT ──
//
// tools/check_effect_fields.py found these summed by getTotalEffect and read
// by nothing: resourceModPct (2 research nodes, 14 doctrines), popModPct (7
// and 7) and passiveIncome (5 and 14). Every one of those printed a number in
// a tooltip and changed no figure in the game.
//
// What each MEANS is not a guess -- tools/gen_policies.py, which generates the
// advertised text from the levers, states it:
//
//     resourceModPct   "Resource income {sign}{v}%"
//     popModPct        "Population income {sign}{v}%"
//     passiveIncome    "Treasury {sign}{v}/turn"
//
// So they scale the two income components the snapshot already separates, and
// the third is flat. The research nodes agree: "Population Income Bonus I,
// +10% per level", "Passive Income I, +1 to economy per research level".
//
// SIGN: positive is MORE, unlike the cost levers next door where a reduction
// is stored positive. Doctrines carry both signs -- autarky is resource +15
// and treasury -4 -- so getting this backwards would not fail loudly, it would
// invert half the economy.
//
// The multiplier is floored at zero so a future stack of -60% levers cannot
// turn income negative; a NEGATIVE passiveIncome is allowed through, because
// "treasury -4/turn" is a cost a doctrine is entitled to impose.
void Game::applyIncomeLevers(CountryIncomeSnapshot& cs, int countryId) const {
    cs.resource *= std::max(0.0f, 1.0f + getTotalEffect("resourceModPct", countryId) / 100.0f);
    cs.pop      *= std::max(0.0f, 1.0f + getTotalEffect("popModPct", countryId) / 100.0f);
    cs.total = cs.gross + cs.resource + cs.pop + getTotalEffect("passiveIncome", countryId);
}

CountryIncomeSnapshot Game::projectIncome(int countryId, int turns) const {
    CountryIncomeSnapshot cs = computeCountryIncome(countryId);
    if (turns <= 0) return cs;
    // What today costs, kept so the deltas below are added once each rather
    // than re-derived from a second lookup of the same snapshot.
    const float navyNow = cs.navyExpenses;

    auto ownedByUs = [&](int pid) {
        const Province* p = m_provinces.getProvinceById(pid);
        return p && p->countryId == countryId;
    };

    for (const PendingUpgrade& pu : m_pendingUpgrades) {
        if (pu.turnsRemaining > turns || !ownedByUs(pu.provinceId)) continue;
        if (pu.type != "industry") continue;      // forts and ports earn nothing
        // The gain is the difference from whatever the province earns today,
        // and the arrival level is what processUpgrades will actually write --
        // capped by the province's capacity, and never below what is already
        // built. Computed through provinceIndustryIncome for the same reason
        // the note above gave for reading `now` from the province rather than
        // assuming it: this used to hardcode `targetLevel * 2` beside a
        // resolver that also hardcoded `level * 2`, and two copies of a formula
        // drift the first time one of them changes. That change is this one.
        auto it = m_provinceIndustry.find(pu.provinceId);
        const float now = (it != m_provinceIndustry.end()) ? it->second.income : 0.0f;
        const int   lvl = (it != m_provinceIndustry.end()) ? it->second.level : 0;
        const int   arrives = std::max(lvl, std::min(pu.targetLevel,
                                                     provinceIndustryCapacity(pu.provinceId)));
        cs.gross += std::max(0.0f, provinceIndustryIncome(pu.provinceId, arrives) - now);
        cs.industryLevels += std::max(0, arrives - lvl);
    }

    // A hull under construction is free until it floats and then costs its
    // berth every turn for ever. This is the half of a ship's price that
    // nothing used to look at -- see the note in execEconomy's ship case.
    for (const PendingShipBuild& sb : m_pendingShipBuilds) {
        if (sb.turnsRemaining > turns || !ownedByUs(sb.provinceId)) continue;
        // A hull under construction gets the same discount as one afloat:
        // "ship cost -10%" that applied only after launch would be true of a
        // number the player never sees separately.
        cs.navyExpenses += ((sb.type == "carrier") ? 25.0f : 10.0f) *
                           navyCostMod(getTotalEffect("navyCostPct", countryId));
    }

    // The factories' own running cost grows with how many levels are held, so
    // a projection that added the income and not the upkeep would make every
    // build look better than it is. Recomputed from the projected totals
    // through the same function the live snapshot uses.
    const float upkeepNow = cs.industryUpkeep;
    cs.industryUpkeep = industryUpkeep(cs.industryLevels, cs.gross,
                                       getTotalEffect("industryUpkeepPct", countryId));
    cs.expenses += (cs.industryUpkeep - upkeepNow);

    cs.expenses += (cs.navyExpenses - navyNow);
    applyIncomeLevers(cs, countryId);
    cs.net   = cs.total - cs.expenses;
    return cs;
}

CountryIncomeSnapshot Game::computeCountryIncome(int countryId) const {
    auto cacheIt = m_countryIncomeCache.find(countryId);
    if (cacheIt != m_countryIncomeCache.end()) return cacheIt->second;
    if (countryId == m_lastIncomeCountryId) return m_cachedIncome;
    CountryIncomeSnapshot cs;
    const auto& allProvs = m_provinces.getAllProvinces();
    for (int pid : provincesOf(countryId)) {
        auto pIt = allProvs.find(pid);
        if (pIt == allProvs.end() || pIt->second.countryId != countryId) continue;
        auto ind = m_provinceIndustry.find(pid);
        if (ind != m_provinceIndustry.end()) {
            cs.gross += ind->second.income;
            cs.industryLevels += ind->second.level;
            cs.resource += provinceResourceIncome(pid);
            cs.pop += ind->second.popIncome;
        }
    }
    // ── WHAT AN ARMY COSTS TO KEEP, AND WHO GETS TO CHANGE IT ──
    //
    // The modifier was missing entirely. This line was a flat rate per man with
    // nothing multiplying it, while getTotalEffect("maintenanceCostPct") added
    // up four research nodes and four doctrines that all advertise an army
    // maintenance discount -- Demobilisation says -25%, Austerity -20%. Every
    // one of those promises was decoration. See maintenanceCostMod().
    //
    // Read ONCE for the country rather than per stack: it is a national policy,
    // it cannot differ between two provinces, and this loop runs over every
    // army on the map for every country every turn.
    // ── WHAT AN ARMY COSTS TO KEEP, AND WHO GETS TO CHANGE IT ──
    //
    // The modifier was missing entirely. This line was a flat rate per man with
    // nothing multiplying it, while getTotalEffect("maintenanceCostPct") added
    // up four research nodes and four doctrines that all advertise an army
    // maintenance discount -- Demobilisation says -25%, Austerity -20%. Every
    // one of those eight promises was decoration from the day it was written.
    // See maintenanceCostMod().
    //
    // Read ONCE for the country rather than per stack: it is a national policy,
    // it cannot differ between two provinces, and this loop runs over every
    // army on the map for every country every turn.
    const float upkeepMod = maintenanceCostMod(getTotalEffect("maintenanceCostPct", countryId));
    for (auto& [pid, units] : m_provinceArmies) {
        for (auto& u : units) {
            if (u.countryId != countryId) continue;
            cs.armyExpenses += (u.count / 10000.0f) * 0.01f * upkeepMod;
        }
    }
    // And the men in standing battles, who are off the map but very much still
    // being paid. See Game::countryTroops.
    cs.armyExpenses += (battleTroops(countryId) / 10000.0f) * 0.01f * upkeepMod;
    // Through shipUpkeep() rather than a second copy of the three constants,
    // which is what these three lines were: the same prices already live in
    // BuildCosts.h and a fleet valued from one table and billed from another
    // is a plan for divergence.
    const float navyMod = navyCostMod(getTotalEffect("navyCostPct", countryId));
    for (auto& ship : m_ships) {
        if (ship.countryId != countryId) continue;
        cs.navyExpenses += shipUpkeep(ship.type, ship.crew) * navyMod;
    }
    auto it = m_countryActivePolicyIndices.find(countryId);
    if (it != m_countryActivePolicyIndices.end()) {
        for (int apIdx : it->second) {
            if (apIdx >= (int)m_activePolicies.size()) continue;
            auto& ap = m_activePolicies[apIdx];
            if (ap.countryId != countryId) continue;
            if (ap.turnsRemaining < 0) continue;
            const Policy* p = nullptr;
            for (const auto& policy : m_allPolicies) {
                if (policy.id == ap.policyId) { p = &policy; break; }
            }
            if (p) cs.policyCosts += policyUpkeep(ap, *p);
        }
    }
    // Doctrines a district runs on its own, priced by how much of the country
    // it covers. Zero for an undivided country. See Game::districtPolicyCost.
    cs.policyCosts += districtPolicyCost(countryId);
    std::unordered_set<std::string> processedMinorities;
    for (int pid : provincesOf(countryId)) {
        auto pIt = allProvs.find(pid);
        if (pIt == allProvs.end() || pIt->second.countryId != countryId) continue;
        auto mit = m_provinceMinorities.find(pid);
        if (mit == m_provinceMinorities.end()) continue;
        for (auto& mg : mit->second) {
            if (processedMinorities.count(mg.name)) continue;
            processedMinorities.insert(mg.name);
            for (size_t ci = 0; ci < m_ethnicPolicyCategories.size(); ci++) {
                const int oi = ethnicPolicyOption(countryId, mg.name, ci);
                if (oi >= 0 && oi < (int)m_ethnicPolicyCategories[ci].options.size())
                    cs.minorityCosts += m_ethnicPolicyCategories[ci].options[oi].costPerTurn;
            }
        }
    }
    applyIncomeLevers(cs, countryId);
    // Factories cost money to run, and more of them cost disproportionately
    // more. See industryUpkeep() for why this is a running cost rather than a
    // higher price.
    cs.industryUpkeep = industryUpkeep(cs.industryLevels, cs.gross,
                                       getTotalEffect("industryUpkeepPct", countryId));
    float baseExpenses = cs.armyExpenses + cs.navyExpenses + cs.policyCosts +
                         cs.minorityCosts + cs.industryUpkeep;
    float affordable = std::max(0.0f, cs.total - baseExpenses);
    // Allocations are per-country: only the player pays the research slider
    // Each country pays for ITS OWN allocations: the player's come from the
    // UI sliders, AI countries' from the per-country maps the AI sets — they
    // used to be billed the player's sliders as pure money sinks.
    float rAlloc = m_researchAllocation;
    float pAlloc = m_pacificationAllocation;
    if (countryId != m_playerCountryId) {
        auto raIt = m_countryResearchAllocation.find(countryId);
        rAlloc = (raIt != m_countryResearchAllocation.end()) ? raIt->second : 0.0f;
        auto pacIt = m_countryPacification.find(countryId);
        pAlloc = (pacIt != m_countryPacification.end()) ? pacIt->second : 0.0f;
    }
    cs.researchCost = cs.total * rAlloc;
    cs.pacificationCost = cs.total * pAlloc;
    float totalAlloc = cs.researchCost + cs.pacificationCost;
    if (totalAlloc > affordable && totalAlloc > 0) {
        float scale = affordable / totalAlloc;
        cs.researchCost *= scale;
        cs.pacificationCost *= scale;
    }
    cs.expenses = baseExpenses + cs.researchCost + cs.pacificationCost;
    cs.net = cs.total - cs.expenses;
    m_lastIncomeCountryId = countryId;
    m_cachedIncome = cs;
    return cs;
}

void Game::refreshIncomeCache() {
    m_countryIncomeCache.clear();
    struct IncomeAccum { float gross = 0, res = 0, pop = 0; int levels = 0; };
    std::unordered_map<int, IncomeAccum> incAcc;
    for (auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId <= 0) continue;
        auto ind = m_provinceIndustry.find(pid);
        if (ind == m_provinceIndustry.end()) continue;
        auto& a = incAcc[p.countryId];
        a.gross += ind->second.income;
        a.levels += ind->second.level;
        a.res += provinceResourceIncome(pid);
        a.pop += ind->second.popIncome;
    }
    // Upkeep in one pass each, not one pass per country. Both of these used to
    // sit inside the per-country loop below, so every army stack and every ship
    // on the map was visited once for every country in the game.
    std::unordered_map<int, float> armyUpkeep, navyUpkeep;
    for (auto& [pid, units] : m_provinceArmies)
        for (auto& u : units)
            if (u.countryId > 0) armyUpkeep[u.countryId] += (u.count / 10000.0f) * 0.01f;
    // Men in standing battles are off the map and still on the payroll.
    for (const auto& b : m_battles)
        if (b.attackerCid > 0) armyUpkeep[b.attackerCid] += (b.attackers() / 10000.0f) * 0.01f;
    for (auto& ship : m_ships) {
        if (ship.countryId <= 0) continue;
        // navyCostMod here too. This is the SECOND navy bill -- the first fix
        // reached computeCountryIncome and missed this one, which is the cache
        // every screen actually reads, so the discount would have applied to a
        // number the player never sees and not to the one they do.
        navyUpkeep[ship.countryId] += shipUpkeep(ship.type, ship.crew) *
                                      navyCostMod(getTotalEffect("navyCostPct", ship.countryId));
    }

    const auto& allProvs = m_provinces.getAllProvinces();
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID) continue;
        auto& a = incAcc[cid];
        CountryIncomeSnapshot cs;
        cs.gross = a.gross;
        cs.resource = a.res;
        cs.pop = a.pop;
        applyIncomeLevers(cs, cid);
        cs.industryLevels = a.levels;
        cs.industryUpkeep = industryUpkeep(a.levels, a.gross,
                                           getTotalEffect("industryUpkeepPct", cid));
        // THE MODIFIER BELONGS HERE TOO, AND THIS IS THE COPY THAT COUNTS.
        //
        // The per-man rate above is a SECOND copy of the one in
        // computeCountryIncome, and this is the one the game actually reads --
        // computeCountryIncome returns from m_countryIncomeCache for almost
        // every caller, so a fix applied only there is applied to nothing. That
        // was measurable: with maintenanceCostMod wired into the other copy, a
        // 30-turn eval produced byte-identical output and a probe found the
        // modifier reaching zero countries.
        //
        // Applied to the country TOTAL rather than inside the accumulation
        // loop, because the loop above is per-stack across the whole map and
        // the modifier is per-country: multiplying here is one lookup instead
        // of one per stack, and it cannot disagree with itself.
        //
        // See maintenanceCostMod(). This is the third time in this file that
        // one formula has had two homes; if a fourth appears, make them share a
        // function rather than a comment.
        {
            auto it = armyUpkeep.find(cid);
            if (it != armyUpkeep.end())
                cs.armyExpenses = it->second *
                    maintenanceCostMod(getTotalEffect("maintenanceCostPct", cid));
        }
        {
            // THE SAME MODIFIER THE ARMY GETS. It was applied to armyExpenses
            // and not here, so every maintenance-reducing doctrine and research
            // node in the game quietly did nothing for a fleet -- a player who
            // invested in maintenance saw their infantry get cheaper and their
            // navy not move at all.
            auto it = navyUpkeep.find(cid);
            if (it != navyUpkeep.end())
                cs.navyExpenses = it->second *
                    maintenanceCostMod(getTotalEffect("maintenanceCostPct", cid));
        }
        auto pIt = m_countryActivePolicyIndices.find(cid);
        if (pIt != m_countryActivePolicyIndices.end()) {
            for (int apIdx : pIt->second) {
                if (apIdx >= (int)m_activePolicies.size()) continue;
                auto& ap = m_activePolicies[apIdx];
                if (ap.countryId != cid || ap.turnsRemaining < 0) continue;
                for (const auto& policy : m_allPolicies)
                    if (policy.id == ap.policyId) { cs.policyCosts += policyUpkeep(ap, policy); break; }
            }
        }
        cs.policyCosts += districtPolicyCost(cid);
        std::unordered_set<std::string> pm;
        for (int pid : provincesOf(cid)) {
            auto pIt = allProvs.find(pid);
            if (pIt == allProvs.end() || pIt->second.countryId != cid) continue;
            auto mit = m_provinceMinorities.find(pid);
            if (mit == m_provinceMinorities.end()) continue;
            for (auto& mg : mit->second) {
                if (pm.count(mg.name)) continue;
                pm.insert(mg.name);
                for (size_t ci = 0; ci < m_ethnicPolicyCategories.size(); ci++) {
                    const int oi = ethnicPolicyOption(cid, mg.name, ci);
                    if (oi >= 0 && oi < (int)m_ethnicPolicyCategories[ci].options.size())
                        cs.minorityCosts += m_ethnicPolicyCategories[ci].options[oi].costPerTurn;
                }
            }
        }
        // industryUpkeep included HERE TOO. This is the bulk path every AI
        // country's income comes from and the one above is the player's; a cost
        // added to one and not the other is the two of them playing different
        // games, which is the mistake the header of BuildCosts.h exists to
        // record.
        float baseExpenses = cs.armyExpenses + cs.navyExpenses + cs.policyCosts +
                             cs.minorityCosts + cs.industryUpkeep;
        float affordable = std::max(0.0f, cs.total - baseExpenses);
        float rAlloc = m_researchAllocation;
        float pAlloc = m_pacificationAllocation;
        if (cid != m_playerCountryId) {
            auto raIt = m_countryResearchAllocation.find(cid);
            rAlloc = (raIt != m_countryResearchAllocation.end()) ? raIt->second : 0.0f;
            auto pacIt = m_countryPacification.find(cid);
            pAlloc = (pacIt != m_countryPacification.end()) ? pacIt->second : 0.0f;
        }
        cs.researchCost = cs.total * rAlloc;
        cs.pacificationCost = cs.total * pAlloc;
        float totalAlloc = cs.researchCost + cs.pacificationCost;
        if (totalAlloc > affordable && totalAlloc > 0) {
            float scale = affordable / totalAlloc;
            cs.researchCost *= scale;
            cs.pacificationCost *= scale;
        }
        cs.expenses = baseExpenses + cs.researchCost + cs.pacificationCost;
        cs.net = cs.total - cs.expenses;
        m_countryIncomeCache[cid] = cs;
    }
}

long long Game::countryPopulation(int countryId) const {
    long long pop = 0;
    for (const auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId != countryId) continue;
        auto it = m_provincePopulations.find(pid);
        if (it != m_provincePopulations.end()) pop += it->second;
    }
    return pop;
}

float Game::countryNationalValue(int countryId) const {
    if (countryId <= 0) return 0.0f;
    double v = 0.0;

    // ── WHAT IS STANDING ON THE GROUND ──
    // Cumulative, not the price of the current level: a level-5 works cost the
    // sum of the five steps that built it, which is what the player paid.
    for (const auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId != countryId) continue;
        auto iit = m_provinceIndustry.find(pid);
        if (iit != m_provinceIndustry.end()) {
            const int lv = std::clamp(iit->second.level, 0, IND_MAX_LEVEL);
            for (int k = 1; k <= lv; ++k) v += IND_COST[k];
            const int fl = std::clamp(iit->second.fortification, 0, 5);
            for (int k = 1; k <= fl; ++k) v += FORT_COST[k];
        }
        auto pit = m_provincePorts.find(pid);
        if (pit != m_provincePorts.end())
            v += (double)PORT_COST_PER_LEVEL * std::max(0, pit->second.level);
    }

    // ── AND WHAT IT FIELDS ──
    // Counted through countryTroops so men standing in a battle are not free,
    // which is the same mistake fuel, munitions and both upkeep paths made.
    for (const auto& [pid, units] : m_provinceArmies) {
        for (const auto& u : units) {
            if (u.countryId != countryId) continue;
            v += (double)u.count / 10000.0 * troopCost(u.type).money;
        }
    }
    for (const auto& b : m_battles)
        for (int t = 0; t < TROOP_TYPE_COUNT; ++t)
            if (b.attackerCid == countryId)
                v += (double)b.men.men[t] / 10000.0 * troopCost((TroopType)t).money;
    for (const auto& sh : m_ships) {
        if (sh.countryId != countryId) continue;
        v += (sh.type == "carrier") ? CARRIER_COST : DESTROYER_COST;
    }

    const Country* c = m_countries.getCountry(countryId);
    if (c) v += std::max(0.0, c->treasury);
    return (float)v;
}

void Game::recordIncomeSnapshot() {
    refreshIncomeCache();
    // ── ONE PASS OVER THE WORLD, NOT ONE PER COUNTRY ──
    //
    // countryNationalValue and countryPopulation each walk every province, and
    // calling them from inside a loop over every country made this O(countries
    // x provinces) -- on the shipped map about two hundred countries against
    // five thousand provinces, every turn, plus the armies and the fleet walked
    // once per country on top. Reported as "process turn feels pretty slow",
    // and it is the only thing in this turn that grew by a factor of the world.
    //
    // The same numbers, accumulated by owner in a single sweep. The two helpers
    // stay for the panel, which asks about one country and can afford it.
    std::unordered_map<int, long long> popBy;
    std::unordered_map<int, double> valBy;
    for (const auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId <= 0) continue;
        auto popIt = m_provincePopulations.find(pid);
        if (popIt != m_provincePopulations.end()) popBy[p.countryId] += popIt->second;
        double v = 0.0;
        auto iit = m_provinceIndustry.find(pid);
        if (iit != m_provinceIndustry.end()) {
            const int lv = std::clamp(iit->second.level, 0, IND_MAX_LEVEL);
            for (int k = 1; k <= lv; ++k) v += IND_COST[k];
            const int fl = std::clamp(iit->second.fortification, 0, 5);
            for (int k = 1; k <= fl; ++k) v += FORT_COST[k];
        }
        auto pit = m_provincePorts.find(pid);
        if (pit != m_provincePorts.end())
            v += (double)PORT_COST_PER_LEVEL * std::max(0, pit->second.level);
        valBy[p.countryId] += v;
    }
    for (const auto& [pid, units] : m_provinceArmies)
        for (const auto& u : units)
            if (u.countryId > 0)
                valBy[u.countryId] += (double)u.count / 10000.0 * troopCost(u.type).money;
    for (const auto& b : m_battles)
        if (b.attackerCid > 0)
            for (int t = 0; t < TROOP_TYPE_COUNT; ++t)
                valBy[b.attackerCid] +=
                    (double)b.men.men[t] / 10000.0 * troopCost((TroopType)t).money;
    for (const auto& sh : m_ships)
        if (sh.countryId > 0)
            valBy[sh.countryId] += (sh.type == "carrier") ? CARRIER_COST : DESTROYER_COST;

    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID) continue;
        auto cs = m_countryIncomeCache[cid];
        cs.nationalValue = (float)(valBy[cid] + std::max(0.0, c.treasury));
        cs.population    = popBy[cid];
        m_incomeHistory[cid].push_back(cs);
        while (m_incomeHistory[cid].size() > 12)
            m_incomeHistory[cid].erase(m_incomeHistory[cid].begin());
        if (m_countryBalances.find(cid) == m_countryBalances.end())
            m_countryBalances[cid] = 0;
        m_countryBalances[cid] += cs.net;
        // Treasury is NOT credited here. processEconomy() already applied this
        // turn's net income (Game_TurnLogic.cpp), so doing it again made every
        // country earn double the income actually shown in the UI — which also
        // drove treasuries into the range where float precision breaks down.
        // This function only records history/balances.
    }
}

void Game::updateEconomy() {
    Vector2 mouse = getMouse();
    if (IsKeyPressed(KEY_ESCAPE)) {
        m_inEconomy = false;
        m_activeSidebarTab = 0;
        m_economyTab = 0;
        m_economyShowWorst = false;
        m_economyScroll = 0;
        m_economyExpScroll = 0;
        m_economyGrossScroll = 0;
        return;
    }
    int centerX = m_screenW / 2;
    int tabY = 100;
    int tabSpacing = 200;

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        Rectangle closeBtn = {(float)(m_screenW - 44), 8, 36, 36};
        if (CheckCollisionPointRec(mouse, closeBtn)) {
            m_inEconomy = false;
            m_activeSidebarTab = 0;
            m_economyTab = 0;
            m_economyShowWorst = false;
            m_economyScroll = 0;
            m_economyExpScroll = 0;
            m_economyGrossScroll = 0;
            printf("[DIAG] Economy X button clicked\n");
            return;
        }
        const char* tabs[] = {"Global Economy", "Local Economy"};
        int visibleTabs = 2;
        int tabStartX = centerX - (visibleTabs * tabSpacing) / 2 + tabSpacing / 2;
        for (int t = 0; t < 2; ++t) {
            int tx = tabStartX + t * tabSpacing;
            int tw = MeasureText(tabs[t], 24);
            Rectangle tr = {(float)(tx - tw/2 - 10), (float)(tabY - 5), (float)(tw + 20), 34};
            if (CheckCollisionPointRec(mouse, tr)) {
                m_economyTab = t;
                m_economyScroll = 0;
                m_economyExpScroll = 0;
                m_economyGrossScroll = 0;
                return;
            }
        }
        if (m_economyTab == 0) {
            const char* toggleLabel = m_economyShowWorst ? "Show Top 10" : "Show Bottom 10";
            int tw = MeasureText(toggleLabel, 16);
            int startY = tabY + 70;
            Rectangle toggleBtn = {(float)(centerX - tw/2 - 10), (float)(startY - 30), (float)(tw + 20), 26};
            if (CheckCollisionPointRec(mouse, toggleBtn)) {
                m_economyShowWorst = !m_economyShowWorst;
            }
        }
    }
    int wheel = odMouseWheel();
    if (wheel != 0 && m_economyTab == 0) {
        int scrollSpeed = 20;
        int marginX = 16;
        int colGap = 4;
        int availW = m_screenW - marginX * 2;
        int colW = (availW - colGap * 2) / 3;
        if (colW < 140) { colW = 140; availW = colW * 3 + colGap * 2; }
        int col0 = (m_screenW - availW) / 2;

        int listTop = tabY + 70 + 4 + std::min(140, (m_screenH - (tabY + 70) - 60) / 3) + 4;

        if (mouse.y >= listTop) {
            int colIdx = (int)((mouse.x - col0) / (colW + colGap));
            if (colIdx < 0) colIdx = 0;
            if (colIdx > 2) colIdx = 2;
            int* scrollVar = (colIdx == 0) ? &m_economyGrossScroll :
                             (colIdx == 1) ? &m_economyScroll :
                                             &m_economyExpScroll;
            *scrollVar -= wheel * scrollSpeed;
            if (*scrollVar < 0) *scrollVar = 0;
        }
    }
}

void Game::drawEconomy() {
    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 180});
    int centerX = m_screenW / 2;
    int tabY = 100;
    int tabSpacing = 200;

    const char* tabs[] = {"Global Economy", "Local Economy"};
    int visibleTabs = 2;
    int tabStartX = centerX - (visibleTabs * tabSpacing) / 2 + tabSpacing / 2;
    for (int t = 0; t < 2; ++t) {
        int tx = tabStartX + t * tabSpacing;
        bool active = (t == m_economyTab);
        Color tc = active ? hexToColor(m_config.accent()) : LIGHTGRAY;
        DrawText(tabs[t], tx - MeasureText(tabs[t], 24) / 2, tabY, 24, tc);
        if (active) {
            int tw = MeasureText(tabs[t], 24);
            DrawRectangle(tx - tw / 2, tabY + 28, tw, 3, hexToColor(m_config.accent()));
        }
    }

    Rectangle closeBtn = {(float)(m_screenW - 44), 8, 36, 36};
    Vector2 mouse = getMouse();
    bool closeHover = CheckCollisionPointRec(mouse, closeBtn);
    Color closeCol = closeHover ? RED : Color{180, 180, 180, 200};
    DrawRectangleRounded(closeBtn, 0.2f, 6, {60, 60, 70, 180});
    DrawRectangleRoundedLines(closeBtn, 0.2f, 6, closeCol);
    int xw = MeasureText("X", 20);
    DrawText("X", (int)(closeBtn.x + closeBtn.width/2 - xw/2), 12, 20, closeCol);

    DrawText(T("ESC to close"), m_screenW - 140, 55, 14, Color{120, 120, 140, 150});

    int startY = tabY + 70;
    if (m_economyTab == 0) {
        drawEconomyGlobal(centerX, startY);
    } else {
        drawEconomyLocal(centerX, startY);
    }
}

void Game::drawEconomyGlobal(int centerX, int startY) {
    struct CountryEcon {
        int cid;
        float gross, resource, pop, expenses, net, total;
        Color color;
    };
    std::vector<CountryEcon> list;
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID) continue;
        auto cs = computeCountryIncome(cid);
        list.push_back({cid, cs.gross, cs.resource, cs.pop, cs.expenses, cs.net, cs.total, c.color});
    }

    auto getName = [&](int cid) -> std::string {
        const Country* c = m_countries.getCountry(cid);
        return c ? od::i18n::properName(c->name) : "CID" + std::to_string(cid);
    };

    auto buildData = [&](const std::vector<CountryEcon>& src, std::function<float(const CountryEcon&)> extract) {
        std::vector<CountryEcon> sorted = src;
        std::sort(sorted.begin(), sorted.end(), [&](auto& a, auto& b) { return extract(a) > extract(b); });
        std::vector<CountryEcon> d;
        int n = std::min(10, (int)sorted.size());
        if (m_economyShowWorst) {
            std::sort(sorted.begin(), sorted.end(), [&](auto& a, auto& b) { return extract(a) < extract(b); });
            for (int i = 0; i < n; ++i) d.push_back(sorted[i]);
        } else {
            for (int i = 0; i < n; ++i) d.push_back(sorted[i]);
        }
        return d;
    };

    auto extractGross     = [](const CountryEcon& e) { return e.gross; };
    auto extractNet       = [](const CountryEcon& e) { return e.net; };
    auto extractExpenses  = [](const CountryEcon& e) { return e.expenses; };

    auto dGross  = buildData(list, extractGross);
    auto dNet    = buildData(list, extractNet);
    auto dExp    = buildData(list, extractExpenses);

    const char* toggleLabel = m_economyShowWorst ? "Show Top 10" : "Show Bottom 10";
    int tw = MeasureText(toggleLabel, 16);
    int toggleX = centerX;
    Rectangle toggleBtn = {(float)(toggleX - tw/2 - 10), (float)(startY - 30), (float)(tw + 20), 26};
    Vector2 mouse = getMouse();
    bool toggleHover = CheckCollisionPointRec(mouse, toggleBtn);
    Color toggleCol = toggleHover ? ColorAlpha(hexToColor(m_config.accent()), 200.0f/255.0f) : ColorAlpha(hexToColor(m_config.accent()), 150.0f/255.0f);
    DrawRectangleRounded(toggleBtn, 0.2f, 6, ColorAlpha(hexToColor(m_config.accent()), 40.0f/255.0f));
    DrawRectangleRoundedLines(toggleBtn, 0.2f, 6, toggleCol);
    DrawText(toggleLabel, (int)(toggleX - tw/2), startY - 26, 16, toggleCol);

    int marginX = 16;
    int colGap = 4;
    int availW = m_screenW - marginX * 2;
    int colW = (availW - colGap * 2) / 3;
    if (colW < 140) { colW = 140; availW = colW * 3 + colGap * 2; }
    int col0 = (m_screenW - availW) / 2;

    int graphH = std::clamp((m_screenH - startY - 30) * 35 / 100, 140, 240);
    int listTop = startY + 4 + graphH + 28;
    int listH = std::clamp(m_screenH - listTop - 50, 120, 400);

    auto drawGraph = [&](int x, int y, int w, int h, const std::vector<CountryEcon>& data,
                          const std::string& title, std::function<float(const CountryEcon&)> extract) {
        // Translated here rather than at the three call sites, the same way
        // drawButton does it: this is the one place the heading is drawn.
        DrawText(T(title), x, y, 16, WHITE);
        y += 22;
        if (data.empty()) { DrawText(T("No data"), x + 5, y + 5, 12, LIGHTGRAY); return; }
        int barArea = w - 14;
        int barGap = 3;
        int barCnt = (int)data.size();
        int barW = (barArea - barGap * (barCnt + 1)) / barCnt;
        if (barW < 6) barW = 6;
        if (barW > 26) barW = 26;

        float maxVal = 0;
        for (auto& e : data) { float v = extract(e); if (fabsf(v) > maxVal) maxVal = fabsf(v); }
        if (maxVal <= 0) maxVal = 1;
        float scale = (h - 44) / maxVal;

        BeginScissorMode(x, y, w, h);

        for (auto& e : data) {
            if (extract(e) < 0) {
                DrawLine(x + 6, y + h - 22, x + w - 6, y + h - 22, {80, 80, 100, 150});
                break;
            }
        }

        for (int i = 0; i < barCnt; ++i) {
            float v = extract(data[i]);
            int bx = x + 6 + barGap + i * (barW + barGap);
            int bh = (int)(fabsf(v) * scale);
            if (bh < 1 && v != 0) bh = 1;
            int by = (v >= 0) ? (y + h - 22 - bh) : (y + h - 22);
            Color barCol = (v >= 0) ? data[i].color : odPalette::of(odPalette::Role::Bad);
            DrawRectangle(bx, by, barW, bh, barCol);
        }

        bool showNames = barW >= 14;
        int labelCnt = showNames ? std::min(barCnt, 10) : std::min(barCnt, 5);
        for (int i = 0; i < labelCnt; ++i) {
            int bx = x + 6 + barGap + i * (barW + barGap);
            std::string name = getName(data[i].cid);
            // AS MANY CHARACTERS AS FIT THE BAR, not a fixed five.
            //
            // Five was five bytes, which was five Latin letters and one and
            // two thirds of a Japanese one -- so the label was cut mid-
            // character and drew as "南?". Counting CHARACTERS fixes the
            // mojibake and leaves the other half of the problem: five kanji
            // are about three times as wide as five letters at this size, and
            // the labels ran into each other. Measuring is the only rule that
            // holds for twenty languages at once.
            const int slot = barW + barGap;
            if (odText::measureText(name.c_str(), 8) > slot) {
                int n = odText::charCount(name);
                while (n > 1) {
                    const std::string cut = odText::firstChars(name, --n) + ".";
                    if (odText::measureText(cut.c_str(), 8) <= slot) { name = cut; break; }
                    if (n == 1) name = cut;
                }
            }
            DrawText(name.c_str(), bx, y + h - 20, 8, LIGHTGRAY);
            float v = extract(data[i]);
            if (v != 0) {
                int bh = (int)(fabsf(v) * scale);
                if (bh < 1) bh = 1;
                int vy = (v >= 0) ? (y + h - 22 - bh - 12) : (y + h - 22 + bh + 2);
                float dv = (fabsf(v) < 0.5f) ? 0 : v;
                DrawText(TextFormat("%.0f", dv), bx, vy, 8, WHITE);
            }
        }

        EndScissorMode();
    };

    drawGraph(col0, startY + 4, colW, graphH, dGross, "Gross Income", extractGross);
    drawGraph(col0 + colW + colGap, startY + 4, colW, graphH, dNet, "Net Income", extractNet);
    drawGraph(col0 + (colW + colGap) * 2, startY + 4, colW, graphH, dExp, "Expenses", extractExpenses);

    auto drawScrollableList = [&](int x, int y, int w, int h, const char* title,
                                   const std::vector<CountryEcon>& sortedList,
                                   std::function<const char*(const CountryEcon&)> formatLine,
                                   int& scrollVar) {
        int headerH = 22;
        int entryH = 18;
        int contentH = (int)sortedList.size() * entryH;
        int scrollH = h - headerH - 4;
        int maxScroll = std::max(0, contentH - scrollH);
        if (scrollVar > maxScroll) scrollVar = maxScroll;
        if (scrollVar < 0) scrollVar = 0;

        DrawRectangle(x, y, w, h, {20, 20, 30, 180});
        DrawText(T(title), x + 6, y + 3, 14, WHITE);
        DrawLine(x, y + headerH, x + w, y + headerH, {60, 60, 80, 150});

        int clipY = y + headerH + 2;
        int clipH = h - headerH - 2;
        BeginScissorMode(x, clipY, w, clipH);

        int yOff = clipY - scrollVar;
        for (size_t i = 0; i < sortedList.size(); ++i) {
            auto& e = sortedList[i];
            Color textCol = (e.cid == m_playerCountryId) ? hexToColor(m_config.accent()) : LIGHTGRAY;
            DrawText(formatLine(e), x + 6, yOff + i * entryH, 12, textCol);
        }

        EndScissorMode();

        if (maxScroll > 0) {
            int barX = x + w - 10;
            int barW_sb = 6;
            int barH_sb = std::max(20, (int)(scrollH * scrollH / (float)contentH));
            int barY_sb = clipY + (int)(scrollVar / (float)maxScroll * (scrollH - barH_sb));
            DrawRectangle(barX, clipY, barW_sb, scrollH, {40, 40, 50, 150});
            DrawRectangle(barX, barY_sb, barW_sb, barH_sb, {120, 120, 140, 200});
        }
    };

    auto formatGrossLine = [&](const CountryEcon& e) -> const char* {
        const Country* c = m_countries.getCountry(e.cid);
        std::string name = c ? od::i18n::properName(c->name)
                             : "CID" + std::to_string(e.cid);
        static char buf[128];
        float v = e.gross;
        if (fabsf(v) < 0.5f) v = 0;
        snprintf(buf, sizeof(buf), "%-8s %.0f", name.c_str(), v);
        return buf;
    };

    auto formatNetLine = [&](const CountryEcon& e) -> const char* {
        const Country* c = m_countries.getCountry(e.cid);
        std::string name = c ? od::i18n::properName(c->name)
                             : "CID" + std::to_string(e.cid);
        static char buf[128];
        float v = e.net;
        if (fabsf(v) < 0.5f) v = 0;
        snprintf(buf, sizeof(buf), "%-8s %.0f", name.c_str(), v);
        return buf;
    };

    auto formatExpLine = [&](const CountryEcon& e) -> const char* {
        const Country* c = m_countries.getCountry(e.cid);
        std::string name = c ? od::i18n::properName(c->name)
                             : "CID" + std::to_string(e.cid);
        static char buf[128];
        float v = e.expenses;
        if (fabsf(v) < 0.5f) v = 0;
        snprintf(buf, sizeof(buf), "%-8s %.0f", name.c_str(), v);
        return buf;
    };

    std::sort(list.begin(), list.end(),
              [](auto& a, auto& b) { return a.gross > b.gross; });

    std::vector<CountryEcon> listSortedByNet = list;
    std::sort(listSortedByNet.begin(), listSortedByNet.end(),
              [](auto& a, auto& b) { return a.net > b.net; });

    std::vector<CountryEcon> listSortedByExp = list;
    std::sort(listSortedByExp.begin(), listSortedByExp.end(),
              [](auto& a, auto& b) { return a.expenses > b.expenses; });

    drawScrollableList(col0, listTop, colW, listH, "Top Gross Income",
                       list, formatGrossLine, m_economyGrossScroll);

    drawScrollableList(col0 + colW + colGap, listTop, colW, listH, "Top Net Income",
                       listSortedByNet, formatNetLine, m_economyScroll);

    drawScrollableList(col0 + (colW + colGap) * 2, listTop, colW, listH, "Top Expenses",
                       listSortedByExp, formatExpLine, m_economyExpScroll);
}

void Game::drawEconomyLocal(int centerX, int startY) {
    int cid = m_playerCountryId;
    if (cid <= 0 || cid == SPC_CID) {
        DrawText(T("Select a country to view local economy"), centerX - 200, startY + 40, 18, LIGHTGRAY);
        return;
    }
    const Country* c = m_countries.getCountry(cid);
    if (!c) return;

    auto cs = computeCountryIncome(cid);
    int pieStartY = startY + 30;

    int lx = std::max(20, centerX - 340);
    DrawText(TextFormat(T("%s - Economic Breakdown"),
                        od::i18n::properName(c->name).c_str()), lx, startY, 20, WHITE);
    startY += 30;

    int valX = lx + 260;

    startY = drawBreakdownRow(lx, startY, valX, "Component", "Amount", LIGHTGRAY, false);
    startY = drawBreakdownRow(lx, startY, valX, "Industry Income (base)", TextFormat("%.1f", cs.gross), WHITE, false);
    startY = drawBreakdownRow(lx, startY, valX, "Resource Bonus", TextFormat("%.1f", cs.resource), WHITE, false);
    startY = drawBreakdownRow(lx, startY, valX, "Population Bonus", TextFormat("%.1f", cs.pop), WHITE, false);
    DrawRectangle(lx, startY, 320, 1, {100, 100, 120, 150});
    startY += 6;
    startY = drawBreakdownRow(lx, startY, valX, "Gross Income", TextFormat("%.1f", cs.total), WHITE, false);
    startY += 4;
    // Shown with the percentage, and shown even at zero, because an upkeep the
    // player cannot see is a tax they conclude is a bug. The number they need
    // in order to decide whether the next factory is worth building is the RATE
    // it is charged at, not the total.
    if (cs.industryLevels > 0) {
        const float pct = cs.gross > 0.0f ? (cs.industryUpkeep / cs.gross) * 100.0f : 0.0f;
        startY = drawBreakdownRow(lx, startY, valX,
                                  TextFormat(T("Industry Upkeep (%.0f%% of %d lvl)"), pct, cs.industryLevels),
                                  TextFormat("-%.1f", cs.industryUpkeep),
                                  Color{255, 210, 160, 255}, false);
    }
    // The RATE beside the total, exactly as industry upkeep does above and for
    // the same reason: the number a player needs in order to decide whether the
    // next doctrine is worth it is what it changes, not what it currently adds
    // up to. Shown only when a doctrine or a research node is actually moving
    // it, so an ordinary country's panel is unchanged.
    {
        const float upkeepPct = getTotalEffect("maintenanceCostPct", cid);
        if (fabsf(upkeepPct) >= 0.5f)
            startY = drawBreakdownRow(lx, startY, valX,
                                      TextFormat(T("Army Cost (%+.0f%% upkeep)"), -upkeepPct),
                                      TextFormat("-%.1f", cs.armyExpenses),
                                      Color{255, 180, 180, 255}, false);
        else
            startY = drawBreakdownRow(lx, startY, valX, "Army Cost",
                                      TextFormat("-%.1f", cs.armyExpenses),
                                      Color{255, 180, 180, 255}, false);
    }
    if (cs.navyExpenses > 0) {
        startY = drawBreakdownRow(lx, startY, valX, "Navy Cost", TextFormat("-%.1f", cs.navyExpenses), Color{255, 180, 255, 255}, false);
    }
    if (cs.policyCosts > 0) {
        startY = drawBreakdownRow(lx, startY, valX, "Doctrine Costs", TextFormat("-%.1f", cs.policyCosts), Color{180, 200, 255, 255}, false);
    }
    if (cs.minorityCosts > 0) {
        startY = drawBreakdownRow(lx, startY, valX, "Minority Programmes", TextFormat("-%.1f", cs.minorityCosts), Color{255, 200, 180, 255}, false);
    }
    if (cs.researchCost > 0) {
        startY = drawBreakdownRow(lx, startY, valX, "Research Allocation", TextFormat("-%.1f", cs.researchCost), Color{180, 255, 180, 255}, false);
    }
    if (cs.pacificationCost > 0) {
        startY = drawBreakdownRow(lx, startY, valX, "Pacification Budget", TextFormat("-%.1f", cs.pacificationCost), Color{180, 180, 255, 255}, false);
    }
    DrawRectangle(lx, startY, 320, 1, {100, 100, 120, 150});
    startY += 6;
    { float netv = (fabsf(cs.net) < 0.05f) ? 0 : cs.net;
    startY = drawBreakdownRow(lx, startY, valX, "Net Income", TextFormat("%.1f", netv), netv >= 0 ? hexToColor(m_config.accent()) : odPalette::of(odPalette::Role::Bad), false); }
    startY += 10;

    // ── WHAT THE COUNTRY MAKES, AND WHETHER IT IS ENOUGH ──
    //
    // Only in a world that runs the production economy; absent entirely
    // otherwise, so the panel a player has always seen is unchanged.
    //
    // THIS IS NOT DECORATION. Living standards feed unrest
    // (getProvinceRebellionChance), and a government losing provinces to
    // rebellion because a number it cannot see is below one would be exactly
    // the failure this codebase already names for industry upkeep -- "a tax a
    // player can see and cannot answer reads as a bug even when the arithmetic
    // is right". The shortfall is stated in the same units as the production
    // beside it, so the answer -- build more, or trade for it -- is legible
    // from the panel rather than from the source.
    if (m_goodsEconomy) {
        auto prodIt = m_countryProduction.find(cid);
        if (prodIt != m_countryProduction.end()) {
            const CountryProduction& pr = prodIt->second;
            auto spIt = m_countryStockpiles.find(cid);

            DrawText(T("Production"), lx, startY, 16, WHITE);
            startY += 22;

            const float fed = pr.livingStandards;
            const Color fedCol = fed >= 0.999f ? odPalette::of(odPalette::Role::Good)
                               : fed >= 0.75f  ? Color{255, 210, 160, 255}
                                               : odPalette::of(odPalette::Role::Bad);
            startY = drawBreakdownRow(lx, startY, valX, "Living Standards",
                                      TextFormat("%.0f%%", fed * 100.0f), fedCol, true);
            if (fed < 0.999f) {
                const float shortBy = pr.demand[GOOD_CONSUMER] - pr.consumed[GOOD_CONSUMER];
                startY = drawBreakdownRow(lx, startY, valX, "  short of consumer goods",
                                          TextFormat("%.1f/turn", shortBy),
                                          odPalette::of(odPalette::Role::Bad), false);
            }

            for (int g = 0; g < GOOD_COUNT; ++g) {
                const float stock = (spIt != m_countryStockpiles.end())
                                        ? spIt->second.goods[g] : 0.0f;
                // Made per turn, and held. Both, because one turn's production
                // says whether the economy is working and the pile says whether
                // it can afford to lose a province that was feeding it.
                startY = drawBreakdownRow(
                    lx, startY, valX, goodName(g),
                    TextFormat(T("%.1f/turn  (%.0f held)"), pr.produced[g], stock),
                    WHITE, false);
            }

            if (pr.factoriesTotal > 0 && pr.factoriesIdle > 0) {
                // Named rather than left to be inferred from a low output: an
                // idle factory is a fixable thing, and the fix (get the
                // materials) is different from the fix for a small one.
                startY = drawBreakdownRow(lx, startY, valX, "Factories idle",
                                          TextFormat("%d of %d", pr.factoriesIdle,
                                                     pr.factoriesTotal),
                                          Color{255, 210, 160, 255}, false);
            }
            if (pr.rawSold > 0.05f) {
                startY = drawBreakdownRow(lx, startY, valX, "Surplus sold",
                                          TextFormat("+%.1f", pr.rawSold),
                                          Color{180, 255, 180, 255}, false);
            }
            // Fuel the army needed and the country did not have, bought in at a
            // premium. Named rather than buried in the net, because it is a
            // large, fixable bill: build refineries, take an oil province, or
            // demobilise. A player who cannot see it just watches their
            // treasury drain for no stated reason.
            if (pr.fuelBought > 0.005f) {
                startY = drawBreakdownRow(lx, startY, valX, "Fuel bought in",
                                          TextFormat("-%.1f", pr.fuelBought * FUEL_SHORTFALL_PRICE),
                                          odPalette::of(odPalette::Role::Bad), false);
            }
            startY += 10;
        }
    }

    // ── THE GRAPHS, AND WHICH COLUMN EACH BELONGS IN ──
    //
    // All three were drawn down the left, under a breakdown table that is
    // already 300 px tall, and the third ran off the bottom of the screen
    // entirely. The right column holds two pie charts and then several hundred
    // pixels of nothing, so the two national-value graphs go there and the
    // balance stays beside the table whose bottom line it plots.
    std::vector<float> incomeV, expenseV, netV, valueV, perHeadV;
    float valNow = 0.0f, headNow = 0.0f;
    bool haveHist = false;
    const int graphW = 350, graphH = 92;
    auto plot = [&](int gx, int gy,
                    const std::vector<std::pair<std::vector<float>, Color>>& series,
                    bool zeroLine) {
        float lo = 0.0f, hi = 0.0f;
        for (const auto& [vals, col] : series)
            for (float v : vals) { lo = std::min(lo, v); hi = std::max(hi, v); }
        if (hi <= lo) hi = lo + 1.0f;
        const float span = hi - lo;
        DrawRectangle(gx, gy, graphW, graphH, Color{14, 16, 22, 170});
        DrawRectangleLines(gx, gy, graphW, graphH, Color{80, 80, 100, 150});
        auto yOf = [&](float v) {
            return gy + graphH - 3 - (int)((v - lo) / span * (graphH - 6));
        };
        // ZERO IS DRAWN WHEN IT IS IN RANGE, because "is this line above or
        // below nothing" is the entire question for a balance.
        if (zeroLine && lo < 0.0f && hi > 0.0f) {
            const int zy = yOf(0.0f);
            DrawLine(gx, zy, gx + graphW, zy, Color{110, 114, 132, 190});
        }
        for (const auto& [vals, col] : series) {
            if (vals.size() < 2) continue;
            for (size_t i = 1; i < vals.size(); ++i) {
                const int x1 = gx + (int)((i - 1) * graphW / (float)(vals.size() - 1));
                const int x2 = gx + (int)(i * graphW / (float)(vals.size() - 1));
                DrawLineEx({(float)x1, (float)yOf(vals[i - 1])},
                           {(float)x2, (float)yOf(vals[i])}, 2.0f, col);
            }
        }
        return gy + graphH + 8;
    };
    const Color cIn  = SKYBLUE;
    const Color cOut = Color{210, 90, 90, 255};
    const Color cNet = odPalette::of(odPalette::Role::Good);
    const Color cVal = hexToColor(m_config.accent());
    const Color cHead = Color{200, 180, 120, 255};

    auto histIt = m_incomeHistory.find(cid);
    if (histIt != m_incomeHistory.end() && histIt->second.size() >= 2) {
        auto& hist = histIt->second;
        haveHist = true;

        // ONE GRAPH DRAWER, THREE QUESTIONS. What was here was five lines of
        // five different quantities on one pair of axes, sharing a maximum --
        // so net income, the one line a player actually watches, was a flat
        // scratch along the bottom whenever gross income was large. Its legend
        // sat to the RIGHT of the plot, which is where the pie charts live, so
        // the two overlapped and neither could be read.
        for (auto& h : hist) {
            incomeV.push_back(h.total);
            expenseV.push_back(h.expenses);
            netV.push_back(h.net);
            valueV.push_back(h.nationalValue);
            perHeadV.push_back(h.population > 0
                ? h.nationalValue / ((float)h.population / 1000000.0f) : 0.0f);
        }
        valNow  = hist.back().nationalValue;
        headNow = perHeadV.empty() ? 0.0f : perHeadV.back();

        // The legend is in the title line, where it cannot collide with anything.
        DrawText(T("Income vs Expenses"), lx, startY, 15, WHITE);
        DrawText(T("income"),   lx + 200, startY + 2, 12, cIn);
        DrawText(T("expenses"), lx + 256, startY + 2, 12, cOut);
        DrawText(T("net"),      lx + 322, startY + 2, 12, cNet);
        startY = plot(lx, startY + 19,
                      {{incomeV, cIn}, {expenseV, cOut}, {netV, cNet}}, true);
    } else {
        DrawText(T("(No historical data yet — play more turns)"), lx, startY, 12, Color{120, 120, 140, 200});
        startY += 22;
    }

    int pieX = centerX + 80;
    int pieY = pieStartY;
    int pieBottom = pieY, legBottom = pieY;   // how far the first chart reaches
    float expTotal = cs.expenses;
    if (expTotal > 0) {
        DrawText(T("Expense Composition"), pieX - 50, pieY, 16, WHITE);
        pieY += 24;

        // The wedges, their colours and their labels come from Game::
        // expenseSlices -- the country profile publishes the same breakdown,
        // and two copies of this table would drift the first time a cost was
        // added. The note that used to live here, about industry upkeep being
        // in the denominator with no wedge of its own, is on the definition.
        const std::vector<ExpenseSlice> slices = expenseSlices(cs);

        int radius = 55;
        int cx = pieX;
        int cy = pieY + radius;
        float startAngle = 0;
        for (auto& sl : slices) {
            if (sl.value <= 0) continue;
            float sweep = (sl.value / expTotal) * 360.0f;
            DrawCircleSector({(float)cx, (float)cy}, (float)radius, startAngle, startAngle + sweep, 40, sl.col);
            startAngle += sweep;
        }
        DrawCircleLines(cx, cy, radius, {100, 100, 120, 150});
        pieBottom = cy + radius;

        int legX = pieX + radius + 20;
        int legY = pieY + radius - 40;
        for (auto& sl : slices) {
            if (sl.value <= 0) continue;
            DrawRectangle(legX, legY, 10, 10, sl.col);
            float pct = (sl.value / expTotal) * 100.0f;
            DrawText(TextFormat("%s: %.1f%%", sl.label.c_str(), pct), legX + 14, legY, 12, LIGHTGRAY);
            legY += 16;
        }
        legBottom = legY;
    }

    // ── CLEAR OF WHAT IS ABOVE IT, MEASURED RATHER THAN GUESSED ──
    //
    // This advanced 100 px from the first chart's TITLE. The chart itself is
    // 110 px of circle below that title, and its legend runs further still --
    // seven expense categories at 16 px each. So the second pie was drawn on
    // top of the first one's legend, every time a country had more than about
    // four kinds of expense. Now it starts below whichever of the two actually
    // reaches lower.
    pieY = (expTotal > 0) ? std::max(pieBottom, legBottom) + 18 : pieY + 40;
    float incTotal = cs.total;
    if (incTotal > 0) {
        DrawText(T("Income Composition"), pieX - 50, pieY, 16, WHITE);
        pieY += 24;

        struct Slice { float val; Color col; const char* label; };
        Slice slices[] = {
            {cs.gross, SKYBLUE, T("Industry")},
            {cs.resource, odPalette::of(odPalette::Role::Good), T("Resource")},
            {cs.pop, ORANGE, T("Population")},
        };

        int radius = 55;
        int cx = pieX;
        int cy = pieY + radius;
        float startAngle = 0;
        for (auto& sl : slices) {
            if (sl.val <= 0) continue;
            float sweep = (sl.val / incTotal) * 360.0f;
            DrawCircleSector({(float)cx, (float)cy}, (float)radius, startAngle, startAngle + sweep, 40, sl.col);
            startAngle += sweep;
        }
        DrawCircleLines(cx, cy, radius, {100, 100, 120, 150});

        int legX = pieX + radius + 20;
        int legY = pieY + radius - 40;
        for (auto& sl : slices) {
            if (sl.val <= 0) continue;
            DrawRectangle(legX, legY, 10, 10, sl.col);
            float pct = (sl.val / incTotal) * 100.0f;
            DrawText(TextFormat("%s: %.1f%%", sl.label, pct), legX + 14, legY, 12, LIGHTGRAY);
            legY += 16;
        }
        pieY = std::max(cy + radius, legY) + 20;
    }

    // ── WHAT THE COUNTRY IS WORTH, UNDER THE PIES ──
    //
    // Everything else on this screen is a rate: what came in this turn, what
    // went out. None of it answers how much there IS, so a country that spent
    // forty turns turning income into factories and divisions reads the same as
    // one that spent it on nothing. This is the price list run backwards over
    // what it holds.
    //
    // On its own axis, and separately from the per-head figure below it,
    // because the two are orders of magnitude apart and a shared scale would
    // flatten one into the baseline -- which is exactly what the old five-line
    // graph did to net income.
    if (haveHist) {
        const int gx = pieX - 50;
        DrawText(T("National Value"), gx, pieY, 15, WHITE);
        DrawText(TextFormat("$%s", formatBalance(valNow).c_str()),
                 gx + 200, pieY + 2, 13, cVal);
        pieY = plot(gx, pieY + 19, {{valueV, cVal}}, false);

        DrawText(T("Value per 1M people"), gx, pieY, 15, WHITE);
        DrawText(TextFormat("$%s", formatBalance(headNow).c_str()),
                 gx + 200, pieY + 2, 13, cHead);
        pieY = plot(gx, pieY + 19, {{perHeadV, cHead}}, false);

        // Said once, where it is being read.
        DrawText(T("Everything built and fielded, at what it cost: industry, forts,"),
                 gx, pieY, 11, Color{125, 129, 145, 255});
        DrawText(T("ports, every soldier and hull, plus the treasury."),
                 gx, pieY + 13, 11, Color{125, 129, 145, 255});
    }
}

int Game::drawBreakdownRow(int x, int y, int valX, const char* label, const char* value, Color col, bool highlight) {
    if (highlight) DrawRectangle(x - 4, y, 400, 22, {255, 255, 255, 12});
    // The label is a heading ("Gross Income"); the value is a number the
    // caller has already formatted, so only the label is translated here.
    // Same one-place rule as the button helpers -- see Game_Multiplayer.cpp.
    DrawText(T(label), x, y, 16, col);
    DrawText(value, valX, y, 16, col);
    return y + 22;
}
