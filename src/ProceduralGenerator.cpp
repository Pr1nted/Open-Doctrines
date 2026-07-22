#include "ProceduralGenerator.h"
#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <map>
#include <queue>
#include <cstring>
#include <cctype>
#include <climits>
#include <iostream>

static Color idToColor(int id) {
    return Color{(uint8_t)((id >> 16) & 0xFF),
                 (uint8_t)((id >> 8) & 0xFF),
                 (uint8_t)(id & 0xFF), 255};
}

static int colorToId(Color c) {
    return (c.r << 16) | (c.g << 8) | c.b;
}

// Land = green channel > blue (COL_LAND={0,180,0}, COL_SEA={0,0,120})
static bool isLand(Color c) { return c.g > c.b; }
static bool isSea(Color c) { return c.b > c.g; }

// ── Country name generator ──
static std::string generateCountryName(int cid, int seed) {
    std::mt19937 rng((unsigned int)(cid * 7919u + seed * 104729u));
    static const char* roots[] = {"ard","bel","carth","dun","erin","falk","goth","hyr",
        "ith","jor","kath","lor","myth","nur","orl","pryd","quen","ryn","sar","tor",
        "umb","vald","welk","xan","yrm","zol","brel","cart","dorn","feld","gard",
        "helm","ing","keld","lorn","march","norn","ost","rath","sul","tarn","uld",
        "ven","wulf","yarl","zak"};
    static const char* prefixes[] = {"al","bal","cal","dor","el","fel","gal","han",
        "il","jun","kel","lan","mar","nor","ol","pal","ran","sol","tal","val",
        "ar","bel","cer","del","er","for","gor","hel","ir","jel","kor","lar",
        "mel","nel","or","por","rel","sil","tol","ul","ver","wel","xen","yl","zor"};
    static const char* suffixes[] = {"ia","land","stan","burg","mark","dor","via",
        "nia","rea","gard","thal","wich","field","more","shire","stead","town",
        "vale","wood","wold","maar","lund","holm","grad","gorod"};
    int nRoots = sizeof(roots)/sizeof(roots[0]);
    int nPrefs = sizeof(prefixes)/sizeof(prefixes[0]);
    int nSuffs = sizeof(suffixes)/sizeof(suffixes[0]);

    std::uniform_int_distribution<int> pat(0, 4);
    int p = pat(rng);
    std::string name;
    auto cap = [](std::string s) { s[0] = (char)toupper(s[0]); return s; };
    switch (p) {
        case 0: case 1: {
            std::string r = roots[std::uniform_int_distribution<int>(0,nRoots-1)(rng)];
            name = cap(r) + ((p==0)?"ia":"land");
            break;
        }
        case 2: case 3: {
            std::string pr = prefixes[std::uniform_int_distribution<int>(0,nPrefs-1)(rng)];
            std::string r = roots[std::uniform_int_distribution<int>(0,nRoots-1)(rng)];
            name = cap(pr) + r;
            break;
        }
        default: {
            std::string pr = prefixes[std::uniform_int_distribution<int>(0,nPrefs-1)(rng)];
            std::string s = suffixes[std::uniform_int_distribution<int>(0,nSuffs-1)(rng)];
            name = cap(pr) + s;
            break;
        }
    }
    return name;
}

// Simple hash-based noise
static float noiseFunc(int x, int y, int seed) {
    int h = seed * 374761393 + x * 668265263 + y * 1274126177;
    h = (h ^ (h >> 13)) * 1274126177;
    h = h ^ (h >> 16);
    return (h & 0x7FFFFFFF) / (float)0x7FFFFFFF;
}

static float smoothNoise(float fx, float fy, int seed) {
    int ix = (int)std::floor(fx);
    int iy = (int)std::floor(fy);
    float fracX = fx - ix, fracY = fy - iy;
    float sx = fracX * fracX * (3.0f - 2.0f * fracX);
    float sy = fracY * fracY * (3.0f - 2.0f * fracY);
    float v00 = noiseFunc(ix, iy, seed);
    float v10 = noiseFunc(ix + 1, iy, seed);
    float v01 = noiseFunc(ix, iy + 1, seed);
    float v11 = noiseFunc(ix + 1, iy + 1, seed);
    return v00 * (1 - sx) * (1 - sy) + v10 * sx * (1 - sy) +
           v01 * (1 - sx) * sy + v11 * sx * sy;
}

static float fbmNoise(float x, float y, int seed) {
    float val = 0, amp = 1, freq = 1, sum = 0;
    for (int i = 0; i < 4; ++i) {
        val += amp * smoothNoise(x * freq, y * freq, seed + i * 73);
        sum += amp;
        amp *= 0.5f;
        freq *= 2.3f;
    }
    return val / sum;
}

ProceduralOutput generateProcedural(
    const std::vector<Color>& landSea,
    int w, int h,
    int seed,
    int targetCountries,
    float provinceDensity)
{
    ProceduralOutput out;
    std::mt19937 rng((unsigned int)seed);

    // ── Step 1: Find connected land components ──
    struct Component {
        int id;
        std::vector<int> pixels;
        float cx, cy; // centroid
    };
    std::vector<Component> components;
    std::vector<int> compOfPixel(w * h, 0);
    {
        int nextComp = 1;
        for (int i = 0; i < w * h; ++i) {
            if (!isLand(landSea[i]) || compOfPixel[i] != 0) continue;
            std::vector<int> stack, pixels;
            stack.push_back(i);
            compOfPixel[i] = nextComp;
            while (!stack.empty()) {
                int cur = stack.back(); stack.pop_back();
                pixels.push_back(cur);
                int cx = cur % w, cy = cur / w;
                int nbs[4][2] = {{cx-1,cy},{cx+1,cy},{cx,cy-1},{cx,cy+1}};
                for (auto& nb : nbs) {
                    int nx = nb[0], ny = nb[1];
                    if (nx < 0) nx = w - 1; else if (nx >= w) nx = 0;
                    if (ny < 0 || ny >= h) continue;
                    int ni = ny * w + nx;
                    if (compOfPixel[ni] == 0 && isLand(landSea[ni])) {
                        compOfPixel[ni] = nextComp;
                        stack.push_back(ni);
                    }
                }
            }
            long long sx = 0, sy = 0;
            for (int p : pixels) { sx += p % w; sy += p / w; }
            components.push_back({nextComp++, pixels,
                (float)sx / pixels.size(), (float)sy / pixels.size()});
        }
    }
    if (components.empty()) return out;
    std::cout << "  Found " << components.size() << " land components\n";

    // Sort components by size descending
    std::sort(components.begin(), components.end(),
        [](const Component& a, const Component& b) { return a.pixels.size() > b.pixels.size(); });

    // ── Step 2: Distribute countries across components ──
    long long totalLandPixels = 0;
    for (auto& c : components) totalLandPixels += c.pixels.size();

    // Floor allocation + highest-remainder (0-slot components get merged later)
    std::vector<int> compCountryCount(components.size(), 0);
    {
        int allocated = 0;
        for (size_t ci = 0; ci < components.size(); ++ci)
            compCountryCount[ci] = (int)((double)targetCountries * components[ci].pixels.size() / totalLandPixels);
        for (size_t ci = 0; ci < components.size(); ++ci) allocated += compCountryCount[ci];
        struct Rem { int idx; double frac; };
        std::vector<Rem> remainders;
        for (size_t ci = 0; ci < components.size(); ++ci) {
            double exact = (double)targetCountries * components[ci].pixels.size() / totalLandPixels;
            remainders.push_back({(int)ci, exact - compCountryCount[ci]});
        }
        std::sort(remainders.begin(), remainders.end(),
            [](const Rem& a, const Rem& b) { return a.frac > b.frac; });
        int slot = 0;
        while (allocated < targetCountries && slot < (int)remainders.size()) {
            compCountryCount[remainders[slot].idx]++;
            allocated++;
            slot++;
        }
    }

    // Generate country seeds and flood fill within each component
    std::vector<int> countryOfPixel(w * h, 0);
    int nextCountryId = 1;
    for (size_t ci = 0; ci < components.size(); ++ci) {
        int k = compCountryCount[ci];
        if (k <= 0) continue;
        auto& comp = components[ci];
        auto& pixels = comp.pixels;

        if (k == 1) {
            // Single country = entire component
            int cid = nextCountryId++;
            for (int p : pixels) countryOfPixel[p] = cid;
            continue;
        }

        // Place k repulsive seeds within this component
        float avgArea = (float)pixels.size() / k;
        float minDist = std::sqrt(avgArea * 0.5f);

        std::vector<int> shuffled = pixels;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        std::vector<int> seeds;

        for (int idx : shuffled) {
            if ((int)seeds.size() >= k) break;
            int cx = idx % w, cy = idx / w;
            bool tooClose = false;
            for (int s : seeds) {
                int sx = s % w, sy = s / w;
                int dx = cx - sx, dy = cy - sy;
                if (dx * dx + dy * dy < minDist * minDist * 0.5f) { tooClose = true; break; }
            }
            if (!tooClose) seeds.push_back(idx);
        }
        // Fill remaining
        for (int idx : shuffled) {
            if ((int)seeds.size() >= k) break;
            bool already = false;
            for (int s : seeds) if (s == idx) { already = true; break; }
            if (!already) seeds.push_back(idx);
        }

        // Assign country IDs to seeds
        std::vector<int> seedCids(seeds.size());
        for (size_t si = 0; si < seeds.size(); ++si) {
            seedCids[si] = nextCountryId++;
            countryOfPixel[seeds[si]] = seedCids[si];
        }

        // Flood fill from seeds within this component
        std::vector<int> frontier;
        std::vector<bool> inFrontier(w * h, false);
        int compId = comp.id;
        for (size_t si = 0; si < seeds.size(); ++si) {
            int sx = seeds[si] % w, sy = seeds[si] / w;
            int nbs[4][2] = {{sx-1,sy},{sx+1,sy},{sx,sy-1},{sx,sy+1}};
            for (auto& nb : nbs) {
                int nx = nb[0], ny = nb[1];
                if (nx < 0) nx = w - 1; else if (nx >= w) nx = 0;
                if (ny < 0 || ny >= h) continue;
                int nidx = ny * w + nx;
                if (countryOfPixel[nidx] == 0 && isLand(landSea[nidx])
                    && compOfPixel[nidx] == compId && !inFrontier[nidx]) {
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

            if (countryOfPixel[idx] != 0) continue;
            if (compOfPixel[idx] != compId) continue;

            int cx = idx % w, cy = idx / w;
            int ownerCid = 0;
            int nbs[4][2] = {{cx-1,cy},{cx+1,cy},{cx,cy-1},{cx,cy+1}};
            for (auto& nb : nbs) {
                int nx = nb[0], ny = nb[1];
                if (nx < 0) nx = w - 1; else if (nx >= w) nx = 0;
                if (ny < 0 || ny >= h) continue;
                int nidx = ny * w + nx;
                if (compOfPixel[nidx] != compId) continue;
                int nid = countryOfPixel[nidx];
                if (nid != 0) { ownerCid = nid; break; }
            }
            if (ownerCid == 0) continue;
            countryOfPixel[idx] = ownerCid;

            for (auto& nb : nbs) {
                int nx = nb[0], ny = nb[1];
                if (nx < 0) nx = w - 1; else if (nx >= w) nx = 0;
                if (ny < 0 || ny >= h) continue;
                int nidx = ny * w + nx;
                if (countryOfPixel[nidx] == 0 && isLand(landSea[nidx])
                    && compOfPixel[nidx] == compId && !inFrontier[nidx]) {
                    frontier.push_back(nidx);
                    inFrontier[nidx] = true;
                }
            }
        }
    }

    // ── Fallback: assign unseeded land (0-slot components) to nearest country ──
    {
        std::vector<int> frontier2;
        std::vector<bool> inF2(w * h, false);
        // Seed frontier2 from all existing country pixels
        for (int i = 0; i < w * h; ++i) {
            if (countryOfPixel[i] > 0 || !isLand(landSea[i])) continue;
            int cx = i % w, cy = i / w;
            int nbs[4][2] = {{cx-1,cy},{cx+1,cy},{cx,cy-1},{cx,cy+1}};
            for (auto& nb : nbs) {
                int nx = nb[0], ny = nb[1];
                if (nx < 0) nx = w - 1; else if (nx >= w) nx = 0;
                if (ny < 0 || ny >= h) continue;
                int nidx = ny * w + nx;
                if (countryOfPixel[nidx] > 0 && !inF2[i]) {
                    frontier2.push_back(i);
                    inF2[i] = true;
                    break;
                }
            }
        }
        while (!frontier2.empty()) {
            int ri = rng() % (int)frontier2.size();
            int idx = frontier2[ri];
            std::swap(frontier2[ri], frontier2.back());
            frontier2.pop_back();
            inF2[idx] = false;
            if (countryOfPixel[idx] != 0) continue;
            int cx = idx % w, cy = idx / w;
            int ownerCid = 0;
            int nbs[4][2] = {{cx-1,cy},{cx+1,cy},{cx,cy-1},{cx,cy+1}};
            for (auto& nb : nbs) {
                int nx = nb[0], ny = nb[1];
                if (nx < 0) nx = w - 1; else if (nx >= w) nx = 0;
                if (ny < 0 || ny >= h) continue;
                int nid = countryOfPixel[ny * w + nx];
                if (nid > 0) { ownerCid = nid; break; }
            }
            if (ownerCid == 0) continue;
            countryOfPixel[idx] = ownerCid;
            for (auto& nb : nbs) {
                int nx = nb[0], ny = nb[1];
                if (nx < 0) nx = w - 1; else if (nx >= w) nx = 0;
                if (ny < 0 || ny >= h) continue;
                int nidx = ny * w + nx;
                if (countryOfPixel[nidx] == 0 && isLand(landSea[nidx]) && !inF2[nidx]) {
                    frontier2.push_back(nidx);
                    inF2[nidx] = true;
                }
            }
        }
    }

    // ── Fallback 2: orphan components (no country at all) → nearest country centroid ──
    {
        // Find all unassigned land pixels
        std::vector<int> unassigned;
        for (int i = 0; i < w * h; ++i)
            if (countryOfPixel[i] == 0 && isLand(landSea[i]))
                unassigned.push_back(i);
        if (!unassigned.empty()) {
            // Compute centroids of all existing countries
            std::map<int, double> cxSum, cySum, cnt;
            for (int i = 0; i < w * h; ++i) {
                int cid = countryOfPixel[i];
                if (cid > 0) {
                    cxSum[cid] += i % w;
                    cySum[cid] += i / w;
                    cnt[cid] += 1.0;
                }
            }
            std::vector<std::pair<int, double>> centroids;
            for (auto& kv : cnt) {
                int cid = kv.first;
                double cx = cxSum[cid] / cnt[cid];
                double cy = cySum[cid] / cnt[cid];
                centroids.push_back({cid, 0});
            }

            // Group unassigned by component
            std::vector<int> compOfU(w * h, 0);
            int nextComp = 1;
            std::vector<std::vector<int>> orphanComps;
            for (int idx : unassigned) {
                if (compOfU[idx] != 0) continue;
                std::vector<int> stack, pix;
                stack.push_back(idx); compOfU[idx] = nextComp;
                while (!stack.empty()) {
                    int cur = stack.back(); stack.pop_back();
                    pix.push_back(cur);
                    int cx = cur % w, cy = cur / w;
                    int nbs[4][2] = {{cx-1,cy},{cx+1,cy},{cx,cy-1},{cx,cy+1}};
                    for (auto& nb : nbs) {
                        int nx = nb[0]; if (nx < 0) nx = w - 1; else if (nx >= w) nx = 0;
                        int ny = nb[1]; if (ny < 0 || ny >= h) continue;
                        int ni = ny * w + nx;
                        if (compOfU[ni] == 0 && isLand(landSea[ni]) && countryOfPixel[ni] == 0) {
                            compOfU[ni] = nextComp;
                            stack.push_back(ni);
                        }
                    }
                }
                orphanComps.push_back(pix);
                ++nextComp;
            }

            for (auto& pix : orphanComps) {
                // Centroid of orphan
                double ox = 0, oy = 0;
                for (int p : pix) { ox += p % w; oy += p / w; }
                ox /= pix.size(); oy /= pix.size();
                // Find nearest country centroid
                int bestCid = 0;
                double bestDist = 1e30;
                for (auto& kv : cnt) {
                    int cid2 = kv.first;
                    double cx2 = cxSum[cid2] / cnt[cid2];
                    double cy2 = cySum[cid2] / cnt[cid2];
                    double dx = ox - cx2, dy = oy - cy2;
                    double d = dx * dx + dy * dy;
                    if (d < bestDist) { bestDist = d; bestCid = cid2; }
                }
                if (bestCid > 0)
                    for (int p : pix) countryOfPixel[p] = bestCid;
            }
            std::cout << "  Assigned " << unassigned.size() << " orphan pixels to nearest country\n";
        }
    }

    // Collect all country IDs
    std::vector<int> countryIds;
    std::map<int, std::vector<int>> countryPixels;
    for (int i = 0; i < w * h; ++i) {
        int cid = countryOfPixel[i];
        if (cid > 0) {
            if (countryPixels.find(cid) == countryPixels.end()) countryIds.push_back(cid);
            countryPixels[cid].push_back(i);
        }
    }
    std::cout << "  Created " << countryIds.size() << " countries\n";

    // ── Step 3: Generate provinces via flood fill within each country ──
    out.provincePixels.resize(w * h, Color{0,0,0,0});
    nlohmann::json provinceJson;
    int nextProvinceId = 1;

    auto idToPixel = [](int id) -> Color { return idToColor(id); };

    for (int cid : countryIds) {
        auto& countryPix = countryPixels[cid];
        if (countryPix.empty()) continue;
        int np = (int)countryPix.size();

        float targetSize = 2500.0f / provinceDensity;
        int numProv = std::max(1, std::min(200, (int)((float)np / targetSize)));

        if (numProv == 1) {
            int pid = nextProvinceId++;
            nlohmann::json entry;
            entry["id"] = pid;
            entry["name"] = "Province #" + std::to_string(pid);
            entry["country_id"] = cid;
            entry["iso_a3"] = "";
            char hex[8]; snprintf(hex, sizeof(hex), "#%06x", pid);
            entry["color"] = std::string(hex);
            provinceJson[std::to_string(pid)] = entry;
            Color pc = idToPixel(pid);
            for (int p : countryPix) out.provincePixels[p] = pc;
            continue;
        }

        float avgArea = (float)np / numProv;
        float minDist = std::sqrt(avgArea * 0.5f);
        float minDistSq = minDist * minDist;

        std::vector<int> shuffled = countryPix;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        std::vector<int> seeds;

        for (int idx : shuffled) {
            if ((int)seeds.size() >= numProv) break;
            int cx = idx % w, cy = idx / w;
            float density = 1.0f;
            for (int o = 0; o < 3; ++o)
                density += (fbmNoise((float)cx * 0.003f, (float)cy * 0.003f, seed + o * 137) - 0.5f) * 0.5f;
            density = std::max(0.4f, std::min(1.6f, density));
            float adjMinDistSq = minDistSq / (density * density);
            bool tooClose = false;
            for (int s : seeds) {
                int sx = s % w, sy = s / w;
                int dx = cx - sx, dy = cy - sy;
                if (dx * dx + dy * dy < adjMinDistSq) { tooClose = true; break; }
            }
            if (!tooClose) seeds.push_back(idx);
        }
        for (int idx : shuffled) {
            if ((int)seeds.size() >= numProv) break;
            bool already = false;
            for (int s : seeds) if (s == idx) { already = true; break; }
            if (!already) seeds.push_back(idx);
        }

        std::vector<int> seedProvIds(seeds.size());
        for (size_t si = 0; si < seeds.size(); ++si) {
            int pid = nextProvinceId++;
            seedProvIds[si] = pid;
            nlohmann::json entry;
            entry["id"] = pid;
            entry["name"] = "Province #" + std::to_string(pid);
            entry["country_id"] = cid;
            entry["iso_a3"] = "";
            char hex[8]; snprintf(hex, sizeof(hex), "#%06x", pid);
            entry["color"] = std::string(hex);
            provinceJson[std::to_string(pid)] = entry;
            out.provincePixels[seeds[si]] = idToPixel(pid);
        }

        std::vector<int> frontier;
        std::vector<bool> inFrontier(w * h, false);
        for (size_t si = 0; si < (int)seeds.size(); ++si) {
            int sx = seeds[si] % w, sy = seeds[si] / w;
            int nbs[4][2] = {{sx-1,sy},{sx+1,sy},{sx,sy-1},{sx,sy+1}};
            for (auto& nb : nbs) {
                int nx = nb[0], ny = nb[1];
                if (nx < 0) nx = w - 1; else if (nx >= w) nx = 0;
                if (ny < 0 || ny >= h) continue;
                int nidx = ny * w + nx;
                if (colorToId(out.provincePixels[nidx]) == 0 && isLand(landSea[nidx])
                    && countryOfPixel[nidx] == cid && !inFrontier[nidx]) {
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

            if (colorToId(out.provincePixels[idx]) != 0) continue;
            int cx = idx % w, cy = idx / w;
            Color owner{0,0,0,0};
            int nbs[4][2] = {{cx-1,cy},{cx+1,cy},{cx,cy-1},{cx,cy+1}};
            for (auto& nb : nbs) {
                int nx = nb[0], ny = nb[1];
                if (nx < 0) nx = w - 1; else if (nx >= w) nx = 0;
                if (ny < 0 || ny >= h) continue;
                int nidx = ny * w + nx;
                if (countryOfPixel[nidx] != cid) continue;
                Color nv = out.provincePixels[nidx];
                if (colorToId(nv) != 0) { owner = nv; break; }
            }
            if (colorToId(owner) == 0) continue;
            out.provincePixels[idx] = owner;
            for (auto& nb : nbs) {
                int nx = nb[0], ny = nb[1];
                if (nx < 0) nx = w - 1; else if (nx >= w) nx = 0;
                if (ny < 0 || ny >= h) continue;
                int nidx = ny * w + nx;
                if (colorToId(out.provincePixels[nidx]) == 0 && isLand(landSea[nidx])
                    && countryOfPixel[nidx] == cid && !inFrontier[nidx]) {
                    frontier.push_back(nidx);
                    inFrontier[nidx] = true;
                }
            }
        }
    }

    // ── Step 4: Merge tiny provinces ──
    {
        const int MIN_PROV_PIXELS = 80;
        std::map<int, int> provSizes;
        std::map<int, std::set<int>> ridProvinces; // country -> provinces
        for (int i = 0; i < w * h; ++i) {
            int pid = colorToId(out.provincePixels[i]);
            if (pid == 0) continue;
            provSizes[pid]++;
            int rid = countryOfPixel[i];
            ridProvinces[rid].insert(pid);
        }
        std::set<int> tinyProvs;
        for (auto& [pid, sz] : provSizes)
            if (sz < MIN_PROV_PIXELS) tinyProvs.insert(pid);

        for (int pid : tinyProvs) {
            std::vector<int> pixs;
            for (int i = 0; i < w * h; ++i)
                if (colorToId(out.provincePixels[i]) == pid) pixs.push_back(i);
            if (pixs.empty()) continue;
            int myRid = countryOfPixel[pixs[0]];
            if ((int)ridProvinces[myRid].size() <= 1) continue;

            // Find best neighbor province in same country
            int bestNbr = 0;
            for (int radius : {1, 2, 3, 5, 10, 20, 50}) {
                std::map<int, int> votes;
                for (int idx : pixs) {
                    int x = idx % w, y = idx / w;
                    for (int dy = -radius; dy <= radius; ++dy) {
                        for (int dx = -radius; dx <= radius; ++dx) {
                            if (dx == 0 && dy == 0) continue;
                            int nx = x + dx, ny = y + dy;
                            if (nx < 0) nx = w - 1; else if (nx >= w) nx = 0;
                            if (ny < 0 || ny >= h) continue;
                            int nidx = ny * w + nx;
                            if (countryOfPixel[nidx] != myRid) continue;
                            int npid = colorToId(out.provincePixels[nidx]);
                            if (npid != pid && npid != 0) votes[npid]++;
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
            if (bestNbr > 0) {
                Color bc = idToPixel(bestNbr);
                for (int idx : pixs) out.provincePixels[idx] = bc;
            }
        }
        for (int pid : tinyProvs) provinceJson.erase(std::to_string(pid));
    }

    // ── Step 5: Generate country colors (graph coloring) ──
    // Add UNC and BLC
    const int UNC_CID = 65534, BLC_CID = 65535;

    std::map<int, std::set<int>> adj;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int i = y * w + x;
            int rid = countryOfPixel[i];
            if (rid == 0 || !isLand(landSea[i])) continue;
            // X-wrapping: check neighbor to the right (including wrap at seam)
            int nx = x + 1 == w ? 0 : x + 1;
            int rid2 = countryOfPixel[y * w + nx];
            if (rid2 > 0 && rid2 != rid) { adj[rid].insert(rid2); adj[rid2].insert(rid); }
            if (y < h - 1) {
                int rid2 = countryOfPixel[i + w];
                if (rid2 > 0 && rid2 != rid) { adj[rid].insert(rid2); adj[rid2].insert(rid); }
            }
        }
    }

    std::map<int, Color> countryColors;
    for (int cid : countryIds) {
        float bestHue = fmodf(cid * 0.618033988749895f, 1.0f);
        float bestScore = -1e9f;
        for (int ci = 0; ci < 360; ++ci) {
            float h = ci / 360.0f;
            float minDist = 1.0f;
            auto it = adj.find(cid);
            if (it != adj.end()) {
                for (int nid : it->second) {
                    if (nid == UNC_CID || nid == BLC_CID) continue;
                    auto it2 = countryColors.find(nid);
                    if (it2 != countryColors.end()) {
                        Color oc = it2->second;
                        // Convert to hue approximation
                        float r = oc.r/255.0f, g = oc.g/255.0f, b = oc.b/255.0f;
                        float mx = std::max({r,g,b}), mn = std::min({r,g,b});
                        float nh = 0;
                        if (mx == mn) nh = 0;
                        else if (mx == r) nh = fmodf((g-b)/(mx-mn), 6.0f)/6.0f;
                        else if (mx == g) nh = ((b-r)/(mx-mn)+2.0f)/6.0f;
                        else nh = ((r-g)/(mx-mn)+4.0f)/6.0f;
                        if (nh < 0) nh += 1.0f;
                        float d = fabsf(h - nh);
                        if (d > 0.5f) d = 1.0f - d;
                        if (d < minDist) minDist = d;
                    }
                }
            }
            float hd = fabsf(h - bestHue);
            if (hd > 0.5f) hd = 1.0f - hd;
            float score = minDist * 4.0f - hd;
            if (score > bestScore) { bestScore = score; bestHue = h; }
        }
        // Vary saturation (0.45-0.85) and value (0.55-0.9) per country for visual distinction
        float sat = 0.45f + fmodf(cid * 0.137f, 0.4f);
        float val = 0.55f + fmodf(cid * 0.237f, 0.35f);
        int hi = (int)(bestHue * 6.0f);
        float f = bestHue * 6.0f - hi;
        float p = val * (1.0f - sat);
        float q = val * (1.0f - sat * f);
        float t = val * (1.0f - sat * (1.0f - f));
        float r, g, b;
        switch (hi % 6) {
            case 0: r=val; g=t; b=p; break;
            case 1: r=q; g=val; b=p; break;
            case 2: r=p; g=val; b=t; break;
            case 3: r=p; g=q; b=val; break;
            case 4: r=t; g=p; b=val; break;
            default: r=val; g=p; b=q; break;
        }
        countryColors[cid] = {(uint8_t)(r*255), (uint8_t)(g*255), (uint8_t)(b*255), 255};
    }
    countryColors[UNC_CID] = {140,140,140,255};
    countryColors[BLC_CID] = {80,80,80,255};

    // ── Step 6: Build political.png ──
    out.politicalPixels.resize(w * h, Color{0,0,0,0});
    for (int i = 0; i < w * h; ++i) {
        if (!isLand(landSea[i])) {
            out.politicalPixels[i] = Color{35, 60, 80, 255}; // sea
            continue;
        }
        int rid = countryOfPixel[i];
        if (rid == 0) {
            out.politicalPixels[i] = Color{140,140,140,255};
        } else if (countryColors.count(rid)) {
            out.politicalPixels[i] = countryColors[rid];
        } else {
            out.politicalPixels[i] = Color{140,140,140,255};
        }
    }
    // ── Step 7: Build countries.json ──
    nlohmann::json countriesJson;
    auto genFlag = [&](int cid, int rr, int gg, int bb) -> nlohmann::json {
        std::mt19937 frng((unsigned int)(cid * 99991u));
        std::uniform_int_distribution<int> patDist(0, 14);
        int pattern = patDist(frng);
        auto h = [](int r, int g, int b) {
            char buf[8]; snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
            return std::string(buf);
        };
        // Generate contrasting colors: lightened and darkened variants
        int r2 = std::min(rr + 60, 255), g2 = std::min(gg + 60, 255), b2 = std::min(bb + 60, 255);
        int r3 = std::max(rr - 40, 0), g3 = std::max(gg - 40, 0), b3 = std::max(bb - 40, 0);
        int r4 = std::max(rr - 80, 0), g4 = std::max(gg - 80, 0), b4 = std::max(bb - 80, 0);
        // Complementary-ish color for accent
        int rc = 255 - rr, gc = 255 - gg, bc = 255 - bb;
        int rw = 255, gw = 255, bw = 255; // white

        nlohmann::json f;
        switch (pattern) {
            case 0: f["type"] = "solid"; f["colors"] = {h(rr,gg,bb)}; break;
            case 1: f["type"] = "hstripes_3"; f["colors"] = {h(r3,g3,b3), h(rr,gg,bb), h(r3,g3,b3)}; break;
            case 2: f["type"] = "vstripes_3"; f["colors"] = {h(r3,g3,b3), h(rr,gg,bb), h(r3,g3,b3)}; break;
            case 3: f["type"] = "diagonal_r"; f["colors"] = {h(rr,gg,bb), h(r4,g4,b4)}; break;
            case 4: f["type"] = "diagonal_l"; f["colors"] = {h(r2,g2,b2), h(rr,gg,bb)}; break;
            case 5: f["type"] = "pale"; f["colors"] = {h(r3,g3,b3), h(rr,gg,bb), h(r3,g3,b3)}; break;
            case 6: f["type"] = "fess"; f["colors"] = {h(r3,g3,b3), h(rr,gg,bb), h(r3,g3,b3)}; break;
            case 7: f["type"] = "cross_greek"; f["colors"] = {h(rr,gg,bb), h(rw,gw,bw)}; break;
            case 8: f["type"] = "saltir"; f["colors"] = {h(rr,gg,bb), h(rw,gw,bw)}; break;
            case 9: f["type"] = "triangle"; f["colors"] = {h(r2,g2,b2), h(rr,gg,bb)}; break;
            case 10: f["type"] = "canton"; f["colors"] = {h(rr,gg,bb), h(r2,g2,b2)}; break;
            case 11: f["type"] = "quartered"; f["colors"] = {h(rr,gg,bb), h(r2,g2,b2), h(r2,g2,b2), h(rr,gg,bb)}; break;
            case 12: f["type"] = "cross_nordic"; f["colors"] = {h(rr,gg,bb), h(rw,gw,bw)}; break;
            case 13: f["type"] = "sunburst"; f["colors"] = {h(rr,gg,bb), h(r2,g2,b2)}; break;
            default: f["type"] = "solid"; f["colors"] = {h(rr,gg,bb)}; break;
        }

        // Add overlay symbol for visual interest (~50% chance)
        std::uniform_int_distribution<int> symDist(0, 9);
        int symroll = symDist(frng);
        if (symroll < 5) {
            nlohmann::json sym;
            static const char* symTypes[] = {"star_5","circle","cross_latin","crescent","diamond","sun","gear","mountain","cross_saltir","star_6"};
            sym["type"] = symTypes[patDist(frng) % 10];
            sym["colors"] = {h(rw,gw,bw)};
            sym["x"] = 0.5; sym["y"] = 0.5;
            sym["size"] = 0.22 + patDist(frng) * 0.02f;
            // Some flags get centered symbols, some get offset
            if (pattern == 10) { sym["x"] = 0.25; sym["y"] = 0.25; sym["size"] = 0.18; }
            f["symbols"] = {sym};
        }

        return f;
    };
    for (int cid : countryIds) {
        Color col = countryColors[cid];
        std::string name = generateCountryName(cid, seed);
        nlohmann::json entry;
        entry["id"] = cid;
        entry["name"] = name;
        entry["iso_a3"] = "";
        char hex[8]; snprintf(hex, sizeof(hex), "#%02x%02x%02x", col.r, col.g, col.b);
        entry["color"] = std::string(hex);
        entry["flag_actual"] = genFlag(cid, col.r, col.g, col.b);
        entry["flag_censored"] = genFlag(cid + 1000, col.r, col.g, col.b);
        entry["treasury"] = 5000.0;
        entry["compass_economic"] = (float)((cid * 7 + 3) % 201 - 100) * 0.5f; // -50..+50
        entry["compass_social"] = (float)((cid * 13 + 7) % 201 - 100) * 0.5f;   // -50..+50
        entry["doctrine"] = "";
        entry["research"] = nlohmann::json::array();
        countriesJson[std::to_string(cid)] = entry;
    }
    // UNC/BLC entries
    {
        nlohmann::json unc;
        unc["id"] = UNC_CID; unc["name"] = "Unclaimed"; unc["iso_a3"] = "UNC";
        unc["color"] = "#8c8c8c"; unc["flag_actual"] = genFlag(UNC_CID, 140,140,140);
        unc["flag_censored"] = genFlag(UNC_CID, 140,140,140);
        countriesJson[std::to_string(UNC_CID)] = unc;
        nlohmann::json blc;
        blc["id"] = BLC_CID; blc["name"] = "Blocked Territory"; blc["iso_a3"] = "BLC";
        blc["color"] = "#505050"; blc["flag_actual"] = genFlag(BLC_CID, 80,80,80);
        blc["flag_censored"] = genFlag(BLC_CID, 80,80,80);
        countriesJson[std::to_string(BLC_CID)] = blc;
    }
    out.countryJson = countriesJson.dump(2);

    // ── Step 8: Build province JSON (update with country_ids) ──
    // Update province entries with correct country_id
    for (int i = 0; i < w * h; ++i) {
        int pid = colorToId(out.provincePixels[i]);
        if (pid == 0) continue;
        int rid = countryOfPixel[i];
        if (rid > 0 && provinceJson.contains(std::to_string(pid))) {
            provinceJson[std::to_string(pid)]["country_id"] = rid;
        }
    }
    out.provinceJson = provinceJson.dump(2);

    // ── Step 9: Generate population.json ──
    // Population proportional to province pixel count, using noise for variation
    {
        nlohmann::json popJson;
        std::map<int, long long> provPop;
        std::map<int, int> provPixels;
        for (int i = 0; i < w * h; ++i) {
            int pid = colorToId(out.provincePixels[i]);
            if (pid == 0) continue;
            provPixels[pid]++;
        }
        long long totalLand = 0;
        for (auto& [pid, cnt] : provPixels) totalLand += cnt;
        // Assume total world pop ~200M for a procedural map (scale by land area)
        // ~50 people per land pixel on average
        long long totalPop = (long long)(totalLand * 50);
        for (auto& [pid, cnt] : provPixels) {
            int x = 0, y = 0, n = 0;
            for (int i = 0; i < w * h && n < 1; ++i) {
                if (colorToId(out.provincePixels[i]) == pid) { x = i % w; y = i / w; n++; }
            }
            float noiseVar = 0.5f + fbmNoise((float)x * 0.005f, (float)y * 0.005f, seed + 500);
            // Lat factor: lower pop at high latitudes
            float latFactor = 1.0f - fabsf((float)(y - h/2) / (float)(h/2)) * 0.5f;
            long long pop = (long long)((double)cnt / totalLand * totalPop * noiseVar * latFactor);
            pop = std::max(pop, 100LL);
            popJson[std::to_string(pid)] = pop;
        }
        out.populationJson = popJson.dump(2);
    }

    // ── Step 10: Generate resources.json (industry + resources) ──
    {
        nlohmann::json resJson;
        for (int i = 0; i < w * h; ++i) {
            int pid = colorToId(out.provincePixels[i]);
            if (pid == 0) continue;
            if (resJson.contains(std::to_string(pid))) continue;
            int x = i % w, y = i / w;
            // Industry: base level 0-5 based on noise + population density
            float indNoise = fbmNoise((float)x * 0.002f, (float)y * 0.002f, seed + 1000);
            int indLevel = (int)(indNoise * 4.0f);
            float income = 5.0f + indNoise * 25.0f;

            // Resources: oil, metal, gold, rubber based on noise
            float oilN = fbmNoise((float)x * 0.001f, (float)y * 0.001f, seed + 1100);
            float metalN = fbmNoise((float)x * 0.0015f, (float)y * 0.0015f, seed + 1200);
            float goldN = fbmNoise((float)x * 0.002f, (float)y * 0.002f, seed + 1300);
            float rubberN = fbmNoise((float)x * 0.003f, (float)y * 0.003f, seed + 1400);

            nlohmann::json entry;
            entry["industry"] = {
                {"level", indLevel},
                {"income", income},
                {"resourceIncome", 0.0f},
                {"popIncome", 0.0f}
            };

            if (oilN > 0.65f) entry["oil"] = {{"a", (int)((oilN - 0.65f) * 200)}, {"b", 5}};
            if (metalN > 0.6f) entry["metal"] = {{"a", (int)((metalN - 0.6f) * 200)}, {"b", 8}};
            if (goldN > 0.7f) entry["gold"] = {{"a", (int)((goldN - 0.7f) * 100)}, {"b", 15}};
            if (rubberN > 0.65f) entry["rubber"] = {{"a", (int)((rubberN - 0.65f) * 150)}, {"b", 10}};

            resJson[std::to_string(pid)] = entry;
        }
        out.resourcesJson = resJson.dump(2);
    }

    return out;
}
