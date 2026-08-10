#include "Game.h"
#include "GameInternals.h"
#include "MapEditor.h"
#include "ai/AISystem.h"
#include <algorithm>
#include <chrono>
#include <array>
#include <cstdio>
#include <random>
#include <string>
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

// ── The worlds a player actually opens ──
//
// Everything above generates its map. These are the six in data/STDmaps that
// the new-game menu offers, and until now neither training nor measurement had
// ever seen one. A generated archetype has no historical alliance network, no
// real claims, no minority map anyone has heard of, and nothing remotely like
// the 185-country present-day world; it has an even spread of country sizes
// where a real scenario has five great powers and forty small states.
//
// Every one of those differences is something buildFeatures reads. So the
// policy met all of it for the first time in the PLAYER'S game -- the one run
// that cannot be re-rolled, on the map the whole project is nominally about.
//
// Filenames rather than the maps_index.json the menu reads: this list has to
// work headless on a machine with no data/ overlay, and an index that gains an
// entry should not silently change what a benchmark measures. Keep it in step
// with that file by hand; a scenario missing from here is only missing from
// training, and one missing from there is missing from the game.
struct ShippedMap { const char* file; const char* name; };
const ShippedMap SHIPPED_MAPS[] = {
    {"STDmaps/1914.odmap", "1914"},
    {"STDmaps/1918.odmap", "1918"},
    {"STDmaps/1939.odmap", "1939"},
    {"STDmaps/1945.odmap", "1945"},
    {"STDmaps/1962.odmap", "1962"},
    {"STDmaps/map.odmap",  "modern"},
};
const int SHIPPED_COUNT = sizeof(SHIPPED_MAPS) / sizeof(SHIPPED_MAPS[0]);

// How often a training round plays a shipped scenario instead of a generated
// one. Every third, not every round: there are six fixed worlds and a policy
// given nothing else would learn those six rather than learn to play. The
// generated archetypes are what supplies variety; these are what stop it being
// variety around the wrong centre.
const int SHIPPED_TRAIN_EVERY = 3;

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
        // ── 1. Pick a world ──
        // Every SHIPPED_TRAIN_EVERY-th round plays one of the maps the game
        // actually ships, cycling through them; the rest generate. Interleaved
        // rather than run in a block so the policy never spends a long stretch
        // seeing only one kind of world -- the arrangement that produces a
        // model good at whichever it saw last.
        const ShippedMap* ship = ((m + 1) % SHIPPED_TRAIN_EVERY == 0)
                                     ? &SHIPPED_MAPS[(m / SHIPPED_TRAIN_EVERY) % SHIPPED_COUNT]
                                     : nullptr;
        // Cycle archetypes so every shape gets equal training time, jitter
        // everything else so no two rounds are the same world.
        const Scenario& sc = SCENARIOS[m % SCENARIO_COUNT];
        MapEditor::GeneratorParams p;
        p.seed = (int)(rng() & 0x7FFFFFFF);
        // Same reasoning as the evaluation loop below: turn logic uses rand(),
        // and raylib seeds it from the clock. Training tolerates noise better
        // than measurement does, but a training run that cannot be replayed
        // cannot be debugged either.
        srand((unsigned int)p.seed);
        seedSimRng((unsigned int)p.seed);
        p.landCoverage = randRange(rng, sc.landMin, sc.landMax);
        p.numContinents = randRange(rng, sc.contMin, sc.contMax);
        p.jaggedness = randRange(rng, sc.jagMin, sc.jagMax);
        p.provinceDensity = randRange(rng, sc.densMin, sc.densMax);
        p.numCountries = numCountries > 0 ? numCountries
                                          : randRange(rng, sc.countriesMin, sc.countriesMax);

        if (ship)
            printf("[TRAIN] Map %d%s: SHIPPED %s (%s) rollseed=%d\n",
                   m + 1, infinite ? "" : TextFormat("/%d", numMaps),
                   ship->name, ship->file, p.seed);
        else
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

        std::string odmPath;
        if (ship) {
            // A shipped map is not built, only opened -- but the seeding above
            // still applies: the WORLD is fixed and the GAME played on it is
            // not, so two rounds on 1939 are two different games rather than
            // the same one replayed.
            odmPath = m_dataDir + ship->file;
            if (!FileExists(odmPath.c_str())) {
                printf("[TRAIN] %s not installed, skipping round\n", odmPath.c_str());
                continue;
            }
        } else {
            // Per-process temp map name: a second trainer (or a stray teardown
            // from a previous run) must not delete the map this one just built.
            std::string mapName = TextFormat("__ai_training_%d__", (int)getpid());
            MapEditor ed;
            ed.init(m_screenW, m_screenH, m_dataDir);
            odmPath = ed.generateAndExportHeadless(p, mapName.c_str());
            if (odmPath.empty()) {
                printf("[TRAIN] Map generation failed, skipping round\n");
                continue;
            }
        }
        if (WindowShouldClose()) break;

        // ── 2. Load it through the normal pipeline (no world save) ──
        // (World already torn down above, before generation.)
        startLoading(odmPath);
        while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE)
            updateLoading();
        if (m_loadingFailed) {
            printf("[TRAIN] Failed to load %s, skipping round\n", odmPath.c_str());
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
        // MEASURED DIRECTLY NOW, SO THE WINDOW CAN BE SHORT.
        //
        // The old signal was a high-water mark -- biggest real country grew, or
        // somebody died -- which stops moving long before a war does: two
        // powers can trade the same provinces for a thousand turns and neither
        // ever exceeds its own record. That forced a 1500-turn window to avoid
        // rotating mid-conflict, and the cost was enormous. Measured over the
        // 8-hour run of 2026-08-06: 92 maps froze, each burning its full 1500
        // turns first, which is ~138,000 turns of dead compute -- roughly a
        // third of the run spent grinding worlds that had already resolved.
        //
        // Counting province transfers between REAL countries instead (rebel
        // churn excluded -- that was the original bug) says exactly what we
        // mean. A live war produces conquests constantly, so an active map
        // never trips this, and 400 turns with not one province changing hands
        // anywhere on the map is genuinely finished.
        const int STAGNATION_TURNS = 400;
        long long lastConquestCount = 0;
        resetRealConquests();
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
            if (realConquests() != lastConquestCount) {
                lastConquestCount = realConquests();
                turnsSinceProgress = 0;
            } else if (++turnsSinceProgress >= STAGNATION_TURNS) {
                printf("[TRAIN] map %d strategically frozen after %d turns "
                       "(no province changed hands between real countries in "
                       "%d) — rotating\n", m + 1, t + 1, STAGNATION_TURNS);
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
        // DOES THE ADVANTAGE DISCRIMINATE? See TrainStats::warAdvSum. Printed
        // per map because the AISystem is destroyed on rotation and these
        // counters go with it.
        if (m_ai) {
            const AISystem::TrainStats& T = m_ai->trainStats();
            static const char* WN[AISystem::WAR_ACTIONS] = {
                "hold", "recruit", "reinforce", "attack",
                "declare war", "artillery", "ceasefire", "stage"};
            long long tot = 0;
            for (int i = 0; i < AISystem::WAR_ACTIONS; ++i) tot += T.warAdvN[i];
            if (tot > 0) {
                printf("[TRAIN] mean advantage credited per war action:\n");
                printf("[TRAIN]     %-11s %9s %9s %9s %9s %8s\n",
                       "action", "adv", "immediate", "bootstrap", "baseline", "n");
                for (int i = 0; i < AISystem::WAR_ACTIONS; ++i) {
                    const double k = T.warAdvN[i] ? (double)T.warAdvN[i] : 1.0;
                    printf("[TRAIN]     %-11s %+9.4f %+9.4f %+9.4f %+9.4f %8lld\n",
                           WN[i], T.warAdvSum[i] / k, T.warImmSum[i] / k,
                           T.warBootSum[i] / k, T.warBaseSum[i] / k, T.warAdvN[i]);
                }
            }
        }
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
bool Game::runAIEvaluation(int numMaps, int turnsPerMap, unsigned int baseSeed,
                           int difficulty, bool vsRandom,
                           const std::string& opponentModel, bool scenarios) {
    setvbuf(stdout, nullptr, _IOLBF, 0);

    // ── Who the control cohort is ──
    //
    // One split, two possible opponents. `split` is what every cohort-aware
    // branch below tests, and CONTROL is what they print, so a report can never
    // claim one opponent while the run used the other -- the failure mode that
    // makes a benchmark worse than having none.
    const bool vsOpponent = !opponentModel.empty();
    const bool vsScript = AISystem::s_scriptedControl;
    const bool split = vsRandom || vsOpponent || vsScript;
    const bool duel = AISystem::s_scriptDuel;
    const char* CONTROL = duel ? "AGGRESSOR"
                        : vsOpponent ? "OPPONENT" : vsScript ? "SCRIPT" : "RANDOM";
    const char* control = duel ? "aggressor"
                        : vsOpponent ? "opponent" : vsScript ? "script" : "random";
    AISystem::s_opponentModelPath = opponentModel;

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

    // Shared by the normal exit and the "there was nothing to measure" one, so
    // a run that gives up early still leaves the config, the frame cap and the
    // temporary map exactly as it found them.
    auto restore = [&]() {
        m_config.aiLearning = savedLearning;
        m_config.aiDifficulty = savedDifficulty;
        // Cleared with the rest: it is a static, and an AISystem built later in
        // this process for any other reason must not quietly acquire an
        // opponent left behind by a measurement run.
        AISystem::s_opponentModelPath.clear();
        applyFpsTarget(m_config.fpsTarget);
        unloadGameData();
        remove((m_dataDir + TextFormat("custom_maps/__ai_eval_%d__.odmap",
                                       (int)getpid())).c_str());
    };

    printf("[EVAL] %d map(s) x %d turn(s), seed %u, difficulty %s, worlds %s\n",
           numMaps, turnsPerMap, baseSeed, DIFF_NAMES[m_config.aiDifficulty],
           scenarios ? "SHIPPED SCENARIOS" : "generated");
    // OD_EVAL_MODEL points the evaluation at a specific file instead of the
    // shared data/ai/model.bin. PBT's fitness needs to rank each worker by how
    // it actually PLAYS, and it cannot do that by swapping the shared model
    // aside while three workers are mid-run. Read-only either way.
    if (const char* em = std::getenv("OD_EVAL_MODEL")) {
        if (em[0] && FileExists(em)) {
            m_aiModelPath.clear();
            m_evalModelOverride = em;
            printf("[EVAL] Model: %s (OD_EVAL_MODEL, read-only)\n", em);
        }
    }
    if (m_evalModelOverride.empty())
        printf("[EVAL] Model: %sai/model.bin (read-only — training may keep running)\n",
               m_dataDir.c_str());
    if (vsOpponent)
        printf("[EVAL] Opponent: %s (the control cohort plays this, not dice)\n",
               opponentModel.c_str());
    if (duel)
        printf("[EVAL] SCRIPT DUEL: model cohort = TURTLE (never attacks), "
               "control cohort = AGGRESSOR. No network is consulted.\n");
    else if (vsScript)
        printf("[EVAL] Opponent: the scripted rung -- a competent hand-written "
               "player, not dice\n");

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

    // Industry and ports the AI ADDED, summed over every map. Absolute
    // end-of-run counts cannot answer "does the AI build anything", because a
    // shipped scenario hands it a developed world and a generated map hands it
    // almost nothing; only the change over the run is comparable between them.
    long long bIndProv = 0, bLvls = 0, bSpec = 0, bPorts = 0;
    auto industrySnapshot = [&](long long& ip, long long& lv, long long& sp, long long& pt) {
        ip = lv = sp = 0;
        for (const auto& [pid, ind] : m_provinceIndustry) {
            (void)pid;
            if (ind.level > 0) { ip++; lv += ind.level; }
            if (!ind.specialization.empty()) sp++;
        }
        pt = (long long)m_provincePorts.size();
    };
    long long sIndProv = 0, sLvls = 0, sSpec = 0, sPorts = 0;
    // Beached hulls counted at load as well as at the end, because a map file
    // can ship one and that is not the movement code's fault. Only the rise
    // over a run indicts the resolver.
    long long beachedAtLoad = 0, hullsAtLoad = 0;
    auto beachedNow = [&]() {
        long long n = 0;
        for (const auto& s : m_ships)
            if (m_landSea.isLand((float)s.lon, (float)s.lat)) n++;
        return n;
    };

    for (int m = 0; m < numMaps && !aborted; ++m) {
        // Which world this map is. Under --scenarios the run walks the shipped
        // list instead of the archetypes; the two are never mixed inside one
        // run, because a mean over "three generated and two historical" is a
        // number with no referent and every stored row would need a footnote.
        const ShippedMap* ship = scenarios ? &SHIPPED_MAPS[m % SHIPPED_COUNT] : nullptr;
        const Scenario& sc = SCENARIOS[m % SCENARIO_COUNT];
        MapEditor::GeneratorParams p;
        p.seed = (int)(rng() & 0x7FFFFFFF);
        // SEED THE C PRNG, PER MAP, FROM THE MAP'S OWN SEED.
        //
        // Turn logic calls rand() directly -- combat damage rolls, rebellion
        // chances, breakaway naming. raylib's InitWindow calls
        // SetRandomSeed(time(NULL)), which on this build is srand(time(NULL)),
        // so that stream was seeded from the WALL CLOCK and every run of the
        // same seed played a different game.
        //
        // Measured before this line existed: two --eval-ai runs of seed 4242,
        // identical in every other respect, diverged on TURN 2 (province
        // ownership and treasury identical, army counts not -- combat rolls)
        // and finished 0.32x against 0.54x ADVANTAGE. That spread is larger
        // than most effects worth measuring, and it silently inflated every
        // interval and invalidated every cross-run comparison.
        //
        // Seeded from p.seed rather than once per process on purpose: map N
        // must play the same way whether it is the first map of the run or the
        // fourth, or --maps would change the result of every map after the
        // first.
        srand((unsigned int)p.seed);
        seedSimRng((unsigned int)p.seed);

        p.landCoverage = randRange(rng, sc.landMin, sc.landMax);
        p.numContinents = randRange(rng, sc.contMin, sc.contMax);
        p.jaggedness = randRange(rng, sc.jagMin, sc.jagMax);
        p.provinceDensity = randRange(rng, sc.densMin, sc.densMax);
        p.numCountries = randRange(rng, sc.countriesMin, sc.countriesMax);

        unloadGameData();
        std::string odmPath;
        if (ship) {
            // Loaded, not generated -- but still seeded above, because the map
            // being fixed does not make the GAME deterministic: combat rolls,
            // rebellion chances and breakaway names all come from rand(), and
            // the whole reproducibility argument above applies unchanged.
            odmPath = m_dataDir + ship->file;
            if (!FileExists(odmPath.c_str())) {
                printf("[EVAL] map %d: %s not installed, skipping\n", m + 1,
                       odmPath.c_str());
                continue;
            }
        } else {
            // A distinct name from the trainer's, so an eval run and a training
            // run side by side cannot delete each other's scratch map even if
            // they somehow shared a pid.
            std::string mapName = TextFormat("__ai_eval_%d__", (int)getpid());
            MapEditor ed;
            ed.init(m_screenW, m_screenH, m_dataDir);
            odmPath = ed.generateAndExportHeadless(p, mapName.c_str());
            if (odmPath.empty()) {
                printf("[EVAL] map %d: generation failed, skipping\n", m + 1);
                continue;
            }
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
        r.scenario = ship ? ship->name : sc.name;
        r.seed = p.seed;
        // COUNTED, not requested. numCountries is what the generator was asked
        // for; a shipped map was never asked anything, and every per-country
        // rate in the report divides by this. Counting the world that actually
        // loaded is right in both cases -- the generator does not always hit
        // its target either.
        {
            std::unordered_set<int> present;
            for (int owner : m_provinceCountryLookup)
                if (owner > 0 && owner < REBEL_CID_MIN) present.insert(owner);
            r.startCountries = (int)present.size();
        }
        industrySnapshot(sIndProv, sLvls, sSpec, sPorts);
        beachedAtLoad += beachedNow(); hullsAtLoad += (long long)m_ships.size();

        // ── Matched cohorts ──
        //
        // Alternating country ids would work only if id correlated with nothing;
        // it correlates with generation order, which correlates with position.
        // Ranking by starting size and alternating down the list gives both
        // cohorts the same spread of strong and weak starts, so a difference at
        // the end is a difference in play rather than in dealt hands. The seed
        // is fixed, so the split is identical in every run of this map.
        std::unordered_set<int> randomCids;
        if (split) {
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
        if (!m_ai) m_ai = new AISystem(this, m_evalModelOverride.empty()
                                                 ? m_dataDir + m_aiModelPath
                                                 : m_evalModelOverride);
        // BEFORE A SINGLE TURN, and fatal rather than a warning.
        //
        // A model file that would not load leaves the control cohort picking at
        // random, and every line of the report below would then describe a run
        // against dice under an "OPPONENT" heading. Nothing downstream could
        // tell, and the number would be quoted for weeks. The constructor has
        // already said why on stderr.
        // RETURNS, rather than breaking to the summary. With no maps played the
        // aggregate below still prints a full report -- survival, concentration,
        // and an ADVANTAGE of exactly 1.00x, because zero over zero is defined
        // to 1.0 there. Every one of those numbers is a formatting artefact of
        // an empty result set, and 1.00x in particular reads as a perfectly
        // respectable dead heat.
        if (vsOpponent && !m_ai->opponentLoaded()) {
            fprintf(stderr, "[EVAL] no opponent to measure against; nothing was run.\n");
            restore();
            return false;
        }
        m_ai->setRandomCountries(randomCids);

        printf("[EVAL] map %d/%d [%s] %s=%d countries=%d%s\n",
               m + 1, numMaps, r.scenario, ship ? "rollseed" : "seed",
               p.seed, r.startCountries,
               split ? TextFormat("  (%d model vs %d %s)", r.trainedCount,
                                  r.randomCount, control) : "");

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
            if (split)
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
        if (split) {
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
        if (split) {
            // Behaviour, side by side. The ratio says the model is losing; only
            // this says what it is doing differently while it loses.
            const double mk = r.trainedCountryTurns ? r.trainedCountryTurns / 1000.0 : 1.0;
            const double rk = r.randomCountryTurns  ? r.randomCountryTurns  / 1000.0 : 1.0;
            const AISystem::TrainStats& M = r.stats;
            const AISystem::TrainStats& R = r.rstats;
            // %11s so the control heading lands on the same column whether it
            // reads RANDOM or the two-characters-longer OPPONENT.
            printf("[EVAL]   per 1k country-turns      MODEL%11s\n", CONTROL);
            printf("[EVAL]     wars declared          %7.2f   %7.2f\n", M.warsDeclared/mk,      R.warsDeclared/rk);
            printf("[EVAL]     ceasefires offered     %7.2f   %7.2f\n", M.ceasefiresOffered/mk, R.ceasefiresOffered/rk);
            printf("[EVAL]     pacts proposed         %7.2f   %7.2f\n", M.pactsProposed/mk,     R.pactsProposed/rk);
            printf("[EVAL]     troops embarked        %7.2f   %7.2f\n", M.embarks/mk,           R.embarks/rk);
            printf("[EVAL]     landings               %7.2f   %7.2f\n", M.landings/mk,          R.landings/rk);
            printf("[EVAL]     staging moves          %7.2f   %7.2f\n", M.stagingMoves/mk,      R.stagingMoves/rk);
            printf("[EVAL]     destroyers built       %7.2f   %7.2f\n",
                   M.destroyersBuilt/mk, R.destroyersBuilt/rk);
            printf("[EVAL]     carriers built         %7.2f   %7.2f\n",
                   M.carriersBuilt/mk,   R.carriersBuilt/rk);
            printf("[EVAL]     ships scrapped         %7.2f   %7.2f\n", M.shipsScrapped/mk,     R.shipsScrapped/rk);
            printf("[EVAL]     austerity cuts         %7.2f   %7.2f\n", M.austerityCuts/mk,     R.austerityCuts/rk);
            printf("[EVAL]     turns bankrupt         %7.2f   %7.2f\n", M.bankruptTurns/mk,     R.bankruptTurns/rk);
            printf("[EVAL]     calming policies       %7.2f   %7.2f\n", M.calmingPolicies/mk,   R.calmingPolicies/rk);
            printf("[EVAL]     minorities conciliated %7.2f   %7.2f\n", M.minorityConciliations/mk, R.minorityConciliations/rk);
            printf("[EVAL]     minorities repressed   %7.2f   %7.2f\n", M.minorityRepressions/mk,   R.minorityRepressions/rk);
            printf("[EVAL]     research completed     %7.2f   %7.2f\n", M.researchCompleted/mk, R.researchCompleted/rk);
            printf("[EVAL]     calls answered/issued  %4lld/%-4lld  %4lld/%-4lld\n",
                   M.callsAnswered, M.callsIssued, R.callsAnswered, R.callsIssued);
            // EVERY request, not only calls to arms. See diploRequests: an
            // alliance nobody accepts is why nobody ever gets to issue a call.
            printf("[EVAL]     said yes/was asked     %4lld/%-4lld  %4lld/%-4lld\n",
                   M.diploAccepted, M.diploRequests, R.diploAccepted, R.diploRequests);
            // WHAT IT SAID WHEN IT SAID NO. "caught" is an invariant, not a
            // statistic: a lie the asker could check against the map should
            // never be chosen, so anything other than zero means the
            // believability rule has stopped working.
            printf("[EVAL]     refusals: silent       %7lld   %7lld\n",
                   M.refusalsSilent, R.refusalsSilent);
            printf("[EVAL]     refusals: told true    %7lld   %7lld\n",
                   M.refusalsTrue, R.refusalsTrue);
            printf("[EVAL]     refusals: lied         %7lld   %7lld\n",
                   M.refusalsLied, R.refusalsLied);
            printf("[EVAL]     refusals: caught out   %7lld   %7lld\n",
                   M.refusalsCaught, R.refusalsCaught);
            // ...and the same three questions about declarations. "pretext"
            // counts wars announced as something other than what they are for;
            // "caught out" is the invariant and must be zero.
            printf("[EVAL]     wars: no reason given  %7lld   %7lld\n",
                   M.warGoalSilent, R.warGoalSilent);
            printf("[EVAL]     wars: true goal stated %7lld   %7lld\n",
                   M.warGoalTrue, R.warGoalTrue);
            printf("[EVAL]     wars: pretext          %7lld   %7lld\n",
                   M.warGoalPretext, R.warGoalPretext);
            printf("[EVAL]     wars: caught out       %7lld   %7lld\n",
                   M.warGoalCaught, R.warGoalCaught);
            // Does the war goal reach the peace table. See ceasefireProvsAsked.
            {
                auto pct = [](long long a, long long b) {
                    return b ? 100.0 * a / b : 0.0;
                };
                printf("[EVAL]     peace: provs demanded  %7lld   %7lld\n",
                       M.ceasefireProvsAsked, R.ceasefireProvsAsked);
                printf("[EVAL]     peace: %% of them claimed %6.1f   %7.1f\n",
                       pct(M.ceasefireClaimedAsked, M.ceasefireProvsAsked),
                       pct(R.ceasefireClaimedAsked, R.ceasefireProvsAsked));
                printf("[EVAL]     peace: kept the claim  %7lld   %7lld\n",
                       M.ceasefireHeldClaim, R.ceasefireHeldClaim);
            }
            // Country-turns under each posture. A stance that never changes is
            // not a plan either, and only this says which it is.
            {
                static const char* SN[AISystem::STANCE_COUNT] =
                    {"expand", "consolidate", "defend", "develop"};
                long long mt = 0, rt = 0;
                for (int i = 0; i < AISystem::STANCE_COUNT; ++i) {
                    mt += M.stanceHeld[i]; rt += R.stanceHeld[i];
                }
                printf("[EVAL]   -- stance: share of country-turns --\n");
                for (int i = 0; i < AISystem::STANCE_COUNT; ++i)
                    printf("[EVAL]     %-11s %7lld (%4.1f%%)  %7lld (%4.1f%%)\n",
                           SN[i], M.stanceHeld[i],
                           mt ? 100.0 * M.stanceHeld[i] / mt : 0.0,
                           R.stanceHeld[i],
                           rt ? 100.0 * R.stanceHeld[i] / rt : 0.0);
            }
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
                printf("[EVAL]     %-8s start %4d  +%lld -%lld  => %lld predicted, "
                       "%d actual, UNEXPLAINED %+lld\n",
                       who, startP, gained, lost, predicted, endP,
                       (long long)endP - predicted);
            };
            ledger("MODEL", r.trainedStartProv, r.trainedProvinces, M);
            ledger(CONTROL, r.randomStartProv,  r.randomProvinces,  R);

            // WHAT THE WAR MODULE DOES WITH ITS TURN. The ledger says the model
            // shrinks because it wins fewer battles; this says whether that is
            // because attack was never on the menu or because it was declined.
            // "take" is chosen/offered. What the control column means depends
            // on which control ran: a RANDOM cohort is uniform over whatever is
            // valid, so its take rate IS the availability baseline and any
            // action where the model sits far below it is a learned preference
            // rather than a constraint the game imposed. An OPPONENT cohort has
            // preferences of its own, so the same column stops being a baseline
            // and becomes the other policy's taste -- still worth reading, but
            // as a comparison of two players rather than against availability.
            static const char* ECON_NAME[AISystem::ECON_ACTIONS] = {
                "save", "industry", "fort", "port", "specialize", "destroyer",
                "carrier", "fund up", "fund down", "focus bldg", "focus army",
                "focus navy"};
            printf("[EVAL]   -- econ action: offered / chosen (take%%) --\n");
            for (int i = 0; i < AISystem::ECON_ACTIONS; ++i) {
                const double mt = M.econOffered[i] ? 100.0 * M.econChosen[i] / M.econOffered[i] : 0.0;
                const double rt = R.econOffered[i] ? 100.0 * R.econChosen[i] / R.econOffered[i] : 0.0;
                printf("[EVAL]     %-11s %7lld/%-7lld (%4.1f%%)  %7lld/%-7lld (%4.1f%%)\n",
                       ECON_NAME[i], M.econOffered[i], M.econChosen[i], mt,
                       R.econOffered[i], R.econChosen[i], rt);
            }
            printf("[EVAL]     research picks         %7lld   %7lld\n",
                   M.researchPicked, R.researchPicked);
            printf("[EVAL]       ...armed a node      %7lld   %7lld\n",
                   M.researchArmed, R.researchArmed);
            printf("[EVAL]       ...nothing left      %7lld   %7lld\n",
                   M.researchNothingLeft, R.researchNothingLeft);
            printf("[EVAL]     funded turns on a node %7lld   %7lld\n",
                   M.researchFundedTurns, R.researchFundedTurns);
            printf("[EVAL]     stalled unfunded turns %7lld   %7lld\n",
                   M.researchStalls, R.researchStalls);
            printf("[EVAL]     nodes completed        %7lld   %7lld\n",
                   M.researchCompleted, R.researchCompleted);

            static const char* WAR_NAME[AISystem::WAR_ACTIONS] = {
                "hold", "recruit", "reinforce", "attack",
                "declare war", "artillery", "ceasefire", "stage"};
            // THE OFFENSIVE FUNNEL. Raw battle counts compare nothing when the
            // two cohorts spend different amounts of time at war; these are the
            // stages between "at war" and "province taken".
            printf("[EVAL]   -- offensive funnel --\n");
            printf("[EVAL]     country-turns at war   %7lld   %7lld\n", M.turnsAtWar,     R.turnsAtWar);
            printf("[EVAL]     attack orders issued   %7lld   %7lld\n", M.attackIssued,   R.attackIssued);
            // Of those, how many the learned head aimed. Zero means the margin
            // rule is still choosing -- which is correct below its warmup and a
            // problem after it, and those look identical in every other number.
            printf("[EVAL]       ...aimed by the head %7lld   %7lld\n",
                   M.attackSteered, R.attackSteered);
            printf("[EVAL]     attack: no target      %7lld   %7lld\n", M.attackNoTarget, R.attackNoTarget);
            // ATTACKS THAT LOST. Everything above this counts attempts and
            // successes; without the failures, "issued minus won" is an attack
            // in progress and an army thrown away added together.
            printf("[EVAL]     attacks repulsed       %7lld   %7lld\n",
                   M.attacksRepulsed, R.attacksRepulsed);
            printf("[EVAL]     men lost attacking     %7lld   %7lld\n",
                   M.troopsLostAttacking, R.troopsLostAttacking);
            {
                // AGAINST BATTLES WON, not against attack orders issued.
                //
                // Those are two different denominators and dividing by the
                // wrong one reads 650%: attackIssued counts the war module
                // DECIDING to attack, at most once per country-turn, while a
                // repulse is counted where combat resolves -- which also
                // happens for the garrison and redeploy reflexes' moves, and
                // more than once a turn. Both numbers below are counted at the
                // same place on the same events, so their ratio is the share of
                // resolved assaults that failed, which is the thing worth
                // knowing.
                auto lossRate = [](long long lost, long long won) {
                    const long long n = lost + won;
                    return n ? 100.0 * lost / n : 0.0;
                };
                printf("[EVAL]     %% of assaults that lost %6.1f   %7.1f\n",
                       lossRate(M.attacksRepulsed, M.provTakenInBattle),
                       lossRate(R.attacksRepulsed, R.provTakenInBattle));
            }
            printf("[EVAL]     attack: order pending  %7lld   %7lld\n", M.attackPending,  R.attackPending);
            {
                auto rate = [](long long num, long long den) { return den ? (double)num / den : 0.0; };
                printf("[EVAL]     battles won / turn at war  %7.3f   %7.3f\n",
                       rate(M.provTakenInBattle, M.turnsAtWar),
                       rate(R.provTakenInBattle, R.turnsAtWar));
                printf("[EVAL]     battles won / attack issued %6.3f   %7.3f\n",
                       rate(M.provTakenInBattle, M.attackIssued),
                       rate(R.provTakenInBattle, R.attackIssued));
                printf("[EVAL]     attacks issued / turn at war %5.3f   %7.3f\n",
                       rate(M.attackIssued, M.turnsAtWar),
                       rate(R.attackIssued, R.turnsAtWar));
            }

            // THE POLICY'S OWN DISTRIBUTION, at a neutral temperature.
            //
            // One column, not two: this describes what a NET believes, and the
            // control cohort is either dice or a script, neither of which has a
            // belief to report. Sums to 100% down each module. Read it against
            // the take rates -- a module whose take rate is 98% and whose mean
            // probability is 30% has a preference that sampling amplified; one
            // where both read 98% has stopped choosing, and only the second
            // needs a reward corrected and a head reset.
            {
                static const char* WN[AISystem::WAR_ACTIONS] = {
                    "hold", "recruit", "reinforce", "attack",
                    "declare war", "artillery", "ceasefire", "stage"};
                // Denominator is offers ON TURNS THERE WAS A CHOICE -- see
                // pickAction. A turn with one legal action is not evidence
                // about a preference, and averaging it in is what made "hold"
                // read as 100% when the advantage said attack.
                printf("[EVAL]   -- policy shape at T=1.0 (mean P where a choice existed) --\n");
                for (int i = 0; i < AISystem::WAR_ACTIONS; ++i)
                    printf("[EVAL]     P war:%-11s %6.1f  (n=%lld)\n", WN[i],
                           M.warProbN[i] ? 100.0 * M.warProbMass[i] / M.warProbN[i] : 0.0,
                           M.warProbN[i]);
                for (int i = 0; i < AISystem::ECON_ACTIONS; ++i)
                    printf("[EVAL]     P econ:%-10s %6.1f  (n=%lld)\n", ECON_NAME[i],
                           M.econProbN[i] ? 100.0 * M.econProbMass[i] / M.econProbN[i] : 0.0,
                           M.econProbN[i]);
            }

            printf("[EVAL]   -- war action: offered / chosen (take%%) --\n");
            for (int i = 0; i < AISystem::WAR_ACTIONS; ++i) {
                const double mt = M.warOffered[i] ? 100.0 * M.warChosen[i] / M.warOffered[i] : 0.0;
                const double rt = R.warOffered[i] ? 100.0 * R.warChosen[i] / R.warOffered[i] : 0.0;
                printf("[EVAL]     %-11s %7lld/%-7lld (%4.1f%%)  %7lld/%-7lld (%4.1f%%)\n",
                       WAR_NAME[i], M.warOffered[i], M.warChosen[i], mt,
                       R.warOffered[i], R.warChosen[i], rt);
            }

            const int total = r.trainedProvinces + r.randomProvinces;
            printf("[EVAL]   MODEL %d provinces (%.0f%%), %d/%d alive | "
                   "%s %d provinces (%.0f%%), %d/%d alive  -> %s\n",
                   r.trainedProvinces, total ? 100.0 * r.trainedProvinces / total : 0.0,
                   r.trainedAlive, r.trainedCount, CONTROL,
                   r.randomProvinces, total ? 100.0 * r.randomProvinces / total : 0.0,
                   r.randomAlive, r.randomCount,
                   r.trainedProvinces > r.randomProvinces ? "MODEL WINS"
                     : (r.trainedProvinces < r.randomProvinces
                          ? (vsOpponent ? "OPPONENT WINS"
                             : vsScript ? "SCRIPT WINS" : "RANDOM WINS") : "draw"));
        }
        {
            long long eIndProv, eLvls, eSpec, ePorts;
            industrySnapshot(eIndProv, eLvls, eSpec, ePorts);
            bIndProv += eIndProv - sIndProv; bLvls  += eLvls  - sLvls;
            bSpec    += eSpec    - sSpec;    bPorts += ePorts - sPorts;
        }
        results.push_back(r);
    }

    // ── Aggregate ──
    long long totalTurns = 0, totalCountryTurns = 0, decided = 0, frozen = 0;
    long long wars = 0, ceases = 0, pacts = 0, rebels = 0, research = 0;
    long long calls = 0, answered = 0, refused = 0, staged = 0;
    long long dipReq = 0, dipYes = 0;
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
        dipReq += r.stats.diploRequests;   dipYes += r.stats.diploAccepted;
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
    // Before the coalition line, because it is the one that explains it: with
    // nothing agreed to, there are no allies, and "0 of 0 calls answered" is
    // the symptom rather than the disease.
    // WHETHER LYING IS COSTING ANYTHING YET. Mean credibility over every pair
    // that has one at all -- pairs nobody has lied to are absent and would
    // otherwise drown the number in 1.0s. A run where this stays at 1.000 means
    // the conduct checks never fired, which is a finding about the checks
    // rather than about the AI's honesty.
    {
        double sum = 0.0; long long n = 0; double worst = 1.0;
        for (const auto& [speaker, byHearer] : m_credibility)
            for (const auto& [hearer, c] : byHearer) {
                (void)speaker; (void)hearer;
                sum += c; n++; worst = std::min(worst, (double)c);
            }
        printf("[EVAL] credibility   %lld caught out, lowest word ever %.3f; "
               "now %.3f mean over %lld pair(s)\n",
               m_credibilityHits, m_credibilityLow,
               n > 0 ? sum / n : 1.0, n);
    }
    printf("[EVAL] agreements    %.0f%% of %lld diplomatic requests accepted\n",
           dipReq ? 100.0 * dipYes / dipReq : 0.0, dipReq);
    printf("[EVAL] coalition      %.0f%% of %lld calls to arms answered, %lld staging moves\n",
           calls ? 100.0 * answered / calls : 0.0, calls, staged);
    printf("[EVAL] amphibious     %.0f%% of %lld embarkations reached a hostile shore (%lld came home)\n",
           embarks ? 100.0 * landings / embarks : 0.0, embarks, home);
    printf("[EVAL] fleet          %.2f hulls scrapped per 1k country-turns\n", scrapped / kct);
    printf("[EVAL] unrest         %.2f rebellions, %.2f research nodes per 1k country-turns\n",
           rebels / kct, research / kct);
    // Political distance between governments and their own provinces. Reported
    // because the feature is only worth having if it is ever non-zero: a
    // generated map with no compass data would leave it dead and look
    // identical to a country in perfect agreement with itself.
    if (m_ai) {
        const int ghosts = m_ai->warsWithTheDead();
        printf("[EVAL] ghost wars    %d live war(s) against countries holding no "
               "land%s\n", ghosts, ghosts ? "   << INVARIANT BROKEN" : "");
        float gm = 0.0f, gw = 0.0f;
        m_ai->compassGap(gm, gw);
        printf("[EVAL] compass       mean gap %.3f, worst %.3f (government vs its own provinces)\n",
               gm, gw);
    }
    // WHAT THE ECONOMY MODULE ACTUALLY BUILT, not what it chose to build. The
    // econ take rate counts the decision, which is taken before the executor
    // runs and long before processUpgrades applies anything -- so a build that
    // is charged for and then dropped on the floor reads as a healthy take
    // rate and an unchanged world. That is precisely what was happening to
    // every first factory and every new port. If these counts sit near their
    // map-file starting values while the take rate is non-zero, the builds are
    // being eaten again somewhere between the decision and the tick.
    // DOES THE SUFFICIENCY BAR BIND? armyTerm only has a gradient while PHI is
    // below 1, so if countries routinely sit above the bar the whole term is
    // dead and recruit's only remaining signal is its cost. Reports the army a
    // country actually holds per province against the peacetime bar, which is
    // ARMY_SUFFICIENCY * PEACETIME_READINESS per province.
    {
        std::unordered_map<int, long long> armyOf;
        std::unordered_map<int, int> provOf;
        for (int pid = 0; pid < (int)m_provinceCountryLookup.size(); ++pid) {
            const int o = m_provinceCountryLookup[pid];
            if (o <= 0 || o >= REBEL_CID_MIN) continue;
            provOf[o]++;
            auto it = m_provinceArmies.find(pid);
            if (it == m_provinceArmies.end()) continue;
            for (const auto& u : it->second)
                if (u.countryId == o) armyOf[o] += u.count;
        }
        // The bar the reward actually uses: ARMY_SUFFICIENCY * PEACETIME_PARITY
        // * the world's mean garrison density. Reported so it is obvious at a
        // glance whether the term has any gradient left -- a bar far under what
        // countries hold means PHI is pinned at 1 and armyTerm is dead, which
        // is what a hand-set constant of 400 turned out to mean.
        double worldPerProv = 0.0;
        {
            std::vector<double> dens;
            for (const auto& [c, p] : provOf)
                if (p > 0) dens.push_back((double)armyOf[c] / (double)p);
            if (!dens.empty()) {
                const size_t mid = dens.size() / 2;
                std::nth_element(dens.begin(), dens.begin() + mid, dens.end());
                worldPerProv = dens[mid];   // median, matching refreshStats
            }
        }
        const double bar = 2.0 /*ARMY_SUFFICIENCY*/ * 0.5 /*PEACETIME_PARITY*/ * worldPerProv;
        int below = 0, n = 0; double sumPerProv = 0.0;
        for (const auto& [cid, prov] : provOf) {
            if (prov <= 0) continue;
            const double perProv = (double)armyOf[cid] / (double)prov;
            sumPerProv += perProv; n++;
            if (perProv < bar) below++;
        }
        printf("[EVAL] readiness      %.0f troops/province held on average; "
               "%d of %d countries below the %.0f bar (%.0f%%)\n",
               n ? sumPerProv / n : 0.0, below, n, bar,
               n ? 100.0 * below / n : 0.0);
    }
    // A CLAIM ON YOUR OWN GROUND IS A DEMAND THAT IS ALREADY MET. It paints an
    // owned province contested, lists the owner under its own "Claimed by",
    // and feeds the AI a grievance it can never discharge.
    {
        int selfClaims = 0, allClaims = 0;
        for (const auto& [iso, pids] : m_claims) {
            const int cid = cidForIso(iso);
            for (int pid : pids) {
                allClaims++;
                if (cid <= 0) continue;
                const Province* p = m_provinces.getProvinceById(pid);
                if (p && p->countryId == cid) selfClaims++;
            }
        }
        printf("[EVAL] self-claims    %d of %d claim(s) are on ground the claimant "
               "already holds%s\n", selfClaims, allClaims,
               selfClaims ? "   << INVARIANT BROKEN" : "");
    }
    // TROOPS STANDING IN A COUNTRY THEY ARE AT PEACE WITH. Legal only while a
    // war or an alliance justifies the presence: anything else is an army that
    // some earlier transition forgot to send home, and it sits there forever
    // because nothing in the engine attrits or expels a foreign stack.
    {
        int trespass = 0, stacks = 0;
        for (const auto& [pid, units] : m_provinceArmies) {
            const int owner = (pid >= 0 && (size_t)pid < m_provinceCountryLookup.size())
                                ? m_provinceCountryLookup[pid] : 0;
            if (owner <= 0) continue;
            const std::string& oIso = m_countries.getAll()[owner].isoA3;
            for (const auto& u : units) {
                if (u.count <= 0 || u.countryId == owner || u.countryId <= 0) continue;
                stacks++;
                const auto* uc = m_countries.getCountry(u.countryId);
                if (!uc) continue;
                const CountryRelation& r = m_relations[uc->isoA3][oIso];
                if (!r.war && !r.alliance) {
                    trespass++;
                    // Named, capped, because "5 stacks" does not tell you WHICH
                    // transition forgot to send them home and the pair usually
                    // does -- a rebel and its parent, a dead state, an ex-ally.
                    if (trespass <= 5)
                        printf("[EVAL]   trespass: %s (%d) has %d troop(s) in %s (%d) prov %d%s\n",
                               uc->name.c_str(), u.countryId, u.count,
                               m_countries.getAll()[owner].name.c_str(), owner, pid,
                               u.countryId >= REBEL_CID_MIN ? " [rebel]"
                                 : owner >= REBEL_CID_MIN ? " [in rebel]" : "");
                }
            }
        }
        printf("[EVAL] trespass       %d of %d foreign stack(s) in a country they are "
               "neither at war with nor allied to%s\n", trespass, stacks,
               trespass ? "   << INVARIANT BROKEN" : "");
    }
    // TWO ARMIES AT WAR STANDING ON THE SAME PROVINCE, and one of them owns it.
    //
    // The battle is the thing that resolves this: whoever wins holds the
    // ground and the loser is destroyed. A pair still sharing a province after
    // the turn means some assault settled with one of them, which is exactly
    // what resolveAssault replaced -- the old code fought the first hostile
    // stack it found and left every other one standing on ground that had just
    // changed hands. Counted separately from trespass above, which only sees
    // stacks with no war to justify them; these have a war and are still wrong.
    //
    // Also counts two stacks of ONE country in a province: a move order takes
    // its share of the first it finds, so the second is an army no order can
    // reach.
    {
        int contested = 0, doubled = 0;
        for (const auto& [pid, units] : m_provinceArmies) {
            const int owner = (pid >= 0 && (size_t)pid < m_provinceCountryLookup.size())
                                ? m_provinceCountryLookup[pid] : 0;
            std::unordered_set<int> seenCid;
            for (const auto& u : units) {
                if (u.count <= 0 || u.countryId <= 0) continue;
                if (!seenCid.insert(u.countryId).second) doubled++;
                if (owner > 0 && u.countryId != owner && atWarCids(u.countryId, owner)) {
                    contested++;
                    // Named and capped, for the same reason trespass names its
                    // pairs: the count says how bad, the pair says which
                    // transition let it happen.
                    if (contested <= 3) {
                        const auto* uc = m_countries.getCountry(u.countryId);
                        printf("[EVAL]   occupation: %s (%d) has %d troop(s) inside %s's "
                               "prov %d\n", uc ? uc->name.c_str() : "?", u.countryId,
                               u.count, m_countries.getAll()[owner].name.c_str(), pid);
                    }
                }
            }
        }
        printf("[EVAL] occupation     %d stack(s) sharing a province with an owner they "
               "are at war with, %d duplicate stack(s)%s\n", contested, doubled,
               (contested || doubled) ? "   << INVARIANT BROKEN" : "");
    }
    printf("[EVAL] sea combat    %lld engagement(s), %lld hull(s) sunk, of which "
           "%lld loaded transport(s) carrying %lld troops\n",
           m_navEngagements, m_navSinkings, m_navTransportsSunk, m_navCrewDrowned);
    printf("[EVAL] routing       %lld of %lld ship move(s) stopped dead by land "
           "(%.0f%%)\n", m_navBlocked, m_navMoves,
           m_navMoves ? 100.0 * (double)m_navBlocked / (double)m_navMoves : 0.0);
    // A HULL SITTING ON LAND IS ALWAYS A BUG, whoever moved it there. The navy
    // executor used to assign its destination outright with no water test, so
    // any crossing whose straight line clipped a headland beached the ship
    // permanently. Reported rather than asserted because pre-existing saves can
    // load with beached hulls and the clamp is meant to walk them back to sea.
    {
        const long long beached = beachedNow();
        printf("[EVAL] beached        %lld of %zu hull(s) on land now; "
               "%lld of %lld were already beached at load%s\n",
               beached, m_ships.size(), beachedAtLoad, hullsAtLoad,
               beached ? "   << INVARIANT BROKEN" : "");
    }
    printf("[EVAL] built         %+lld industrial province(s), %+lld industry level(s), "
           "%+lld specialization(s), %+lld port(s) over the run\n",
           bIndProv, bLvls, bSpec, bPorts);
    printf("[EVAL] minorities     mean alignment %.0f%%, %.0f%% of groups disaffected (<40%%)\n",
           meanAlign / n, 100.0 * disaffected / n);
    printf("[EVAL] governing      %.2f conciliations, %.2f repressions, %.2f calming policies "
           "per 1k country-turns\n", conciliated / kct, repressed / kct, calming / kct);
    // The headline solvency number. Bankruptcy costs twenty points of rebellion
    // chance in every province, so a high figure here explains a high one two
    // lines up.
    // HOW MUCH TRAINING IS BEHIND EACH HEAD. Without it a reward-term gate
    // cannot tell a collapsed distribution from a freshly reset one, and those
    // need opposite responses. See AISystem::moduleUpdates.
    if (m_ai)
        printf("[EVAL] training      econ %llu  politics %llu  war %llu  navy %llu"
               "  policy updates\n",
               m_ai->moduleUpdates(0), m_ai->moduleUpdates(1),
               m_ai->moduleUpdates(2), m_ai->moduleUpdates(3));
    // The playability line. Not about how well it plays -- about whether the
    // player is waiting for it. 185 countries thinking on the present-day map
    // is where this stops being free.
    {
        long long us = 0, calls = 0;
        for (const MapResult& r : results) {
            us += r.stats.thinkMicros + r.rstats.thinkMicros;
            calls += r.stats.thinkCalls + r.rstats.thinkCalls;
        }
        printf("[EVAL] thinking      %.3f ms per country-turn (%.1f s total over "
               "%lld decisions)\n",
               calls ? (double)us / calls / 1000.0 : 0.0, us / 1.0e6, calls);
    }
    printf("[EVAL] solvency       %.1f%% of country-turns spent bankrupt, "
           "%.2f austerity cuts per 1k\n",
           totalCountryTurns ? 100.0 * bankruptTurns / totalCountryTurns : 0.0,
           austerityCuts / kct);
    // One line, stable field order, for diffing two model versions.
    // ── Per world ──
    //
    // The aggregate is a mean over worlds that are not the same game. A model
    // can hold its own on pangaea and be dismantled on archipelago, or play the
    // procedural maps well and fall apart on 1939 -- which is the whole reason
    // the shipped scenarios were added -- and one ADVANTAGE figure reports the
    // average of those as though it described either.
    //
    // ONE space after [EVAL], deliberately. tools/ai_bench.py picks up global
    // two-column metrics with a four-space prefix; these rows must not be
    // mistaken for them and averaged into the headline.
    if (split && results.size() > 1) {
        std::vector<std::string> order;
        std::unordered_map<std::string, std::array<long long, 3>> byName; // maps, model, control
        for (const MapResult& r : results) {
            auto it = byName.find(r.scenario);
            if (it == byName.end()) {
                order.push_back(r.scenario);
                it = byName.emplace(r.scenario, std::array<long long, 3>{0, 0, 0}).first;
            }
            it->second[0] += 1;
            it->second[1] += r.trainedProvinces;
            it->second[2] += r.randomProvinces;
        }
        if (order.size() > 1) {
            printf("\n[EVAL] ----- BY WORLD -----\n");
            printf("[EVAL] world       maps    model %8s   ADVANTAGE\n", control);
            for (const std::string& nm : order) {
                const auto& v = byName[nm];
                printf("[EVAL] %-11s %4lld %8lld %8lld %8.2fx\n", nm.c_str(),
                       v[0], v[1], v[2],
                       v[2] ? (double)v[1] / (double)v[2] : (v[1] ? 99.0 : 1.0));
            }
        }
    }

    // FIELD POSITIONS ARE FIXED HERE, only the words change. tools/ai_bench.py
    // and tools/ai_benchmark.sh both read these lines by column -- "maps won"
    // is $4 and $6, "land held" is $4, "ADVANTAGE" is $3 -- so relabelling the
    // control may replace a word but must never add or drop one, or both tools
    // start silently reporting the wrong number.
    if (split) {
        const long long tot = trainedProv + randomProv;
        const double share = tot ? 100.0 * trainedProv / tot : 0.0;
        // Survival rates rather than raw counts: the cohorts are the same size
        // by construction, but a map that ends early leaves both incomplete.
        const double tSurv = trainedStart ? 100.0 * trainedAlive / trainedStart : 0.0;
        const double rSurv = randomStart ? 100.0 * randomAlive / randomStart : 0.0;
        printf("\n[EVAL] ----- MODEL vs %s -----\n", CONTROL);
        printf("[EVAL] maps won       %lld model, %lld %s, %zu drawn\n",
               modelWins, randomWins, control,
               results.size() - (size_t)modelWins - (size_t)randomWins);
        printf("[EVAL] land held      %.1f%% model / %.1f%% %s  (50%% = %s)\n",
               share, 100.0 - share, control,
               vsOpponent ? "level with the opponent"
                          : vsScript ? "level with the scripted player"
                                     : "no better than a coin flip");
        printf("[EVAL] survival       %.0f%% of model countries, %.0f%% of %s countries\n",
               tSurv, rSurv, control);
        // One number to watch across model versions. Below 1.0 the model is
        // losing, which no reward curve will tell you and which has exactly one
        // honest interpretation.
        //
        // What 1.0 MEANS is not the same in the two modes, and that is the
        // whole reason --vs-model exists. Against random it is a floor that
        // never rises, so a model well past it has no yardstick left. Against a
        // named file it is parity with a specific opponent, which stays a real
        // question however good both sides get -- pin a stronger file and 1.0
        // means something harder.
        printf("[EVAL] ADVANTAGE      %.2fx the land %s holds\n",
               randomProv ? (double)trainedProv / randomProv
                          : (trainedProv ? 99.0 : 1.0),
               vsOpponent ? "the opponent"
                          : vsScript ? "the scripted player" : "a coin flip");
    }

    printf("[EVAL] CSV,maps,turns,decided,frozen,alive_pct,largest_pct,herfindahl,"
           "wars_k,ceasefires_k,pacts_k,calls_answered_pct,landing_pct,scrap_k,rebellions_k,"
           "align_pct,disaffected_pct,conciliate_k,repress_k,calming_k,"
           "vs_random,model_land_pct,model_wins,random_wins,bankrupt_pct,opponent\n");
    printf("[EVAL] CSV,%zu,%lld,%lld,%lld,%.2f,%.2f,%.4f,%.3f,%.3f,%.3f,%.2f,%.2f,%.3f,%.3f,"
           "%.2f,%.2f,%.3f,%.3f,%.3f\n",
           results.size(), totalTurns, decided, frozen, 100.0 * aliveFrac / n,
           100.0 * largest / n, conc / n, wars / kct, ceases / kct, pacts / kct,
           calls ? 100.0 * answered / calls : 0.0,
           embarks ? 100.0 * landings / embarks : 0.0, scrapped / kct, rebels / kct,
           meanAlign / n, 100.0 * disaffected / n,
           conciliated / kct, repressed / kct, calming / kct);
    // `opponent` is APPENDED rather than folded into vs_random, which keeps the
    // existing five fields where any reader already expects them. It is the one
    // thing that distinguishes two otherwise identical rows measured against
    // completely different controls, so "-" has to mean dice and nothing else.
    printf("[EVAL] CSV_EXTRA,%d,%.2f,%lld,%lld,%.2f,%s\n", split ? 1 : 0,
           (trainedProv + randomProv) ? 100.0 * trainedProv / (trainedProv + randomProv) : 0.0,
           modelWins, randomWins,
           totalCountryTurns ? 100.0 * bankruptTurns / totalCountryTurns : 0.0,
           vsOpponent ? opponentModel.c_str() : "-");

    restore();
    return true;
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
