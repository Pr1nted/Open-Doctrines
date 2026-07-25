#include "Game.h"
#include "GameInternals.h"
#include "MapEditor.h"
#include "ai/AISystem.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <unordered_map>
#include <unistd.h>
#ifdef __APPLE__
#include <IOKit/pwr_mgt/IOPMLib.h>
#endif

// ─── Headless AI self-play training ──────────────────────
// `OpenDoctrines --train-ai [maps] [turns] [countries] [seed]`
//   maps      0 = train forever, rotating maps until the window is closed
//   turns     per-map turn cap (default 3000); maps also rotate when decided
//             or after ~1500 turns with no strategic progress
//   countries 0 = the scenario decides; >0 forces a fixed count
//
// Each round: pick a scenario archetype, generate a fresh procedural map from
// it, load it through the exact same pipeline the menu uses (so training sees
// real game state, not a mock), let every country play against each other,
// then rotate. Rotating maps AND scenario shapes is what stops the net from
// overfitting one geography: features are ratios/normalised values, so what
// transfers between worlds is strategy, not memorised terrain. The model file
// is saved every 25 turns and on window close, so a run can be stopped at any
// time without losing progress.

namespace {
// Scenario archetypes: deliberately different strategic worlds. Land wars on
// a pangaea, naval invasions across archipelagos, crowded borders, 1v1 duels…
// Ranges are jittered per round so no two rounds are identical.
struct Scenario {
    const char* name;
    float landMin, landMax;      // landCoverage
    int contMin, contMax;        // numContinents
    float jagMin, jagMax;        // jaggedness
    float densMin, densMax;      // provinceDensity
    int countriesMin, countriesMax;
};
const Scenario SCENARIOS[] = {
    {"pangaea",     0.45f, 0.60f, 1, 2, 0.15f, 0.35f, 0.8f, 1.4f, 20, 45},
    {"continents",  0.30f, 0.42f, 3, 5, 0.20f, 0.40f, 0.8f, 1.2f, 25, 50},
    {"islands",     0.18f, 0.28f, 6, 9, 0.40f, 0.65f, 1.0f, 1.6f, 15, 35},
    {"archipelago", 0.12f, 0.20f, 8, 12, 0.50f, 0.70f, 1.2f, 2.0f, 10, 25},
    {"crowded",     0.35f, 0.50f, 2, 4, 0.20f, 0.40f, 1.4f, 2.2f, 50, 80},
    {"sparse",      0.30f, 0.45f, 2, 5, 0.15f, 0.35f, 0.4f, 0.8f, 8,  16},
    {"duel",        0.35f, 0.50f, 1, 2, 0.20f, 0.40f, 0.8f, 1.2f, 2,  5},
    {"coldwar",     0.30f, 0.45f, 2, 3, 0.20f, 0.40f, 0.9f, 1.3f, 6,  12},
};
const int SCENARIO_COUNT = sizeof(SCENARIOS) / sizeof(SCENARIOS[0]);

float randRange(std::mt19937& rng, float lo, float hi) {
    return lo + (hi - lo) * std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
}
int randRange(std::mt19937& rng, int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng);
}

// Tiny reward sparkline, zero-centered: the midline is reward 0, scale is the
// largest |value| in view (min ±0.25 so near-zero noise doesn't fill the box).
// Above the line = average country gaining, below = losing.
void drawSparkline(const std::deque<float>& hist, Rectangle box, Color col, const char* label) {
    DrawRectangleRec(box, {255, 255, 255, 8});
    DrawRectangleLinesEx(box, 1, {255, 255, 255, 30});
    float midY = box.y + box.height / 2;
    DrawLineV({box.x, midY}, {box.x + box.width, midY}, {255, 255, 255, 25}); // zero axis
    DrawText(label, (int)box.x + 4, (int)box.y + 2, 10, {200, 200, 200, 180});
    if (hist.size() < 2) return;
    float mag = 0.25f;
    for (float v : hist) mag = std::max(mag, std::fabs(v));
    float halfH = box.height / 2 - 10;
    float dx = box.width / (hist.size() - 1);
    Vector2 prev{};
    for (size_t i = 0; i < hist.size(); ++i) {
        Vector2 pt = {box.x + i * dx, midY - hist[i] / mag * halfH};
        if (i) DrawLineV(prev, pt, col);
        prev = pt;
    }
    DrawText(TextFormat("%+.2f", hist.back()), (int)(box.x + box.width - 38),
             (int)box.y + 2, 10, col);
}
} // namespace

void Game::runAITraining(int numMaps, int turnsPerMap, int numCountries, unsigned int baseSeed) {
    const bool infinite = numMaps <= 0;
    printf("[TRAIN] Self-play: %s x %d turn(s)%s, base seed %u\n",
           infinite ? "endless maps" : TextFormat("%d map(s)", numMaps), turnsPerMap,
           numCountries > 0 ? TextFormat(", %d countries fixed", numCountries) : ", scenario-sized countries",
           baseSeed);
    printf("[TRAIN] Model: %sai/model.bin  (close the window any time — progress is saved)\n",
           m_dataDir.c_str());

    auto runStart = std::chrono::steady_clock::now();
    std::mt19937 rng(baseSeed);
    long long totalTurns = 0;
    bool aborted = false;

    // Training must never wait on the display: VSync would add up to 16ms per
    // turn once the simulation itself gets fast. Restored on exit.
    applyFpsTarget(-1);

#ifdef __APPLE__
    // Keep the Mac awake for overnight runs. This blocks IDLE system sleep
    // (the timer that sleeps an idle machine), so a locked screen or an idle
    // desktop keeps training. It does NOT override closing a laptop lid —
    // built-in-display clamshell sleep can't be prevented; use an external
    // display in clamshell, or keep the lid open.
    IOPMAssertionID sleepAssertion = 0;
    IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep,
                                kIOPMAssertionLevelOn,
                                CFSTR("OpenDoctrines AI training"), &sleepAssertion);
#endif

    // Mini political map: a 256x128 ownership sample refreshed every few
    // turns. 32k pixel lookups — microseconds, unlike the real 33M-pixel
    // political texture the training mode skips.
    const int MINI_W = 256, MINI_H = 128;
    std::vector<Color> miniPix((size_t)MINI_W * MINI_H, BLACK);
    Texture2D miniTex{};
    auto refreshMiniMap = [&]() {
        const Image& img = m_provinces.getImage();
        if (!img.data || img.width <= 0) return;
        const Color* src = (const Color*)img.data;
        for (int y = 0; y < MINI_H; ++y) {
            int sy = y * img.height / MINI_H;
            for (int x = 0; x < MINI_W; ++x) {
                int sx = x * img.width / MINI_W;
                Color pc = src[(size_t)sy * img.width + sx];
                int pid = Province::colorToId(pc.r, pc.g, pc.b);
                Color out = {12, 24, 40, 255}; // sea
                if (pid > 0 && pid < (int)m_provinceCountryLookup.size()) {
                    int cid = m_provinceCountryLookup[pid];
                    if (cid > 0) {
                        const Country* cc = m_countries.getCountry(cid);
                        out = cc ? cc->color : Color{90, 90, 90, 255};
                        out.a = 255;
                    } else {
                        out = {60, 60, 60, 255}; // unowned land
                    }
                }
                miniPix[(size_t)y * MINI_W + x] = out;
            }
        }
        if (miniTex.id == 0) {
            Image mi{miniPix.data(), MINI_W, MINI_H, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            miniTex = LoadTextureFromImage(mi);
        } else {
            UpdateTexture(miniTex, miniPix.data());
        }
    };

    for (int m = 0; (infinite || m < numMaps) && !aborted; ++m) {
        // ── 1. Pick a scenario, jitter its parameters ──
        // Cycle archetypes so every shape gets equal training time, jitter
        // everything else so no two rounds are the same world.
        const Scenario& sc = SCENARIOS[m % SCENARIO_COUNT];
        MapEditor::GeneratorParams p;
        p.seed = (int)(rng() & 0x7FFFFFFF);
        p.landCoverage = randRange(rng, sc.landMin, sc.landMax);
        p.numContinents = randRange(rng, sc.contMin, sc.contMax);
        p.jaggedness = randRange(rng, sc.jagMin, sc.jagMax);
        p.provinceDensity = randRange(rng, sc.densMin, sc.densMax);
        p.numCountries = numCountries > 0 ? numCountries
                                          : randRange(rng, sc.countriesMin, sc.countriesMax);

        printf("[TRAIN] Map %d%s: scenario=%s seed=%d land=%.2f continents=%d jag=%.2f density=%.2f countries=%d\n",
               m + 1, infinite ? "" : TextFormat("/%d", numMaps), sc.name, p.seed,
               p.landCoverage, p.numContinents, p.jaggedness, p.provinceDensity, p.numCountries);

        // Tear the previous world down BEFORE generating the next map, so the
        // old Game's ~1.5 GB of per-pixel buffers aren't still resident while
        // the MapEditor allocates another ~1 GB to build the new one — that
        // overlap is what pushed the peak into an OOM kill on big maps. Also
        // persists the AI model before the map rotates. (Harmless no-op on the
        // very first round when nothing is loaded yet.)
        unloadGameData();

        // Per-process temp map name: a second trainer (or a stray teardown
        // from a previous run) must not delete the map this one just built.
        std::string mapName = TextFormat("__ai_training_%d__", (int)getpid());
        std::string odmPath;
        {
            MapEditor ed;
            ed.init(m_screenW, m_screenH, m_dataDir);
            odmPath = ed.generateAndExportHeadless(p, mapName.c_str());
        }
        if (odmPath.empty()) {
            printf("[TRAIN] Map generation failed, skipping round\n");
            continue;
        }
        if (WindowShouldClose()) break;

        // ── 2. Load it through the normal pipeline (no world save) ──
        // (World already torn down above, before generation.)
        startLoading(odmPath);
        while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE)
            updateLoading();
        if (m_loadingFailed) {
            printf("[TRAIN] Failed to load generated map, skipping round\n");
            continue;
        }
        hideLoadingScreen();
        m_currentSavePath.clear(); // never write an .odsv during training
        m_playerCountryId = 0;     // spectator: every country is AI-driven
        m_aiTraining = true;

        // ── 3. Self-play ──
        auto mapStart = std::chrono::steady_clock::now();
        int alive = 0;
        // Stagnation must be measured by STRATEGIC progress, not raw ownership.
        // Rebel provinces flip owner every turn (rebellion spawns, parent
        // reconquers), so the raw lookup always differs — that bug kept maps
        // running for thousands of turns and overtrained the model on one map.
        // Instead: the map is progressing while the biggest real country keeps
        // growing OR someone gets eliminated. Rebel churn does neither.
        // Window sized so a map fights a good long war (mid/late game, naval
        // invasions, research payoffs) — ~1500-3000 turns — before we call it
        // frozen and rotate. Too small (the old 300) rotated maps mid-conflict;
        // too large overfits the model to one geography.
        const int STAGNATION_TURNS = 1500;
        int bestTerritory = 0;   // high-water: largest real-country province count
        int fewestAlive = 1 << 30;
        int turnsSinceProgress = 0;
        double lastFrameSec = -1; // wall-clock of the last dashboard draw (this map)
        for (int t = 0; t < turnsPerMap; ++t) {
            if (WindowShouldClose()) { aborted = true; break; }
            processTurn();
            totalTurns++;

            // Count REAL countries that actually hold land (conquered shells
            // stay in m_countries but own nothing), and the biggest one's size.
            std::unordered_map<int, int> realCount;
            int maxReal = 0;
            for (int owner : m_provinceCountryLookup)
                if (owner > 0 && owner < REBEL_CID_MIN) {
                    int n = ++realCount[owner];
                    if (n > maxReal) maxReal = n;
                }
            alive = (int)realCount.size();
            if (alive <= 1) {
                printf("[TRAIN] map %d decided after %d turns (1 country left)\n", m + 1, t + 1);
                break;
            }
            if (maxReal > bestTerritory || alive < fewestAlive) {
                bestTerritory = std::max(bestTerritory, maxReal);
                fewestAlive = std::min(fewestAlive, alive);
                turnsSinceProgress = 0;
            } else if (++turnsSinceProgress >= STAGNATION_TURNS) {
                printf("[TRAIN] map %d strategically frozen after %d turns "
                       "(no real conquest in %d) — rotating\n",
                       m + 1, t + 1, STAGNATION_TURNS);
                break;
            }

            // Crash resilience: the model is tiny, save it often.
            if (m_ai && (t + 1) % 25 == 0) m_ai->saveModel();

            double mapSecs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - mapStart).count() / 1000.0;
            // Throttle the dashboard to ~20 fps: once the simulation is fast,
            // drawing every turn would dominate the frame budget AND a slept /
            // powered-off display can stall the buffer swap. Training never
            // waits on the display — it just draws when a frame is due. We
            // still MUST poll events every turn (PollInputEvents) so the window
            // stays responsive and WindowShouldClose fires promptly.
            const double FRAME_INTERVAL = 0.05; // seconds
            bool drawFrame = (lastFrameSec < 0) || (mapSecs - lastFrameSec) >= FRAME_INTERVAL;
            if (!drawFrame) { PollInputEvents(); continue; }
            lastFrameSec = mapSecs;
            refreshMiniMap();
            BeginDrawing();
            ClearBackground(BLACK);
            DrawText("AI SELF-PLAY TRAINING", 40, 30, 30, {255, 215, 0, 255});
            // Run clock (whole session) + per-map clock
            {
                long long rs = (long long)std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::steady_clock::now() - runStart).count();
                long long ms2 = (long long)mapSecs;
                DrawText(TextFormat("run %02lld:%02lld:%02lld   map %02lld:%02lld:%02lld",
                                    rs / 3600, (rs / 60) % 60, rs % 60,
                                    ms2 / 3600, (ms2 / 60) % 60, ms2 % 60),
                         m_screenW - 340, 36, 20, {255, 215, 0, 255});
            }
            DrawText(TextFormat("Map %d%s [%s]   turn %d   %.2f s/turn   total %lld turns", m + 1,
                                infinite ? "" : TextFormat("/%d", numMaps), sc.name,
                                t + 1, mapSecs / (t + 1), totalTurns),
                     40, 72, 20, RAYWHITE);
            if (m_ai) {
                const auto& ts = m_ai->trainStats();
                DrawText(TextFormat("alive %d   wars %lld   ceasefire offers %lld   pacts %lld   research done %lld",
                                    alive, ts.warsDeclared, ts.ceasefiresOffered,
                                    ts.pactsProposed, ts.researchCompleted),
                         40, 100, 20, LIGHTGRAY);
                // Reward trends per module — the actual "is it learning" view:
                // rising lines mean the average country is doing better.
                static const char* RLBL[] = {"economy reward", "politics reward", "war reward", "navy reward"};
                static const Color RCOL[] = {{120, 220, 120, 255}, {120, 170, 255, 255},
                                             {255, 140, 120, 255}, {180, 160, 255, 255}};
                for (int mm = 0; mm < 4; ++mm)
                    drawSparkline(m_ai->rewardHistory()[mm],
                                  {40.0f + (mm % 2) * 215.0f, 132.0f + (mm / 2) * 62.0f, 205.0f, 56.0f},
                                  RCOL[mm], RLBL[mm]);
                int y = 268;
                for (const auto& line : m_ai->debugLines(14)) {
                    DrawText(line.c_str(), 40, y, 12, {180, 200, 180, 220});
                    y += 14;
                }
            }
            // Live world map (ownership only, sampled)
            float mapBottom = 72;
            if (miniTex.id) {
                float mw = (float)std::min(m_screenW - 520, 640);
                float mh = mw / 2;
                Rectangle dst = {(float)m_screenW - mw - 40, 72.0f, mw, mh};
                DrawTexturePro(miniTex, {0, 0, (float)MINI_W, (float)MINI_H}, dst, {0, 0}, 0, WHITE);
                DrawRectangleLinesEx(dst, 1, {255, 255, 255, 40});
                mapBottom = dst.y + dst.height;
            }
            // Model + hyperparameter panel
            if (m_ai) {
                int iy = (int)mapBottom + 14;
                int ix = m_screenW - (int)std::min(m_screenW - 520, 640) - 40;
                Color dim = {170, 180, 190, 220};
                DrawText("MODEL", ix, iy, 14, {255, 215, 0, 200}); iy += 20;
                DrawText(TextFormat("%lld parameters in 9 nets (%.1f MB on disk)",
                                    m_ai->paramCount(), m_ai->lastSaveBytes() / 1048576.0),
                         ix, iy, 14, dim); iy += 18;
                DrawText(TextFormat("policy 96-512-320-A x4   value 96-160-1 x4   diplo 96-256-160-2"),
                         ix, iy, 14, dim); iy += 18;
                DrawText(TextFormat("%llu gradient updates lifetime   lr policy %.3f / value %.3f",
                                    m_ai->totalUpdates(), AISystem::LR_POLICY, AISystem::LR_VALUE),
                         ix, iy, 14, dim); iy += 18;
                float temp, eps;
                m_ai->samplingParams(temp, eps);
                DrawText(TextFormat("sampling: temperature %.2f, %.0f%% random (difficulty %d)",
                                    temp, eps * 100, m_config.aiDifficulty),
                         ix, iy, 14, dim); iy += 18;
                const float* rm = m_ai->rewardMeans();
                DrawText(TextFormat("reward means: econ %+.2f  pol %+.2f  war %+.2f  navy %+.2f",
                                    rm[0], rm[1], rm[2], rm[3]),
                         ix, iy, 14, dim);
            }
            DrawText("Close the window to stop — progress is saved.", 40,
                     m_screenH - 40, 16, GRAY);
            EndDrawing();

            if ((t + 1) % 100 == 0) {
                printf("[TRAIN] map %d turn %d/%d (%.2f s/turn, %d alive)\n",
                       m + 1, t + 1, turnsPerMap, mapSecs / (t + 1), alive);
            }
        }
        m_aiTraining = false;
        if (m_ai) m_ai->saveModel();
    }

    double mins = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - runStart).count() / 60.0;
    printf("[TRAIN] %s: %lld turns in %.1f min. Model saved.\n",
           aborted ? "Stopped" : "Done", totalTurns, mins);
    if (miniTex.id) UnloadTexture(miniTex);
#ifdef __APPLE__
    if (sleepAssertion) IOPMAssertionRelease(sleepAssertion); // let the Mac sleep again
#endif
    applyFpsTarget(m_config.fpsTarget); // restore the user's frame pacing
    unloadGameData(); // final model save + teardown
    // The throwaway training map shouldn't linger in the custom-maps menu
    remove((m_dataDir + TextFormat("custom_maps/__ai_training_%d__.odmap", (int)getpid())).c_str());
}
