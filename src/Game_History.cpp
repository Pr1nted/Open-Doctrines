// Turn history: browse past turns, export the playthrough as an animated GIF,
// or revert the game to an earlier turn.
//
// Everything here works off the .odsv save rather than live game state, so
// browsing history never mutates the running game. Per turn the archive holds
// turns/t_NNNNN.dat (binary province/ship/army numbers) and turns/s_NNNNN.json
// (pending orders, policies, research). Deltas store absolute values and only
// list what changed, so a turn is reconstructed by starting from the map's
// baseline and applying every delta up to it.

#include "Game.h"
#include "GameInternals.h"
#include "GifEncoder.h"
#include "SaveManager.h"
#include "json.hpp"
#include "miniz.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {

// Pull one entry out of an in-memory .odmap (itself a zip).
std::string odmapEntry(const std::vector<uint8_t>& odm, const char* name) {
    if (odm.empty()) return {};
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, odm.data(), odm.size(), 0)) return {};
    int idx = mz_zip_reader_locate_file(&z, name, nullptr, 0);
    std::string out;
    if (idx >= 0) {
        size_t sz = 0;
        void* d = mz_zip_reader_extract_to_heap(&z, idx, &sz, 0);
        if (d) { out.assign((char*)d, sz); mz_free(d); }
    }
    mz_zip_reader_end(&z);
    return out;
}

} // namespace

// ─── Snapshot reconstruction ─────────────────────────────

bool Game::buildTurnSnapshots(const std::string& savePath, std::vector<TurnSnapshot>& out) {
    out.clear();
    SaveMetadata meta;
    try { meta = SaveManager::readMetadata(savePath); } catch (...) { return false; }

    // Turn 0 = the map's own starting state, read from the embedded .odmap so
    // this is independent of however far the live game has progressed.
    auto odm = SaveManager::extractODM(savePath);
    TurnSnapshot base;
    base.turn = 0;
    try {
        auto pj = odmapEntry(odm, "provinces.json");
        if (!pj.empty()) {
            auto j = nlohmann::json::parse(pj);
            for (auto& [pidStr, e] : j.items())
                base.owner[std::stoi(pidStr)] = e.value("country_id", 0);
        }
        auto sj = odmapEntry(odm, "ships.json");
        if (!sj.empty()) {
            auto j = nlohmann::json::parse(sj);
            for (auto& e : j)
                base.ships.push_back({e.value("lat", 0.0), e.value("lon", 0.0),
                                      e.value("country_id", 0)});
        }
    } catch (...) { std::cerr << "  History: bad baseline map data" << std::endl; }
    out.push_back(base);

    // Apply each turn's delta on top of the previous snapshot.
    for (int t = 1; t <= meta.turnCount; ++t) {
        TurnSnapshot cur = out.back();
        cur.turn = t;
        TurnDelta d = SaveManager::readTurn(savePath, t);
        for (auto& p : d.provinces) {
            if (p.ownerChanged) cur.owner[p.provinceId] = p.newOwner;
            if (p.populationChanged) cur.population[p.provinceId] = p.newPopulation;
        }
        for (auto& s : d.ships) {
            if (s.shipIndex < 0) continue;
            if (s.shipIndex >= (int)cur.ships.size()) cur.ships.resize(s.shipIndex + 1);
            if (s.latChanged) cur.ships[s.shipIndex].lat = s.newLat;
            if (s.lonChanged) cur.ships[s.shipIndex].lon = s.newLon;
            if (s.countryIdChanged) cur.ships[s.shipIndex].countryId = s.newCountryId;
        }
        for (auto& a : d.armies) {
            long long tot = 0;
            for (auto& u : a.units) tot += u.count;
            cur.troops[a.provinceId] = tot;
        }
        // A per-turn state snapshot means this turn can be fully restored.
        cur.hasState = !SaveManager::readEntry(
            savePath, "turns/s_" + std::string(5 - std::to_string(t).size(), '0') +
                          std::to_string(t) + ".json").empty();
        out.push_back(cur);
    }
    return true;
}

// ─── Frame rendering ─────────────────────────────────────

// Renders one interpolated frame at the requested size straight from the CPU
// province image — no offscreen GPU pass needed. `t` blends a->b so ownership
// changes cross-fade instead of popping, and ships glide between positions.
void Game::renderHistoryFrame(const TurnSnapshot& a, const TurnSnapshot& b, float t,
                              int outW, int outH, std::vector<uint8_t>& rgba) {
    rgba.assign((size_t)outW * outH * 4, 255);
    const Image& provImg = m_provinces.getImage();
    if (!provImg.data) return;
    const Color* src = (const Color*)provImg.data;
    int mapW = provImg.width, mapH = provImg.height;

    const Color SEA = {10, 15, 40, 255};
    std::unordered_map<int, Color> colorCache;
    auto ownerColor = [&](const std::unordered_map<int, int>& owners, int pid) -> Color {
        if (pid == 0) return SEA;
        auto it = owners.find(pid);
        if (it == owners.end() || it->second == 0) return SEA;
        auto cc = colorCache.find(it->second);
        if (cc != colorCache.end()) return cc->second;
        const Country* c = m_countries.getCountry(it->second);
        Color col = c ? c->color : Color{80, 80, 80, 255};
        colorCache[it->second] = col;
        return col;
    };

    for (int y = 0; y < outH; ++y) {
        int sy = (int)((int64_t)y * mapH / outH);
        for (int x = 0; x < outW; ++x) {
            int sx = (int)((int64_t)x * mapW / outW);
            const Color& pc = src[(size_t)sy * mapW + sx];
            int pid = Province::colorToId(pc.r, pc.g, pc.b);
            Color ca = ownerColor(a.owner, pid);
            Color cb = ownerColor(b.owner, pid);
            size_t o = ((size_t)y * outW + x) * 4;
            rgba[o + 0] = (uint8_t)(ca.r + (cb.r - ca.r) * t);
            rgba[o + 1] = (uint8_t)(ca.g + (cb.g - ca.g) * t);
            rgba[o + 2] = (uint8_t)(ca.b + (cb.b - ca.b) * t);
            rgba[o + 3] = 255;
        }
    }

    // Ships, interpolated between the two snapshots
    size_t nShips = std::min(a.ships.size(), b.ships.size());
    for (size_t i = 0; i < nShips; ++i) {
        double lat = a.ships[i].lat + (b.ships[i].lat - a.ships[i].lat) * t;
        double lon = a.ships[i].lon + (b.ships[i].lon - a.ships[i].lon) * t;
        int px = (int)((lon + 180.0) / 360.0 * outW);
        int py = (int)((90.0 - lat) / 180.0 * outH);
        int cid = b.ships[i].countryId;
        const Country* c = m_countries.getCountry(cid);
        Color col = c ? c->color : Color{220, 220, 220, 255};
        int r = std::max(1, outW / 320);
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                int nx = px + dx, ny = py + dy;
                if (nx < 0 || nx >= outW || ny < 0 || ny >= outH) continue;
                size_t o = ((size_t)ny * outW + nx) * 4;
                // White rim so ships stay readable over their owner's fill
                bool edge = (abs(dx) == r || abs(dy) == r);
                rgba[o + 0] = edge ? 255 : col.r;
                rgba[o + 1] = edge ? 255 : col.g;
                rgba[o + 2] = edge ? 255 : col.b;
            }
    }
}

// ─── GIF export ──────────────────────────────────────────

bool Game::exportHistoryGif(const std::string& savePath, int outW, int outH,
                            int subFrames, std::string& outPath) {
    std::vector<TurnSnapshot> snaps;
    if (!buildTurnSnapshots(savePath, snaps) || snaps.size() < 2) {
        m_historyStatus = "Need at least 2 turns to export";
        return false;
    }

    std::string base = savePath;
    auto slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    auto dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    outPath = m_dataDir + "timelapses/" + base + "_" + std::to_string(outW) + "x" +
              std::to_string(outH) + ".gif";
    system(("mkdir -p \"" + m_dataDir + "timelapses\"").c_str());

    GifEncoder gif;
    if (!gif.begin(outPath, outW, outH, 8)) {
        m_historyStatus = "Could not start GIF";
        return false;
    }

    std::vector<uint8_t> frame;
    // Sample a spread of keyframes so the palette covers colours that only
    // appear late (rebel states, conquests) — and crucially also sample
    // MID-TRANSITION frames. Without those the blend colours have no palette
    // entry, so every ownership cross-fade quantises to the nearest endpoint
    // and snaps instead of fading.
    int sampleStep = std::max<int>(1, (int)snaps.size() / 8);
    for (size_t i = 0; i < snaps.size(); i += sampleStep) {
        renderHistoryFrame(snaps[i], snaps[i], 0.0f, outW, outH, frame);
        gif.addPaletteSample(frame.data());
        if (i + 1 < snaps.size()) {
            for (float t : {0.25f, 0.5f, 0.75f}) {
                renderHistoryFrame(snaps[i], snaps[i + 1], t, outW, outH, frame);
                gif.addPaletteSample(frame.data());
            }
        }
    }
    renderHistoryFrame(snaps.back(), snaps.back(), 0.0f, outW, outH, frame);
    gif.addPaletteSample(frame.data());

    int total = (int)(snaps.size() - 1) * subFrames + 1;
    int done = 0;
    for (size_t i = 0; i + 1 < snaps.size(); ++i) {
        for (int s = 0; s < subFrames; ++s) {
            float t = (float)s / (float)subFrames;
            renderHistoryFrame(snaps[i], snaps[i + 1], t, outW, outH, frame);
            gif.writeFrame(frame.data());
            if ((++done % 8) == 0) {
                setLoadingProgress((float)done / total, "Rendering timelapse...");
                drawLoadingScreen();
            }
        }
    }
    renderHistoryFrame(snaps.back(), snaps.back(), 0.0f, outW, outH, frame);
    gif.writeFrame(frame.data());

    int n = gif.frameCount();
    bool ok = gif.end();
    m_historyStatus = ok ? ("Saved " + std::to_string(n) + " frames to " + outPath)
                         : "GIF export failed";
    std::cout << "  Timelapse: " << m_historyStatus << std::endl;
    return ok;
}

// ─── Revert ──────────────────────────────────────────────

bool Game::revertToTurn(int turn) {
    if (m_currentSavePath.empty()) { m_historyStatus = "No save file loaded"; return false; }
    std::string tag = std::string(5 - std::to_string(turn).size(), '0') + std::to_string(turn);
    std::string stateJson = SaveManager::readEntry(m_currentSavePath, "turns/s_" + tag + ".json");
    if (stateJson.empty()) {
        // Saves made before per-turn snapshots existed can't be restored
        // faithfully — better to refuse than to silently load the wrong state.
        m_historyStatus = "Turn " + std::to_string(turn) + " has no snapshot (older save)";
        return false;
    }

    std::vector<TurnSnapshot> snaps;
    if (!buildTurnSnapshots(m_currentSavePath, snaps) || turn >= (int)snaps.size()) {
        m_historyStatus = "Could not reconstruct that turn";
        return false;
    }
    const TurnSnapshot& s = snaps[turn];

    restoreRebels(m_currentSavePath);
    for (auto& [pid, cid] : s.owner) {
        Province* p = m_provinces.getProvinceById(pid);
        if (p) p->countryId = cid;
        if (pid >= 0 && pid < (int)m_provinceCountryLookup.size())
            m_provinceCountryLookup[pid] = cid;
    }
    for (auto& [pid, pop] : s.population) m_provincePopulations[pid] = pop;
    for (size_t i = 0; i < s.ships.size() && i < m_ships.size(); ++i) {
        m_ships[i].lat = s.ships[i].lat;
        m_ships[i].lon = s.ships[i].lon;
        m_ships[i].countryId = s.ships[i].countryId;
    }
    loadStateJson(stateJson);
    m_turnNumber = turn;

    buildPopulationLookups();
    generatePoliticalTexture();
    reloadBorders();

    m_historyStatus = "Reverted to turn " + std::to_string(turn);
    std::cout << "  " << m_historyStatus << std::endl;
    return true;
}

// ─── History screen ──────────────────────────────────────

// Output presets. The map is 2:1, so these keep that aspect.
static const struct { int w, h; const char* label; } HIST_RES[] = {
    {480, 240,  "480x240 (small)"},
    {960, 480,  "960x480 (medium)"},
    {1920, 960, "1920x960 (large)"},
};
static const int HIST_RES_COUNT = 3;

void Game::openHistoryScreen() {
    m_inHistory = true;
    m_historyStatus.clear();
    m_historySnaps.clear();
    if (!m_currentSavePath.empty()) {
        buildTurnSnapshots(m_currentSavePath, m_historySnaps);
        m_historyIndex = (int)m_historySnaps.size() - 1;
    } else {
        m_historyStatus = "No save file — play a turn first";
    }
    if (m_historyIndex < 0) m_historyIndex = 0;
    m_historyScroll = 0;
}

void Game::updateHistoryScreen() {
    if (IsKeyPressed(KEY_ESCAPE)) { m_inHistory = false; return; }
    int n = (int)m_historySnaps.size();
    if (n > 0) {
        if (IsKeyPressed(KEY_UP))   m_historyIndex = std::max(0, m_historyIndex - 1);
        if (IsKeyPressed(KEY_DOWN)) m_historyIndex = std::min(n - 1, m_historyIndex + 1);
    }
    float wheel = GetMouseWheelMove();
    if (wheel != 0) m_historyScroll = std::max(0, m_historyScroll - (int)(wheel * 3));
}

void Game::drawHistoryScreen() {
    int cx = m_screenW / 2;
    Color accent = hexToColor(m_config.accentColor);
    DrawRectangle(0, 0, m_screenW, m_screenH, {8, 8, 12, 245});

    const char* title = "Turn History";
    int tw = MeasureText(title, 34);
    DrawText(title, cx - tw / 2, 28, 34, accent);

    Vector2 mouse = getMouse();
    int n = (int)m_historySnaps.size();

    // ── Turn list ──
    int listX = 40, listY = 92, listW = m_screenW / 2 - 70;
    int rowH = 26;
    int visible = std::max(1, (m_screenH - listY - 150) / rowH);
    m_historyScroll = std::clamp(m_historyScroll, 0, std::max(0, n - visible));

    DrawText("Turns", listX, listY - 22, 16, LIGHTGRAY);
    DrawRectangle(listX, listY, listW, visible * rowH, {16, 16, 22, 255});
    for (int i = m_historyScroll; i < n && i < m_historyScroll + visible; ++i) {
        auto& s = m_historySnaps[i];
        int y = listY + (i - m_historyScroll) * rowH;
        Rectangle r = {(float)listX, (float)y, (float)listW, (float)(rowH - 1)};
        bool hov = CheckCollisionPointRec(mouse, r);
        bool sel = (i == m_historyIndex);
        if (sel)      DrawRectangleRec(r, ColorAlpha(accent, 0.18f));
        else if (hov) DrawRectangleRec(r, {255, 255, 255, 12});
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_historyIndex = i;

        DrawText(s.turn == 0 ? "Turn 0 (start)" : TextFormat("Turn %d", s.turn),
                 listX + 10, y + 5, 15, sel ? accent : WHITE);
        // Countries still holding land that turn — a rough shape of the game
        int alive = 0;
        std::unordered_map<int, int> seen;
        for (auto& [pid, cid] : s.owner) if (cid > 0 && cid < SPC_CID) seen[cid]++;
        alive = (int)seen.size();
        DrawText(TextFormat("%d countries", alive), listX + listW - 130, y + 6, 12, LIGHTGRAY);
        if (!s.hasState && s.turn > 0)
            DrawText("no snapshot", listX + listW - 240, y + 6, 11, Color{170, 140, 90, 255});
    }

    // ── Detail + actions ──
    int px = m_screenW / 2 + 20, py = listY;
    if (n > 0 && m_historyIndex >= 0 && m_historyIndex < n) {
        auto& s = m_historySnaps[m_historyIndex];
        DrawText(s.turn == 0 ? "Start of game" : TextFormat("Turn %d", s.turn), px, py, 22, accent);
        py += 34;
        long long totalPop = 0;
        for (auto& [pid, p] : s.population) totalPop += p;
        DrawText(TextFormat("Provinces tracked: %d", (int)s.owner.size()), px, py, 14, LIGHTGRAY); py += 20;
        DrawText(TextFormat("Ships: %d", (int)s.ships.size()), px, py, 14, LIGHTGRAY); py += 20;
        if (totalPop > 0) { DrawText(TextFormat("Recorded population: %lld", totalPop), px, py, 14, LIGHTGRAY); py += 20; }
        py += 10;
    }

    // Resolution picker
    DrawText("GIF resolution:", px, py, 14, LIGHTGRAY); py += 20;
    for (int i = 0; i < HIST_RES_COUNT; ++i) {
        Rectangle r = {(float)px, (float)py, 240, 24};
        bool hov = CheckCollisionPointRec(mouse, r);
        bool sel = (i == m_historyResIndex);
        DrawRectangleRounded(r, 0.15f, 6, sel ? ColorAlpha(accent, 0.2f) : (hov ? Color{40,40,55,255} : Color{24,24,32,255}));
        DrawRectangleRoundedLines(r, 0.15f, 6, sel ? accent : Color{60,60,75,255});
        DrawText(HIST_RES[i].label, px + 10, py + 5, 13, sel ? accent : LIGHTGRAY);
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_historyResIndex = i;
        py += 28;
    }
    py += 6;
    DrawText(TextFormat("Smoothing: %d frames per turn", m_historySubFrames), px, py, 13, LIGHTGRAY);
    {
        Rectangle minus = {(float)(px + 230), (float)py - 4, 24, 20};
        Rectangle plus  = {(float)(px + 258), (float)py - 4, 24, 20};
        for (int i = 0; i < 2; ++i) {
            Rectangle r = i ? plus : minus;
            bool hov = CheckCollisionPointRec(mouse, r);
            DrawRectangleRounded(r, 0.2f, 4, hov ? Color{55,55,70,255} : Color{30,30,40,255});
            DrawRectangleRoundedLines(r, 0.2f, 4, Color{80,80,95,255});
            DrawText(i ? "+" : "-", (int)r.x + 9, (int)r.y + 3, 14, WHITE);
            if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
                m_historySubFrames = std::clamp(m_historySubFrames + (i ? 1 : -1), 1, 12);
        }
    }
    py += 34;

    // ── Buttons ──
    auto button = [&](const char* label, int y, bool enabled, Color tint) -> bool {
        Rectangle r = {(float)px, (float)y, 300, 34};
        bool hov = enabled && CheckCollisionPointRec(mouse, r);
        DrawRectangleRounded(r, 0.15f, 6, enabled ? (hov ? ColorAlpha(tint, 0.30f) : ColorAlpha(tint, 0.15f))
                                                  : Color{22, 22, 28, 200});
        DrawRectangleRoundedLines(r, 0.15f, 6, enabled ? tint : Color{50, 50, 60, 150});
        int lw = MeasureText(label, 15);
        DrawText(label, (int)(r.x + r.width / 2 - lw / 2), (int)r.y + 9, 15,
                 enabled ? WHITE : Color{110, 110, 120, 200});
        return hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    };

    bool haveTurns = n > 1;
    if (button("Download GIF of playthrough", py, haveTurns, accent)) {
        std::string out;
        exportHistoryGif(m_currentSavePath, HIST_RES[m_historyResIndex].w,
                         HIST_RES[m_historyResIndex].h, m_historySubFrames, out);
    }
    py += 42;

    bool canRevert = (n > 0 && m_historyIndex >= 0 && m_historyIndex < n &&
                      m_historySnaps[m_historyIndex].hasState);
    if (button(canRevert ? TextFormat("Revert to turn %d", m_historySnaps[m_historyIndex].turn)
                         : "Revert (no snapshot for this turn)",
               py, canRevert, Color{220, 140, 70, 255})) {
        if (revertToTurn(m_historySnaps[m_historyIndex].turn)) {
            m_inHistory = false;
            m_paused = false;
        }
    }
    py += 42;

    if (button("Back to save selection", py, true, Color{140, 140, 160, 255})) {
        m_inHistory = false;
        m_paused = false;
        unloadGameData();
        m_currentScreen = SCREEN_FILE_BROWSER;
        m_browsingSaves = true;
    }
    py += 42;

    if (button("Close", py, true, Color{110, 110, 125, 255})) m_inHistory = false;

    if (!m_historyStatus.empty()) {
        int sw = MeasureText(m_historyStatus.c_str(), 14);
        DrawText(m_historyStatus.c_str(), cx - sw / 2, m_screenH - 40, 14, accent);
    }
    DrawText("ESC to close", m_screenW - 130, 30, 13, Color{120, 120, 140, 160});
}
