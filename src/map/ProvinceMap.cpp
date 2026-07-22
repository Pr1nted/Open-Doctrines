#include "ProvinceMap.h"
#include "../json.hpp"
#include <fstream>
#include <iostream>

ProvinceMap::~ProvinceMap() {
    if (m_loaded) {
        UnloadImage(m_image);
    }
}

bool ProvinceMap::load(const std::string& imagePath, const std::string& jsonPath) {
    m_image = LoadImage(imagePath.c_str());
    if (m_image.data == nullptr) return false;

    ImageFormat(&m_image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    m_width = m_image.width;
    m_height = m_image.height;

    std::ifstream f(jsonPath);
    if (!f.is_open()) {
        std::cerr << "Could not open " << jsonPath << std::endl;
        return false;
    }

    try {
        nlohmann::json j;
        f >> j;

        for (auto& [key, val] : j.items()) {
            Province p;
            p.id = val["id"];
            p.countryId = val.value("country_id", 0);
            p.name = val["name"];
            p.isoA3 = val["iso_a3"];

            std::string colorStr = val["color"];
            unsigned int hex;
            sscanf(colorStr.c_str(), "#%06x", &hex);
            p.r = (hex >> 16) & 0xFF;
            p.g = (hex >> 8) & 0xFF;
            p.b = hex & 0xFF;

            m_provinces[p.id] = p;
        }
    } catch (std::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
        return false;
    }

    m_loaded = true;
    return true;
}

bool ProvinceMap::loadFromMemory(const void* imageData, int imageSize,
                                  const std::string& jsonStr) {
    m_image = LoadImageFromMemory(".png", static_cast<const unsigned char*>(imageData), imageSize);
    if (m_image.data == nullptr) return false;

    ImageFormat(&m_image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    m_width = m_image.width;
    m_height = m_image.height;

    try {
        nlohmann::json j = nlohmann::json::parse(jsonStr);
        for (auto& [key, val] : j.items()) {
            Province p;
            p.id = val["id"];
            p.countryId = val.value("country_id", 0);
            p.name = val["name"];
            p.isoA3 = val["iso_a3"];

            std::string colorStr = val["color"];
            unsigned int hex;
            sscanf(colorStr.c_str(), "#%06x", &hex);
            p.r = (hex >> 16) & 0xFF;
            p.g = (hex >> 8) & 0xFF;
            p.b = hex & 0xFF;

            m_provinces[p.id] = p;
        }
    } catch (std::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
        return false;
    }

    m_loaded = true;
    return true;
}

const Province* ProvinceMap::getProvince(int pixelX, int pixelY) const {
    if (!m_loaded) return nullptr;
    if (pixelX < 0 || pixelX >= m_width || pixelY < 0 || pixelY >= m_height)
        return nullptr;

    const auto* pixels = static_cast<const unsigned char*>(m_image.data);
    int idx = (pixelY * m_width + pixelX) * 4;
    uint8_t r = pixels[idx], g = pixels[idx + 1], b = pixels[idx + 2];

    int id = Province::colorToId(r, g, b);
    auto it = m_provinces.find(id);
    if (it != m_provinces.end()) return &it->second;
    return nullptr;
}

const Province* ProvinceMap::getProvince(float lon, float lat) const {
    int px = static_cast<int>((lon + 180.0f) / 360.0f * m_width);
    int py = static_cast<int>((90.0f - lat) / 180.0f * m_height);
    return getProvince(px, py);
}

Province* ProvinceMap::getProvinceById(int id) {
    auto it = m_provinces.find(id);
    if (it != m_provinces.end()) return &it->second;
    return nullptr;
}
