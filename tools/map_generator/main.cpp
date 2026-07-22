#include "Generator.h"
#include <iostream>

int main(int argc, char* argv[]) {
    Generator::Config cfg;
    cfg.mapWidth = 8192;
    cfg.mapHeight = 4096;
    cfg.dataDir = "data";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--width" && i + 1 < argc) {
            cfg.mapWidth = std::stoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            cfg.mapHeight = std::stoi(argv[++i]);
        } else if (arg == "--data-dir" && i + 1 < argc) {
            cfg.dataDir = argv[++i];
        } else if (arg == "--year" && i + 1 < argc) {
            cfg.popYear = std::stoi(argv[++i]);
        } else if (arg == "--scale" && i + 1 < argc) {
            std::string scale = argv[++i];
            if (scale == "110m") {
                cfg.landUrl = "https://naciscdn.org/naturalearth/110m/physical/ne_110m_land.zip";
                cfg.lakesUrl = "https://naciscdn.org/naturalearth/110m/physical/ne_110m_lakes.zip";
                cfg.deFactoUrl = "https://naciscdn.org/naturalearth/110m/cultural/ne_110m_admin_0_countries.zip";
                cfg.disputedUrl = "https://naciscdn.org/naturalearth/110m/cultural/ne_110m_admin_0_disputed_areas.zip";
                cfg.populatedPlacesUrl = "https://naciscdn.org/naturalearth/110m/cultural/ne_110m_populated_places.zip";
            } else if (scale == "50m") {
                cfg.landUrl = "https://naciscdn.org/naturalearth/50m/physical/ne_50m_land.zip";
                cfg.lakesUrl = "https://naciscdn.org/naturalearth/50m/physical/ne_50m_lakes.zip";
                cfg.deFactoUrl = "https://naciscdn.org/naturalearth/50m/cultural/ne_50m_admin_0_countries.zip";
                cfg.disputedUrl = "https://naciscdn.org/naturalearth/50m/cultural/ne_50m_admin_0_disputed_areas.zip";
                cfg.populatedPlacesUrl = "https://naciscdn.org/naturalearth/50m/cultural/ne_50m_populated_places.zip";
            } else if (scale == "10m") {
                cfg.landUrl = "https://naciscdn.org/naturalearth/10m/physical/ne_10m_land.zip";
                cfg.lakesUrl = "https://naciscdn.org/naturalearth/10m/physical/ne_10m_lakes.zip";
                cfg.deFactoUrl = "https://naciscdn.org/naturalearth/10m/cultural/ne_10m_admin_0_countries.zip";
                cfg.disputedUrl = "https://naciscdn.org/naturalearth/10m/cultural/ne_10m_admin_0_disputed_areas.zip";
                cfg.populatedPlacesUrl = "https://naciscdn.org/naturalearth/10m/cultural/ne_10m_populated_places.zip";
            } else {
                std::cerr << "Unknown scale: " << scale << " (use 110m, 50m, or 10m)\n";
                return 1;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: MapGenerator [options]\n"
                      << "  --width W         Map width in pixels (default: 8192)\n"
                      << "  --height H        Map height in pixels (default: 4096)\n"
                      << "  --data-dir DIR    Output directory (default: data)\n"
                      << "  --year Y          Population year for World Bank data (default: 2000)\n"
                      << "  --scale SCALE     Natural Earth scale: 110m, 50m, 10m (default: 10m)\n";
            return 0;
        }
    }

    std::cout << "OpenDoctrines Map Generator\n";
    std::cout << "  Population year: " << cfg.popYear << "\n";

    Generator gen(cfg);
    if (!gen.run()) {
        std::cerr << "Map generation failed!" << std::endl;
        return 1;
    }

    std::cout << "\nDone! Generated in: " << cfg.dataDir << "/" << std::endl;
    return 0;
}