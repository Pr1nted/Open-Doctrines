#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "raylib.h"
// T(): both war-goal tables below are sentences the player reads.
#include "i18n/Locale.h"

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
struct CeasefireTerms {
    int ourMoney = 0;                    // money offered by the sender
    int theirMoney = 0;                  // money demanded from the recipient
    std::vector<int> ourProvs;           // provinces the sender cedes (province IDs)
    std::vector<int> theirProvs;        // provinces the sender demands (province IDs)
    std::vector<int> ourDropClaims;     // claims the sender drops (province IDs)
    std::vector<int> theirDropClaims;   // claims the sender demands the recipient drops (province IDs)
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

// ─── Industry System ────────────────────────────────────────
struct ProvinceIndustry {
    int level = 0;          // 0-10, displayed as Roman numeral
    float income = 0.0f;    // total income per turn
    std::string specialization;  // resource name or empty
    float resourceIncome = 0.0f;
    float popIncome = 0.0f;
    float popModifier = 1.0f;
    int fortification = 0; // 0-5, defence bonus 10% per level
};

// ─── Army System ────────────────────────────────────────────
struct ArmyUnit {
    int countryId = 0;
    int count = 0;      // number of soldiers
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
    int count = 0;
    int turnsRemaining = 1;
};

struct PendingMoveOrder {
    int fromProvince = 0;
    int toProvince = 0;
    int pct = 50; // percentage of garrison to send
    int countryId = 0; // who issued this order
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
