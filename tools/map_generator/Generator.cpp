#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "Generator.h"
#include "ShapefileReader.h"
#include "population_data.h"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <set>
#include <map>
#include <unordered_map>
#include <cstring>
#include <filesystem>
#include <random>
#include <limits>
#include <queue>
#include <cmath>

namespace fs = std::filesystem;

Generator::Generator(const Config& cfg) : m_cfg(cfg) {
    m_pixels.resize(m_cfg.mapWidth * m_cfg.mapHeight, 0);
}

Generator::~Generator() = default;

bool Generator::downloadFile(const std::string& url, const std::string& destPath) {
    std::cout << "  Downloading " << url << " ..." << std::endl;
    std::string cmd = "curl -sfL --max-time 30 --connect-timeout 10 \"" + url + "\" -o \"" + destPath + "\" 2>/dev/null";
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        std::cerr << "    Download failed (curl exit " << ret << ")" << std::endl;
        return false;
    }
    std::ifstream f(destPath, std::ios::binary | std::ios::ate);
    if (!f || f.tellg() == 0) {
        std::cerr << "    Downloaded file is empty" << std::endl;
        return false;
    }
    return true;
}

bool Generator::extractZip(const std::string& zipPath, const std::string& outDir) {
    fs::create_directories(outDir);

    std::ifstream file(zipPath, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "    Cannot open zip: " << zipPath << std::endl;
        return false;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> zipData(size);
    if (!file.read(reinterpret_cast<char*>(zipData.data()), size)) {
        std::cerr << "    Failed to read zip file" << std::endl;
        return false;
    }
    file.close();

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0)) {
        std::cerr << "    Failed to open zip archive" << std::endl;
        return false;
    }

    int numFiles = mz_zip_reader_get_num_files(&zip);
    for (int i = 0; i < numFiles; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;

        std::string name(stat.m_filename);
        std::string outPath = outDir + "/" + name;

        size_t pos = 0;
        while ((pos = name.find('/', pos)) != std::string::npos) {
            fs::create_directories(outDir + "/" + name.substr(0, pos));
            pos++;
        }

        if (name.back() == '/') {
            fs::create_directories(outPath);
            continue;
        }

        if (!mz_zip_reader_extract_to_file(&zip, i, outPath.c_str(), 0)) {
            std::cerr << "    Failed to extract " << name << std::endl;
            mz_zip_reader_end(&zip);
            return false;
        }
    }

    mz_zip_reader_end(&zip);
    return true;
}

bool Generator::findSHP(const std::string& dir, std::string& out) {
    for (const auto& entry : fs::directory_iterator(dir)) {
        std::string path = entry.path().string();
        if (path.size() > 4 && path.substr(path.size() - 4) == ".shp") {
            out = path;
            return true;
        }
    }
    return false;
}

void Generator::lonLatToPixel(double lon, double lat, int& px, int& py) const {
    px = static_cast<int>((lon + 180.0) / 360.0 * m_cfg.mapWidth);
    py = static_cast<int>((90.0 - lat) / 180.0 * m_cfg.mapHeight);
}

static void lonLatToPixelD(double lon, double lat, int mapW, int mapH, double& px, double& py) {
    px = (lon + 180.0) / 360.0 * mapW;
    py = (90.0 - lat) / 180.0 * mapH;
}

static void fillScanline(std::vector<uint32_t>& pixels, int mapW, int mapH,
                         const std::vector<std::pair<double,double>>& points, uint32_t color)
{
    if (points.size() < 3) return;
    int n = static_cast<int>(points.size());

    double minYd = points[0].second, maxYd = points[0].second;
    for (const auto& p : points) {
        if (p.second < minYd) minYd = p.second;
        if (p.second > maxYd) maxYd = p.second;
    }

    int minY = std::max(0, static_cast<int>(std::floor(minYd)));
    int maxY = std::min(mapH - 1, static_cast<int>(std::ceil(maxYd)));

    for (int y = minY; y <= maxY; ++y) {
        double yf = y + 0.5;
        std::vector<double> xs;
        xs.reserve(32);

        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            double x1 = points[i].first, y1 = points[i].second;
            double x2 = points[j].first, y2 = points[j].second;

            if (y1 == y2) continue;

            if ((y1 <= yf && yf < y2) || (y2 <= yf && yf < y1)) {
                double t = (yf - y1) / (y2 - y1);
                double x = x1 + t * (x2 - x1);
                xs.push_back(x);
            }
        }

        if (xs.size() < 2) continue;
        std::sort(xs.begin(), xs.end());

        for (size_t i = 0; i + 1 < xs.size(); i += 2) {
            double xa = xs[i];
            double xb = xs[i + 1];

            int startX = static_cast<int>(std::ceil(std::max(xa, 0.0)));
            int endX = static_cast<int>(std::floor(std::min(xb, static_cast<double>(mapW))));
            if (endX >= mapW) endX = mapW - 1;
            if (startX <= endX) {
                int rowStart = y * mapW;
                for (int x = startX; x <= endX; ++x)
                    pixels[rowStart + x] = color;
            }

            if (xb > mapW) {
                int wrapEnd = static_cast<int>(std::floor(xb - mapW));
                if (wrapEnd >= mapW) wrapEnd = mapW - 1;
                int rowStart = y * mapW;
                for (int x = 0; x <= wrapEnd; ++x)
                    pixels[rowStart + x] = color;
                }
            }
        }
    }

static void renderRing(std::vector<uint32_t>& pixels, int mapW, int mapH,
                       const std::vector<std::pair<double,double>>& lonlat, uint32_t color)
{
    if (lonlat.size() < 3) return;
    int n = static_cast<int>(lonlat.size());

    std::vector<std::pair<double,double>> pts(n);

    bool crosses = false;
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        if (std::abs(lonlat[i].first - lonlat[j].first) > 180.0) {
            crosses = true;
            break;
        }
    }

    for (int i = 0; i < n; ++i) {
        lonLatToPixelD(lonlat[i].first, lonlat[i].second, mapW, mapH, pts[i].first, pts[i].second);
    }

    double minX = pts[0].first;
    for (const auto& p : pts)
        if (p.first < minX) minX = p.first;
    if (minX < 0) {
        double shift = std::ceil(-minX / mapW) * mapW;
        for (auto& p : pts) p.first += shift;
    }

    fillScanline(pixels, mapW, mapH, pts, color);
}

bool Generator::generateLandSea() {
    std::cout << "\n========================================\n";
    std::cout << "  Generating Land/Sea Map\n";
    std::cout << "========================================\n";

    std::string tmpDir = m_cfg.dataDir + "/tmp_land";
    fs::create_directories(tmpDir);

    std::string landZip = tmpDir + "/land.zip";
    if (!downloadFile(m_cfg.landUrl, landZip)) return false;
    if (!extractZip(landZip, tmpDir)) return false;

    std::string landShp;
    if (!findSHP(tmpDir, landShp)) {
        std::cerr << "  No shapefile found for land" << std::endl;
        return false;
    }

    std::string dbfPath = landShp.substr(0, landShp.size() - 4) + ".dbf";
    ShapefileReader reader;
    if (!reader.open(landShp, dbfPath)) return false;

    std::cout << "  Rasterizing " << reader.getRecordCount() << " land polygons...\n";

    for (int i = 0; i < reader.getRecordCount(); ++i) {
        const ShapeObject& shape = reader.getShape(i);

        for (const auto& ring : shape.rings) {
            std::vector<std::pair<double,double>> pts;
            pts.reserve(ring.points.size());
            for (const auto& pt : ring.points) {
                pts.emplace_back(pt.x, pt.y);
            }
            renderRing(m_pixels, m_cfg.mapWidth, m_cfg.mapHeight, pts,
                       ring.isHole ? 0 : 0xFFFFFFFF);
        }
    }

    std::string outPath = m_cfg.dataDir + "/land_sea.png";
    stbi_write_png(outPath.c_str(), m_cfg.mapWidth, m_cfg.mapHeight, 4,
                   m_pixels.data(), m_cfg.mapWidth * 4);
    std::cout << "  Saved " << outPath << " (" << m_cfg.mapWidth << "x" << m_cfg.mapHeight << ")\n";

    if (m_cfg.lakesUrl != m_cfg.landUrl) {
        std::string lakesZip = tmpDir + "/lakes.zip";
        if (downloadFile(m_cfg.lakesUrl, lakesZip)) {
            std::string lakesDir = tmpDir + "/lakes";
            if (extractZip(lakesZip, lakesDir)) {
                std::string lakesShp;
                if (findSHP(lakesDir, lakesShp)) {
                    std::string lakesDbf = lakesShp.substr(0, lakesShp.size() - 4) + ".dbf";
                    ShapefileReader lakeReader;
                    if (lakeReader.open(lakesShp, lakesDbf)) {
                        std::cout << "  Rasterizing " << lakeReader.getRecordCount() << " lake polygons...\n";
                        for (int i = 0; i < lakeReader.getRecordCount(); ++i) {
                            const ShapeObject& shape = lakeReader.getShape(i);
                            for (const auto& ring : shape.rings) {
                                std::vector<std::pair<double,double>> pts;
                                pts.reserve(ring.points.size());
                                for (const auto& pt : ring.points) {
                                    pts.emplace_back(pt.x, pt.y);
                                }
                                renderRing(m_pixels, m_cfg.mapWidth, m_cfg.mapHeight, pts, 0);
                            }
                        }
                        stbi_write_png(outPath.c_str(), m_cfg.mapWidth, m_cfg.mapHeight, 4,
                                       m_pixels.data(), m_cfg.mapWidth * 4);
                        std::cout << "  Updated with lakes\n";
                    }
                }
            }
        }
    }

    fs::remove_all(tmpDir);
    return true;
}

bool Generator::rasterizeUrl(const std::string& url, const std::string& label,
                             std::vector<uint32_t>& pixels,
                             nlohmann::json& json,
                             int& nextId) {
    std::string tmpDir = m_cfg.dataDir + "/tmp_" + label;
    fs::create_directories(tmpDir);
    std::string zipPath = tmpDir + "/data.zip";

    {
        std::string cmd = "curl -sfL --max-time 90 --connect-timeout 10 \"" + url + "\" -o \"" + zipPath + "\" 2>/dev/null";
        if (std::system(cmd.c_str()) != 0) {
            std::cerr << "  Failed to download " << url << std::endl;
            fs::remove_all(tmpDir);
            return false;
        }
    }

    std::ifstream file(zipPath, std::ios::binary | std::ios::ate);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> zipData(size);
    if (!file.read(reinterpret_cast<char*>(zipData.data()), size)) {
        std::cerr << "  Failed to read zip\n";
        fs::remove_all(tmpDir);
        return false;
    }
    file.close();

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0)) {
        std::cerr << "  Failed to open zip archive\n";
        fs::remove_all(tmpDir);
        return false;
    }

    int numFiles = mz_zip_reader_get_num_files(&zip);
    std::string shpPath, dbfPath;
    for (int i = 0; i < numFiles; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
        std::string name(stat.m_filename);
        std::string outPath = tmpDir + "/" + name;

        size_t pos = 0;
        while ((pos = name.find('/', pos)) != std::string::npos) {
            fs::create_directories(tmpDir + "/" + name.substr(0, pos));
            pos++;
        }

        if (name.back() == '/') { fs::create_directories(outPath); continue; }
        if (!mz_zip_reader_extract_to_file(&zip, i, outPath.c_str(), 0)) continue;

        if (name.size() > 4 && name.substr(name.size() - 4) == ".shp") shpPath = outPath;
        if (name.size() > 4 && name.substr(name.size() - 4) == ".dbf") dbfPath = outPath;
    }
    mz_zip_reader_end(&zip);

    if (shpPath.empty()) { std::cerr << "  No SHP found in archive\n"; fs::remove_all(tmpDir); return false; }
    if (dbfPath.empty()) dbfPath = shpPath.substr(0, shpPath.size() - 4) + ".dbf";

    ShapefileReader reader;
    if (!reader.open(shpPath, dbfPath)) { fs::remove_all(tmpDir); return false; }

    int nameIdx = reader.getFieldIndex("NAME");
    if (nameIdx < 0) nameIdx = reader.getFieldIndex("NAME_LONG");
    if (nameIdx < 0) nameIdx = reader.getFieldIndex("ADMIN");
    if (nameIdx < 0) nameIdx = reader.getFieldIndex("SOVEREIGNT");
    if (nameIdx < 0) nameIdx = reader.getFieldIndex("name");
    if (nameIdx < 0) nameIdx = 0;

    int isoIdx = reader.getFieldIndex("ISO_A3");
    if (isoIdx < 0) isoIdx = reader.getFieldIndex("ADM0_A3");
    if (isoIdx < 0) isoIdx = reader.getFieldIndex("ADM0_A3_IS");
    if (isoIdx < 0) isoIdx = -1;

    int typeIdx = reader.getFieldIndex("TYPE");
    if (typeIdx < 0) typeIdx = reader.getFieldIndex("FEATURECLA");
    if (typeIdx < 0) typeIdx = -1;

    std::cout << "  Rasterizing " << reader.getRecordCount() << " polygons...\n";

    int count = 0;
    for (int i = 0; i < reader.getRecordCount(); ++i) {
        const ShapeObject& shape = reader.getShape(i);

        uint8_t r = (nextId >> 16) & 0xFF;
        uint8_t g = (nextId >> 8) & 0xFF;
        uint8_t b = nextId & 0xFF;
        uint32_t color = (0xFF << 24) | (b << 16) | (g << 8) | r;

        std::string name = reader.getStringField(i, nameIdx);
        if (name.empty()) name = "Province " + std::to_string(nextId);

        std::string iso_a3;
        if (isoIdx >= 0) iso_a3 = reader.getStringField(i, isoIdx);

        // Fix Natural Earth returning "-99" for known countries
        if (iso_a3 == "-99") {
            auto& overrides = getIsoOverrides();
            auto it = overrides.find(name);
            if (it != overrides.end()) iso_a3 = it->second;
        }

        std::string featType;
        if (typeIdx >= 0) featType = reader.getStringField(i, typeIdx);

        char hex[8];
        snprintf(hex, sizeof(hex), "#%02x%02x%02x", r, g, b);

        nlohmann::json entry;
        entry["id"] = nextId;
        entry["name"] = name;
        entry["iso_a3"] = iso_a3;
        if (!featType.empty()) entry["type"] = featType;
        entry["color"] = std::string(hex);
        json[std::to_string(nextId)] = entry;

        for (const auto& ring : shape.rings) {
            std::vector<std::pair<double,double>> pts;
            pts.reserve(ring.points.size());
            for (const auto& pt : ring.points) {
                pts.emplace_back(pt.x, pt.y);
            }
            renderRing(pixels, m_cfg.mapWidth, m_cfg.mapHeight, pts,
                       ring.isHole ? 0 : color);
        }

        nextId++;
        count++;
    }

    std::cout << "  Rasterized " << count << " features into province map\n";

    fs::remove_all(tmpDir);
    return true;
}

bool Generator::saveProvinceFiles(const std::vector<uint32_t>& pixels,
                                        const nlohmann::json& json,
                                        const std::string& filePrefix,
                                        const std::string& label) {
    int totalProvinces = json.size();

    std::string provImgPath = m_cfg.dataDir + "/" + filePrefix + "provinces.png";
    stbi_write_png(provImgPath.c_str(), m_cfg.mapWidth, m_cfg.mapHeight, 4,
                   pixels.data(), m_cfg.mapWidth * 4);
    std::cout << "  Saved " << provImgPath << "\n";

    std::string jsonPath = m_cfg.dataDir + "/" + filePrefix + "provinces.json";
    std::ofstream jsonFile(jsonPath);
    jsonFile << json.dump(2);
    jsonFile.close();
    std::cout << "  Saved " << jsonPath << " (" << totalProvinces << " provinces)\n";

    return true;
}

static int pixelToId(uint32_t pixel) {
    uint8_t r = pixel & 0xFF;
    uint8_t g = (pixel >> 8) & 0xFF;
    uint8_t b = (pixel >> 16) & 0xFF;
    if (r == 0 && g == 0 && b == 0 && ((pixel >> 24) == 0)) return 0;
    return (r << 16) | (g << 8) | b;
}

static uint32_t idToPixel(int id) {
    uint8_t r = (id >> 16) & 0xFF;
    uint8_t g = (id >> 8) & 0xFF;
    uint8_t b = id & 0xFF;
    return (0xFF << 24) | (b << 16) | (g << 8) | r;
}

static float noiseFunc(int x, int y, int seed) {
    int h = seed * 374761393 + x * 668265263 + y * 1274126177;
    h = (h ^ (h >> 13)) * 1274126177;
    h = h ^ (h >> 16);
    return (h & 0x7FFFFFFF) / (float)0x7FFFFFFF;
}

static float smoothNoiseFunc(float fx, float fy, int seed) {
    int ix = (int)std::floor(fx);
    int iy = (int)std::floor(fy);
    float fracX = fx - ix;
    float fracY = fy - iy;
    float sx = fracX * fracX * (3.0f - 2.0f * fracX);
    float sy = fracY * fracY * (3.0f - 2.0f * fracY);
    float v00 = noiseFunc(ix, iy, seed);
    float v10 = noiseFunc(ix + 1, iy, seed);
    float v01 = noiseFunc(ix, iy + 1, seed);
    float v11 = noiseFunc(ix + 1, iy + 1, seed);
    return v00 * (1 - sx) * (1 - sy) + v10 * sx * (1 - sy) +
           v01 * (1 - sx) * sy + v11 * sx * sy;
}

static float fbmNoiseFunc(float x, float y, int seed) {
    float val = 0, amp = 1, freq = 1, sum = 0;
    for (int i = 0; i < 4; ++i) {
        val += amp * smoothNoiseFunc(x * freq, y * freq, seed + i * 73);
        sum += amp;
        amp *= 0.5f;
        freq *= 2.3f;
    }
    return val / sum;
}

bool Generator::createODMArchive(const std::vector<std::string>& files,
                                  const std::string& odmPath) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, odmPath.c_str(), 0)) {
        std::cerr << "  Failed to create .odmap archive" << std::endl;
        return false;
    }

    for (const auto& filePath : files) {
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file) {
            std::cerr << "    Skipping missing file: " << filePath << std::endl;
            continue;
        }
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> data(size);
        if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
            std::cerr << "    Failed to read " << filePath << std::endl;
            continue;
        }
        file.close();

        std::string filename = fs::path(filePath).filename().string();
        if (!mz_zip_writer_add_mem(&zip, filename.c_str(),
                                    data.data(), data.size(),
                                    MZ_BEST_COMPRESSION)) {
            std::cerr << "    Failed to add " << filename << " to archive (skipping)" << std::endl;
            continue;
        }
    }

    // Always add empty scripts/ directory as a marker
    mz_zip_writer_add_mem(&zip, "scripts/", nullptr, 0, MZ_NO_COMPRESSION);

    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return true;
}

bool Generator::generateProvinces() {
    int W = m_cfg.mapWidth, H = m_cfg.mapHeight;
    int noiseSeed = 19349663;

    std::cout << "\n========================================\n";
    std::cout << "  Generating Provinces\n";
    std::cout << "========================================\n";

    std::string landPath = m_cfg.dataDir + "/land_sea.png";
    int lw, lh, lc;
    uint8_t* landData = stbi_load(landPath.c_str(), &lw, &lh, &lc, 4);
    if (!landData) {
        std::cerr << "  Failed to load land/sea map\n";
        return false;
    }
    std::cout << "  Loaded land/sea map: " << lw << "x" << lh << "\n";

    std::cout << "\n--- Step 1: Rasterize countries (de-facto borders) ---\n";
    std::vector<uint32_t> basePixels(W * H, 0);
    nlohmann::json baseJson;
    int nextId = 1;
    if (!rasterizeUrl(m_cfg.deFactoUrl, "countries_base", basePixels, baseJson, nextId))
        return false;

    std::cout << "\n--- Step 2: Overlay disputed areas ---\n";
    std::vector<uint32_t> mergedPixels = basePixels;
    nlohmann::json mergedJson = baseJson;
    nextId = 1 + (int)baseJson.size();
    if (!rasterizeUrl(m_cfg.disputedUrl, "merged_overlay", mergedPixels, mergedJson, nextId))
        return false;

    // Restore base country pixels where disputed overlay overwrote recognized countries
    {
        std::set<int> anyBaseIds;
        for (auto& [idStr, entry] : baseJson.items()) {
            anyBaseIds.insert(std::stoi(idStr));
        }
        std::set<int> disputeIds;
        for (auto& [idStr, entry] : mergedJson.items()) {
            std::string iso = entry.value("iso_a3", "");
            if (iso == "-99")
                disputeIds.insert(std::stoi(idStr));
        }
        int restored = 0;
        for (int i = 0; i < W * H; ++i) {
            uint32_t bCol = basePixels[i];
            uint32_t mCol = mergedPixels[i];
            if (bCol == 0 || mCol == 0) continue;
            int bid = pixelToId(bCol);
            int mid = pixelToId(mCol);
            if (bid == mid) continue;
            if (anyBaseIds.count(bid) && disputeIds.count(mid)) {
                mergedPixels[i] = bCol;
                restored++;
            }
        }
        if (restored > 0)
            std::cout << "  Restored " << restored << " base country pixels overwritten by disputed features\n";
        else
            std::cout << "  No base country pixels overwritten\n";
    }

    // Mask for Crimea pixels (used later to split into separate province)
    std::vector<bool> crimeaMask(W * H, false);
    int crimeaRid = 0;

    // ── Step 2.5: Reassign Crimea from de-facto Russia to UN-recognized Ukraine ──
    {
        const char* CRIMEA_PREFIXES[] = {"Crimea", "Avtonomna", "Sevastopol"};

        if (!m_cfg.adminUrl.empty()) {
            std::string tmpDir = m_cfg.dataDir + "/tmp_admin_fix";
            fs::create_directories(tmpDir);
            std::string zipPath = tmpDir + "/admin.zip";

            if (downloadFile(m_cfg.adminUrl, zipPath) && extractZip(zipPath, tmpDir)) {
                std::string shpPath;
                if (findSHP(tmpDir, shpPath)) {
                    std::string dbfPath = shpPath.substr(0, shpPath.size() - 4) + ".dbf";
                    ShapefileReader adminReader;
                    if (adminReader.open(shpPath, dbfPath)) {
                        int nameIdx = adminReader.getFieldIndex("name");
                        if (nameIdx < 0) nameIdx = adminReader.getFieldIndex("NAME");

                        // Find Russia and Ukraine IDs in mergedJson
                        int rusId = 0, ukrId = 0;
                        for (auto& [idStr, entry] : mergedJson.items()) {
                            std::string nm = entry.value("name", "");
                            if (nm == "Russia") rusId = entry.value("id", 0);
                            if (nm == "Ukraine") ukrId = entry.value("id", 0);
                        }
                        if (rusId > 0 && ukrId > 0) {
                            uint8_t rr = (rusId >> 16) & 0xFF, rg = (rusId >> 8) & 0xFF, rb = rusId & 0xFF;
                            uint8_t ur = (ukrId >> 16) & 0xFF, ug = (ukrId >> 8) & 0xFF, ub = ukrId & 0xFF;
                            uint32_t rusCol = (0xFF << 24) | (rb << 16) | (rg << 8) | rr;
                            uint32_t ukrCol = (0xFF << 24) | (ub << 16) | (ug << 8) | ur;
                            uint32_t tmpCol = 0xDEADBEEF;
                            int totalFixed = 0;

                            for (auto prefix : CRIMEA_PREFIXES) {
                                for (int i = 0; i < adminReader.getRecordCount(); ++i) {
                                    std::string nm = adminReader.getStringField(i, nameIdx);
                                    if (nm.empty() || nm.find(prefix) != 0) continue;

                                    const ShapeObject& shape = adminReader.getShape(i);
                                    std::vector<uint32_t> tempPixels(W * H, 0);
                                    int bxMin = W, bxMax = 0, byMin = H, byMax = 0;
                                    for (const auto& ring : shape.rings) {
                                        std::vector<std::pair<double,double>> pts;
                                        pts.reserve(ring.points.size());
                                        for (const auto& pt : ring.points) {
                                            pts.emplace_back(pt.x, pt.y);
                                            double dpx, dpy;
                                            lonLatToPixelD(pt.x, pt.y, W, H, dpx, dpy);
                                            int ipx = static_cast<int>(dpx);
                                            int ipy = static_cast<int>(dpy);
                                            while (ipx < 0) ipx += W;
                                            while (ipx >= W) ipx -= W;
                                            ipx = std::clamp(ipx, 0, W - 1);
                                            ipy = std::clamp(ipy, 0, H - 1);
                                            if (ipx < bxMin) bxMin = ipx;
                                            if (ipx > bxMax) bxMax = ipx;
                                            if (ipy < byMin) byMin = ipy;
                                            if (ipy > byMax) byMax = ipy;
                                        }
                                        renderRing(tempPixels, W, H, pts, ring.isHole ? 0 : tmpCol);
                                    }
                                    bxMin = std::max(0, bxMin - 5);
                                    bxMax = std::min(W - 1, bxMax + 5);
                                    byMin = std::max(0, byMin - 5);
                                    byMax = std::min(H - 1, byMax + 5);

                                    int changed = 0;
                                    for (int py = byMin; py <= byMax; ++py)
                                        for (int px = bxMin; px <= bxMax; ++px) {
                                            int pi = py * W + px;
                                            if (tempPixels[pi] == tmpCol && mergedPixels[pi] == rusCol) {
                                                mergedPixels[pi] = ukrCol;
                                                crimeaMask[pi] = true;
                                                changed++;
                                            }
                                        }
                                    if (changed > 0) totalFixed += changed;
                                    break;
                                }
                            }

                            if (totalFixed > 0) {
                                std::cout << "\n--- Step 2.5: Reassign Crimea from Russia to Ukraine ---\n";
                                std::cout << "  Crimea → Ukraine (" << totalFixed << " px)\n";
                            }
                        }
                    }
                }
            }
            fs::remove_all(tmpDir);
        }
    }

    // ── Step 2.6: Merge post-2000 territories for Jan 2000 map accuracy ──
    {
        struct MergeSpec { const char* targetIso; const char* targetName; const char* parentName; };
        MergeSpec specs[] = {
            {"SSD", nullptr, "Sudan"},           // South Sudan → Sudan (2011)
            {"XKX", nullptr, "Serbia"},          // Kosovo → Serbia (2008)
            {nullptr, "Kosovo", "Serbia"},       // Kosovo (no-ISO fallback)
            {"TLS", nullptr, "Indonesia"},       // East Timor → Indonesia (2002)
            {"MNE", nullptr, "Serbia"},          // Montenegro → Serbia (2006)
        };
        for (auto& sp : specs) {
            int targetId = 0, parentId = 0;
            std::string targetIso;
            for (auto& [idStr, entry] : mergedJson.items()) {
                std::string iso = entry.value("iso_a3", "");
                std::string nm = entry.value("name", "");
                if (sp.targetIso && iso == sp.targetIso) {
                    targetId = entry["id"];
                    targetIso = iso;
                }
                if (sp.targetName && nm == sp.targetName) {
                    targetId = entry["id"];
                    targetIso = iso;
                }
                if (nm == sp.parentName) parentId = entry["id"];
            }
            if (targetId <= 0 || parentId <= 0 || targetId == parentId) continue;

            uint8_t tr = (targetId >> 16) & 0xFF, tg = (targetId >> 8) & 0xFF, tb = targetId & 0xFF;
            uint8_t pr = (parentId >> 16) & 0xFF, pg = (parentId >> 8) & 0xFF, pb = parentId & 0xFF;
            uint32_t tCol = (0xFF << 24) | (tb << 16) | (tg << 8) | tr;
            uint32_t pCol = (0xFF << 24) | (pb << 16) | (pg << 8) | pr;

            int changed = 0;
            for (int i = 0; i < W * H; ++i) {
                if (mergedPixels[i] == tCol) { mergedPixels[i] = pCol; changed++; }
                if (basePixels[i] == tCol) basePixels[i] = pCol;
            }
            if (changed > 0) {
                std::string targetName = targetIso.empty() ? (sp.targetName ? sp.targetName : "?") : targetIso;
                std::cout << "  Merged " << targetName << " (" << changed << " px) into " << sp.parentName << "\n";
            }
        }
    }

    std::cout << "\n--- Step 3: Build combined region map (countries) ---\n";
    std::vector<uint32_t> regionPixels(W * H, 0);
    std::map<std::pair<int,int>, int> pairToRegionId;
    std::map<int, std::string> regionNames;
    std::map<int, std::string> regionIsoA3;
    std::map<int, std::string> regionTypes;
    std::set<int> baseOnlyRids;
    int nextRegionId = 1;

    for (int i = 0; i < W * H; ++i) {
        uint32_t baseCol = basePixels[i];
        uint32_t mergCol = mergedPixels[i];
        if (baseCol == 0 && mergCol == 0) continue;

        int idA = pixelToId(baseCol);
        int idB = pixelToId(mergCol);
        if (idA == 0) idA = idB;
        if (idB == 0) idB = idA;

        auto key = std::make_pair(idA, idB);
        auto it = pairToRegionId.find(key);
        int rid;
        if (it == pairToRegionId.end()) {
            rid = nextRegionId++;
            pairToRegionId[key] = rid;
            if (idA == idB) baseOnlyRids.insert(rid);
            std::string keyB = std::to_string(idB);
            if (mergedJson.contains(keyB)) {
                regionNames[rid] = mergedJson[keyB]["name"];
                regionIsoA3[rid] = mergedJson[keyB]["iso_a3"];
                if (mergedJson[keyB].contains("type"))
                    regionTypes[rid] = mergedJson[keyB]["type"];
            } else {
                std::string keyA = std::to_string(idA);
                if (baseJson.contains(keyA)) {
                    regionNames[rid] = baseJson[keyA]["name"];
                    regionIsoA3[rid] = baseJson[keyA]["iso_a3"];
                    if (baseJson[keyA].contains("type"))
                        regionTypes[rid] = baseJson[keyA]["type"];
                }
            }
        } else {
            rid = it->second;
        }
        regionPixels[i] = rid;
    }
    int totalRegions = nextRegionId - 1;
    std::cout << "  Created " << totalRegions << " countries\n";

    // Remove small isolated islands from land/sea (unclickable tiny provinces)
    {
        const int MIN_ISLAND_PIXELS = 50;
        std::vector<uint8_t> visited(W * H, 0);
        int islandsRemoved = 0, pixelsRemoved = 0;
        for (int i = 0; i < W * H; ++i) {
            if (landData[i * 4 + 3] == 0 || visited[i]) continue;
            std::vector<int> stack, comp;
            stack.push_back(i);
            visited[i] = 1;
            while (!stack.empty()) {
                int cur = stack.back(); stack.pop_back();
                comp.push_back(cur);
                int cx = cur % W, cy = cur / W;
                for (int d = 0; d < 4; ++d) {
                    int nx = cx + (d == 0) - (d == 1);
                    int ny = cy + (d == 2) - (d == 3);
                    if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                    int nidx = ny * W + nx;
                    if (landData[nidx * 4 + 3] != 0 && !visited[nidx]) {
                        visited[nidx] = 1;
                        stack.push_back(nidx);
                    }
                }
            }
            if ((int)comp.size() >= MIN_ISLAND_PIXELS) continue;
            for (int p : comp) {
                landData[p * 4] = 0;
                landData[p * 4 + 1] = 0;
                landData[p * 4 + 2] = 0;
                landData[p * 4 + 3] = 255;
            }
            islandsRemoved++;
            pixelsRemoved += (int)comp.size();
        }
        if (islandsRemoved > 0) {
            stbi_write_png(landPath.c_str(), W, H, 4, landData, W * 4);
            std::cout << "  Removed " << islandsRemoved << " tiny islands ("
                      << pixelsRemoved << " pixels) from land/sea\n";
        } else {
            std::cout << "  No tiny islands to remove from land/sea\n";
        }
    }

    // Blend left/right edges to hide map seam (horizontal wrap)
    {
        const int MARGIN = 5;
        for (int y = 0; y < H; ++y) {
            for (int m = 0; m < MARGIN; ++m) {
                regionPixels[y * W + m] = regionPixels[y * W + (W - 1 - m)];
                regionPixels[y * W + (W - 1 - m)] = regionPixels[y * W + m];
                for (int c = 0; c < 4; ++c) {
                    landData[(y * W + m) * 4 + c] = landData[(y * W + (W - 1 - m)) * 4 + c];
                    landData[(y * W + (W - 1 - m)) * 4 + c] = landData[(y * W + m) * 4 + c];
                }
            }
        }
        // Save fixed land_sea back to disk
        stbi_write_png(landPath.c_str(), W, H, 4, landData, W * 4);
        std::cout << "  Blended " << MARGIN << "-px edge strip for seam\n";
    }

    // ── Give Crimea its own region ID (so smoothing won't merge its provinces) ──
    // Step 3 gave Crimea a separate (base=Russia, merged=Ukraine) rid from Ukraine's.
    // Rename it so Step 4.5 doesn't merge them back.
    {
        for (int i = 0; i < W * H; ++i) {
            if (crimeaMask[i]) { crimeaRid = (int)regionPixels[i]; break; }
        }
        if (crimeaRid > 0) {
            int ukrRid = 0;
            for (auto& [rid, name] : regionNames)
                if (name == "Ukraine") { ukrRid = rid; break; }
            if (crimeaRid != ukrRid) {
                regionNames[crimeaRid] = "Crimea";
                std::cout << "  Crimea isolated as rid " << crimeaRid << "\n";
            } else {
                crimeaRid = 0;
            }
        }
    }

    std::cout << "\n--- Step 4: Fill unassigned land via region BFS ---\n";
    {
        const uint32_t TMP = 65533;
        std::vector<int> allMarked;
        allMarked.reserve(4096);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                int idx = y * W + x;
                if (regionPixels[idx] != 0 || landData[idx * 4 + 3] == 0) continue;
                std::vector<int> stack = {idx};
                regionPixels[idx] = TMP;
                allMarked.push_back(idx);
                int foundRegion = 0;
                std::vector<int> cluster;
                bool searching = true;
                while (searching && !stack.empty()) {
                    int cur = stack.back(); stack.pop_back();
                    cluster.push_back(cur);
                    int cx = cur % W, cy = cur / W;
                    for (int d = 0; d < 4; ++d) {
                        int nx = cx + (d == 0) - (d == 1);
                        int ny = cy + (d == 2) - (d == 3);
                        if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                        int nidx = ny * W + nx;
                        uint32_t nv = regionPixels[nidx];
                        if (nv > 0 && nv != TMP && landData[nidx * 4 + 3] != 0) {
                            foundRegion = nv;
                            searching = false;
                            break;
                        }
                        if (nv == 0 && landData[nidx * 4 + 3] != 0) {
                            regionPixels[nidx] = TMP;
                            allMarked.push_back(nidx);
                            stack.push_back(nidx);
                        }
                    }
                }
                if (foundRegion > 0) {
                    for (int ci : cluster) regionPixels[ci] = foundRegion;
                }
            }
        }
        // Reset any leaked TMP marks (unprocessed stack items or clusters that found nothing)
        for (int vi : allMarked) {
            if (regionPixels[vi] == TMP) regionPixels[vi] = 0;
        }
    }

    std::cout << "\n--- Step 4.5: Merge countries with same name ---\n";
    {
        std::map<std::string, std::vector<int>> nameToRids;
        for (auto& [rid, _] : regionNames) {
            if (!regionNames[rid].empty())
                nameToRids[regionNames[rid]].push_back(rid);
        }
        std::map<int, int> ridRemap;
        for (auto& [name, rids] : nameToRids) {
            if (rids.size() <= 1) continue;
            // Prefer canonical with valid iso_a3 (not "-99")
            int canonical = rids[0];
            for (int rid : rids) {
                auto it = regionIsoA3.find(rid);
                if (it != regionIsoA3.end() && it->second != "-99" && !it->second.empty()) {
                    canonical = rid;
                    break;
                }
            }
            for (size_t i = 0; i < rids.size(); ++i)
                if (rids[i] != canonical)
                    ridRemap[rids[i]] = canonical;
        }
        if (!ridRemap.empty()) {
            for (int j = 0; j < W * H; ++j) {
                auto it = ridRemap.find((int)regionPixels[j]);
                if (it != ridRemap.end())
                    regionPixels[j] = it->second;
            }
            for (auto& [oldRid, _] : ridRemap) {
                regionNames.erase(oldRid);
                regionIsoA3.erase(oldRid);
            }
            totalRegions = (int)regionNames.size();
            std::cout << "  Merged " << ridRemap.size() << " sub-features, now "
                      << totalRegions << " unique countries\n";
        } else {
            std::cout << "  No merges needed\n";
        }
    }

    // Step 4.6: Filter non-government features (military bases, buffer zones, DMZs, etc.) into smallest neighbor
    {
        const int UNC_RID = 65534;
        std::set<int> filterRids;
        std::vector<std::string> nonGovPatterns = {
            "DMZ", "Demilitarized", "Buffer Zone", "Base", "NSF", "Cosmodrome", "Baikonur",
            "Siachen Glacier", "Bir Tawil", "Halayib Triangle",
            "Corner of Artigas", "Southern Patagonian Ice Field"
        };
    std::cout << "  Saving " << regionNames.size() << " country entries...\n";
    for (auto& [rid, name] : regionNames) {
            auto it = regionIsoA3.find(rid);
            if (it == regionIsoA3.end() || it->second != "-99") continue;
            bool isNonGov = false;
            for (const auto& pat : nonGovPatterns) {
                if (name.find(pat) != std::string::npos) {
                    isNonGov = true;
                    break;
                }
            }
            if (isNonGov) filterRids.insert(rid);
        }
        // Also filter any base-only "-99" features that match non-government patterns
        // (these already matched above since they're in regionNames)
        std::cout << "\n--- Step 4.6: Filter non-government features ---\n";
        if (!filterRids.empty()) {
            // For each filtered pixel, find its smallest neighboring region
            std::vector<int> replacement(W * H, 0);
            int totalMerged = 0;
            for (int i = 0; i < W * H; ++i) {
                if (landData[i * 4 + 3] == 0) continue;
                int rid = (int)regionPixels[i];
                if (!filterRids.count(rid)) continue;
                // Count neighbor regions by pixel count
                std::map<int, int> neighborSizes;
                int cx = i % W, cy = i / W;
                for (int d = 0; d < 4; ++d) {
                    int nx = cx + (d == 0) - (d == 1);
                    int ny = cy + (d == 2) - (d == 3);
                    if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                    int nidx = ny * W + nx;
                    int nrid = (int)regionPixels[nidx];
                    if (nrid > 0 && !filterRids.count(nrid))
                        neighborSizes[nrid]++;
                }
                // 5-pixel radius fallback
                if (neighborSizes.empty()) {
                    for (int dy = -5; dy <= 5; ++dy) {
                        for (int dx = -5; dx <= 5; ++dx) {
                            if (dx == 0 && dy == 0) continue;
                            int nx = cx + dx, ny = cy + dy;
                            if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                            int nrid = (int)regionPixels[ny * W + nx];
                            if (nrid > 0 && !filterRids.count(nrid))
                                neighborSizes[nrid]++;
                        }
                    }
                }
                if (neighborSizes.empty()) {
                    replacement[i] = UNC_RID;
                } else {
                    int best = -1, bestCount = INT_MAX;
                    for (auto& [nrid, cnt] : neighborSizes)
                        if (cnt < bestCount) { bestCount = cnt; best = nrid; }
                    replacement[i] = best;
                }
                totalMerged++;
            }
            int mergedToUnc = 0;
            for (int i = 0; i < W * H; ++i) {
                if (replacement[i] == 0) continue;
                if (replacement[i] == UNC_RID) mergedToUnc++;
                regionPixels[i] = replacement[i];
            }
            for (int rid : filterRids) {
                regionNames.erase(rid);
                regionIsoA3.erase(rid);
            }
            const int BLC_RID2 = 65535;
            regionNames[UNC_RID] = "Unclaimed";
            regionIsoA3[UNC_RID] = "UNC";
            regionNames[BLC_RID2] = "Blocked Territory";
            regionIsoA3[BLC_RID2] = "BLC";
            totalRegions = (int)regionNames.size();
            std::cout << "  Merged " << filterRids.size() << " features ("
                      << totalMerged << " pixels) into smallest neighbor";
            if (mergedToUnc > 0)
                std::cout << " (" << mergedToUnc << " to UNC)";
            std::cout << "\n  Now " << totalRegions << " countries (+ UNC)\n";
        } else {
            std::cout << "  No non-government features found\n";
        }
        // Reassign Antarctica to UNC
        for (auto& [rid, iso] : regionIsoA3) {
            if (iso == "ATA" || regionNames[rid] == "Antarctica") {
                int antPixels = 0;
                for (int i = 0; i < W * H; ++i) {
                    if ((int)regionPixels[i] == rid) {
                        regionPixels[i] = UNC_RID;
                        antPixels++;
                    }
                }
                regionNames.erase(rid);
                regionIsoA3.erase(rid);
                std::cout << "  Reassigned Antarctica (" << antPixels
                          << " pixels) to UNC\n";
                break;
            }
        }
    }

    // Remove micro-countries (too small to be playable).
    // Reassign their pixels to the neighboring country with the longest shared border.
    {
        const int MIN_COUNTRY_PIXELS = 40;
        std::map<int, int> countrySizes;
        for (int i = 0; i < W * H; ++i) {
            if (regionPixels[i] != 0) countrySizes[(int)regionPixels[i]]++;
        }
        std::set<int> microRids;
        for (auto& [rid, sz] : countrySizes) {
            if (sz < MIN_COUNTRY_PIXELS && regionNames.count(rid)) {
                microRids.insert(rid);
            }
        }
        if (!microRids.empty()) {
            std::cout << "\n--- Step 4.7: Remove micro-countries ---\n";
            int removed = 0, reassignedPx = 0;
            for (int rid : microRids) {
                // Find neighbor with longest shared border
                std::map<int, int> borderLen;
                for (int y = 0; y < H; ++y) {
                    for (int x = 0; x < W; ++x) {
                        int i = y * W + x;
                        if ((int)regionPixels[i] != rid) continue;
                        int nx[4] = {x + 1, x - 1, x, x};
                        int ny[4] = {y, y, y + 1, y - 1};
                        for (int d = 0; d < 4; ++d) {
                            if (nx[d] < 0 || nx[d] >= W || ny[d] < 0 || ny[d] >= H) continue;
                            int nid = ny[d] * W + nx[d];
                            int nrid = (int)regionPixels[nid];
                            if (nrid > 0 && nrid != rid && nrid != 65534 && nrid != 65535) {
                                borderLen[nrid]++;
                            }
                        }
                    }
                }
                if (borderLen.empty()) {
                    // No valid neighbor — merge into UNC
                    for (int i = 0; i < W * H; ++i) {
                        if ((int)regionPixels[i] == rid) regionPixels[i] = 65534;
                    }
                } else {
                    int bestNbr = 0, bestLen = 0;
                    for (auto& [nrid, len] : borderLen) {
                        if (len > bestLen) { bestLen = len; bestNbr = nrid; }
                    }
                    for (int i = 0; i < W * H; ++i) {
                        if ((int)regionPixels[i] == rid) {
                            regionPixels[i] = bestNbr;
                            reassignedPx++;
                        }
                    }
                }
                regionNames.erase(rid);
                regionIsoA3.erase(rid);
                regionTypes.erase(rid);
                removed++;
            }
            std::cout << "  Removed " << removed << " micro-countries ("
                      << reassignedPx << " px reassigned)\n";
        }
    }

    // Build adjacency graph and assign optimized hues
    std::cout << "\n--- Step 4.8: Graph coloring for neighbor-distinct colors ---\n";
    const int UNC_RID2 = 65534;
    const int BLC_RID = 65535;
    std::map<int, std::set<int>> adj;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int i = y * W + x;
            int rid = (int)regionPixels[i];
            if (rid == 0 || landData[i * 4 + 3] == 0) continue;
            if (x < W - 1) {
                int rid2 = (int)regionPixels[i + 1];
                if (rid2 > 0 && rid2 != rid) {
                    adj[rid].insert(rid2);
                    adj[rid2].insert(rid);
                }
            }
            if (y < H - 1) {
                int rid2 = (int)regionPixels[i + W];
                if (rid2 > 0 && rid2 != rid) {
                    adj[rid].insert(rid2);
                    adj[rid2].insert(rid);
                }
            }
        }
    }
    std::map<int, float> hueMap;
    {
        // Load flag-based hue suggestions from data file
        std::map<int, float> countryHueHints;
        {
            std::string huePath = m_cfg.dataDir + "/country_hues.json";
            std::ifstream hf(huePath);
            if (hf.is_open()) {
                nlohmann::json hj;
                hf >> hj;
                for (auto& [key, val] : hj.items())
                    countryHueHints[std::stoi(key)] = val.get<float>();
                std::cout << "  Loaded " << countryHueHints.size()
                          << " flag-based hue hints from country_hues.json\n";
            } else {
                std::cout << "  No country_hues.json found (optional, using hash defaults)\n";
            }
        }

        std::vector<int> sortedRids;
        for (auto& [rid, _] : regionNames) {
            if (rid == UNC_RID2 || rid == BLC_RID) continue;
            sortedRids.push_back(rid);
        }
        std::sort(sortedRids.begin(), sortedRids.end(),
            [&](int a, int b) { return adj[a].size() > adj[b].size(); });
        for (int rid : sortedRids) {
            float suggested = countryHueHints.count(rid) ? countryHueHints[rid]
                : fmodf(rid * 0.618033988749895f, 1.0f);
            float bestHue = 0.0f, bestScore = -1e9f;
            for (int ci = 0; ci < 360; ++ci) {
                float h = ci / 360.0f;
                float minDist = 1.0f;
                auto it = adj.find(rid);
                if (it != adj.end()) {
                    for (int nid : it->second) {
                        if (nid == UNC_RID2 || nid == BLC_RID) continue;
                        auto it2 = hueMap.find(nid);
                        if (it2 != hueMap.end()) {
                            float d = fabsf(h - it2->second);
                            if (d > 0.5f) d = 1.0f - d;
                            if (d < minDist) minDist = d;
                        }
                    }
                }
                float hd = fabsf(h - suggested);
                if (hd > 0.5f) hd = 1.0f - hd;
                float score = minDist * 4.0f - hd;
                if (score > bestScore) { bestScore = score; bestHue = h; }
            }
            hueMap[rid] = bestHue;
        }
        hueMap[UNC_RID2] = -1.0f; // UNC = gray (no hue)
        hueMap[BLC_RID] = -1.0f;  // BLC = gray (no hue)
    }
    std::cout << "  Assigned " << (hueMap.size() - 2)
              << " country hues + UNC + BLC (gray)\n";
    if (crimeaRid > 0) {
        int ukrRid = 0;
        for (auto& [rid, name] : regionNames)
            if (name == "Ukraine") { ukrRid = rid; break; }
        if (ukrRid > 0 && hueMap.count(ukrRid))
            hueMap[crimeaRid] = hueMap[ukrRid];
    }

    std::cout << "\n--- Step 5: Find connected components per country ---\n";
    std::vector<int> compId(W * H, 0);
    std::map<int, std::vector<std::vector<int>>> countryComps;
    int nextComp = 1;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int idx = y * W + x;
            if (compId[idx] != 0 || regionPixels[idx] == 0) continue;
            uint32_t rid = regionPixels[idx];
            if (landData[idx * 4 + 3] == 0) continue;
            std::vector<int> stack = {idx};
            compId[idx] = nextComp;
            std::vector<int> pixels;
            while (!stack.empty()) {
                int cur = stack.back(); stack.pop_back();
                pixels.push_back(cur);
                int cx = cur % W, cy = cur / W;
                int nx[4] = {cx + 1, cx - 1, cx, cx};
                int ny[4] = {cy, cy, cy + 1, cy - 1};
                for (int d = 0; d < 4; ++d) {
                    if (nx[d] < 0 || nx[d] >= W || ny[d] < 0 || ny[d] >= H) continue;
                    int nidx = ny[d] * W + nx[d];
                    if (compId[nidx] == 0 && regionPixels[nidx] == rid && landData[nidx * 4 + 3] != 0) {
                        compId[nidx] = nextComp;
                        stack.push_back(nidx);
                    }
                }
            }
            countryComps[rid].push_back(pixels);
            nextComp++;
        }
    }
    std::cout << "  Found components across " << countryComps.size() << " countries\n";

    // Merge small components (islands) into the largest component of the same country.
    // This prevents each tiny island from becoming its own province.
    // Components far from the mainland (> 500px between centroids) are never merged,
    // so remote islands like Hawaii keep their own provinces.
    {
        int mergedComps = 0, mergedPixels = 0;
        for (auto& [rid, components] : countryComps) {
            if (components.size() <= 1) continue;
            if (rid == UNC_RID2 || rid == BLC_RID) continue;
            std::sort(components.begin(), components.end(),
                      [](const auto& a, const auto& b) { return a.size() > b.size(); });
            int largestSize = (int)components[0].size();
            int targetCompId = compId[components[0][0]];
            // Compute centroid of largest (mainland) component
            long long mx = 0, my = 0;
            for (int idx : components[0]) { mx += idx % W; my += idx / W; }
            float cx = (float)mx / largestSize;
            float cy = (float)my / largestSize;
            for (size_t ci = 1; ci < components.size(); ++ci) {
                int sz = (int)components[ci].size();
                // Don't merge islands that are large enough to deserve their own province
                if (sz >= 1000) continue;
                // Don't merge components far from the mainland (e.g. Hawaii, Galapagos)
                long long sx = 0, sy = 0;
                for (int idx : components[ci]) { sx += idx % W; sy += idx / W; }
                float cix = (float)sx / sz, ciy = (float)sy / sz;
                float dist = sqrtf((cix - cx) * (cix - cx) + (ciy - cy) * (ciy - cy));
                if (dist > 500.0f) continue;
                if (sz * 100 < largestSize * 30 && sz < 100000) {
                    for (int idx : components[ci]) {
                        compId[idx] = targetCompId;
                        components[0].push_back(idx);
                    }
                    mergedComps++;
                    mergedPixels += sz;
                    components[ci].clear();
                }
            }
            components.erase(std::remove_if(components.begin(), components.end(),
                [](const auto& v) { return v.empty(); }), components.end());
        }
        std::cout << "  Merged " << mergedComps << " small components (" << mergedPixels << " px)\n";
    }

    std::cout << "\n--- Step 6: Random flood fill subdivision per country ---\n";
    std::vector<uint32_t> provincePixels(W * H, 0);
    nlohmann::json provinceJson;
    int provNextId = 1;
    std::mt19937 rng(noiseSeed);
    std::vector<uint8_t> inCp(W * H, 0);

    int countryCount = 0;
    for (auto& [rid, components] : countryComps) {
        countryCount++;
        std::string countryName = regionNames.count(rid) ? regionNames[rid] : "Country " + std::to_string(rid);
        std::string isoA3 = regionIsoA3.count(rid) ? regionIsoA3[rid] : "";

        for (auto& cp : components) {
            int np = (int)cp.size();
            bool isCrimea = !cp.empty() && crimeaMask[cp[0]];
            std::string provPrefix = isCrimea ? "Crimea" : countryName;

            // Mercator area adjustment: at higher latitudes the same pixel count
            // represents less real area, so scale down the effective pixel count.
            long long sumY = 0, sumX = 0;
            for (int idx : cp) { sumY += idx / W; sumX += idx % W; }
            float avgLat = 90.0f - ((float)sumY / np / H) * 180.0f;
            float avgLon = ((float)sumX / np / W) * 360.0f - 180.0f;
            float mercFactor = cosf(avgLat * (float)M_PI / 180.0f);
            mercFactor = std::max(mercFactor, 0.3f);

            // European countries get extra subdivision for more detail
            bool isEurope = avgLat >= 35.0f && avgLat <= 70.0f && avgLon >= -10.0f && avgLon <= 40.0f;
            float europeMult = isEurope ? 2.0f : 1.0f;

            // Canada has too many provinces due to its large landmass; reduce count
            float canadaMult = (isoA3 == "CAN") ? 0.2f : 1.0f;

            int numProv = std::max(1, (int)std::sqrt(np * mercFactor * europeMult * canadaMult / 250.0f));
            numProv = std::min(numProv, 120);

            if (countryCount <= 30 || countryCount == (int)countryComps.size()) {
                std::cout << "  [" << countryCount << "] \"" << countryName << "\": "
                          << np << " px -> " << numProv << " provinces\n";
            } else if (countryCount == 31) {
                std::cout << "  ... (" << ((int)countryComps.size() - 31) << " more countries)\n";
            }

            if (numProv == 1) {
                int pid = provNextId++;
                uint8_t r = (pid >> 16) & 0xFF;
                uint8_t g = (pid >> 8) & 0xFF;
                uint8_t b = pid & 0xFF;
                char hex[8];
                snprintf(hex, sizeof(hex), "#%02x%02x%02x", r, g, b);
                nlohmann::json entry;
                entry["id"] = pid;
                entry["name"] = provPrefix + " #" + std::to_string(pid);
                entry["country_id"] = rid;
                entry["iso_a3"] = isoA3;
                entry["color"] = std::string(hex);
                provinceJson[std::to_string(pid)] = entry;
                for (int ci : cp) provincePixels[ci] = idToPixel(pid);
                continue;
            }

            float avgArea = (float)np / numProv;
            float minDist = std::sqrt(avgArea * 0.5f);
            float minDistSq = minDist * minDist;

            std::vector<int> shuffled = cp;
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            std::vector<int> seeds;

            for (int idx : shuffled) {
                if ((int)seeds.size() >= numProv) break;
                int cx = idx % W, cy = idx / W;
                float density = 1.0f;
                for (int o = 0; o < 3; ++o) {
                    float nv = fbmNoiseFunc(cx * 0.003f, cy * 0.003f, noiseSeed + o * 137);
                    density += (nv - 0.5f) * 0.5f;
                }
                density = std::max(0.4f, std::min(1.6f, density));
                float adjMinDistSq = minDistSq / (density * density);
                bool tooClose = false;
                for (int s : seeds) {
                    int sx = s % W, sy = s / W;
                    int dx = cx - sx, dy = cy - sy;
                    if (dx * dx + dy * dy < adjMinDistSq) { tooClose = true; break; }
                }
                if (!tooClose) seeds.push_back(idx);
            }
            if ((int)seeds.size() < numProv) {
                for (int idx : shuffled) {
                    if ((int)seeds.size() >= numProv) break;
                    bool already = false;
                    for (int s : seeds) if (s == idx) { already = true; break; }
                    if (!already) seeds.push_back(idx);
                }
            }

            std::vector<int> seedProvIds(seeds.size());
            for (int si = 0; si < (int)seeds.size(); ++si) {
                int pid = provNextId++;
                seedProvIds[si] = pid;
                std::string provName = provPrefix + " #" + std::to_string(pid);
                uint8_t r = (pid >> 16) & 0xFF;
                uint8_t g = (pid >> 8) & 0xFF;
                uint8_t b = pid & 0xFF;
                char hex[8];
                snprintf(hex, sizeof(hex), "#%02x%02x%02x", r, g, b);
                nlohmann::json entry;
                entry["id"] = pid;
                entry["name"] = provName;
                entry["country_id"] = rid;
                entry["iso_a3"] = isoA3;
                entry["color"] = std::string(hex);
                provinceJson[std::to_string(pid)] = entry;
                provincePixels[seeds[si]] = idToPixel(pid);
            }

            std::vector<int> frontier;
            std::vector<bool> inFrontier(W * H, false);
            for (int si = 0; si < (int)seeds.size(); ++si) {
                int sx = seeds[si] % W, sy = seeds[si] / W;
                for (int d = 0; d < 4; ++d) {
                    int nx = sx + (d == 0) - (d == 1);
                    int ny = sy + (d == 2) - (d == 3);
                    if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                    int nidx = ny * W + nx;
                    if (provincePixels[nidx] == 0 && landData[nidx * 4 + 3] != 0
                        && regionPixels[nidx] == rid && !inFrontier[nidx]) {
                        frontier.push_back(nidx);
                        inFrontier[nidx] = true;
                    }
                }
            }

            while (!frontier.empty()) {
                int ri = rng() % (int)frontier.size();
                int idx = frontier[ri];
                std::swap(frontier[ri], frontier.back());
                frontier.pop_back();
                inFrontier[idx] = false;

                if (provincePixels[idx] != 0) continue;
                int cx = idx % W, cy = idx / W;
                uint32_t owner = 0;
                for (int d = 0; d < 4; ++d) {
                    int nx = (cx + (d == 0) - (d == 1) + W) % W;
                    int ny = cy + (d == 2) - (d == 3);
                    if (ny < 0 || ny >= H) continue;
                    int nidx = ny * W + nx;
                    if (regionPixels[nidx] != rid) continue;
                    uint32_t nv = provincePixels[nidx];
                    if (nv != 0) { owner = nv; break; }
                }
                if (owner == 0) continue;
                provincePixels[idx] = owner;
                for (int d = 0; d < 4; ++d) {
                    int nx = (cx + (d == 0) - (d == 1) + W) % W;
                    int ny = cy + (d == 2) - (d == 3);
                    if (ny < 0 || ny >= H) continue;
                    int nidx = ny * W + nx;
                    if (provincePixels[nidx] == 0 && landData[nidx * 4 + 3] != 0
                        && regionPixels[nidx] == rid && !inFrontier[nidx]) {
                        frontier.push_back(nidx);
                        inFrontier[nidx] = true;
                    }
                }
            }
            // Fill any remaining unassigned pixels in this component
            // (flood fill couldn't reach fragments disconnected by water gaps)
            // Uses expanding search within the component's pixel set only.
            {
                for (int idx : cp) inCp[idx] = 1;
                for (int idx : cp) {
                    if (provincePixels[idx] != 0) continue;
                    int cx = idx % W, cy = idx / W;
                    for (int r = 1; r <= 500; ++r) {
                        bool found = false;
                        // Top/bottom rows
                        for (int dx = -r; dx <= r && !found; ++dx) {
                            int nx = cx + dx;
                            if (nx < 0 || nx >= W) continue;
                            int ny = cy - r;
                            if (ny >= 0) {
                                int nidx = ny * W + nx;
                                if (inCp[nidx] && provincePixels[nidx] != 0) {
                                    provincePixels[idx] = provincePixels[nidx];
                                    found = true;
                                }
                            }
                            if (!found) {
                                ny = cy + r;
                                if (ny < H) {
                                    int nidx = ny * W + nx;
                                    if (inCp[nidx] && provincePixels[nidx] != 0) {
                                        provincePixels[idx] = provincePixels[nidx];
                                        found = true;
                                    }
                                }
                            }
                        }
                        // Left/right columns (excluding corners)
                        for (int dy = -(r - 1); dy <= r - 1 && !found; ++dy) {
                            int ny = cy + dy;
                            if (ny < 0 || ny >= H) continue;
                            int nx = cx - r;
                            if (nx >= 0) {
                                int nidx = ny * W + nx;
                                if (inCp[nidx] && provincePixels[nidx] != 0) {
                                    provincePixels[idx] = provincePixels[nidx];
                                    found = true;
                                }
                            }
                            if (!found) {
                                nx = cx + r;
                                if (nx < W) {
                                    int nidx = ny * W + nx;
                                    if (inCp[nidx] && provincePixels[nidx] != 0) {
                                        provincePixels[idx] = provincePixels[nidx];
                                        found = true;
                                    }
                                }
                            }
                        }
                        if (found) break;
                    }
                }
                for (int idx : cp) inCp[idx] = 0;
            }
        }
    }

    // Step 6.5: Merge tiny provinces into neighboring provinces
    // This merges disconnected island components (< MIN_PROV_PIXELS) into
    // the nearest province of the same country to prevent each tiny island
    // from becoming its own unclickable province.
    {
        const int MIN_PROV_PIXELS = 80;
        std::map<int, int> provSizes;
        for (int i = 0; i < W * H; ++i) {
            if (provincePixels[i] == 0) continue;
            int pid = pixelToId(provincePixels[i]);
            provSizes[pid]++;
        }
        // Count provinces per country so we never merge the last one
        std::map<int, std::set<int>> ridProvinces;
        for (int i = 0; i < W * H; ++i) {
            if (provincePixels[i] == 0) continue;
            int pid = pixelToId(provincePixels[i]);
            int rid = (int)regionPixels[i];
            ridProvinces[rid].insert(pid);
        }
        std::set<int> tinyProvs;
        for (auto& [pid, sz] : provSizes)
            if (sz < MIN_PROV_PIXELS) tinyProvs.insert(pid);
        std::cout << "\n--- Step 6.5: Merge small provinces ---\n";
        if (!tinyProvs.empty()) {
            int mergedPixels = 0;
            int skipped = 0;
            for (int pid : tinyProvs) {
                std::vector<int> pixs;
                for (int i = 0; i < W * H; ++i) {
                    if (provincePixels[i] == 0) continue;
                    if (pixelToId(provincePixels[i]) == pid)
                        pixs.push_back(i);
                }
                if (pixs.empty()) continue;
                int myRid = (int)regionPixels[pixs[0]];
                // Don't merge the last province of a country
                if ((int)ridProvinces[myRid].size() <= 1) {
                    skipped++;
                    continue;
                }
                // Search for a same-country neighbor, expanding radius until found
                int bestNbr = 0;
                std::vector<int> radii = {1, 2, 3, 5, 10, 20, 50, 100, 200};
                for (int radius : radii) {
                    std::map<int, int> votes;
                    for (int idx : pixs) {
                        int x = idx % W, y = idx / W;
                        int x0 = x - radius, x1 = x + radius;
                        int y0 = y - radius, y1 = y + radius;
                        for (int ny = y0; ny <= y1; ++ny) {
                            if (ny < 0 || ny >= H) continue;
                            for (int nx = x0; nx <= x1; ++nx) {
                                if (nx < 0 || nx >= W) continue;
                                int nidx = ny * W + nx;
                                if ((int)regionPixels[nidx] != myRid) continue;
                                if (provincePixels[nidx] == 0) continue;
                                int npid = pixelToId(provincePixels[nidx]);
                                if (npid != pid) votes[npid]++;
                            }
                        }
                    }
                    if (!votes.empty()) {
                        // Pick the SMALLEST neighbor (by pixel count) to absorb this tiny province
                        int bestSize = INT_MAX;
                        for (auto& [npid, cnt] : votes) {
                            int sz = provSizes.count(npid) ? provSizes[npid] : 0;
                            if (sz < bestSize) { bestSize = sz; bestNbr = npid; }
                        }
                        break;
                    }
                }
                // If still no same-country neighbor found, try any neighbor with expanded search
                if (bestNbr == 0) {
                    for (int radius : radii) {
                        std::map<int, int> votes;
                        for (int idx : pixs) {
                            int x = idx % W, y = idx / W;
                            int x0 = x - radius, x1 = x + radius;
                            int y0 = y - radius, y1 = y + radius;
                            for (int ny = y0; ny <= y1; ++ny) {
                                if (ny < 0 || ny >= H) continue;
                                for (int nx = x0; nx <= x1; ++nx) {
                                    if (nx < 0 || nx >= W) continue;
                                    int nidx = ny * W + nx;
                                    if (provincePixels[nidx] == 0) continue;
                                    int npid = pixelToId(provincePixels[nidx]);
                                    if (npid != pid) votes[npid]++;
                                }
                            }
                        }
                        if (!votes.empty()) {
                            int bestSize = INT_MAX;
                            for (auto& [npid, cnt] : votes) {
                                int sz = provSizes.count(npid) ? provSizes[npid] : 0;
                                if (sz < bestSize) { bestSize = sz; bestNbr = npid; }
                            }
                            break;
                        }
                    }
                }
                if (bestNbr > 0) {
                    for (int idx : pixs)
                        provincePixels[idx] = idToPixel(bestNbr);
                    mergedPixels += (int)pixs.size();
                }
            }
            for (int pid : tinyProvs)
                provinceJson.erase(std::to_string(pid));
            std::cout << "  Merged " << (tinyProvs.size() - skipped) << " tiny provinces ("
                      << mergedPixels << " pixels), kept " << skipped << " as last of country\n";
        } else {
            std::cout << "  No tiny provinces to merge\n";
        }
    }

    std::cout << "\n--- Step 7: Organic smoothing ---\n";
    for (int relaxIter = 0; relaxIter < m_cfg.voronoiRelaxIterations; ++relaxIter) {
        std::vector<uint32_t> newPixels = provincePixels;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                int idx = y * W + x;
                uint32_t cur = provincePixels[idx];
                if (cur == 0) continue;
                int curId = pixelToId(cur);
                std::unordered_map<int, int> counts;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        int nx = (x + dx + W) % W;
                        int ny = y + dy;
                        if (ny < 0 || ny >= H) continue;
                        int nidx = ny * W + nx;
                        if (regionPixels[nidx] != regionPixels[idx]) continue;
                        uint32_t npx = provincePixels[nidx];
                        if (npx == 0) continue;
                        counts[pixelToId(npx)]++;
                    }
                }
                int maxCount = 0, maxId = curId;
                for (auto& [pid, c] : counts) {
                    if (c > maxCount) { maxCount = c; maxId = pid; }
                }
                if (maxId != curId && maxCount >= 6) {
                    newPixels[idx] = idToPixel(maxId);
                }
            }
        }
        provincePixels.swap(newPixels);
        std::cout << "    Smooth pass " << (relaxIter + 1) << " done\n";
    }

    // Final seam-fix: force left/right edges to match for seamless wrap
    {
        const int MARGIN2 = 5;
        for (int y = 0; y < H; ++y) {
            for (int m = 0; m < MARGIN2; ++m) {
                provincePixels[y * W + m] = provincePixels[y * W + (W - 1 - m)];
                provincePixels[y * W + (W - 1 - m)] = provincePixels[y * W + m];
            }
        }
    }

    // ── Re-merge Crimea into Ukraine ──
    if (crimeaRid > 0) {
        std::cout << "  Re-merging Crimea rid " << crimeaRid << " into Ukraine\n";
        int ukrRid = 0;
        for (auto& [rid, name] : regionNames)
            if (name == "Ukraine") { ukrRid = rid; break; }
        if (ukrRid > 0) {
            // Patch province country_ids
            for (auto& [pidStr, entry] : provinceJson.items()) {
                if (entry["country_id"] == crimeaRid) {
                    entry["country_id"] = ukrRid;
                    // Rename province from "Crimea #..." to "Crimea #..." (keep name)
                }
            }
            // Re-merge regionPixels so political map shows correct colors
            for (int i = 0; i < W * H; ++i)
                if (regionPixels[i] == crimeaRid) regionPixels[i] = ukrRid;
            // Remove Crimea from region maps
            regionNames.erase(crimeaRid);
            regionIsoA3.erase(crimeaRid);
            regionTypes.erase(crimeaRid);
        }
    }

    std::cout << "\n--- Step 8: Saving ---\n";
    if (!saveProvinceFiles(provincePixels, provinceJson, "", "Provinces"))
        return false;

    // Build and save countries.json (with graph-colored hues, bright base values)
    std::map<int, uint32_t> countryColors;
    auto hsvToRgb = [](float h, float s, float v) -> uint32_t {
        int hi = (int)(h * 6.0f);
        float f = h * 6.0f - hi;
        float p = v * (1.0f - s);
        float q = v * (1.0f - f * s);
        float t = v * (1.0f - (1.0f - f) * s);
        float r, g, b;
        switch (hi % 6) {
            case 0: r=v; g=t; b=p; break;
            case 1: r=q; g=v; b=p; break;
            case 2: r=p; g=v; b=t; break;
            case 3: r=p; g=q; b=v; break;
            case 4: r=t; g=p; b=v; break;
            default: r=v; g=p; b=q; break;
        }
        return ((uint32_t)255 << 24) | ((uint32_t)(b*255) << 16) |
               ((uint32_t)(g*255) << 8) | (uint32_t)(r*255);
    };
    for (auto& [rid, _] : regionNames) {
        float h = hueMap[rid];
        if (h < 0.0f) {
            uint8_t g = 140;
            countryColors[rid] = (255u << 24) | (g << 16) | (g << 8) | g;
        } else {
            countryColors[rid] = hsvToRgb(h, 0.55f, 0.72f);
        }
    }

    std::cout << "  Assigned " << countryColors.size() << " country colors\n";

    nlohmann::json countriesJson;

    // Build real flag database at runtime
    auto getRealFlag = [](const std::string& iso) -> nlohmann::json {
        auto col = [](const char* c1) { return std::string(c1); };
        auto cols = [&](std::initializer_list<const char*> list) {
            nlohmann::json arr = nlohmann::json::array();
            for (auto c : list) arr.push_back(c);
            return arr;
        };
        auto make = [&](const std::string& type, const nlohmann::json& colors, int stars = 0) -> nlohmann::json {
            nlohmann::json f;
            f["type"] = type;
            f["colors"] = colors;
            if (stars > 0) f["starCount"] = stars;
            return f;
        };
        auto S = [&](const std::string& type, std::initializer_list<const char*> colors,
                     float x = 0.5, float y = 0.5, float size = 0.3,
                     int count = 0, const std::string& text = "") -> nlohmann::json {
            nlohmann::json sym;
            sym["type"] = type;
            sym["x"] = x; sym["y"] = y; sym["size"] = size;
            if (count > 0) sym["count"] = count;
            if (!text.empty()) sym["text"] = text;
            auto arr = nlohmann::json::array();
            for (auto c : colors) arr.push_back(std::string(c));
            sym["colors"] = arr;
            return sym;
        };
        auto addSym = [&](nlohmann::json& f, const nlohmann::json& sym) {
            if (!f.contains("symbols")) f["symbols"] = nlohmann::json::array();
            f["symbols"].push_back(sym);
        };

        if (iso == "FRA") return make("vstripes", cols({"#0055a4", "#ffffff", "#ef4135"}));
        if (iso == "NLD") return make("hstripes", cols({"#ae1c28", "#ffffff", "#21468b"}));
        if (iso == "DEU") return make("hstripes", cols({"#000000", "#dd0000", "#ffce00"}));
        if (iso == "RUS") return make("hstripes", cols({"#ffffff", "#0039a6", "#d52b1e"}));
        if (iso == "ITA") return make("vstripes", cols({"#009246", "#ffffff", "#ce2b37"}));
        if (iso == "GBR") { auto f = make("hstripes", cols({"#012169", "#ffffff", "#c8102e"})); addSym(f, S("star", {"#ffffff"}, 0.25, 0.5, 0.15)); addSym(f, S("star", {"#ffffff"}, 0.75, 0.5, 0.15)); return f; }
        if (iso == "JPN") return make("circle", cols({"#ffffff", "#bc002d"}));
        if (iso == "CHN") { auto f = make("solid", cols({"#de2910"})); addSym(f, S("star", {"#ffde00"}, 0.3, 0.35, 0.15)); addSym(f, S("star", {"#ffde00"}, 0.5, 0.25, 0.06)); addSym(f, S("star", {"#ffde00"}, 0.62, 0.3, 0.06)); addSym(f, S("star", {"#ffde00"}, 0.68, 0.45, 0.06)); addSym(f, S("star", {"#ffde00"}, 0.62, 0.6, 0.06)); return f; }
        if (iso == "POL") return make("hstripes", cols({"#ffffff", "#dc143c"}));
        if (iso == "AUT") return make("hstripes", cols({"#ed2939", "#ffffff", "#ed2939"}));
        if (iso == "UKR") return make("hstripes", cols({"#0057b7", "#ffdd00"}));
        if (iso == "SWE") return make("cross", cols({"#005baa", "#fecc00"}));
        if (iso == "DNK") return make("cross", cols({"#c60c30", "#ffffff"}));
        if (iso == "FIN") return make("cross", cols({"#ffffff", "#003580"}));
        if (iso == "NOR") return make("cross", cols({"#ba0c2f", "#ffffff"}));
        if (iso == "ISL") return make("cross", cols({"#003897", "#ffffff"}));
        if (iso == "CHE") return make("cross", cols({"#ff0000", "#ffffff"}));
        if (iso == "TUR") { auto f = make("solid", cols({"#e30a17"})); addSym(f, S("star", {"#ffffff"}, 0.45, 0.5, 0.15)); addSym(f, S("circle", {"#ffffff"}, 0.38, 0.47, 0.04)); return f; }
        if (iso == "VNM") { auto f = make("solid", cols({"#da251d"})); addSym(f, S("star", {"#ffff00"}, 0.5, 0.5, 0.2)); return f; }
        if (iso == "BEL") return make("vstripes", cols({"#000000", "#fdff00", "#ff0000"}));
        if (iso == "IRL") return make("vstripes", cols({"#169b62", "#ffffff", "#ff883e"}));
        if (iso == "ESP") { auto f = make("hstripes", cols({"#c60b1e", "#ffc400", "#c60b1e"})); addSym(f, S("coat_arms", {"#ffc400", "#c60b1e", "#7b3f00"}, 0.45, 0.4, 0.2)); return f; }
        if (iso == "PRT") { auto f = make("hstripes", cols({"#006600", "#ff0000"})); addSym(f, S("coat_arms", {"#006600", "#ff0000", "#ffce00"}, 0.5, 0.5, 0.25)); return f; }
        if (iso == "GRC") return make("cross", cols({"#0d5eaf", "#ffffff"}));
        if (iso == "HUN") return make("hstripes", cols({"#ce2b37", "#ffffff", "#436f4d"}));
        if (iso == "BGR") return make("hstripes", cols({"#ffffff", "#00966e", "#d62612"}));
        if (iso == "SRB") return make("hstripes", cols({"#c6363c", "#ffffff", "#0c4076"}));
        if (iso == "HRV") return make("hstripes", cols({"#ff0000", "#ffffff", "#0093dd"}));
        if (iso == "SVN") return make("hstripes", cols({"#ffffff", "#0000ff", "#ff0000"}));
        if (iso == "SVK") { auto f = make("hstripes", cols({"#ffffff", "#0000ff", "#ff0000"})); addSym(f, S("coat_arms", {"#ff0000", "#0000ff", "#ffffff"}, 0.25, 0.5, 0.25)); return f; }
        if (iso == "ROU") return make("vstripes", cols({"#00319c", "#ffde00", "#de2110"}));
        if (iso == "MDA") return make("vstripes", cols({"#00319c", "#ffde00", "#de2110"}));
        if (iso == "LTU") return make("hstripes", cols({"#ffce00", "#006a44", "#c1272d"}));
        if (iso == "LVA") return make("hstripes", cols({"#9e3039", "#ffffff", "#9e3039"}));
        if (iso == "EST") return make("hstripes", cols({"#0072ce", "#000000", "#ffffff"}));
        if (iso == "CZE") return make("hstripes", cols({"#ffffff", "#d7141a", "#11457e"}));
        if (iso == "ARG") return make("hstripes", cols({"#75aadb", "#ffffff", "#75aadb"}));
        if (iso == "BRA") { auto f = make("solid", cols({"#009c3b"})); addSym(f, S("circle", {"#ffdf00"}, 0.5, 0.5, 0.3)); addSym(f, S("circle", {"#002776"}, 0.5, 0.55, 0.2)); addSym(f, S("star", {"#ffffff"}, 0.5, 0.55, 0.08)); return f; }
        if (iso == "MEX") { auto f = make("vstripes", cols({"#006341", "#ffffff", "#ce1126"})); addSym(f, S("coat_arms", {"#006341", "#ce1126", "#7b3f00"}, 0.5, 0.5, 0.2)); return f; }
        if (iso == "CAN") return make("vstripes", cols({"#ff0000", "#ffffff", "#ff0000"}));
        if (iso == "USA") { auto f = make("hstripes", cols({"#b22234", "#ffffff"})); addSym(f, S("circle", {"#3c3b6e"}, 0.15, 0.3, 0.1)); return f; }
        if (iso == "AUS") { auto f = make("star", cols({"#00008b", "#ffffff"}), 1); addSym(f, S("star", {"#ffffff"}, 0.5, 0.5, 0.1)); addSym(f, S("star", {"#ffffff"}, 0.7, 0.3, 0.06)); addSym(f, S("star", {"#ffffff"}, 0.8, 0.45, 0.06)); addSym(f, S("star", {"#ffffff"}, 0.75, 0.65, 0.06)); return f; }
        if (iso == "NZL") { auto f = make("solid", cols({"#00247d"})); addSym(f, S("star", {"#cc142b"}, 0.35, 0.4, 0.12)); addSym(f, S("star", {"#ffffff"}, 0.5, 0.4, 0.1)); addSym(f, S("star", {"#ffffff"}, 0.7, 0.3, 0.07)); addSym(f, S("star", {"#ffffff"}, 0.78, 0.5, 0.07)); addSym(f, S("star", {"#ffffff"}, 0.62, 0.6, 0.07)); return f; }
        if (iso == "ZAF") return make("vstripes", cols({"#de3831", "#ffffff", "#002395"}));
        if (iso == "EGY") { auto f = make("hstripes", cols({"#ce1126", "#ffffff", "#000000"})); addSym(f, S("coat_arms", {"#000000", "#ce1126", "#c7a600"}, 0.5, 0.5, 0.2)); return f; }
        if (iso == "SAU") { auto f = make("solid", cols({"#006c35"})); addSym(f, S("text", {"#ffffff"}, 0.5, 0.5, 0.3, 0, "لا إله إلا الله محمد رسول الله")); return f; }
        if (iso == "IRN") { auto f = make("hstripes", cols({"#239f40", "#ffffff", "#da0000"})); addSym(f, S("text", {"#da0000"}, 0.5, 0.08, 0.08, 0, "الله أكبر")); addSym(f, S("text", {"#da0000"}, 0.5, 0.92, 0.08, 0, "الله أكبر")); addSym(f, S("coat_arms", {"#da0000"}, 0.5, 0.5, 0.15)); return f; }
        if (iso == "IRQ") { auto f = make("hstripes", cols({"#ce1126", "#ffffff", "#000000"})); addSym(f, S("text", {"#239f40"}, 0.5, 0.5, 0.2, 0, "الله أكبر")); return f; }
        if (iso == "SYR") return make("hstripes", cols({"#ce1126", "#ffffff", "#000000"}));
        if (iso == "YEM") return make("hstripes", cols({"#ce1126", "#ffffff", "#000000"}));
        if (iso == "SDN") return make("hstripes", cols({"#d21034", "#ffffff", "#000000"}));
        if (iso == "LBY") return make("hstripes", cols({"#e70013", "#000000", "#239e46"}));
        if (iso == "TUN") { auto f = make("solid", cols({"#e70013"})); addSym(f, S("circle", {"#ffffff"}, 0.5, 0.5, 0.25)); addSym(f, S("star", {"#e70013"}, 0.45, 0.5, 0.12)); return f; }
        if (iso == "DZA") { auto f = make("vstripes", cols({"#007229", "#ffffff"})); addSym(f, S("star", {"#d21034"}, 0.45, 0.5, 0.12)); addSym(f, S("circle", {"#d21034"}, 0.4, 0.45, 0.04)); return f; }
        if (iso == "MAR") { auto f = make("solid", cols({"#c1272d"})); addSym(f, S("star", {"#006233"}, 0.5, 0.5, 0.2)); return f; }
        if (iso == "KEN") return make("hstripes", cols({"#000000", "#ff0000", "#00ff00"}));
        if (iso == "NGA") return make("vstripes", cols({"#008751", "#ffffff", "#008751"}));
        if (iso == "GHA") return make("hstripes", cols({"#ce1126", "#ffce00", "#006b3f"}));
        if (iso == "ETH") return make("hstripes", cols({"#009a44", "#ffce00", "#da121a"}));
        if (iso == "CMR") return make("vstripes", cols({"#007a5e", "#ce1126", "#ffce00"}));
        if (iso == "CIV") return make("vstripes", cols({"#f77f00", "#ffffff", "#009e60"}));
        if (iso == "MLI") return make("vstripes", cols({"#14b53a", "#fcd116", "#ce1126"}));
        if (iso == "SEN") return make("vstripes", cols({"#00853f", "#fdef42", "#e31b23"}));
        if (iso == "THA") return make("hstripes", cols({"#ed1c24", "#ffffff", "#241d4f", "#ffffff", "#ed1c24"}));
        if (iso == "IDN") return make("hstripes", cols({"#ff0000", "#ffffff"}));
        if (iso == "MYS") return make("hstripes", cols({"#ff0000", "#ffffff", "#0000cc"}));
        if (iso == "PHL") return make("hstripes", cols({"#0038a8", "#ce1126", "#ffffff"}));
        if (iso == "ARE") return make("hstripes", cols({"#009e00", "#ffffff", "#000000", "#ff0000"}));
        if (iso == "ISR") { auto f = make("solid", cols({"#ffffff"})); addSym(f, S("star", {"#0038b8"}, 0.5, 0.5, 0.22)); return f; }
        if (iso == "PRK") { auto f = make("hstripes", cols({"#024fa2", "#ffffff", "#ed1c24", "#ffffff", "#024fa2"})); addSym(f, S("star", {"#ed1c24"}, 0.25, 0.5, 0.12)); return f; }
        if (iso == "MNG") return make("vstripes", cols({"#da2032", "#0066b4", "#da2032"}));
        if (iso == "KAZ") return make("solid", cols({"#00afca"}));
        if (iso == "AFG") { auto f = make("vstripes", cols({"#000000", "#d90000", "#007a36"})); addSym(f, S("coat_arms", {"#ffffff", "#000000", "#d90000"}, 0.5, 0.5, 0.25)); return f; }
        if (iso == "PAK") return make("vstripes", cols({"#01411c", "#ffffff"}));
        if (iso == "IND") { auto f = make("vstripes", cols({"#ff9933", "#ffffff", "#138808"})); addSym(f, S("circle", {"#000080"}, 0.5, 0.5, 0.15)); addSym(f, S("circle", {"#ffffff"}, 0.5, 0.5, 0.12)); return f; }
        if (iso == "NPL") return make("solid", cols({"#dc143c"}));
        if (iso == "MMR") { auto f = make("hstripes", cols({"#fecb00", "#34b233", "#ea2839"})); addSym(f, S("star", {"#ffffff"}, 0.5, 0.5, 0.15)); return f; }
        if (iso == "KHM") { auto f = make("vstripes", cols({"#032ea1", "#e00025", "#032ea1"})); addSym(f, S("coat_arms", {"#e00025", "#032ea1", "#ffce00"}, 0.5, 0.5, 0.22)); return f; }
        if (iso == "LAO") { auto f = make("hstripes", cols({"#ce1126", "#002868", "#ce1126"})); addSym(f, S("circle", {"#ffffff"}, 0.5, 0.5, 0.2)); return f; }
        if (iso == "TWN") { auto f = make("solid", cols({"#fe0000"})); addSym(f, S("circle", {"#000095"}, 0.35, 0.35, 0.15)); addSym(f, S("circle", {"#ffffff"}, 0.35, 0.35, 0.12)); addSym(f, S("star", {"#ffffff"}, 0.5, 0.65, 0.04, 1)); return f; }
        if (iso == "CUB") return make("hstripes", cols({"#002a8f", "#ffffff", "#cf142b"}));
        if (iso == "CHL") return make("hstripes", cols({"#ffffff", "#0039a6", "#da291c"}));
        if (iso == "COL") return make("hstripes", cols({"#fcd116", "#003893", "#ce1126"}));
        if (iso == "PER") { auto f = make("vstripes", cols({"#d91023", "#ffffff", "#d91023"})); addSym(f, S("coat_arms", {"#d91023", "#ffffff", "#7b3f00"}, 0.5, 0.5, 0.2)); return f; }
        if (iso == "VEN") return make("hstripes", cols({"#cf142b", "#00247d", "#ffffff"}));
        if (iso == "URY") return make("hstripes", cols({"#ffffff", "#0038a8", "#ffffff", "#0038a8", "#ffffff"}));
        if (iso == "PRY") return make("hstripes", cols({"#d52b1e", "#ffffff", "#0038a8"}));
        if (iso == "ECU") return make("hstripes", cols({"#fcd116", "#003893", "#ce1126"}));
        if (iso == "BOL") return make("hstripes", cols({"#d52b1e", "#ffce00", "#007a3d"}));
        if (iso == "GUY") return make("vstripes", cols({"#009e49", "#ffffff", "#ce1126"}));
        if (iso == "SUR") return make("hstripes", cols({"#377e3f", "#ffffff", "#b40a2d"}));
        if (iso == "DNR") return make("hstripes", cols({"#000000", "#0057b8", "#cf2e2e"}));
        if (iso == "LNR") return make("hstripes", cols({"#66ccff", "#0039a6", "#d52b1e"}));
        if (iso == "XCR") return make("hstripes", cols({"#0039a6", "#ffffff", "#d52b1e"}));
        // For countries with iso="-99" in Natural Earth, fall back to name match
        return nlohmann::json(); // null json = not found
    };

    // Name-based fallback for countries with iso="-99"
    auto getRealFlagByName = [&](const std::string& name) -> nlohmann::json {
        auto cols = [&](std::initializer_list<const char*> list) {
            nlohmann::json arr = nlohmann::json::array();
            for (auto c : list) arr.push_back(std::string(c));
            return arr;
        };
        auto make = [&](const std::string& type, const nlohmann::json& colors, int stars = 0) -> nlohmann::json {
            nlohmann::json f;
            f["type"] = type;
            f["colors"] = colors;
            if (stars > 0) f["starCount"] = stars;
            return f;
        };
        if (name == "France") return make("vstripes", cols({"#0055a4", "#ffffff", "#ef4135"}));
        if (name == "Norway") return make("cross", cols({"#ba0c2f", "#ffffff"}));
        if (name == "Kosovo") return make("star", cols({"#244aa5", "#d0a650"}), 1);
        if (name == "Somaliland") return make("hstripes", cols({"#000000", "#ffffff", "#8b0000", "#008000"}), 1);
        if (name == "N. Cyprus") return make("cross", cols({"#ffffff", "#e30a17"}));
        return nlohmann::json();
    };

    auto toGrayscale = [](const std::string& hex) -> std::string {
        unsigned int r, g, b;
        sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
        int gray = (r * 77 + g * 151 + b * 28) / 256;
        char buf[8]; snprintf(buf, sizeof(buf), "#%02x%02x%02x", gray, gray, gray);
        return std::string(buf);
    };

    auto genFlag = [&](const std::string& iso, const std::string& name, int r, int g, int b, int seed, bool censored) -> nlohmann::json {
        // Check real flag database first by ISO, then by name
        nlohmann::json real = getRealFlag(iso);
        if (real.is_null()) real = getRealFlagByName(name);
        if (!real.is_null()) {
            if (censored) real["censored"] = true;
            return real;
        }

        int pattern = seed % 6;
        auto h = [](int rr, int gg, int bb) {
            char buf[8]; snprintf(buf, sizeof(buf), "#%02x%02x%02x", rr, gg, bb);
            return std::string(buf);
        };
        int r2 = std::min(r + 60, 255), g2 = std::min(g + 60, 255), b2 = std::min(b + 60, 255);
        int r3 = std::max(r - 40, 0), g3 = std::max(g - 40, 0), b3 = std::max(b - 40, 0);

        nlohmann::json f;
        switch (pattern) {
            case 0: // SOLID
                f["type"] = "solid";
                f["colors"] = {h(r, g, b)};
                break;
            case 1: // HSTRIPES
                f["type"] = "hstripes";
                f["colors"] = {h(r2, g2, b2), h(r, g, b), h(r3, g3, b3)};
                break;
            case 2: // VSTRIPES
                f["type"] = "vstripes";
                f["colors"] = {h(r2, g2, b2), h(r, g, b), h(r3, g3, b3)};
                break;
            case 3: // STAR
                f["type"] = "star";
                f["colors"] = {h(r, g, b), "#ffffff"};
                f["starCount"] = (seed % 3) + 1;
                break;
            case 4: // CROSS
                f["type"] = "cross";
                f["colors"] = {h(r, g, b), "#ffffff"};
                break;
            case 5: // DIAGONAL
                f["type"] = "diagonal";
                f["colors"] = {h(r, g, b), h(r2, g2, b2)};
                break;
        }
        if (censored) f["censored"] = true;
        return f;
    };
    std::string flagDir = m_cfg.dataDir + "/flags";
    for (auto& [rid, name] : regionNames) {
        nlohmann::json entry;
        entry["id"] = rid;
        entry["name"] = name;
        std::string iso = regionIsoA3.count(rid) ? regionIsoA3[rid] : "";
        // Fix Natural Earth returning "-99" for known countries
        if (iso == "-99") {
            auto& overrides = getIsoOverrides();
            auto it = overrides.find(name);
            if (it != overrides.end()) iso = it->second;
        }
        entry["iso_a3"] = iso;
        uint32_t col = countryColors[rid];
        char hex[8];
        snprintf(hex, sizeof(hex), "#%02x%02x%02x",
                 (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
        entry["color"] = std::string(hex);
        // Check for actual flag image file
        std::string imgPath = flagDir + "/" + iso + ".png";
        std::string imgName = "flags/" + iso + ".png";
        bool hasImage = !iso.empty() && fs::exists(imgPath);
        if (hasImage) {
            entry["flag_actual"] = nlohmann::json{{"image", imgName}};
            entry["flag_censored"] = nlohmann::json{{"image", imgName}};
            // Countries with hate symbols (communist star, AK-47, etc.) get pixelated in censored mode
            std::set<std::string> hateIsos = {"PRK", "CHN", "VNM", "LAO", "AGO", "MOZ"};
            if (hateIsos.count(iso)) {
                entry["flag_censored"]["censored"] = true;
            }
        } else {
            entry["flag_actual"] = genFlag(iso, name, (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, rid, false);
            entry["flag_censored"] = genFlag(iso, name, (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, rid, true);
        }
        countriesJson[std::to_string(rid)] = entry;
    }
    std::string countriesPath = m_cfg.dataDir + "/countries.json";
    std::ofstream countriesFile(countriesPath);
    countriesFile << countriesJson.dump(2);
    countriesFile.close();
    std::cout << "  Saved " << countriesPath << " (" << countriesJson.size() << " countries)\n";

    // Generate political.png with border-distance gradient (duller center, brighter border)
    std::cout << "  Computing political map with border gradient...\n";
    {
        // Use scaled integers for Euclidean distance (scale 100 to handle sqrt(2))
        const uint16_t INF = 65535;
        std::vector<uint16_t> dist(W * H, INF);
        for (int i = 0; i < W * H; ++i) {
            if (landData[i * 4 + 3] == 0) continue;
            int rid = (int)regionPixels[i];
            int x = i % W, y = i / W;
            int leftX = (x == 0) ? W - 1 : x - 1;
            int rightX = (x == W - 1) ? 0 : x + 1;
            int leftIdx = y * W + leftX;
            int rightIdx = y * W + rightX;
            bool isBorder = (int)regionPixels[leftIdx] != rid ||
                            (int)regionPixels[rightIdx] != rid ||
                            (y > 0 && (int)regionPixels[i - W] != rid) ||
                            (y < H - 1 && (int)regionPixels[i + W] != rid);
            if (isBorder) dist[i] = 0;
        }
        auto minU16 = [](uint16_t a, uint16_t b) { return a < b ? a : b; };
        // Forward 8-point pass (top-left to bottom-right)
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                int i = y * W + x;
                if (landData[i * 4 + 3] == 0) continue;
                uint32_t rid = regionPixels[i];
                int leftX = (x == 0) ? W - 1 : x - 1;
                int leftI = y * W + leftX;
                if (regionPixels[leftI] == rid)
                    dist[i] = minU16(dist[i], dist[leftI] + 100);
                if (y > 0 && regionPixels[i - W] == rid)
                    dist[i] = minU16(dist[i], dist[i - W] + 100);
                if (y > 0) {
                    int tlX = (x == 0) ? W - 1 : x - 1;
                    int tlI = (y - 1) * W + tlX;
                    if (regionPixels[tlI] == rid)
                        dist[i] = minU16(dist[i], dist[tlI] + 141);
                }
                if (y > 0) {
                    int trX = (x == W - 1) ? 0 : x + 1;
                    int trI = (y - 1) * W + trX;
                    if (regionPixels[trI] == rid)
                        dist[i] = minU16(dist[i], dist[trI] + 141);
                }
            }
        }
        // Backward 8-point pass (bottom-right to top-left)
        for (int y = H - 1; y >= 0; --y) {
            for (int x = W - 1; x >= 0; --x) {
                int i = y * W + x;
                if (landData[i * 4 + 3] == 0) continue;
                uint32_t rid = regionPixels[i];
                int rightX = (x == W - 1) ? 0 : x + 1;
                int rightI = y * W + rightX;
                if (regionPixels[rightI] == rid)
                    dist[i] = minU16(dist[i], dist[rightI] + 100);
                if (y < H - 1 && regionPixels[i + W] == rid)
                    dist[i] = minU16(dist[i], dist[i + W] + 100);
                if (y < H - 1) {
                    int brX = (x == W - 1) ? 0 : x + 1;
                    int brI = (y + 1) * W + brX;
                    if (regionPixels[brI] == rid)
                        dist[i] = minU16(dist[i], dist[brI] + 141);
                }
                if (y < H - 1) {
                    int blX = (x == 0) ? W - 1 : x - 1;
                    int blI = (y + 1) * W + blX;
                    if (regionPixels[blI] == rid)
                        dist[i] = minU16(dist[i], dist[blI] + 141);
                }
            }
        }
        // Find max distance per country for normalization
        std::map<int, uint16_t> maxDist;
        for (int i = 0; i < W * H; ++i) {
            if (landData[i * 4 + 3] == 0) continue;
            int rid = (int)regionPixels[i];
            if (dist[i] < INF && dist[i] > maxDist[rid])
                maxDist[rid] = dist[i];
        }
        // Generate pixels with smoothstep saturation/value gradient
        // Compute approximate Euclidean distance from land for sea depth gradient.
        // Uses 8-directional BFS with cardinal=10, diagonal=14 to avoid diamond/square artifacts.
        std::vector<int> seaDist(W * H, 999999);
        {
            struct QE { int x, y; };
            std::vector<QE> sq;
            for (int i = 0; i < W * H; ++i) {
                if (landData[i * 4 + 3] != 0) {
                    seaDist[i] = 0;
                    sq.push_back({i % W, i / W});
                }
            }
            int dx8[8] = {1, -1, 0, 0, 1, 1, -1, -1};
            int dy8[8] = {0, 0, 1, -1, 1, -1, 1, -1};
            int w8[8]  = {10,10,10,10, 14,14,14,14};
            size_t head = 0;
            while (head < sq.size()) {
                auto [cx, cy] = sq[head++];
                int cd = seaDist[cy * W + cx];
                if (cd >= 2000) continue;
                for (int d = 0; d < 8; ++d) {
                    int nx = cx + dx8[d];
                    if (nx < 0) nx += W;
                    if (nx >= W) nx -= W;
                    int ny = cy + dy8[d];
                    if (ny < 0 || ny >= H) continue;
                    int ni = ny * W + nx;
                    int nd = cd + w8[d];
                    if (seaDist[ni] > nd) {
                        seaDist[ni] = nd;
                        sq.push_back({nx, ny});
                    }
                }
            }
        }

        std::vector<uint32_t> politicalPixels(W * H, 0);
        for (int i = 0; i < W * H; ++i) {
            int rid = (int)regionPixels[i];
            if (landData[i * 4 + 3] != 0) {
                if (rid == UNC_RID2) {
                    uint8_t g = 140;
                    politicalPixels[i] = (255u << 24) | (g << 16) | (g << 8) | g;
                } else if (rid > 0) {
                    float h = hueMap[rid];
                    float t = dist[i] / (float)maxDist[rid];
                    if (t > 1.0f) t = 1.0f;
                    t = t * t * (3.0f - 2.0f * t);
                    float s = 0.65f - 0.35f * t;
                    float v = 0.78f - 0.28f * t;
                    politicalPixels[i] = hsvToRgb(h, s, v);
                }
            } else {
                // Sea with subtle depth gradient: slightly lighter near coast, darker in deep water
                float depth = std::min((float)seaDist[i] / 600.0f, 1.0f);
                int R = 18 + (int)((35 - 18) * (1.0f - depth));
                int G = 35 + (int)((60 - 35) * (1.0f - depth));
                int B = 45 + (int)((80 - 45) * (1.0f - depth));
                politicalPixels[i] = (255u << 24) | (B << 16) | (G << 8) | R;
            }
        }
        // Seam-fix political map edges
        {
            const int MARGIN3 = 5;
            for (int y = 0; y < H; ++y) {
                for (int m = 0; m < MARGIN3; ++m) {
                    politicalPixels[y * W + m] = politicalPixels[y * W + (W - 1 - m)];
                    politicalPixels[y * W + (W - 1 - m)] = politicalPixels[y * W + m];
                }
            }
        }
        std::string polPath = m_cfg.dataDir + "/political.png";
        stbi_write_png(polPath.c_str(), W, H, 4,
                       politicalPixels.data(), W * 4);
        std::cout << "  Saved " << polPath << "\n";
    }

    // ── Generate thumbnail from political.png ──
    {
        std::string polPath = m_cfg.dataDir + "/political.png";
        std::string thumbPath = m_cfg.dataDir + "/thumb.png";
        int pw, ph, pc;
        unsigned char* polData = stbi_load(polPath.c_str(), &pw, &ph, &pc, 4);
        if (polData) {
            int tw = 160, th = 80;
            std::vector<unsigned char> thumbData(tw * th * 4);
            for (int y = 0; y < th; ++y) {
                for (int x = 0; x < tw; ++x) {
                    int sx = x * pw / tw;
                    int sy = y * ph / th;
                    std::memcpy(&thumbData[(y * tw + x) * 4], &polData[(sy * pw + sx) * 4], 4);
                }
            }
            stbi_write_png(thumbPath.c_str(), tw, th, 4, thumbData.data(), tw * 4);
            std::cout << "  Saved " << thumbPath << " (" << tw << "x" << th << ")\n";
            stbi_image_free(polData);
        } else {
            std::cerr << "  Warning: could not load political.png for thumbnail\n";
        }
    }

    // ── Compute province populations using gravity model ──
    computeAndSavePopulation(provincePixels, provinceJson, regionIsoA3);

    // Write map metadata
    {
        nlohmann::json meta;
        meta["map_date"] = "January 2000 AD";
        meta["author"] = "Pr1nted";
        meta["license"] = "CC-BY-4.0";
        meta["has_scripts"] = false;
        std::string metaPath = m_cfg.dataDir + "/metadata.json";
        std::ofstream mf(metaPath);
        mf << meta.dump();
        mf.close();
    }

    // Package everything into .odmap archive
    std::string odmPath = m_cfg.dataDir + "/map.odmap";
    std::vector<std::string> odmFiles = {
        m_cfg.dataDir + "/land_sea.png",
        m_cfg.dataDir + "/provinces.png",
        m_cfg.dataDir + "/provinces.json",
        m_cfg.dataDir + "/political.png",
        countriesPath,
        m_cfg.dataDir + "/metadata.json",
        m_cfg.dataDir + "/population.json",
        m_cfg.dataDir + "/political_compass.json",
        m_cfg.dataDir + "/minorities.json",
        m_cfg.dataDir + "/minority_colors.json",
        m_cfg.dataDir + "/country_compass.json",
        m_cfg.dataDir + "/starting_minority_policies.json",
        m_cfg.dataDir + "/thumb.png"
    };

    // Write license files temporarily to data dir for inclusion in .odmap
    std::string licPath = m_cfg.dataDir + "/LICENSE";
    std::string compPath = m_cfg.dataDir + "/license compliance.txt";
    {
        auto writeStr = [](const std::string& path, const std::string& content) {
            std::ofstream f(path); f << content; f.close();
        };
        writeStr(licPath,
            "MIT License\n"
            "\n"
            "Copyright (c) 2025 Pr1nted\n"
            "\n"
            "Permission is hereby granted, free of charge, to any person obtaining a copy\n"
            "of this map data and associated documentation files (the \".odmap\" file),\n"
            "to deal in the Software without restriction, including without limitation the\n"
            "rights to use, copy, modify, merge, publish, distribute, sublicense, and/or\n"
            "sell copies of the Software, and to permit persons to whom the Software is\n"
            "furnished to do so, subject to the following conditions:\n"
            "\n"
            "The above copyright notice and this permission notice shall be included in all\n"
            "copies or substantial portions of the Software.\n"
            "\n"
            "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
            "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
            "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
            "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
            "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
            "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n"
            "SOFTWARE.\n");
        writeStr(compPath,
            "License Compliance & Attribution\n"
            "=================================\n"
            "\n"
            "This .odmap file incorporates data from several third-party sources.\n"
            "Below are the required attributions and license information.\n"
            "\n"
            "1. Map Boundaries - Natural Earth Data\n"
            "--------------------------------------\n"
            "Source: https://www.naturalearthdata.com/\n"
            "License: Public Domain\n"
            "\n"
            "Natural Earth vector and raster map data are made available in the\n"
            "public domain. No permission is needed to use them, but attribution\n"
            "is appreciated.\n"
            "\n"
            "2. Population Data - World Bank\n"
            "-------------------------------\n"
            "Source: https://api.worldbank.org/v2/country/*/indicator/SP.POP.TOTL\n"
            "License: CC-BY 4.0 (https://creativecommons.org/licenses/by/4.0/)\n"
            "\n"
            "World Bank data is used under the Creative Commons Attribution 4.0\n"
            "International license. You must give appropriate credit when using\n"
            "or redistributing this data.\n"
            "\n"
            "3. Country Flags\n"
            "----------------\n"
            "Sources:\n"
            "  - https://flagcdn.com - Flags are CC0 / public domain\n"
            "  - https://commons.wikimedia.org - Various Creative Commons licenses\n"
            "    (Flag of Kosovo, Flag of Northern Cyprus, Flag of Somaliland)\n"
            "\n"
            "4. Administrative Boundaries (ne_10m_admin_1_states_provinces)\n"
            "--------------------------------------------------------------\n"
            "Source: https://www.naturalearthdata.com/\n"
            "License: Public Domain\n"
            "\n"
            "Used for Crimea region identification within Ukraine.\n");
    }
    odmFiles.push_back(licPath);
    odmFiles.push_back(compPath);
    if (createODMArchive(odmFiles, odmPath)) {
        std::cout << "  Saved " << odmPath << "\n";
    } else {
        std::cerr << "  Warning: .odmap archive creation failed\n";
    }

    // Clean up temp files
    fs::remove(licPath);
    fs::remove(compPath);
    fs::remove(m_cfg.dataDir + "/thumb.png");

    stbi_image_free(landData);
    return true;
}

bool Generator::run() {
    std::cout << "OpenDoctrines Map Generator (C++)\n";
    std::cout << "  Resolution: " << m_cfg.mapWidth << "x" << m_cfg.mapHeight << "\n";
    std::cout << "  Data dir: " << m_cfg.dataDir << "\n";
    std::cout << "  Pop year: " << m_cfg.popYear << "\n";

    fs::create_directories(m_cfg.dataDir);

    std::string landPath = m_cfg.dataDir + "/land_sea.png";
    bool landExists = false;
    {
        int lw, lh, lc;
        if (stbi_info(landPath.c_str(), &lw, &lh, &lc)) {
            landExists = (lw == m_cfg.mapWidth && lh == m_cfg.mapHeight);
        }
    }

    if (!landExists) {
        if (!generateLandSea()) return false;
    } else {
        std::cout << "  Land/sea map already exists and matches resolution, skipping\n";
    }

    if (!generateProvinces()) return false;

    return true;
}

// ── Fetch country populations from World Bank API ──
std::unordered_map<std::string, long long> Generator::fetchCountryPopulations(int year) {
    std::unordered_map<std::string, long long> result;
    std::cout << "\n  Fetching country populations for year " << year
              << " from World Bank API...\n";

    std::string tmpDir = m_cfg.dataDir + "/tmp_pop";
    fs::create_directories(tmpDir);

    int page = 1;
    int totalPages = 1;

    do {
        std::string url = "https://api.worldbank.org/v2/country/all/indicator/SP.POP.TOTL"
                          "?date=" + std::to_string(year) +
                          "&format=json&per_page=20000"
                          "&page=" + std::to_string(page);

        std::string filePath = tmpDir + "/wb_page_" + std::to_string(page) + ".json";
        if (!downloadFile(url, filePath)) {
            std::cerr << "  Failed to download World Bank data (page " << page << ")\n";
            fs::remove_all(tmpDir);
            return result;
        }

        std::ifstream f(filePath);
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        f.close();

        if (content.empty() || content.size() < 20) {
            std::cerr << "  World Bank response too small or empty (page " << page << ")\n";
            fs::remove_all(tmpDir);
            return result;
        }

        // Detect HTML error pages (not JSON)
        if (content[0] == '<') {
            std::cerr << "  World Bank returned HTML instead of JSON (page " << page
                      << "). Is the API reachable?\n";
            fs::remove_all(tmpDir);
            return result;
        }

        nlohmann::json json;
        try {
            json = nlohmann::json::parse(content);
        } catch (const nlohmann::json::parse_error& e) {
            std::cerr << "  Failed to parse World Bank JSON (page " << page
                      << "): " << e.what() << "\n";
            fs::remove_all(tmpDir);
            return result;
        }

        if (!json.is_array() || json.size() < 2) {
            std::cerr << "  Unexpected World Bank API response format\n";
            fs::remove_all(tmpDir);
            return result;
        }

        // First element: metadata
        auto& meta = json[0];
        totalPages = meta.value("pages", 1);
        if (meta.value("total", 0) == 0) {
            std::cerr << "  World Bank API returned 0 total records\n";
            fs::remove_all(tmpDir);
            return result;
        }

        // Second element: array of data points
        auto& data = json[1];
        if (!data.is_array()) {
            fs::remove_all(tmpDir);
            return result;
        }

        int added = 0;
        for (auto& entry : data) {
            // Skip entries without valid ISO3 code (aggregates, regions)
            std::string iso3 = entry.value("countryiso3code", "");
            if (iso3.length() != 3) continue;

            // Skip null values
            if (!entry.contains("value") || entry["value"].is_null()) continue;
            long long pop = 0;
            try { pop = entry["value"].get<long long>(); } catch (...) { continue; }
            if (pop <= 0) continue;

            result[iso3] = pop;
            added++;
        }

        std::cout << "    Page " << page << "/" << totalPages
                  << ": " << added << " countries\n";
        page++;
    } while (page <= totalPages);

    fs::remove_all(tmpDir);
    std::cout << "  Fetched " << result.size() << " country populations\n";

    // Manual overrides for unrecognised countries and territories not in World Bank database
    // Population estimates for year 2000
    std::unordered_map<std::string, long long> manualOverrides = {
        {"TWN", 22280000},  // Taiwan (Republic of China) - ~22.3M in 2000
        {"ALA", 26500},    // Åland Islands - ~26.5K in 2000
        {"ATF", 150},      // French Southern and Antarctic Lands - ~150 (research stations)
        {"ESH", 350000},   // Western Sahara - ~350K (estimated, disputed territory)
        {"FLK", 2900},     // Falkland Islands - ~2.9K in 2000
        {"SGS", 20},       // South Georgia and the South Sandwich Islands - ~20 (seasonal researchers)
        {"UNC", 0},        // Unclaimed territories - no permanent population
        {"XSO", 3500000},  // Somaliland - ~3.5M in 2000 (estimated)
        {"XNC", 265000},   // Northern Cyprus - ~265K in 2000 (estimated)
    };

    for (auto& [iso, pop] : manualOverrides) {
        if (!result.count(iso)) {
            result[iso] = pop;
            std::cout << "  Added manual override for " << iso << ": " << pop << "\n";
        }
    }

    return result;
}

// ── Fetch city database from Natural Earth Populated Places ──
std::vector<City> Generator::fetchCityDatabase() {
    std::vector<City> cities;
    std::cout << "\n  Fetching populated places from Natural Earth...\n";

    std::string tmpDir = m_cfg.dataDir + "/tmp_cities";
    fs::create_directories(tmpDir);

    std::string zipPath = tmpDir + "/populated_places.zip";
    if (!downloadFile(m_cfg.populatedPlacesUrl, zipPath)) {
        std::cerr << "  Failed to download populated places\n";
        fs::remove_all(tmpDir);
        return cities;
    }

    if (!extractZip(zipPath, tmpDir)) {
        std::cerr << "  Failed to extract populated places\n";
        fs::remove_all(tmpDir);
        return cities;
    }

    std::string shpPath;
    if (!findSHP(tmpDir, shpPath)) {
        std::cerr << "  No shapefile found for populated places\n";
        fs::remove_all(tmpDir);
        return cities;
    }

    std::string dbfPath = shpPath.substr(0, shpPath.size() - 4) + ".dbf";
    ShapefileReader reader;
    if (!reader.open(shpPath, dbfPath)) {
        fs::remove_all(tmpDir);
        return cities;
    }

    int nameIdx   = reader.getFieldIndex("NAME");
    int latIdx    = reader.getFieldIndex("LATITUDE");
    int lonIdx    = reader.getFieldIndex("LONGITUDE");
    int popMaxIdx = reader.getFieldIndex("POP_MAX");

    if (nameIdx < 0 || latIdx < 0 || lonIdx < 0 || popMaxIdx < 0) {
        std::cerr << "  Populated places DBF missing required fields"
                  << " (NAME/LATITUDE/LONGITUDE/POP_MAX)\n";
        fs::remove_all(tmpDir);
        return cities;
    }

    // Collect all city data, then sort names into a persistent buffer
    struct CityRaw {
        std::string name;
        double lat = 0;
        double lon = 0;
        long long pop = 0;
    };
    std::vector<CityRaw> rawCities;
    rawCities.reserve(reader.getRecordCount());

    for (int i = 0; i < reader.getRecordCount(); ++i) {
        std::string popStr = reader.getStringField(i, popMaxIdx);
        if (popStr.empty()) continue;
        long long pop = 0;
        try { pop = std::stoll(popStr); } catch (...) { continue; }
        if (pop < 10000) continue; // skip tiny settlements

        std::string latStr = reader.getStringField(i, latIdx);
        std::string lonStr = reader.getStringField(i, lonIdx);
        if (latStr.empty() || lonStr.empty()) continue;

        double lat = 0, lon = 0;
        try { lat = std::stod(latStr); } catch (...) { continue; }
        try { lon = std::stod(lonStr); } catch (...) { continue; }

        std::string name = reader.getStringField(i, nameIdx);

        rawCities.push_back({std::move(name), lat, lon, pop});
    }

    // Build a single contiguous name buffer so City.name pointers stay valid
    // after rawCities is destroyed. We use a static string since cities
    // is returned by value and the buffer must outlive it.
    // Instead, store names in a shared static buffer.
    static std::vector<std::string> nameStore;
    nameStore.clear();
    nameStore.reserve(rawCities.size());

    cities.reserve(rawCities.size());
    for (auto& rc : rawCities) {
        nameStore.push_back(std::move(rc.name));
        cities.push_back({nameStore.back().c_str(), rc.lat, rc.lon, rc.pop});
    }

    // Sort by population descending for better cache behavior in gravity loop
    std::sort(cities.begin(), cities.end(),
              [](const City& a, const City& b) { return a.pop > b.pop; });

    fs::remove_all(tmpDir);
    std::cout << "  Loaded " << cities.size() << " populated places\n";
    return cities;
}

// ── Gravity-model population redistribution ──
void Generator::computeAndSavePopulation(
    const std::vector<uint32_t>& provincePixels,
    const nlohmann::json& provinceJson,
    const std::map<int, std::string>& regionIsoA3)
{
    const int W = m_cfg.mapWidth;
    const int H = m_cfg.mapHeight;
    const int N = W * H;
    const long long MIN_POP = 1000;

    std::cout << "\n  Computing population distribution (gravity model)...\n";

    // 1. Build province-to-country mapping
    std::unordered_map<int, int> provToCountry; // pid -> rid
    for (auto& [pidStr, entry] : provinceJson.items()) {
        int pid = entry["id"];
        int cid = entry.value("country_id", 0);
        provToCountry[pid] = cid;
    }

    // 2. Build country-ISO map and fetch country populations from World Bank
    std::unordered_map<int, std::string> countryIso;
    for (auto& [rid, iso] : regionIsoA3) {
        countryIso[rid] = iso;
    }

    auto countryPops = fetchCountryPopulations(m_cfg.popYear);

    // 3. Compute coastal proximity using BFS from sea pixels
    // Load land/sea to find sea pixels
    std::string landPath = m_cfg.dataDir + "/land_sea.png";
    int lw, lh, lc;
    unsigned char* landData = stbi_load(landPath.c_str(), &lw, &lh, &lc, 4);
    if (!landData) {
        std::cerr << "  Warning: cannot load land_sea.png for population calc\n";
        landData = (unsigned char*)calloc(N * 4, 1);
        lc = 4; lw = W; lh = H;
    }

    std::vector<float> coastProx(N, 0.0f);
    std::vector<float> distFromSea(N, 1e9f);
    std::queue<int> bfsQueue;

    // Initialize: all sea pixels at distance 0
    for (int i = 0; i < N; ++i) {
        int a = landData[i * 4 + 3];
        if (a == 0) { // sea
            distFromSea[i] = 0;
            bfsQueue.push(i);
        }
    }

    // BFS from sea inward
    std::cout << "    Computing coastal proximity (BFS)...\n";
    int dx4[] = {1, -1, 0, 0};
    int dy4[] = {0, 0, 1, -1};
    while (!bfsQueue.empty()) {
        int idx = bfsQueue.front(); bfsQueue.pop();
        int x = idx % W, y = idx / W;
        float d = distFromSea[idx];
        for (int d2 = 0; d2 < 4; ++d2) {
            int nx = x + dx4[d2], ny = y + dy4[d2];
            if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
            int ni = ny * W + nx;
            float nd = d + 1.0f;
            if (nd < distFromSea[ni]) {
                distFromSea[ni] = nd;
                bfsQueue.push(ni);
            }
        }
    }

    // Normalize coastal proximity: 1 at coast, 0 deep inland
    const float MAX_COAST_DIST = 200.0f; // pixels
    for (int i = 0; i < N; ++i) {
        coastProx[i] = std::max(0.0f, 1.0f - distFromSea[i] / MAX_COAST_DIST);
    }

    stbi_image_free(landData);

    // 4. Compute density grid (coarse, for performance)
    const int GRID_SCALE = 16;
    const int GW = W / GRID_SCALE;
    const int GH = H / GRID_SCALE;
    std::vector<double> densityGrid(GW * GH, 0.0);

    auto cities = fetchCityDatabase();
    double maxCityPop = 0;
    for (auto& c : cities) if (c.pop > maxCityPop) maxCityPop = c.pop;

    std::cout << "    Computing gravity model density (" << GW << "x" << GH
              << " grid, " << cities.size() << " cities)...\n";

    for (auto& city : cities) {
        // Convert city lon/lat to grid coordinates
        int gx = (int)((city.lon + 180.0) / 360.0 * GW);
        int gy = (int)((90.0 - city.lat) / 180.0 * GH);
        gx = std::max(0, std::min(GW - 1, gx));
        gy = std::max(0, std::min(GH - 1, gy));

        // Sigma: larger cities have wider influence
        double sigma = 0.5 + 1.5 * std::pow(city.pop / maxCityPop, 0.3);
        // Convert sigma from degrees to grid cells
        double sigmaGrid = sigma * (GW / 360.0);
        double sigma2 = sigmaGrid * sigmaGrid;

        // Apply gravity within a bounding box
        int radius = (int)(sigmaGrid * 5) + 1;
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                int nx = gx + dx, ny = gy + dy;
                if (nx < 0 || nx >= GW || ny < 0 || ny >= GH) continue;
                double distSq = (double)dx * dx + (double)dy * dy;
                // Cosine correction for longitude
                double centerLon = -180.0 + (nx + 0.5) / GW * 360.0;
                double cosLat = std::cos((90.0 - (ny + 0.5) / GH * 180.0) * M_PI / 180.0);
                distSq = (double)dy * dy + ((double)dx * cosLat) * ((double)dx * cosLat);
                densityGrid[ny * GW + nx] += (double)city.pop / (1.0 + distSq / sigma2);
            }
        }
    }

    // 5. Apply climate/biome corrections
    std::cout << "    Applying biome corrections...\n";
    for (int gy = 0; gy < GH; ++gy) {
        double lat = 90.0 - (gy + 0.5) / GH * 180.0;
        double absLat = std::fabs(lat);

        // Temperature comfort: peaks at 25° and 45°
        double tropical = std::exp(-0.5 * std::pow((absLat - 25) / 20.0, 2));
        double temperate = std::exp(-0.5 * std::pow((absLat - 45) / 15.0, 2));
        double climateMod = std::max(tropical * 0.7, temperate * 1.0);

        // Polar penalty
        if (absLat > 65) {
            climateMod *= std::exp(-std::pow((absLat - 65) / 5.0, 2));
        }

        for (int gx = 0; gx < GW; ++gx) {
            densityGrid[gy * GW + gx] *= climateMod;

            // Coastal bonus
            int px = gx * GRID_SCALE + GRID_SCALE / 2;
            int py = gy * GRID_SCALE + GRID_SCALE / 2;
            if (px < W && py < H) {
                densityGrid[gy * GW + gx] *= (0.5 + 0.5 * coastProx[py * W + px]);
            }
        }
    }

    // Add small baseline to prevent zero-density
    double baseline = 0;
    int nonZero = 0;
    for (auto d : densityGrid) if (d > 0) { baseline += d; nonZero++; }
    baseline = nonZero > 0 ? baseline / nonZero * 0.01 : 1.0;
    for (auto& d : densityGrid) d += baseline;

    // 6. Compute density weight per province
    std::cout << "    Computing per-province density weights...\n";
    std::unordered_map<int, double> provWeight;
    std::unordered_map<int, int> provPixelCount;

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint32_t pixel = provincePixels[y * W + x];
            if (pixel == 0) continue;
            int pid = pixelToId(pixel);
            if (pid <= 0) continue;
            int gx = x / GRID_SCALE;
            int gy = y / GRID_SCALE;
            if (gx < GW && gy < GH) {
                provWeight[pid] += densityGrid[gy * GW + gx];
                provPixelCount[pid]++;
            }
        }
    }

    // 7. Distribute country populations proportionally
    std::cout << "    Distributing population...\n";

    // Group provinces by country
    std::unordered_map<int, std::vector<int>> countryProvs;
    for (auto& [pidStr, entry] : provinceJson.items()) {
        int pid = entry["id"];
        int cid = provToCountry[pid];
        if (cid > 0) countryProvs[cid].push_back(pid);
    }

    nlohmann::json popJson;
    nlohmann::json provPopJson; // also save province_population_2000.json
    long long totalAssigned = 0;

    for (auto& [cid, pids] : countryProvs) {
        // Get country ISO and total population
        std::string iso = countryIso.count(cid) ? countryIso[cid] : "";

        // Apply ISO overrides for country names we know are wrong
        // (This is already handled above when building countries.json)

        long long countryTotal = 0;
        auto popIt = countryPops.find(iso);
        if (popIt != countryPops.end()) countryTotal = popIt->second;

        if (countryTotal <= 0) {
            for (int pid : pids) {
                popJson[std::to_string(pid)] = MIN_POP;
                provPopJson[std::to_string(pid)] = MIN_POP;
                totalAssigned += MIN_POP;
            }
            continue;
        }

        // Compute weights
        std::vector<double> weights(pids.size());
        double wSum = 0;
        for (size_t i = 0; i < pids.size(); ++i) {
            double w = provWeight.count(pids[i]) ? provWeight[pids[i]] : baseline * 0.1;
            if (w <= 0) w = baseline * 0.1;
            weights[i] = w;
            wSum += w;
        }

        if (wSum <= 0) {
            long long per = countryTotal / (long long)pids.size();
            for (int pid : pids) {
                popJson[std::to_string(pid)] = per;
                provPopJson[std::to_string(pid)] = per;
                totalAssigned += per;
            }
            continue;
        }

        // Distribute proportionally, last province gets remainder
        long long remaining = countryTotal;
        // Adaptive minimum: use MIN_POP for normal countries, but allow smaller values for tiny territories
        long long adaptiveMin = std::min(MIN_POP, countryTotal / (long long)pids.size());
        for (size_t i = 0; i < pids.size(); ++i) {
            long long pop;
            if (i == pids.size() - 1) {
                pop = remaining;
            } else {
                pop = (long long)std::round((double)countryTotal * weights[i] / wSum);
                remaining -= pop;
            }
            pop = std::max(pop, adaptiveMin);
            popJson[std::to_string(pids[i])] = pop;
            provPopJson[std::to_string(pids[i])] = pop;
            totalAssigned += pop;
        }
    }

    // Save population.json
    std::string popPath = m_cfg.dataDir + "/population.json";
    {
        std::ofstream popf(popPath);
        popf << popJson.dump();
        popf.close();
    }
    std::cout << "  Saved " << popPath << " (" << popJson.size() << " provinces, "
              << totalAssigned << " total pop)\n";

    // Save province_population_{year}.json for reference
    std::string ppPath = m_cfg.dataDir + "/province_population_" + std::to_string(m_cfg.popYear) + ".json";
    {
        std::ofstream ppf(ppPath);
        ppf << provPopJson.dump();
        ppf.close();
    }
    std::cout << "  Saved " << ppPath << "\n";

    // Save population_{year}.json (country-level totals)
    nlohmann::json countryPopJson = nlohmann::json::object();
    for (auto& [iso, pop] : countryPops) {
        countryPopJson[iso] = pop;
    }
    std::string cpPath = m_cfg.dataDir + "/population_" + std::to_string(m_cfg.popYear) + ".json";
    {
        std::ofstream cpf(cpPath);
        cpf << countryPopJson.dump(2);
        cpf.close();
    }
    std::cout << "  Saved " << cpPath << " (" << countryPopJson.size() << " countries)\n";
}
