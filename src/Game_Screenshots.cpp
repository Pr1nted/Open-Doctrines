#include "Game.h"
#include "GameInternals.h"
#include "Audio.h"
#include "MapEditor.h"
#include "mods/ModManager.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

// ─── Scripted screenshot tour ────────────────────────────
// `OpenDoctrines --screenshots <dir> [save.odsv]`
//
// Walks a fixed list of screens, waits for each to settle, and writes a PNG.
// Documentation images are the first thing to go stale in a project that keeps
// moving, and they go stale silently -- a screenshot does not fail to compile.
// The only fix that holds is being able to retake all of them with one command.
//
// HOW IT HOOKS IN
//
// One call at the bottom of Game::run(). The tour does not own a loop, does not
// draw, and does not know what any screen looks like: it sets the same state
// the menus set, lets the real frame happen, and captures the result. So a
// screen that changes is photographed as it now is, and a screen that is broken
// photographs as broken rather than as whatever the tour imagined.
//
// SETTLING
//
// Shots are taken several frames after the state is set, never on the same
// frame. Backgrounds scroll, panels animate open, and the map's border texture
// is built on first draw -- capturing immediately catches a screen mid-assembly.

namespace {

struct Shot {
    const char* name;      // file stem; the PNG is <dir>/<name>.png
    int settleFrames;      // frames to let it settle before capturing
    bool needsWorld;       // requires the save to have been loaded
};

// Order matters: everything before the first needsWorld shot is photographed
// while no world is loaded, which is also the cheapest time to photograph it.
const Shot SHOTS[] = {
    {"main-menu",     30, false},
    {"mods",          20, false},
    {"multiplayer",   20, false},
    {"map-editor",    45, false},
    {"world-map",     45, true},
    {"province",      20, true},
    {"policies",      20, true},
    {"economy",       20, true},
    {"research",      20, true},
};
const int SHOT_COUNT = (int)(sizeof(SHOTS) / sizeof(SHOTS[0]));

}  // namespace

void Game::beginScreenshotTour(const std::string& outDir, const std::string& savePath) {
    m_shotTour  = true;
    m_shotDir   = outDir;
    m_shotSave  = savePath;
    m_shotIndex = 0;
    m_shotFrame = 0;
    if (!m_shotDir.empty() && m_shotDir.back() == '/') m_shotDir.pop_back();
    // Same reason as the GIF export in Game_History.cpp: "mkdir -p" is a POSIX
    // command that cmd.exe does not have, and it spawns a shell to do what one
    // library call does on every platform.
    {
        std::error_code ec;
        std::filesystem::create_directories(m_shotDir, ec);
        if (ec)
            fprintf(stderr, "[SHOT] could not create %s: %s\n",
                    m_shotDir.c_str(), ec.message().c_str());
    }

    // The tour starts on the main menu, never on the splash: the splash is a
    // timed fade, so shooting it means racing it.
    m_currentScreen = SCREEN_MENU;
    m_inSettings = false;
    printf("[SHOT] %d screens -> %s\n", SHOT_COUNT, m_shotDir.c_str());
}

// Everything the world shots share: the save loaded, on the map, with a
// province worth looking at already selected. Run once, before the first of
// them, because loading a save costs seconds and the shots differ only in
// which panel is open over it.
//
// Returns false if the world could not be loaded, which fails the tour rather
// than quietly producing four pictures of an empty map.
static bool g_worldReady = false;

bool Game::tickScreenshotTour() {
    if (m_shotIndex >= SHOT_COUNT) {
        printf("[SHOT] done\n");
        return false;
    }
    const Shot& shot = SHOTS[m_shotIndex];

    // ── frame 0: put the game on the screen this shot wants ──
    if (m_shotFrame == 0) {
        if (shot.needsWorld && !g_worldReady) {
            if (m_shotSave.empty()) {
                fprintf(stderr, "[SHOT] %s needs a save and none was given\n", shot.name);
                return false;
            }
            printf("[SHOT] loading %s\n", m_shotSave.c_str());
            startLoadedGame(m_shotSave);
            // Same hand-cranking as the simulation: the loader normally runs a
            // step per frame from run(), and we are inside that frame already.
            while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE) {
                if (WindowShouldClose()) return false;
                updateLoading();
            }
            if (m_loadingFailed) {
                fprintf(stderr, "[SHOT] could not load %s\n", m_shotSave.c_str());
                return false;
            }
            hideLoadingScreen();
            m_currentScreen = SCREEN_PLAYING;

            // Play as the country holding the most ground, so the panels have
            // real numbers in them. A spectator (country 0) renders the same
            // map with every player-facing panel empty, which is a picture of
            // the UI not working.
            {
                std::vector<int> byCountry;
                for (int owner : m_provinceCountryLookup) {
                    if (owner <= 0 || owner >= REBEL_CID_MIN) continue;
                    if ((int)byCountry.size() <= owner) byCountry.resize(owner + 1, 0);
                    byCountry[owner]++;
                }
                int best = 0, bestN = 0;
                for (int cid = 1; cid < (int)byCountry.size(); ++cid)
                    if (byCountry[cid] > bestN) { bestN = byCountry[cid]; best = cid; }
                m_playerCountryId = best;
                printf("[SHOT] playing as country %d (%d provinces)\n", best, bestN);

                // The most populous province we own: the province panel is
                // mostly numbers, and an empty tundra tile shows none of them.
                int pick = 0;
                long bestPop = -1;
                for (auto& [pid, pop] : m_provincePopulations) {
                    if (pid <= 0 || (size_t)pid >= m_provinceCountryLookup.size()) continue;
                    if (m_provinceCountryLookup[pid] != best) continue;
                    if ((long)pop > bestPop) { bestPop = (long)pop; pick = pid; }
                }
                m_shotProvince = pick;
            }
            g_worldReady = true;
        }

        // Every shot starts from a clean slate, so an overlay left open by the
        // previous one cannot end up in this one's picture.
        m_inResearch = m_inEconomy = m_inPolitics = m_inClaims = false;
        m_activeSidebarTab = 0;
        m_inSettings = false;

        // Which province the panels talk about. Cleared for the map shot,
        // because that one is meant to show the map and nothing over it.
        //
        // The selection lives on the RENDERER: update() copies it into
        // m_lastSelectedProvince every frame, so setting the Game-side field
        // alone is undone before anything is drawn.
        if (shot.needsWorld) {
            const int pid = (std::string(shot.name) == "world-map") ? 0 : m_shotProvince;
            if (m_renderer) m_renderer->setSelectedProvince(pid);
            m_lastSelectedProvince = pid;
            if (pid > 0) buildCountryProvinceList(pid);
        }

        const std::string name = shot.name;
        if (name == "main-menu") {
            m_currentScreen = SCREEN_MENU;
        } else if (name == "mods") {
            m_modIndex = m_modScroll = 0;
            m_modAdvancedFor = m_modDeleteFor = m_modAiWarnFor = -1;
            ModManager::get().rescan();
            m_currentScreen = SCREEN_MODS;
        } else if (name == "multiplayer") {
            openMultiplayerMenu();
        } else if (name == "map-editor") {
            if (!m_mapEditor) {
                // Loads synchronously on this thread, exactly as the menu does.
                Audio::get().beginBackgroundPump();
                m_mapEditor = new MapEditor();
                m_mapEditor->init(m_screenW, m_screenH, m_dataDir);
                Audio::get().endBackgroundPump();
            }
            m_currentScreen = SCREEN_MAP_EDITOR;
        } else if (name == "world-map") {
            m_activeViewTab = 0;          // no panel: this shot is the map itself
        } else if (name == "province") {
            m_activeViewTab = 2;          // industry: the busiest of the tabs
        } else if (name == "policies") {
            m_activeSidebarTab = 1;
            m_inPolitics = true;
        } else if (name == "economy") {
            m_activeSidebarTab = 2;
            m_inEconomy = true;
        } else if (name == "research") {
            m_activeSidebarTab = 4;
            m_inResearch = true;
        }
    }

    // ── settle, then capture ──
    if (++m_shotFrame < shot.settleFrames) return true;

    // TakeScreenshot throws the directory away -- it calls GetFileName() on
    // whatever it is handed and writes the result into the working directory.
    // Passing a full path therefore silently drops nine PNGs into the repo
    // root and reports success, so the move has to happen here.
    const std::string file = std::string(shot.name) + ".png";
    const std::string path = m_shotDir + "/" + file;
    TakeScreenshot(file.c_str());
    if (rename(file.c_str(), path.c_str()) != 0) {
        fprintf(stderr, "[SHOT] captured %s but could not move it to %s\n",
                file.c_str(), path.c_str());
        return false;
    }
    printf("[SHOT] %s\n", path.c_str());
    fflush(stdout);

    m_shotIndex++;
    m_shotFrame = 0;
    return m_shotIndex < SHOT_COUNT;
}
