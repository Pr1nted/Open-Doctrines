#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "MapEditor.h"
#include "ProceduralGenerator.h"
#include "json.hpp"
#include "miniz.h"
#include "stb_image_write.h"
#include "stb_image.h"
#include "raylib.h"
#include "raymath.h"
#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <random>
#include <iostream>
#include <sys/stat.h>
#include <dirent.h>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

static bool dirExists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
static bool createDir(const std::string& p) { return mkdir(p.c_str(), 0755) == 0 || errno == EEXIST; }
static Rectangle Rect(float x, float y, float w, float h) { return {x, y, w, h}; }

static const Color ACCENT = {255, 215, 0, 255};  // Gold accent
static const Color COL_LAND  = {200, 190, 160, 255}; // matches LandSeaMap in-game land
static const Color COL_SEA   = { 40,  80, 160, 255}; // matches LandSeaMap in-game sea

MapEditor::MapEditor() {}
MapEditor::~MapEditor() {
    if (m_flagPreviewTex.id > 0) UnloadTexture(m_flagPreviewTex);
    if (m_renderer) { delete m_renderer; m_renderer = nullptr; }
}

void MapEditor::init(int screenW, int screenH, const std::string& dataDir) {
    m_screenW = screenW; m_screenH = screenH; m_dataDir = dataDir;
    m_canvasX = 0; m_canvasY = m_toolbarH;
    m_canvasW = screenW - m_panelW;
    m_canvasH = screenH - m_toolbarH - m_bottomH;
    m_projectState = PROJ_STARTUP;
}

void MapEditor::resize(int screenW, int screenH) {
    m_screenW = screenW; m_screenH = screenH;
    m_canvasX = 0; m_canvasY = m_toolbarH;
    m_canvasW = screenW - m_panelW;
    m_canvasH = screenH - m_toolbarH - m_bottomH;
    if (m_renderer) m_renderer->resize(m_canvasW, m_canvasH);
}

// ════════════════════════════════════════════════════════════════
//  Button helper - matches main menu style
// ════════════════════════════════════════════════════════════════

bool MapEditor::drawButtonCol(const char* label, Rectangle rect, Color accent, bool selected, int fontSize) {
    Vector2 mouse = GetMousePosition();
    bool hover = CheckCollisionPointRec(mouse, rect);
    Color bg = hover ? Color{255,255,255,20} : (selected ? ColorAlpha(accent, 0.15f) : BLANK);
    Color border = selected ? accent : (hover ? Color{255,255,255,80} : Color{255,255,255,30});
    Color textCol = selected ? accent : (hover ? WHITE : LIGHTGRAY);

    DrawRectangleRounded(rect, 0.12f, 8, bg);
    DrawRectangleRoundedLines(rect, 0.12f, 8, border);
    int tw = MeasureText(label, fontSize);
    DrawText(label, (int)(rect.x + rect.width/2 - tw/2), (int)(rect.y + rect.height/2 - fontSize/2), fontSize, textCol);
    return hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

bool MapEditor::drawButton(const char* label, Rectangle rect, bool selected, int fontSize) {
    return drawButtonCol(label, rect, ACCENT, selected, fontSize);
}

// ════════════════════════════════════════════════════════════════
//  Project Dialogs
// ════════════════════════════════════════════════════════════════

void MapEditor::drawStartupDialog() {
    ClearBackground(Color{15, 15, 20, 255});
    float cx = m_screenW / 2.0f, cy = m_screenH / 2.0f - 80.0f;

    int titleW = MeasureText("Map Editor", 40);
    DrawText("Map Editor", cx - titleW/2, cy - 70, 40, ACCENT);

    const char* labels[] = {"Open Existing Project", "Create New Project"};
    Rectangle btns[] = { {cx-160, cy, 320, 44}, {cx-160, cy+54, 320, 44} };
    for (int i = 0; i < 2; i++) {
        if (drawButton(labels[i], btns[i], false, 20)) {
            if (i == 0) {
                m_projectState = PROJ_OPEN;
                m_projFiles.clear();
                std::string dir = m_dataDir + "projects/";
                DIR* d = opendir(dir.c_str());
                if (d) { struct dirent* de; while ((de = readdir(d))) { std::string n = de->d_name; if (n.size() >= 7 && n.substr(n.size()-7) == ".uodmap") m_projFiles.push_back(n); } closedir(d); }
                std::sort(m_projFiles.begin(), m_projFiles.end());
                m_projChoice = 0; m_projScroll = 0;
            } else {
                m_projectState = PROJ_CREATE;
                m_projChoice = 0;
            }
        }
    }
}

void MapEditor::drawOpenDialog() {
    ClearBackground(Color{15, 15, 20, 255});
    float cx = m_screenW / 2.0f, cy = m_screenH / 2.0f - 120.0f;
    DrawText("Open Project", cx - 80, cy - 40, 28, WHITE);

    Rectangle listRect = {cx-200, cy, 400, 260};
    DrawRectangleRounded(listRect, 0.05f, 4, Color{30,30,35,255});
    DrawRectangleRoundedLines(listRect, 0.05f, 4, Color{80,80,90,255});

    if (m_projFiles.empty()) {
        DrawText("No .uodmap files found", cx-100, cy+120, 16, GRAY);
        DrawText("in data/projects/", cx-70, cy+142, 14, GRAY);
    } else {
        int itemH = 28;
        for (int i = 0; i < 9 && (i + m_projScroll) < (int)m_projFiles.size(); i++) {
            int idx = i + m_projScroll;
            Rectangle item = {listRect.x+5, listRect.y+5+i*itemH, listRect.width-10, (float)(itemH-2)};
            bool sel = (idx == m_projChoice);
            bool hov = CheckCollisionPointRec(GetMousePosition(), item);
            DrawRectangleRounded(item, 0.1f, 4, sel ? ColorAlpha(ACCENT,0.2f) : (hov ? Color{255,255,255,16} : BLANK));
            DrawText(m_projFiles[idx].c_str(), (int)item.x+10, (int)item.y+6, 14, sel ? ACCENT : (hov ? WHITE : LIGHTGRAY));
            if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) { m_projChoice = idx; }
        }
        float wheel = GetMouseWheelMove();
        if (wheel > 0 && m_projScroll > 0) m_projScroll--;
        if (wheel < 0 && m_projScroll < (int)m_projFiles.size() - 9) m_projScroll++;
    }

    cy += 280;
    Rectangle openBtn = {cx-100, cy, 200, 40};
    Rectangle cancelBtn = {cx-100, cy+50, 200, 36};
    if (drawButton("Open", openBtn, false, 18)) {
        if (m_projChoice >= 0 && m_projChoice < (int)m_projFiles.size()) {
            initBlankMap();
            m_projectState = PROJ_EDITING;
        }
    }
    if (drawButton("Cancel", cancelBtn, false, 16)) { m_projectState = PROJ_STARTUP; }
}

void MapEditor::drawCreateDialog() {
    ClearBackground(Color{15, 15, 20, 255});
    float cx = m_screenW / 2.0f, cy = m_screenH / 2.0f - 100.0f;
    int titleW = MeasureText("Create New Map", 32);
    DrawText("Create New Map", cx - titleW/2, cy - 60, 32, ACCENT);

    const char* labels[] = {"Blank Canvas", "Generate Procedurally", "Based on Existing Map"};
    Rectangle btns[3];
    for (int i = 0; i < 3; i++) btns[i] = {cx-160.0f, cy+i*52.0f, 320.0f, 44.0f};
    for (int i = 0; i < 3; i++) {
        if (drawButton(labels[i], btns[i], m_projChoice == i, 18)) { m_projChoice = i; }
    }

    cy += 168;
    Rectangle confirmBtn = {cx-100, cy, 200, 42};
    Rectangle cancelBtn = {cx-100, cy+52, 200, 36};
    if (drawButton("Create", confirmBtn, false, 18)) {
        m_genParams.seed = rand();
        initBlankMap();
        if (m_projChoice == 1) {
            m_genPending = 1;
            m_genStatus = "Generating world...";
        }
        m_projectState = PROJ_EDITING;
    }
    if (drawButton("Cancel", cancelBtn, false, 16)) { m_projectState = PROJ_STARTUP; }
}

// ════════════════════════════════════════════════════════════════
//  Map initialization
// ════════════════════════════════════════════════════════════════

void MapEditor::initBlankMap() {
    m_pixels.resize(MAP_W * MAP_H, COL_SEA);
    m_editLandSea.setFromPixels(m_pixels.data(), MAP_W, MAP_H);
    if (!m_renderer) {
        m_renderer = new MapRenderer(m_canvasW, m_canvasH, MAP_W, MAP_H);
        m_renderer->setMaxZoom(20.0f);
    }
    m_renderer->resize(m_canvasW, m_canvasH);
    m_dirty = false;
}

// ════════════════════════════════════════════════════════════════
//  Brush operations
// ════════════════════════════════════════════════════════════════

void MapEditor::applyBrush(int cx, int cy, bool land) {
    int r = m_brushSize;
    Color c = land ? COL_LAND : COL_SEA;
    int minX = MAP_W, maxX = 0;
    int minY = std::max(0, cy - r), maxY = std::min(MAP_H - 1, cy + r);
    bool wrapped = false;
    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= MAP_H) continue;
        for (int dx = -r; dx <= r; dx++) {
            if (dx*dx + dy*dy > r*r) continue;
            int px = (cx + dx) % MAP_W;
            if (px < 0) px += MAP_W;
            if (cx + dx != px) wrapped = true;
            m_pixels[py * MAP_W + px] = c;
            if (px < minX) minX = px;
            if (px > maxX) maxX = px;
        }
    }
    trackChange();
    if (!wrapped && minX <= maxX && maxY >= minY) {
        int uw = maxX - minX + 1;
        int uh = maxY - minY + 1;
        std::vector<Color> rectData(uw * uh);
        for (int row = 0; row < uh; row++)
            memcpy(&rectData[row * uw], &m_pixels[(minY + row) * MAP_W + minX], uw * sizeof(Color));
        m_editLandSea.updatePixelsRect(rectData.data(), minX, minY, uw, uh);
    } else {
        m_editLandSea.updatePixels(m_pixels.data());
    }
}

void MapEditor::applyRect(int x1, int y1, int x2, int y2, bool land) {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(MAP_W-1, x2); y2 = std::min(MAP_H-1, y2);
    Color c = land ? COL_LAND : COL_SEA;
    for (int py = y1; py <= y2; py++) {
        for (int px = x1; px <= x2; px++) {
            m_pixels[py * MAP_W + px] = c;
        }
    }
    trackChange();
    m_editLandSea.updatePixels(m_pixels.data());
}

void MapEditor::applyFloodFill(int sx, int sy, bool land) {
    if (sx < 0 || sx >= MAP_W || sy < 0 || sy >= MAP_H) return;
    Color target = m_pixels[sy * MAP_W + sx];
    Color fill = land ? COL_LAND : COL_SEA;
    if (target.r == fill.r && target.g == fill.g && target.b == fill.b) return;
    std::vector<std::pair<int,int>> stack; stack.push_back({sx, sy});
    std::unordered_set<int> visited;
    while (!stack.empty()) {
        auto [x, y] = stack.back(); stack.pop_back();
        int idx = y * MAP_W + x;
        if (visited.count(idx) || x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) continue;
        if (m_pixels[idx].r != target.r || m_pixels[idx].g != target.g || m_pixels[idx].b != target.b) continue;
        visited.insert(idx);
        m_pixels[idx] = fill;
        stack.push_back({(x+1)%MAP_W, y});
        stack.push_back({(x-1+MAP_W)%MAP_W, y});
        stack.push_back({x, y+1});
        stack.push_back({x, y-1});
    }
    trackChange();
    m_editLandSea.updatePixels(m_pixels.data());
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

// ════════════════════════════════════════════════════════════════
//  Map Generator - multi-octave noise with horizontal wrapping
// ════════════════════════════════════════════════════════════════

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
    if (m_renderer) {
        m_renderer->setPoliticalTexture(Texture2D{}); // clear political overlay
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

// ════════════════════════════════════════════════════════════════
//  Procedural Province, Country & Game Data Generation
// ════════════════════════════════════════════════════════════════

void MapEditor::generateProvincesCountries() {
    m_provincePixels.clear();
    m_provinceJson.clear();
    m_politicalPixels.clear();
    m_countryJson.clear();
    m_hasProvinces = false;

    // Downscale land/sea 2x for fast generation (4x fewer pixels)
    const int SCALE = 2;
    int sw = MAP_W / SCALE, sh = MAP_H / SCALE;
    std::vector<Color> smallPixels(sw * sh);
    for (int py = 0; py < sh; ++py) {
        int srcRow = py * SCALE * MAP_W;
        for (int px = 0; px < sw; ++px)
            smallPixels[py * sw + px] = m_pixels[srcRow + px * SCALE];
    }

    // SCALE changed from 4→2 → 4× more pixels → divide density by 4 to keep same province count
    float adjustedDensity = m_genParams.provinceDensity / 4.0f;
    auto result = generateProcedural(smallPixels, sw, sh,
                                     m_genParams.seed, m_genParams.numCountries,
                                     adjustedDensity);

    // ── 3×3 mode filter on province pixels to straighten jagged borders ──
    {
        std::vector<Color> smoothed(sw * sh);
        for (int py = 0; py < sh; ++py) {
            for (int px = 0; px < sw; ++px) {
                Color c = result.provincePixels[py * sw + px];
                // Fast path: check if pixel is on a border first
                bool onBorder = false;
                if (px > 0 && memcmp(&c, &result.provincePixels[py * sw + px - 1], sizeof(Color))) onBorder = true;
                else if (px < sw-1 && memcmp(&c, &result.provincePixels[py * sw + px + 1], sizeof(Color))) onBorder = true;
                else if (py > 0 && memcmp(&c, &result.provincePixels[(py-1) * sw + px], sizeof(Color))) onBorder = true;
                else if (py < sh-1 && memcmp(&c, &result.provincePixels[(py+1) * sw + px], sizeof(Color))) onBorder = true;
                else { smoothed[py * sw + px] = c; continue; }

                // 3×3 majority vote
                Color candidates[9]; int votes[9] = {0}, n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int ny = py + dy;
                        int nx = (px + dx + sw) % sw;
                        if (ny < 0 || ny >= sh) continue;
                        Color nc = result.provincePixels[ny * sw + nx];
                        int idx = 0;
                        while (idx < n && memcmp(&candidates[idx], &nc, sizeof(Color))) ++idx;
                        if (idx == n && n < 9) { candidates[n] = nc; votes[n] = 1; ++n; }
                        else if (idx < n) ++votes[idx];
                    }
                }
                int best = 0;
                for (int i = 1; i < n; ++i) if (votes[i] > votes[best]) best = i;
                smoothed[py * sw + px] = candidates[best];
            }
        }
        result.provincePixels.swap(smoothed);

        // Apply same smoothing to political pixels
        std::vector<Color> polSmoothed(sw * sh);
        for (int py = 0; py < sh; ++py) {
            for (int px = 0; px < sw; ++px) {
                Color c = result.politicalPixels[py * sw + px];
                bool onBorder = false;
                if (px > 0 && memcmp(&c, &result.politicalPixels[py * sw + px - 1], sizeof(Color))) onBorder = true;
                else if (px < sw-1 && memcmp(&c, &result.politicalPixels[py * sw + px + 1], sizeof(Color))) onBorder = true;
                else if (py > 0 && memcmp(&c, &result.politicalPixels[(py-1) * sw + px], sizeof(Color))) onBorder = true;
                else if (py < sh-1 && memcmp(&c, &result.politicalPixels[(py+1) * sw + px], sizeof(Color))) onBorder = true;
                else { polSmoothed[py * sw + px] = c; continue; }
                Color candidates[9]; int votes[9] = {0}, n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int ny = py + dy, nx = (px + dx + sw) % sw;
                        if (ny < 0 || ny >= sh) continue;
                        Color nc = result.politicalPixels[ny * sw + nx];
                        int idx = 0;
                        while (idx < n && memcmp(&candidates[idx], &nc, sizeof(Color))) ++idx;
                        if (idx == n && n < 9) { candidates[n] = nc; votes[n] = 1; ++n; }
                        else if (idx < n) ++votes[idx];
                    }
                }
                int best = 0;
                for (int i = 1; i < n; ++i) if (votes[i] > votes[best]) best = i;
                polSmoothed[py * sw + px] = candidates[best];
            }
        }
        result.politicalPixels.swap(polSmoothed);
    }

    // Scale province/political pixels back to full resolution (nearest-neighbor)
    m_provincePixels.resize(MAP_W * MAP_H, Color{0,0,0,0});
    m_politicalPixels.resize(MAP_W * MAP_H, Color{0,0,0,0});
    for (int py = 0; py < MAP_H; ++py) {
        int si = (py / SCALE) * sw;
        int di = py * MAP_W;
        for (int px = 0; px < MAP_W; ++px)
            m_provincePixels[di + px] = result.provincePixels[si + (px / SCALE)];
    }
    for (int py = 0; py < MAP_H; ++py) {
        int si = (py / SCALE) * sw;
        int di = py * MAP_W;
        for (int px = 0; px < MAP_W; ++px)
            m_politicalPixels[di + px] = result.politicalPixels[si + (px / SCALE)];
    }

    m_provinceJson = std::move(result.provinceJson);
    m_countryJson = std::move(result.countryJson);
    m_populationJson = std::move(result.populationJson);
    m_resourcesJson = std::move(result.resourcesJson);

    m_hasProvinces = !m_provincePixels.empty() && !m_provinceJson.empty();
    m_hasGameData = m_hasProvinces && !m_populationJson.empty();

    if (m_hasProvinces && m_renderer) {
        // ── Encode province pixels to PNG in memory ──
        std::vector<uint8_t> rawProv(MAP_W * MAP_H * 4);
        for (int i = 0; i < MAP_W * MAP_H; ++i) {
            rawProv[i*4]   = m_provincePixels[i].r;
            rawProv[i*4+1] = m_provincePixels[i].g;
            rawProv[i*4+2] = m_provincePixels[i].b;
            rawProv[i*4+3] = 255;
        }
        int pngLen = 0;
        unsigned char* pngData = stbi_write_png_to_mem(rawProv.data(), MAP_W * 4,
                                                       MAP_W, MAP_H, 4, &pngLen);
        if (pngData) {
            m_editProvinces.loadFromMemory(pngData, pngLen, m_provinceJson);
            STBI_FREE(pngData);
        }

        // ── Load countries ──
        m_editCountries.loadFromJson(m_countryJson);

        // ── Create political texture from m_politicalPixels ──
        Image polImg{};
        polImg.data = m_politicalPixels.data();
        polImg.width = MAP_W;
        polImg.height = MAP_H;
        polImg.mipmaps = 1;
        polImg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        Texture2D polTex = LoadTextureFromImage(polImg);
        SetTextureFilter(polTex, TEXTURE_FILTER_POINT);
        m_renderer->setPoliticalTexture(polTex);

        // ── Compute province border texture (only if ProvinceMap loaded OK) ──
        if (m_editProvinces.getImage().data != nullptr)
            m_renderer->computeBorderTexture(m_editProvinces.getImage());

        std::cout << "  Province data loaded into editor\n";
    }

    m_dirty = true;
    std::cout << "  Generated " << (m_hasProvinces ? "OK" : "FAILED") << "\n";
}

void MapEditor::generateGameData() {
    // Already generated alongside provinces in generateProvincesCountries().
    m_hasGameData = !m_populationJson.empty();
    m_dirty = true;
    std::cout << "  Game data confirmed\n";
}

void MapEditor::exportODMap() {
    if (!m_hasProvinces) {
        std::cout << "  No provinces to export. Generate provinces first.\n";
        return;
    }
    fs::create_directories(m_dataDir + "custom_maps/");
    std::string odmPath = m_dataDir + "custom_maps/" + m_mapName + ".odmap";
    if (odmPath.empty()) odmPath = m_dataDir + "custom_maps/map.odmap";

    // Write temp files
    auto writeStr = [](const std::string& path, const std::string& content) {
        std::ofstream f(path, std::ios::binary); f << content; f.close();
    };

    std::string tmpDir = m_dataDir + "tmp_export/";
    fs::create_directories(tmpDir);

    // Save land_sea.png from m_pixels
    {
        std::vector<uint8_t> raw(MAP_W * MAP_H * 4);
        for (int i = 0; i < MAP_W * MAP_H; ++i) {
            raw[i*4]   = m_pixels[i].r;
            raw[i*4+1] = m_pixels[i].g;
            raw[i*4+2] = m_pixels[i].b;
            raw[i*4+3] = 255;
        }
        stbi_write_png((tmpDir + "land_sea.png").c_str(), MAP_W, MAP_H, 4, raw.data(), MAP_W * 4);
    }

    // Save provinces.png
    if (!m_provincePixels.empty()) {
        std::vector<uint8_t> raw(MAP_W * MAP_H * 4);
        for (int i = 0; i < MAP_W * MAP_H; ++i) {
            raw[i*4]   = m_provincePixels[i].r;
            raw[i*4+1] = m_provincePixels[i].g;
            raw[i*4+2] = m_provincePixels[i].b;
            raw[i*4+3] = 255;
        }
        stbi_write_png((tmpDir + "provinces.png").c_str(), MAP_W, MAP_H, 4, raw.data(), MAP_W * 4);
    }

    // Save political.png
    if (!m_politicalPixels.empty()) {
        std::vector<uint8_t> raw(MAP_W * MAP_H * 4);
        for (int i = 0; i < MAP_W * MAP_H; ++i) {
            raw[i*4]   = m_politicalPixels[i].r;
            raw[i*4+1] = m_politicalPixels[i].g;
            raw[i*4+2] = m_politicalPixels[i].b;
            raw[i*4+3] = 255;
        }
        stbi_write_png((tmpDir + "political.png").c_str(), MAP_W, MAP_H, 4, raw.data(), MAP_W * 4);
    }

    // Save JSON files
    writeStr(tmpDir + "provinces.json", m_provinceJson);
    // Rebuild countries.json from edited data (includes compass/doctrine/research changes)
    {
        nlohmann::json cj;
        for (auto& [cid, c] : m_editCountries.getAll()) {
            nlohmann::json e;
            e["id"] = c.id;
            e["name"] = c.name;
            e["iso_a3"] = c.isoA3;
            char ch[8]; snprintf(ch, sizeof(ch), "#%02x%02x%02x", c.color.r, c.color.g, c.color.b);
            e["color"] = std::string(ch);
            e["treasury"] = c.treasury;
            e["compass_economic"] = c.compassEconomic;
            e["compass_social"] = c.compassSocial;
            e["doctrine"] = c.doctrine;
            nlohmann::json rarr = nlohmann::json::array();
            for (auto& r : c.research) rarr.push_back(r);
            e["research"] = rarr;
            // Rebuild flag JSON from FlagPattern
            auto flagToJson = [](const FlagPattern& fp) -> nlohmann::json {
                nlohmann::json f;
                if (!fp.imagePath.empty()) { f["image"] = fp.imagePath; return f; }
                const char* tn = "solid";
                switch (fp.type) {
                    case FlagType::SOLID: tn = "solid"; break;
                    case FlagType::HSTRIPES_2: tn = "hstripes_2"; break;
                    case FlagType::HSTRIPES_3: tn = "hstripes_3"; break;
                    case FlagType::VSTRIPES_2: tn = "vstripes_2"; break;
                    case FlagType::VSTRIPES_3: tn = "vstripes_3"; break;
                    case FlagType::DIAGONAL_L: tn = "diagonal_l"; break;
                    case FlagType::DIAGONAL_R: tn = "diagonal_r"; break;
                    case FlagType::TRIANGLE: tn = "triangle"; break;
                    case FlagType::TRIANGLE_DOUBLE: tn = "triangle_double"; break;
                    case FlagType::QUARTERED: tn = "quartered"; break;
                    case FlagType::SALTIR: tn = "saltir"; break;
                    case FlagType::CANTON: tn = "canton"; break;
                    case FlagType::PALE: tn = "pale"; break;
                    case FlagType::FESS: tn = "fess"; break;
                    case FlagType::CROSS_NORDIC: tn = "cross_nordic"; break;
                    case FlagType::CROSS_GREEK: tn = "cross_greek"; break;
                    case FlagType::STRIPED_EDGE: tn = "striped_edge"; break;
                    case FlagType::SUNBURST: tn = "sunburst"; break;
                    default: tn = "solid"; break;
                }
                f["type"] = tn;
                nlohmann::json cols = nlohmann::json::array();
                for (auto& cl : fp.colors) {
                    char buf[8]; snprintf(buf, sizeof(buf), "#%02x%02x%02x", cl.r, cl.g, cl.b);
                    cols.push_back(std::string(buf));
                }
                f["colors"] = cols;
                f["starCount"] = fp.starCount;
                if (!fp.symbols.empty()) {
                    nlohmann::json syms = nlohmann::json::array();
                    for (auto& sym : fp.symbols) {
                        nlohmann::json s;
                        const char* st = "star_5";
                        switch (sym.type) {
                            case SymbolType::STAR_5: st = "star_5"; break;
                            case SymbolType::STAR_6: st = "star_6"; break;
                            case SymbolType::CRESCENT: st = "crescent"; break;
                            case SymbolType::CRESCENT_STAR: st = "crescent_star"; break;
                            case SymbolType::CROSS_LATIN: st = "cross_latin"; break;
                            case SymbolType::CROSS_SALTIR: st = "cross_saltir"; break;
                            case SymbolType::CIRCLE: st = "circle"; break;
                            case SymbolType::DIAMOND: st = "diamond"; break;
                            case SymbolType::SUN: st = "sun"; break;
                            case SymbolType::GEAR: st = "gear"; break;
                            case SymbolType::MOUNTAIN: st = "mountain"; break;
                            case SymbolType::TREE: st = "tree"; break;
                            case SymbolType::TEXT_BLOCK: st = "text_block"; break;
                            default: st = "star_5"; break;
                        }
                        s["type"] = st;
                        s["x"] = sym.x; s["y"] = sym.y; s["size"] = sym.size;
                        s["count"] = sym.count;
                        nlohmann::json sc = nlohmann::json::array();
                        for (auto& cl : sym.colors) {
                            char buf[8]; snprintf(buf, sizeof(buf), "#%02x%02x%02x", cl.r, cl.g, cl.b);
                            sc.push_back(std::string(buf));
                        }
                        s["colors"] = sc;
                        syms.push_back(s);
                    }
                    f["symbols"] = syms;
                }
                return f;
            };
            e["flag_actual"] = flagToJson(c.flagActual);
            e["flag_censored"] = flagToJson(c.flagCensored);
            cj[std::to_string(cid)] = e;
        }
        writeStr(tmpDir + "countries.json", cj.dump(2));
    }
    writeStr(tmpDir + "population.json", m_populationJson);
    writeStr(tmpDir + "resources.json", m_resourcesJson);

    // Metadata
    nlohmann::json meta;
    meta["map_date"] = "Procedural";
    meta["author"] = "MapEditor";
    meta["license"] = "MIT";
    meta["has_scripts"] = false;
    writeStr(tmpDir + "metadata.json", meta.dump());

    // Thumbnail from political
    {
        std::string polPath = tmpDir + "political.png";
        int pw, ph, pc;
        unsigned char* polData = stbi_load(polPath.c_str(), &pw, &ph, &pc, 4);
        if (polData) {
            int tw = 160, th = 80;
            std::vector<unsigned char> thumbData(tw * th * 4);
            for (int y = 0; y < th; ++y)
                for (int x = 0; x < tw; ++x)
                    std::memcpy(&thumbData[(y * tw + x) * 4], &polData[((y * ph / th) * pw + x * pw / tw) * 4], 4);
            stbi_write_png((tmpDir + "thumb.png").c_str(), tw, th, 4, thumbData.data(), tw * 4);
            stbi_image_free(polData);
        }
    }

    // Package into .odmap
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, odmPath.c_str(), 0)) {
        std::cout << "  Failed to create .odmap: " << odmPath << "\n";
        fs::remove_all(tmpDir);
        return;
    }

    auto addFile = [&](const std::string& name) {
        std::string full = tmpDir + name;
        std::ifstream f(full, std::ios::binary | std::ios::ate);
        if (!f) { std::cout << "  Skipping " << name << "\n"; return; }
        std::streamsize sz = f.tellg();
        f.seekg(0);
        std::vector<uint8_t> buf(sz);
        if (f.read((char*)buf.data(), sz))
            mz_zip_writer_add_mem(&zip, name.c_str(), buf.data(), buf.size(), MZ_BEST_COMPRESSION);
        f.close();
    };

    addFile("land_sea.png");
    addFile("provinces.png");
    addFile("provinces.json");
    addFile("political.png");
    addFile("countries.json");
    addFile("population.json");
    addFile("resources.json");
    addFile("metadata.json");
    addFile("thumb.png");
    mz_zip_writer_add_mem(&zip, "scripts/", nullptr, 0, MZ_NO_COMPRESSION);

    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    fs::remove_all(tmpDir);

    std::cout << "  Exported .odmap to: " << odmPath << "\n";
}

// ════════════════════════════════════════════════════════════════
//  Main update / draw
// ════════════════════════════════════════════════════════════════

void MapEditor::update(float dt) {
    // ── Two-step deferred generation ──
    // m_genPending=2: loading overlay was drawn in previous frame, now run generation
    if (m_genPending == 2) {
        m_genStatus = "Generating landmass...";
        generateMap(m_genParams);
        m_genStatus = "Generating provinces & countries...";
        generateProvincesCountries();
        m_genStatus = "Generating game data...";
        generateGameData();
        m_genPending = 0;
        return;
    }
    if (m_projectState != PROJ_EDITING) return;
    updateEdit(dt);
}

void MapEditor::draw() {
    // ── Two-step deferred generation ──
    // m_genPending=1: draw loading overlay, advance to step 2
    if (m_genPending == 1) {
        ClearBackground(Color{15, 15, 20, 255});
        DrawRectangle(0, 0, m_screenW, m_screenH, Color{0, 0, 0, 180});
        const char* msg = m_genStatus.c_str();
        int tw = MeasureText(msg, 28);
        DrawText(msg, m_screenW/2 - tw/2, m_screenH/2 - 14, 28, ACCENT);
        m_genPending = 2;
        return;
    }

    if (m_projectState == PROJ_STARTUP) { drawStartupDialog(); return; }
    if (m_projectState == PROJ_OPEN) { drawOpenDialog(); return; }
    if (m_projectState == PROJ_CREATE) { drawCreateDialog(); return; }
    drawToolbar();
    drawCanvas();
    drawSidePanel();
    drawBottomBar();

    // Warning message (hint when trying to paint in wrong mode)
    if (m_warningTimer > 0) {
        int tw = MeasureText(m_warningMsg.c_str(), 16);
        int wx = m_screenW / 2 - tw / 2, wy = m_toolbarH + 40;
        DrawRectangle(wx - 10, wy - 6, tw + 20, 30, Color{255, 215, 0, 200});
        DrawText(m_warningMsg.c_str(), wx, wy, 16, Color{20, 20, 25, 255});
    }
}

void MapEditor::screenToCanvas(int sx, int sy, int& cx, int& cy) const {
    if (!m_renderer) { cx = 0; cy = 0; return; }
    m_renderer->screenToPixel(sx - m_canvasX, sy - m_canvasY, cx, cy);
}

void MapEditor::updateEdit(float dt) {
    if (!m_renderer) return;
    Vector2 mouse = GetMousePosition();
    bool inCanvas = mouse.x >= m_canvasX && mouse.x < m_canvasX + m_canvasW &&
                    mouse.y >= m_canvasY && mouse.y < m_canvasY + m_canvasH;

    // Pan mode (true): left click pans (blockLeftPan=false)
    // Draw mode (false): left click draws (blockLeftPan=true)
    m_renderer->setBlockLeftPan(!m_isPanMode);
    // Right-click always pans (default in MapRenderer)
    // Middle-click always pans (default in MapRenderer)

    if (inCanvas) {
        m_renderer->setPaused(false);
        m_renderer->update(dt);
    } else {
        m_renderer->setPaused(true);
    }

    updateToolbar();
    updateBottomBar();
    updateSidePanel(); // always allow panel interaction

    // Apply brush (only in draw mode + landmass mode)
    bool lmb = IsMouseButtonDown(MOUSE_LEFT_BUTTON);
    if (!m_isPanMode && lmb && inCanvas && m_tool == TOOL_BRUSH) {
        if (m_mode == MODE_LANDMASS) {
            int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
            applyBrush(cx, cy, m_drawAsLand);
        } else if (m_warningTimer <= 0) {
            m_warningMsg = "Switch to Landmass mode to paint";
            m_warningTimer = 2.5f;
        }
    }
    // ── Country / Province selection click ──
    if (inCanvas && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !m_isPanMode) {
        if (m_mode == MODE_COUNTRIES || m_mode == MODE_PROVINCES) {
            int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
            if (cx >= 0 && cx < MAP_W && cy >= 0 && cy < MAP_H && m_editProvinces.getWidth() > 0) {
                const Province* p = m_editProvinces.getProvince(cx, cy);
                if (p) {
                    m_selectedProvince = p->id;
                    if (m_mode == MODE_COUNTRIES) m_selectedCountry = p->countryId;
                }
            }
        }
    }

    // ── Drag-and-drop SVG flag import ──
    if (IsFileDropped()) {
        FilePathList dropped = LoadDroppedFiles();
        if (dropped.count > 0 && m_selectedCountry >= 0) {
            std::string path = dropped.paths[0];
            std::string ext = path.size() >= 4 ? path.substr(path.size() - 4) : "";
            if (ext == ".svg" || ext == ".SVG") {
                auto& all = m_editCountries.getAll();
                auto it = all.find(m_selectedCountry);
                if (it != all.end()) {
                    // Copy SVG to project flags dir
                    fs::create_directories(m_dataDir + "projects/flags/");
                    std::string dest = m_dataDir + "projects/flags/c" + std::to_string(m_selectedCountry) + ".svg";
                    std::ifstream src(path, std::ios::binary);
                    if (src) {
                        std::ofstream dst(dest, std::ios::binary);
                        dst << src.rdbuf();
                        dst.close();
                        it->second.flagActual.imagePath = dest;
                        m_flagPreviewCountry = -1;
                        m_dirty = true;
                    }
                }
            }
        }
        UnloadDroppedFiles(dropped);
    }

    if (m_warningTimer > 0) m_warningTimer -= dt;
}

// ════════════════════════════════════════════════════════════════
//  Toolbar
// ════════════════════════════════════════════════════════════════

void MapEditor::updateToolbar() {
    Vector2 mouse = GetMousePosition();
    if (mouse.y < 0 || mouse.y > m_toolbarH) return;
    const char* names[] = {"Landmass","Provinces","Countries","Resources","Troops","Navy","Relations","Scripts","Generator","Metadata"};
    int btnW = 95, btnH = 34, gap = 4, startX = 8;
    for (int i = 0; i < 10; i++) {
        Rectangle r = {(float)(startX + i*(btnW+gap)), (float)((m_toolbarH-btnH)/2), (float)btnW, (float)btnH};
        if (CheckCollisionPointRec(mouse, r) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_mode = (EditMode)i;
        }
    }
}

void MapEditor::drawToolbar() {
    DrawRectangle(0, 0, m_screenW, m_toolbarH, Color{25, 25, 30, 255});
    const char* names[] = {"Landmass","Provinces","Countries","Resources","Troops","Navy","Relations","Scripts","Generator","Metadata"};
    int btnW = 95, btnH = 34, gap = 4, startX = 8;
    for (int i = 0; i < 10; i++) {
        Rectangle r = {(float)(startX + i*(btnW+gap)), (float)((m_toolbarH-btnH)/2), (float)btnW, (float)btnH};
        bool sel = (m_mode == i);
        Color accent = sel ? ACCENT : Color{255,255,255,40};
        Color bg = sel ? ColorAlpha(ACCENT, 0.12f) : BLANK;
        bool hov = CheckCollisionPointRec(GetMousePosition(), r);
        if (hov && !sel) bg = Color{255,255,255,10};
        DrawRectangleRounded(r, 0.1f, 6, bg);
        DrawRectangleRoundedLines(r, 0.1f, 6, accent);
        int tw = MeasureText(names[i], 13);
        DrawText(names[i], (int)(r.x + r.width/2 - tw/2), (int)(r.y + r.height/2 - 7), 13, sel ? ACCENT : (hov ? WHITE : LIGHTGRAY));
    }
    DrawText(TextFormat("Zoom: %.2fx", m_renderer ? m_renderer->getZoom() : 1.0f), m_screenW - 120, 16, 12, WHITE);
}

// ════════════════════════════════════════════════════════════════
//  Canvas (uses MapRenderer)
// ════════════════════════════════════════════════════════════════

void MapEditor::drawCanvas() {
    DrawRectangle(m_canvasX, m_canvasY, m_canvasW, m_canvasH, Color{10, 10, 15, 255});
    if (m_renderer && m_editLandSea.getWidth() > 0) {
        m_renderer->drawSubregion(m_canvasX, m_canvasY, m_canvasW, m_canvasH,
            m_renderer->getCameraTarget().x, m_renderer->getCameraTarget().y,
            m_renderer->getZoom(), m_editLandSea, m_editProvinces, m_editCountries);
    }
    DrawRectangleLines(m_canvasX, m_canvasY, m_canvasW, m_canvasH, Color{60, 60, 70, 255});

    // Brush preview
    Vector2 mouse = GetMousePosition();
    if (m_mode == MODE_LANDMASS && (m_tool == TOOL_BRUSH || m_tool == TOOL_ERASE)) {
        if (mouse.x >= m_canvasX && mouse.x < m_canvasX + m_canvasW && mouse.y >= m_canvasY && mouse.y < m_canvasY + m_canvasH) {
            // approximate brush size on screen
            float z = m_renderer ? m_renderer->getZoom() : 1.0f;
            int r = (int)(m_brushSize * z);
            if (r < 2) r = 2;
            DrawCircleLines((int)mouse.x, (int)mouse.y, (float)r, m_drawAsLand ? Color{130,255,130,200} : Color{130,130,255,200});
        }
    }
}

// ════════════════════════════════════════════════════════════════
//  Bottom Bar
// ════════════════════════════════════════════════════════════════

void MapEditor::updateBottomBar() {
    Vector2 mouse = GetMousePosition();
    int by = m_screenH - m_bottomH;
    if (mouse.y < by) return;
    const char* names[] = {"Brush","Rect","Fill","Erase"};
    int startX = 60, gap = 5, btnW = 70;
    for (int i = 0; i < 4; i++) {
        Rectangle r = {(float)(startX + i*(btnW+gap)), (float)(by + 10), (float)btnW, 30};
        if (CheckCollisionPointRec(mouse, r) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_tool = (Tool)i;
    }
    // Brush size slider
    Rectangle slider = {(float)(startX + 4*75 + gap + 80), (float)(by + 17), 120, 14};
    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, {slider.x-5, slider.y-5, slider.width+10, slider.height+10})) {
        m_brushSize = (int)((GetMouseX() - slider.x) / slider.width * 40);
        m_brushSize = std::max(1, std::min(60, m_brushSize));
    }
    // Paint land / sea toggle
    Rectangle landBtn = {(float)(startX + 4*75 + gap + 220), (float)(by+10), 70, 30};
    if (CheckCollisionPointRec(mouse, landBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_drawAsLand = true;
    Rectangle seaBtn = {(float)(startX + 4*75 + gap + 295), (float)(by+10), 70, 30};
    if (CheckCollisionPointRec(mouse, seaBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_drawAsLand = false;
    // Draw / Pan toggle
    Rectangle modeBtn = {(float)(startX + 4*75 + gap + 380), (float)(by+10), 90, 30};
    if (CheckCollisionPointRec(mouse, modeBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_isPanMode = !m_isPanMode;
}

void MapEditor::drawBottomBar() {
    int by = m_screenH - m_bottomH;
    DrawRectangle(0, by, m_screenW, m_bottomH, Color{25, 25, 30, 255});

    const char* names[] = {"Brush","Rect","Fill","Erase"};
    int startX = 60, gap = 5, btnW = 70;
    DrawText("Tools", 10, by + 18, 14, LIGHTGRAY);
    for (int i = 0; i < 4; i++) {
        Rectangle r = {(float)(startX + i*(btnW+gap)), (float)(by + 10), (float)btnW, 30};
        bool sel = (m_tool == i);
        Color accent = sel ? ACCENT : Color{255,255,255,30};
        Color bg = sel ? ColorAlpha(ACCENT, 0.12f) : BLANK;
        bool hov = CheckCollisionPointRec(GetMousePosition(), r);
        if (hov && !sel) bg = Color{255,255,255,10};
        DrawRectangleRounded(r, 0.1f, 6, bg);
        DrawRectangleRoundedLines(r, 0.1f, 6, accent);
        int tw = MeasureText(names[i], 13);
        DrawText(names[i], (int)(r.x + r.width/2 - tw/2), (int)(r.y + 9), 13, sel ? ACCENT : (hov ? WHITE : LIGHTGRAY));
    }

    DrawText("Size", startX + 4*75 + gap + 50, by+18, 13, LIGHTGRAY);
    Rectangle slider = {(float)(startX + 4*75 + gap + 80), (float)(by + 17), 120, 14};
    DrawRectangleRounded(slider, 0.3f, 4, Color{50,50,60,255});
    Rectangle fill = {slider.x, slider.y, slider.width * m_brushSize / 60, slider.height};
    if (fill.width > 0) DrawRectangleRounded(fill, 0.3f, 4, ACCENT);
    DrawText(TextFormat("%d", m_brushSize), (int)(slider.x + slider.width + 8), by+18, 13, WHITE);

    // Land / Sea toggle
    Rectangle landBtn = {(float)(startX + 4*75 + gap + 220), (float)(by+10), 70, 30};
    Rectangle seaBtn = {(float)(startX + 4*75 + gap + 295), (float)(by+10), 70, 30};
    drawButton("Land", landBtn, m_drawAsLand, 14);
    drawButton("Sea", seaBtn, !m_drawAsLand, 14);
    // Draw / Pan toggle
    Rectangle modeBtn = {(float)(startX + 4*75 + gap + 380), (float)(by+10), 90, 30};
    drawButton(m_isPanMode ? "Pan" : "Draw", modeBtn, false, 14);
}

// ════════════════════════════════════════════════════════════════
//  Side Panel
// ══════════════════════════════════════════════

void MapEditor::updateSidePanel() {
    Vector2 mouse = GetMousePosition();
    int px = m_screenW - m_panelW;
    if (mouse.x < px || mouse.x >= m_screenW) return;

    switch (m_mode) {
        case MODE_LANDMASS: updateLandmassPanel(); break;
        case MODE_GENERATOR: updateGeneratorPanel(); break;
        case MODE_PROVINCES: updateProvincePanel(); break;
        case MODE_COUNTRIES: updateCountryPanel(); break;
        case MODE_RESOURCES: updateResourcePanel(); break;
        case MODE_RELATIONS: updateRelationsPanel(); break;
        case MODE_TROOPS: updateTroopsPanel(); break;
        case MODE_NAVY: updateNavyPanel(); break;
        case MODE_SCRIPTS: updateScriptPanel(); break;
        case MODE_METADATA: updateMetadataPanel(); break;
    }
}

void MapEditor::drawSidePanel() {
    int px = m_screenW - m_panelW, py = m_toolbarH, ph = m_screenH - m_toolbarH - m_bottomH;
    DrawRectangle(px, py, m_panelW, ph, Color{30, 30, 35, 255});
    DrawRectangleLines(px, py, m_panelW, ph, Color{60, 60, 70, 255});
    switch (m_mode) {
        case MODE_LANDMASS: drawLandmassPanel(); break;
        case MODE_PROVINCES: drawProvincePanel(); break;
        case MODE_COUNTRIES: drawCountryPanel(); break;
        case MODE_RESOURCES: drawResourcePanel(); break;
        case MODE_RELATIONS: drawRelationsPanel(); break;
        case MODE_TROOPS: drawTroopsPanel(); break;
        case MODE_NAVY: drawNavyPanel(); break;
        case MODE_SCRIPTS: drawScriptPanel(); break;
        case MODE_GENERATOR: drawGeneratorPanel(); break;
        case MODE_METADATA: drawMetadataPanel(); break;
    }
}

// ── Landmass Panel ─────────────────────────────────────────────

void MapEditor::updateLandmassPanel() {}
void MapEditor::drawLandmassPanel() {
    int px = m_screenW - m_panelW + 12, y = m_toolbarH + 16;
    DrawText("Landmass Editor", px, y, 18, ACCENT); y += 35;
    DrawText("Use brush tools below.", px, y, 13, LIGHTGRAY); y += 20;
    DrawText("Left-drag to paint,", px, y, 13, GRAY); y += 16;
    DrawText("right-drag to pan.", px, y, 13, GRAY); y += 16;
    DrawText("Scroll to zoom.", px, y, 13, GRAY);
}

// ── Generator Panel (update & draw use identical y layout) ───────

void MapEditor::updateGeneratorPanel() {
    Vector2 mouse = GetMousePosition();
    int px = m_screenW - m_panelW + 12;
    // y = m_toolbarH+56: "Seed" label
    // y = m_toolbarH+67: seed input rect (h=26) at y+3
    //    RND button at x+204, 28x26
    // y = m_toolbarH+110: "Land Coverage:" label
    // y = m_toolbarH+128: coverage slider
    // y = m_toolbarH+163: "Continents:" label
    // y = m_toolbarH+181: continents ± buttons
    // y = m_toolbarH+215: "Jaggedness:" label
    // y = m_toolbarH+233: jaggedness slider
    // y = m_toolbarH+273: countries ± buttons
    // y = m_toolbarH+315: province size slider
    // y = m_toolbarH+360: Generate World button
    // y = m_toolbarH+410: Export .odmap button

    // Seed + RND button
    Rectangle seedRect = {(float)px, (float)(m_toolbarH + 70), 172, 26};
    Rectangle rndBtn = {(float)(px + 178), (float)(m_toolbarH + 70), 34, 26};
    if (!m_editingSeed && CheckCollisionPointRec(mouse, seedRect) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        m_editingSeed = true;
        m_seedText = TextFormat("%d", m_genParams.seed);
    }
    if (CheckCollisionPointRec(mouse, rndBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        m_genParams.seed = rand() % 100000;
    }
    if (m_editingSeed) {
        int key = GetCharPressed();
        while (key > 0) {
            if (key >= '0' && key <= '9' && m_seedText.size() < 10) m_seedText.push_back((char)key);
            key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !m_seedText.empty()) m_seedText.pop_back();
        if (IsKeyPressed(KEY_ENTER)) { m_genParams.seed = std::max(0, atoi(m_seedText.c_str())); m_editingSeed = false; }
        if (IsKeyPressed(KEY_ESCAPE)) m_editingSeed = false;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, seedRect)) m_editingSeed = false;
    }

    // Coverage slider
    int y = m_toolbarH + 128;
    Rectangle cov = {(float)px, (float)y, 200, 12};
    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, {cov.x-4, cov.y-4, cov.width+8, cov.height+8})) {
        m_genParams.landCoverage = (GetMouseX() - cov.x) / cov.width;
        m_genParams.landCoverage = std::max(0.1f, std::min(0.8f, m_genParams.landCoverage));
    }

    // Continents ±
    y = m_toolbarH + 181;
    Rectangle minus = {(float)px, (float)y, 28, 24};
    Rectangle plus = {(float)(px+170), (float)y, 28, 24};
    if (CheckCollisionPointRec(mouse, minus) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_genParams.numContinents = std::max(1, m_genParams.numContinents-1);
    if (CheckCollisionPointRec(mouse, plus) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_genParams.numContinents = std::min(20, m_genParams.numContinents+1);

    // Jaggedness slider
    y = m_toolbarH + 233;
    Rectangle jag = {(float)px, (float)y, 200, 12};
    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, {jag.x-4, jag.y-4, jag.width+8, jag.height+8})) {
        m_genParams.jaggedness = (GetMouseX() - jag.x) / jag.width;
        m_genParams.jaggedness = std::max(0.1f, std::min(1.0f, m_genParams.jaggedness));
    }

    // Countries slider
    y = m_toolbarH + 270;
    Rectangle cntSlider = {(float)px, (float)y, 200, 12};
    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, {cntSlider.x-4, cntSlider.y-4, cntSlider.width+8, cntSlider.height+8})) {
        float s = (GetMouseX() - cntSlider.x) / cntSlider.width;
        m_genParams.numCountries = (int)(1 + s * 299);
        m_genParams.numCountries = std::max(1, std::min(300, m_genParams.numCountries));
    }
    // Countries text input (click to type exact value)
    Rectangle cntText = {(float)(px + 164), (float)(m_toolbarH + 288), 44, 22};
    if (!m_editingCountryCount && CheckCollisionPointRec(mouse, cntText) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        m_editingCountryCount = true;
        m_countryCountText = TextFormat("%d", m_genParams.numCountries);
    }
    if (m_editingCountryCount) {
        int key = GetCharPressed();
        while (key > 0) {
            if (key >= '0' && key <= '9' && m_countryCountText.size() < 4) m_countryCountText.push_back((char)key);
            key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !m_countryCountText.empty()) m_countryCountText.pop_back();
        if (IsKeyPressed(KEY_ENTER)) { m_genParams.numCountries = std::max(1, std::min(300, atoi(m_countryCountText.c_str()))); m_editingCountryCount = false; }
        if (IsKeyPressed(KEY_ESCAPE)) m_editingCountryCount = false;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, cntText)) m_editingCountryCount = false;
    }

    // Province size slider (exponential: 0.2–5.0, 1.0 at midpoint)
    y = m_toolbarH + 315;
    Rectangle provSize = {(float)px, (float)y, 200, 12};
    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, {provSize.x-4, provSize.y-4, provSize.width+8, provSize.height+8})) {
        float s = (GetMouseX() - provSize.x) / provSize.width;
        s = std::max(0.0f, std::min(1.0f, s));
        m_genParams.provinceDensity = 0.2f * powf(100.0f, s);
    }

    // Generate World button (does landmass + provinces + game data)
    y = m_toolbarH + 360;
    Rectangle genBtn = {(float)px, (float)y, 200, 38};
    if (drawButton("Generate World", genBtn, false, 16)) {
        m_genPending = 1;
        m_genStatus = "Generating world...";
    }

    // Export .odmap button
    y = m_toolbarH + 410;
    Rectangle exportBtn = {(float)px, (float)y, 200, 34};
    if (drawButton("Export .odmap", exportBtn, false, 14)) {
        exportODMap();
    }
}

void MapEditor::drawGeneratorPanel() {
    int px = m_screenW - m_panelW + 12;
    const int LBL = 13;

    DrawText("Map Generator", px, m_toolbarH + 16, 18, ACCENT);

    // Seed label above the input box
    DrawText("Seed", px, m_toolbarH + 56, LBL, LIGHTGRAY);
    Rectangle seedRect = {(float)px, (float)(m_toolbarH + 70), 172, 26};
    DrawRectangleRounded(seedRect, 0.08f, 4, m_editingSeed ? Color{35,35,50,255} : Color{28,28,35,255});
    DrawRectangleRoundedLines(seedRect, 0.08f, 4, m_editingSeed ? ACCENT : Color{60,60,70,255});
    const char* s = m_editingSeed ? m_seedText.c_str() : TextFormat("%d", m_genParams.seed);
    DrawText(s, (int)seedRect.x + 8, (int)seedRect.y + 5, 14, m_editingSeed ? ACCENT : WHITE);
    if (m_editingSeed) DrawText("|", (int)(seedRect.x + 8 + MeasureText(s, 14)), (int)seedRect.y + 5, 14, ACCENT);
    // RND button
    Rectangle rndBtn = {(float)(px + 178), (float)(m_toolbarH + 70), 34, 26};
    bool rndHov = CheckCollisionPointRec(GetMousePosition(), rndBtn);
    DrawRectangleRounded(rndBtn, 0.08f, 4, rndHov ? Color{40,40,55,255} : Color{30,30,40,255});
    DrawRectangleRoundedLines(rndBtn, 0.08f, 4, Color{80,80,90,255});
    DrawText("RND", px + 185, (int)rndBtn.y + 6, 12, rndHov ? ACCENT : LIGHTGRAY);

    // Coverage label + slider
    DrawText("Land Coverage:", px, m_toolbarH + 110, LBL, LIGHTGRAY);
    Rectangle cov = {(float)px, (float)(m_toolbarH + 128), 200, 12};
    DrawRectangleRounded(cov, 0.3f, 4, Color{50,50,60,255});
    Rectangle cfill = {cov.x, cov.y, cov.width * m_genParams.landCoverage, cov.height};
    if (cfill.width > 1) DrawRectangleRounded(cfill, 0.3f, 4, ACCENT);
    DrawText(TextFormat("%.0f%%", m_genParams.landCoverage*100), px+210, m_toolbarH + 127, LBL, WHITE);

    // Continents label + ±
    DrawText("Landmass centers:", px, m_toolbarH + 163, LBL, LIGHTGRAY);
    DrawText(TextFormat("%d", m_genParams.numContinents), px+140, m_toolbarH + 163, 14, WHITE);
    Rectangle cmin = {(float)px, (float)(m_toolbarH + 181), 28, 24}, cplu = {(float)(px+170), (float)(m_toolbarH + 181), 28, 24};
    DrawText("-", (int)cmin.x+9, (int)cmin.y+2, 16, WHITE);
    DrawText("+", (int)cplu.x+9, (int)cplu.y+2, 16, WHITE);

    // Jaggedness label + slider
    DrawText("Jaggedness:", px, m_toolbarH + 215, LBL, LIGHTGRAY);
    Rectangle jag = {(float)px, (float)(m_toolbarH + 233), 200, 12};
    DrawRectangleRounded(jag, 0.3f, 4, Color{50,50,60,255});
    Rectangle jfill = {jag.x, jag.y, jag.width * m_genParams.jaggedness, jag.height};
    if (jfill.width > 1) DrawRectangleRounded(jfill, 0.3f, 4, ACCENT);
    DrawText(TextFormat("%.0f", m_genParams.jaggedness), px+210, m_toolbarH + 232, LBL, WHITE);

    // Countries slider + editable number
    DrawText("Countries:", px, m_toolbarH + 255, LBL, LIGHTGRAY);
    Rectangle cntSlider = {(float)px, (float)(m_toolbarH + 270), 200, 12};
    DrawRectangleRounded(cntSlider, 0.3f, 4, Color{50,50,60,255});
    float cntPct = (m_genParams.numCountries - 1) / 299.0f;
    Rectangle cntFill = {cntSlider.x, cntSlider.y, cntSlider.width * cntPct, cntSlider.height};
    if (cntFill.width > 1) DrawRectangleRounded(cntFill, 0.3f, 4, ACCENT);
    // Editable text field for exact number
    Rectangle cntText = {(float)(px + 164), (float)(m_toolbarH + 288), 44, 22};
    bool cntHov = CheckCollisionPointRec(GetMousePosition(), cntText);
    DrawRectangleRounded(cntText, 0.08f, 4, m_editingCountryCount ? Color{35,35,50,255} : (cntHov ? Color{30,30,40,255} : Color{25,25,35,255}));
    DrawRectangleRoundedLines(cntText, 0.08f, 4, m_editingCountryCount ? ACCENT : Color{60,60,70,255});
    const char* cntStr = m_editingCountryCount ? m_countryCountText.c_str() : TextFormat("%d", m_genParams.numCountries);
    DrawText(cntStr, (int)cntText.x + 6, (int)cntText.y + 3, 12, m_editingCountryCount ? ACCENT : WHITE);
    if (m_editingCountryCount) DrawText("|", (int)(cntText.x + 6 + MeasureText(cntStr, 12)), (int)cntText.y + 3, 12, ACCENT);

    // Province size slider
    DrawText("Province Size:", px, m_toolbarH + 295, LBL, LIGHTGRAY);
    Rectangle provSize = {(float)px, (float)(m_toolbarH + 315), 200, 12};
    DrawRectangleRounded(provSize, 0.3f, 4, Color{50,50,60,255});
    {
        float s = logf(m_genParams.provinceDensity / 0.2f) / logf(100.0f);
        s = std::max(0.0f, std::min(1.0f, s));
        Rectangle pfill = {provSize.x, provSize.y, provSize.width * s, provSize.height};
        if (pfill.width > 2) DrawRectangleRounded(pfill, 0.3f, 4, ACCENT);
    }
    const char* szLabel = "Medium";
    float pd = m_genParams.provinceDensity;
    if (pd < 0.4f) szLabel = "Very Large";
    else if (pd < 0.8f) szLabel = "Large";
    else if (pd < 2.0f) szLabel = "Medium";
    else if (pd < 5.0f) szLabel = "Small";
    else if (pd < 12.0f) szLabel = "Very Small";
    else szLabel = "Tiny";
    DrawText(szLabel, px+210, m_toolbarH + 314, LBL, WHITE);

    // Generate World button (landmass + provinces + game data)
    Rectangle genBtn = {(float)px, (float)(m_toolbarH + 360), 200, 38};
    drawButton("Generate World", genBtn, false, 16);

    // Export .odmap button
    Rectangle exportBtn = {(float)px, (float)(m_toolbarH + 410), 200, 34};
    drawButton("Export .odmap", exportBtn, false, 14);

    // Status indicators
    DrawText(m_hasProvinces ? "Provinces: OK" : "Provinces: --", px, m_toolbarH + 460, 12, m_hasProvinces ? GREEN : GRAY);
    DrawText(m_hasGameData ? "Game Data: OK" : "Game Data: --", px, m_toolbarH + 475, 12, m_hasGameData ? GREEN : GRAY);
}

// ── Province Panel ─────────────────────────────────────────────

void MapEditor::updateProvincePanel() {}
void MapEditor::drawProvincePanel() {
    int px = m_screenW - m_panelW + 12, y = m_toolbarH + 16;
    DrawText("Province Editor", px, y, 18, ACCENT); y += 35;
    if (m_selectedProvince >= 0) {
        DrawText(TextFormat("Selected: %d", m_selectedProvince), px, y, 14, WHITE);
    } else {
        DrawText("Click a province", px, y, 14, GRAY); y += 18;
        DrawText("to edit it.", px, y, 14, GRAY);
    }
}

// ── Country Panel ──────────────────────────────────────────────

void MapEditor::updateCountryPanel() {
    Vector2 mouse = GetMousePosition();
    int px = m_screenW - m_panelW + 12;
    int panelH = m_screenH - m_toolbarH - m_bottomH;
    // Compact list takes top ~40% of panel
    const int LIST_START = m_toolbarH + 56;
    const int LIST_HEIGHT = (int)(panelH * 0.4f);
    int listW = m_panelW - 24;
    int itemH = 18;

    auto& all = m_editCountries.getAll();
    std::vector<int> cids;
    for (auto& [cid, c] : all) cids.push_back(cid);
    std::sort(cids.begin(), cids.end());

    int visible = LIST_HEIGHT / itemH;

    if (CheckCollisionPointRec(mouse, {(float)px, (float)LIST_START, (float)listW, (float)LIST_HEIGHT}))
        m_countryScroll -= GetMouseWheelMove();
    int maxScroll = std::max(0, (int)cids.size() - visible);
    if (m_countryScroll < 0) m_countryScroll = 0;
    if (m_countryScroll > maxScroll) m_countryScroll = maxScroll;

    for (int i = m_countryScroll; i < (int)cids.size() && i < m_countryScroll + visible; ++i) {
        int yi = LIST_START + (i - m_countryScroll) * itemH;
        Rectangle r = {(float)px, (float)yi, (float)listW, (float)(itemH - 1)};
        if (CheckCollisionPointRec(mouse, r) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_selectedCountry = cids[i];
            m_editingCountryName = false;
        }
    }

    // ── Selected country details (bottom ~55% of panel) ──
    if (m_selectedCountry >= 0) {
        auto it = all.find(m_selectedCountry);
        if (it == all.end()) { m_selectedCountry = -1; return; }
        Country& c = it->second;

        int editY = LIST_START + LIST_HEIGHT + 6;
        int maxEditY = m_screenH - m_bottomH - 50;

        // Name editing
        Rectangle nameRect = {(float)px, (float)editY, (float)listW, 24};
        if (!m_editingCountryName && CheckCollisionPointRec(mouse, nameRect) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_editingCountryName = true;
            m_editingNameText = c.name;
            m_renameCountryId = m_selectedCountry;
        }
        if (m_editingCountryName && m_renameCountryId != m_selectedCountry) m_editingCountryName = false;
        if (m_editingCountryName) {
            int key = GetCharPressed();
            while (key > 0) {
                if (key >= 32 && key < 128 && m_editingNameText.size() < 40)
                    m_editingNameText.push_back((char)key);
                key = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE) && !m_editingNameText.empty()) m_editingNameText.pop_back();
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
                if (!m_editingNameText.empty() && IsKeyPressed(KEY_ENTER)) {
                    it->second.name = m_editingNameText;
                    m_dirty = true;
                }
                m_editingCountryName = false;
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, nameRect)) {
                if (!m_editingNameText.empty()) {
                    it->second.name = m_editingNameText;
                    m_dirty = true;
                }
                m_editingCountryName = false;
            }
        }

        // Flag preview bounds (matching drawCountryPanel layout)
        editY += 30;
        Rectangle flagRect = {(float)px, (float)editY, 120, 60};

        // Color swatch
        editY += (int)flagRect.height + 10;
        // Buttons
        if (editY + 28 < maxEditY) {
            Rectangle delBtn = {(float)px, (float)editY, (float)(listW * 0.42f), 24};
            if (CheckCollisionPointRec(mouse, delBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                if (all.size() > 1) { all.erase(m_selectedCountry); m_selectedCountry = -1; m_dirty = true; }
            }
            Rectangle createBtn = {(float)(px + listW * 0.46f + 4), (float)editY, (float)(listW * 0.42f), 24};
            if (CheckCollisionPointRec(mouse, createBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                int newCid = 1; while (all.count(newCid)) newCid++;
                Country nc; nc.id = newCid; nc.name = "New Country";
                nc.color = Color{(uint8_t)(rand()%200), (uint8_t)(rand()%200), (uint8_t)(rand()%200), 255};
                nc.isoA3 = ""; nc.treasury = 1000.0f;
                all[newCid] = nc; m_selectedCountry = newCid; m_dirty = true;
            }
        }

        // ── Compass sliders ──
        editY += 34;
        if (editY + 120 < maxEditY) {
            editY += 16;
            int sliderW = listW - 10;
            // Economic slider drag
            editY += 14;
            Rectangle econSlide = {(float)px, (float)editY, (float)sliderW, 10};
            if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, {econSlide.x-4, econSlide.y-4, econSlide.width+8, econSlide.height+8})) {
                float pct = (GetMouseX() - econSlide.x) / econSlide.width;
                pct = std::max(0.0f, std::min(1.0f, pct));
                c.compassEconomic = pct * 200.0f - 100.0f;
                m_dirty = true;
            }
            editY += 30;
            // Social slider drag
            editY += 14;
            Rectangle socSlide = {(float)px, (float)editY, (float)sliderW, 10};
            if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, {socSlide.x-4, socSlide.y-4, socSlide.width+8, socSlide.height+8})) {
                float pct = (GetMouseX() - socSlide.x) / socSlide.width;
                pct = std::max(0.0f, std::min(1.0f, pct));
                c.compassSocial = pct * 200.0f - 100.0f;
                m_dirty = true;
            }
            editY += 30;
            // ── Doctrine ──
            editY += 6 + 16;
            Rectangle docRect = {(float)px, (float)editY, (float)listW, 22};
            if (CheckCollisionPointRec(mouse, docRect) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                m_editingDoctrine = true;
                m_editingDoctrineText = c.doctrine;
            }
            if (m_editingDoctrine) {
                if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
                    if (IsKeyPressed(KEY_ENTER)) { c.doctrine = m_editingDoctrineText; m_dirty = true; }
                    m_editingDoctrine = false;
                }
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, docRect)) {
                    c.doctrine = m_editingDoctrineText; m_dirty = true;
                    m_editingDoctrine = false;
                }
                int key = GetCharPressed();
                while (key > 0) {
                    if (key >= 32 && key < 128 && m_editingDoctrineText.size() < 40)
                        m_editingDoctrineText.push_back((char)key);
                    key = GetCharPressed();
                }
                if (IsKeyPressed(KEY_BACKSPACE) && !m_editingDoctrineText.empty()) m_editingDoctrineText.pop_back();
            }
            editY += 28;

            // ── Research ──
            editY += 16;
            Rectangle resRect = {(float)px, (float)editY, (float)listW, 22};
            if (CheckCollisionPointRec(mouse, resRect) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                m_editingResearch = true;
                std::string joined;
                for (size_t ri = 0; ri < c.research.size(); ++ri) {
                    if (ri > 0) joined += ",";
                    joined += c.research[ri];
                }
                m_editingResearchText = joined;
            }
            if (m_editingResearch) {
                if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
                    if (IsKeyPressed(KEY_ENTER)) {
                        c.research.clear();
                        std::string cur;
                        for (char ch : m_editingResearchText) {
                            if (ch == ',') { if (!cur.empty()) c.research.push_back(cur); cur.clear(); }
                            else cur.push_back(ch);
                        }
                        if (!cur.empty()) c.research.push_back(cur);
                        m_dirty = true;
                    }
                    m_editingResearch = false;
                }
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, resRect)) {
                    c.research.clear();
                    std::string cur;
                    for (char ch : m_editingResearchText) {
                        if (ch == ',') { if (!cur.empty()) c.research.push_back(cur); cur.clear(); }
                        else cur.push_back(ch);
                    }
                    if (!cur.empty()) c.research.push_back(cur);
                    m_dirty = true;
                    m_editingResearch = false;
                }
                int key = GetCharPressed();
                while (key > 0) {
                    if ((key >= 32 && key < 128 || key == ',') && m_editingResearchText.size() < 200)
                        m_editingResearchText.push_back((char)key);
                    key = GetCharPressed();
                }
                if (IsKeyPressed(KEY_BACKSPACE) && !m_editingResearchText.empty()) m_editingResearchText.pop_back();
            }
        }
    }
}

void MapEditor::drawCountryPanel() {
    int px = m_screenW - m_panelW + 12, y = m_toolbarH + 16;
    DrawText("Country Editor", px, y, 18, ACCENT); y += 30;

    auto& all = m_editCountries.getAll();
    if (all.empty()) {
        DrawText("No countries loaded.", px, y, 14, GRAY); y += 18;
        DrawText("Generate a world first.", px, y, 14, GRAY);
        return;
    }

    std::vector<int> cids;
    for (auto& [cid, c] : all) cids.push_back(cid);
    std::sort(cids.begin(), cids.end());

    int panelH = m_screenH - m_toolbarH - m_bottomH;
    const int LIST_START = m_toolbarH + 50;
    const int LIST_HEIGHT = (int)(panelH * 0.4f);
    int listW = m_panelW - 24;
    int itemH = 18;
    int visible = LIST_HEIGHT / itemH;

    DrawText(TextFormat("Countries: %zu", all.size()), px, LIST_START - 14, 12, LIGHTGRAY);

    // Scrollbar
    float totalH = (float)cids.size() * itemH;
    float viewH = (float)visible * itemH;
    if (totalH > viewH) {
        float thumbH = viewH / totalH * viewH;
        float thumbY = LIST_START + (float)m_countryScroll / std::max(1, (int)cids.size() - visible) * (viewH - thumbH);
        DrawRectangle(px + listW + 2, LIST_START, 6, (int)viewH, Color{50,50,60,255});
        DrawRectangle(px + listW + 2, (int)thumbY, 6, (int)thumbH, Color{100,100,120,255});
    }

    // Draw compact list
    for (int i = m_countryScroll; i < (int)cids.size() && i < m_countryScroll + visible; ++i) {
        int cid = cids[i];
        auto it = all.find(cid);
        if (it == all.end()) continue;
        Country& c = it->second;
        int yi = LIST_START + (i - m_countryScroll) * itemH;
        bool sel = (cid == m_selectedCountry);
        Color bg = sel ? ColorAlpha(ACCENT, 0.12f) : Color{0,0,0,0};
        if (!sel) {
            Vector2 mp = GetMousePosition();
            if (mp.x >= px && mp.x < px + listW && mp.y >= yi && mp.y < yi + itemH)
                bg = Color{255,255,255,8};
        }
        DrawRectangle(px, yi, listW, itemH - 1, bg);
        DrawRectangle(px + 2, yi + 2, 10, 10, c.color);
        const char* label = c.name.c_str();
        int tw = MeasureText(label, 12);
        if (tw > listW - 20) {
            std::string sn = c.name.substr(0, (listW - 24) / 7) + "..";
            DrawText(sn.c_str(), px + 16, yi + 2, 12, sel ? ACCENT : WHITE);
        } else {
            DrawText(label, px + 16, yi + 2, 12, sel ? ACCENT : WHITE);
        }
    }

    // ── Selected country details ──
    if (m_selectedCountry >= 0) {
        auto it = all.find(m_selectedCountry);
        if (it == all.end()) return;
        Country& c = it->second;

        int editY = LIST_START + LIST_HEIGHT + 6;
        int maxEditY = m_screenH - m_bottomH - 50;

        // Divider line
        DrawLine(px, editY - 3, px + listW, editY - 3, Color{60,60,70,255});

        // Name
        DrawText("Name:", px, editY, 12, LIGHTGRAY); editY += 16;
        Rectangle nameRect = {(float)px, (float)editY, (float)listW, 24};
        bool nameHov = CheckCollisionPointRec(GetMousePosition(), nameRect);
        Color nameBg = m_editingCountryName ? Color{35,35,50,255} : (nameHov ? Color{30,30,40,255} : Color{25,25,35,255});
        DrawRectangleRounded(nameRect, 0.06f, 4, nameBg);
        DrawRectangleRoundedLines(nameRect, 0.06f, 4, m_editingCountryName ? ACCENT : Color{60,60,70,255});
        const char* nameDisplay = m_editingCountryName ? m_editingNameText.c_str() : c.name.c_str();
        DrawText(nameDisplay, (int)nameRect.x + 6, (int)nameRect.y + 4, 13, m_editingCountryName ? ACCENT : WHITE);
        if (m_editingCountryName)
            DrawText("|", (int)(nameRect.x + 6 + MeasureText(nameDisplay, 13)), (int)nameRect.y + 4, 13, ACCENT);

        // Flag preview (cached — only re-render when selection changes)
        editY += 30;
        Rectangle flagRect = {(float)px, (float)editY, 120, 60};
        DrawRectangleLinesEx(flagRect, 1, Color{60,60,70,255});
        if (m_flagPreviewCountry != m_selectedCountry) {
            if (m_flagPreviewTex.id > 0) UnloadTexture(m_flagPreviewTex);
            m_flagPreviewTex = {};
            m_flagPreviewCountry = m_selectedCountry;
            if (!c.flagActual.imagePath.empty()) {
                Image img = LoadImage(c.flagActual.imagePath.c_str());
                if (img.data) {
                    ImageResize(&img, 240, 120);
                    m_flagPreviewTex = LoadTextureFromImage(img);
                    UnloadImage(img);
                }
            } else {
                Texture2D genFlag = FlagRenderer::render(c.flagActual, 240, 120, m_dataDir);
                if (genFlag.id > 0) m_flagPreviewTex = genFlag;
            }
        }
        if (m_flagPreviewTex.id > 0) {
            float sc = fminf(flagRect.width/240.0f, flagRect.height/120.0f);
            float fw = 240*sc, fh = 120*sc;
            DrawTexturePro(m_flagPreviewTex, {0,0,240,120},
                {flagRect.x+(flagRect.width-fw)/2, flagRect.y+(flagRect.height-fh)/2, fw, fh}, {0,0}, 0, WHITE);
        } else {
            DrawText("No flag", (int)flagRect.x + 30, (int)flagRect.y + 22, 12, GRAY);
        }
        DrawText("Drop .svg to set your own flag", (int)(px + flagRect.width + 6), (int)editY + 2, 10, GRAY);

        // Color swatch + ID
        DrawRectangle(px + (int)flagRect.width + 6, (int)editY + 20, 20, 14, c.color);
        DrawText(TextFormat("ID: %d", m_selectedCountry), px + (int)flagRect.width + 30, (int)editY + 20, 11, DARKGRAY);

        // Buttons (if space allows)
        editY += (int)flagRect.height + 10;
        if (editY + 28 < maxEditY) {
            Rectangle delBtn = {(float)px, (float)editY, (float)(listW * 0.42f), 24};
            drawButton("Delete", delBtn, false, 11);
            Rectangle createBtn = {(float)(px + listW * 0.46f + 4), (float)editY, (float)(listW * 0.42f), 24};
            drawButton("Create", createBtn, false, 11);
        }

        // ── Compass sliders ──
        editY += 34;
        if (editY + 120 < maxEditY) {
            DrawText("Compass Options:", px, editY, 12, LIGHTGRAY); editY += 16;
            int sliderW = listW - 10;
            // Economic
            DrawText("Economic:", px, editY, 11, DARKGRAY);
            float econPct = (c.compassEconomic + 100.0f) / 200.0f;
            econPct = std::max(0.0f, std::min(1.0f, econPct));
            Rectangle econSlide = {(float)px, (float)(editY + 14), (float)sliderW, 10};
            DrawRectangleRounded(econSlide, 0.3f, 4, Color{50,50,60,255});
            Rectangle econFill = {econSlide.x + 2, econSlide.y + 2, (econSlide.width - 4) * econPct, econSlide.height - 4};
            if (econFill.width > 2) DrawRectangleRounded(econFill, 0.3f, 4, ACCENT);
            DrawText(TextFormat("%+.0f", c.compassEconomic), (int)(px + sliderW + 6), (int)editY + 12, 11, WHITE);
            DrawText("Left", px + 2, editY + 26, 9, GRAY);
            DrawText("Right", (int)(px + sliderW - 30), editY + 26, 9, GRAY);
            editY += 42;

            // Social
            DrawText("Social:", px, editY, 11, DARKGRAY);
            float socPct = (c.compassSocial + 100.0f) / 200.0f;
            socPct = std::max(0.0f, std::min(1.0f, socPct));
            Rectangle socSlide = {(float)px, (float)(editY + 14), (float)sliderW, 10};
            DrawRectangleRounded(socSlide, 0.3f, 4, Color{50,50,60,255});
            Rectangle socFill = {socSlide.x + 2, socSlide.y + 2, (socSlide.width - 4) * socPct, socSlide.height - 4};
            if (socFill.width > 2) DrawRectangleRounded(socFill, 0.3f, 4, ACCENT);
            DrawText(TextFormat("%+.0f", c.compassSocial), (int)(px + sliderW + 6), (int)editY + 12, 11, WHITE);
            DrawText("Auth", px + 2, editY + 26, 9, GRAY);
            DrawText("Lib", (int)(px + sliderW - 20), editY + 26, 9, GRAY);
            editY += 42;

            // ── Doctrine ──
            editY += 6;
            DrawText("Doctrine:", px, editY, 11, LIGHTGRAY); editY += 16;
            Rectangle docRect = {(float)px, (float)editY, (float)listW, 22};
            bool docHov = CheckCollisionPointRec(GetMousePosition(), docRect);
            Color docBg = m_editingDoctrine ? Color{35,35,50,255} : (docHov ? Color{30,30,40,255} : Color{25,25,35,255});
            DrawRectangleRounded(docRect, 0.06f, 4, docBg);
            DrawRectangleRoundedLines(docRect, 0.06f, 4, m_editingDoctrine ? ACCENT : Color{60,60,70,255});
            const char* docStr = m_editingDoctrine ? m_editingDoctrineText.c_str() : (c.doctrine.empty() ? "(none)" : c.doctrine.c_str());
            DrawText(docStr, (int)docRect.x + 6, (int)docRect.y + 3, 12, (m_editingDoctrine || !c.doctrine.empty()) ? WHITE : GRAY);
            if (m_editingDoctrine) DrawText("|", (int)(docRect.x + 6 + MeasureText(docStr, 12)), (int)docRect.y + 3, 12, ACCENT);
            editY += 28;

            // ── Research ──
            DrawText("Research (comma-separated):", px, editY, 11, LIGHTGRAY); editY += 16;
            Rectangle resRect = {(float)px, (float)editY, (float)listW, 22};
            bool resHov = CheckCollisionPointRec(GetMousePosition(), resRect);
            Color resBg = m_editingResearch ? Color{35,35,50,255} : (resHov ? Color{30,30,40,255} : Color{25,25,35,255});
            DrawRectangleRounded(resRect, 0.06f, 4, resBg);
            DrawRectangleRoundedLines(resRect, 0.06f, 4, m_editingResearch ? ACCENT : Color{60,60,70,255});
            std::string resDisplay;
            if (m_editingResearch) {
                resDisplay = m_editingResearchText;
            } else if (c.research.empty()) {
                resDisplay = "(auto-unlocked by infrastructure)";
            } else {
                for (size_t ri = 0; ri < c.research.size(); ++ri) {
                    if (ri > 0) resDisplay += ",";
                    resDisplay += c.research[ri];
                }
            }
            const char* resStr = resDisplay.c_str();
            DrawText(resStr, (int)resRect.x + 6, (int)resRect.y + 3, 12, (!c.research.empty() || m_editingResearch) ? WHITE : GRAY);
            if (m_editingResearch) DrawText("|", (int)(resRect.x + 6 + MeasureText(resStr, 12)), (int)resRect.y + 3, 12, ACCENT);
        }
    }
}

// ── Resource Panel ─────────────────────────────────────────────

void MapEditor::updateResourcePanel() {}
void MapEditor::drawResourcePanel() {
    int px = m_screenW - m_panelW + 12, y = m_toolbarH + 16;
    DrawText("Resource Brush", px, y, 18, ACCENT); y += 35;
    const char* res[] = {"Oil", "Gold", "Metal", "Rubber", "Gemstones"};
    for (int i = 0; i < 5; i++) {
        Rectangle r = {(float)px, (float)y, 200, 30};
        drawButton(res[i], r, i == 0, 14);
        y += 35;
    }
}

// ── Relations Panel ─────────────────────────────────────────────

void MapEditor::updateRelationsPanel() {}
void MapEditor::drawRelationsPanel() {
    int px = m_screenW - m_panelW + 12, y = m_toolbarH + 16;
    DrawText("Relations Editor", px, y, 18, ACCENT); y += 35;
    DrawText("Select countries to", px, y, 14, GRAY); y += 18;
    DrawText("set relations.", px, y, 14, GRAY);
}

// ── Troops Panel ───────────────────────────────────────────────

void MapEditor::updateTroopsPanel() {}
void MapEditor::drawTroopsPanel() {
    int px = m_screenW - m_panelW + 12, y = m_toolbarH + 16;
    DrawText("Troop Editor", px, y, 18, ACCENT); y += 35;
    DrawText("Click a province to", px, y, 14, GRAY); y += 18;
    DrawText("add/remove troops.", px, y, 14, GRAY);
}

// ── Navy Panel ─────────────────────────────────────────────────

void MapEditor::updateNavyPanel() {}
void MapEditor::drawNavyPanel() {
    int px = m_screenW - m_panelW + 12, y = m_toolbarH + 16;
    DrawText("Navy Editor", px, y, 18, ACCENT); y += 35;
    DrawText("Click ocean to place", px, y, 14, GRAY); y += 18;
    DrawText("ships.", px, y, 14, GRAY);
}

// ── Script Panel ───────────────────────────────────────────────

void MapEditor::updateScriptPanel() {}
void MapEditor::drawScriptPanel() {
    int px = m_screenW - m_panelW + 12, y = m_toolbarH + 16;
    DrawText("Script Editor", px, y, 18, ACCENT); y += 35;
    Rectangle box = {(float)px, (float)y, (float)(m_panelW-24), 350};
    DrawRectangleRounded(box, 0.05f, 4, Color{20,20,30,255});
    DrawRectangleRoundedLines(box, 0.05f, 4, Color{70,70,80,255});
}

// ── Metadata Panel ─────────────────────────────────────────────

void MapEditor::updateMetadataPanel() {}
void MapEditor::drawMetadataPanel() {
    int px = m_screenW - m_panelW + 12, y = m_toolbarH + 16;
    DrawText("Map Metadata", px, y, 18, ACCENT); y += 35;
    DrawText(TextFormat("Name: %s", m_mapName.c_str()), px, y, 14, WHITE); y += 25;
    DrawText("Author: ...", px, y, 14, WHITE); y += 25;
    DrawText("License: CC-BY-4.0", px, y, 14, WHITE);
}