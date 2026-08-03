#include "Game.h"
#include "GameInternals.h"
#include "MapEditor.h"
#include "ai/AISystem.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <unordered_map>
// getpid, to name the throwaway training map per process so two trainers do not
// delete each other's. POSIX spells it unistd.h/getpid; MSVC spells it
// process.h/_getpid and has no unistd.h at all.
#ifdef _WIN32
  #include <process.h>
  #define getpid _getpid
#else
  #include <unistd.h>
#endif
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

void Game::setAIWorker(int id, int count) {
    if (count <= 1 || id < 0 || id >= count) return;
    m_aiWorkerId = id;
    m_aiWorkerCount = count;
    m_aiModelPath = TextFormat("ai/model.w%d.bin", id);
    // Seed a fresh worker from the shared model if it has no file of its own,
    // so a pool starts from the training already done rather than from noise.
    // Copied rather than symlinked: the whole point is that workers diverge.
    const std::string own = m_dataDir + m_aiModelPath;
    const std::string shared = m_dataDir + "ai/model.bin";
    if (!FileExists(own.c_str()) && FileExists(shared.c_str())) {
        int len = 0;
        if (unsigned char* data = LoadFileData(shared.c_str(), &len)) {
            SaveFileData(own.c_str(), data, len);
            UnloadFileData(data);
            printf("[TRAIN] worker %d seeded from ai/model.bin\n", id);
        }
    }
    printf("[TRAIN] worker %d of %d — model %s\n", id, count, m_aiModelPath.c_str());
}

void Game::runAITraining(int numMaps, int turnsPerMap, int numCountries, unsigned int baseSeed) {
    // A training run is something you watch, and usually something you watch
    // through a pipe or a redirect -- which makes stdout fully buffered, so
    // hours of [TRAIN] and [AI] output sit in a 4 KB buffer instead of
    // appearing. Line buffering costs nothing here and is the difference
    // between a log you can tail and one that looks like a hung process.
    setvbuf(stdout, nullptr, _IOLBF, 0);
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
    // Pool synchronisation cadence. Long enough that a worker has learned
    // something worth sharing, short enough that the copies cannot drift far
    // apart — averaging is only a good approximation of a summed gradient
    // while the things being averaged are still near each other.
    const double PEER_SYNC_SECONDS = 120.0;
    double lastPeerSync = GetTime();

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
        // Generating and loading a world is the one burst of full-tilt CPU the
        // per-turn throttle never sees — it happens outside the turn loop. On a
        // long run it is a rounding error, but a rotation every few thousand
        // turns pegging every core is exactly what a player who set the limiter
        // asked not to happen, so the phase pays its own idle time below.
        auto genStart = std::chrono::steady_clock::now();
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
        throttleForBudget(std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - genStart).count(),
                          /*maxSleepSeconds=*/300.0);
        m_currentSavePath.clear(); // never write an .odsv during training
        m_playerCountryId = 0;     // spectator: every country is AI-driven
        m_aiTraining = true;

        // Headless self-play exists to learn, so it always learns.
        //
        // config.aiLearning defaults to false and gates AISystem::endTurn(),
        // because in-game learning is an opt-in experiment that must not mutate
        // data/ai/model.bin behind a player's back. That switch is about normal
        // play; applying it here silently turned --train-ai into an expensive
        // no-op -- it ran thousands of turns, took no gradient steps, and left
        // the dashboard's reward graphs empty because nothing fed them.
        m_config.aiLearning = true;

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
                // Settle the winner's outstanding decisions as a WIN before the
                // map rotates and the AISystem is destroyed with them still
                // open. Conquering the world was previously worth nothing to
                // the model.
                if (m_ai && !realCount.empty()) m_ai->noteVictory(realCount.begin()->first);
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

            // Checkpointing lives in AISystem::endTurn, on a wall clock. This
            // was a SECOND, independent turn-based schedule on top of it, so
            // the model was written nine times per hundred turns instead of
            // five — and the comment ("the model is tiny") stopped being true
            // when it grew to 12 MB. The per-map save below still runs, and the
            // destructor saves on exit, so nothing is lost by dropping it.

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
            // Input is checked on EVERY turn, not only on drawn frames.
            // IsKeyPressed reports "pressed since the last PollInputEvents", so
            // a toggle handled only on drawn frames silently swallows any press
            // that lands in one of the turns between them — which at 0.25 s a
            // turn is most of them.
            samplePerformance();
            updateResourcePanel();
            // Progress logging lives OUTSIDE the draw branch. It used to sit
            // after EndDrawing, so a turn that fell between two drawn frames
            // logged nothing -- and at 0.05 s a turn most of them do. Three
            // maps of a five-map validation reported neither progress nor
            // outcome purely because turn 3000 happened not to be drawn.
            if ((t + 1) % 100 == 0) {
                const auto& ts2 = m_ai ? m_ai->trainStats() : AISystem::TrainStats{};
                printf("[TRAIN] map %d turn %d/%d (%.4f s/turn, %d alive) "
                       "embarks=%lld landings=%lld home=%lld scrapped=%lld | "
                       "calls=%lld answered=%lld refused=%lld staging=%lld\n",
                       m + 1, t + 1, turnsPerMap, mapSecs / (t + 1), alive,
                       ts2.embarks, ts2.landings, ts2.unloadsHome, ts2.shipsScrapped,
                       ts2.callsIssued, ts2.callsAnswered, ts2.callsRefused,
                       ts2.stagingMoves);
            }
            // ── Pool sync ──
            // Save our own file, then pull part of the way toward the mean of
            // the peers. Doing it on a wall clock rather than a turn count
            // keeps every worker on roughly the same rhythm even though they
            // are on different maps at different speeds, and it is the same
            // reasoning the checkpoint interval uses.
            //
            // A THIRD of the way, not all of it: pulling fully to the mean each
            // time would erase whatever a worker had just learned between
            // syncs, which is the only thing it contributes.
            if (m_aiWorkerCount > 1 && m_ai) {
                const double nowSec = GetTime();
                if (nowSec - lastPeerSync >= PEER_SYNC_SECONDS) {
                    lastPeerSync = nowSec;
                    m_ai->saveModel();
                    std::vector<std::string> peers;
                    for (int w = 0; w < m_aiWorkerCount; ++w) {
                        if (w == m_aiWorkerId) continue;
                        peers.push_back(m_dataDir + TextFormat("ai/model.w%d.bin", w));
                    }
                    const int merged = m_ai->syncWithPeers(peers, 0.33f);
                    printf("[TRAIN] worker %d synced with %d/%zu peer(s)\n",
                           m_aiWorkerId, merged, peers.size());
                }
            }

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
                // Coalition readout: whether alliances have started to mean
                // anything, and whether anyone is using one to reach a front.
                DrawText(TextFormat("calls to arms %lld  answered %lld  refused %lld   staged into allied land %lld   ships scrapped %lld",
                                    ts.callsIssued, ts.callsAnswered, ts.callsRefused,
                                    ts.stagingMoves, ts.shipsScrapped),
                         40, 122, 18, {170, 200, 255, 230});
                // Domestic government: what kind of states self-play produces.
                DrawText(TextFormat("calming policies %lld   minorities conciliated %lld  repressed %lld   "
                                    "bankrupt turns %lld   austerity cuts %lld",
                                    ts.calmingPolicies, ts.minorityConciliations,
                                    ts.minorityRepressions, ts.bankruptTurns, ts.austerityCuts),
                         40, 142, 18, {230, 200, 170, 230});
                // Reward trends per module — the actual "is it learning" view:
                // rising lines mean the average country is doing better.
                static const char* RLBL[] = {"economy reward", "politics reward", "war reward", "navy reward"};
                static const Color RCOL[] = {{120, 220, 120, 255}, {120, 170, 255, 255},
                                             {255, 140, 120, 255}, {180, 160, 255, 255}};
                for (int mm = 0; mm < 4; ++mm)
                    drawSparkline(m_ai->rewardHistory()[mm],
                                  {40.0f + (mm % 2) * 215.0f, 170.0f + (mm / 2) * 62.0f, 205.0f, 56.0f},
                                  RCOL[mm], RLBL[mm]);
                int y = 306;
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
                DrawText("policy 96-512-320-A x4   value 96-160-1 x4   diplo 96-256-160-2",
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
            DrawText("Close the window to stop — progress is saved.   F10 / Ctrl+L: resource limit", 40,
                     m_screenH - 40, 16, GRAY);
            drawResourcePanel();
            EndDrawing();

        }
        // Settle whatever is still open before the world (and the AISystem with
        // it) is torn down. A map that was WON already flushed through
        // noteVictory and cleared m_pending, so this is a no-op there; every
        // other exit from the loop — turn cap, stagnation, the window closing —
        // used to drop the last N_STEP turns of every country's decisions
        // unscored, and with them the only statement the run could make about
        // who finished the map ahead.
        if (m_ai) m_ai->noteMapEnd();
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

// ─── Measuring the model ─────────────────────────────────
// `OpenDoctrines --eval-ai [maps] [turnsPerMap] [seed] [difficulty]`
//
// Training tells you the reward went up. It cannot tell you the AI got better,
// because the reward function is one of the things that keeps changing: every
// time a term is added or reweighted the sparklines are measuring a different
// quantity, and comparing yesterday's curve to today's is comparing two
// different questions. This plays the model instead and counts what it did.
//
// Three properties make the numbers comparable across model versions:
//
//   Fixed seeds.  The default seed is a constant, not the clock, so map N is
//                 the same world in every run. The turn resolver and the AI's
//                 own RNG are both deterministic, so the whole run is.
//   No learning.  The model is loaded read-only and never updated. What is
//                 measured is the file on disk, not a moving target — and a
//                 training session can keep running in another process without
//                 the two fighting over data/ai/model.bin.
//   No training-mode sampling.  Self-play deliberately injects exploration
//                 noise. A measurement that inherits it is measuring dice.
//                 Sampling here comes from the difficulty setting, the same way
//                 a real game samples it.
//
// Counters are reported per thousand country-turns, because every one of them
// scales with how many countries are alive; a raw total says more about the
// scenario's country count than about the model. The last line is machine
// readable on purpose: two runs of this are meant to be diffed.
void Game::runAIEvaluation(int numMaps, int turnsPerMap, unsigned int baseSeed,
                           int difficulty, bool vsRandom) {
    setvbuf(stdout, nullptr, _IOLBF, 0);

    // Read-only before the first AISystem is constructed (that happens lazily
    // inside the first processTurn), so no path through this can write the
    // model — including the destructor's save on teardown.
    AISystem::s_readOnlyModel = true;
    AISystem::s_evaluating = true;
    const bool savedLearning = m_config.aiLearning;
    const int savedDifficulty = m_config.aiDifficulty;
    m_config.aiLearning = false;
    m_config.aiDifficulty = std::clamp(difficulty, 0, 3);
    static const char* DIFF_NAMES[] = {"easy", "normal", "hard", "insane"};

    printf("[EVAL] %d map(s) x %d turn(s), seed %u, difficulty %s\n",
           numMaps, turnsPerMap, baseSeed, DIFF_NAMES[m_config.aiDifficulty]);
    printf("[EVAL] Model: %sai/model.bin (read-only — training may keep running)\n",
           m_dataDir.c_str());

    applyFpsTarget(-1);
    auto runStart = std::chrono::steady_clock::now();
    std::mt19937 rng(baseSeed);

    // One row per map, aggregated at the end.
    struct MapResult {
        const char* scenario = "";
        int seed = 0, startCountries = 0, turns = 0;
        int aliveEnd = 0;
        double largestShare = 0;    // biggest country's share of owned land
        double concentration = 0;   // sum of squared shares: 1.0 = one owner
        const char* outcome = "cap";
        long long countryTurns = 0; // sum of alive counts over the map
        long long rebellions = 0;
        // Mean minority alignment across every real country at the end, and how
        // many of them ended up below the 40% mark where minority unrest starts
        // feeding rebellion chance. A model that governs well should hold this
        // up; one that only knows how to repress will drive it down and pay for
        // it in revolts a hundred turns later.
        double meanAlignEnd = 50.0;
        double disaffectedShare = 0.0;
        // Control-group comparison (--vs-random). Counts at the start, land and
        // survivors at the end.
        int trainedCount = 0, randomCount = 0;
        int trainedProvinces = 0, randomProvinces = 0;
        // Provinces each cohort STARTED with, so the six flow counters can be
        // reconciled against the outcome instead of merely described.
        int trainedStartProv = 0, randomStartProv = 0;
        int trainedAlive = 0, randomAlive = 0;
        // Country-turns per cohort. Every behavioural counter has to be
        // normalised by the cohort that produced it, not by the map total:
        // once one side starts losing countries the two denominators diverge,
        // and dividing both by the same number would report the shrinking side
        // as busier per country than it is.
        long long trainedCountryTurns = 0, randomCountryTurns = 0;
        AISystem::TrainStats stats, rstats;
    };
    std::vector<MapResult> results;
    bool aborted = false;

    for (int m = 0; m < numMaps && !aborted; ++m) {
        const Scenario& sc = SCENARIOS[m % SCENARIO_COUNT];
        MapEditor::GeneratorParams p;
        p.seed = (int)(rng() & 0x7FFFFFFF);
        p.landCoverage = randRange(rng, sc.landMin, sc.landMax);
        p.numContinents = randRange(rng, sc.contMin, sc.contMax);
        p.jaggedness = randRange(rng, sc.jagMin, sc.jagMax);
        p.provinceDensity = randRange(rng, sc.densMin, sc.densMax);
        p.numCountries = randRange(rng, sc.countriesMin, sc.countriesMax);

        unloadGameData();
        // A distinct name from the trainer's, so an eval run and a training run
        // side by side cannot delete each other's scratch map even if they
        // somehow shared a pid.
        std::string mapName = TextFormat("__ai_eval_%d__", (int)getpid());
        std::string odmPath;
        {
            MapEditor ed;
            ed.init(m_screenW, m_screenH, m_dataDir);
            odmPath = ed.generateAndExportHeadless(p, mapName.c_str());
        }
        if (odmPath.empty()) {
            printf("[EVAL] map %d: generation failed, skipping\n", m + 1);
            continue;
        }
        if (WindowShouldClose()) break;

        startLoading(odmPath);
        while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE)
            updateLoading();
        if (m_loadingFailed) {
            printf("[EVAL] map %d: load failed, skipping\n", m + 1);
            continue;
        }
        hideLoadingScreen();
        m_currentSavePath.clear();  // never write an .odsv
        m_playerCountryId = 0;      // spectator: every country is AI-driven
        // Reuses the training loop's headless shortcuts (no political texture,
        // no label raster, no pixel bookkeeping). s_evaluating is what keeps
        // this from also inheriting the training exploration schedule.
        m_aiTraining = true;

        MapResult r;
        r.scenario = sc.name;
        r.seed = p.seed;
        r.startCountries = p.numCountries;

        // ── Matched cohorts ──
        //
        // Alternating country ids would work only if id correlated with nothing;
        // it correlates with generation order, which correlates with position.
        // Ranking by starting size and alternating down the list gives both
        // cohorts the same spread of strong and weak starts, so a difference at
        // the end is a difference in play rather than in dealt hands. The seed
        // is fixed, so the split is identical in every run of this map.
        std::unordered_set<int> randomCids;
        if (vsRandom) {
            std::unordered_map<int, int> startSize;
            for (int owner : m_provinceCountryLookup)
                if (owner > 0 && owner < REBEL_CID_MIN) startSize[owner]++;
            std::vector<std::pair<int, int>> ranked; // (provinces, cid)
            ranked.reserve(startSize.size());
            for (auto& [cid2, n] : startSize) ranked.push_back({n, cid2});
            // Descending by size, cid as the tie-break so the order is total
            // and the split is reproducible.
            std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) {
                if (a.first != b.first) return a.first > b.first;
                return a.second < b.second;
            });
            for (size_t k = 1; k < ranked.size(); k += 2) randomCids.insert(ranked[k].second);
            r.trainedCount = (int)(ranked.size() - randomCids.size());
            r.randomCount = (int)randomCids.size();
            for (auto& [cid2, n] : startSize) {
                if (randomCids.count(cid2)) r.randomStartProv += n;
                else                        r.trainedStartProv += n;
            }
        }
        // processTurn builds the AISystem lazily on its first call, which is one
        // turn too late to tell it who is in the control group. Build it here
        // instead; the lazy path then finds it already present.
        if (!m_ai) m_ai = new AISystem(this, m_dataDir + m_aiModelPath);
        m_ai->setRandomCountries(randomCids);

        printf("[EVAL] map %d/%d [%s] seed=%d countries=%d%s\n",
               m + 1, numMaps, sc.name, p.seed, p.numCountries,
               vsRandom ? TextFormat("  (%d model vs %d random)", r.trainedCount, r.randomCount) : "");

        const int STAGNATION_TURNS = 1500;
        int bestTerritory = 0, fewestAlive = 1 << 30, turnsSinceProgress = 0;
        auto mapStart = std::chrono::steady_clock::now();

        for (int t = 0; t < turnsPerMap; ++t) {
            if (WindowShouldClose()) { aborted = true; break; }
            processTurn();
            r.turns = t + 1;

            for (auto& [cid, n] : m_rebellionsThisTurnByCid)
                if (cid > 0) r.rebellions += n;

            std::unordered_map<int, int> realCount;
            int maxReal = 0, ownedProvs = 0;
            for (int owner : m_provinceCountryLookup)
                if (owner > 0 && owner < REBEL_CID_MIN) {
                    int n = ++realCount[owner];
                    if (n > maxReal) maxReal = n;
                    ownedProvs++;
                }
            const int alive = (int)realCount.size();
            r.aliveEnd = alive;
            r.countryTurns += alive;
            if (vsRandom)
                for (auto& [cid2, n2] : realCount) {
                    if (randomCids.count(cid2)) r.randomCountryTurns++;
                    else                        r.trainedCountryTurns++;
                }
            if (ownedProvs > 0) {
                r.largestShare = (double)maxReal / ownedProvs;
                double h = 0;
                for (auto& [cid, n] : realCount) {
                    const double s = (double)n / ownedProvs;
                    h += s * s;
                }
                r.concentration = h;
            }

            if (alive <= 1) { r.outcome = "decided"; break; }
            if (maxReal > bestTerritory || alive < fewestAlive) {
                bestTerritory = std::max(bestTerritory, maxReal);
                fewestAlive = std::min(fewestAlive, alive);
                turnsSinceProgress = 0;
            } else if (++turnsSinceProgress >= STAGNATION_TURNS) {
                r.outcome = "frozen";
                break;
            }

            PollInputEvents();  // or the compositor decides the process hung
            if ((t + 1) % 250 == 0) {
                double secs = std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - mapStart).count();
                printf("[EVAL]   turn %d/%d  %d alive  largest %.1f%%  (%.3f s/turn)\n",
                       t + 1, turnsPerMap, alive, r.largestShare * 100, secs / (t + 1));
            }
        }

        if (m_ai) { r.stats = m_ai->trainStats(); r.rstats = m_ai->randomStats(); }

        // Where the two cohorts finished.
        if (vsRandom) {
            std::unordered_map<int, int> held;
            for (int owner : m_provinceCountryLookup)
                if (owner > 0 && owner < REBEL_CID_MIN) held[owner]++;
            for (auto& [cid2, n] : held) {
                if (randomCids.count(cid2)) { r.randomProvinces += n; r.randomAlive++; }
                else                        { r.trainedProvinces += n; r.trainedAlive++; }
            }
        }

        // Minority standing at the end of the map, measured once rather than
        // sampled per turn: it is a slow-moving quantity and the walk is over
        // every province that has minorities.
        {
            std::unordered_map<int, std::unordered_set<std::string>> byCountry;
            for (auto& [pid, groups] : m_provinceMinorities) {
                if (pid < 0 || pid >= (int)m_provinceCountryLookup.size()) continue;
                const int owner = m_provinceCountryLookup[pid];
                if (owner <= 0 || owner >= REBEL_CID_MIN) continue;
                for (auto& mg : groups) byCountry[owner].insert(mg.name);
            }
            double sum = 0; long long n = 0, disaffected = 0;
            for (auto& [cid, names] : byCountry)
                for (const std::string& name : names) {
                    const float a = getMinorityAlignment(cid, name);
                    sum += a; ++n;
                    if (a < 40.0f) ++disaffected;
                }
            if (n > 0) {
                r.meanAlignEnd = sum / n;
                r.disaffectedShare = (double)disaffected / n;
            }
        }
        m_aiTraining = false;

        const double kct = r.countryTurns > 0 ? r.countryTurns / 1000.0 : 1.0;
        printf("[EVAL]   %s after %d turns | alive %d/%d | largest %.1f%% | concentration %.3f\n",
               r.outcome, r.turns, r.aliveEnd, r.startCountries,
               r.largestShare * 100, r.concentration);
        printf("[EVAL]   per 1k country-turns: wars %.2f  ceasefires %.2f  pacts %.2f  "
               "rebellions %.2f  research %.2f\n",
               r.stats.warsDeclared / kct, r.stats.ceasefiresOffered / kct,
               r.stats.pactsProposed / kct, r.rebellions / kct,
               r.stats.researchCompleted / kct);
        printf("[EVAL]   calls %lld answered %lld refused %lld (%.0f%% refused)  staged %lld\n",
               r.stats.callsIssued, r.stats.callsAnswered, r.stats.callsRefused,
               r.stats.callsIssued ? 100.0 * r.stats.callsRefused / r.stats.callsIssued : 0.0,
               r.stats.stagingMoves);
        printf("[EVAL]   embarks %lld landings %lld (%.0f%% reached a hostile shore)  "
               "home %lld  scrapped %lld\n",
               r.stats.embarks, r.stats.landings,
               r.stats.embarks ? 100.0 * r.stats.landings / r.stats.embarks : 0.0,
               r.stats.unloadsHome, r.stats.shipsScrapped);
        printf("[EVAL]   minorities: mean alignment %.0f%%, %.0f%% disaffected | "
               "conciliated %lld  repressed %lld  calming policies %lld\n",
               r.meanAlignEnd, r.disaffectedShare * 100,
               r.stats.minorityConciliations, r.stats.minorityRepressions,
               r.stats.calmingPolicies);
        printf("[EVAL]   solvency: %.1f%% of country-turns bankrupt, %.2f austerity cuts per 1k\n",
               r.countryTurns ? 100.0 * r.stats.bankruptTurns / r.countryTurns : 0.0,
               r.stats.austerityCuts / kct);
        if (vsRandom) {
            // Behaviour, side by side. The ratio says the model is losing; only
            // this says what it is doing differently while it loses.
            const double mk = r.trainedCountryTurns ? r.trainedCountryTurns / 1000.0 : 1.0;
            const double rk = r.randomCountryTurns  ? r.randomCountryTurns  / 1000.0 : 1.0;
            const AISystem::TrainStats& M = r.stats;
            const AISystem::TrainStats& R = r.rstats;
            printf("[EVAL]   per 1k country-turns      MODEL     RANDOM\n");
            printf("[EVAL]     wars declared          %7.2f   %7.2f\n", M.warsDeclared/mk,      R.warsDeclared/rk);
            printf("[EVAL]     ceasefires offered     %7.2f   %7.2f\n", M.ceasefiresOffered/mk, R.ceasefiresOffered/rk);
            printf("[EVAL]     pacts proposed         %7.2f   %7.2f\n", M.pactsProposed/mk,     R.pactsProposed/rk);
            printf("[EVAL]     troops embarked        %7.2f   %7.2f\n", M.embarks/mk,           R.embarks/rk);
            printf("[EVAL]     landings               %7.2f   %7.2f\n", M.landings/mk,          R.landings/rk);
            printf("[EVAL]     staging moves          %7.2f   %7.2f\n", M.stagingMoves/mk,      R.stagingMoves/rk);
            printf("[EVAL]     ships scrapped         %7.2f   %7.2f\n", M.shipsScrapped/mk,     R.shipsScrapped/rk);
            printf("[EVAL]     austerity cuts         %7.2f   %7.2f\n", M.austerityCuts/mk,     R.austerityCuts/rk);
            printf("[EVAL]     turns bankrupt         %7.2f   %7.2f\n", M.bankruptTurns/mk,     R.bankruptTurns/rk);
            printf("[EVAL]     calming policies       %7.2f   %7.2f\n", M.calmingPolicies/mk,   R.calmingPolicies/rk);
            printf("[EVAL]     minorities conciliated %7.2f   %7.2f\n", M.minorityConciliations/mk, R.minorityConciliations/rk);
            printf("[EVAL]     minorities repressed   %7.2f   %7.2f\n", M.minorityRepressions/mk,   R.minorityRepressions/rk);
            printf("[EVAL]     research completed     %7.2f   %7.2f\n", M.researchCompleted/mk, R.researchCompleted/rk);
            printf("[EVAL]     calls answered/issued  %4lld/%-4lld  %4lld/%-4lld\n",
                   M.callsAnswered, M.callsIssued, R.callsAnswered, R.callsIssued);
            // WHERE THE LAND CAME FROM. "land held" cannot tell a cohort that
            // conquers its neighbours from one that absorbs whatever other
            // people's rebellions shed, and those need different answers.
            printf("[EVAL]   -- province flow (absolute counts) --\n");
            printf("[EVAL]     taken from a country   %7lld   %7lld\n",
                   M.provTakenFromCountry, R.provTakenFromCountry);
            printf("[EVAL]       ...won in battle      %7lld   %7lld\n",
                   M.provTakenInBattle,    R.provTakenInBattle);
            printf("[EVAL]       ...walked into        %7lld   %7lld\n",
                   M.provWalkedInto,       R.provWalkedInto);
            printf("[EVAL]     taken from a rebel     %7lld   %7lld\n",
                   M.provTakenFromRebel,   R.provTakenFromRebel);
            printf("[EVAL]     lost to a country      %7lld   %7lld\n",
                   M.provLostToCountry,    R.provLostToCountry);
            printf("[EVAL]     lost to a revolt       %7lld   %7lld\n",
                   M.provLostToRebel,      R.provLostToRebel);
            printf("[EVAL]     gained by treaty       %7lld   %7lld\n",
                   M.provByTreaty,         R.provByTreaty);
            printf("[EVAL]     ceded by treaty        %7lld   %7lld\n",
                   M.provCededByTreaty,    R.provCededByTreaty);
            // THE RECONCILIATION. start + everything gained - everything lost
            // should be what the cohort ends with. Whatever is left over moved
            // by a route none of the counters above watch, and naming that
            // number is the difference between a measurement and a story.
            auto ledger = [](const char* who, int startP, int endP,
                             const AISystem::TrainStats& S) {
                const long long gained = S.provTakenFromCountry + S.provTakenFromRebel
                                       + S.provByTreaty;
                const long long lost   = S.provLostToCountry + S.provLostToRebel
                                       + S.provCededByTreaty;
                const long long predicted = (long long)startP + gained - lost;
                printf("[EVAL]     %-6s start %4d  +%lld -%lld  => %lld predicted, "
                       "%d actual, UNEXPLAINED %+lld\n",
                       who, startP, gained, lost, predicted, endP,
                       (long long)endP - predicted);
            };
            ledger("MODEL",  r.trainedStartProv, r.trainedProvinces, M);
            ledger("RANDOM", r.randomStartProv,  r.randomProvinces,  R);

            const int total = r.trainedProvinces + r.randomProvinces;
            printf("[EVAL]   MODEL %d provinces (%.0f%%), %d/%d alive | "
                   "RANDOM %d provinces (%.0f%%), %d/%d alive  -> %s\n",
                   r.trainedProvinces, total ? 100.0 * r.trainedProvinces / total : 0.0,
                   r.trainedAlive, r.trainedCount,
                   r.randomProvinces, total ? 100.0 * r.randomProvinces / total : 0.0,
                   r.randomAlive, r.randomCount,
                   r.trainedProvinces > r.randomProvinces ? "MODEL WINS"
                     : (r.trainedProvinces < r.randomProvinces ? "RANDOM WINS" : "draw"));
        }
        results.push_back(r);
    }

    // ── Aggregate ──
    long long totalTurns = 0, totalCountryTurns = 0, decided = 0, frozen = 0;
    long long wars = 0, ceases = 0, pacts = 0, rebels = 0, research = 0;
    long long calls = 0, answered = 0, refused = 0, staged = 0;
    long long embarks = 0, landings = 0, home = 0, scrapped = 0;
    long long conciliated = 0, repressed = 0, calming = 0;
    long long bankruptTurns = 0, austerityCuts = 0;
    long long trainedProv = 0, randomProv = 0, trainedAlive = 0, randomAlive = 0;
    long long trainedStart = 0, randomStart = 0, modelWins = 0, randomWins = 0;
    double aliveFrac = 0, largest = 0, conc = 0, meanAlign = 0, disaffected = 0;
    for (const MapResult& r : results) {
        totalTurns += r.turns;
        totalCountryTurns += r.countryTurns;
        if (std::string(r.outcome) == "decided") decided++;
        if (std::string(r.outcome) == "frozen") frozen++;
        wars += r.stats.warsDeclared;      ceases += r.stats.ceasefiresOffered;
        pacts += r.stats.pactsProposed;    rebels += r.rebellions;
        research += r.stats.researchCompleted;
        calls += r.stats.callsIssued;      answered += r.stats.callsAnswered;
        refused += r.stats.callsRefused;   staged += r.stats.stagingMoves;
        embarks += r.stats.embarks;        landings += r.stats.landings;
        home += r.stats.unloadsHome;       scrapped += r.stats.shipsScrapped;
        bankruptTurns += r.stats.bankruptTurns;
        austerityCuts += r.stats.austerityCuts;
        conciliated += r.stats.minorityConciliations;
        repressed += r.stats.minorityRepressions;
        calming += r.stats.calmingPolicies;
        aliveFrac += r.startCountries > 0 ? (double)r.aliveEnd / r.startCountries : 0.0;
        largest += r.largestShare;
        conc += r.concentration;
        meanAlign += r.meanAlignEnd;
        disaffected += r.disaffectedShare;
        trainedProv += r.trainedProvinces;   randomProv += r.randomProvinces;
        trainedAlive += r.trainedAlive;      randomAlive += r.randomAlive;
        trainedStart += r.trainedCount;      randomStart += r.randomCount;
        if (r.trainedProvinces > r.randomProvinces) modelWins++;
        else if (r.trainedProvinces < r.randomProvinces) randomWins++;
    }
    const double n = results.empty() ? 1.0 : (double)results.size();
    const double kct = totalCountryTurns > 0 ? totalCountryTurns / 1000.0 : 1.0;
    const double mins = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - runStart).count() / 60.0;

    printf("\n[EVAL] ===== %zu map(s), %lld turns, %.1f min, difficulty %s%s =====\n",
           results.size(), totalTurns, mins, DIFF_NAMES[m_config.aiDifficulty],
           aborted ? " (ABORTED — partial)" : "");
    printf("[EVAL] outcome        %lld decided, %lld frozen, %zu hit the turn cap\n",
           decided, frozen, results.size() - (size_t)decided - (size_t)frozen);
    printf("[EVAL] survival       %.1f%% of countries still alive at the end\n", 100.0 * aliveFrac / n);
    printf("[EVAL] largest power  %.1f%% of the owned world\n", 100.0 * largest / n);
    printf("[EVAL] concentration  %.3f  (1.000 = one country owns everything)\n", conc / n);
    printf("[EVAL] war            %.2f declarations, %.2f ceasefire offers per 1k country-turns\n",
           wars / kct, ceases / kct);
    printf("[EVAL] diplomacy      %.2f pacts proposed per 1k country-turns\n", pacts / kct);
    printf("[EVAL] coalition      %.0f%% of %lld calls to arms answered, %lld staging moves\n",
           calls ? 100.0 * answered / calls : 0.0, calls, staged);
    printf("[EVAL] amphibious     %.0f%% of %lld embarkations reached a hostile shore (%lld came home)\n",
           embarks ? 100.0 * landings / embarks : 0.0, embarks, home);
    printf("[EVAL] fleet          %.2f hulls scrapped per 1k country-turns\n", scrapped / kct);
    printf("[EVAL] unrest         %.2f rebellions, %.2f research nodes per 1k country-turns\n",
           rebels / kct, research / kct);
    printf("[EVAL] minorities     mean alignment %.0f%%, %.0f%% of groups disaffected (<40%%)\n",
           meanAlign / n, 100.0 * disaffected / n);
    printf("[EVAL] governing      %.2f conciliations, %.2f repressions, %.2f calming policies "
           "per 1k country-turns\n", conciliated / kct, repressed / kct, calming / kct);
    // The headline solvency number. Bankruptcy costs twenty points of rebellion
    // chance in every province, so a high figure here explains a high one two
    // lines up.
    printf("[EVAL] solvency       %.1f%% of country-turns spent bankrupt, "
           "%.2f austerity cuts per 1k\n",
           totalCountryTurns ? 100.0 * bankruptTurns / totalCountryTurns : 0.0,
           austerityCuts / kct);
    // One line, stable field order, for diffing two model versions.
    if (vsRandom) {
        const long long tot = trainedProv + randomProv;
        const double share = tot ? 100.0 * trainedProv / tot : 0.0;
        // Survival rates rather than raw counts: the cohorts are the same size
        // by construction, but a map that ends early leaves both incomplete.
        const double tSurv = trainedStart ? 100.0 * trainedAlive / trainedStart : 0.0;
        const double rSurv = randomStart ? 100.0 * randomAlive / randomStart : 0.0;
        printf("\n[EVAL] ----- MODEL vs RANDOM -----\n");
        printf("[EVAL] maps won       %lld model, %lld random, %zu drawn\n",
               modelWins, randomWins, results.size() - (size_t)modelWins - (size_t)randomWins);
        printf("[EVAL] land held      %.1f%% model / %.1f%% random  (50%% = no better than a coin flip)\n",
               share, 100.0 - share);
        printf("[EVAL] survival       %.0f%% of model countries, %.0f%% of random countries\n",
               tSurv, rSurv);
        // One number to watch across model versions. Below 1.0 the trained
        // policy is losing to random selection, which no reward curve will tell
        // you and which has exactly one honest interpretation.
        printf("[EVAL] ADVANTAGE      %.2fx the land a coin flip holds\n",
               randomProv ? (double)trainedProv / randomProv
                          : (trainedProv ? 99.0 : 1.0));
    }

    printf("[EVAL] CSV,maps,turns,decided,frozen,alive_pct,largest_pct,herfindahl,"
           "wars_k,ceasefires_k,pacts_k,calls_answered_pct,landing_pct,scrap_k,rebellions_k,"
           "align_pct,disaffected_pct,conciliate_k,repress_k,calming_k,"
           "vs_random,model_land_pct,model_wins,random_wins,bankrupt_pct\n");
    printf("[EVAL] CSV,%zu,%lld,%lld,%lld,%.2f,%.2f,%.4f,%.3f,%.3f,%.3f,%.2f,%.2f,%.3f,%.3f,"
           "%.2f,%.2f,%.3f,%.3f,%.3f\n",
           results.size(), totalTurns, decided, frozen, 100.0 * aliveFrac / n,
           100.0 * largest / n, conc / n, wars / kct, ceases / kct, pacts / kct,
           calls ? 100.0 * answered / calls : 0.0,
           embarks ? 100.0 * landings / embarks : 0.0, scrapped / kct, rebels / kct,
           meanAlign / n, 100.0 * disaffected / n,
           conciliated / kct, repressed / kct, calming / kct);
    printf("[EVAL] CSV_EXTRA,%d,%.2f,%lld,%lld,%.2f\n", vsRandom ? 1 : 0,
           (trainedProv + randomProv) ? 100.0 * trainedProv / (trainedProv + randomProv) : 0.0,
           modelWins, randomWins,
           totalCountryTurns ? 100.0 * bankruptTurns / totalCountryTurns : 0.0);

    m_config.aiLearning = savedLearning;
    m_config.aiDifficulty = savedDifficulty;
    applyFpsTarget(m_config.fpsTarget);
    unloadGameData();
    remove((m_dataDir + TextFormat("custom_maps/__ai_eval_%d__.odmap", (int)getpid())).c_str());
}

// ─── Unattended self-play on a shipped scenario ──────────
// `OpenDoctrines --simulate <map.odmap> <turns> [world name]`
//
// Deliberately NOT a variant of training. Training generates its own maps and
// clears m_currentSavePath so it never writes an .odsv; this loads a scenario
// that actually ships and keeps the save, because the save IS the output. What
// it produces is a world with a real turn history, which is the input
// --export-timelapse has always needed and never had a way to make.
//
// It is also the smallest honest end-to-end check of a build: load a map,
// resolve turns, write an archive. A platform where that works is a platform
// where the game runs, and it needs nobody at the keyboard to say so.
bool Game::runHeadlessSimulation(const std::string& mapPath, int turns,
                                 const std::string& worldName) {
    printf("[SIM] %s — %d turns\n", mapPath.c_str(), turns);

    // Nothing is drawn between turns, so a frame cap would only add sleep.
    applyFpsTarget(-1);

    // The menu's own new-world path, so this exercises what a player exercises
    // rather than a second loader that could drift away from it.
    startNewGameWithName(mapPath, worldName);

    // startNewGameWithName hands off to the async loader, which normally runs
    // one step per frame from Game::run(). There is no run() here, so drive it.
    while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE) {
        if (WindowShouldClose()) { printf("[SIM] aborted while loading\n"); return false; }
        updateLoading();
    }
    if (m_loadingFailed) {
        fprintf(stderr, "[SIM] could not load %s\n", mapPath.c_str());
        return false;
    }
    hideLoadingScreen();
    m_currentScreen = SCREEN_PLAYING;
    m_playerCountryId = 0;   // spectator: every country is AI-driven

    if (m_currentSavePath.empty()) {
        fprintf(stderr, "[SIM] no save was created; there would be no history to keep\n");
        return false;
    }
    printf("[SIM] save: %s\n", m_currentSavePath.c_str());

    auto start = std::chrono::steady_clock::now();
    int played = 0;
    for (int t = 0; t < turns; ++t) {
        if (WindowShouldClose()) { printf("[SIM] stopped early at turn %d\n", played); break; }
        processTurn();
        played++;
        // The window is never drawn to, but it still has to be pumped or the
        // compositor decides the process has hung and WindowShouldClose never
        // fires -- the same reason the training loop polls on skipped frames.
        PollInputEvents();
        if (played % 10 == 0 || played == turns) {
            double secs = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - start).count();
            printf("[SIM] turn %d/%d  (%.1fs, %.2f s/turn)\n",
                   played, turns, secs, secs / played);
            fflush(stdout);
        }
    }

    // A timelapse needs two turns to have something to animate between, so a
    // run that produced fewer has not produced anything usable.
    bool ok = played >= 2;
    if (!ok) fprintf(stderr, "[SIM] only %d turn(s) resolved; need at least 2\n", played);

    applyFpsTarget(m_config.fpsTarget);
    unloadGameData();
    return ok;
}
