#include "LandSeaMap.h"
#include <cstring>
#include <cmath>

LandSeaMap::~LandSeaMap() {
    if (m_loaded) {
        UnloadTexture(m_texture);
        // dropPixels() may already have freed it. UnloadImage on a null data
        // pointer is not safe to assume, so ask.
        if (m_image.data) UnloadImage(m_image);
    }
}

// Land is the red channel over 128 -- the same test isLand() used to make
// against the image, so the mask cannot disagree with the old behaviour.
//
// The recolouring both load paths do (land -> 200, sea -> 40) preserves that
// test, which is why this can run either before or after it.
void LandSeaMap::rebuildMask() {
    m_landMask.assign(((size_t)m_width * m_height + 7) / 8, 0);
    if (!m_image.data) return;
    const auto* pixels = static_cast<const unsigned char*>(m_image.data);
    const size_t n = (size_t)m_width * m_height;
    for (size_t i = 0; i < n; ++i)
        if (pixels[i * 4] > 128) m_landMask[i >> 3] |= (unsigned char)(1u << (i & 7));
}

void LandSeaMap::dropPixels() {
    if (m_landMask.empty()) rebuildMask();   // never drop what nothing replaced
    if (m_image.data) {
        UnloadImage(m_image);
        m_image = Image{};
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

    rebuildMask();
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

    rebuildMask();
    m_texture = LoadTextureFromImage(m_image);
    SetTextureFilter(m_texture, TEXTURE_FILTER_BILINEAR);
    m_loaded = true;
    return true;
}

void LandSeaMap::setFromPixels(Color* pixels, int w, int h) {
    if (m_loaded) {
        UnloadTexture(m_texture);
        if (m_image.data) UnloadImage(m_image);   // dropPixels() may have gone first
        m_loaded = false;
    }
    m_image = GenImageColor(w, h, WHITE);
    ImageFormat(&m_image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    m_width = w; m_height = h;
    memcpy(m_image.data, pixels, (size_t)w * h * 4);
    rebuildMask();
    m_texture = LoadTextureFromImage(m_image);
    SetTextureFilter(m_texture, TEXTURE_FILTER_BILINEAR);
    m_loaded = true;
}

void LandSeaMap::updatePixels(Color* pixels) {
    if (!m_loaded || !m_image.data) return;
    memcpy(m_image.data, pixels, (size_t)m_width * m_height * 4);
    rebuildMask();
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
    // Only the rectangle's bits, not a full rebuild: this runs on every brush
    // stroke, and rescanning 33 million pixels to repaint a few hundred would
    // make the editor crawl.
    if (m_landMask.size() == ((size_t)m_width * m_height + 7) / 8) {
        for (int row = 0; row < h; ++row) {
            for (int col = 0; col < w; ++col) {
                const size_t i = (size_t)(y + row) * m_width + (x + col);
                const bool land = ((const unsigned char*)(pixels + row * w))[col * 4] > 128;
                if (land) m_landMask[i >> 3] |=  (unsigned char)(1u << (i & 7));
                else      m_landMask[i >> 3] &= (unsigned char)~(1u << (i & 7));
            }
        }
    }

    Rectangle rec = {(float)x, (float)y, (float)w, (float)h};
    UpdateTextureRec(m_texture, rec, pixels);
}

bool LandSeaMap::isLand(int pixelX, int pixelY) const {
    if (!m_loaded) return false;
    if (pixelX < 0 || pixelX >= m_width || pixelY < 0 || pixelY >= m_height)
        return false;
    // The mask, not the image: the image may have been freed by dropPixels(),
    // and this is the only thing that ever needed it.
    const size_t i = (size_t)pixelY * m_width + pixelX;
    if (i >> 3 >= m_landMask.size()) return false;
    return (m_landMask[i >> 3] >> (i & 7)) & 1u;
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
