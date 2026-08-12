#pragma once
#include <string>
#include <vector>
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

    /**
     * Free the RGBA pixels, keeping the land mask and the texture.
     *
     * THE WHOLE POINT. This layer answers one question -- isLand(x, y) -- and it
     * was answering it out of an 8192x4096 RGBA image: 128 MB held for the life
     * of the session to store one bit per pixel. The mask below is the same
     * information at 1/32nd the size, so once it and the GPU texture are built
     * the image is dead weight.
     *
     * 128 MB is survivable on a desktop and is not on a phone, where the heap
     * starts at 512 MB and the province layer wants another 128 MB of its own.
     * That is why a scenario would load on a laptop and kill the tab on a
     * handset -- the menu worked, because nothing before the map is big.
     *
     * NOT called by the map editor, which owns a separate LandSeaMap and edits
     * its pixels in place through updatePixels(). Only the game's own loader
     * calls this, after the map is loaded and nothing will write to it again.
     */
    void dropPixels();

    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

    const Texture2D& getTexture() const { return m_texture; }

    void pixelToLonLat(int px, int py, float& lon, float& lat) const;
    void lonLatToPixel(float lon, float lat, int& px, int& py) const;

    const Image& getImage() const { return m_image; }

private:
    /** Rebuild m_landMask from the RGBA pixels. Cheap; call after any write. */
    void rebuildMask();

    Image m_image{};
    Texture2D m_texture{};

    // One bit per pixel: set means land. 4 MB where the image is 128 MB, and it
    // is what isLand() reads, so the image only has to exist while something is
    // painting into it. See dropPixels().
    std::vector<unsigned char> m_landMask;

    int m_width = 0;
    int m_height = 0;
    bool m_loaded = false;
};
