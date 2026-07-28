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
#include "Audio.h"
#include "GameInternals.h"
#include "GifEncoder.h"
#include "SaveManager.h"
#include "json.hpp"
#include "miniz.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/stat.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

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

// Zero-padded turn tag, matching the "%05d" format SaveManager writes with.
//
// The three call sites each built this by hand, padding with
// std::string(5 - std::to_string(turn).size(), '0'). That subtraction is
// int minus size_t, so any turn of six digits or more
// underflows to about 1.8e19 and the std::string construction throws
// length_error rather than producing a name. Unreachable at one turn per month
// -- turn 100000 is the year 10333 -- but it is an underflow, and the two sides
// also disagreed about the format past five digits. snprintf widens instead.
static std::string turnTag(int turn) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%05d", turn);
    return std::string(buf);
}

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
            savePath, "turns/s_" + turnTag(t) + ".json").empty();
        out.push_back(cur);
    }
    return true;
}

// ─── Standalone map data ─────────────────────────────────

// History can be opened straight from the save browser, with no game loaded,
// so pull the province image + countries out of the save's embedded .odmap.
bool Game::loadHistoryMapData(const std::string& savePath) {
    auto odm = SaveManager::extractODM(savePath);
    if (odm.empty()) return false;

    // provinces.png bytes + provinces.json
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, odm.data(), odm.size(), 0)) return false;
    std::vector<uint8_t> pngBytes;
    int pidx = mz_zip_reader_locate_file(&z, "provinces.png", nullptr, 0);
    if (pidx >= 0) {
        size_t sz = 0;
        void* d = mz_zip_reader_extract_to_heap(&z, pidx, &sz, 0);
        if (d) { pngBytes.assign((uint8_t*)d, (uint8_t*)d + sz); mz_free(d); }
    }
    mz_zip_reader_end(&z);

    std::string provJson = odmapEntry(odm, "provinces.json");
    std::string countryJson = odmapEntry(odm, "countries.json");
    if (pngBytes.empty() || provJson.empty() || countryJson.empty()) return false;

    m_provinces.loadFromMemory(pngBytes.data(), (int)pngBytes.size(), provJson);
    m_countries.loadFromJson(countryJson);
    restoreRebels(savePath); // so rebel-owned territory has a colour
    return m_provinces.getImage().data != nullptr;
}

// ─── Frame rendering ─────────────────────────────────────

// Renders one interpolated frame at the requested size straight from the CPU
// province image — no offscreen GPU pass needed. `t` blends a->b so ownership
// changes cross-fade instead of popping, and ships glide between positions.
void Game::renderHistoryFrame(const TurnSnapshot& a, const TurnSnapshot& b, float t,
                              int outW, int outH, std::vector<uint8_t>& rgba,
                              HistoryView view) {
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

    // For the heatmap views, find the max so colours span the full range.
    auto peak = [](const std::unordered_map<int, long long>& m) -> long long {
        long long mx = 1;
        for (auto& [k, v] : m) if (v > mx) mx = v;
        return mx;
    };
    long long popMaxA = 1, popMaxB = 1, trMaxA = 1, trMaxB = 1;
    if (view == HV_POPULATION) { popMaxA = peak(a.population); popMaxB = peak(b.population); }
    if (view == HV_TROOPS)     { trMaxA = peak(a.troops);      trMaxB = peak(b.troops); }

    // A blue→red heat ramp for the value views.
    auto heat = [&](const std::unordered_map<int, long long>& m, long long mx, int pid,
                    bool onLand) -> Color {
        if (!onLand) return SEA;
        auto it = m.find(pid);
        double v = (it == m.end()) ? 0.0 : (double)it->second / (double)mx;
        v = v < 0 ? 0 : (v > 1 ? 1 : v);
        // sqrt so small-but-nonzero values are visible
        v = std::sqrt(v);
        return Color{(uint8_t)(40 + 215 * v), (uint8_t)(60 * (1 - v)),
                     (uint8_t)(200 * (1 - v) + 30 * v), 255};
    };

    // Country borders. The live map derives its outlines from the province
    // image at source resolution (MapRenderer::computeBorderTexture), but this
    // frame point-samples that image at mapW/outW — roughly 6:1 for a 960px
    // GIF — so a source-resolution border mask would lose ~5 of every 6 of its
    // pixels. Detect the edge at OUTPUT resolution instead: compare each
    // pixel's owner against its right and below neighbours.
    //
    // Only country edges, not province ones: at 480x240 a province is about
    // five pixels across, so outlining provinces would darken most of the land
    // into mush. Ownership is taken from whichever snapshot the frame is
    // weighted toward, so borders flip at the midpoint of a transition rather
    // than smearing.
    const std::unordered_map<int, int>& ownerNow = (t < 0.5f) ? a.owner : b.owner;

    // Owner per OUTPUT pixel, computed once. Used for the border pass and the
    // gradient below, both of which would otherwise re-sample the source image
    // several times per pixel.
    std::vector<int32_t> cidOut((size_t)outW * outH, 0);
    for (int y = 0; y < outH; ++y) {
        int sy = (int)((int64_t)y * mapH / outH);
        for (int x = 0; x < outW; ++x) {
            int sx = (int)((int64_t)x * mapW / outW);
            const Color& c = src[(size_t)sy * mapW + sx];
            auto it = ownerNow.find(Province::colorToId(c.r, c.g, c.b));
            cidOut[(size_t)y * outW + x] = (it == ownerNow.end()) ? 0 : it->second;
        }
    }
    auto ownerAtOut = [&](int ox, int oy) -> int {
        if (ox >= outW) ox = outW - 1;
        if (oy >= outH) oy = outH - 1;
        return cidOut[(size_t)oy * outW + ox];
    };

    // Border-distance field, so the political view carries the same inward
    // gradient the live map draws (generatePoliticalTexture in Game_Loading.cpp:
    // chamfer 2-3 from every country edge, capped, then blended toward grey).
    //
    // The cap is scaled to output resolution. In game the field runs 60 SOURCE
    // pixels inward and the player sees it scaled by however far they are zoomed
    // out; at 640px for the whole world that is 60 * 640 / 8192, about five
    // pixels. Using 60 here would flood every country to full darkening and the
    // gradient would vanish into a flat, muddier colour.
    const int gradCap = (view == HV_POLITICAL)
        ? std::max(6, (int)((int64_t)60 * outW / mapW)) : 0;
    std::vector<int16_t> gdist;
    if (gradCap > 0) {
        gdist.assign((size_t)outW * outH, (int16_t)gradCap);
        for (int y = 0; y < outH; ++y)
            for (int x = 0; x < outW; ++x) {
                const size_t i = (size_t)y * outW + x;
                const int32_t c = cidOut[i];
                if ((x + 1 < outW && cidOut[i + 1] != c) ||
                    (x > 0 && cidOut[i - 1] != c) ||
                    (y + 1 < outH && cidOut[i + outW] != c) ||
                    (y > 0 && cidOut[i - outW] != c))
                    gdist[i] = 0;
            }
        // Relax against the eight neighbours until settled: orthogonal +2,
        // diagonal +3, the same metric the game's BFS uses.
        for (int pass = 0; pass < gradCap; ++pass) {
            bool moved = false;
            for (int y = 0; y < outH; ++y)
                for (int x = 0; x < outW; ++x) {
                    const size_t i = (size_t)y * outW + x;
                    int best = gdist[i];
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (!dx && !dy) continue;
                            int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= outW || ny >= outH) continue;
                            int cand = gdist[(size_t)ny * outW + nx] + ((dx && dy) ? 3 : 2);
                            if (cand < best) best = cand;
                        }
                    if (best < gdist[i]) { gdist[i] = (int16_t)best; moved = true; }
                }
            if (!moved) break;
        }
    }

    for (int y = 0; y < outH; ++y) {
        int sy = (int)((int64_t)y * mapH / outH);
        for (int x = 0; x < outW; ++x) {
            int sx = (int)((int64_t)x * mapW / outW);
            const Color& pc = src[(size_t)sy * mapW + sx];
            int pid = Province::colorToId(pc.r, pc.g, pc.b);
            Color ca, cb;
            if (view == HV_POPULATION) {
                bool land = a.owner.count(pid) && a.owner.at(pid) != 0;
                bool landB = b.owner.count(pid) && b.owner.at(pid) != 0;
                ca = heat(a.population, popMaxA, pid, land);
                cb = heat(b.population, popMaxB, pid, landB);
            } else if (view == HV_TROOPS) {
                bool land = a.owner.count(pid) && a.owner.at(pid) != 0;
                bool landB = b.owner.count(pid) && b.owner.at(pid) != 0;
                ca = heat(a.troops, trMaxA, pid, land);
                cb = heat(b.troops, trMaxB, pid, landB);
            } else {
                ca = ownerColor(a.owner, pid);
                cb = ownerColor(b.owner, pid);
            }
            size_t o = ((size_t)y * outW + x) * 4;
            float mr = ca.r + (cb.r - ca.r) * t;
            float mg = ca.g + (cb.g - ca.g) * t;
            float mb = ca.b + (cb.b - ca.b) * t;

            if (gradCap > 0) {
                // blendColor(base, g) from Game_Loading.cpp, and the same sea
                // ramp, so a frame matches what the map looks like in play.
                const float g = std::min(1.0f, (float)gdist[(size_t)y * outW + x]
                                                   / (float)gradCap);
                if (cidOut[(size_t)y * outW + x] == 0) {
                    const float inv = 1.0f - g;
                    mr = 8.0f + inv * 16.0f;
                    mg = 10.0f + inv * 22.0f;
                    mb = 15.0f + inv * 38.0f;
                } else {
                    mr = mr * (1.0f - g * 0.4f) + 40.0f * g * 0.3f;
                    mg = mg * (1.0f - g * 0.4f) + 40.0f * g * 0.3f;
                    mb = mb * (1.0f - g * 0.4f) + 40.0f * g * 0.3f;
                }
            }
            rgba[o + 0] = (uint8_t)std::min(255.0f, std::max(0.0f, mr));
            rgba[o + 1] = (uint8_t)std::min(255.0f, std::max(0.0f, mg));
            rgba[o + 2] = (uint8_t)std::min(255.0f, std::max(0.0f, mb));
            rgba[o + 3] = 255;

            // Draw the edge on the owned side only, so coastlines and country
            // borders both come out as a single clean line rather than a
            // double-width one straddling the boundary.
            auto oit = ownerNow.find(pid);
            int ownHere = (oit == ownerNow.end()) ? 0 : oit->second;
            if (ownHere != 0 &&
                (ownerAtOut(x + 1, y) != ownHere || ownerAtOut(x, y + 1) != ownHere)) {
                rgba[o + 0] = (uint8_t)(rgba[o + 0] * 0.45f);
                rgba[o + 1] = (uint8_t)(rgba[o + 1] * 0.45f);
                rgba[o + 2] = (uint8_t)(rgba[o + 2] * 0.45f);
            }
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

std::string Game::defaultTimelapsePath(const std::string& savePath, int w, int h) const {
    std::string base = savePath;
    auto slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    auto dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    std::string name = base + "_" + std::to_string(w) + "x" + std::to_string(h) + ".gif";
#ifdef __EMSCRIPTEN__
    // Web: the path is irrelevant (virtual FS); the browser download uses the
    // filename only.
    return name;
#else
    // Native: default to the Desktop if it exists, else the home dir, so the
    // GIF lands somewhere the user can actually find it.
    const char* home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE");
    if (home) {
        std::string desktop = std::string(home) + "/Desktop";
        struct stat st;
        if (stat(desktop.c_str(), &st) == 0 && (st.st_mode & S_IFDIR))
            return desktop + "/" + name;
        return std::string(home) + "/" + name;
    }
    return m_dataDir + "timelapses/" + name;
#endif
}

bool Game::exportTimelapseHeadless(const std::string& savePath,
                                   const std::string& outPath,
                                   int outW, int outH, int subFrames,
                                   HistoryView view) {
    m_headless = true;
    m_historyView = view;
    if (!loadHistoryMapData(savePath)) {
        fprintf(stderr, "could not read map data out of %s\n", savePath.c_str());
        return false;
    }
    std::string msg;
    bool ok = exportHistoryGif(savePath, outW, outH, subFrames, outPath, msg);
    printf("%s\n", msg.empty() ? (ok ? "exported" : "failed") : msg.c_str());
    return ok;
}

bool Game::exportHistoryGif(const std::string& savePath, int outW, int outH,
                            int subFrames, const std::string& destPath, std::string& outMsg) {
    std::vector<TurnSnapshot> snaps;
    if (!buildTurnSnapshots(savePath, snaps) || snaps.size() < 2) {
        outMsg = "Need at least 2 turns to export";
        return false;
    }

    // On web we encode to a temp path in the virtual FS, then hand the bytes
    // to the browser as a download. Natively we write straight to destPath.
#ifdef __EMSCRIPTEN__
    std::string writePath = "/tmp_timelapse.gif";
#else
    std::string writePath = destPath;
    // Make sure the parent directory exists (best effort).
    auto slash = writePath.find_last_of('/');
    if (slash != std::string::npos)
        system(("mkdir -p \"" + writePath.substr(0, slash) + "\"").c_str());
#endif

    GifEncoder gif;
    if (!gif.begin(writePath, outW, outH, 8)) {
        outMsg = "Could not create GIF at that path";
        return false;
    }

    std::vector<uint8_t> frame;
    // Sample keyframes AND mid-transition frames for the palette: sampling only
    // pure turn states leaves blend colours with no palette entry, so every
    // cross-fade would quantise to the nearest endpoint and snap.
    int sampleStep = std::max<int>(1, (int)snaps.size() / 8);
    for (size_t i = 0; i < snaps.size(); i += sampleStep) {
        renderHistoryFrame(snaps[i], snaps[i], 0.0f, outW, outH, frame, m_historyView);
        gif.addPaletteSample(frame.data());
        if (i + 1 < snaps.size())
            for (float t : {0.25f, 0.5f, 0.75f}) {
                renderHistoryFrame(snaps[i], snaps[i + 1], t, outW, outH, frame, m_historyView);
                gif.addPaletteSample(frame.data());
            }
    }
    renderHistoryFrame(snaps.back(), snaps.back(), 0.0f, outW, outH, frame, m_historyView);
    gif.addPaletteSample(frame.data());

    int total = (int)(snaps.size() - 1) * subFrames + 1;
    int done = 0;
    for (size_t i = 0; i + 1 < snaps.size(); ++i)
        for (int s = 0; s < subFrames; ++s) {
            float t = (float)s / (float)subFrames;
            renderHistoryFrame(snaps[i], snaps[i + 1], t, outW, outH, frame, m_historyView);
            gif.writeFrame(frame.data());
            if ((++done % 8) == 0 && !m_headless) {
                setLoadingProgress((float)done / total, "Rendering timelapse...");
                drawLoadingScreen();
            }
        }
    renderHistoryFrame(snaps.back(), snaps.back(), 0.0f, outW, outH, frame, m_historyView);
    gif.writeFrame(frame.data());

    int n = gif.frameCount();
    bool ok = gif.end();
    if (!ok) { outMsg = "GIF export failed"; return false; }

#ifdef __EMSCRIPTEN__
    // Read the encoded file back and trigger a browser download so it lands on
    // the user's real machine rather than the sandbox FS.
    {
        std::string fname = destPath.empty() ? "timelapse.gif" : destPath;
        auto slash2 = fname.find_last_of('/');
        if (slash2 != std::string::npos) fname = fname.substr(slash2 + 1);
        std::string js =
            "var d=FS.readFile('" + writePath + "');"
            "var b=new Blob([d],{type:'image/gif'});"
            "var u=URL.createObjectURL(b);var a=document.createElement('a');"
            "a.href=u;a.download='" + fname + "';document.body.appendChild(a);"
            "a.click();document.body.removeChild(a);URL.revokeObjectURL(u);";
        emscripten_run_script(js.c_str());
        outMsg = "Downloaded " + fname + " (" + std::to_string(n) + " frames)";
    }
#else
    outMsg = "Saved " + std::to_string(n) + " frames to " + writePath;
#endif
    std::cout << "  Timelapse: " << outMsg << std::endl;
    return true;
}

// ─── Revert ──────────────────────────────────────────────

bool Game::revertToTurn(int turn) {
    std::string savePath = m_historySavePath.empty() ? m_currentSavePath : m_historySavePath;
    if (savePath.empty()) { m_historyStatus = "No save file"; return false; }

    std::string tag = turnTag(turn);
    std::string stateJson = SaveManager::readEntry(savePath, "turns/s_" + tag + ".json");
    if (stateJson.empty()) {
        // Saves made before per-turn snapshots existed can't be restored
        // faithfully — refuse rather than silently load the wrong state.
        m_historyStatus = "Turn " + std::to_string(turn) + " has no snapshot (older save)";
        return false;
    }

    // Opened from the browser (no game running): the save has to be loaded
    // before anything can be rewound onto it. startLoadedGame() is ASYNCHRONOUS
    // — it tears the current world down (deleting the renderer) and then runs
    // one load phase per frame — so rewinding inline here walked all over a
    // world that no longer existed, and generatePoliticalTexture() dereferenced
    // the freed renderer. Hand the rewind to the loader instead; it runs it on
    // its final step, once everything has been rebuilt.
    if (!m_historyFromGame) {
        std::string fname = savePath;
        auto slash = fname.find_last_of('/');
        if (slash != std::string::npos) fname = fname.substr(slash + 1);
        m_inHistory = false;
        startLoadedGame(fname);          // leaves m_currentScreen = SCREEN_LOADING
        // Armed after the load starts: showLoadingScreen() clears any stale request.
        m_pendingRevertSave = savePath;
        m_pendingRevertTurn = turn;
        m_historyStatus = "Loading save to revert to turn " + std::to_string(turn);
        return true;
    }

    return applyTurnRewind(savePath, turn);
}

bool Game::applyTurnRewind(const std::string& savePath, int turn) {
    // Everything below repaints the map, so a world without a renderer is not
    // something we can rewind onto.
    if (!m_renderer || !m_provinces.getImage().data) {
        m_historyStatus = "No world loaded to revert";
        return false;
    }
    std::string stateJson = SaveManager::readEntry(savePath, "turns/s_" +
        turnTag(turn) + ".json");
    if (stateJson.empty()) {
        m_historyStatus = "Turn " + std::to_string(turn) + " has no snapshot (older save)";
        return false;
    }

    std::vector<TurnSnapshot> snaps;
    if (!buildTurnSnapshots(savePath, snaps) || turn >= (int)snaps.size()) {
        m_historyStatus = "Could not reconstruct that turn";
        return false;
    }
    const TurnSnapshot& s = snaps[turn];

    restoreRebels(savePath);
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
    synthesizeMissingRebels(); // same safety net as the normal load path
    loadStateJson(stateJson);
    m_turnNumber = turn;

    buildPopulationLookups();
    reloadBorders();   // regenerates the political texture too

    m_currentSavePath = savePath;
    m_inHistory = false;
    // A save with no player country still needs the selection screen first.
    if (m_playerCountryId > 0) m_currentScreen = SCREEN_PLAYING;
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
static const char* HIST_VIEW_LABELS[] = {"Political", "Population", "Troops"};

void Game::refreshHistoryPreview() {
    if (m_historyIndex < 0 || m_historyIndex >= (int)m_historySnaps.size()) return;
    // Render the selected turn (no interpolation) at a small preview size.
    const int PW = 480, PH = 240;
    std::vector<uint8_t> rgba;
    const TurnSnapshot& s = m_historySnaps[m_historyIndex];
    renderHistoryFrame(s, s, 0.0f, PW, PH, rgba, m_historyView);

    if (m_historyPreviewTex.id > 0) UnloadTexture(m_historyPreviewTex);
    Image img{};
    img.data = rgba.data();
    img.width = PW; img.height = PH;
    img.mipmaps = 1; img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    m_historyPreviewTex = LoadTextureFromImage(img);
    SetTextureFilter(m_historyPreviewTex, TEXTURE_FILTER_BILINEAR);
    m_historyPreviewTurn = m_historyIndex;
    m_historyPreviewView = m_historyView;
}

void Game::openHistoryScreen(const std::string& savePath) {
    m_inHistory = true;
    m_historyStatus.clear();
    m_historySnaps.clear();
    m_historyConfirmRevert = false;
    m_historyEditingDest = false;
    m_historyPreviewTurn = -1;
    m_historyView = HV_POLITICAL;
    m_historySavePath = savePath;
    // "From game" means a live game is running under us (currently unused, since
    // the entry point is the save browser, but revert() honours it).
    m_historyFromGame = (m_currentScreen == SCREEN_PLAYING);

    if (savePath.empty()) {
        m_historyStatus = "No save selected";
        return;
    }
    // If no game is loaded, pull the map out of the save so we can render.
    if (!m_provinces.getImage().data)
        loadHistoryMapData(savePath);

    buildTurnSnapshots(savePath, m_historySnaps);
    m_historyIndex = std::max(0, (int)m_historySnaps.size() - 1);
    m_historyScroll = 0;
    m_historyDestPath = defaultTimelapsePath(savePath, HIST_RES[m_historyResIndex].w,
                                             HIST_RES[m_historyResIndex].h);
    refreshHistoryPreview();
}

void Game::updateHistoryScreen() {
    if (m_historyEditingDest) {
        // The destination text field owns the keyboard while focused.
        int c = GetCharPressed();
        while (c > 0) {
            // Every character the field takes. Jittered, because a
            // typed word is a run of distinct taps, not one tap looped.
            Audio::get().playSfx("key_type", 0.12f);
            if (c >= 32 && c < 127 && m_historyDestPath.size() < 400)
                m_historyDestPath.push_back((char)c);
            c = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !m_historyDestPath.empty())
            m_historyDestPath.pop_back();
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_ESCAPE)) m_historyEditingDest = false;
        return;
    }
    if (IsKeyPressed(KEY_ESCAPE)) { m_inHistory = false; return; }
    int n = (int)m_historySnaps.size();
    if (n > 0) {
        int prev = m_historyIndex;
        if (IsKeyPressed(KEY_UP))   m_historyIndex = std::max(0, m_historyIndex - 1);
        if (IsKeyPressed(KEY_DOWN)) m_historyIndex = std::min(n - 1, m_historyIndex + 1);
        if (m_historyIndex != prev) m_historyConfirmRevert = false;
    }
    float wheel = GetMouseWheelMove();
    if (wheel != 0) m_historyScroll = std::max(0, m_historyScroll - (int)(wheel * 3));

    // Rebuild the preview only when the selection or view actually changes.
    if (m_historyPreviewTurn != m_historyIndex || m_historyPreviewView != m_historyView)
        refreshHistoryPreview();
}

void Game::drawHistoryScreen() {
    int cx = m_screenW / 2;
    Color accent = hexToColor(m_config.accentColor);
    Vector2 mouse = getMouse();
    DrawRectangle(0, 0, m_screenW, m_screenH, {8, 8, 12, 248});

    const char* title = "Turn History";
    DrawText(title, cx - MeasureText(title, 32) / 2, 22, 32, accent);
    DrawText("ESC to close", m_screenW - 130, 28, 13, Color{120, 120, 140, 160});

    int n = (int)m_historySnaps.size();

    // ── Turn list (left) ──
    int listX = 30, listY = 84, listW = 300;
    int rowH = 24;
    int visible = std::max(1, (m_screenH - listY - 70) / rowH);
    m_historyScroll = std::clamp(m_historyScroll, 0, std::max(0, n - visible));

    DrawText("Turns", listX, listY - 20, 15, LIGHTGRAY);
    DrawRectangle(listX, listY, listW, visible * rowH, {16, 16, 22, 255});
    for (int i = m_historyScroll; i < n && i < m_historyScroll + visible; ++i) {
        auto& s = m_historySnaps[i];
        int y = listY + (i - m_historyScroll) * rowH;
        Rectangle r = {(float)listX, (float)y, (float)listW, (float)(rowH - 1)};
        bool hov = CheckCollisionPointRec(mouse, r);
        bool sel = (i == m_historyIndex);
        if (sel)      DrawRectangleRec(r, ColorAlpha(accent, 0.18f));
        else if (hov) DrawRectangleRec(r, {255, 255, 255, 12});
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_historyIndex = i;
            m_historyConfirmRevert = false;
        }
        DrawText(s.turn == 0 ? "Turn 0 (start)" : TextFormat("Turn %d", s.turn),
                 listX + 10, y + 4, 14, sel ? accent : WHITE);
        std::unordered_map<int, int> seen;
        for (auto& [pid, cid] : s.owner) if (cid > 0 && cid < SPC_CID) seen[cid]++;
        DrawText(TextFormat("%d", (int)seen.size()), listX + listW - 90, y + 5, 12, LIGHTGRAY);
        if (!s.hasState && s.turn > 0)
            DrawText("view only", listX + listW - 66, y + 5, 11, Color{170, 140, 90, 255});
    }

    // ── Map preview (right/top) ──
    int px = listX + listW + 30;
    int previewW = std::min(m_screenW - px - 30, 640);
    int previewH = previewW / 2;
    int py = listY;
    if (m_historyPreviewTex.id > 0) {
        DrawTexturePro(m_historyPreviewTex, {0, 0, (float)m_historyPreviewTex.width, (float)m_historyPreviewTex.height},
                       {(float)px, (float)py, (float)previewW, (float)previewH}, {0, 0}, 0, WHITE);
    } else {
        DrawRectangle(px, py, previewW, previewH, {20, 20, 28, 255});
        DrawText("No preview", px + previewW / 2 - 40, py + previewH / 2, 14, GRAY);
    }
    DrawRectangleLines(px, py, previewW, previewH, {60, 60, 75, 255});
    if (n > 0 && m_historyIndex < n) {
        auto& s = m_historySnaps[m_historyIndex];
        DrawText(s.turn == 0 ? "Start of game" : TextFormat("Turn %d", s.turn),
                 px + 6, py + 4, 16, accent);
    }
    py += previewH + 8;

    // View toggle buttons
    DrawText("View:", px, py + 4, 14, LIGHTGRAY);
    int vbx = px + 50;
    for (int v = 0; v < 3; ++v) {
        Rectangle r = {(float)vbx, (float)py, 100, 24};
        bool hov = CheckCollisionPointRec(mouse, r);
        bool sel = (m_historyView == v);
        DrawRectangleRounded(r, 0.15f, 6, sel ? ColorAlpha(accent, 0.22f) : (hov ? Color{40,40,55,255} : Color{24,24,32,255}));
        DrawRectangleRoundedLines(r, 0.15f, 6, sel ? accent : Color{60,60,75,255});
        DrawText(HIST_VIEW_LABELS[v], (int)r.x + 10, (int)r.y + 5, 13, sel ? accent : LIGHTGRAY);
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_historyView = (HistoryView)v;
        vbx += 106;
    }
    py += 34;

    // ── Export controls ──
    DrawText("GIF resolution:", px, py, 14, LIGHTGRAY);
    int rbx = px + 130;
    for (int i = 0; i < HIST_RES_COUNT; ++i) {
        Rectangle r = {(float)rbx, (float)py - 4, 150, 24};
        bool hov = CheckCollisionPointRec(mouse, r);
        bool sel = (i == m_historyResIndex);
        DrawRectangleRounded(r, 0.15f, 6, sel ? ColorAlpha(accent, 0.2f) : (hov ? Color{40,40,55,255} : Color{24,24,32,255}));
        DrawRectangleRoundedLines(r, 0.15f, 6, sel ? accent : Color{60,60,75,255});
        DrawText(HIST_RES[i].label, (int)r.x + 8, (int)r.y + 5, 12, sel ? accent : LIGHTGRAY);
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_historyResIndex = i;
            m_historyDestPath = defaultTimelapsePath(m_historySavePath, HIST_RES[i].w, HIST_RES[i].h);
        }
        rbx += 156;
    }
    py += 30;

    DrawText(TextFormat("Smoothing: %d frames/turn", m_historySubFrames), px, py, 13, LIGHTGRAY);
    {
        Rectangle minus = {(float)(px + 210), (float)py - 4, 24, 20};
        Rectangle plus  = {(float)(px + 238), (float)py - 4, 24, 20};
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
    py += 30;

    // Destination path field
    DrawText("Save GIF to:", px, py, 13, LIGHTGRAY); py += 18;
    {
        Rectangle field = {(float)px, (float)py, (float)std::min(previewW, 560), 26};
        bool hov = CheckCollisionPointRec(mouse, field);
        DrawRectangleRounded(field, 0.1f, 4, m_historyEditingDest ? Color{35,35,50,255} : (hov ? Color{28,28,38,255} : Color{22,22,30,255}));
        DrawRectangleRoundedLines(field, 0.1f, 4, m_historyEditingDest ? accent : Color{60,60,75,255});
        // Show the tail of long paths so the filename stays visible
        std::string shown = m_historyDestPath;
        int maxCh = (int)(field.width - 16) / 8;
        if ((int)shown.size() > maxCh) shown = "..." + shown.substr(shown.size() - maxCh + 3);
        DrawText(shown.c_str(), (int)field.x + 8, (int)field.y + 6, 13, m_historyEditingDest ? accent : WHITE);
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_historyEditingDest = true;
        else if (m_historyEditingDest && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !hov) m_historyEditingDest = false;
    }
    py += 34;

    // ── Action buttons ──
    auto button = [&](const char* label, int y, int w, bool enabled, Color tint) -> bool {
        Rectangle r = {(float)px, (float)y, (float)w, 34};
        bool hov = enabled && CheckCollisionPointRec(mouse, r);
        DrawRectangleRounded(r, 0.15f, 6, enabled ? (hov ? ColorAlpha(tint, 0.30f) : ColorAlpha(tint, 0.15f)) : Color{22,22,28,200});
        DrawRectangleRoundedLines(r, 0.15f, 6, enabled ? tint : Color{50,50,60,150});
        DrawText(label, (int)(r.x + r.width / 2 - MeasureText(label, 15) / 2), (int)r.y + 9, 15,
                 enabled ? WHITE : Color{110,110,120,200});
        return hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    };

    bool haveTurns = n > 1;
    if (button("Download GIF of playthrough", py, std::min(previewW, 560), haveTurns, accent)) {
        std::string msg;
        exportHistoryGif(m_historySavePath, HIST_RES[m_historyResIndex].w,
                         HIST_RES[m_historyResIndex].h, m_historySubFrames, m_historyDestPath, msg);
        m_historyStatus = msg;
    }
    py += 42;

    bool canRevert = (n > 0 && m_historyIndex >= 0 && m_historyIndex < n &&
                      m_historySnaps[m_historyIndex].hasState);
    int revertTurn = (n > 0 && m_historyIndex < n) ? m_historySnaps[m_historyIndex].turn : 0;
    const char* revertLabel = !canRevert ? "Revert (no snapshot for this turn)"
                            : m_historyConfirmRevert ? "Click again to confirm revert"
                            : TextFormat("Revert to turn %d", revertTurn);
    if (button(revertLabel, py, std::min(previewW, 560), canRevert, Color{220,140,70,255})) {
        if (!m_historyConfirmRevert) {
            m_historyConfirmRevert = true;
        } else {
            int t = revertTurn;
            m_paused = false;
            // revertToTurn() owns the screen transition: a revert that needs the
            // save loaded first has to stay on SCREEN_LOADING until the async
            // load finishes, so forcing SCREEN_PLAYING here would strand it.
            revertToTurn(t);
        }
    }
    py += 42;

    int halfW = (std::min(previewW, 560) - 10) / 2;
    // Back to save selection
    {
        Rectangle r = {(float)px, (float)py, (float)halfW, 34};
        bool hov = CheckCollisionPointRec(mouse, r);
        DrawRectangleRounded(r, 0.15f, 6, hov ? Color{50,50,64,255} : Color{30,30,40,255});
        DrawRectangleRoundedLines(r, 0.15f, 6, Color{90,90,110,255});
        DrawText("Back to save selection", (int)(r.x + r.width/2 - MeasureText("Back to save selection", 14)/2), (int)r.y + 10, 14, WHITE);
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_inHistory = false;
            m_currentScreen = SCREEN_FILE_BROWSER;
            m_browsingSaves = true;
        }
    }
    // Close
    {
        Rectangle r = {(float)(px + halfW + 10), (float)py, (float)halfW, 34};
        bool hov = CheckCollisionPointRec(mouse, r);
        DrawRectangleRounded(r, 0.15f, 6, hov ? Color{50,50,64,255} : Color{30,30,40,255});
        DrawRectangleRoundedLines(r, 0.15f, 6, Color{90,90,110,255});
        DrawText("Close", (int)(r.x + r.width/2 - MeasureText("Close", 14)/2), (int)r.y + 10, 14, WHITE);
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_inHistory = false;
    }
    py += 44;

    if (!m_historyStatus.empty())
        DrawText(m_historyStatus.c_str(), px, py, 13, accent);
}
