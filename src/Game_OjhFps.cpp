#include "Game.h"
#include "GameInternals.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

// ─── Objective Judge Horizon frame-rate scenes ────────────
// `OpenDoctrines --ojh-fps <seconds> <save.odsv> [turns]`
//
// Times every frame in the scenes Objective Judge Horizon measures in every game --
// the menu, the map at the start, zoomed out, zoomed in, scrolling, the heaviest panel
// and the map after [turns] turns -- and prints one `OJH scene` line per scene. Vsync
// and the frame cap are off for the run. Each scene settles for two seconds after it is
// set up before any frame counts, so a rebuilt texture or a first draw is not timed.
// Built on the screenshot tour's hook points: one tick at the end of every frame.

namespace {

using OjhClock = std::chrono::steady_clock;

struct OjhFpsRun {
    double seconds = 5.0;
    std::string save;
    int turns = 20;
    int stage = 0;
    std::string scene;
    std::vector<double> frames;
    OjhClock::time_point last, warmUntil, sceneEnd;
    bool haveLast = false;
    float homeX = 0.0f, homeY = 0.0f, homeZoom = 1.0f;
};

OjhFpsRun g_ojh;

void ojhReportScene(const std::string& name, std::vector<double> frames) {
    if (frames.size() < 2) {
        printf("OJH noscene %s too few frames were drawn to time\n", name.c_str());
        fflush(stdout);
        return;
    }
    std::sort(frames.begin(), frames.end());
    const size_t n = frames.size();
    double total = 0.0;
    for (double f : frames) total += f;
    const size_t slowCount = std::max<size_t>(1, n / 100);
    double slow = 0.0;
    for (size_t i = n - slowCount; i < n; ++i) slow += frames[i];
    auto percentile = [&](double p) { return frames[std::min(n - 1, (size_t)(p * (double)(n - 1)))] * 1000.0; };
    printf("OJH scene %s %zu %.5f %.3f %.3f %.3f %.2f\n", name.c_str(), n, total, percentile(0.50),
           percentile(0.95), percentile(0.99), slow > 0.0 ? (double)slowCount / slow : 0.0);
    fflush(stdout);
}

}  // namespace

void Game::beginOjhFps(double seconds, const std::string& savePath, int turns) {
    m_ojhFps = true;
    g_ojh = OjhFpsRun{};
    g_ojh.seconds = seconds > 0.0 ? seconds : 5.0;
    g_ojh.save = savePath;
    g_ojh.turns = std::max(0, turns);
    m_currentScreen = SCREEN_MENU;
    m_inSettings = false;
    ClearWindowState(FLAG_VSYNC_HINT);
    SetTargetFPS(0);
    printf("OJH renderer raylib %s, OpenGL\nOJH resolution %dx%d\nOJH vsync off\n", RAYLIB_VERSION, GetRenderWidth(),
           GetRenderHeight());
    fflush(stdout);
}

bool Game::tickOjhFps() {
    const auto now = OjhClock::now();
    if (!g_ojh.scene.empty() && g_ojh.haveLast && now >= g_ojh.warmUntil)
        g_ojh.frames.push_back(std::chrono::duration<double>(now - g_ojh.last).count());
    g_ojh.last = now;
    g_ojh.haveLast = true;

    auto begin = [&](const char* name, int settleSeconds = 2) {
        g_ojh.scene = name;
        g_ojh.frames.clear();
        // From the clock now, not from the start of this tick: the late-game turns run in
        // between, and a scene timed from before them would already be over.
        g_ojh.warmUntil = OjhClock::now() + std::chrono::seconds(settleSeconds);
        g_ojh.sceneEnd = g_ojh.warmUntil +
                         std::chrono::duration_cast<OjhClock::duration>(std::chrono::duration<double>(g_ojh.seconds));
    };
    auto finished = [&]() { return !g_ojh.scene.empty() && OjhClock::now() >= g_ojh.sceneEnd; };
    auto report = [&]() {
        ojhReportScene(g_ojh.scene, g_ojh.frames);
        g_ojh.scene.clear();
    };
    auto home = [&](float zoom) {
        if (m_renderer) m_renderer->snapTo(g_ojh.homeX, g_ojh.homeY, zoom);
    };

    switch (g_ojh.stage) {
        case 0:
            begin("menu");
            g_ojh.stage = 1;
            return true;

        case 1: {
            if (!finished()) return true;
            report();
            if (g_ojh.save.empty()) {
                printf("OJH noscene map-start no save was given to --ojh-fps\n");
                fflush(stdout);
                return false;
            }
            // A save continues a game; a map file (.odmap) starts a new one on that
            // world, the way the eval does, so a benchmark can put Open Doctrines on
            // the same map as another game.
            const bool mapFile = g_ojh.save.size() > 6 && g_ojh.save.compare(g_ojh.save.size() - 6, 6, ".odmap") == 0;
            if (mapFile) startLoading(g_ojh.save);
            else startLoadedGame(g_ojh.save);
            while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE) {
                if (WindowShouldClose()) return false;
                updateLoading();
            }
            if (m_loadingFailed) {
                fprintf(stderr, "[OJH] could not load %s\n", g_ojh.save.c_str());
                return false;
            }
            hideLoadingScreen();
            m_currentScreen = SCREEN_PLAYING;
            m_paused = false;
            m_inSettings = false;
            if (m_renderer) {
                m_renderer->snapViewMode(MapRenderer::ViewMode::Flat);
                g_ojh.homeX = m_renderer->getCameraTarget().x;
                g_ojh.homeY = m_renderer->getCameraTarget().y;
                g_ojh.homeZoom = m_renderer->getZoom();
            }
            begin("map-start", 5);
            g_ojh.stage = 2;
            return true;
        }

        case 2:
            if (!finished()) return true;
            report();
            home(m_renderer ? m_renderer->getMinZoom() : 1.0f);
            begin("map-out");
            g_ojh.stage = 3;
            return true;

        case 3:
            if (!finished()) return true;
            report();
            home(5.0f);
            begin("map-in");
            g_ojh.stage = 4;
            return true;

        case 4:
            if (!finished()) return true;
            report();
            home(g_ojh.homeZoom);
            begin("map-pan");
            g_ojh.stage = 5;
            return true;

        case 5:
            if (m_renderer) {
                const Vector2 at = m_renderer->getCameraTarget();
                const float zoom = std::max(0.01f, m_renderer->getZoom());
                m_renderer->snapTo(at.x + 6.0f / zoom, at.y, zoom);
            }
            if (!finished()) return true;
            report();
            home(g_ojh.homeZoom);
            m_activeSidebarTab = 4;
            m_inResearch = true;
            m_researchTab = 2;
            m_researchZoom = 0.75f;
            begin("panel");
            g_ojh.stage = 6;
            return true;

        case 6:
            if (!finished()) return true;
            report();
            m_inResearch = false;
            printf("OJH noscene end-turn Open Doctrines resolves a turn inside one frame, behind its loading screen\n");
            fflush(stdout);
            for (int i = 0; i < g_ojh.turns; ++i) {
                const auto turnStart = OjhClock::now();
                processTurn();
                // What Continue does: leave the orders phase and repaint the map now, the
                // way a turn does behind its progress bar when that phase is skipped.
                m_turnState = TURN_NORMAL;
                flushMapRepaint();
                fprintf(stderr, "[OJH] late-game turn %d of %d took %.2f s\n", i + 1, g_ojh.turns,
                        std::chrono::duration<double>(OjhClock::now() - turnStart).count());
            }
            m_popupQueue.clear();
            home(g_ojh.homeZoom);
            begin("map-late");
            g_ojh.stage = 7;
            return true;

        case 7:
            if (!finished()) return true;
            report();
            printf("[OJH] frame-rate scenes done\n");
            fflush(stdout);
            return false;

        default:
            return false;
    }
}
