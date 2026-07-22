#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "raylib.h"

// Generate provinces, countries, and all game data from a land/sea pixel map.
// All outputs are strings ready to write to disk or embed in .odmap.
struct ProceduralOutput {
    std::vector<Color> provincePixels;  // provinces.png data
    std::string provinceJson;           // provinces.json
    std::vector<Color> politicalPixels; // political.png data
    std::string countryJson;            // countries.json
    std::string populationJson;         // population.json
    std::string resourcesJson;          // resources.json (industry + resources)
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
