#pragma once
#include <string>
#include "raylib.h"

class LandSeaMap {
public:
    LandSeaMap() = default;
    ~LandSeaMap();

    bool load(const std::string& path);
    bool loadFromMemory(const void* data, int size);
    void setFromPixels(Color* pixels, int w, int h);
    void updatePixels(Color* pixels);
    void updatePixelsRect(Color* pixels, int x, int y, int w, int h);

    bool isLand(int pixelX, int pixelY) const;
    bool isLand(float lon, float lat) const;

    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

    const Texture2D& getTexture() const { return m_texture; }

    void pixelToLonLat(int px, int py, float& lon, float& lat) const;
    void lonLatToPixel(float lon, float lat, int& px, int& py) const;

    const Image& getImage() const { return m_image; }

private:
    Image m_image{};
    Texture2D m_texture{};
    int m_width = 0;
    int m_height = 0;
    bool m_loaded = false;
};
