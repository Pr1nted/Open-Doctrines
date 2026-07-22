#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "Province.h"
#include "raylib.h"

class ProvinceMap {
public:
    ProvinceMap() = default;
    ~ProvinceMap();

    bool load(const std::string& imagePath, const std::string& jsonPath);
    bool loadFromMemory(const void* imageData, int imageSize,
                        const std::string& jsonStr);

    const Province* getProvince(int pixelX, int pixelY) const;
    const Province* getProvince(float lon, float lat) const;
    Province* getProvinceById(int id);

    const std::unordered_map<int, Province>& getAllProvinces() const { return m_provinces; }
    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

    const Image& getImage() const { return m_image; }

private:
    Image m_image{};
    int m_width = 0;
    int m_height = 0;
    bool m_loaded = false;
    std::unordered_map<int, Province> m_provinces;
};
