#include <string>
#include "Keybinds.h"

struct Color;

struct Config {
    float flySpeed = 2.0f;
    float maxZoom = 5.0f;
    int screenW = 1600;
    int screenH = 900;
    bool fullscreen = false;
    bool showActualFlags = true;
    bool debugMode = false;
    bool showFps = false;
    bool showZoom = false;
    bool showConsole = false;
    int fpsTarget = 0; // -1=Unlimited, 0=VSync, 10-120=capped
    // AI difficulty: 0=Easy 1=Normal 2=Hard 3=Insane. One shared model —
    // difficulty only changes how deterministically countries follow it.
    int aiDifficulty = 1;
    bool aiDebug = false;   // log AI decisions + enable the in-game AI overlay
    // Online reinforcement learning during a normal session (Experimental tab).
    // Off by default: it costs time on every AI decision and writes
    // data/ai/model.bin, so a single play session can overwrite progress that a
    // long self-play training run has accumulated in the shared model.
    bool aiLearning = false;

    int keybinds[ACTION_COUNT];

    int accentColor = 0xFFD700; // default gold, hex 0xRRGGBB

    Config() {
        for (int i = 0; i < ACTION_COUNT; ++i)
            keybinds[i] = DEFAULT_KEYBINDS[i];
    }

    bool load(const std::string& path);
    bool save(const std::string& path);
};
