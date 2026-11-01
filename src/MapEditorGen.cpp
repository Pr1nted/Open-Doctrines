// Hot generation kernels split out of MapEditor.cpp so this one translation
// unit can be built with optimisation even in a Debug build (see CMakeLists.txt).
// These run per-pixel over the full 8192x4096 map; at -O0 that was tens of
// seconds of frozen UI per "Generate World". The rest of the editor stays at
// the build's own optimisation level so it remains easy to step-debug.
//
// Everything here is ordinary MapEditor member functions; only their file
// moved. pnoise/pfbm are generateMap's private noise helpers and live here too.
#include "MapEditor.h"
#include "util/LoadLog.h"
#include "renderer/MapRenderer.h"
#include "map/Province.h"
#include "raylib.h"
#include <cmath>
#include <algorithm>
#include <random>
#include <cstdint>
#include <vector>

// The generator works at half resolution and upscales ×2, while land/sea is
// full resolution — coastlines end up with sea pixels that still carry a
// province color (uneditable specks) and land pixels with none. Strip the
// former, then run the orphan-land pass to fix the latter.
void MapEditor::computePoliticalGradient() {
    if (!m_hasProvinces || m_provincePixels.empty()) return;
    int total = MAP_W * MAP_H;

    // cid per pixel (0 = sea/unclaimed), same source data the game builds
    // m_pixelCountryArray from.
    std::vector<int> cidArr(total, 0);
    for (int i = 0; i < total; ++i) {
        const Color& pc = m_provincePixels[i];
        int pid = Province::colorToId(pc.r, pc.g, pc.b);
        if (pid == 0) continue;
        if (Province* prov = m_editProvinces.getProvinceById(pid)) cidArr[i] = prov->countryId;
    }

    // Multi-source BFS distance-to-border field, identical weights to
    // Game_Loading.cpp's gradient pass (orth +2, diag +3, capped at 60).
    std::vector<uint8_t> dist(total, 255);
    struct QE { int idx; uint8_t dist; };
    std::vector<QE> queue;
    auto enqueue = [&](int idx, uint8_t d) {
        if (idx < 0 || idx >= total) return;
        if (dist[idx] <= d) return;
        dist[idx] = d;
        queue.push_back({idx, d});
    };
    for (int y = 0; y < MAP_H; ++y) {
        for (int x = 0; x < MAP_W; ++x) {
            int i = y * MAP_W + x;
            int cid = cidArr[i];
            int nx4[4] = {x-1, x+1, x, x};
            int ny4[4] = {y, y, y-1, y+1};
            for (int k = 0; k < 4; ++k) {
                if (nx4[k] < 0 || nx4[k] >= MAP_W || ny4[k] < 0 || ny4[k] >= MAP_H) continue;
                if (cidArr[ny4[k] * MAP_W + nx4[k]] != cid) { enqueue(i, 0); break; }
            }
        }
    }
    size_t qpos = 0;
    while (qpos < queue.size()) {
        QE cur = queue[qpos++];
        if (cur.dist >= 60) continue;
        int x = cur.idx % MAP_W, y = cur.idx / MAP_W;
        int nx8[8] = {x-1, x+1, x, x, x-1, x-1, x+1, x+1};
        int ny8[8] = {y, y, y-1, y+1, y-1, y+1, y-1, y+1};
        for (int k = 0; k < 8; ++k) {
            if (nx8[k] < 0 || nx8[k] >= MAP_W || ny8[k] < 0 || ny8[k] >= MAP_H) continue;
            int step = (k < 4) ? 2 : 3;
            enqueue(ny8[k] * MAP_W + nx8[k], cur.dist + step);
        }
    }

    // Same blend formula as Game_Loading.cpp's static blendColor().
    auto blend = [](Color base, float t) -> Color {
        uint8_t r = (uint8_t)std::min(255, std::max(0, (int)(base.r * (1.0f - t * 0.4f) + 40.0f * t * 0.3f)));
        uint8_t g = (uint8_t)std::min(255, std::max(0, (int)(base.g * (1.0f - t * 0.4f) + 40.0f * t * 0.3f)));
        uint8_t b = (uint8_t)std::min(255, std::max(0, (int)(base.b * (1.0f - t * 0.4f) + 40.0f * t * 0.3f)));
        return Color{r, g, b, 255};
    };
    for (int i = 0; i < total; ++i) {
        int cid = cidArr[i];
        float t = std::min(1.0f, dist[i] / 60.0f);
        if (cid <= 0) {
            uint8_t rb = (uint8_t)(8 + (uint8_t)((1.0f - t) * 16));
            uint8_t gb = (uint8_t)(10 + (uint8_t)((1.0f - t) * 22));
            uint8_t bb = (uint8_t)(15 + (uint8_t)((1.0f - t) * 38));
            m_politicalPixels[i] = {rb, gb, bb, 255};
        } else {
            const Country* c = m_editCountries.getCountry(cid);
            Color base = c ? c->color : Color{80, 80, 80, 255};
            m_politicalPixels[i] = blend(base, t);
        }
    }

    // 1px dark border line at country boundaries.
    for (int y = 0; y < MAP_H; ++y) {
        for (int x = 0; x < MAP_W; ++x) {
            int i = y * MAP_W + x;
            int cid = cidArr[i];
            if (cid <= 0) continue;
            int nx4[4] = {x-1, x+1, x, x};
            int ny4[4] = {y, y, y-1, y+1};
            for (int k = 0; k < 4; ++k) {
                if (nx4[k] < 0 || nx4[k] >= MAP_W || ny4[k] < 0 || ny4[k] >= MAP_H) continue;
                if (cidArr[ny4[k] * MAP_W + nx4[k]] != cid) {
                    Color& px = m_politicalPixels[i];
                    px = {(uint8_t)(px.r / 3), (uint8_t)(px.g / 3), (uint8_t)(px.b / 3), 255};
                    break;
                }
            }
        }
    }

    if (m_renderer) m_renderer->updatePoliticalTexture(m_politicalPixels.data());
}

void MapEditor::sanitizeProvincePixels() {
    if (!m_hasProvinces || m_provincePixels.empty()) return;
    const Color SEA_POL = {35, 60, 80, 255};
    for (int y = 0; y < MAP_H; ++y) {
        for (int x = 0; x < MAP_W; ++x) {
            int idx = y * MAP_W + x;
            const Color& pp = m_provincePixels[idx];
            int pid = Province::colorToId(pp.r, pp.g, pp.b);
            if (pid == 0) continue;
            bool bad = !m_editLandSea.isLand(x, y);                       // province color on sea
            if (!bad && !m_editProvinces.getProvinceById(pid)) bad = true; // color of no known province
            if (bad) {
                auto it = m_provincePixelCounts.find(pid);
                if (it != m_provincePixelCounts.end()) it->second--;
                m_provincePixels[idx] = Color{0, 0, 0, 0};
                m_politicalPixels[idx] = m_editLandSea.isLand(x, y)
                                             ? Color{140, 140, 140, 255} : SEA_POL;
            }
        }
    }
    // Orphan land (including half-res gaps at coastlines) joins its touching
    // province or becomes a new unclaimed province
    resetStrokeBBox();
    growStrokeBBox(0, 0, MAP_W - 1, MAP_H - 1);
    m_strokeWrapped = true;
    finalizeLandStroke();
    resetStrokeBBox();
}

// ── Periodic noise with hash-grid X wrapping ───────────────────
// period = number of integer hash-grid cells before wrapping.
// For pixel-perfect seam: pass period = (int)(MAP_W * scale) so
// the hash at pixel 0 and pixel MAP_W-1 land in the same cell.
static float pnoise(float x, float y, int seed, int period) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy;
    fx = fx*fx*(3-2*fx); fy = fy*fy*(3-2*fy);

    auto hash = [&](int hx, int hy) -> float {
        hx = hx % period;
        if (hx < 0) hx += period;
        unsigned int h = (unsigned int)(hx * 374761393 + hy * 668265263 + seed * 982451653);
        h = (h ^ (h >> 13)) * 1274126177u;
        return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
    };
    float a = hash(ix, iy), b = hash(ix+1, iy);
    float c = hash(ix, iy+1), d = hash(ix+1, iy+1);
    return a + (b-a)*fx + (c-a)*fy + (a-b-c+d)*fx*fy;
}

static float pfbm(float x, float y, int oct, int seed, int period) {
    float val = 0, amp = 1, freq = 1, maxVal = 0;
    for (int i = 0; i < oct; i++) {
        val += pnoise(x*freq, y*freq, seed+i*1000, period) * amp;
        maxVal += amp; amp *= 0.5f; freq *= 2.0f;
    }
    return val / maxVal;
}

void MapEditor::generateMap(const GeneratorParams& p) {
    m_genParams = p;
    // Clear province/country state when generating new landmass
    m_hasProvinces = false;
    m_hasGameData = false;
    m_provincePixels.clear();
    m_provinceJson.clear();
    m_politicalPixels.clear();
    m_countryJson.clear();
    m_populationJson.clear();
    m_resourcesJson.clear();
    m_minoritiesJson.clear();
    m_minorityColorsJson.clear();
    m_ethnicColors.clear();
    m_provinceData.clear();
    m_provincePixelCounts.clear();
    m_editorShips.clear();
    m_editorRelations.clear();
    m_countryPolicies.clear();
    m_ethnicityPolicies.clear();
    m_ethnicRelations.clear();
    m_thumbnailPath.clear();
    m_thumbnailTexPath.clear();
    m_editorClaims.clear();
    m_claimsPixels.clear();
    m_claimsOverlayCid = -2;
    m_selectedProvince = -1;
    m_selectedCountry = -1;
    resetStrokeBBox();
    if (m_renderer) {
        m_renderer->setPoliticalTexture(Texture2D{}); // clear political overlay
        m_renderer->setEditorOverlay(Texture2D{});
        m_renderer->setShowEditorOverlay(false);
        m_buildingCentroids.clear();
        m_renderer->clearHighlight();
    }
    int w = MAP_W, h = MAP_H;
    std::mt19937 rng((unsigned int)p.seed);

    // ── Continent centers with toroidal placement ──
    struct Center { float x, y; };
    std::vector<Center> centers;
    float minDist = (float)w / p.numContinents * 0.45f;
    for (int i = 0; i < p.numContinents; i++) {
        for (int attempt = 0; attempt < 50; attempt++) {
            float cx = (float)(rng() % w);
            float cy = h * 0.2f + (rng() % (int)(h * 0.6f));
            bool ok = true;
            for (auto& c : centers) {
                float dx = fabsf(cx - c.x);
                dx = fminf(dx, (float)w - dx);
                if (sqrtf(dx*dx + (cy-c.y)*(cy-c.y)) < minDist) { ok = false; break; }
            }
            if (ok) { centers.push_back({cx, cy}); break; }
        }
        if (centers.size() <= (size_t)i)
            centers.push_back({(float)(rng() % w), h * 0.2f + (rng() % (int)(h * 0.6f))});
    }

    // Ghost copies for wrapping
    auto distToLand = [&](float px, float py) -> float {
        float best = 1e9f;
        for (auto& c : centers) {
            // Toroidal distance: wrap X
            float dx = fabsf(px - c.x);
            dx = fminf(dx, (float)w - dx);
            float dy = py - c.y;
            best = fminf(best, sqrtf(dx*dx + dy*dy));
        }
        return best;
    };

    float zoom = 0.0035f;
    float warpStr = 80.0f + p.jaggedness * 120.0f;
    float threshold = 0.48f - p.landCoverage * 0.22f;
    float contRad = (float)w / p.numContinents * 0.65f;

    // Period for each noise scale: number of hash-grid cells across one map width.
    // This ensures noise(px=0) and noise(px=MAP_W-1) share the same hash cell.
    int pWarp  = std::max(1, (int)((float)w * zoom * 0.3f));   // ~8
    int pFine  = std::max(1, (int)((float)w * zoom * 2.0f));   // ~57
    int pBase  = std::max(1, (int)((float)w * zoom));           // ~28
    int pScat  = std::max(1, (int)((float)w * 0.02f));          // ~163

    // ── Generate elevation at 1/4 resolution (16x fewer pixels) ──
    const int SCALE = 4;
    int sw = w / SCALE, sh = h / SCALE;
    std::vector<float> smallElev(sw * sh);
    for (int py = 0; py < sh; py++) {
        for (int px = 0; px < sw; px++) {
            float fullX = (float)(px * SCALE), fullY = (float)(py * SCALE);
            float wx = fullX, wy = fullY;

            float w1 = pfbm(wx * zoom * 0.3f, wy * zoom * 0.3f, 3, p.seed + 10, pWarp);
            float w2 = pfbm(wx * zoom * 0.3f + 50, wy * zoom * 0.3f + 50, 3, p.seed + 20, pWarp);
            wx += (w1 - 0.5f) * warpStr;
            wy += (w2 - 0.5f) * warpStr;

            float w3 = pfbm(wx * zoom * 2.0f, wy * zoom * 2.0f, 2, p.seed + 30, pFine);
            float w4 = pfbm(wx * zoom * 2.0f + 100, wy * zoom * 2.0f + 100, 2, p.seed + 40, pFine);
            wx += (w3 - 0.5f) * warpStr * 0.4f;
            wy += (w4 - 0.5f) * warpStr * 0.4f;

            float n = pfbm(wx * zoom, wy * zoom, 6, p.seed, pBase);
            n = 1.0f - fabsf(n * 2.0f - 1.0f);

            float scatter = pfbm(fullX * 0.02f, fullY * 0.02f, 3, p.seed + 100, pScat);
            n += (scatter - 0.5f) * 0.12f;

            float d = distToLand(fullX, fullY);
            float mask = d / contRad;
            if (mask < 1.0f) mask = mask * mask;

            float val = n - mask * 0.55f;
            float latF = 1.0f - fabsf((float)(fullY - h/2) / (h/2));
            val -= (1.0f - latF) * 0.35f;

            smallElev[py * sw + px] = val;
        }
    }

    // ── Bilinear upscale elevation to full resolution ──
    std::vector<float> elevation(w * h);
    for (int py = 0; py < h; py++) {
        float sy = ((float)py + 0.5f) / SCALE - 0.5f;
        int iy = (int)sy; if (iy < 0) iy = 0; if (iy >= sh - 1) iy = sh - 2;
        float fy = sy - iy;
        for (int px = 0; px < w; px++) {
            float sx = ((float)px + 0.5f) / SCALE - 0.5f;
            int ix = (int)sx; if (ix < 0) ix = 0; if (ix >= sw - 1) ix = sw - 2;
            float fx = sx - ix;
            float v00 = smallElev[iy * sw + ix];
            float v10 = smallElev[iy * sw + ix + 1];
            float v01 = smallElev[(iy + 1) * sw + ix];
            float v11 = smallElev[(iy + 1) * sw + ix + 1];
            float v = v00 * (1-fx)*(1-fy) + v10 * fx*(1-fy) + v01 * (1-fx)*fy + v11 * fx*fy;
            elevation[py * w + px] = v;
        }
    }

    // ── X-seam: smooth elevation blend ──
    int seamW = 80;
    for (int py = 0; py < h; ++py) {
        float leftEdge = elevation[py * w];
        for (int b = 0; b < seamW; ++b) {
            float t = (float)b / (seamW - 1);
            int ri = py * w + (w - seamW + b);
            elevation[ri] = elevation[ri] * (1.0f - t) + leftEdge * t;
        }
        elevation[py * w + (w - 1)] = elevation[py * w];
    }

    // ── Threshold elevation to land/sea ──
    m_pixels.resize(w * h);
    for (int i = 0; i < w * h; ++i) {
        m_pixels[i] = (elevation[i] > threshold) ? COL_LAND : COL_SEA;
    }

    // ── Find inland lakes via edge-connected ocean flood fill ──
    auto isSea = [&](int i) { return m_pixels[i].b > m_pixels[i].g; }; // COL_SEA has b=120 > g=0
    std::vector<uint8_t> ocean(w * h, 0);
    std::vector<std::pair<int,int>> queue;
    for (int px = 0; px < w; px++) {
        if (isSea(px)) { ocean[px] = 1; queue.push_back({px, 0}); }
        if (isSea((h-1)*w + px)) { ocean[(h-1)*w + px] = 1; queue.push_back({px, h-1}); }
    }
    for (int py = 0; py < h; py++) {
        if (isSea(py*w)) { ocean[py*w] = 1; queue.push_back({0, py}); }
        if (isSea(py*w + w-1)) { ocean[py*w + w-1] = 1; queue.push_back({w-1, py}); }
    }
    while (!queue.empty()) {
        auto [x, y] = queue.back(); queue.pop_back();
        int nbs[4][2] = {{x-1,y},{x+1,y},{x,y-1},{x,y+1}};
        for (auto& nb : nbs) {
            int nx = nb[0], ny = nb[1];
            if (ny < 0 || ny >= h) continue;
            if (nx < 0) nx = w - 1; else if (nx >= w) nx = 0;
            int idx = ny * w + nx;
            if (isSea(idx) && !ocean[idx]) { ocean[idx] = 1; queue.push_back({nx, ny}); }
        }
    }

    // Lakes are already COL_SEA (no visual change), but they're now identified.
    // They'll remain as COL_SEA — visually they're already "lakes" (sea inside land).

    m_editLandSea.updatePixels(m_pixels.data());
    m_dirty = true;
}
