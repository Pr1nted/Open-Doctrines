#include "ProvinceMap.h"
#include "util/LoadLog.h"
#include "util/PngRows.h"
#include "../json.hpp"
#include <fstream>
#include <iostream>
#include <cstring>

ProvinceMap::~ProvinceMap() {
    if (m_loaded && m_image.data) {
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
        LoadLog() << "Could not open " << jsonPath << std::endl;
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
        LoadLog() << "JSON parse error: " << e.what() << std::endl;
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
    return parseJson(jsonStr);
}

bool ProvinceMap::loadCompactFromMemory(const void* imageData, int imageSize,
                                        const std::string& jsonStr) {
    std::vector<uint16_t> index;
    std::vector<int> palette;
    std::unordered_map<int, uint16_t> slotOf;
    int w = 0, h = 0;
    bool tooMany = false;
    std::string why;
    const bool ok = pngForEachRow(static_cast<const uint8_t*>(imageData), (size_t)imageSize,
        [&](int ww, int hh) {
            w = ww; h = hh;
            index.resize((size_t)w * (size_t)h);
            return true;
        },
        [&](int y, const uint8_t* rgba) {
            if (tooMany) return;
            // The same table compact() builds, in the same order of first
            // appearance, so the two forms are indistinguishable.
            int lastId = -1;
            uint16_t lastSlot = 0;
            uint16_t* out = index.data() + (size_t)y * (size_t)w;
            for (int x = 0; x < w; ++x) {
                const uint8_t* p = rgba + (size_t)x * 4;
                const int id = (p[0] << 16) | (p[1] << 8) | p[2];
                if (id != lastId) {
                    auto it = slotOf.find(id);
                    if (it == slotOf.end()) {
                        if (palette.size() >= 0xFFFF) { tooMany = true; return; }
                        it = slotOf.emplace(id, (uint16_t)palette.size()).first;
                        palette.push_back(id);
                    }
                    lastId = id;
                    lastSlot = it->second;
                }
                out[x] = lastSlot;
            }
        }, &why);
    if (!ok || tooMany) {
        LoadLog() << "  provinces.png: row decode unavailable (" << (tooMany ? "too many colours" : why)
                  << "); decoding whole" << std::endl;
        if (!loadFromMemory(imageData, imageSize, jsonStr)) return false;
        compact();
        return true;
    }
    m_index.swap(index);
    m_palette.swap(palette);
    m_image = Image{};
    m_image.width = m_width = w;
    m_image.height = m_height = h;
    return parseJson(jsonStr);
}

bool ProvinceMap::parseJson(const std::string& jsonStr) {
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
        LoadLog() << "JSON parse error: " << e.what() << std::endl;
        return false;
    }

    m_loaded = true;
    return true;
}

const Province* ProvinceMap::getProvince(int pixelX, int pixelY) const {
    if (!m_loaded) return nullptr;
    if (pixelX < 0 || pixelX >= m_width || pixelY < 0 || pixelY >= m_height)
        return nullptr;
    if (!hasPixels()) return nullptr;

    const int id = idAt(pixelX, pixelY);
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

const Province* ProvinceMap::getProvinceById(int id) const {
    auto it = m_provinces.find(id);
    if (it != m_provinces.end()) return &it->second;
    return nullptr;
}

void ProvinceMap::updatePixels(const Color* pixels) {
    if (!m_loaded || m_image.data == nullptr) return;
    // m_image is forced to R8G8B8A8 on load, so a straight copy is safe
    memcpy(m_image.data, pixels, (size_t)m_width * m_height * sizeof(Color));
}

void ProvinceMap::updatePixelsRect(const Color* fullPixels, int x, int y, int w, int h) {
    if (!m_loaded || m_image.data == nullptr) return;
    if (x < 0 || y < 0 || x + w > m_width || y + h > m_height) return;
    auto* dst = static_cast<Color*>(m_image.data);
    for (int row = 0; row < h; ++row)
        memcpy(&dst[(size_t)(y + row) * m_width + x],
               &fullPixels[(size_t)(y + row) * m_width + x],
               (size_t)w * sizeof(Color));
}

bool ProvinceMap::compact() {
    if (!m_loaded || m_image.data == nullptr) return false;
    const size_t total = (size_t)m_width * (size_t)m_height;
    const auto* px = static_cast<const uint8_t*>(m_image.data);
    std::unordered_map<int, uint16_t> slotOf;
    std::vector<int> palette;
    std::vector<uint16_t> index(total);
    int lastId = -1;
    uint16_t lastSlot = 0;
    for (size_t i = 0; i < total; ++i) {
        const uint8_t* p = px + i * 4;
        const int id = (p[0] << 16) | (p[1] << 8) | p[2];
        // Neighbouring pixels are nearly always the same province, so the
        // last answer is checked before the table: a map-sized walk of hash
        // lookups is what would make this slow, not the copy.
        if (id != lastId) {
            auto it = slotOf.find(id);
            if (it == slotOf.end()) {
                if (palette.size() >= 0xFFFF) return false;   // keep the image
                it = slotOf.emplace(id, (uint16_t)palette.size()).first;
                palette.push_back(id);
            }
            lastId = id;
            lastSlot = it->second;
        }
        index[i] = lastSlot;
    }
    m_index.swap(index);
    m_palette.swap(palette);
    UnloadImage(m_image);
    m_image = Image{};
    m_image.width = m_width;    // still answers "how big", for the readers that ask
    m_image.height = m_height;
    return true;
}

void ProvinceMap::clear() {
    if (m_loaded && m_image.data) UnloadImage(m_image);
    std::vector<uint16_t>().swap(m_index);
    std::vector<int>().swap(m_palette);
    m_image = Image{};
    m_width = 0;
    m_height = 0;
    m_loaded = false;
    m_provinces.clear();
}
