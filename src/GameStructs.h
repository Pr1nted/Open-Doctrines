#pragma once

#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "raylib.h"
// T(): both war-goal tables below are sentences the player reads.
#include "i18n/Locale.h"
#include "BuildCosts.h"   // TroopType: an army is made of something now

// ─── Credits ─────────────────────────────────────────────────
struct CreditEntry {
    enum Type { ROLE, NAME, SMALL, DIVIDER, SPACER };
    Type type;
    std::string text;
    float spacing = 0;
};

// ─── Map Selection ──────────────────────────────────────────
struct MapEntry {
    std::string id;
    std::string name;
    std::string filename;     // .odmap filename
    std::string description;
    std::string thumbPath;    // PNG thumbnail
    std::string directory;    // full dir containing the map files
    bool isStandard = false;
    // True when the map is a bare "<name>.odmap" sitting directly in
    // custom_maps/ rather than its own subfolder. `directory` is then the
    // shared custom_maps/ root, so deleting must remove only `filename` —
    // never sweep the directory.
    bool isLooseFile = false;
    std::string author;       // map author
    std::string license;      // map license
    bool hasScripts = false;  // whether map has scripted events
};

// ─── Save World Info ────────────────────────────────────────
struct SaveWorldInfo {
    std::string filename;
    std::string worldName;
    std::string version;       // Game version this save was created with
    std::string lastPlayed;
    int turnCount = 0;
};

// ─── Notifications ──────────────────────────────────────────
struct Notification {
    std::string message;
    float timer = 0.0f;
    float duration = 5.0f;
    Color color = WHITE;
};

// ─── Popup Queue ────────────────────────────────────────────
enum class PopupType {
    NONE,
    REBELLION,         // breakaway state formed; OK only
    WAR_DECLARED,      // war declared on player; OK only
    DIPLOMATIC_REQUEST, // incoming alliance/guarantee/NAP request; Approve/Reject
    CEASEFIRE_REQUEST  // incoming ceasefire offer with terms; Approve/Reject
};

// ─── Why somebody said no ───────────────────────────────────
//
// A refusal used to be a bare `false`. The AI computed a perfectly good reason
// for it -- "already fighting two wars of its own", "the aggressor outguns both
// of us" -- and threw the reason away, so the player was told only that their
// offer was declined. An opponent whose every refusal is unexplained reads as
// arbitrary, and arbitrary reads as stupid even when the decision was sound.
//
// THIS GATES NOTHING. It is an utterance, not a precondition: no request is
// blocked for want of a reason, no reason has to be given, and deleting the
// whole mechanism leaves every action in the game exactly where it was.
//
// SILENCE IS A MOVE, and that only works because a reason is optional. A
// country that declines and says nothing has told you something real about
// itself, and it could not if the game demanded an answer.
//
// EITHER SIDE MAY LIE. What is stated is chosen separately from what is true,
// by the AI and by the player alike, from this one list. Believability is not
// enforced by hiding options -- both may say anything -- it is a consequence:
// a claim the other side can check against what it can already see is worse
// than having said nothing. See Game::refusalIsContradicted.
enum RefusalReason {
    REFUSE_NONE = 0,      // said nothing
    REFUSE_OWN_WARS,      // already fighting wars of its own      (publicly checkable)
    REFUSE_WEARINESS,     // the home front will not take another  (private)
    REFUSE_LOSING_GROUND, // losing ground on its own borders      (publicly checkable)
    REFUSE_OUTGUNNED,     // the other side is too strong          (roughly checkable)
    REFUSE_NO_INTEREST,   // simply does not want to               (a preference; unfalsifiable)
    REFUSE_DISTRUST,      // does not trust the asker              (a preference; unfalsifiable)
    REFUSE_COUNT
};

/** Player-facing phrasing, in the refuser's voice. */
inline const char* refusalText(int r) {
    switch (r) {
        case REFUSE_OWN_WARS:      return "they are already fighting wars of their own";
        case REFUSE_WEARINESS:     return "their people will not stand another war";
        case REFUSE_LOSING_GROUND: return "they are losing ground on their own borders";
        case REFUSE_OUTGUNNED:     return "they say the other side is too strong";
        case REFUSE_NO_INTEREST:   return "it is not in their interest";
        case REFUSE_DISTRUST:      return "they do not trust you";
        default:                   return nullptr;   // no reason given
    }
}

/** The same list in the player's own voice, for refusing a request. */
inline const char* refusalTextOwn(int r) {
    switch (r) {
        case REFUSE_OWN_WARS:      return "We are already at war elsewhere";
        case REFUSE_WEARINESS:     return "Our people will not stand another war";
        case REFUSE_LOSING_GROUND: return "We are losing ground at home";
        case REFUSE_OUTGUNNED:     return "They are too strong";
        case REFUSE_NO_INTEREST:   return "It is not in our interest";
        case REFUSE_DISTRUST:      return "We do not trust you";
        default:                   return "Say nothing";
    }
}

// Terms carried with a ceasefire request. Stored in the popup entry so the
// player can see exactly what is being offered/demanded before deciding.
/**
 * A REGIONAL LAW -- what a provincial administration decides, not what a
 * country is.
 *
 * These are deliberately a different kind of thing from the doctrines in the
 * Politics list. A doctrine is a statement about a whole state: where its
 * compass sits, who it lets in, what it does with its minorities. A curfew is
 * not. Neither is a land tax holiday, or whose language the courts run in.
 * Offering the national list per district would have been the same content
 * under a smaller heading, and most of it would have meant nothing applied to
 * a third of a country.
 *
 * So they are their own set, in data/district_laws.json, small and cheap and
 * each one a trade: calm bought with money, or money bought with resentment.
 * `costPerTurn` is PER PROVINCE, so a law is priced by how much ground it is
 * administered over.
 */
struct DistrictLaw {
    std::string id;
    std::string name;
    std::string description;
    float costPerTurn = 0.0f;   ///< per province in the district
    float unrestPct = 0.0f;     ///< added to the rebellion sum; negative calms
    float incomePct = 0.0f;     ///< % on this province's industry income
    float growthPct = 0.0f;     ///< % on its population growth
};

/**
 * A slice of a country that is governed as a unit.
 *
 * WHY THIS EXISTS. Pacification was one slider for a whole country: the same
 * suppression fell on a quiet heartland and on a province in open revolt, and
 * the only choice a player had was how much to overpay for the calm half in
 * order to reach the loud one. Districts make that a question of WHERE rather
 * than only HOW MUCH.
 *
 * THE BUDGET IS A SHARE, THE GROUND IS A SHARE, AND WHAT MATTERS IS THE RATIO.
 * A district holding a tenth of the country and drawing a tenth of the budget
 * is being policed exactly as hard as the old single slider policed everything
 * -- which is what makes this inert until somebody actually redraws the lines.
 * Draw a small district around the trouble and give it a quarter of the money
 * and it is policed several times harder, at the cost of everywhere else.
 */
struct District {
    int id = 0;                       ///< stable within its country
    std::string name;
    unsigned char r = 120, g = 140, b = 200;   ///< how it is painted
    std::vector<int> provinces;       ///< ascending; every province owned sits in exactly one
    int sharePct = 100;               ///< its claim on the pacification budget
    /**
     * Doctrines this district runs on its own, on top of the national ones.
     *
     * Local by design: the point of dividing a country up is to be able to
     * govern its halves differently, and a policy that could only ever be
     * national would make a district a budget line rather than a place.
     */
    std::vector<std::string> policies;
    /**
     * The name is STORED IN ENGLISH and translated when it is drawn.
     *
     * It used to be built with T() at creation time, which froze whichever
     * language was loaded into the save: districts named in Ukrainian stayed
     * Ukrainian after a switch to German, because by then they were just text.
     * The canonical form goes through od::i18n::properName, the same path a
     * country name takes.
     *
     * `customName` turns that off. A name the player typed is theirs, in the
     * language they typed it, and must never be re-rendered as though it were
     * generated.
     */
    bool customName = false;
};

struct CeasefireTerms {
    int ourMoney = 0;                    // money offered by the sender
    int theirMoney = 0;                  // money demanded from the recipient
    std::vector<int> ourProvs;           // provinces the sender cedes (province IDs)
    std::vector<int> theirProvs;        // provinces the sender demands (province IDs)
    std::vector<int> ourDropClaims;     // claims the sender drops (province IDs)
    std::vector<int> theirDropClaims;   // claims the sender demands the recipient drops (province IDs)

    /**
     * A nation set free as part of the deal, and the ground it is set free ON.
     *
     * Releasing was a unilateral button: a country could dismantle itself and
     * nobody could ever ASK it to, which is the shape of most of the real
     * settlements this game is about. Now it is a term like any other -- `our`
     * is what the sender does to itself, `their` what the sender demands of
     * the recipient.
     *
     * THE PROVINCES ARE CARRIED, not recomputed from the minority. What a
     * release consists of is a judgement -- which of the candidate's provinces
     * actually go -- and recomputing it at resolution would silently hand over
     * a different country than the one both sides agreed to, on any turn where
     * the front had moved in between.
     */
    std::string ourReleaseTag;          // minority id the sender frees
    std::vector<int> ourReleaseProvs;   // and the provinces it is given
    std::string theirReleaseTag;        // minority the sender demands be freed
    std::vector<int> theirReleaseProvs;
};

struct PopupEntry {
    PopupType type = PopupType::NONE;
    std::string title;
    std::string message;
    int countryId = 0;        // rebel CID or requesting CID
    std::string action;       // for diplomacy: "request_alliance", etc.
    std::string sourceIso;
    std::string targetIso;
    std::string subjectIso;   // used only for a call to arms: the aggressor
    CeasefireTerms terms;     // used only for CEASEFIRE_REQUEST

    /**
     * Identifies this popup for as long as it is queued.
     *
     * Only the front of the queue is on screen; the rest are waiting. The open
     * sound used to be played when a popup was PUSHED, so a turn that produced
     * six of them played six chimes at once while five were still invisible,
     * and then each one appeared in silence. This is what lets the sound follow
     * the popup that is actually being shown. Set by Game::pushPopup().
     */
    unsigned long long id = 0;
};

// ─── Relations System ───────────────────────────────────────
// ─── What a war is said to be about ─────────────────────────
//
// The same arrangement as RefusalReason, for the same reasons, and with the
// same hard rule: THIS GATES NOTHING. A declaration needs no goal, and anyone
// may declare war on anyone at any time exactly as before. The goal is
// something you may announce, not something you must have.
//
// The difference from a refusal is that here there are TWO of them. What a
// country SAYS it is fighting for is public and goes on the relation below.
// What it is ACTUALLY fighting for is private -- the AI keeps its own, and the
// player keeps theirs in their head -- and the only way to learn it is to watch
// which provinces get taken and which ones will not be traded away at the
// peace table. That is deduction from behaviour, which is the thing that makes
// an opponent worth reading, rather than a number displayed on a panel.
enum WarGoal {
    WAR_GOAL_NONE = 0,   // declared without saying why
    WAR_GOAL_RECONQUEST, // they hold land we claim   (checkable: claims are public)
    WAR_GOAL_SECURITY,   // they are a threat to us   (checkable: borders are public)
    WAR_GOAL_HUMBLE,     // they have grown too big   (checkable: the map is public)
    WAR_GOAL_ALLY,       // we were called in         (checkable: alliances are public)
    WAR_GOAL_CONQUEST,   // we want what they have    (honest; nothing to disprove)
    WAR_GOAL_COUNT
};

/** How the declaration reads to everyone else. */
inline const char* warGoalText(int g) {
    switch (g) {
        case WAR_GOAL_RECONQUEST: return T("to recover land they claim as theirs");
        case WAR_GOAL_SECURITY:   return T("citing the threat on their border");
        case WAR_GOAL_HUMBLE:     return T("to check a power they say has grown too large");
        case WAR_GOAL_ALLY:       return T("in support of an ally");
        case WAR_GOAL_CONQUEST:   return T("openly, for territory");
        default:                  return nullptr;   // no reason given
    }
}

/** The same list in the declaring country's own voice. */
inline const char* warGoalTextOwn(int g) {
    switch (g) {
        case WAR_GOAL_RECONQUEST: return T("To recover what is ours");
        case WAR_GOAL_SECURITY:   return T("They threaten our border");
        case WAR_GOAL_HUMBLE:     return T("They have grown too large");
        case WAR_GOAL_ALLY:       return T("In support of our ally");
        case WAR_GOAL_CONQUEST:   return T("For territory");
        default:                  return T("State no reason");
    }
}

struct CountryRelation {
    bool war = false;
    bool alliance = false;
    bool nonAggression = false;
    bool guarantee = false;
    /**
     * The goal the attacker ANNOUNCED, on the attacker->defender direction.
     *
     * Public by construction: it is a statement, and statements are the only
     * thing either side ever learns about the other's intent directly. It may
     * be false. It is not the goal the war is actually being fought for, and
     * nothing in the game treats it as though it were.
     */
    int warGoalStated = WAR_GOAL_NONE;
};

// ─── Resources ──────────────────────────────────────────────
struct ProvinceResource {
    float amount = 0.0f;   // 0-100 scale
    float boost = 0.0f;    // industry boost percentage
};

struct ProvinceResources {
    ProvinceResource oil;
    ProvinceResource gold;
    ProvinceResource rubber;
    ProvinceResource gemstones;
    ProvinceResource metal;
};

// ─── Goods ──────────────────────────────────────────────────
//
// WHAT A FACTORY IS FOR. Industry used to emit money and nothing else, so the
// only question a province ever answered was "how much". A player asked for the
// other question -- "of what" -- and for the consequence that makes it matter:
// "specific material/thing you need to create for your population (the higher
// and cheaper these things are, the better the living standards)".
//
// FOUR, AND NOT MORE. Every good is an AI action, a UI row, a save field, a
// tutorial page and twenty-one translations. Two was on the table and four was
// chosen; six would cost more than it explains. The inputs are the deposits the
// game already models, so nothing new has to be authored onto any map.
//
//   consumer    <- rubber, gemstones    the population. THIS is living standards.
//   machinery   <- metal                industry upgrades, forts, ports
//   fuel        <- oil                  navy, army movement, artillery
//   munitions   <- metal                recruitment, artillery
//
// Gold is deliberately not an input to anything: it stays money. Historically
// apt, and it saves inventing a conversion rule nobody asked for.
enum GoodId {
    GOOD_CONSUMER = 0,
    GOOD_MACHINERY,
    GOOD_FUEL,
    GOOD_MUNITIONS,
    GOOD_COUNT
};

/**
 * The raw materials a factory eats, which are the map's own deposits.
 *
 * Gold is absent ON PURPOSE -- see above. The order matches nothing in
 * ProvinceResources by accident: goodInputs() below is the only place the two
 * are related, so that a reader can check the mapping in one place instead of
 * trusting two enums to stay parallel.
 */
enum RawId {
    RAW_OIL = 0,
    RAW_METAL,
    RAW_RUBBER,
    RAW_GEMSTONES,
    RAW_COUNT
};

inline const char* goodName(int g) {
    switch (g) {
        case GOOD_CONSUMER:  return T("Consumer goods");
        case GOOD_MACHINERY: return T("Machinery");
        case GOOD_FUEL:      return T("Fuel");
        case GOOD_MUNITIONS: return T("Munitions");
        default:             return T("Nothing");
    }
}

/** Stable, lowercase, never translated: save files, mods and the bench. */
inline const char* goodKey(int g) {
    switch (g) {
        case GOOD_CONSUMER:  return "consumer";
        case GOOD_MACHINERY: return "machinery";
        case GOOD_FUEL:      return "fuel";
        case GOOD_MUNITIONS: return "munitions";
        default:             return "";
    }
}

inline const char* rawName(int r) {
    switch (r) {
        case RAW_OIL:       return T("Oil");
        case RAW_METAL:     return T("Metal");
        case RAW_RUBBER:    return T("Rubber");
        case RAW_GEMSTONES: return T("Gemstones");
        default:            return "";
    }
}

/**
 * The same, but stable and never translated -- save files and mods.
 *
 * A separate function from rawName ON PURPOSE. rawName goes through T(), so
 * keying a save with it would write "Öl" for a player running the game in
 * German and then fail to read it back in English. That is the sort of bug that
 * only ever reproduces on somebody else's machine.
 */
inline const char* rawKey(int r) {
    switch (r) {
        case RAW_OIL:       return "oil";
        case RAW_METAL:     return "metal";
        case RAW_RUBBER:    return "rubber";
        case RAW_GEMSTONES: return "gemstones";
        default:            return "";
    }
}

/**
 * What one unit of a good is made of.
 *
 * A SINGLE STEP, deliberately: raw -> good -> consumed. Not raw -> part ->
 * assembly -> good. A deeper chain is a logistics game, and the point of this
 * one is that a player can look at a shortage and say what caused it.
 *
 * `fixed` names materials that CANNOT be substituted -- you do not make fuel
 * without oil. `any` is drawn from whatever the country actually has, most
 * plentiful first.
 *
 * WHY `any` EXISTS, and it is not a convenience. The first version of this made
 * consumer goods out of rubber and gemstones, which read well and was
 * unplayable: gemstones are on 123 of the 1298 provinces of the 1939 map, and
 * TWENTY-SEVEN OF SIXTY-FOUR COUNTRIES HAVE NEITHER RUBBER NOR GEMSTONES AT
 * ALL. Those countries could never feed their people whatever they built or
 * however well they played -- not a hard game, an unwinnable one, and it showed
 * up as 93% of the world's factories standing idle and rebellions tripling.
 *
 * So the good the population eats is the one that is not fussy about inputs:
 * light industry runs on whatever is to hand, and what actually limits it is
 * people and factories, which is what the tutorial has always said. The war
 * goods stay strategically constrained -- no oil, no fuel -- because that is
 * the interesting kind of scarcity, and a country can trade for it.
 */
struct GoodRecipe {
    float fixed[RAW_COUNT];
    float any;
};

inline GoodRecipe goodInputs(int good) {
    GoodRecipe r{{0.0f, 0.0f, 0.0f, 0.0f}, 0.0f};
    switch (good) {
        // Light industry: cheap, and it will take anything.
        case GOOD_CONSUMER:  r.any = 0.35f; break;
        case GOOD_MACHINERY: r.fixed[RAW_METAL] = 1.0f; break;
        case GOOD_FUEL:      r.fixed[RAW_OIL]   = 1.0f; break;
        // Shells are mostly steel with a little of everything else.
        case GOOD_MUNITIONS: r.fixed[RAW_METAL] = 0.6f; r.any = 0.2f; break;
        default: break;
    }
    return r;
}

// ── THE RATES, AND WHAT IS KNOWN ABOUT THEM ──
//
// PROVISIONAL. These set the scale of the whole production economy and they
// have NOT yet been through the bench. They are picked to be roughly
// self-consistent -- a mid-sized country's factories can feed a mid-sized
// country's population, and a maximal deposit is worth having -- and that is
// all that can honestly be claimed for them today.
//
// It is safe to ship them unproven only because the goods economy is behind
// Game::m_goodsEconomy and defaults OFF, so nothing here runs in a normal game
// until it has been measured. Do not turn that flag on by default until these
// have been swept; the note on m_autoSellPct says what the sweep is for.
//
// A worked example of the intended balance, so the next person can tell whether
// a change makes it better or worse. Note it is stated as FRACTIONS, not as
// absolute quantities -- that is the point of the rewrite: the answer must not
// depend on how many people happen to be alive.
//
//   a country industrialised to 100% of its capacity, all factories
//   on consumer goods                              1.6x its own demand
//   the same country at 63% of capacity                  1.0x -- just fed
//   so a country needs roughly two thirds of its capacity built, or
//   fewer factories on other goods, to feed itself
//   a province holding a maximal (100) deposit    8.0x its own
//                                                 population's demand in ore
//
/**
 * What a province industrialised to its OWN CAPACITY produces, as a multiple of
 * its own population's consumer demand.
 *
 * Replaced a flat goods-per-level rate, which could not survive a long game --
 * see the note on Game::provinceGoodOutput. Above 1 so that a country feeds
 * itself somewhat before it is fully built, and the headroom is what pays for
 * machinery, fuel and munitions out of the same factories.
 */
inline constexpr float GOOD_OUTPUT_SCALE  = 1.6f;
/**
 * What a province with a MAXIMAL deposit yields per unit of its own population,
 * in the same denominator as demand and production.
 *
 * Replaced a flat raw-per-deposit-point rate for the reason written on the
 * extraction step: a fixed tonnage set by the map cannot feed a population that
 * grows, so the shortfall simply moved from the factories to the ore.
 *
 * Sized so that ORE IS NOT THE BINDING CONSTRAINT -- the average province holds
 * about 8% of a maximal deposit across the four materials, which at this scale
 * supports roughly 1.9x the consumer demand of the people living on it, against
 * the 1.6x a fully-built province can actually manufacture. Industrialisation
 * is therefore the lever a player pulls, and ore is what makes a particular
 * country's version of that problem easier or harder -- which is the point of
 * having deposits on a map at all.
 */
inline constexpr float EXTRACT_SCALE      = 8.0f;
inline constexpr double CONSUMER_PER_CAPITA = 3000000.0; ///< people per unit of demand

/**
 * What the auto-sale pays per unit of raw material, indexed by RawId.
 *
 * An ADMINISTERED floor, not a market. A real price simulation is a project of
 * its own and would swamp everything else it sits next to; this is the price
 * below which a country can always dispose of what it does not need, which is
 * all m_autoSellPct requires. Gemstones pay most, rubber least.
 */
inline constexpr float RAW_FLOOR_PRICE[RAW_COUNT] = {
    0.8f,  // oil
    0.6f,  // metal
    0.5f,  // rubber
    1.2f   // gemstones
};

/**
 * The most of any one material a country may hold.
 *
 * Not flavour: without a ceiling a country that out-produces its own use
 * accumulates for three thousand turns, the panel figure stops meaning
 * anything, and the running total leaves the range a float represents exactly.
 */
inline constexpr float STOCKPILE_CAP = 1000000.0f;

/**
 * A country's materials, held nationally rather than per province.
 *
 * ONE POOL, NOT A WAREHOUSE PER PROVINCE. 416 of the 1298 provinces on the 1939
 * map hold none of the five deposits at all, so a factory that could only eat
 * what was under it would stand idle nearly everywhere, and the player would be
 * playing a haulage puzzle instead of an economy. A pool is also what makes
 * "trade deals for the resources you lack" mean anything.
 */
struct CountryStockpile {
    float raw[RAW_COUNT]    = {0.0f, 0.0f, 0.0f, 0.0f};
    float goods[GOOD_COUNT] = {0.0f, 0.0f, 0.0f, 0.0f};
};

/**
 * What a country's production did this turn, for the panel and the bench.
 *
 * Recomputed each turn rather than accumulated: every field is a rate, and a
 * rate that is allowed to drift from what actually happened is how an economy
 * panel starts lying to the player.
 */
struct CountryProduction {
    float produced[GOOD_COUNT] = {0.0f, 0.0f, 0.0f, 0.0f};
    float consumed[GOOD_COUNT] = {0.0f, 0.0f, 0.0f, 0.0f};
    float demand[GOOD_COUNT]   = {0.0f, 0.0f, 0.0f, 0.0f};
    float extracted[RAW_COUNT] = {0.0f, 0.0f, 0.0f, 0.0f};
    float rawSold = 0.0f;        ///< money earned by the auto-sale this turn
    float fuelBought = 0.0f;     ///< fuel the army needed and the country had to buy in
    float livingStandards = 1.0f;///< consumer supply / demand, 0..1+
    int   factoriesIdle = 0;     ///< assigned nothing, or starved of inputs
    int   factoriesTotal = 0;
};

// ─── Industry System ────────────────────────────────────────
struct ProvinceIndustry {
    int level = 0;          // 0-10, displayed as Roman numeral
    float income = 0.0f;    // total income per turn
    std::string specialization;  // resource name or empty
    float resourceIncome = 0.0f;
    float popIncome = 0.0f;
    float popModifier = 1.0f;
    int fortification = 0; // 0-5, defence bonus 10% per level
    /**
     * The good this province's factories make; -1 means NOBODY HAS DIRECTED IT.
     *
     * -1 is not "makes nothing". It is the un-directed state, and an undirected
     * factory is allocated by the economy itself -- see Game::autoAssignOutputs.
     * That is the shape the request had: set each factory yourself, "or just
     * letting the ai do it", which is the difference between a planned economy
     * and a market one and eventually the thing the compass will move.
     *
     * It is also the right default for every save written before goods existed:
     * those provinces have no assignment because nobody could have made one, and
     * an economy that allocates them is much closer to what that save meant than
     * a country of idle factories would be.
     */
    int output = -1;
    /**
     * True when a PERSON chose that output -- the player, or the AI acting as a
     * government -- as against the economy allocating it.
     *
     * The distinction is the whole of the planned-versus-market question. An
     * undirected factory is reallocated every turn by whoever runs the economy;
     * a directed one keeps its orders until the government changes them. So
     * this is not a cache flag, it is a record of who is in charge of this
     * particular shed, and how many of them a country may hold is set by where
     * its government sits on the economic compass. See
     * Game::directableFactories().
     */
    bool directed = false;
};

// ─── Army System ────────────────────────────────────────────
/**
 * A body of troops, broken down by kind.
 *
 * An assault used to be an `int`. Once soldiers have types, both sides of a
 * fight need their composition, not their headcount -- and it has to be BOTH
 * sides, because typed defence against untyped attack is not a rule, it is a
 * bug waiting for somebody to notice which half of the fight got the good
 * numbers.
 *
 * A fixed array rather than a vector: there are four types, it is copied into
 * every Battle and passed to every assault, and this way it costs no
 * allocation and compares trivially.
 *
 * EVERY HELPER RETURNS THE OLD ANSWER FOR AN ALL-LINE FORCE, because every
 * column of line infantry is 1.0. That is what keeps this stage provable.
 */
struct ForceComposition {
    long long men[TROOP_TYPE_COUNT] = {};

    long long total() const {
        long long n = 0;
        for (int t = 0; t < (int)TROOP_TYPE_COUNT; ++t) n += men[t];
        return n;
    }
    bool empty() const { return total() <= 0; }
    void add(TroopType t, long long n) {
        if (n > 0 && (int)t >= 0 && (int)t < (int)TROOP_TYPE_COUNT) men[(int)t] += n;
    }

    /**
     * How much frontage this force needs to deploy all of it.
     *
     * The column that changes the game. Measured 2026-09-05, numbers win TWICE
     * -- depth in the comparison and surplus in the attrition -- so troops that
     * use a frontage efficiently are the first thing that competes with simply
     * bringing more men. An all-line force needs exactly its own headcount,
     * which is what the frontage meant before types existed.
     */
    double frontageNeeded() const {
        double f = 0.0;
        for (int t = 0; t < (int)TROOP_TYPE_COUNT; ++t)
            f += (double)men[t] * (double)TROOP_TYPES[t].frontage;
        return f;
    }

    /**
     * The share of this force that fits on `width` of frontage.
     *
     * Proportional across types rather than "best first": a commander does not
     * get to deploy only his mechanised and leave the militia in reserve by
     * accident of the arithmetic, and proportional is the rule that makes a
     * mixed army behave like a mixed army.
     */
    double engagedFraction(long long width) const {
        const double need = frontageNeeded();
        if (need <= 0.0 || width <= 0) return 0.0;
        return std::min(1.0, (double)width / need);
    }

    /** Men, weighted by a column of TROOP_TYPES -- attack or defence. */
    double weighted(float TroopCost::*column) const {
        double v = 0.0;
        for (int t = 0; t < (int)TROOP_TYPE_COUNT; ++t)
            v += (double)men[t] * (double)(TROOP_TYPES[t].*column);
        return v;
    }

    /**
     * Take exactly `n` men away, spread across the kinds in proportion.
     *
     * INTEGER AND TELESCOPING, like the garrison split and the movement shares,
     * and for the same reason: `men[t] x (n / total)` in floating point loses a
     * soldier on a large stack, and one man's difference in who was on the line
     * cascades through a campaign. Running the cumulative total in integers
     * makes the shares exact and their sum exactly `n` -- and for a force of a
     * single kind it removes precisely `n`, bit for bit, which is what keeps a
     * world of nothing but line infantry identical to the one before kinds
     * existed.
     */
    long long removeMen(long long n) {
        const long long tot = total();
        if (n <= 0 || tot <= 0) return 0;
        if (n >= tot) { *this = ForceComposition{}; return tot; }
        long long seen = 0, allocated = 0;
        for (int t = 0; t < (int)TROOP_TYPE_COUNT; ++t) {
            if (men[t] <= 0) continue;
            seen += men[t];
            long long want = n * seen / tot - allocated;
            if (want > men[t]) want = men[t];
            if (want < 0) want = 0;
            men[t]    -= want;
            allocated += want;
        }
        return allocated;
    }
};

struct ArmyUnit {
    int countryId = 0;
    int count = 0;      // number of soldiers
    /**
     * What kind of soldiers. See TROOP_TYPES in BuildCosts.h.
     *
     * A province now holds one entry per (country, TYPE) rather than one per
     * country, so `addTroopsTo` merges on both -- a country with militia and
     * mechanised in the same province has two entries, and merging them by
     * country alone would silently turn one into the other.
     *
     * Defaults to TROOP_LINE, which is exactly neutral in every column, so a
     * world made entirely of it behaves precisely as it did before types
     * existed. That is what makes the change provable rather than hopeful.
     */
    TroopType type = TROOP_LINE;
};

// ─── Navy System ────────────────────────────────────────────
struct PortInfo {
    int level = 0;  // 1-3
};

struct NavyShip {
    int countryId = 0;
    std::string type;
    double lat = 0, lon = 0;
    int health = 30;
    int crew = 0;
};

// ─── Political Compass ──────────────────────────────────────
struct PoliticalCompass {
    float economic = 0.0f;   // -100 (left) to +100 (right)
    float social = 0.0f;     // -100 (authoritarian) to +100 (libertarian)
};

// ─── The compass is a bounded space, and has to be made one ──────────────────
//
// Those ranges above were a comment, not a rule. shiftCountryCompass clamps
// what it adds, but every place a compass ENTERS the game -- the map's
// political_compass.json, a country's own fields, a save file, the average
// taken over a new rebel's provinces -- assigned whatever it was handed. One
// training run logged econ=9988 soc=-7669, and 25,701 identity lines outside
// the documented range.
//
// That is not a cosmetic overflow. PoliticalIdentity works in radii from the
// centre -- committed at 45, radical at 72, released at 32 -- so a country
// sitting at r=12,000 is permanently Radical and the hysteresis that is
// supposed to let it back can never reach it. The same values are read by
// getProvinceRebellionChance as a distance, and fed to the AI's feature vector,
// where an unbounded input is how NaN gets into a net.
//
// Clamped at ingestion rather than at use: there is one bounded truth, and
// every reader should be able to trust it without re-checking.
inline constexpr float COMPASS_MIN = -100.0f;
inline constexpr float COMPASS_MAX =  100.0f;

inline float clampCompassAxis(float v) {
    // NaN first: it compares false against everything, so a plain clamp lets it
    // straight through -- and NaN on the compass propagates into the policy net
    // exactly like the exploded stats that caused the training SIGSEGV.
    if (!(v == v)) return 0.0f;
    return v < COMPASS_MIN ? COMPASS_MIN : (v > COMPASS_MAX ? COMPASS_MAX : v);
}
inline PoliticalCompass makeCompass(float economic, float social) {
    return { clampCompassAxis(economic), clampCompassAxis(social) };
}
inline Vector2 makeCompassVec(float economic, float social) {
    return { clampCompassAxis(economic), clampCompassAxis(social) };
}

// ─── Minorities ─────────────────────────────────────────────
struct MinorityGroup {
    std::string name;
    float pct;
};

// ─── Policy System ──────────────────────────────────────────
struct Policy {
    std::string id;
    std::string name;
    std::string category;  // left, right, authoritarian, libertarian, miscellaneous
    std::string folder;    // display folder: "Left", "Right", "Authoritarian", "Libertarian", "Miscellaneous"
    std::string description;
    int costPerTurn = 0;           // income cost per turn while active
    int implementationTurns = 3;   // turns to implement
    int propagandaDuration = 0;    // 0 = permanent, >0 = lasts N turns then auto-cancels
    // Compass shift per turn while active
    float econShift = 0.0f;
    float socShift = 0.0f;
    // Requirements: min/max compass to be available
    float minEcon = -100, maxEcon = 100;
    float minSoc = -100, maxSoc = 100;
    // Effects
    struct PolicyEffect {
        float minorityGrowthRate = 0.0f;      // % per turn for target minority
        float immigrationBoost = 0.0f;         // general immigration
        float pacificationCost = 0.0f;         // money to reduce unrest
        float unrestReduction = 0.0f;          // direct unrest reduction
        float publicOpinionShift = 0.0f;       // shifts province compass toward/away from gov
        std::string targetMinority;            // empty = all minorities
    } effect;
    bool isUniversal = true;  // if false, only for specific country types
    // Incompatibility
    std::vector<std::string> incompatibleWith;
    /**
     * The doctrine's continuous effects, keyed by the RESEARCH TREE's own
     * effect names -- so a doctrine and a research node are the same kind of
     * thing to Game::getTotalEffect and every caller downstream of it.
     *
     * These were generated into policies.json, printed in the doctrine screen's
     * gains/costs, and read by nothing: the loader did not parse them and
     * getTotalEffect iterated research nodes alone. Every doctrine in the game
     * therefore advertised effects it did not apply -- which is the exact
     * failure tools/gen_policies.py was written to prevent, since it derives
     * the advertised text FROM these numbers so the claim and the effect cannot
     * disagree. They could, because only one end was connected.
     *
     * Sign convention follows the tree: a COST reduction is stored POSITIVE,
     * because buildCostMod() and its siblings subtract.
     */
    std::unordered_map<std::string, float> levers;
    // Tradeoffs for UI display
    struct Tradeoffs {
        std::vector<std::string> gains;
        std::vector<std::string> costs;
    } tradeoffs;
};

struct ActivePolicy {
    std::string policyId;
    int countryId = 0;
    int turnsRemaining = 0;  // >0 = implementing, 0 = active, -1 = completed/removed
    int targetProvince = -1; // -1 = nationwide
    std::string targetMinority;
};

// ─── Ethnic Policy System ───────────────────────────────────
struct EthnicPolicyOption {
    std::string name;
    std::string desc;
    float alignmentPerTurn = 0.0f;
    float popGrowthPerTurn = 0.0f;
    float costPerTurn = 0.0f;
    float compassShiftEcon = 0.0f;
    float compassShiftSoc = 0.0f;
    bool isDefault = false;
};

struct EthnicPolicyCategory {
    std::string id;
    std::string displayName;
    std::vector<EthnicPolicyOption> options;
};

// ─── Menu Background ────────────────────────────────────────
struct BgParticle {
    float tx, ty;   // texture coordinates (0..bgW, 0..bgH)
    float size;
    float maxSize;
    float alpha;    // elapsed time since spawn
    float lifetime;
};

// ─── Economy ────────────────────────────────────────────────
struct CountryIncomeSnapshot {
    float gross = 0;      // income (base industry)
    float resource = 0;   // resourceIncome
    float pop = 0;        // popIncome
    float expenses = 0;   // total expenses (army + navy + policies + minorities + research + pacification)
    float armyExpenses = 0;// army payroll (0.01/10k men)
    float navyExpenses = 0;// navy maintenance (carrier=25, destroyer=10, crew=0.2/10k)
    float policyCosts = 0;// doctrine cost per turn
    float minorityCosts = 0;// ethnic policy cost per turn
    float researchCost = 0;// research allocation cost per turn
    float pacificationCost = 0;// pacification budget cost per turn
    float industryUpkeep = 0;// what the factories cost to run; see industryUpkeep()
    int   industryLevels = 0;// total levels held, which is what sets the above
    float net = 0;        // gross + resource + pop - expenses = net income
    float total = 0;      // gross + resource + pop (pre-expenses)
    /**
     * WHAT THE WHOLE COUNTRY COST TO BUILD -- a stock, not a flow.
     *
     * Everything else on this struct is income or outgoings per turn, which
     * answers "how is it doing" and never answers "how much is there". A
     * country that has spent forty turns turning income into factories, forts
     * and divisions shows the same net income as one that has spent it on
     * nothing, and the whole difference between them is invisible.
     *
     * So this is the price list run backwards over what the country actually
     * holds: every industry level, fort and port at what it cost to raise,
     * every soldier and hull at what it cost to field, plus the treasury. It
     * is deliberately built from the SAME tables the build buttons charge
     * from -- one table, one reader -- so it cannot drift into describing a
     * different game than the one being played.
     */
    float nationalValue = 0;
    long long population = 0;   ///< so the same figure can be shown per head
};

// ─── Research System ────────────────────────────────────────
struct ResearchNode {
    std::string id;
    std::string name;
    std::string desc;
    std::string category;    // "buildings", "army", "population", "misc"
    std::string subcategory; // tree name
    std::vector<std::string> deps;
    int cost = 0;
    float posX = 0, posY = 0;
    bool infinite = false;
    int mutexGroup = 0; // branches in same group are mutually exclusive
    bool depsAny = false; // if true, any one dep suffices (OR logic) instead of all

    // Effects
    int fortLevel = 0;
    int industryLevel = 0;
    int portLevel = 0;
    float armyDefPct = 0;
    float armyAtkPct = 0;
    float conscriptionCostPct = 0;
    float maintenanceCostPct = 0;
    float navyCostPct = 0;
    float navyAtkPct = 0;
    float navyDefPct = 0;
    float navySpeedPct = 0;
    bool unlockShips = false;
    float popModPct = 0;
    float resourceModPct = 0;
    float industryCostPct = 0;
    /**
     * Cuts the RUNNING cost of industry, as a percentage of the upkeep rate.
     *
     * Industry upkeep is the second largest line on most industrial economies'
     * books and, until this existed, the only one with no counterplay at all:
     * no doctrine lever, no research node, nothing in the effects system read
     * it. A tax a player can see and cannot answer reads as a bug even when the
     * arithmetic is right. See industryUpkeep().
     */
    float industryUpkeepPct = 0;
    float passiveIncome = 0;
    float popGrowthPct = 0;
    float migrationRate = 0;
    float indoctrinationPct = 0;
    float conscriptionPct = 0;
    /**
     * The troop kind this node unlocks, if any.
     *
     * Exactly how `artilleryType` works one line below, and deliberately so:
     * ammunition was already typed, researched and priced per type from one
     * table, and troops are the same kind of thing. Empty for every node that
     * unlocks nothing.
     */
    std::string troopType;
    std::string artilleryType;
    float artilleryTroopKillPct = 0;
    float artilleryPopKillPct = 0;
    float artilleryFortDamage = 0;
    int artilleryIndustryDamage = 0;
    float artilleryFortDamageChance = 0;

    bool researched = false;
    bool inProgress = false;
    int invested = 0;

    bool isAvailable(const std::vector<ResearchNode>& nodes) const;
};

// Builds the full research-tree node definitions into `out`. Shared by the game
// (Game::initResearchTrees) and the map editor's research picker.
void buildResearchNodes(std::vector<ResearchNode>& out);

// ─── Pending Actions (queued for processing on next turn) ────
struct PendingDiplomaticAction {
    std::string sourceIso;
    std::string targetIso;
    std::string action; // "request_alliance", "break_alliance", "request_guarantee", "break_guarantee", "request_nap", "break_nap", "declare_war", "call_to_arms"
    int turnsRemaining = 1;
    // "call_to_arms" only: who the ally is being asked to fight. The request
    // itself travels defender -> ally, so the aggressor is a third party and
    // cannot be recovered from source/target.
    std::string subjectIso;
    // "declare_war" only: what the declaring country says it is for. Chosen
    // when the declaration is queued, because that is when the country decides
    // -- a turn later, the state it decided from may have moved.
    int statedGoal = WAR_GOAL_NONE;
};

// ─── A statement somebody may yet be caught out on ──────────
//
// The believability checks used when a thing is SAID only ask whether the map
// already disproves it. That leaves the safe lies safe forever -- "our people
// will not stand another war" cannot be checked against anything, so a country
// could say it every turn and pay nothing.
//
// What catches those is not evidence, it is CONDUCT. Plead exhaustion and then
// declare a war of your own; announce a war to recover what is yours and then
// take provinces you never claimed. Nobody could have known at the time, and
// everybody can see it afterwards.
//
// So a claim of that kind is written down when it is made, watched for as long
// as it is reasonable to hold somebody to it, and dropped. The statement is
// private -- only the country it was said to knows it was said -- which is why
// credibility is kept per pair rather than as one public reputation.
struct SpokenClaim {
    enum Kind {
        CLAIM_WEARY,      // "our people will not stand another war"
        CLAIM_RECONQUEST, // "we fight only to recover what is ours"
    };
    std::string speakerIso;
    std::string hearerIso;
    int kind = CLAIM_WEARY;
    int madeOnTurn = 0;
    std::string aboutIso;   // CLAIM_RECONQUEST: who the war is against
};

struct PendingUpgrade {
    int provinceId = 0;
    std::string type; // "industry", "fortification", "port"
    int targetLevel = 0;
    int turnsRemaining = 0;
};

struct PendingSpecialization {
    int provinceId = 0;
    std::string specialization;
    int turnsRemaining = 3;
};

struct PendingRecruitment {
    int provinceId = 0;
    int count = 0;      ///< SOLDIERS raised, not people spent -- see `type`
    int turnsRemaining = 1;
    /**
     * What kind, which decides how many PEOPLE the soldiers cost.
     *
     * A soldier has always come out of the province's population, one for one.
     * A better soldier costs more of it: the men behind him -- the training
     * base, the crews, the mechanics -- are people too, and a country that
     * fields mechanised divisions is spending its population several times over
     * per rifle at the front. `count` stays the number of SOLDIERS; the draw on
     * population is `count x TROOP_TYPES[type].manpower`.
     *
     * This is the whole of "the better the troop, the less manpower available",
     * and it needed no new resource: the population is already deducted in the
     * resolver and already the thing that runs out.
     */
    TroopType type = TROOP_LINE;
};

struct PendingMoveOrder {
    int fromProvince = 0;
    int toProvince = 0;
    int pct = 50; // percentage of garrison to send
    int countryId = 0; // who issued this order
    /**
     * Which kind to send, or -1 for the whole garrison.
     *
     * Carried ON THE ORDER rather than read from the panel when the turn
     * resolves: a player who selects the militia, gives an order, then selects
     * something else before pressing the turn button has still ordered the
     * militia. The alternative -- a live global filter -- makes an order mean
     * whatever the interface happened to be showing at execution time, which is
     * not a thing anybody can reason about.
     *
     * -1 is the default, so every existing caller moves the whole garrison
     * exactly as it always did.
     */
    int troopType = -1;
};

struct PendingDisbandOrder {
    int provinceId = 0;
    int count = 0; // 0 = all
};

struct PendingShipBuild {
    int provinceId = 0;
    std::string type; // "destroyer" or "carrier"
    int turnsRemaining = 3;
};

struct PendingScrapShip {
    int shipIndex = 0;
};

struct PendingEmbark {
    int provinceId = 0;
    int count = 0;
    int turnsRemaining = 1;
};

/**
 * A FIGHT THAT DID NOT FINISH.
 *
 * An assault used to be settled the moment it was ordered: it carried, or it
 * was thrown back and the men who had not reached the line marched home. So a
 * war was a sequence of instants, there was never a contested province at the
 * end of a turn, and there was nothing for a player to DO inside a war -- only
 * decisions before one.
 *
 * A battle is what an unfinished assault becomes. It stands in the province,
 * fights a round a turn, and can be fed or abandoned. That is the whole
 * feature: reinforce and withdraw are the two verbs the game was missing.
 *
 * THE ATTACKERS LIVE HERE, NOT IN m_provinceArmies. A province holds its
 * owner's garrison and nobody else's -- the eval asserts it ("occupation: 0
 * stacks sharing a province with an owner they are at war with") and a great
 * deal of code walks province armies assuming it. Men committed to a battle are
 * off the map until it ends, which keeps that invariant exactly as it was and
 * means nothing that iterates garrisons has to learn a new case.
 */
struct Battle {
    int provinceId = 0;
    int attackerCid = 0;
    /// Men committed, by kind. Not in the province, not at home: in the fight.
    ForceComposition men;
    /// How many, whatever they are. The count this used to be.
    long long attackers() const { return men.total(); }
    /// Where the survivors go on a withdrawal, and where they came from.
    /// -1 for a landing, which has nowhere to go back to -- the same fact that
    /// makes a failed landing drown.
    int fromProvince = -1;
    /// Rounds fought. Counted so a battle can be seen to be going badly, by a
    /// player reading the panel and by an AI deciding whether to pull out.
    int rounds = 0;
    /**
     * The two sides as the resolver last compared them, and what it cost.
     *
     * Recorded so that "this has been going against us for three rounds and is
     * not improving" can be WRITTEN, by an AI reflex or by the panel, instead
     * of being guessed at from troop counts that do not include the frontage,
     * the fort, supply, depth or either side's research. Those are the numbers
     * that actually decided the round, and reconstructing them outside the
     * resolver would be a second copy of the rule -- which this codebase has
     * been bitten by often enough to know better.
     *
     * Zero before the first round is fought.
     */
    double lastAtkPower = 0.0;
    double lastDefPower = 0.0;
    /// Men lost by each side in the last round, for the panel to show.
    long long lastAtkLosses = 0;
    long long lastDefLosses = 0;
    /**
     * The whole fight so far, not just its last round.
     *
     * ONE ROUND IS NOT A TREND, and reading it as one is a mistake this record
     * invited. A repulse grinds the DEFENDER as well as the attacker -- the
     * province 824 trace is four repulses in a row, each killing about as many
     * defenders as attackers, and the fourth carries -- so "we lost this round
     * and did not win the exchange" describes almost every round of a battle
     * that is being won by persistence. The first withdraw reflex was written
     * on exactly that signal and measured 7 to 38 rating points WORSE than not
     * withdrawing at all, because it pulled out of the fights persistence wins.
     *
     * These are what make the honest question askable: over the whole battle,
     * is the defender weakening faster than we are, and is their power falling?
     * `openingDefPower` is what the defence was worth on the first round, so a
     * grind that is working shows as lastDefPower well below it -- without the
     * caller having to keep a history of its own between turns.
     */
    long long totalAtkLosses = 0;
    long long totalDefLosses = 0;
    double openingDefPower = 0.0;
};

struct PendingArtilleryOrder {
    int fromProvince = 0;
    int targetProvince = 0;
    std::string ammoType;
};

/**
 * A VOYAGE, NOT A STEP.
 *
 * This used to be a destination and nothing else, consumed and thrown away by
 * processNavyMovement in the turn it was given. The resolver walked the
 * straight line to it, stopped at the last water, and dropped the order -- so a
 * crossing that had to round a headland was re-planned down the same blocked
 * chord every turn and never got anywhere. Measured on the shipped scenarios,
 * 80% of all ship movement in the world went nowhere, and every one of those
 * stalls was a destination that was reachable by sea and not in a straight
 * line.
 *
 * `route` is the sea path from Game::navRoute -- water waypoints that go round
 * the coast -- with the destination itself appended as the final leg. The order
 * survives the turn and the resolver spends one turn's range along the polyline
 * each time, so one click sails a fleet to the other side of the world over
 * however many turns that takes.
 *
 * EMPTY WHEN THE ORDER IS MADE, filled by the resolver. Every caller -- the
 * player's click, the AI, a mod, an order off the wire -- writes a destination
 * and nothing else, exactly as before, and planning happens in the one place
 * that has to be right about it.
 */
struct PendingShipMoveOrder {
    int shipIndex = 0;
    double destLon = 0, destLat = 0;
    /// Remaining legs. The last is the destination; the rest came from the
    /// router and are water by construction.
    std::vector<std::pair<double, double>> route;
    /// Set once route has been computed, so an unreachable destination is
    /// planned once and then abandoned rather than re-planned every turn.
    bool planned = false;
    /// The port province this voyage is for, when it is for one. Lets the AI
    /// ask "am I already sailing where I would choose to sail?" and leave a
    /// voyage in progress alone instead of re-issuing it every turn. -1 for a
    /// destination somebody simply picked off the map.
    int destProvince = -1;
};

struct PendingShipEngageOrder {
    int shipIndex = 0;
    int targetIndex = 0;
};

struct PendingShipBombardOrder {
    int shipIndex = 0;
    int targetProvince = 0;
    std::string ammoType;
};

struct PendingShipDisembark {
    int shipIndex = 0;
    int targetProvince = 0;
};
