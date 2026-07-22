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

    int keybinds[ACTION_COUNT];

    int accentColor = 0xFFD700; // default gold, hex 0xRRGGBB

    Config() {
        for (int i = 0; i < ACTION_COUNT; ++i)
            keybinds[i] = DEFAULT_KEYBINDS[i];
    }

    bool load(const std::string& path);
    bool save(const std::string& path);
};
