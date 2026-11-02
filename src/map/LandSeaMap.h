#pragma once
#include <cstddef>   // size_t
#include <cstdint>   // uint32_t, used by setLand below
#include <string>
#include <vector>
#include "raylib.h"

class LandSeaMap {
public:
    LandSeaMap() = default;
    ~LandSeaMap();

    bool load(const std::string& path);
    /// withTexture false: the mask only, no GPU copy. The game draws its
    /// political layer over every pixel of this one, opaque, so the 128 MB
    /// texture it used to upload was never seen. The map editor, which shows
    /// the coastline, loads its own copy with the texture.
    bool loadFromMemory(const void* data, int size, bool withTexture = true);
    void setFromPixels(Color* pixels, int w, int h);
    void updatePixels(Color* pixels);
    void updatePixelsRect(Color* pixels, int x, int y, int w, int h);

    /**
     * Flip a batch of pixels (row-major linear indices) to land or sea, writing
     * the 1-bit mask directly. This is the live terrain path: the running game
     * has called dropPixels(), so the RGBA image updatePixelsRect needs is gone.
     * Keeps the image in sync too when it is still present (the editor).
     */
    void setLand(const uint32_t* idx, size_t n, bool land);

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

    /**
     * Read only the land mask, a row at a time: what dropPixels() leaves,
     * without the 128 MB image ever existing. For a process that never draws.
     * Falls back to loadFromMemory + dropPixels for a PNG the row decoder
     * will not read.
     */
    bool loadMaskFromMemory(const void* data, int size);

    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

    const Texture2D& getTexture() const { return m_texture; }

    void pixelToLonLat(int px, int py, float& lon, float& lat) const;
    void lonLatToPixel(float lon, float lat, int& px, int& py) const;

    /**
     * The mask at the resolution the map was DRAWN at. The same as the above
     * on a full world; on one loaded reduced (map/RasterScale.h) the loader
     * kept the full-resolution bits as well -- 4 MB at 8192x4096 -- for the
     * one reader that sampling would break: sea routing, whose straits can be
     * a few pixels wide (Game::buildNavGrid). fullDivisor() is how many full
     * pixels one loaded pixel spans.
     */
    int fullWidth() const { return m_fullMask.empty() ? m_width : m_fullWidth; }
    int fullHeight() const { return m_fullMask.empty() ? m_height : m_fullHeight; }
    int fullDivisor() const { return (m_fullMask.empty() || m_width <= 0) ? 1 : m_fullWidth / m_width; }
    bool isLandFull(int pixelX, int pixelY) const;
    void lonLatToFullPixel(float lon, float lat, int& px, int& py) const;

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
    std::vector<unsigned char> m_fullMask;   ///< see fullWidth(); empty on a full world
    int m_fullWidth = 0;
    int m_fullHeight = 0;

    int m_width = 0;
    int m_height = 0;
    bool m_loaded = false;
};
