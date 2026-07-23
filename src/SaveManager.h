#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

// .odsv = OpenDoctrines SaVe archive (ZIP)
// Contains: original map.odmap + per-turn binary delta files + metadata + state.json
//
// Format:
//   map.odmap              — embedded original map
//   metadata.json          — save metadata
//   state.json             — full game state snapshot (pending orders, claims, research, etc.)
//   turns/t_00001.dat      — binary delta for turn 1
//   turns/t_00002.dat      — binary delta for turn 2
//   ...
//
// Binary delta (.dat) packed format (little-endian):
//   Header (8 bytes):
//     uint16 turn_number
//     uint16 province_entry_count
//     uint16 ship_entry_count
//     uint16 army_entry_count
//   Province entries (variable):
//     uint16 province_id
//     uint16 changed_bitmask
//     [packed field data per set bit:
//       OWNER(0):        uint16 country_id
//       POPULATION(1):   int32  population
//       INDUSTRY_LVL(2): uint8  level
//       FORTIFICATION(3):uint8  fort
//       INCOME(4):       float  income
//       RESOURCE_INC(5): float  resourceIncome
//       POP_INCOME(6):   float  popIncome
//       POP_MODIFIER(7): float  popModifier
//     ]
//   Ship entries:
//     uint16 ship_index
//     uint8  changed_bitmask
//     [LAT(0):float  LON(1):float  HEALTH(2):uint8  CREW(3):uint16  COUNTRY_ID(4):uint16]
//   Army entries:
//     uint16 province_id
//     uint8  unit_count
//     each unit: uint16 country_id + uint32 count
//   Actions:
//     uint16 action_count
//     each: uint8 type + [variable]

struct ProvinceDelta {
    int provinceId = 0;
    bool ownerChanged = false;       int newOwner = 0;
    bool populationChanged = false;  long long newPopulation = 0;
    bool industryLevelChanged = false; int newIndustryLevel = 0;
    bool fortificationChanged = false; int newFortification = 0;
    bool incomeChanged = false;      float newIncome = 0.0f;
    bool resourceIncomeChanged = false; float newResourceIncome = 0.0f;
    bool popIncomeChanged = false;   float newPopIncome = 0.0f;
    bool popModifierChanged = false; float newPopModifier = 1.0f;
};

struct ShipDelta {
    int shipIndex = 0;
    bool latChanged = false;   double newLat = 0.0;
    bool lonChanged = false;   double newLon = 0.0;
    bool healthChanged = false; int newHealth = 100;
    bool crewChanged = false;  int newCrew = 0;
    bool countryIdChanged = false; int newCountryId = 0;
};

struct ArmyDelta {
    int provinceId = 0;
    struct Unit { int countryId = 0; int count = 0; };
    std::vector<Unit> units;
};

struct TurnDelta {
    int turnNumber = 0;
    std::vector<ProvinceDelta> provinces;
    std::vector<ShipDelta> ships;
    std::vector<ArmyDelta> armies;
    // Research/pacification state (recorded each turn for replay)
    float researchAllocation = 0.25f;
    float pacificationAllocation = 0.0f;
    int researchActiveNode = -1;
    int researchPoints = 0;
};

struct PoliticalCompassSave {
    float economic = 0.0f;
    float social = 0.0f;
};

struct SaveMetadata {
    std::string saveName;
    std::string version;
    std::string created;
    std::string lastPlayed;
    int turnCount = 0;
    int provinceCount = 0;
    int shipCount = 0;
    int playerCountryId = 0;
    std::unordered_map<int, PoliticalCompassSave> countryCompasses;
    std::unordered_map<int, double> countryTreasuries;
};

class SaveManager {
public:
    // Create a .odsv from a .odmap (writes initial archive)
    static bool createSave(const std::string& odsvPath,
                           const std::string& odmData,
                           const SaveMetadata& meta);
    // Append a turn delta to an existing .odsv.
    // Optionally writes state.json and extra entries (e.g. rebellion/{cid}.svg)
    // in the SAME archive rewrite. Every save operation has to rebuild the
    // whole zip, so folding the state write in here halves the per-turn cost
    // versus calling appendTurn() then writeState().
    static bool appendTurn(const std::string& odsvPath,
                           const TurnDelta& delta,
                           const std::string* stateJson = nullptr,
                           const std::vector<std::pair<std::string, std::string>>* extraFiles = nullptr);
    // Read metadata from a .odsv
    static SaveMetadata readMetadata(const std::string& odsvPath);
    // Read a specific turn's delta
    static TurnDelta readTurn(const std::string& odsvPath, int turn);
    // Update only the last_played timestamp (also accepts updated meta)
    static bool updateLastPlayed(const std::string& odsvPath, const SaveMetadata* updatedMeta = nullptr);
    // Update the player country ID in metadata
    static bool updatePlayerCountry(const std::string& odsvPath, int playerCountryId);
    // Extract the embedded .odmap to memory
    static std::vector<uint8_t> extractODM(const std::string& odsvPath);
    // Write/read state.json (full game state snapshot beyond turn deltas)
    // extraFiles: additional entries to write (e.g. rebellions/{cid}.svg)
    static bool writeState(const std::string& odsvPath, const std::string& stateJson,
                           const std::vector<std::pair<std::string, std::string>>& extraFiles = {});
    static std::string readState(const std::string& odsvPath);
    // Estimate size of a delta in bytes
    static size_t estimateDeltaSize(const TurnDelta& delta);
    // Estimate total save growth over N turns
    static std::string estimateGrowth(const TurnDelta& avgDelta, int turns);

private:
    // Binary pack/unpack helpers
    static std::vector<uint8_t> packTurn(const TurnDelta& delta);
    static bool unpackTurn(const uint8_t* data, size_t size, TurnDelta& out);
    static void writeU16(std::vector<uint8_t>& buf, uint16_t v);
    static void writeU32(std::vector<uint8_t>& buf, uint32_t v);
    static void writeFloat(std::vector<uint8_t>& buf, float v);
};
