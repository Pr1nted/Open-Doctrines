#include "LandSeaMap.h"
#include <cstring>
#include <cmath>

LandSeaMap::~LandSeaMap() {
    if (m_loaded) {
        UnloadTexture(m_texture);
        UnloadImage(m_image);
    }
}

bool LandSeaMap::load(const std::string& path) {
    m_image = LoadImage(path.c_str());
    if (m_image.data == nullptr) return false;

    ImageFormat(&m_image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    m_width = m_image.width;
    m_height = m_image.height;

    auto* pixels = static_cast<unsigned char*>(m_image.data);
    for (int i = 0; i < m_width * m_height; ++i) {
        int idx = i * 4;
        if (pixels[idx] > 128) {
            pixels[idx]     = 200;
            pixels[idx + 1] = 190;
            pixels[idx + 2] = 160;
            pixels[idx + 3] = 255;
        } else {
            pixels[idx]     =  40;
            pixels[idx + 1] =  80;
            pixels[idx + 2] = 160;
            pixels[idx + 3] = 255;
        }
    }

    m_texture = LoadTextureFromImage(m_image);
    SetTextureFilter(m_texture, TEXTURE_FILTER_BILINEAR);
    m_loaded = true;
    return true;
}

bool LandSeaMap::loadFromMemory(const void* data, int size) {
    m_image = LoadImageFromMemory(".png", static_cast<const unsigned char*>(data), size);
    if (m_image.data == nullptr) return false;

    ImageFormat(&m_image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    m_width = m_image.width;
    m_height = m_image.height;

    auto* pixels = static_cast<unsigned char*>(m_image.data);
    for (int i = 0; i < m_width * m_height; ++i) {
        int idx = i * 4;
        if (pixels[idx] > 128) {
            pixels[idx]     = 200;
            pixels[idx + 1] = 190;
            pixels[idx + 2] = 160;
            pixels[idx + 3] = 255;
        } else {
            pixels[idx]     =  40;
            pixels[idx + 1] =  80;
            pixels[idx + 2] = 160;
            pixels[idx + 3] = 255;
        }
    }

    m_texture = LoadTextureFromImage(m_image);
    SetTextureFilter(m_texture, TEXTURE_FILTER_BILINEAR);
    m_loaded = true;
    return true;
}

void LandSeaMap::setFromPixels(Color* pixels, int w, int h) {
    if (m_loaded) { UnloadTexture(m_texture); UnloadImage(m_image); m_loaded = false; }
    m_image = GenImageColor(w, h, WHITE);
    ImageFormat(&m_image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    m_width = w; m_height = h;
    memcpy(m_image.data, pixels, (size_t)w * h * 4);
    m_texture = LoadTextureFromImage(m_image);
    SetTextureFilter(m_texture, TEXTURE_FILTER_BILINEAR);
    m_loaded = true;
}

void LandSeaMap::updatePixels(Color* pixels) {
    if (!m_loaded || !m_image.data) return;
    memcpy(m_image.data, pixels, (size_t)m_width * m_height * 4);
    UpdateTexture(m_texture, pixels);
}

void LandSeaMap::updatePixelsRect(Color* pixels, int x, int y, int w, int h) {
    if (!m_loaded || !m_image.data) return;
    if (x < 0 || y < 0 || x + w > m_width || y + h > m_height) return;
    // Copy each scanline into the full image
    for (int row = 0; row < h; row++) {
        memcpy((unsigned char*)m_image.data + ((y + row) * m_width + x) * 4,
               (unsigned char*)(pixels + row * w), (size_t)w * 4);
    }
    Rectangle rec = {(float)x, (float)y, (float)w, (float)h};
    UpdateTextureRec(m_texture, rec, pixels);
}

bool LandSeaMap::isLand(int pixelX, int pixelY) const {
    if (!m_loaded) return false;
    if (pixelX < 0 || pixelX >= m_width || pixelY < 0 || pixelY >= m_height)
        return false;
    const auto* pixels = static_cast<const unsigned char*>(m_image.data);
    int idx = (pixelY * m_width + pixelX) * 4;
    return pixels[idx] > 128;
}

bool LandSeaMap::isLand(float lon, float lat) const {
    int px, py;
    lonLatToPixel(lon, lat, px, py);
    return isLand(px, py);
}

void LandSeaMap::pixelToLonLat(int px, int py, float& lon, float& lat) const {
    lon = (static_cast<float>(px) / m_width) * 360.0f - 180.0f;
    lat = 90.0f - (static_cast<float>(py) / m_height) * 180.0f;
}

void LandSeaMap::lonLatToPixel(float lon, float lat, int& px, int& py) const {
    px = static_cast<int>(std::round((lon + 180.0f) / 360.0f * m_width));
    py = static_cast<int>(std::round((90.0f - lat) / 180.0f * m_height));
}
