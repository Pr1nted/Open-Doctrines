#include "Game.h"
#include "Audio.h"
#include "ai/AISystem.h"
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <iostream>

int main(int argc, char** argv) {
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

    // Training has nobody listening. This has to be decided before init(),
    // which is where the device would otherwise be opened -- and the headless
    // machines that run training are the ones least likely to have one.
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--train-ai") == 0) Audio::s_disabled = true;

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
        bool ok = g.exportTimelapseHeadless(save, out, w, h, 6, view);
        return ok ? 0 : 1;
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
    if (!shotDir.empty()) {
        game.beginScreenshotTour(shotDir, shotSave);
        game.run();
        return 0;
    }

    // Headless AI self-play training:
    //   OpenDoctrines --train-ai [maps] [turnsPerMap] [countries] [seed]
    //   maps 0 (default) = train forever until the window is closed
    //   countries 0 (default) = each scenario picks its own country count
    // Rotates through scenario archetypes (pangaea, islands, crowded, duel…)
    // with jittered parameters so the shared model learns strategy instead of
    // memorising one geography. Model persists in data/ai/model.bin and is
    // picked up automatically by normal games.
    if (argc > 1 && strcmp(argv[1], "--train-ai") == 0) {
        int maps      = argc > 2 ? atoi(argv[2]) : 0;
        // Per-map turn cap. Maps normally rotate earlier — when decided (one
        // country left) or strategically frozen (no real conquest for
        // ~1500 turns). This cap is the hard ceiling: with the stagnation
        // window at 1500, a typical map fights ~1500-3000 turns before
        // rotating — long enough to play out mid/late-game naval & research
        // arcs, short enough to keep rotating geographies so the shared model
        // learns strategy rather than memorising one map.
        int turns     = argc > 3 ? atoi(argv[3]) : 3000;
        int countries = argc > 4 ? atoi(argv[4]) : 0;
        unsigned seed = argc > 5 ? (unsigned)strtoul(argv[5], nullptr, 10)
                                 : (unsigned)time(nullptr);
        if (turns < 1) turns = 3000;
        game.runAITraining(maps, turns, countries, seed);
        return 0;
    }

    // If save file provided as argument, load it. Skip flags so
    // `--ai-readonly` on its own isn't mistaken for a save path.
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--", 2) == 0) continue;
        game.loadSaveAndStart(std::string(argv[i]));
        break;
    }

    game.run();
    return 0;
}
