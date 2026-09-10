#include "SkyFile.h"

#include "../json.hpp"

#include <fstream>

namespace {

// Read one field if the document has it AND it is the right type. A map written
// by hand with "moonSize": "big" should keep the default rather than take a
// zero -- a silently-zeroed moon is harder to diagnose than an ignored line.
template <typename T>
void take(const nlohmann::json& j, const char* key, T& out) {
    if (!j.contains(key)) return;
    const auto& v = j.at(key);
    if constexpr (std::is_same_v<T, bool>) { if (v.is_boolean()) out = v.get<bool>(); }
    else if constexpr (std::is_same_v<T, int>) { if (v.is_number_integer()) out = v.get<int>(); }
    else { if (v.is_number()) out = v.get<float>(); }
}

void takeColour(const nlohmann::json& j, const char* key, Color& out) {
    if (!j.contains(key)) return;
    const auto& v = j.at(key);
    // "#rrggbb" or [r,g,b]. Two spellings because the editor writes one and a
    // person editing the file by hand reaches for the other.
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (s.size() == 7 && s[0] == '#') {
            auto hex = [&](int i) { return (unsigned char)strtol(s.substr(i, 2).c_str(), nullptr, 16); };
            out = Color{hex(1), hex(3), hex(5), 255};
        }
    } else if (v.is_array() && v.size() >= 3) {
        out = Color{(unsigned char)v[0].get<int>(), (unsigned char)v[1].get<int>(),
                    (unsigned char)v[2].get<int>(), 255};
    }
}

std::string hexOf(Color c) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

}  // namespace

bool skyfile::load(const std::string& path, GlobeView::Sky& out) {
    std::ifstream f(path);
    if (!f) return false;
    nlohmann::json j;
    try { f >> j; } catch (...) { return false; }
    if (!j.is_object()) return false;

    take(j, "stars", out.stars);
    take(j, "star_count", out.starCount);
    take(j, "star_brightness", out.starBrightness);

    take(j, "moon", out.moon);
    takeColour(j, "moon_colour", out.moonColour);
    take(j, "moon_size", out.moonSize);
    take(j, "moon_distance", out.moonDistance);
    take(j, "moon_longitude", out.moonLon);
    take(j, "moon_latitude", out.moonLat);

    take(j, "cloud", out.cloud);
    take(j, "cloud_opacity", out.cloudOpacity);
    take(j, "cloud_drift", out.cloudDrift);
    take(j, "cloud_height", out.cloudHeight);

    take(j, "atmosphere", out.atmosphere);
    takeColour(j, "air_colour", out.airColour);
    take(j, "air_height", out.airHeight);
    take(j, "air_strength", out.airStrength);
    take(j, "air_falloff", out.airFalloff);

    take(j, "sun_disc", out.sunDisc);
    take(j, "sun_size", out.sunSize);
    take(j, "sun_distance", out.sunDistance);
    return true;
}

bool skyfile::save(const std::string& path, const GlobeView::Sky& s) {
    nlohmann::json j;
    j["stars"] = s.stars;
    j["star_count"] = s.starCount;
    j["star_brightness"] = s.starBrightness;

    j["moon"] = s.moon;
    j["moon_colour"] = hexOf(s.moonColour);
    j["moon_size"] = s.moonSize;
    j["moon_distance"] = s.moonDistance;
    j["moon_longitude"] = s.moonLon;
    j["moon_latitude"] = s.moonLat;

    j["cloud"] = s.cloud;
    j["cloud_opacity"] = s.cloudOpacity;
    j["cloud_drift"] = s.cloudDrift;
    j["cloud_height"] = s.cloudHeight;

    j["atmosphere"] = s.atmosphere;
    j["air_colour"] = hexOf(s.airColour);
    j["air_height"] = s.airHeight;
    j["air_strength"] = s.airStrength;
    j["air_falloff"] = s.airFalloff;

    j["sun_disc"] = s.sunDisc;
    j["sun_size"] = s.sunSize;
    j["sun_distance"] = s.sunDistance;

    std::ofstream f(path);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return f.good();
}
