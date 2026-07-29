#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "raylib.h"

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
};

// ─── Relations System ───────────────────────────────────────
struct CountryRelation {
    bool war = false;
    bool alliance = false;
    bool nonAggression = false;
    bool guarantee = false;
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

// Canonical list of selectable country doctrines (index 0 = "None"/empty).
const std::vector<std::string>& doctrineList();

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

struct PendingShipMoveOrder {
    int shipIndex = 0;
    double destLon = 0, destLat = 0;
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
