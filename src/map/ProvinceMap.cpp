#include "ProvinceMap.h"
#include "RasterScale.h"
#include "util/LoadLog.h"
#include "util/PngRows.h"
#include "../json.hpp"
#include <fstream>
#include <iostream>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <unordered_set>

void ProvinceMap::bumpRaster() {
    static std::atomic<uint64_t> s_next{0};
    m_rasterGen = ++s_next;
}

ProvinceMap::~ProvinceMap() {
    if (m_loaded && m_image.data) {
        UnloadImage(m_image);
    }
}

bool ProvinceMap::load(const std::string& imagePath, const std::string& jsonPath) {
    bumpRaster();
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
            // Initialised, and the parse is checked: a malformed or missing
            // "#rrggbb" in a hand-edited or hostile .odmap left hex holding
            // whatever was on the stack and gave the province a garbage colour.
            unsigned int hex = 0;
            if (sscanf(colorStr.c_str(), "#%06x", &hex) != 1) hex = 0;
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
    bumpRaster();
    m_image = LoadImageFromMemory(".png", static_cast<const unsigned char*>(imageData), imageSize);
    if (m_image.data == nullptr) return false;

    ImageFormat(&m_image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    m_width = m_image.width;
    m_height = m_image.height;
    return parseJson(jsonStr);
}

bool ProvinceMap::loadFromPixels(const Color* pixels, int w, int h,
                                 const std::string& jsonStr) {
    if (m_loaded && m_image.data) UnloadImage(m_image);
    m_image = Image{};
    m_image.width = w;
    m_image.height = h;
    m_image.mipmaps = 1;
    m_image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    m_image.data = MemAlloc((unsigned int)((size_t)w * (size_t)h * 4));
    if (m_image.data == nullptr) return false;
    unsigned char* d = static_cast<unsigned char*>(m_image.data);
    const size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; ++i) {
        d[i * 4 + 0] = pixels[i].r;
        d[i * 4 + 1] = pixels[i].g;
        d[i * 4 + 2] = pixels[i].b;
        d[i * 4 + 3] = 255;   // province id is RGB only; keep alpha opaque
    }
    m_width = w;
    m_height = h;
    return parseJson(jsonStr);
}

bool ProvinceMap::loadCompactFromMemory(const void* imageData, int imageSize,
                                        const std::string& jsonStr) {
    bumpRaster();
    std::vector<uint16_t> index;
    std::vector<int> palette;
    std::unordered_map<int, uint16_t> slotOf;
    int w = 0, h = 0;           // the index's size: the image's, over the divisor
    bool tooMany = false;
    std::string why;
    // A reduced world (see RasterScale.h): every div-th pixel of every div-th
    // row is kept. Each colour's first full-resolution pixel is remembered, and
    // whether sampling ever kept one, so none is lost below.
    const int div = odmap::rasterDivisor();
    std::vector<uint32_t> firstAt;      // per slot: (y/div)*w + x/div of its first pixel
    std::vector<uint8_t> kept;          // per slot: sampling put it in the index
    // Full-resolution neighbours (see fullResolutionAdjacency): each row's
    // slots against the row above, as slot pairs.
    std::vector<uint16_t> prevRow, curRow;
    std::unordered_set<uint32_t> slotPairs;
    auto touch = [&](uint16_t a, uint16_t b) {
        if (a == b) return;
        if (a > b) std::swap(a, b);
        slotPairs.insert(((uint32_t)a << 16) | b);
    };
    const bool ok = pngForEachRow(static_cast<const uint8_t*>(imageData), (size_t)imageSize,
        [&](int ww, int hh) {
            w = ww / div; h = hh / div;
            if (w <= 0 || h <= 0) return false;
            index.resize((size_t)w * (size_t)h);
            if (div > 1) { prevRow.assign((size_t)w * div, 0); curRow.assign((size_t)w * div, 0); }
            return true;
        },
        [&](int y, const uint8_t* rgba) {
            if (tooMany) return;
            const bool sampledRow = (y % div) == 0 && y / div < h;
            const int fullW = w * div;
            // The same table compact() builds, in the same order of first
            // appearance, so the two forms are indistinguishable.
            int lastId = -1;
            uint16_t lastSlot = 0;
            uint16_t* out = sampledRow ? index.data() + (size_t)(y / div) * (size_t)w : nullptr;
            for (int x = 0; x < fullW; ++x) {
                const uint8_t* p = rgba + (size_t)x * 4;
                const int id = (p[0] << 16) | (p[1] << 8) | p[2];
                if (id != lastId) {
                    auto it = slotOf.find(id);
                    if (it == slotOf.end()) {
                        if (palette.size() >= 0xFFFF) { tooMany = true; return; }
                        it = slotOf.emplace(id, (uint16_t)palette.size()).first;
                        palette.push_back(id);
                        if (div > 1) {
                            const int sy = std::min(y / div, h - 1), sx = std::min(x / div, w - 1);
                            firstAt.push_back((uint32_t)sy * (uint32_t)w + (uint32_t)sx);
                            kept.push_back(0);
                        }
                    }
                    lastId = id;
                    lastSlot = it->second;
                }
                if (div == 1) out[x] = lastSlot;
                else {
                    if (out && (x % div) == 0) { out[x / div] = lastSlot; kept[lastSlot] = 1; }
                    curRow[(size_t)x] = lastSlot;
                }
            }
            if (div > 1) {
                // Left-right (wrapping at the date line), then up, up-left and
                // up-right: with the row above, every 8-way contact once.
                for (int x = 0; x < fullW; ++x) {
                    const uint16_t s = curRow[(size_t)x];
                    touch(s, curRow[(size_t)((x + 1) % fullW)]);
                    if (y > 0) {
                        touch(s, prevRow[(size_t)x]);
                        touch(s, prevRow[(size_t)((x + fullW - 1) % fullW)]);
                        touch(s, prevRow[(size_t)((x + 1) % fullW)]);
                    }
                }
                prevRow.swap(curRow);
            }
        }, &why);
    m_fullAdjacency.clear();
    if (ok && !tooMany && div > 1) {
        m_fullAdjacency.reserve(slotPairs.size());
        for (uint32_t pr : slotPairs) {
            int a = palette[pr >> 16], b = palette[pr & 0xFFFF];
            if (a > b) std::swap(a, b);
            m_fullAdjacency.emplace_back(a, b);
        }
        int rescued = 0;
        for (size_t s = 0; s < kept.size(); ++s)
            if (!kept[s]) { index[firstAt[s]] = (uint16_t)s; ++rescued; }
        LoadLog() << "  provinces.png: " << w * div << "x" << h * div << " sampled to " << w << "x" << h
                  << (rescued ? ", " + std::to_string(rescued) + " small province(s) kept at one pixel" : std::string())
                  << std::endl;
    }
    if (!ok || tooMany) {
        LoadLog() << "  provinces.png: row decode unavailable (" << (tooMany ? "too many colours" : why)
                  << "); decoding whole" << std::endl;
        if (!loadFromMemory(imageData, imageSize, jsonStr)) return false;
        if (div > 1) {
            // No province rescue on this rare path (a raster with more than
            // 65535 colours): plain nearest-neighbour.
            ImageResizeNN(&m_image, m_image.width / div, m_image.height / div);
            m_width = m_image.width;
            m_height = m_image.height;
        }
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
            // Initialised, and the parse is checked: a malformed or missing
            // "#rrggbb" in a hand-edited or hostile .odmap left hex holding
            // whatever was on the stack and gave the province a garbage colour.
            unsigned int hex = 0;
            if (sscanf(colorStr.c_str(), "#%06x", &hex) != 1) hex = 0;
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
    bumpRaster();
    if (!m_loaded || m_image.data == nullptr) return;
    // m_image is forced to R8G8B8A8 on load, so a straight copy is safe
    memcpy(m_image.data, pixels, (size_t)m_width * m_height * sizeof(Color));
}

void ProvinceMap::updatePixelsRect(const Color* fullPixels, int x, int y, int w, int h) {
    bumpRaster();
    if (!m_loaded || m_image.data == nullptr) return;
    if (x < 0 || y < 0 || x + w > m_width || y + h > m_height) return;
    auto* dst = static_cast<Color*>(m_image.data);
    for (int row = 0; row < h; ++row)
        memcpy(&dst[(size_t)(y + row) * m_width + x],
               &fullPixels[(size_t)(y + row) * m_width + x],
               (size_t)w * sizeof(Color));
}

int ProvinceMap::slotForId(int id) {
    // The reverse index is derived from the palette, so build it on first use
    // rather than carrying it through compact(); the palette is a few thousand
    // entries, so this is cheap and happens at most once per morphed map.
    if (m_slotOf.empty() && !m_palette.empty())
        for (size_t s = 0; s < m_palette.size(); ++s)
            m_slotOf.emplace(m_palette[s], (uint16_t)s);
    auto it = m_slotOf.find(id);
    if (it != m_slotOf.end()) return it->second;
    if (m_palette.size() >= 0xFFFF) return -1;   // palette full; caller keeps the raster
    const uint16_t slot = (uint16_t)m_palette.size();
    m_palette.push_back(id);
    m_slotOf.emplace(id, slot);
    return slot;
}

bool ProvinceMap::paintPixels(const uint32_t* idx, size_t n, int id) {
    bumpRaster();
    if (!m_loaded || idx == nullptr) return false;
    const size_t total = (size_t)m_width * (size_t)m_height;
    // Image form (editor, or an uncompacted map): write the RGB id directly.
    if (m_image.data != nullptr) {
        auto* dst = static_cast<uint8_t*>(m_image.data);
        for (size_t k = 0; k < n; ++k) {
            const size_t i = idx[k];
            if (i >= total) continue;
            uint8_t* p = dst + i * 4;
            p[0] = (uint8_t)((id >> 16) & 0xFF);
            p[1] = (uint8_t)((id >> 8) & 0xFF);
            p[2] = (uint8_t)(id & 0xFF);
            p[3] = 255;
        }
        return true;
    }
    // Compact form (the running game): one slot for the whole batch, since the
    // target id is constant.
    if (m_index.empty()) return false;
    const int slot = slotForId(id);
    if (slot < 0) return false;
    for (size_t k = 0; k < n; ++k) {
        const size_t i = idx[k];
        if (i < total) m_index[i] = (uint16_t)slot;
    }
    return true;
}

bool ProvinceMap::compact() {
    bumpRaster();
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
    bumpRaster();
    if (m_loaded && m_image.data) UnloadImage(m_image);
    std::vector<uint16_t>().swap(m_index);
    std::vector<std::pair<int, int>>().swap(m_fullAdjacency);
    std::vector<int>().swap(m_palette);
    m_image = Image{};
    m_width = 0;
    m_height = 0;
    m_loaded = false;
    m_provinces.clear();
}
