#include "Game.h"
#include <iostream>

int main(int argc, char** argv) {
    Game game;
    if (!game.init(1600, 900, "OpenDoctrines")) {
        return 1;
    }
    
    // If save file provided as argument, load it
    if (argc > 1) {
        std::string savePath = argv[1];
        game.loadSaveAndStart(savePath);
    }
    
    game.run();
    return 0;
}
