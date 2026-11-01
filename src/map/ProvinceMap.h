#pragma once
#include <cstddef>
#include <cstdint>
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
    /**
     * The same, straight into the compact form compact() produces, decoding
     * the PNG a row at a time so the RGBA image never exists. For a process
     * that never draws: a dedicated server reads an 8192x4096 map holding
     * 64 MB instead of 256 MB in flight. Falls back to the ordinary load
     * (and then compacts) for a PNG the row decoder will not read.
     */
    bool loadCompactFromMemory(const void* imageData, int imageSize,
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

    /**
     * Reassign a batch of pixels (given as row-major linear indices) to province
     * `id`, in whichever form the raster currently holds -- this is the live
     * morph path, and the running game keeps the raster COMPACT (the editor's
     * updatePixelsRect above only touches the RGBA image, which the game has
     * released). Compact-aware: a single palette slot is resolved for the whole
     * batch, allocating one if this id has not been used on the map yet. Returns
     * false only when a new slot is needed and the 65,535-colour palette is full
     * (the raster is then left unchanged). idAt() answers the new value after.
     */
    bool paintPixels(const uint32_t* idx, size_t n, int id);

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

    /**
     * The province id at pixel `i` (row-major), whichever form the raster is
     * in. Every reader that runs after a world has loaded goes through here,
     * so the raster can be compacted underneath them. 0 is no province.
     */
    int idAt(size_t i) const {
        if (const auto* p = static_cast<const uint8_t*>(m_image.data)) {
            p += i * 4;
            return (p[0] << 16) | (p[1] << 8) | p[2];
        }
        return m_index.empty() ? 0 : m_palette[m_index[i]];
    }
    int idAt(int x, int y) const { return idAt((size_t)y * (size_t)m_width + (size_t)x); }

    /** Whether there is a raster to read at all, in either form. */
    bool hasPixels() const { return m_image.data != nullptr || !m_index.empty(); }

    /**
     * Swap the RGBA image for two bytes per pixel and a table of the ids.
     *
     * A province image is 8192x4096 at four bytes a pixel -- 128 MB, a quarter
     * of what a free container host allows -- but a map uses a few thousand
     * colours at most, so an index into a table of them says exactly the same
     * thing in half the space, and idAt() answers identically from either.
     *
     * For a process that never DRAWS the map: getImage() is empty afterwards,
     * and anything that hands it to the renderer gets nothing. False, and
     * nothing changed, when the map has more than 65,535 distinct colours.
     */
    bool compact();

    /**
     * Which provinces touch, as the FULL-resolution raster had it -- filled only
     * when the map was loaded reduced (see map/RasterScale.h), and empty
     * otherwise. Sampling a raster down drops the thinnest borders, so a pixel
     * scan of the reduced map would lose real neighbours; the loader saw every
     * row at full resolution anyway and wrote down every touching pair (8-way,
     * wrapping east-west), once. Each pair is (smaller id, larger id).
     */
    const std::vector<std::pair<int, int>>& fullResolutionAdjacency() const { return m_fullAdjacency; }

private:
    bool parseJson(const std::string& jsonStr);
    /** Palette slot for `id` in the compact form, allocating one if new. -1 when
     *  the palette is full. Builds the reverse index lazily on first write. */
    int slotForId(int id);
    Image m_image{};
    std::vector<uint16_t> m_index;   ///< after compact(): palette slot per pixel
    std::vector<int>      m_palette; ///< after compact(): slot -> province id
    std::unordered_map<int, uint16_t> m_slotOf; ///< id -> palette slot, lazy; see slotForId
    std::vector<std::pair<int, int>> m_fullAdjacency; ///< see fullResolutionAdjacency
    int m_width = 0;
    int m_height = 0;
    bool m_loaded = false;
    std::unordered_map<int, Province> m_provinces;
};
