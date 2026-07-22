#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <map>
#include <unordered_map>

#include "json.hpp"

#include "stb_image_write.h"
#include "stb_image.h"

#include "miniz.h"
#include "miniz_zip.h"

struct City {
    const char* name;
    double lat;
    double lon;
    long long pop;
};

class Generator {
public:
    struct Config {
        int mapWidth = 8192;
        int mapHeight = 4096;
        std::string dataDir = "data";
        int popYear = 2000;
        std::string landUrl = "https://naciscdn.org/naturalearth/10m/physical/ne_10m_land.zip";
        std::string lakesUrl = "https://naciscdn.org/naturalearth/10m/physical/ne_10m_lakes.zip";
        std::string deFactoUrl = "https://naciscdn.org/naturalearth/10m/cultural/ne_10m_admin_0_countries.zip";
        std::string disputedUrl = "https://naciscdn.org/naturalearth/10m/cultural/ne_10m_admin_0_disputed_areas.zip";
        std::string adminUrl = "https://naciscdn.org/naturalearth/10m/cultural/ne_10m_admin_1_states_provinces.zip";
        std::string populatedPlacesUrl = "https://naciscdn.org/naturalearth/10m/cultural/ne_10m_populated_places.zip";
        int voronoiRelaxIterations = 2;
    };

    Generator(const Config& cfg);
    ~Generator();

    bool run();

private:
    bool downloadFile(const std::string& url, const std::string& destPath);
    bool extractZip(const std::string& zipPath, const std::string& outDir);
    bool findSHP(const std::string& dir, std::string& out);

    bool generateLandSea();
    bool rasterizeUrl(const std::string& url, const std::string& label,
                      std::vector<uint32_t>& pixels,
                      nlohmann::json& json,
                      int& nextId);
    bool saveProvinceFiles(const std::vector<uint32_t>& pixels,
                           const nlohmann::json& json,
                           const std::string& filePrefix,
                           const std::string& label);
    bool createODMArchive(const std::vector<std::string>& files,
                          const std::string& odmPath);
    bool generateProvinces();
    void computeAndSavePopulation(
        const std::vector<uint32_t>& provincePixels,
        const nlohmann::json& provinceJson,
        const std::map<int, std::string>& regionIsoA3);

    // Fetch country populations from World Bank API for a given year
    std::unordered_map<std::string, long long> fetchCountryPopulations(int year);

    // Fetch city database from Natural Earth Populated Places shapefile
    std::vector<City> fetchCityDatabase();

    void lonLatToPixel(double lon, double lat, int& px, int& py) const;

    Config m_cfg;
    std::vector<uint32_t> m_pixels;
};
