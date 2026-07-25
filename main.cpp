#include "Game.h"
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <iostream>

int main(int argc, char** argv) {
    Game game;
    if (!game.init(1600, 900, "OpenDoctrines")) {
        return 1;
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

    // If save file provided as argument, load it
    if (argc > 1) {
        std::string savePath = argv[1];
        game.loadSaveAndStart(savePath);
    }

    game.run();
    return 0;
}
