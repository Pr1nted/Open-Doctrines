#pragma once
#include <string>
#include <vector>
#include <map>
#include <unordered_set>
#include <cstdint>
#include "raylib.h"
#include "GameStructs.h"

// Generate provinces, countries, and all game data from a land/sea pixel map.
// All outputs are strings ready to write to disk or embed in .odmap.
struct ProceduralOutput {
    std::vector<Color> provincePixels;  // provinces.png data
    std::string provinceJson;           // provinces.json
    std::vector<Color> politicalPixels; // political.png data
    std::string countryJson;            // countries.json
    std::string populationJson;         // population.json
    std::string resourcesJson;          // resources.json (industry + resources)
    std::string minoritiesJson;         // minorities.json (per-province ethnic groups)
    std::string minorityColorsJson;     // minority_colors.json (group -> RGB)

    // Structured game data (serialized by the editor, not here)
    std::map<int, std::string> isoByCid;              // cid -> generated iso_a3
    std::map<int, int> portLevelByPid;                // pid -> port level 1-3
    std::map<int, std::vector<ArmyUnit>> armiesByPid; // pid -> garrison units
    std::vector<NavyShip> ships;                      // starting navies
    std::map<std::pair<int,int>, CountryRelation> relations; // {min cid, max cid} -> relation
    std::map<int, std::pair<float,float>> provinceCompassByPid; // pid -> {economic, social}, -100..100
};

// landSea: RGBA pixels, alpha=255 = land, alpha=0 = sea
// provinceDensity: 0.2 = very few large provinces, 5.0 = many small provinces
ProceduralOutput generateProcedural(
    const std::vector<Color>& landSea,
    int w, int h,
    int seed,
    int targetCountries,
    float provinceDensity = 1.0f
);

// Derive a unique 3-letter ISO-style code from a country name, avoiding codes
// in `used`. Deterministic. Shared by the generator and MapEditor::ensureIsoCodes.
std::string makeIsoA3(const std::string& name, const std::unordered_set<std::string>& used);
