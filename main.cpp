#include "Game.h"
#include "Audio.h"
#include "ai/AISystem.h"
#include "WinFatalDialog.h"
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    // No dialogs in a run with nobody at the keyboard.
    //
    // A message box blocks the thread that raised it until it is dismissed, so
    // on a build server or inside a script it does not report a failure, it
    // becomes one. A Windows CI job hung for its full thirty-minute timeout on
    // a startup message nobody could click OK on, and every one of these modes
    // is meant to run unattended. The text still goes to stderr.
    for (int i = 1; i < argc; ++i) {
        static const char* kHeadless[] = {
            "--train-ai", "--eval-ai", "--simulate", "--screenshots", "--tutorial-walk",
            "--export-timelapse", "--merge-ai", "--reset-ai-head",
        };
        for (const char* f : kHeadless)
            if (strcmp(argv[i], f) == 0) { odSuppressFatalDialogs(); break; }
    }

    // --ai-readonly: use the trained model but never save over it. Intended for
    // watching current AI behaviour in a real game while a --train-ai session
    // runs in parallel; without it both processes write data/ai/model.bin every
    // 20 turns and the shorter session clobbers the longer one's progress.
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--ai-readonly") == 0) {
            AISystem::s_readOnlyModel = true;
            std::cout << "[AI] Read-only model: this session will not save "
                         "data/ai/model.bin" << std::endl;
        }

    // Training has nobody listening, and neither does a measurement run. This
    // has to be decided before init(), which is where the device would
    // otherwise be opened -- and the headless machines that run training are
    // the ones least likely to have one.
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--train-ai") == 0 || strcmp(argv[i], "--eval-ai") == 0)
            Audio::s_disabled = true;

    // --export-timelapse <save.odsv> [out.gif] [WxH] [political|population|troops]
    // Headless: no window, no audio, no UI. Runs before anything is initialised.
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--export-timelapse") != 0) continue;
        if (i + 1 >= argc) {
            fprintf(stderr, "--export-timelapse needs a save path\n");
            return 2;
        }
        Audio::s_disabled = true;
        std::string save = argv[i + 1];
        std::string out = (i + 2 < argc && argv[i + 2][0] != '-') ? argv[i + 2]
                                                                 : "timelapse.gif";
        int w = 960, h = 480;
        Game::HistoryView view = Game::HV_POLITICAL;
        for (int k = i + 3; k < argc; ++k) {
            std::string a = argv[k];
            int pw = 0, ph = 0;
            if (sscanf(a.c_str(), "%dx%d", &pw, &ph) == 2 && pw > 0 && ph > 0) {
                w = pw; h = ph;
            } else if (a == "population") {
                view = Game::HV_POPULATION;
            } else if (a == "troops") {
                view = Game::HV_TROOPS;
            }
        }
        Game g;
        // Turn the corner credit off for this run without editing config.json,
        // which is what a script wants when it is generating art rather than
        // sharing a playthrough.
        for (int k = i + 2; k < argc; ++k)
            if (strcmp(argv[k], "--no-watermark") == 0) g.setTimelapseWatermark(false);
        bool ok = g.exportTimelapseHeadless(save, out, w, h, 6, view);
        return ok ? 0 : 1;
    }

    // --merge-ai <out.bin> <in1.bin> <in2.bin> ...
    // Averages several model files into one. The end of a parallel training
    // run: each worker leaves its own model behind and this folds them into the
    // shared data/ai/model.bin the game actually loads. No window, no world.
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--merge-ai") != 0) continue;
        std::vector<std::string> inputs;
        for (int k = i + 2; k < argc && strncmp(argv[k], "--", 2) != 0; ++k)
            inputs.push_back(argv[k]);
        if (i + 1 >= argc || inputs.empty()) {
            fprintf(stderr, "--merge-ai needs an output path and at least one input\n");
            return 2;
        }
        return AISystem::mergeModelFiles(argv[i + 1], inputs) ? 0 : 1;
    }

    // --reset-ai-head <model.bin> <econ|politics|war|navy>
    // Discards one module's policy, value baseline and reward statistics and
    // leaves the other three untouched. What to run after correcting a reward
    // the policy has already converged against: see AISystem::resetModuleHead.
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--reset-ai-head") != 0) continue;
        if (i + 2 >= argc) {
            fprintf(stderr, "--reset-ai-head needs a model path and a module name\n");
            return 2;
        }
        // Index 4 = AISystem::MOD_COUNT is the diplomacy head and 5 is the
        // stance head, which is how resetModuleHead reads them. Both kept after
        // the four modules so those keep their MOD_* values.
        static const char* NAMES[] = {"econ", "politics", "war", "navy",
                                      "diplo", "stance"};
        int mod = -1;
        for (int m = 0; m < 6; ++m)
            if (strcmp(argv[i + 2], NAMES[m]) == 0) { mod = m; break; }
        if (mod < 0) {
            fprintf(stderr, "--reset-ai-head: module must be one of "
                            "econ, politics, war, navy, diplo, stance\n");
            return 2;
        }
        return AISystem::resetModuleHead(argv[i + 1], mod) ? 0 : 1;
    }

    // --simulate <map.odmap> <turns> [world name]
    // Unattended self-play on a shipped scenario. Leaves behind an .odsv with a
    // real turn history -- what --export-timelapse needs, and what a fresh
    // world does not have. Doubles as the per-platform smoke test: a build that
    // loads a map, resolves turns and writes a save is a build that runs.
    //
    // The trained model is opened read-only. A smoke test that ran on every
    // platform and quietly rewrote data/ai/model.bin each time would be a way
    // to lose a model, not a way to check a build.
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--simulate") != 0) continue;
        if (i + 2 >= argc) {
            fprintf(stderr, "--simulate needs a map path and a turn count\n");
            return 2;
        }
        AISystem::s_readOnlyModel = true;
        Audio::s_disabled = true;
        std::string map = argv[i + 1];
        int turns = atoi(argv[i + 2]);
        if (turns < 1) {
            fprintf(stderr, "--simulate: turn count must be at least 1\n");
            return 2;
        }
        std::string world = (i + 3 < argc && strncmp(argv[i + 3], "--", 2) != 0)
                                ? argv[i + 3] : "Simulated";
        Game g;
        if (!g.init(1600, 900, "OpenDoctrines — simulating")) return 1;
        return g.runHeadlessSimulation(map, turns, world) ? 0 : 1;
    }

    // --measure-text <jobs.tsv> <out.tsv>
    //
    // Measure strings the way the GAME measures them, in whatever language is
    // asked for, and write the widths out. It exists because nothing outside
    // the game can do this honestly: a translation with non-Latin characters
    // is measured against the per-language glyph atlas, a pure-ASCII one goes
    // to raylib's own variable-width font, and Urdu goes through HarfBuzz --
    // three different answers, none of them a character count. An offline
    // model of that was wrong by up to 17% on the first strings it was checked
    // against, which is the whole margin a button has.
    //
    // Input:  <language>\t<fontSize>\t<text>
    // Output: <language>\t<fontSize>\t<width>\t<text>
    //
    // tools/i18n_fit.py drives it. Needs a window, because the atlas it
    // measures against is a texture.
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--measure-text") != 0) continue;
        if (i + 2 >= argc) {
            fprintf(stderr, "--measure-text needs an input and an output path\n");
            return 2;
        }
        Audio::s_disabled = true;
        Game g;
        if (!g.init(1600, 900, "OpenDoctrines — measuring")) return 1;
        return g.measureTextJobs(argv[i + 1], argv[i + 2]) ? 0 : 1;
    }

    // --screenshots <dir> [save.odsv]
    // Walks the game through a fixed list of screens and writes a PNG of each,
    // so the images in README.md and on the store page can be retaken with one
    // command instead of by hand. Needs a window: these are real frames.
    std::string shotDir, shotSave;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--screenshots") != 0) continue;
        if (i + 1 >= argc) {
            fprintf(stderr, "--screenshots needs an output directory\n");
            return 2;
        }
        shotDir = argv[i + 1];
        if (i + 2 < argc && strncmp(argv[i + 2], "--", 2) != 0) shotSave = argv[i + 2];
        break;
    }

    Game game;
    if (!game.init(1600, 900, "OpenDoctrines")) {
        return 1;
    }

    // --resource-limit <percent>: cap this session at a share of the machine,
    // for the length of this run only.
    //
    // The setting already exists (Settings > Display, and the F10 / Ctrl+L
    // panel) and is persisted in config.json. What did not exist was a way to
    // ask for it on the command line, which is exactly what an overnight
    // training run needs: leaving the machine usable is a property of THIS
    // invocation, not a preference to be written back and silently inherited by
    // the next normal game. Applied without saving for that reason.
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--resource-limit") != 0) continue;
        const double pct = atof(argv[i + 1]);
        if (pct <= 0.0) {
            fprintf(stderr, "--resource-limit takes a percentage, e.g. 90\n");
            return 2;
        }
        // Accept "90" and "0.9" as the same thing: the panel reads in percent,
        // the config file stores a fraction, and both spellings get typed.
        game.setSessionResourceLimit((float)(pct > 1.0 ? pct / 100.0 : pct));
    }
    if (!shotDir.empty()) {
        game.beginScreenshotTour(shotDir, shotSave);
        game.run();
        return 0;
    }

    // --tutorial-walk
    // Plays every route of the tutorial, page by page, and reports every page
    // that points at nothing, waits on a condition that never comes true, or
    // offers a choice that opens a script which is not there. Needs a window
    // for the same reason the tour does: the checks are about what was drawn.
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--tutorial-walk") != 0) continue;
        game.beginTutorialWalk();
        game.run();
        return game.tutorialWalkProblems() == 0 ? 0 : 1;
    }

    // --bench-play <map:ISO> [turns]
    //
    // Puts a PERSON on one of the benchmark seats the AI is scored on: same map,
    // same country, every other country played by the frozen scripted rung, and
    // the run stops and prints its score on the same turn the model's did. That
    // last part is the whole point -- a human number and a model number on one
    // scale, so "how good is the AI" has an answer somebody can check.
    //
    //   tools/od_bench.py --list-seats     what the seats are
    //   OpenDoctrines --bench-play 1939:NOR
    //   OpenDoctrines --bench-play 1939:NOR:rush
    //
    // A normal window, because a person is playing it.
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--bench-play") != 0) continue;
        if (i + 1 >= argc || strncmp(argv[i + 1], "--", 2) == 0) {
            fprintf(stderr, "--bench-play needs a seat, e.g. 1939:NOR "
                            "or 1939:NOR:rush\n");
            return 2;
        }
        int untilTurn = 120;   // must match tools/od_bench.py TURNS
        if (i + 2 < argc && argv[i + 2][0] >= '0' && argv[i + 2][0] <= '9')
            untilTurn = atoi(argv[i + 2]);
        game.startBenchSeat(argv[i + 1], untilTurn);
        game.run();
        return 0;
    }

    // --bench-agent <seat> <save.odsv> [--do e:1,w:2] [--until N]
    //
    // A benchmark seat played by hand, a turn per invocation, against the same
    // scripted world the model faces. Prints the position and the legal actions
    // and stops; run it again with --do to take them. See Game::runBenchAgent.
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--bench-agent") != 0) continue;
        if (i + 2 >= argc) {
            fprintf(stderr, "--bench-agent needs a seat and a command FIFO, e.g. "
                            "mkfifo /tmp/od.fifo && --bench-agent 1914:FRA:rush /tmp/od.fifo\n");
            return 2;
        }
        AISystem::s_readOnlyModel = true;
        Audio::s_disabled = true;
        const std::string seat = argv[i + 1];
        const std::string save = argv[i + 2];
        int until = 120;
        unsigned int seed = 20260801u;   // the seat bench's first seed
        for (int k = 1; k < argc - 1; ++k) {
            if (strcmp(argv[k], "--until") == 0) until = atoi(argv[k + 1]);
            if (strcmp(argv[k], "--seed") == 0)
                seed = (unsigned int)strtoul(argv[k + 1], nullptr, 10);
        }
        Game g;
        if (!g.init(1600, 900, "OpenDoctrines — bench agent")) return 1;
        return g.runBenchAgent(seat, save, seed, until) ? 0 : 1;
    }

    // Headless AI self-play training:
    //   OpenDoctrines --train-ai [maps] [turnsPerMap] [countries] [seed]
    //   maps 0 (default) = train forever until the window is closed
    //   countries 0 (default) = each scenario picks its own country count
    // Rotates through scenario archetypes (pangaea, islands, crowded, duel…)
    // with jittered parameters so the shared model learns strategy instead of
    // memorising one geography. Model persists in data/ai/model.bin and is
    // picked up automatically by normal games.
    //
    // The mode flag may sit anywhere in the line, so `--resource-limit 90
    // --train-ai 0 10000` works. Its positional arguments are the words that
    // FOLLOW it, up to the next flag — reading them from fixed argv slots meant
    // the mode had to be argv[1], and putting anything before it silently
    // turned training off while looking like it had worked.
    auto positionals = [&](const char* flag) {
        std::vector<const char*> out;
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], flag) != 0) continue;
            for (int k = i + 1; k < argc && strncmp(argv[k], "--", 2) != 0; ++k)
                out.push_back(argv[k]);
            out.insert(out.begin(), argv[i]); // marker: the flag was present
            break;
        }
        return out;
    };
    auto argAt = [](const std::vector<const char*>& v, size_t n) -> const char* {
        return n + 1 < v.size() ? v[n + 1] : nullptr;   // v[0] is the flag itself
    };

    if (const auto tv = positionals("--train-ai"); !tv.empty()) {
        const char* a1 = argAt(tv, 0);
        const char* a2 = argAt(tv, 1);
        const char* a3 = argAt(tv, 2);
        const char* a4 = argAt(tv, 3);
        int maps      = a1 ? atoi(a1) : 0;
        // Per-map turn cap. Maps normally rotate earlier — when decided (one
        // country left) or strategically frozen (no real conquest for
        // ~1500 turns). This cap is the hard ceiling: with the stagnation
        // window at 1500, a typical map fights ~1500-3000 turns before
        // rotating — long enough to play out mid/late-game naval & research
        // arcs, short enough to keep rotating geographies so the shared model
        // learns strategy rather than memorising one map.
        int turns     = a2 ? atoi(a2) : 3000;
        int countries = a3 ? atoi(a3) : 0;
        unsigned seed = a4 ? (unsigned)strtoul(a4, nullptr, 10)
                           : (unsigned)time(nullptr);
        if (turns < 1) turns = 3000;
        // --worker <id> --workers <n>: one process of a parallel pool. Each
        // gets its own model file and periodically averages toward its peers.
        // See tools/train_parallel.py, which launches a pool and merges after.
        int workerId = -1, workerCount = 0;
        for (int i = 1; i + 1 < argc; ++i) {
            if (strcmp(argv[i], "--worker") == 0)  workerId = atoi(argv[i + 1]);
            if (strcmp(argv[i], "--workers") == 0) workerCount = atoi(argv[i + 1]);
        }
        if (workerCount > 1) game.setAIWorker(workerId, workerCount);
        game.runAITraining(maps, turns, countries, seed);
        return 0;
    }

    // Headless AI measurement:
    //   OpenDoctrines --eval-ai [maps] [turnsPerMap] [seed] [difficulty]
    //   maps       default 8 — one per scenario archetype, so a run covers
    //              pangaea through cold war exactly once
    //   seed       default is a CONSTANT, not the clock: map N must be the same
    //              world in every run or two results are not comparable
    //   difficulty 0 easy, 1 normal, 2 hard (default), 3 insane/argmax
    //   --vs-random  half the countries play uniformly at random instead of
    //                from the model, matched by starting size, so the report
    //                can answer whether the model beats a coin flip
    //   --vs-model <path>
    //                the same split, but that half plays the named model file
    //                instead of dice. Random never improves, so it can only
    //                ever answer "better than nothing"; a named opponent is a
    //                rung, and a model that clears one gets pinned as the next.
    //   --vs-script  that half plays a hand-written competent policy: rung
    //                one, and the first control that stands for a LEVEL rather
    //                than for a floor. Parity with it is the real target.
    //   --script-duel
    //                both halves hand-written: one builds an army and attacks,
    //                the other builds an army and defends. No model is
    //                consulted. Answers whether attacking is worth doing here
    //                without any reward function in the way.
    //   --scenarios  measure on the maps data/STDmaps ships (1914 … modern)
    //                rather than generated archetypes — the worlds a player
    //                actually opens. Not mixed with generated maps in one run.
    // Never writes the model, never writes a save. Safe to run while a
    // --train-ai session is going.
    if (const auto ev = positionals("--eval-ai"); !ev.empty()) {
        const char* a1 = argAt(ev, 0);
        const char* a2 = argAt(ev, 1);
        const char* a3 = argAt(ev, 2);
        const char* a4 = argAt(ev, 3);
        int maps       = a1 ? atoi(a1) : 8;
        int turns      = a2 ? atoi(a2) : 3000;
        unsigned seed  = a3 ? (unsigned)strtoul(a3, nullptr, 10) : 20260801u;
        int difficulty = a4 ? atoi(a4) : 2;
        if (maps < 1) maps = 8;
        if (turns < 1) turns = 3000;
        // --vs-random: split each map into a model cohort and a coin-flip
        // cohort and report which one ends up holding the world. A separate
        // flag rather than a fifth positional, because it changes what the run
        // MEANS and should be readable at a glance in a shell history.
        bool vsRandom = false, scenarios = false;
        std::string vsModel;
        for (int i = 1; i < argc; ++i) {
            // --bench-seat <ISO>: the ABSOLUTE score. One country is played by
            // the model and every other country in the world plays the frozen
            // scripted rung, so the result does not depend on what it was
            // measured against -- unlike ADVANTAGE, --vs-model and the ordinary
            // cohort split, all of which move when the opponent moves. The same
            // seat can be played by a person; see tools/od_bench.py.
            if (strcmp(argv[i], "--bench-seat") == 0 && i + 1 < argc) {
                game.setBenchSeat(argv[i + 1]);
                AISystem::s_scriptedControl = true;
            }
            // --rush-neighbours: with --bench-seat and --vs-exploit, only the
            // seat's land neighbours play the exploit. See s_exploitCids.
            // --rush-neighbours [N]: only the seat's N largest land neighbours
            // play the exploit (default 1; "all" for every neighbour). See the
            // note in Game_AITrain -- all of them at once is HARDER than a
            // world-wide rush, not softer.
            if (strcmp(argv[i], "--rush-neighbours") == 0) {
                int howMany = 1;
                if (i + 1 < argc && strcmp(argv[i + 1], "all") == 0) howMany = -1;
                else if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                    howMany = atoi(argv[i + 1]);
                game.setBenchRushNeighbours(howMany);
            }
            if (strcmp(argv[i], "--vs-random") == 0) vsRandom = true;
            if (strcmp(argv[i], "--scenarios") == 0) scenarios = true;
            if (strcmp(argv[i], "--vs-script") == 0)
                AISystem::s_scriptedControl = true;
            // --vs-exploit <n>: the control cohort plays one of the hand-written
            // EXPLOITS instead of a yardstick -- a one-note human strategy
            // played every turn without deviation. See AISystem::ScriptVariant.
            // 2 tech, 3 blitz, 4 diplomacy hub, 5 sea power.
            if (strcmp(argv[i], "--vs-exploit") == 0 && i + 1 < argc) {
                AISystem::s_scriptedControl = true;
                AISystem::s_exploitVariant =
                    std::clamp(atoi(argv[i + 1]), 2,
                               (int)AISystem::SCRIPT_VARIANT_COUNT - 1);
            }
            // Script against script: no network anywhere. The MODEL cohort
            // turtles, the control attacks, and the only question on the table
            // is whether aggression pays in this game at all.
            if (strcmp(argv[i], "--script-duel") == 0) {
                AISystem::s_scriptedControl = true;
                AISystem::s_scriptDuel = true;
            }
            if (strcmp(argv[i], "--vs-model") == 0) {
                if (i + 1 >= argc || strncmp(argv[i + 1], "--", 2) == 0) {
                    fprintf(stderr, "--vs-model needs a path to a model file\n");
                    return 2;
                }
                vsModel = argv[++i];
            }
        }
        // Both would name two different control groups for one cohort. Picking
        // either silently would make the report's heading a coin toss over what
        // the reader typed, so say so and stop.
        // Three ways to name a control group; naming two is naming neither.
        const int controls = (vsRandom ? 1 : 0) + (vsModel.empty() ? 0 : 1) +
                             (AISystem::s_scriptedControl ? 1 : 0);
        if (controls > 1) {
            fprintf(stderr, "--vs-random, --vs-model and --vs-script name "
                            "different control groups; choose one\n");
            return 2;
        }
        return game.runAIEvaluation(maps, turns, seed, difficulty, vsRandom,
                                    vsModel, scenarios) ? 0 : 1;
    }

    // If save file provided as argument, load it. Skip flags so
    // `--ai-readonly` on its own isn't mistaken for a save path — and skip the
    // VALUE of a flag that takes one, or `--resource-limit 90` would send the
    // loader looking for a save file called "90".
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--", 2) == 0) {
            if (strcmp(argv[i], "--resource-limit") == 0 ||
                strcmp(argv[i], "--worker") == 0 ||
                strcmp(argv[i], "--vs-model") == 0 ||
                strcmp(argv[i], "--workers") == 0) ++i;
            continue;
        }
        game.loadSaveAndStart(std::string(argv[i]));
        break;
    }

    // --tutorial: in at the deep end, exactly as clicking the menu's "?"
    // does. After the save-path loop above, so it wins over a save named on
    // the same line rather than racing it.
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--tutorial") == 0) { game.startTutorial(); break; }

    game.run();
    return 0;
}
