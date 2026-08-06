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
    /** The same lookup for callers that only read. Costs a const-qualified
     *  overload rather than making every reader drop its own constness. */
    const Province* getProvinceById(int id) const;

    // Overwrite the CPU-side province image with new RGBA pixels (same size).
    // Used by the map editor's shape-painting brush; skips the PNG round-trip.
    void updatePixels(const Color* pixels);

    // Copy just a rect of a full-map pixel buffer into the CPU image.
    void updatePixelsRect(const Color* fullPixels, int x, int y, int w, int h);

    // Release the image and forget all provinces (before re-loading).
    void clear();

    // Forget a single province definition (its pixels must be reassigned by the caller).
    void removeProvince(int id) { m_provinces.erase(id); }

    // Register a new province definition (its pixels are painted by the caller).
    void addProvince(const Province& p) { m_provinces[p.id] = p; }

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
