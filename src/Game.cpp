#include "GameUpdates.h"
#include "Game.h"
#include "Audio.h"
#include "net/AccountClient.h"
#include "SaveManager.h"
#include "Keybinds.h"
#include "ai/AISystem.h"
#include "renderer/FlagRenderer.h"
#include "miniz.h"
#include "miniz_zip.h"
#include "raymath.h"
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_set>
#ifdef _WIN32
#include <direct.h>
#endif
#include <sys/stat.h>
#include <ctime>
#include <random>
#include <thread>
#include <deque>
#ifdef _WIN32
// windows.h IS needed here -- samplePerformance() uses FILETIME,
// GetProcessTimes, GetCurrentProcess and ULARGE_INTEGER to measure CPU time --
// but it must not be included raw. NOGDI and NOUSER are what make it safe:
//
//   wingdi.h  declares a function named Rectangle, which collides with
//             raylib's Rectangle STRUCT -- "redefinition; different type
//             modifiers", on a file that is mostly Rectangles.
//   winuser.h defines DrawText as a MACRO expanding to DrawTextA, so every
//             raylib DrawText() call became a call to a GDI text routine:
//             "cannot convert argument 1 from 'const char *' to 'HDC'".
//   NOMINMAX  stops min/max becoming macros and breaking std::min.
//
// Everything this file wants (FILETIME, the process-time calls) lives in the
// kernel32 headers, which NOGDI/NOUSER do not touch. Deleting the include
// instead was the wrong fix and broke the Windows build a different way.
#define NOGDI
#define NOUSER
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <sys/resource.h>
#endif
#include "GameInternals.h"

std::string formatPop(long long pop) {
    struct Step { long long div; const char* suffix; };
    static const Step steps[] = {{1000000000000LL, "t"}, {1000000000, "b"}, {1000000, "m"}, {1000, "k"}};
    for (auto& s : steps) {
        if (pop >= s.div) {
            double val = (double)pop / s.div;
            if (val == (long long)val) return std::to_string((long long)val) + s.suffix;
            char buf[32]; snprintf(buf, sizeof(buf), "%.1f%s", val, s.suffix); return buf;
        }
    }
    return std::to_string(pop);
}

const char* MENU_ITEMS[] = {"Continue", "Settings", "Save", "Quit to Menu"};
const int MENU_COUNT = 4;
const char* MAIN_MENU_ITEMS[] = {"Play Singleplayer", "Play Multiplayer", "Map Editor", "Mod Menu", "Community", "Account", "Credits"};
const int MAIN_MENU_COUNT = 7;
const char* SINGLEPLAYER_ITEMS[] = {"New World", "Load World"};
const int SINGLEPLAYER_COUNT = 2;

// Game version: major.minor.patch + state (a=alpha, b=beta, r=release, s=snapshot).
//
// Injected by CMake from the ./VERSION file, which tools/release.py bumps. Do
// not hardcode it here: the displayed version and the packaged version must be
// the same string, and they were not while this was a separate literal.
#ifndef OD_VERSION_STRING
#define OD_VERSION_STRING "0.0.0-dev"   // building without CMake, e.g. a stray IDE target
#endif
const char* GAME_VERSION = OD_VERSION_STRING;

const char* TAB_NAMES[] = {"Display", "Controls", "Audio", "Keybinds", "Advanced", "Experimental"};
const int TAB_COUNT = 6;

const int RESOLUTIONS[][2] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}};
const int RES_COUNT = 5;

// The resource limiter deliberately does NOT live here. It is a control you
// want while something is running -- watching the CPU graph react as you drag
// it -- not a preference you set once in a menu. F10 / Ctrl+L opens it in
// place, in game and in the trainer alike.
const Setting DISPLAY_ITEMS[] = {{"Fullscreen", false, -1}, {"Show Actual Flags", false, -1}, {"Debug Mode", false, -1}, {"Max Zoom", true, -1}, {"Resolution", false, -1}, {"FPS", false, -1}, {"Accent Color", false, -1}, {"AI Difficulty", false, -1}, {"Back", false, -1}};
const int DISPLAY_COUNT = 9;

const char* AI_DIFFICULTY_NAMES[] = {"Easy", "Normal", "Hard", "Insane"};
const int AI_DIFFICULTY_COUNT = 4;

const int ACCENT_PRESETS[] = {
    0xFFD700, // Gold
    0x0096FF, // Blue
    0x00FF64, // Green
    0xFF3232, // Red
    0xC832FF, // Purple
    0xFF9600, // Orange
    0xFF6496, // Pink
    0x64FFFF, // Cyan
    0xFFFFFF, // White
    0xC8C8C8, // Silver
};
const int ACCENT_PRESETS_COUNT = 10;
const Setting CONTROLS_ITEMS[] = {{"Fly Speed", true, -1}, {"Back", false, -1}};
const int CONTROLS_COUNT = 2;
// isValue stays false on the volume rows even though they hold numbers. That
// flag routes a click into the type-a-number editor, which is right for Max
// Zoom and wrong here: a volume is dragged or nudged, never typed. The two
// settings screens special-case these rows via isVolumeSetting() instead.
const Setting AUDIO_ITEMS[] = {
    {"Master Volume", false, -1},
    {"Music Volume", false, -1},
    {"Sound Effects", false, -1},
    {"Now Playing Toast", false, -1},
    {"Map Atmosphere", false, -1},
    {"Back", false, -1},
};
const int AUDIO_COUNT = 6;

// Defaults live here rather than in Config so the reset button and a fresh
// config cannot drift apart.
const float VOLUME_DEFAULTS[] = {0.8f, 0.6f, 0.8f};

bool isVolumeSetting(int tab, int index) {
    return tab == AUDIO_TAB && index >= 0 && index < VOLUME_COUNT;
}

float* volumeSettingPtr(Config& cfg, int tab, int index) {
    if (!isVolumeSetting(tab, index)) return nullptr;
    switch (index) {
        case 0:  return &cfg.masterVolume;
        case 1:  return &cfg.musicVolume;
        default: return &cfg.sfxVolume;
    }
}

void applyVolumes(const Config& cfg) {
    Audio::get().setMasterVolume(cfg.masterVolume);
    Audio::get().setMusicVolume(cfg.musicVolume);
    Audio::get().setSfxVolume(cfg.sfxVolume);
    Audio::get().setMapAtmosphere(cfg.mapAtmosphere);
}

// Keybinds items — isValue=false; handled specially via m_waitingForKey
const Setting KEYBINDS_ITEMS[] = {
    {"-- Navigation --", false, -1},
    {ACTION_NAMES[0], false, 0},
    {ACTION_NAMES[1], false, 1},
    {ACTION_NAMES[2], false, 2},
    {ACTION_NAMES[3], false, 3},
    {ACTION_NAMES[4], false, 4},
    {"-- Selection --", false, -1},
    {ACTION_NAMES[5], false, 5},
    {"-- Combat --", false, -1},
    {ACTION_NAMES[6], false, 6},
    {ACTION_NAMES[15], false, 15},
    {"-- View Tabs --", false, -1},
    {ACTION_NAMES[7], false, 7},
    {ACTION_NAMES[8], false, 8},
    {ACTION_NAMES[9], false, 9},
    {ACTION_NAMES[10], false, 10},
    {ACTION_NAMES[11], false, 11},
    {ACTION_NAMES[12], false, 12},
    {ACTION_NAMES[13], false, 13},
    {ACTION_NAMES[14], false, 14},
    {"-- Navy --", false, -1},
    {ACTION_NAMES[16], false, 16},
    {ACTION_NAMES[17], false, 17},
    {ACTION_NAMES[18], false, 18},
    {"Back", false, -1},
};
const int KEYBINDS_COUNT = sizeof(KEYBINDS_ITEMS) / sizeof(KEYBINDS_ITEMS[0]);

const Setting ADVANCED_ITEMS[] = {
    {"Display FPS", false, -1},
    {"Display Zoom", false, -1},
    {"Console Window", false, -1},
    {"AI Debug", false, -1},
    // Off by default and left that way unless the player asks. It is the only
    // outbound request the game makes, and it goes to a URL a MOD AUTHOR
    // controls -- turning it on tells that author this player runs their mod.
    // Even on, the game only looks: it never downloads or installs anything.
    {"Check mods for updates", false, -1},
    // On by default. Asks the game's own release host about the game itself,
    // which is a different disclosure from the mod check above.
    {"Check for game updates", false, -1},
    {"Back", false, -1},
};
const int ADVANCED_COUNT = 7;

// Experimental: behaviour that changes how the game plays rather than how it
// looks. "AI Learning" moved here from Advanced and now defaults OFF — it runs
// reinforcement-learning updates on every AI decision during a normal session
// and writes data/ai/model.bin, which costs time and mutates the shared model
// that self-play training is building.
const Setting EXPERIMENTAL_ITEMS[] = {
    {"AI Learning", false, -1},
    {"Back", false, -1},
};
const int EXPERIMENTAL_COUNT = 2;

const Setting* TAB_ITEMS[] = {DISPLAY_ITEMS, CONTROLS_ITEMS, AUDIO_ITEMS, KEYBINDS_ITEMS, ADVANCED_ITEMS, EXPERIMENTAL_ITEMS};
const int TAB_ITEM_COUNTS[] = {DISPLAY_COUNT, CONTROLS_COUNT, AUDIO_COUNT, KEYBINDS_COUNT, ADVANCED_COUNT, EXPERIMENTAL_COUNT};

const char* keyName(int key) {
    if (key == 0) return "\xe2\x80\x94"; // em dash
    switch (key) {
        case KEY_SPACE: return "Space";
        case KEY_ENTER: return "Enter";
        case KEY_ESCAPE: return "Escape";
        case KEY_BACKSPACE: return "Backspace";
        case KEY_DELETE: return "Delete";
        case KEY_TAB: return "Tab";
        case KEY_LEFT: return "Left Arrow";
        case KEY_RIGHT: return "Right Arrow";
        case KEY_UP: return "Up Arrow";
        case KEY_DOWN: return "Down Arrow";
        case KEY_LEFT_SHIFT: case KEY_RIGHT_SHIFT: return "Shift";
        case KEY_LEFT_CONTROL: case KEY_RIGHT_CONTROL: return "Ctrl";
        case KEY_LEFT_ALT: case KEY_RIGHT_ALT: return "Alt";
        case MOUSE_BUTTON_LEFT: return "Left Mouse";
        case MOUSE_BUTTON_RIGHT: return "Right Mouse";
        case MOUSE_BUTTON_MIDDLE: return "Middle Mouse";
        case MOUSE_BUTTON_SIDE: return "Side Mouse";
        case MOUSE_BUTTON_EXTRA: return "Extra Mouse";
        case MOUSE_BUTTON_FORWARD: return "Forward Mouse";
        case MOUSE_BUTTON_BACK: return "Back Mouse";
        case '=': return "=";
        case '-': return "-";
        case '[': return "[";
        case ']': return "]";
        case ';': return ";";
        case '\'': return "'";
        case ',': return ",";
        case '.': return ".";
        case '/': return "/";
        case '\\': return "\\";
        case '`': return "`";
        default:
            if (key >= 32 && key <= 126) {
                static char buf[2];
                buf[0] = (char)key;
                buf[1] = 0;
                return buf;
            }
            return "?";
    }
}

Color hexToColor(int hex) {
    return {
        (unsigned char)((hex >> 16) & 0xFF),
        (unsigned char)((hex >> 8) & 0xFF),
        (unsigned char)(hex & 0xFF),
        255
    };
}

float FLY_SPEED_VALS[] = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
const int FLY_SPEED_COUNT = 6;
float MAX_ZOOM_VALS[] = {2.0f, 3.0f, 5.0f, 8.0f, 10.0f, 15.0f, 20.0f};
const int MAX_ZOOM_COUNT = 7;

// Every *_COUNT above is written by hand, and a settings menu that walks past
// the end of its array reads a row that was never initialised -- which is how
// the Keybinds tab came to dereference a null label. These tie each count to
// the array it counts, so adding a row and forgetting to bump the count is a
// compile error rather than a crash in the menu.
#define OD_COUNT_OF(a) (int)(sizeof(a) / sizeof((a)[0]))
static_assert(MENU_COUNT           == OD_COUNT_OF(MENU_ITEMS),          "MENU_COUNT");
static_assert(MAIN_MENU_COUNT      == OD_COUNT_OF(MAIN_MENU_ITEMS),     "MAIN_MENU_COUNT");
static_assert(SINGLEPLAYER_COUNT   == OD_COUNT_OF(SINGLEPLAYER_ITEMS),  "SINGLEPLAYER_COUNT");
static_assert(TAB_COUNT            == OD_COUNT_OF(TAB_NAMES),           "TAB_COUNT");
static_assert(RES_COUNT            == OD_COUNT_OF(RESOLUTIONS),         "RES_COUNT");
static_assert(DISPLAY_COUNT        == OD_COUNT_OF(DISPLAY_ITEMS),       "DISPLAY_COUNT");
static_assert(AI_DIFFICULTY_COUNT  == OD_COUNT_OF(AI_DIFFICULTY_NAMES), "AI_DIFFICULTY_COUNT");
static_assert(ACCENT_PRESETS_COUNT == OD_COUNT_OF(ACCENT_PRESETS),      "ACCENT_PRESETS_COUNT");
static_assert(CONTROLS_COUNT       == OD_COUNT_OF(CONTROLS_ITEMS),      "CONTROLS_COUNT");
static_assert(AUDIO_COUNT          == OD_COUNT_OF(AUDIO_ITEMS),         "AUDIO_COUNT");
static_assert(VOLUME_COUNT         == OD_COUNT_OF(VOLUME_DEFAULTS),     "VOLUME_DEFAULTS");
// The volume rows are the first VOLUME_COUNT of the Audio tab, and "Back" has
// to stay after them: isVolumeSetting() decides by index alone.
static_assert(VOLUME_COUNT         <  AUDIO_COUNT,                      "Audio tab needs a Back row");
static_assert(KEYBINDS_COUNT       == OD_COUNT_OF(KEYBINDS_ITEMS),      "KEYBINDS_COUNT");
static_assert(ADVANCED_COUNT       == OD_COUNT_OF(ADVANCED_ITEMS),      "ADVANCED_COUNT");
static_assert(EXPERIMENTAL_COUNT   == OD_COUNT_OF(EXPERIMENTAL_ITEMS),  "EXPERIMENTAL_COUNT");
static_assert(FLY_SPEED_COUNT      == OD_COUNT_OF(FLY_SPEED_VALS),      "FLY_SPEED_COUNT");
static_assert(MAX_ZOOM_COUNT       == OD_COUNT_OF(MAX_ZOOM_VALS),       "MAX_ZOOM_COUNT");
static_assert(TAB_COUNT            == OD_COUNT_OF(TAB_ITEMS),           "TAB_ITEMS vs TAB_COUNT");
static_assert(TAB_COUNT            == OD_COUNT_OF(TAB_ITEM_COUNTS),     "TAB_ITEM_COUNTS vs TAB_COUNT");
#undef OD_COUNT_OF

int fpsTargetToIndex(int target) {
    if (target == -1) return 0;
    if (target == 0) return 13;
    if (target >= 10 && target <= 120) return 1 + (target - 10) / 10;
    return 13;
}

int indexToFpsTarget(int idx) {
    if (idx <= 0) return -1;
    if (idx >= 13) return 0;
    return 10 + (idx - 1) * 10;
}

static const char* fpsLabel(int target) {
    if (target == -1) return "Unlimited";
    if (target == 0) return "VSync";
    static char buf[8];
    snprintf(buf, sizeof(buf), "%d", target);
    return buf;
}

// ── Resource limiter ──
// Not a member of Config because applyFpsTarget is a free function that several
// callers reach without a Config in hand (the trainer, the window setup path).
static float g_resourceBudget = 1.0f;

void setResourceBudget(float budget) {
    g_resourceBudget = std::clamp(budget, RESOURCE_BUDGET_MIN, 1.0f);
}
float resourceBudget() { return g_resourceBudget; }

int budgetedFpsCeiling() {
    if (g_resourceBudget >= 0.999f) return 0; // unlimited: don't touch pacing
    int hz = GetMonitorRefreshRate(GetCurrentMonitor());
    if (hz <= 0) hz = 60; // headless / driver won't say
    // Floor of 15: below that the window stops feeling like software, and the
    // limiter is meant to free the machine up, not to break the game.
    return std::max(15, (int)lroundf((float)hz * g_resourceBudget));
}


void applyFpsTarget(int target) {
    ClearWindowState(FLAG_VSYNC_HINT);
    SetTargetFPS(0);
    const int ceiling = budgetedFpsCeiling();
    if (target == 0) {
        // VSync and a frame cap are mutually exclusive here: VSync would block
        // on the swap at the monitor's rate and the cap would never be reached.
        // A budget below 100% is an explicit request to run slower than the
        // display, so it wins.
        if (ceiling > 0) SetTargetFPS(ceiling);
        else SetWindowState(FLAG_VSYNC_HINT);
    } else if (target > 0) {
        SetTargetFPS(ceiling > 0 ? std::min(target, ceiling) : target);
    } else if (ceiling > 0) {
        SetTargetFPS(ceiling); // "unlimited" still respects the budget
    }
    // target == -1 with no budget = unlimited, already set above
}

int nearestIndex(float val, float* vals, int count) {
    int best = 0;
    float bestDist = fabsf(val - vals[0]);
    for (int i = 1; i < count; ++i) {
        float d = fabsf(val - vals[i]);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

std::string makeSettingLabel(int tab, int index, const Config& cfg) {
    const Setting& s = TAB_ITEMS[tab][index];
    std::string label = s.label;
    if (tab == 0 && index == 0) {
        label += cfg.fullscreen ? ": On" : ": Off";
    } else if (tab == 0 && index == 1) {
        label += cfg.showActualFlags ? ": On" : ": Off";
    } else if (tab == 0 && index == 2) {
        label += cfg.debugMode ? ": On" : ": Off";
    } else if (tab == 0 && index == 3) {
        char b[64]; snprintf(b, sizeof(b), ": %.1f", cfg.maxZoom); label += b;
    } else if (tab == 0 && index == 4) {
        char b[64]; snprintf(b, sizeof(b), ": %dx%d", cfg.screenW, cfg.screenH); label += b;
    } else if (tab == 0 && index == 5) {
        label += std::string(": ") + fpsLabel(cfg.fpsTarget);
    } else if (tab == 0 && index == 6) {
        char b[16]; snprintf(b, sizeof(b), ": #%06X", cfg.accentColor); label += b;
    } else if (tab == 0 && index == 7) {
        int d = cfg.aiDifficulty < 0 ? 0 : (cfg.aiDifficulty >= AI_DIFFICULTY_COUNT ? AI_DIFFICULTY_COUNT - 1 : cfg.aiDifficulty);
        label += std::string(": ") + AI_DIFFICULTY_NAMES[d];
    } else if (tab == 1 && index == 0) {
        char b[64]; snprintf(b, sizeof(b), ": %.1f", cfg.flySpeed); label += b;
    } else if (isVolumeSetting(tab, index)) {
        const float* v = volumeSettingPtr(const_cast<Config&>(cfg), tab, index);
        char b[16]; snprintf(b, sizeof(b), ": %d%%", (int)lroundf(*v * 100.0f)); label += b;
    } else if (tab == AUDIO_TAB && index == VOLUME_COUNT) {
        label += cfg.nowPlayingToast ? ": On" : ": Off";
    } else if (tab == AUDIO_TAB && index == VOLUME_COUNT + 1) {
        label += cfg.mapAtmosphere ? ": On" : ": Off";
    } else if (tab == 4 && index == 0) {
        label += cfg.showFps ? ": On" : ": Off";
    } else if (tab == 4 && index == 1) {
        label += cfg.showZoom ? ": On" : ": Off";
    } else if (tab == 4 && index == 2) {
        label += cfg.showConsole ? ": On" : ": Off";
    } else if (tab == 4 && index == 3) {
        label += cfg.aiDebug ? ": On" : ": Off";
    } else if (tab == 4 && index == 4) {
        label += cfg.modUpdateChecks ? ": On" : ": Off";
    } else if (tab == 4 && index == 5) {
        label += cfg.gameUpdateChecks ? ": On" : ": Off";
    } else if (tab == 5 && index == 0) {
        label += cfg.aiLearning ? ": On" : ": Off";
    } else if (tab == 3 && s.actionId >= 0) {
        label += std::string(": ") + keyName(cfg.keybinds[s.actionId]);
    }
    return label;
}

template<typename F>
static Texture2D makeIcon(int w, int h, F drawFn) {
    Image img = GenImageColor(w, h, {0, 0, 0, 0});
    drawFn(img);
    Texture2D tex = LoadTextureFromImage(img);
    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    UnloadImage(img);
    return tex;
}

void forceWindowResize(int w, int h) {
#ifndef __EMSCRIPTEN__
    SetWindowSize(w, h);
    SetWindowSize(w, h);
    PollInputEvents();
    int mon = GetCurrentMonitor();
    int mw = GetMonitorWidth(mon);
    int mh = GetMonitorHeight(mon);
    SetWindowPosition((mw - w) / 2, (mh - h) / 2);
#endif
}

void setFullscreenAttrs(bool fullscreen, int* x, int* y, int* w, int* h) {
    if (fullscreen) {
        SetWindowState(FLAG_WINDOW_UNDECORATED);
        int mon = GetCurrentMonitor();
        *w = GetMonitorWidth(mon);
        *h = GetMonitorHeight(mon);
        SetWindowSize(*w, *h);
        SetWindowPosition(0, 0);
    } else {
        ClearWindowState(FLAG_WINDOW_UNDECORATED);
        SetWindowSize(*w, *h);
        if (*x == 0 && *y == 0) {
            int mon = GetCurrentMonitor();
            *x = (GetMonitorWidth(mon) - *w) / 2;
            *y = (GetMonitorHeight(mon) - *h) / 2;
        }
        SetWindowPosition(*x, *y);
    }
}

static Color blendColor(Color base, float t) {
    uint8_t r = (uint8_t)std::min(255, std::max(0, (int)(base.r * (1.0f - t * 0.4f) + 40.0f * t * 0.3f)));
    uint8_t g = (uint8_t)std::min(255, std::max(0, (int)(base.g * (1.0f - t * 0.4f) + 40.0f * t * 0.3f)));
    uint8_t b = (uint8_t)std::min(255, std::max(0, (int)(base.b * (1.0f - t * 0.4f) + 40.0f * t * 0.3f)));
    return {r, g, b, 255};
}

std::string Game::formatBalance(float val) {
    if (fabsf(val) >= 1000000000.0f)
        return TextFormat("%.2fb", val / 1000000000.0f);
    if (fabsf(val) >= 1000000.0f)
        return TextFormat("%.2fm", val / 1000000.0f);
    if (fabsf(val) >= 1000.0f)
        return TextFormat("%.2fk", val / 1000.0f);
    return TextFormat("%.2f", val);
}

Game::Game() = default;

Game::~Game() {
    shutdown();
}

static Texture2D loadBorderTexture(const std::string& path) {
    Texture2D tex{};
    if (FileExists(path.c_str())) {
        Image img = LoadImage(path.c_str());
        if (img.data) {
            tex = LoadTextureFromImage(img);
            UnloadImage(img);
        }
    }
    return tex;
}

static Texture2D loadBorderTextureFromMemory(const void* data, int size) {
    Texture2D tex{};
    Image img = LoadImageFromMemory(".png", static_cast<const unsigned char*>(data), size);
    if (img.data) {
        tex = LoadTextureFromImage(img);
        UnloadImage(img);
    }
    return tex;
}

static void unloadBorderTexture(Texture2D& tex) {
    if (tex.id > 0) {
        UnloadTexture(tex);
        tex = {};
    }
}

bool Game::init(int screenW, int screenH, const char* title) {
#ifdef __EMSCRIPTEN__
    // On Emscripten, GetApplicationDirectory() returns a URL, not a filesystem path.
    // Data is preloaded into the virtual FS at /data/ via --preload-file.
    m_dataDir = "/data/";
#else
    std::string appDir = GetApplicationDirectory();
    m_dataDir = appDir + "../data/";

    // A second copy of the game on the same machine needs its own account,
    // config and saves -- otherwise the two instances fight over one
    // account.json and the second sign-in evicts the first. Testing
    // multiplayer alone is the whole reason this exists.
    if (const char* over = getenv("OD_DATA_DIR")) {
        std::string d = over;
        if (!d.empty()) {
            if (d.back() != '/') d += '/';
            m_dataDir = d;
            std::cout << "Data directory overridden: " << m_dataDir << std::endl;
        }
    }
#endif
    m_configPath = m_dataDir + "config.json";

    // Tell the updater where the game actually lives before it does anything.
    // GetApplicationDirectory() is the executable's own directory, which is
    // what an update must replace; the working directory can be anywhere when
    // the game is launched by double-clicking it.
    GameUpdates::setInstallDir(GetApplicationDirectory());

    // Sweep up after any update that installed on the previous run: the
    // staging directory and the binary that was displaced to make room. Both
    // are safe to remove now, because nothing is running from them.
    GameUpdates::cleanUpAfterUpdate();
    m_config.load(m_configPath);

    // Mods are scanned here but never started: instantiation only ever happens
    // from the mod menu (docs/modding.md, "Lifecycle rules").
    initModSystem();

    srand((unsigned int)time(nullptr));

    m_screenW = m_config.screenW;
    m_screenH = m_config.screenH;

    int winFlags = FLAG_WINDOW_RESIZABLE;
    if (m_config.fpsTarget == 0) winFlags |= FLAG_VSYNC_HINT;
    SetConfigFlags(winFlags);
    InitWindow(m_screenW, m_screenH, title);

    // InitWindow cannot fail loudly -- it returns void, logs a warning, and
    // leaves the process running with no window and no GL context. Everything
    // after this point then dereferences state that was never created, so the
    // first symptom is a SEGMENTATION FAULT several calls later with nothing
    // in it that names the real problem.
    //
    // This is not only a CI concern. A machine with no GPU driver, a broken
    // one, an unsupported OpenGL version, or a remote session with no
    // compositor all land here, and "it crashes on launch" is the worst
    // possible way to tell somebody their graphics stack cannot run the game.
    if (!IsWindowReady()) {
        std::cerr
            << "OpenDoctrines could not open a window.\n"
               "  The graphics driver did not provide an OpenGL 3.3 context. "
               "That usually means\n"
               "  no GPU driver, a headless session with no display, or a "
               "remote desktop that does\n"
               "  not forward OpenGL. The raylib/GLFW warning above says which.\n";
        return false;
    }
    SetExitKey(0);
    m_dpiScale = GetWindowScaleDPI().x;

    // Redirect stdout/stderr to in-game console
    m_consoleBuf = new ConsoleBuf(this);
    m_origCout = std::cout.rdbuf(m_consoleBuf);
    m_origCerr = std::cerr.rdbuf(m_consoleBuf);

    // Audio comes up here rather than at the end of init(): it depends on
    // nothing below -- not the map, not the fonts -- and anything down there
    // that fails would otherwise take the sound down with it. After the console
    // redirect, so the device report lands in the in-game console too. A
    // machine with no output device leaves this silent and carries on.
    Audio::get().init(m_dataDir);
    applyVolumes(m_config);

    // The account service. An empty issuer means this build offers no sign-in,
    // which the Account screen reports rather than failing at a request.
    AccountClient::get().init(m_config.accountIssuer, m_dataDir + "account.json");

    // Restore the session NOW, not on the first visit to the Account screen.
    // It runs on a worker, so it costs nothing here -- and hosting before ever
    // opening that screen used to publish an EMPTY nickname, which is why the
    // host's own row in its own lobby read "someone".
    AccountClient::get().bootstrap();

#ifndef __EMSCRIPTEN__
    {
        int mon = GetCurrentMonitor();
        int mw = GetMonitorWidth(mon);
        int mh = GetMonitorHeight(mon);
        SetWindowSize(m_screenW, m_screenH);
        SetWindowSize(m_screenW, m_screenH);
        PollInputEvents();
        SetWindowPosition((mw - m_screenW) / 2, (mh - m_screenH) / 2);
    }
#else
    // GLFW sets canvas.style.width/height as inline styles, overriding our CSS.
    // Clear them so canvas{width:100vw;height:100vh} takes effect.
    emscripten_run_script(
        "var c=document.getElementById('canvas');"
        "if(c){c.style.width='100vw';c.style.height='100vh';}"
    );

    // KNOWN BUG, not yet fixed: on first load the game renders into a ~400x300
    // corner of a full-size canvas. It becomes correct the moment the window is
    // resized, because that runs the IsWindowResized() path in run(), which
    // recomputes everything properly. Nobody resizes a window they have just
    // opened, so this is only ever visible on the first screen a player sees.
    //
    // What has been ruled out, so the next attempt does not start from zero:
    //   - the canvas element is fine. Framebuffer and CSS size both match
    //     window.innerWidth/innerHeight, and devicePixelRatio is 1.
    //   - emscripten_set_canvas_element_size() + SetWindowSize() here does NOT
    //     help: GetScreenWidth() immediately afterwards still returns raylib's
    //     cached size, so raylib's GL viewport stays at the old dimensions.
    //   - dispatching a synthetic window 'resize' event does NOT help either;
    //     raylib registers an emscripten resize callback that appears not to
    //     act on untrusted events. A real user resize does work.
    //
    // So the fix likely belongs in the frame loop -- comparing the canvas size
    // against m_screenW each frame and driving the existing resize path when
    // they disagree -- rather than in this one-shot init.
    m_screenW = GetScreenWidth();
    m_screenH = GetScreenHeight();
    TraceLog(LOG_INFO, ("[OD] GetScreen: " + std::to_string(GetScreenWidth()) + "x" + std::to_string(GetScreenHeight())).c_str());
    TraceLog(LOG_INFO, ("[OD] m_screen: " + std::to_string(m_screenW) + "x" + std::to_string(m_screenH)).c_str());
#endif

    if (m_config.fullscreen) {
        int mon = GetCurrentMonitor();
        m_windowedW = m_screenW;
        m_windowedH = m_screenH;
        m_windowedX = (int)GetWindowPosition().x;
        m_windowedY = (int)GetWindowPosition().y;
        SetWindowState(FLAG_WINDOW_UNDECORATED);
        m_screenW = GetMonitorWidth(mon);
        m_screenH = GetMonitorHeight(mon);
        SetWindowSize(m_screenW, m_screenH);
        SetWindowPosition(0, 0);
    }

    // Budget first: applyFpsTarget reads it, so setting it afterwards would
    // leave the first frames running unthrottled.
    setResourceBudget(m_config.resourceBudget);
    applyFpsTarget(m_config.fpsTarget);
    // Open the CPU-budget window HERE rather than at the first throttle call.
    // Everything before that first call -- window creation, loading, generating
    // the first map -- is real CPU this process spent, and starting the clock
    // afterwards silently exempted it. On a short run that exemption was the
    // whole remaining gap between the budget and the measured share.
    m_budgetEpochWall = GetTime();
    m_budgetEpochCpu = processCpuSeconds();

    initMenuBackground();

    srand((unsigned int)time(nullptr));

    // Cache default raylib font reference
    m_defaultFont = GetFontDefault();

    // Load fallback font for non-ASCII characters (Unifont — pixel font, looks crisp)
    {
        std::string fontPath = m_dataDir + "fonts/unifont.ttf";
        if (FileExists(fontPath.c_str())) {
            // Load with common Unicode ranges used in license text
            std::vector<int> codepoints;
            auto addRange = [&](int start, int end) {
                for (int c = start; c <= end; c++) codepoints.push_back(c);
            };
            addRange(32, 255);    // Basic Latin + Latin-1 Supplement
            addRange(256, 383);   // Latin Extended-A
            addRange(1024, 1279); // Cyrillic
            codepoints.push_back(8212); // EM DASH U+2014
            codepoints.push_back(8211); // EN DASH U+2013
            codepoints.push_back(8220); // LEFT DOUBLE QUOTATION MARK U+201C
            codepoints.push_back(8221); // RIGHT DOUBLE QUOTATION MARK U+201D
            codepoints.push_back(8216); // LEFT SINGLE QUOTATION MARK U+2018
            codepoints.push_back(8217); // RIGHT SINGLE QUOTATION MARK U+2019
            codepoints.push_back(8226); // BULLET U+2022
            codepoints.push_back(169);  // COPYRIGHT SIGN U+00A9
            codepoints.push_back(174);  // REGISTERED SIGN U+00AE
            m_gameFont = LoadFontEx(fontPath.c_str(), 16, codepoints.data(), (int)codepoints.size());
            if (m_gameFont.texture.id > 0) {
                SetTextureFilter(m_gameFont.texture, TEXTURE_FILTER_POINT);
                std::cout << "  Loaded fallback font: Unifont (" << m_gameFont.glyphCount << " glyphs)" << std::endl;
            } else {
                std::cerr << "  LoadFontEx FAILED for " << fontPath << std::endl;
            }
        } else {
            std::cerr << "  Fallback font not found at " << fontPath << " — using DejaVuSans" << std::endl;
            fontPath = m_dataDir + "fonts/DejaVuSans.ttf";
            if (FileExists(fontPath.c_str())) {
                std::vector<int> codepoints;
                auto addRange = [&](int start, int end) {
                    for (int c = start; c <= end; c++) codepoints.push_back(c);
                };
                addRange(32, 255);
                addRange(256, 383);
                addRange(1024, 1279);
                codepoints.push_back(8212);
                codepoints.push_back(8211);
                codepoints.push_back(8220);
                codepoints.push_back(8221);
                codepoints.push_back(8216);
                codepoints.push_back(8217);
                codepoints.push_back(8226);
                codepoints.push_back(169);
                codepoints.push_back(174);
                m_gameFont = LoadFontEx(fontPath.c_str(), 16, codepoints.data(), (int)codepoints.size());
                if (m_gameFont.texture.id > 0) {
                    SetTextureFilter(m_gameFont.texture, TEXTURE_FILTER_BILINEAR);
                    std::cout << "  Loaded fallback font: DejaVuSans (" << m_gameFont.glyphCount << " glyphs)" << std::endl;
                }
            } else {
                std::cerr << "  DejaVuSans also not found — no fallback font" << std::endl;
            }
        }
    }

    loadCredits();

    // Reaches the browser console too: emscripten routes stdout to console.log,
    // which is why none of these need an emscripten_run_script of their own.
    std::cout << "OpenDoctrines initialized. " << m_screenW << "x" << m_screenH << std::endl;
    m_running = true;
    m_currentScreen = SCREEN_SPLASH;
    m_splashTimer = 0.0f;
    initMenuBackground(); // ready so the splash's fade-out can reveal it
    m_menuBgScroll = 0;
    return true;
}

void Game::shutdown() {
    // Before the textures below: closing the device stops the streaming thread,
    // and it must not still be reading a Music that the unload is freeing.
    Audio::get().shutdown();

    // Joins any in-flight account request before the process goes away.
    AccountClient::get().shutdown();

    // Same for multiplayer. A live host keeps a listening socket and a
    // registration may be mid-flight; an unjoined std::thread at destruction
    // terminates the process, which would turn a clean quit into a crash.
    mpShutdown();

    // Save the AI model on quit (unloadGameData also does this on world exit)
    if (m_ai) { delete m_ai; m_ai = nullptr; }
    UnloadTexture(m_politicalTex);
    for (auto& [cid, tex] : m_countryFlags) {
        if (tex.id > 0) UnloadTexture(tex);
    }
    m_countryFlags.clear();
    for (auto& [path, tex] : m_thumbCache) {
        if (tex.id > 0) UnloadTexture(tex);
    }
    m_thumbCache.clear();
    m_iconPopulation = m_iconIndustry = m_iconDefence = m_iconRelations = m_iconArmyNav = m_iconNavy = m_iconResources = m_iconCountryNames = m_iconPolicies = m_iconEconomy = m_iconClaims = m_iconResearch = {};
    if (m_renderer) { delete m_renderer; m_renderer = nullptr; }
    if (m_gameFont.texture.id > 0) UnloadFont(m_gameFont);
    m_gameFont = {};
    if (m_menuBgTex.id > 0) UnloadTexture(m_menuBgTex);
    m_menuBgTex = {};
    if (m_resourceTex.id > 0) UnloadTexture(m_resourceTex);
    m_resourceTex = {};
}

void Game::loadSaveAndStart(const std::string& savePath) {
    m_currentSavePath = savePath;
    startLoadingSave(savePath);
    m_currentScreen = SCREEN_LOADING;
}

void Game::buildCountryShipList(int shipIdx) {
    m_countryShipIndices.clear();
    m_countryShipIndex = -1;
    int targetCid = m_playerCountryId;
    if (shipIdx >= 0 && shipIdx < (int)m_ships.size()) {
        targetCid = m_ships[shipIdx].countryId;
    }
    for (int i = 0; i < (int)m_ships.size(); i++) {
        if (m_ships[i].countryId == targetCid) {
            m_countryShipIndices.push_back(i);
            if (i == shipIdx) m_countryShipIndex = (int)m_countryShipIndices.size() - 1;
        }
    }
    if (m_countryShipIndex < 0 && !m_countryShipIndices.empty()) {
        m_countryShipIndex = 0;
    }
}

void Game::flyToShip(int shipIndex) {
    if (!m_renderer || shipIndex < 0 || shipIndex >= (int)m_ships.size()) return;
    const NavyShip& s = m_ships[(size_t)shipIndex];
    int px = 0, py = 0;
    m_landSea.lonLatToPixel((float)s.lon, (float)s.lat, px, py);

    // Fixed rather than derived: a ship has no extent to frame the way a
    // province does, so there is nothing to size the zoom against. Floored at
    // the whole-map zoom so this can never ask to pull further out than the
    // map itself allows.
    const float minZoom = std::max(m_screenW / (float)m_provinces.getWidth(),
                                   m_screenH / (float)m_provinces.getHeight());
    m_renderer->flyTo((float)px, (float)py, std::max(2.0f, minZoom), m_config.flySpeed);
}

void Game::cycleShip(int direction) {
    if (m_countryShipIndices.empty()) return;
    int newIdx = m_countryShipIndex + direction;
    if (newIdx < 0) newIdx = (int)m_countryShipIndices.size() - 1;
    if (newIdx >= (int)m_countryShipIndices.size()) newIdx = 0;
    m_countryShipIndex = newIdx;
    int shipIdx = m_countryShipIndices[m_countryShipIndex];
    if (shipIdx >= 0 && shipIdx < (int)m_ships.size()) {
        m_selectedShipIndices = {shipIdx};
        Audio::get().playSfx("select_province", 0.05f);
        flyToShip(shipIdx);
    }
}

void Game::addConsoleLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(m_console.mutex);
    m_console.lines.push_back(line);
    m_console.scrollOffset = 0;
    if (m_console.lines.size() > 500) {
        m_console.lines.erase(m_console.lines.begin(), m_console.lines.begin() + (m_console.lines.size() - 500));
    }
}

bool Game::isMouseOverConsole() {
    if (!m_config.showConsole) return false;
    Vector2 mouse = getMouse();
    return CheckCollisionPointRec(mouse, m_console.rect);
}

void Game::drawNotifications() {
    if (m_notifications.empty()) return;
    int y = 60;
    int maxW = 500;
    for (auto& n : m_notifications) {
        float alpha = std::min(1.0f, n.timer / n.duration * 2.0f);
        if (n.timer < 1.0f) alpha = n.timer / 1.0f;
        int x = m_screenW - maxW - 20;
        Color bg = {0, 0, 0, (unsigned char)(180 * alpha)};
        Color fg = n.color;
        fg.a = (unsigned char)(255 * alpha);
        int tw = MeasureText(n.message.c_str(), 14);
        int pw = std::min(tw + 24, maxW);
        int ph = 36;
        DrawRectangleRounded({(float)x, (float)y, (float)pw, (float)ph}, 0.08f, 6, bg);
        DrawRectangleRoundedLines({(float)x, (float)y, (float)pw, (float)ph}, 0.08f, 6, {60, 60, 80, (unsigned char)(120 * alpha)});
        DrawText(n.message.c_str(), x + 12, y + 10, 14, fg);
        y += ph + 6;
    }
}

void Game::drawConsoleWindow() {
    ConsoleWindow& c = m_console;
    Vector2 mouse = getMouse();

    bool leftDown = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    bool leftReleased = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);

    // ── Resize handles (check BEFORE title bar drag) ──
    float handleSize = 12;
    Rectangle rightEdge = {c.rect.x + c.rect.width - handleSize, c.rect.y + 24, handleSize, c.rect.height - 24};
    Rectangle bottomEdge = {c.rect.x, c.rect.y + c.rect.height - handleSize, c.rect.width, handleSize};
    Rectangle corner    = {c.rect.x + c.rect.width - handleSize, c.rect.y + c.rect.height - handleSize, handleSize, handleSize};

    int resizeEdge = 0;
    if (CheckCollisionPointRec(mouse, corner)) resizeEdge = 3;
    else if (CheckCollisionPointRec(mouse, rightEdge)) resizeEdge = 1;
    else if (CheckCollisionPointRec(mouse, bottomEdge)) resizeEdge = 2;

    if (leftDown && resizeEdge && !c.resizing) {
        c.resizing = true;
        c.resizeEdge = resizeEdge;
        c.dragOffset = {mouse.x, mouse.y};
    }
    if (c.resizing) {
        if (leftDown) {
            float dx = mouse.x - c.dragOffset.x;
            float dy = mouse.y - c.dragOffset.y;
            if (c.resizeEdge == 1 || c.resizeEdge == 3) {
                c.rect.width = std::max(200.0f, c.rect.width + dx);
                c.dragOffset.x = mouse.x;
            }
            if (c.resizeEdge == 2 || c.resizeEdge == 3) {
                c.rect.height = std::max(100.0f, c.rect.height + dy);
                c.dragOffset.y = mouse.y;
            }
        } else {
            c.resizing = false;
            c.resizeEdge = 0;
        }
    }

    // ── Title bar drag ──
    Rectangle titleBar = {c.rect.x, c.rect.y, c.rect.width - handleSize, 24};
    if (leftDown && CheckCollisionPointRec(mouse, titleBar) && !c.dragging && !c.resizing) {
        c.dragging = true;
        c.dragOffset = {mouse.x - c.rect.x, mouse.y - c.rect.y};
    }
    if (c.dragging) {
        if (leftDown) {
            c.rect.x = mouse.x - c.dragOffset.x;
            c.rect.y = mouse.y - c.dragOffset.y;
        } else {
            c.dragging = false;
        }
    }

    // ── Background ──
    DrawRectangleRounded(c.rect, 0.08f, 8, {15, 15, 25, 230});
    DrawRectangleRoundedLines(c.rect, 0.08f, 8, {100, 100, 140, 200});

    // ── Title bar ──
    DrawRectangle(c.rect.x + 2, c.rect.y + 2, c.rect.width - 4, 24, {30, 30, 50, 255});
    DrawText("Console", (int)c.rect.x + 6, (int)c.rect.y + 4, 14, {180, 180, 200, 255});

    // Close button (X) in title bar
    float closeX = c.rect.x + c.rect.width - 24;
    Rectangle closeBtn = {closeX, c.rect.y + 2, 22, 22};
    bool closeHov = CheckCollisionPointRec(mouse, closeBtn);
    if (closeHov) DrawRectangleRounded(closeBtn, 0.3f, 6, {180, 40, 40, 200});
    DrawText("x", (int)closeX + 6, (int)c.rect.y + 4, 14, closeHov ? WHITE : Color{180, 180, 200, 200});
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, closeBtn)) {
        m_config.showConsole = false;
    }

    // ── Content area (log lines) ──
    float contentY = c.rect.y + 28;
    float contentH = c.rect.height - 28 - 4;
    float lineH = 14;
    int maxLines = (int)(contentH / lineH);

    // Scroll wheel
    float wheel = GetMouseWheelMove();
    if (CheckCollisionPointRec(mouse, {c.rect.x, contentY, c.rect.width, contentH})) {
        c.scrollOffset = std::max(0, c.scrollOffset - (int)wheel);
    }

    int drawFrom, totalLines;
    std::vector<std::string> linesCopy;
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        totalLines = (int)c.lines.size();
        int maxScroll = std::max(0, totalLines - maxLines);
        c.scrollOffset = std::clamp(c.scrollOffset, 0, maxScroll);
        drawFrom = totalLines - maxLines - c.scrollOffset;
        int startIdx = std::max(0, drawFrom);
        int endIdx = std::min(totalLines, drawFrom + maxLines);
        if (startIdx < endIdx) {
            linesCopy.assign(c.lines.begin() + startIdx, c.lines.begin() + endIdx);
        }
    }

    BeginScissorMode((int)c.rect.x + 2, (int)contentY, (int)c.rect.width - 4, (int)contentH);
    for (int i = 0; i < (int)linesCopy.size(); ++i) {
        float ly = contentY + i * lineH;
        DrawText(linesCopy[i].c_str(), (int)c.rect.x + 6, (int)ly, 12, {180, 200, 180, 220});
    }
    EndScissorMode();
}

void Game::drawDebugOverlay() {
    // The resource panel is a system control, not a debug tool, so it draws
    // before the debugMode gate below. Every one of this function's ~15 call
    // sites used to be wrapped in `if (m_config.debugMode)`, which meant the
    // panel was invisible in a normal session no matter what F10 did — the
    // gate now lives here so the two cannot drift apart again.
    drawResourcePanel();
    if (!m_config.debugMode) return;

    // Show FPS with GetFPS() when enabled
    if (m_config.showFps) {
        int fps = GetFPS();
        const char* fpsStr = TextFormat("FPS: %d", fps);
        Color fpsColor = fps >= 55 ? Color{100, 255, 100, 220} : (fps >= 30 ? Color{255, 255, 100, 220} : Color{255, 100, 100, 220});
        DrawText(fpsStr, 10, 10, 18, fpsColor);
    }

    if (m_config.showZoom) {
        int provCount = (int)m_provinces.getAllProvinces().size();
        float zoom = m_renderer ? m_renderer->getZoom() : 1.0f;
        DrawText(TextFormat("Provinces: %d", provCount), 10, 32, 14, Color{180, 220, 255, 200});
        DrawText(TextFormat("Zoom: %.2f", zoom), 10, 48, 14, Color{180, 220, 255, 200});
        DrawText(TextFormat("DPI: %.2f", m_dpiScale), 10, 64, 14, Color{140, 160, 180, 160});
    }

    // AI decision feed: last decisions from the ring buffer, newest first
    if (m_config.aiDebug && m_ai) {
        int y = 84;
        DrawText(TextFormat("AI decisions (turn, country, module, action) — %d this turn",
                            m_ai->decisionsThisTurn()), 10, y, 14, {255, 220, 140, 230});
        y += 18;
        for (const auto& line : m_ai->debugLines(24)) {
            DrawText(line.c_str(), 10, y, 12, {220, 220, 180, 210});
            y += 14;
        }
    }
}

void Game::run() {
    while (m_running && !WindowShouldClose()) {
        float dt = GetFrameTime();

        // Above the popup early-out below: the music stream has to be fed on
        // every frame, and a popup is exactly when it must not stutter.
        Audio::get().update(dt);
        updateMusic(dt);
        // Also above the early-out: the resource panel is a system control, not
        // a game screen, so it has to answer F10 even with a popup up.
        samplePerformance();
        updateResourcePanel();

        // A network game has to be pumped every frame, not only while the
        // multiplayer screen is up -- it spends its life on the map, and a host
        // that stopped listening there would drop every player the moment the
        // game began.
        if (m_netHost || m_netSession) {
            mpDrainEvents();
            mpHostTurnUpdate();
        }

        updateNotifications();
        updatePopup();
        // Block all other update when popup is active (draw popup overlay on any screen)
        if (!m_popupQueue.empty()) {
            BeginDrawing();
            ClearBackground(BLACK);
            // Don't call drawInner() when a popup is active — it would process
            // clicks behind the popup (province selection, button presses, etc.).
            // The popup covers the screen with its own dim overlay anyway.
            drawPopup();
            if (m_config.showConsole) drawConsoleWindow();
            drawDebugOverlay();  // self-gates on debugMode; the resource panel is not gated
            endFrame();
            continue;
        }
        
        if (IsWindowResized()) {
            m_screenW = GetScreenWidth();
            m_screenH = GetScreenHeight();
            if (m_renderer) m_renderer->resize(m_screenW, m_screenH);
            m_dpiScale = GetWindowScaleDPI().x;
            if (m_renderer) m_renderer->setDpiScale(m_dpiScale);
            if (m_currentScreen == SCREEN_MENU || m_currentScreen == SCREEN_SINGLEPLAYER) {
                initMenuBackground();
                m_menuBgScroll = 0;
            } else if (m_currentScreen == SCREEN_FILE_BROWSER || m_currentScreen == SCREEN_MAP_SELECT || m_currentScreen == SCREEN_COUNTRY_SELECT || m_currentScreen == SCREEN_CREDITS || m_currentScreen == SCREEN_COMMUNITY || m_currentScreen == SCREEN_MAP_EDITOR || m_currentScreen == SCREEN_MODS || m_currentScreen == SCREEN_ACCOUNT ||
                       m_currentScreen == SCREEN_MULTIPLAYER) {
                if (m_screenW != m_menuBgInitScreenW || m_screenH != m_menuBgInitScreenH) {
                    initMenuBackground();
                }
            }
        }

        // On entering any menu screen, re-init background if screen size changed
        // (covers transitions back from gameplay/country-select without a resize event)
        // Handle all menu screen types, but only init once per resize
        if ((m_currentScreen == SCREEN_MENU || m_currentScreen == SCREEN_SINGLEPLAYER ||
             m_currentScreen == SCREEN_FILE_BROWSER || m_currentScreen == SCREEN_MAP_SELECT || m_currentScreen == SCREEN_CREDITS || m_currentScreen == SCREEN_COMMUNITY || m_currentScreen == SCREEN_MAP_EDITOR || m_currentScreen == SCREEN_MODS || m_currentScreen == SCREEN_ACCOUNT ||
                       m_currentScreen == SCREEN_MULTIPLAYER) &&
            !IsWindowResized() &&
            (m_screenW != m_menuBgInitScreenW || m_screenH != m_menuBgInitScreenH)) {
            initMenuBackground();
        }

        if (m_currentScreen == SCREEN_SPLASH) {
            updateSplashScreen(dt);
            BeginDrawing();
            ClearBackground(BLACK);
            drawSplashScreen();
            endFrame();
        } else if (m_currentScreen == SCREEN_MENU) {
            if (m_inSettings) {
                updateMenuBackground();
                updateSettingsFromMenu();
                BeginDrawing();
                ClearBackground(BLACK);
                drawSettingsFromMenu();
                if (m_config.showConsole) drawConsoleWindow();
                drawDebugOverlay();  // self-gates on debugMode; the resource panel is not gated
                endFrame();
            } else {
                updateMenuBackground();
                updateMainMenu();
                BeginDrawing();
                ClearBackground(BLACK);
                drawMainMenu();
                if (m_config.showConsole) drawConsoleWindow();
                drawDebugOverlay();  // self-gates on debugMode; the resource panel is not gated
                endFrame();
            }
        } else if (m_currentScreen == SCREEN_SINGLEPLAYER) {
            updateMenuBackground();
            updateSingleplayerMenu();
            BeginDrawing();
            ClearBackground(BLACK);
            drawSingleplayerMenu();
            if (m_config.showConsole) drawConsoleWindow();
            drawDebugOverlay();  // self-gates on debugMode; the resource panel is not gated
            endFrame();
        } else if (m_currentScreen == SCREEN_FILE_BROWSER) {
            if (m_inHistory) {
                // Turn History opened from a save's World Settings takes over
                // the whole screen until closed.
                updateHistoryScreen();
                BeginDrawing();
                ClearBackground(BLACK);
                drawHistoryScreen();
                if (m_config.showConsole) drawConsoleWindow();
                endFrame();
            } else if (m_browsingSaves) {
                updateMenuBackground();
                updateWorldBrowser();
                BeginDrawing();
                ClearBackground(BLACK);
                drawWorldBrowser();
                if (m_config.showConsole) drawConsoleWindow();
                drawDebugOverlay();  // self-gates on debugMode; the resource panel is not gated
                endFrame();
            } else {
                updateMenuBackground();
                updateFileBrowser();
                BeginDrawing();
                ClearBackground(BLACK);
                drawFileBrowser();
                if (m_config.showConsole) drawConsoleWindow();
                drawDebugOverlay();  // self-gates on debugMode; the resource panel is not gated
                endFrame();
            }
        } else if (m_currentScreen == SCREEN_MAP_SELECT) {
            updateMenuBackground();
            updateMapBrowser();
            BeginDrawing();
            ClearBackground(BLACK);
            drawMapBrowser();
            if (m_config.showConsole) drawConsoleWindow();
            drawDebugOverlay();  // self-gates on debugMode; the resource panel is not gated
            endFrame();
        } else if (m_currentScreen == SCREEN_LOADING) {
            // Throttled work: run at most one loading phase per ~33ms (30fps) so the
            // throbber gets a chance to render between heavy steps.  Draw happens
            // BEFORE work so the current throbber frame is presented before the next
            // blocking step.  (Note: no EndDrawing here during loading — raylib on
            // macOS auto-presents on the next BeginDrawing, which keeps the loading
            // screen visible.)
            double now = GetTime();
            if (now - m_lastLoadingWork >= 0.033) {
                m_lastLoadingWork = now;
                if (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE && !m_loadingFailed) {
                    // One loading phase is six to nine seconds of blocking work
                    // at worst, during which nothing here refills the music.
                    // The helper thread does it instead; the brackets are tight
                    // around the call so it cannot outlive the stall.
                    Audio::get().beginBackgroundPump();
                    updateLoading();
                    Audio::get().endBackgroundPump();
                }
            }
            BeginDrawing();
            ClearBackground(BLACK);
            if (m_showLoadingScreen) {
                drawLoadingScreen();
            } else {
                // Loading completed — present one final frame then transition
                endFrame();
                if (m_loadingFailed) {
                    m_currentScreen = SCREEN_MENU;
                } else if (m_currentScreen == SCREEN_LOADING) {
                    // LOAD_FINALIZE didn't set a screen (old path), default to country select
                    m_currentScreen = SCREEN_COUNTRY_SELECT;
                }
            }
        } else if (m_currentScreen == SCREEN_COUNTRY_SELECT) {
            updateCountrySelect();
            BeginDrawing();
            ClearBackground(BLACK);
            drawCountrySelect();
            if (m_config.showConsole) drawConsoleWindow();
            drawDebugOverlay();  // self-gates on debugMode; the resource panel is not gated
            endFrame();
        } else if (m_currentScreen == SCREEN_CREDITS) {
            updateMenuBackground();
            updateCredits();
            BeginDrawing();
            ClearBackground(BLACK);
            drawCredits();
            if (m_config.showConsole) drawConsoleWindow();
            drawDebugOverlay();  // self-gates on debugMode; the resource panel is not gated
            endFrame();
        } else if (m_currentScreen == SCREEN_COMMUNITY) {
            updateMenuBackground();
            updateCommunityMenu();
            BeginDrawing();
            ClearBackground(BLACK);
            drawCommunityMenu();
            if (m_config.showConsole) drawConsoleWindow();
            drawDebugOverlay();  // self-gates on debugMode; the resource panel is not gated
            endFrame();
        } else if (m_currentScreen == SCREEN_MODS) {
            updateMenuBackground();
            updateModsMenu();
            BeginDrawing();
            ClearBackground(BLACK);
            drawModsMenu();
            if (m_config.showConsole) drawConsoleWindow();
            drawDebugOverlay();  // self-gates on debugMode; the resource panel is not gated
            endFrame();
        } else if (m_currentScreen == SCREEN_MULTIPLAYER) {
            updateMultiplayerMenu();
            BeginDrawing();
            ClearBackground(BLACK);
            drawMultiplayerMenu();
            if (m_config.showConsole) drawConsoleWindow();
            drawDebugOverlay();  // self-gates on debugMode; the resource panel is not gated
            endFrame();
        } else if (m_currentScreen == SCREEN_ACCOUNT) {
            updateAccountMenu();
            BeginDrawing();
            ClearBackground(BLACK);
            drawAccountMenu();
            if (m_config.showConsole) drawConsoleWindow();
            drawDebugOverlay();  // self-gates on debugMode; the resource panel is not gated
            endFrame();
        } else if (m_currentScreen == SCREEN_MAP_EDITOR) {
            if (m_mapEditor) {
                // The editor's toolbar can ask for the settings screen. Game
                // owns that screen, so the editor only raises the request and
                // this decides what to run -- the editor keeps drawing behind
                // it, which is what makes it read as an overlay on the work
                // rather than as having left the project.
                if (m_mapEditor->consumeSettingsRequest()) {
                    m_inSettings = true;
                    Audio::get().playSfx("click_light");
                    m_settingsTab = AUDIO_TAB;
                    m_settingsIndex = 0;
                    m_settingsScroll = 0;
                }
                if (m_inSettings) updateSettingsFromMenu();
                else updateMapEditor();
                BeginDrawing();
                ClearBackground(BLACK);
                drawMapEditor();
                if (m_inSettings) drawPauseMenu();
                if (m_config.showConsole) drawConsoleWindow();
                drawDebugOverlay();  // self-gates on debugMode; the resource panel is not gated
                endFrame();
            } else {
                m_currentScreen = SCREEN_MENU;
            }
        } else {
            update(dt);
            draw();
        }

        // Last thing in the frame, so a shot is always of a screen that has
        // just been drawn rather than one that is about to be. Inert unless
        // --screenshots asked for a tour; see Game_Screenshots.cpp.
        if (m_shotTour && !tickScreenshotTour()) m_running = false;
    }
}



// ────────────────────────────────────────────────────────────────────────────
// Music selection
// ────────────────────────────────────────────────────────────────────────────

namespace {
// Long enough to read a title and an author without looking for it, short
// enough that it is gone before it becomes part of the screen.
constexpr float TOAST_SECONDS = 4.5f;
constexpr float TOAST_FADE    = 0.45f;
}  // namespace

Mood Game::currentMood() {
    const double now = GetTime();
    if (m_moodStamp >= 0.0 && now - m_moodStamp < 0.5) return m_mood;
    m_moodStamp = now;

    Mood m;
    if (m_currentScreen == SCREEN_MAP_EDITOR) {
        // Building something, with nothing at stake.
        m = { 0.10f, 0.35f, 0.15f };
    } else if (m_currentScreen != SCREEN_PLAYING) {
        // Menus, browsers, loading. NOT neutral, which was the first guess and
        // the wrong one: neutral is closest to whatever the calmest track in the
        // library happens to be, so the main theme would never play on the main
        // menu. A title screen is poised and ceremonial -- about to begin rather
        // than at rest -- and this is the point that says so.
        m = { 0.35f, 0.45f, -0.15f };
    } else {
        const Country* pc = m_countries.getCountry(m_playerCountryId);
        if (!pc) { m_mood = { 0.2f, 0.4f, 0.0f }; return m_mood; }

        int wars = 0;
        std::vector<std::string> enemies;
        auto rel = m_relations.find(pc->isoA3);
        if (rel != m_relations.end())
            for (const auto& [other, r] : rel->second)
                if (r.war) { ++wars; enemies.push_back(other); }

        // One pass over the map serves three questions: how much the player
        // holds, how much each enemy holds, and how close to revolt the
        // player's own provinces are. Walking it three times would be the same
        // answer for three times the cost.
        int mine = 0;
        float unrest = 0.0f;
        std::unordered_map<int, int> provsByCid;
        for (const auto& [id, p] : m_provinces.getAllProvinces()) {
            ++provsByCid[p.countryId];
            if (p.countryId == m_playerCountryId) {
                ++mine;
                unrest += getProvinceRebellionChance(id, m_playerCountryId);
            }
        }

        // Re-baseline on a different country: a new game, a loaded save or a
        // country switch all change who "we" are, and comparing the new holding
        // against the old one would read as a catastrophic loss.
        if (m_moodBaselineCid != m_playerCountryId) {
            m_moodBaselineCid = m_playerCountryId;
            m_moodBaseline = mine;
        }

        // One war is already the difference in kind; further wars only deepen
        // it. Scaling linearly from zero would have a single desperate war
        // scoring calmer than peace does in the menus.
        m.tension = (wars == 0) ? 0.15f
                                : std::clamp(0.55f + 0.15f * (float)(wars - 1), 0.0f, 1.0f);
        m.energy  = 0.30f + 0.50f * m.tension;

        // Whether the empire has grown since this country was picked up. The
        // x10 is what makes losing a tenth of it read as fully bleak rather
        // than as a rounding error.
        const float trend = (m_moodBaseline > 0)
            ? (float)(mine - m_moodBaseline) / (float)m_moodBaseline : 0.0f;
        m.valence = std::clamp(trend * 10.0f, -1.0f, 1.0f);

        // Money trouble. Deliberately measured as RUNWAY -- how many turns the
        // reserve survives the current bleed -- rather than as a treasury
        // number, because "broke" means nothing without knowing the burn rate:
        // a small country with a small deficit is fine, a large one haemorrhaging
        // is not, and the raw balance cannot tell them apart.
        float trouble = 0.0f;
        if (pc->treasury < 0.0) {
            trouble = 1.0f;                      // already in the red
        } else {
            const CountryIncomeSnapshot inc = computeCountryIncome(m_playerCountryId);
            const float net = inc.gross + inc.resource + inc.pop - inc.expenses;
            if (net < 0.0f) {
                const float runway = (float)(pc->treasury / -net);
                // Forty turns of reserve is comfortable; none is total.
                trouble = std::clamp(1.0f - runway / 40.0f, 0.0f, 1.0f);
            }
        }
        if (trouble > 0.0f) {
            // Bleak without being martial: the threat is that nothing can be
            // paid for, not that anything is attacking.
            m.valence = std::clamp(m.valence - 0.70f * trouble, -1.0f, 1.0f);
            m.energy  = std::clamp(m.energy  - 0.15f * trouble, 0.0f, 1.0f);
            m.tension = std::clamp(m.tension + 0.15f * trouble, 0.0f, 1.0f);
        }

        // ── Who are we fighting? ──────────────────────────────────────────
        // A rebellion or a far smaller neighbour is a different experience from
        // a peer war: still loud, but not frightening. Rebel CIDs start at
        // REBEL_CID_MIN, so a breakaway is recognisable without comparing sizes
        // at all; everyone else is judged on how much of the map they hold.
        float asymmetry = 0.0f;   // 0 peer or stronger .. 1 far weaker
        if (wars > 0 && mine > 0) {
            int biggestEnemy = 0;
            bool allRebels = true;
            for (const std::string& iso : enemies) {
                const Country* ec = m_countries.getCountryByCode(iso);
                if (!ec) continue;
                if (ec->id < REBEL_CID_MIN) allRebels = false;
                auto it = provsByCid.find(ec->id);
                if (it != provsByCid.end()) biggestEnemy = std::max(biggestEnemy, it->second);
            }
            const float ratio = (float)biggestEnemy / (float)mine;
            // Even odds is 0; a quarter of our size or less is total.
            asymmetry = std::clamp(1.0f - ratio / 0.75f, 0.0f, 1.0f);
            if (allRebels) asymmetry = std::max(asymmetry, 0.8f);
        }
        // Being the bigger country is not the same as winning. An empire coming
        // apart is usually at war with something small -- its own breakaways --
        // and reading that as confidence is exactly backwards, so the whole
        // effect is scaled by how the map is actually going.
        if (m.valence < 0.0f) asymmetry *= std::max(0.0f, 1.0f + m.valence);

        if (asymmetry > 0.0f) {
            // Confident rather than threatened: the noise stays, the fear goes.
            // Tension drops only a little on purpose -- dropping it far landed
            // the mood on top of the research theme, and a one-sided war is
            // still a war, which is the whole distinction being drawn here.
            m.tension = std::clamp(m.tension - 0.12f * asymmetry, 0.0f, 1.0f);
            m.energy  = std::clamp(m.energy  + 0.20f * asymmetry, 0.0f, 1.0f);
            m.valence = std::clamp(m.valence + 0.45f * asymmetry, -1.0f, 1.0f);
        }

        // ── Politics ──────────────────────────────────────────────────────
        // Treaties standing rather than wars declared: alliances, pacts and
        // guarantees are the whole of what a country does with its neighbours
        // when it is not fighting them, so how many it holds is how far the
        // situation has become a diplomatic one.
        if (wars == 0 && rel != m_relations.end()) {
            int treaties = 0;
            for (const auto& [other, r] : rel->second)
                if (!r.war && (r.alliance || r.nonAggression || r.guarantee))
                    ++treaties;
            // The first two are subtracted, not counted: a pact with each
            // neighbour is ordinary, and starting the scale at zero would make
            // every country at peace a political one. Eight is a web.
            const float politics = std::clamp((float)(treaties - 2) / 6.0f, 0.0f, 1.0f);
            m.tension = std::clamp(m.tension + 0.15f * politics, 0.0f, 1.0f);
            m.energy  = std::clamp(m.energy  + 0.13f * politics, 0.0f, 1.0f);
            m.valence = std::clamp(m.valence + 0.20f * politics, -1.0f, 1.0f);
        }

        // ── Rebuilding ────────────────────────────────────────────────────
        // Peace right after a war does not look different from any other peace
        // in a snapshot, so the last war has to be remembered.
        if (wars > 0) m_moodLastWarTurn = m_turnCount;
        if (wars == 0 && m_moodLastWarTurn >= 0) {
            const int since = m_turnCount - m_moodLastWarTurn;
            const float recent = std::clamp(1.0f - (float)since / 40.0f, 0.0f, 1.0f);
            m.energy  = std::clamp(m.energy  + 0.20f * recent, 0.0f, 1.0f);
            m.valence = std::clamp(m.valence + 0.35f * recent, -1.0f, 1.0f);
        }

        // ── Arming ────────────────────────────────────────────────────────
        // Army and navy upkeep as a share of income, and only while at peace:
        // a country spending heavily on forces it is not using is preparing to.
        if (wars == 0) {
            const CountryIncomeSnapshot mil = computeCountryIncome(m_playerCountryId);
            const float income = mil.gross + mil.resource + mil.pop;
            if (income > 1.0f) {
                const float share = (mil.armyExpenses + mil.navyExpenses) / income;
                // A third of the budget on forces is already a war footing.
                const float arming = std::clamp((share - 0.10f) / 0.25f, 0.0f, 1.0f);
                m.tension = std::clamp(m.tension + 0.30f * arming, 0.0f, 1.0f);
                m.energy  = std::clamp(m.energy  + 0.30f * arming, 0.0f, 1.0f);
                m.valence = std::clamp(m.valence + 0.10f * arming, -1.0f, 1.0f);
            }
        }

        // ── Something about to go wrong ───────────────────────────────────
        // Rebellion pressure across the player's own provinces. This is the one
        // signal that rises BEFORE anything visible happens, which is why it
        // raises tension while LOWERING energy -- dread, not alarm.
        if (mine > 0) {
            const float avgUnrest = unrest / (float)mine;
            // The loyalty floor is what a province must exceed to revolt at all,
            // so it is the natural zero point for "is this becoming a problem".
            const float risk = std::clamp((avgUnrest - REBELLION_LOYALTY_FLOOR) / 6.0f, 0.0f, 1.0f);
            m.tension = std::clamp(m.tension + 0.35f * risk, 0.0f, 1.0f);
            m.energy  = std::clamp(m.energy  - 0.25f * risk, 0.0f, 1.0f);
            m.valence = std::clamp(m.valence - 0.35f * risk, -1.0f, 1.0f);
        }

        // ── Research ──────────────────────────────────────────────────────
        // An active research project is the one thing the player can be busy
        // with that the map does not show. Scaled by how much of the budget is
        // actually committed, so a token allocation is a nudge and a real push
        // is what moves the music -- at the default quarter this barely
        // registers, and near full commitment it takes over.
        if (m_researchActiveNode >= 0) {
            const float commit = std::clamp(m_researchAllocation, 0.0f, 1.0f);
            m.energy  = std::clamp(m.energy  + 0.35f * commit, 0.0f, 1.0f);
            m.valence = std::clamp(m.valence + 0.45f * commit, -1.0f, 1.0f);
        }
    }

    m_mood = m;
    return m_mood;
}

float Game::mapAtmosphereIntensity() const {
    float zoom = 0.0f, minZoom = 0.0f;
    if (m_currentScreen == SCREEN_PLAYING && m_renderer) {
        zoom = m_renderer->getZoom();
        minZoom = m_renderer->getMinZoom();
    } else if (m_currentScreen == SCREEN_MAP_EDITOR && m_mapEditor) {
        zoom = m_mapEditor->getZoom();
        minZoom = m_mapEditor->getMinZoom();
    } else {
        return 0.0f;   // menus and everything else stay dry
    }
    if (zoom <= 0.0f || minZoom <= 0.0f) return 0.0f;

    // Zoom is multiplicative, so "how far in are we" is a ratio and the curve
    // has to be logarithmic. Measured against the fit-to-screen level rather
    // than a fixed number, because what counts as zoomed out depends on the map
    // and the window. Dry once the view is ZOOM_DRY_RATIO times closer than
    // that, which on a world map is roughly continent-sized.
    constexpr float ZOOM_DRY_RATIO = 8.0f;
    const float t = std::log(zoom / minZoom) / std::log(ZOOM_DRY_RATIO);
    return 1.0f - std::clamp(t, 0.0f, 1.0f);
}

void Game::updateMusic(float dt) {
    // Consumed unconditionally, then shown only if the option is on. Leaving
    // the flag set while the toast is disabled would make the next track the
    // player enables it for pop up instantly and out of context.
    TrackInfo changed;
    if (Audio::get().takeTrackChange(changed) && m_config.nowPlayingToast) {
        m_toast = changed;
        m_toastTimer = TOAST_SECONDS;
    }
    if (m_toastTimer > 0.0f) m_toastTimer -= dt;

    Audio::get().setAtmosphereIntensity(mapAtmosphereIntensity());

    // playForContext returns immediately when the context has not changed, so
    // this runs unconditionally rather than tracking screen transitions here.
    switch (m_currentScreen) {
        case SCREEN_SPLASH:
            // Whatever silence the game started in. Opening a logo fade over a
            // track that starts mid-phrase sounds like a mistake.
            break;
        case SCREEN_PLAYING:
            Audio::get().playForContext("game", currentMood());
            break;
        case SCREEN_MAP_EDITOR:
            Audio::get().playForContext("editor", currentMood());
            break;
        default:
            // Every menu, browser and loading screen is one context. They are
            // places the player passes through rather than sits in, and a track
            // restarting on each hop between them would be worse than one that
            // carries across.
            Audio::get().playForContext("menu", currentMood());
            break;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Now-playing toast
// ────────────────────────────────────────────────────────────────────────────

void Game::showNowPlayingToast() {
    TrackInfo t = Audio::get().nowPlaying();
    if (t.title.empty()) return;
    m_toast = t;
    m_toastTimer = TOAST_SECONDS;
}

void Game::drawNowPlayingToast() {
    if (m_toastTimer <= 0.0f || m_toast.title.empty()) return;

    // Eased at both ends so it never blinks out mid-frame.
    float alpha = 1.0f;
    const float shown = TOAST_SECONDS - m_toastTimer;
    if (shown < TOAST_FADE)         alpha = shown / TOAST_FADE;
    else if (m_toastTimer < TOAST_FADE) alpha = m_toastTimer / TOAST_FADE;
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    const auto A = [&](int v) { return (unsigned char)std::clamp((int)(v * alpha), 0, 255); };

    // Deliberately ASCII and DrawText: the toast draws on every screen,
    // including ones reached before m_gameFont exists, and raylib's built-in
    // font has no glyph for a musical note or an em dash.
    const char* kicker = "NOW PLAYING";
    const std::string title = m_toast.title;
    const std::string author = m_toast.author.empty() ? std::string() : ("by " + m_toast.author);

    const int kickerSize = 12, titleSize = 22, authorSize = 14;
    const int padX = 16, padY = 12, accentW = 3;

    int textW = std::max(MeasureText(kicker, kickerSize), MeasureText(title.c_str(), titleSize));
    if (!author.empty()) textW = std::max(textW, MeasureText(author.c_str(), authorSize));

    const int boxW = textW + padX * 2 + accentW;
    const int boxH = padY * 2 + kickerSize + 6 + titleSize + (author.empty() ? 0 : 4 + authorSize);
    // Bottom-left: notifications already own the top-right corner. The margin
    // clears the version string the main menu draws at screenH - 24 in 14px,
    // which the toast otherwise sits directly on top of.
    const int x = 24;
    const int y = m_screenH - boxH - 46;

    const Color accent = hexToColor(m_config.accentColor);
    DrawRectangleRounded({ (float)x, (float)y, (float)boxW, (float)boxH }, 0.12f, 8,
                         Color{ 12, 12, 18, A(225) });
    DrawRectangleRoundedLines({ (float)x, (float)y, (float)boxW, (float)boxH }, 0.12f, 8,
                              Color{ accent.r, accent.g, accent.b, A(90) });
    // Accent spine down the left edge, inset so the rounding does not clip it.
    DrawRectangle(x + 1, y + 8, accentW, boxH - 16, Color{ accent.r, accent.g, accent.b, A(230) });

    int ty = y + padY;
    const int tx = x + accentW + padX;
    DrawText(kicker, tx, ty, kickerSize, Color{ accent.r, accent.g, accent.b, A(215) });
    ty += kickerSize + 6;
    DrawText(title.c_str(), tx, ty, titleSize, Color{ 245, 245, 250, A(255) });
    if (!author.empty()) {
        ty += titleSize + 4;
        DrawText(author.c_str(), tx, ty, authorSize, Color{ 170, 170, 185, A(200) });
    }
}

void Game::endFrame() {
    // Everything that has to sit above all other drawing goes here. run() has
    // fifteen separate draw blocks, one per screen state, and threading a new
    // overlay through each of them is how one of them ends up missing it.
    drawNowPlayingToast();
    EndDrawing();
}

// ────────────────────────────────────────────────────────────────────────────
// Settings > Audio sliders
// ────────────────────────────────────────────────────────────────────────────

namespace {
constexpr float SLIDER_W = 320.0f;
constexpr float SLIDER_H = 8.0f;
// Under the label, not beside it. The label is centred and its width changes
// as the percentage does ("9%" to "100%"), so a bar beside it would shift.
constexpr float SLIDER_DY = 44.0f;
}  // namespace

Rectangle Game::sliderBarRect(int y, int centerX) const {
    return { (float)centerX - SLIDER_W / 2.0f, (float)y + SLIDER_DY, SLIDER_W, SLIDER_H };
}

void Game::drawSliderWidget(Rectangle bar, float t, bool active, int steps) const {
    const Color accent = hexToColor(m_config.accentColor);
    t = std::clamp(t, 0.0f, 1.0f);

    DrawRectangleRounded(bar, 1.0f, 6, Color{255, 255, 255, 36});
    Rectangle fill = bar;
    fill.width = bar.width * t;
    // Below one bar-height the rounded rect degenerates into a smear.
    if (fill.width >= SLIDER_H) DrawRectangleRounded(fill, 1.0f, 6, accent);

    // Stops are drawn from the same fraction the thumb and the hit test use.
    // They disagreed before: ticks at i/(n-1), thumb at (i+0.5)/n, so the
    // handle never once sat on the stop it was reporting.
    if (steps > 1) {
        for (int i = 0; i < steps; ++i) {
            const float f = (float)i / (float)(steps - 1);
            const int tx = (int)(bar.x + bar.width * f);
            const int th = (i == 0 || i == steps - 1) ? 12 : 6;
            DrawRectangle(tx, (int)(bar.y + bar.height) + 2, 2, th,
                          Color{140, 140, 140, 200});
        }
    }

    const int kx = (int)(bar.x + bar.width * t);
    const int ky = (int)(bar.y + bar.height * 0.5f);
    const float kr = active ? 9.0f : 7.0f;
    DrawCircle(kx, ky, kr, accent);
    DrawCircle(kx, ky, kr - 3.0f, Color{20, 20, 28, 255});
}

bool Game::sliderInteract(Rectangle bar, int steps, float& t, bool& owns) {
    const Vector2 mouse = getMouse();
    // Eight pixels of bar is not a mouse target; the grab band is the row.
    const Rectangle grab = { bar.x - 10.0f, bar.y - 16.0f,
                             bar.width + 20.0f, bar.height + 32.0f };

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        CheckCollisionPointRec(mouse, grab)) {
        owns = true;
        m_sliderDragT = -1.0f;   // nothing reported yet this drag
        Audio::get().playSfx("slider_grab");
    }
    if (owns && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        owns = false;
        Audio::get().playSfx("slider_release");
    }
    if (!owns || !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) return false;

    float nt = std::clamp((mouse.x - bar.x) / bar.width, 0.0f, 1.0f);
    if (steps > 1) {
        const int idx = std::clamp((int)roundf(nt * (float)(steps - 1)), 0, steps - 1);
        nt = (float)idx / (float)(steps - 1);
    } else {
        // Whole percent. The labels read as integer percentages anyway, and a
        // raw mouse position never repeats, so without this the slider had no
        // notion of "not moved" at all.
        nt = roundf(nt * 100.0f) / 100.0f;
    }
    // Against the last reported value, not the caller's -- see m_sliderDragT.
    if (nt == m_sliderDragT) return false;

    // Hitting either stop is its own sound: a slider that ticks identically
    // forever gives no feedback that it has run out of travel.
    const bool wasEnd = (m_sliderDragT >= 0.0f &&
                         (m_sliderDragT <= 0.0f || m_sliderDragT >= 1.0f));
    const bool isEnd  = (nt <= 0.0f || nt >= 1.0f);
    m_sliderDragT = nt;
    t = nt;
    if (isEnd && !wasEnd) {
        Audio::get().playSfx("slider_end");
    } else if (GetTime() - m_lastSliderTick > 0.03) {
        // Rate-limited: a continuous drag would otherwise fire one per frame.
        m_lastSliderTick = GetTime();
        Audio::get().playSfx("slider_tick", 0.08f);
    }
    return true;
}

void Game::adjustVolume(int index, float delta) {
    float* v = volumeSettingPtr(m_config, AUDIO_TAB, index);
    if (!v) return;

    // Whole percent. The label shows an integer percentage, and without the
    // rounding a drag would write 0.7999999 into config.json and then read it
    // back as a value the label disagrees with.
    float nv = roundf(std::clamp(*v + delta, 0.0f, 1.0f) * 100.0f) / 100.0f;
    if (nv == *v) return;
    *v = nv;
    applyVolumes(m_config);

    // Master and Effects are otherwise silent to adjust: with nothing playing
    // there is no way to hear what was just set. Music needs no preview because
    // moving it changes what is already audible. Rate-limited because a drag
    // would otherwise fire one click per frame.
    if (index != 1 && GetTime() - m_lastSliderTick > 0.03) {
        m_lastSliderTick = GetTime();
        Audio::get().playSfx("slider_tick", 0.08f);
    }
}

bool Game::updateVolumeSliders(int startY, int itemH, int centerX, int effScroll) {
    if (m_settingsTab != AUDIO_TAB) { m_draggingVolume = -1; return false; }
    // Once per drag, not once per frame of it. Leaving the settings screen saves
    // too, but a player who drags a slider and then quits from the window's
    // close button never reaches that.
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && m_draggingVolume >= 0)
        m_config.save(m_configPath);

    const Vector2 mouse = getMouse();
    bool over = false;
    for (int i = 0; i < VOLUME_COUNT; ++i) {
        const float* v = volumeSettingPtr(m_config, AUDIO_TAB, i);
        if (!v) continue;
        const Rectangle bar = sliderBarRect(startY + (i - effScroll) * itemH, centerX);
        const Rectangle grab = { bar.x - 10.0f, bar.y - 16.0f,
                                 bar.width + 20.0f, bar.height + 32.0f };
        if (CheckCollisionPointRec(mouse, grab)) over = true;

        bool owns = (m_draggingVolume == i);
        float t = *v;
        const bool changed = sliderInteract(bar, /*steps=*/0, t, owns);
        if (owns) { m_draggingVolume = i; m_settingsIndex = i; }
        else if (m_draggingVolume == i) m_draggingVolume = -1;
        if (changed) adjustVolume(i, t - *v);
    }
    return over || m_draggingVolume >= 0;
}

void Game::drawVolumeSlider(int index, int y, int centerX, bool selected) {
    const float* v = volumeSettingPtr(m_config, AUDIO_TAB, index);
    if (!v) return;
    drawSliderWidget(sliderBarRect(y, centerX), *v,
                     selected || m_draggingVolume == index, /*steps=*/0);
}

// ────────────────────────────────────────────────────────────────────────────
// Settings > Display > Resource Limit
// ────────────────────────────────────────────────────────────────────────────

float Game::resourceBudgetSliderT(float budget) {
    return std::clamp((budget - RESOURCE_BUDGET_MIN) / (1.0f - RESOURCE_BUDGET_MIN),
                      0.0f, 1.0f);
}

float Game::resourceBudgetFromSliderT(float t) {
    float b = RESOURCE_BUDGET_MIN + std::clamp(t, 0.0f, 1.0f) * (1.0f - RESOURCE_BUDGET_MIN);
    // Whole percent, for the same reason the volumes round: the label reads as
    // an integer percentage and config.json must not disagree with it.
    return std::clamp(roundf(b * 100.0f) / 100.0f, RESOURCE_BUDGET_MIN, 1.0f);
}

void Game::applyResourceBudget() {
    setResourceBudget(m_config.resourceBudget);
    applyFpsTarget(m_config.fpsTarget);
    // Restart the measurement window: a slider move should take effect from
    // now, not be blended with several seconds of usage under the old setting.
    m_budgetEpochWall = 0.0;
}


// ────────────────────────────────────────────────────────────────────────────
// Runtime resource panel (F10)
// ────────────────────────────────────────────────────────────────────────────

double Game::processCpuSeconds() {
    // The honest measurement. Timing our own loops would only ever count the
    // work we remembered to instrument, and would miss the driver, the audio
    // thread and the whole render path; the OS already knows the real number.
#if defined(_WIN32)
    FILETIME creation, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return -1.0;
    auto toSec = [](const FILETIME& ft) {
        ULARGE_INTEGER v; v.LowPart = ft.dwLowDateTime; v.HighPart = ft.dwHighDateTime;
        return (double)v.QuadPart / 1.0e7; // 100ns ticks
    };
    return toSec(kernel) + toSec(user);
#elif defined(__EMSCRIPTEN__)
    return -1.0; // no per-process accounting in the browser
#else
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return -1.0;
    return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1.0e6 +
           (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1.0e6;
#endif
}

void Game::samplePerformance() {
    const double now = GetTime();
    if (m_perfLastWall <= 0.0) {          // first call: establish the baseline
        m_perfLastWall = now;
        m_perfLastCpu = processCpuSeconds();
        return;
    }
    const double dWall = now - m_perfLastWall;
    if (dWall < PERF_SAMPLE_SECONDS) return;

    const double cpu = processCpuSeconds();
    float share = 0.0f;
    if (cpu >= 0.0 && m_perfLastCpu >= 0.0 && dWall > 0.0)
        share = (float)((cpu - m_perfLastCpu) / dWall);
    m_perfLastWall = now;
    m_perfLastCpu = cpu;

    m_perfHistory.push_back({share, m_config.resourceBudget, m_lastTurnMs});
    while (m_perfHistory.size() > PERF_HISTORY) m_perfHistory.pop_front();
}

bool Game::updateResourcePanel() {
    // Two ways in. F10 is the conventional one, but on a Mac laptop the
    // function row is media keys by default, so reaching it means holding fn --
    // and a control you have to look up how to press is a control nobody uses.
    // Ctrl+L needs no modifier gymnastics and cannot collide with typing.
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (IsKeyPressed(KEY_F10) || (ctrl && IsKeyPressed(KEY_L))) {
        m_showResourcePanel = !m_showResourcePanel;
        if (!m_showResourcePanel) {
            m_draggingPanelSlider = false;
            m_config.save(m_configPath);
        }
    }
    if (!m_showResourcePanel) return false;

    const Rectangle panel = resourcePanelRect();
    const Rectangle bar = { panel.x + 16, panel.y + 58, panel.width - 32, 8 };
    const bool wasDragging = m_draggingPanelSlider;
    float t = resourceBudgetSliderT(m_config.resourceBudget);
    if (sliderInteract(bar, /*steps=*/0, t, m_draggingPanelSlider)) {
        m_config.resourceBudget = resourceBudgetFromSliderT(t);
        applyResourceBudget();
    }
    if (wasDragging && !m_draggingPanelSlider) m_config.save(m_configPath);

    // Swallow clicks anywhere on the panel so dragging the slider does not also
    // select a province through it.
    return m_draggingPanelSlider || CheckCollisionPointRec(getMouse(), panel);
}

Rectangle Game::resourcePanelRect() const {
    const float w = 300.0f, h = 224.0f;
    return { (float)m_screenW - w - 16.0f, 16.0f, w, h };
}

void Game::drawResourcePanel() {
    if (!m_showResourcePanel) return;
    const Rectangle p = resourcePanelRect();
    const Color accent = hexToColor(m_config.accentColor);

    DrawRectangleRounded(p, 0.06f, 8, Color{14, 16, 22, 232});
    DrawRectangleRoundedLines(p, 0.06f, 8, Color{255, 255, 255, 40});
    DrawText("RESOURCE LIMIT", (int)p.x + 16, (int)p.y + 12, 16, accent);
    DrawText("F10", (int)(p.x + p.width) - 32, (int)p.y + 13, 12, Color{140, 145, 160, 200});

    // Current setting + what it actually caps the frame rate at.
    {
        const int ceiling = budgetedFpsCeiling();
        std::string label = m_config.resourceBudget >= 0.999f
            ? std::string("Unlimited")
            : TextFormat("%d%%   frame cap %d fps",
                         (int)lroundf(m_config.resourceBudget * 100.0f), ceiling);
        DrawText(label.c_str(), (int)p.x + 16, (int)p.y + 34, 14, RAYWHITE);
    }

    const Rectangle bar = { p.x + 16, p.y + 58, p.width - 32, 8 };
    drawSliderWidget(bar, resourceBudgetSliderT(m_config.resourceBudget),
                     m_draggingPanelSlider, /*steps=*/0);

    // ── History graph ──
    const Rectangle g = { p.x + 16, p.y + 88, p.width - 32, 90 };
    DrawRectangleRec(g, Color{255, 255, 255, 10});
    DrawRectangleLinesEx(g, 1, Color{255, 255, 255, 26});

    // The vertical scale is CPU share of ONE core, headroom to the number of
    // cores actually present — a share above 1.0 is real and worth seeing, not
    // a bug, and clamping it at 100% would hide exactly the case the limiter
    // exists for.
    float peak = 1.0f;
    for (const auto& s : m_perfHistory) peak = std::max(peak, s.cpuShare);
    peak = std::ceil(peak * 2.0f) / 2.0f; // snap to half-core marks

    for (int i = 1; i < 4; ++i) {
        const float y = g.y + g.height * i / 4.0f;
        DrawLineV({g.x, y}, {g.x + g.width, y}, Color{255, 255, 255, 14});
    }

    if (m_perfHistory.size() >= 2) {
        const float dx = g.width / (float)(PERF_HISTORY - 1);
        const float x0 = g.x + g.width - dx * (m_perfHistory.size() - 1);
        Vector2 prevCpu{}, prevBudget{};
        for (size_t i = 0; i < m_perfHistory.size(); ++i) {
            const auto& s = m_perfHistory[i];
            const float x = x0 + dx * i;
            const Vector2 cpuPt = {x, g.y + g.height * (1.0f - std::min(s.cpuShare, peak) / peak)};
            // The limit is drawn on the same axis as the measurement, so
            // "did the cap actually bite?" is answerable by looking.
            const Vector2 budPt = {x, g.y + g.height * (1.0f - std::min(s.budget, peak) / peak)};
            if (i) {
                DrawLineEx(prevCpu, cpuPt, 1.6f, Color{120, 220, 160, 235});
                DrawLineV(prevBudget, budPt, Color{255, 200, 90, 150});
            }
            prevCpu = cpuPt; prevBudget = budPt;
        }
    } else {
        DrawText("sampling...", (int)g.x + 8, (int)(g.y + g.height / 2 - 5), 12,
                 Color{150, 150, 160, 180});
    }
    DrawText(TextFormat("%.1f cores", peak), (int)g.x + 4, (int)g.y + 3, 10,
             Color{170, 175, 190, 190});

    // Legend + live readouts
    const int ly = (int)(g.y + g.height) + 8;
    DrawRectangle((int)g.x, ly + 5, 10, 2, Color{120, 220, 160, 235});
    DrawText("cpu used", (int)g.x + 14, ly, 11, Color{170, 180, 190, 210});
    DrawRectangle((int)g.x + 82, ly + 5, 10, 2, Color{255, 200, 90, 190});
    DrawText("limit", (int)g.x + 96, ly, 11, Color{170, 180, 190, 210});

    const float share = m_perfHistory.empty() ? 0.0f : m_perfHistory.back().cpuShare;
    std::string live = TextFormat("now %.0f%% of a core", share * 100.0f);
    if (m_lastTurnMs > 0.0f) live += TextFormat("   turn %.0f ms", m_lastTurnMs);
    DrawText(live.c_str(), (int)g.x, ly + 18, 12, Color{200, 205, 215, 225});
}

void Game::throttleForBudget(double workSeconds, double maxSleepSeconds) {
#ifdef __EMSCRIPTEN__
    // Blocking the browser's main thread does not yield the CPU to anything,
    // it just freezes the page. The frame cap is the only lever in a web build.
    (void)workSeconds; (void)maxSleepSeconds;
    return;
#else
    const float b = resourceBudget();
    if (b >= 0.999f) { m_budgetEpochWall = 0.0; return; }

    // CLOSED loop, against measured process CPU.
    //
    // This used to size the sleep from the turn's own duration alone: work for
    // `b` of the time, idle for the rest. That is only correct if the turn is
    // the only thing burning CPU, and it is not — the render loop runs between
    // turns, and the learning step now spreads across four worker threads, so
    // CPU time accrues faster than the one-thread-at-a-time model assumed. The
    // result was a process measured at 94% of a core while the limiter was set
    // to 77% and believed it was holding.
    //
    // Asking the OS how much CPU we have actually consumed, and sleeping until
    // the ratio comes back under the budget, targets the number the panel
    // graphs — every thread, render included — instead of a proxy for it.
    const double nowWall = GetTime();
    const double nowCpu = processCpuSeconds();
    if (nowCpu < 0.0) {
        // No per-process accounting (web builds): fall back to the old
        // open-loop estimate rather than not throttling at all.
        if (workSeconds <= 0.0) return;
        const double s = std::min(workSeconds * (1.0 / (double)b - 1.0), maxSleepSeconds);
        if (s > 0.0005) std::this_thread::sleep_for(std::chrono::duration<double>(s));
        return;
    }
    if (m_budgetEpochWall <= 0.0) {          // window not open yet (see init)
        m_budgetEpochWall = nowWall;
        m_budgetEpochCpu = nowCpu;
        return;
    }

    const double dCpu = nowCpu - m_budgetEpochCpu;
    const double dWall = nowWall - m_budgetEpochWall;
    // Wall time that much CPU is allowed to have taken at this budget, minus
    // what has already elapsed: the sleep this loop still OWES.
    const double owed = dCpu / (double)b - dWall;

    // The debt is carried, never forgiven. An earlier version rolled the
    // window on a timer, which reset the baseline to "now" and wiped whatever
    // overshoot had built up since the last sleep — and because CPU keeps
    // accruing after each throttle point (the dashboard draw, the next turn's
    // work), there is always some. That leak was worth a consistent ~20%:
    // 0.48 cores measured against a 0.40 budget, 0.94 against 0.77.
    if (owed > 0.0005)
        std::this_thread::sleep_for(std::chrono::duration<double>(
            std::min(owed, maxSleepSeconds)));

    // Credit, on the other hand, is bounded. Without this an idle stretch
    // would bank unlimited entitlement and the next burst would run
    // unthrottled for as long as the idling lasted.
    if (owed < -BUDGET_WINDOW_SECONDS) {
        m_budgetEpochWall = GetTime();
        m_budgetEpochCpu = processCpuSeconds();
    }
#endif
}

void Game::drawPauseMenu() {
    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 160});

    int centerX = m_screenW / 2;
    int centerY = m_screenH / 2;
    int fontSize = 30;

    // Save feedback popup
    if (m_saveFeedbackTimer > 0 && !m_saveFeedback.empty()) {
        int fbW = MeasureText(m_saveFeedback.c_str(), 20);
        DrawRectangle(centerX - fbW / 2 - 20, m_screenH - 120, fbW + 40, 40, {40, 120, 80, 220});
        DrawRectangleRoundedLines({(float)(centerX - fbW / 2 - 20), (float)(m_screenH - 120), (float)(fbW + 40), 40}, 0.1f, 8, {100, 200, 150, 255});
        DrawText(m_saveFeedback.c_str(), centerX - fbW / 2, m_screenH - 110, 20, WHITE);
    }

    if (m_inSettings) {
        int itemH = 80;
        int tabY = 100;
        int startY = tabY + 70;
        int maxVisible = std::max(1, (m_screenH - startY - 20) / itemH);

        // Draw tab bar
        int tabSpacing = 200;
        int visibleTabs = 0;
        for (int t = 0; t < TAB_COUNT; ++t) {
            if (t == 4 && !m_config.debugMode) continue;
            ++visibleTabs;
        }
        int tabStartX = centerX - (visibleTabs * tabSpacing) / 2 + tabSpacing / 2;
        int tabIdx = 0;
        for (int t = 0; t < TAB_COUNT; ++t) {
            if (t == 4 && !m_config.debugMode) continue;
            int tx = tabStartX + tabIdx * tabSpacing;
            bool active = (t == m_settingsTab);
            Color tc = active ? hexToColor(m_config.accentColor) : LIGHTGRAY;
            DrawText(TAB_NAMES[t], tx - MeasureText(TAB_NAMES[t], fontSize) / 2, tabY, fontSize, tc);
            if (active) {
                int tw = MeasureText(TAB_NAMES[t], fontSize);
                DrawRectangle(tx - tw / 2, tabY + fontSize + 4, tw, 3, tc);
            }
            ++tabIdx;
        }

        // Draw search box for keybinds tab (below tab bar, above items)
        if (m_settingsTab == 3) {
            int sbY = tabY + fontSize + 8;
            int sbW = 300;
            int sbH = 24;
            int sbX = centerX - sbW / 2;
            Color sbBg = m_keybindFilterActive ? Color{255, 255, 255, 30} : Color{255, 255, 255, 16};
            DrawRectangle(sbX, sbY, sbW, sbH, sbBg);
            Color sbBorder = m_keybindFilterActive ? ColorAlpha(hexToColor(m_config.accentColor), 180.0f/255.0f) : Color{255, 255, 255, 50};
            DrawRectangleLines(sbX, sbY, sbW, sbH, sbBorder);
            std::string searchText = m_keybindFilter.empty() ? "Search keybinds..." : m_keybindFilter;
            Color sc = m_keybindFilter.empty() ? Color{120, 120, 140, 180} : WHITE;
            DrawText(searchText.c_str(), sbX + 6, sbY + 5, 13, sc);
            if (m_keybindFilterActive && !m_keybindFilter.empty()) {
                int sw = MeasureText(m_keybindFilter.c_str(), 13);
                if ((int)(GetTime() * 2) % 2) DrawRectangle(sbX + 6 + sw, sbY + 5, 2, 13, WHITE);
            }
        }

        // Draw settings items for current tab (compacted for filter/collapse)
        const Setting* items = TAB_ITEMS[m_settingsTab];
        int count = TAB_ITEM_COUNTS[m_settingsTab];

        Vector2 mouse = getMouse();
        int hovered = -1;
        int resetHovered = -1;

        // Build visible index list (skip collapsed/filtered items)
        std::vector<int> s_visible;
        auto isItemSkipped = [&](int idx) -> bool {
            const Setting& s = items[idx];
            if (m_settingsTab != 3) return false;
            if (s.actionId < 0 && s.label[0] == '-' && s.label[1] == '-') return false; // headers always shown
            // Check collapsed section
            for (int si = idx - 1; si >= 0; --si) {
                bool siHeader = (items[si].actionId < 0 && items[si].label[0] == '-' && items[si].label[1] == '-');
                if (siHeader) {
                    if (m_collapsedSections.count(si) > 0) return true;
                    break;
                }
            }
            // Check search filter
            if (!m_keybindFilter.empty()) {
                std::string lower = s.label;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                std::string filter = m_keybindFilter;
                std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
                if (lower.find(filter) == std::string::npos) {
                    if (s.actionId >= 0) {
                        std::string kn = keyName(m_config.keybinds[s.actionId]);
                        std::transform(kn.begin(), kn.end(), kn.begin(), ::tolower);
                        if (kn.find(filter) == std::string::npos) return true;
                    } else return true;
                }
            }
            return false;
        };
        for (int i = 0; i < count; ++i)
            if (!isItemSkipped(i)) s_visible.push_back(i);
        int visCount = (int)s_visible.size();
        int maxVisScroll = std::max(0, visCount - maxVisible);
        int effScroll = std::min(m_settingsScroll, maxVisScroll);

        // Hover detection over visible items
        for (int vi = 0; vi < visCount; ++vi) {
            int i = s_visible[vi];
            int y = startY + (vi - effScroll) * itemH;
            bool isHeader = (items[i].actionId < 0 && items[i].label[0] == '-' && items[i].label[1] == '-');
            std::string label;
            if (isHeader && m_settingsTab == 3) {
                // Use same label as draw for accurate hover rect
                bool collapsed = m_collapsedSections.count(i) > 0;
                label = std::string(collapsed ? "[+] " : "[-] ") + (items[i].label + 2);
            } else {
                label = (m_editingValue && i == m_settingsIndex)
                    ? m_editBuffer + ((int)(GetTime() * 2) % 2 ? "_" : " ")
                    : makeSettingLabel(m_settingsTab, i, m_config);
            }
            int hoverFont = (isHeader && m_settingsTab == 3) ? 16 : fontSize;
            int tw = MeasureText(label.c_str(), hoverFont);
            Rectangle rect = { (float)(centerX - tw / 2 - 20), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10) };
            if (CheckCollisionPointRec(mouse, rect)) { hovered = i; }
            // Reset button hover
            if (!m_editingValue && (TAB_ITEMS[m_settingsTab][i].isValue || 
                strcmp(TAB_ITEMS[m_settingsTab][i].label, "Fullscreen") == 0 ||
                strcmp(TAB_ITEMS[m_settingsTab][i].label, "Show Actual Flags") == 0 ||
                strcmp(TAB_ITEMS[m_settingsTab][i].label, "Debug Mode") == 0 ||
                strcmp(TAB_ITEMS[m_settingsTab][i].label, "FPS") == 0 ||
                strcmp(TAB_ITEMS[m_settingsTab][i].label, "Now Playing Toast") == 0 ||
                strcmp(TAB_ITEMS[m_settingsTab][i].label, "Map Atmosphere") == 0 ||
                isVolumeSetting(m_settingsTab, i) ||
                (m_settingsTab == 3 && TAB_ITEMS[m_settingsTab][i].actionId >= 0))) {
                int smFont = 24;
                const char* rl = "R";
                int rw = MeasureText(rl, smFont);
                float rrx = (m_settingsTab == 0 && i == 5) ? (float)(centerX + 175) : (float)(centerX + tw/2 + 14);
                Rectangle rr = { rrx, (float)(y + 5), (float)(rw + 16), (float)(smFont + 8) };
                if (CheckCollisionPointRec(mouse, rr)) resetHovered = i;
            }
        }

        // Draw visible items
        for (int vi = 0; vi < visCount; ++vi) {
            int i = s_visible[vi];
            int y = startY + (vi - effScroll) * itemH;
            if (y + itemH < startY || y > m_screenH) continue;

            bool isHeader = (items[i].actionId < 0 && items[i].label[0] == '-' && items[i].label[1] == '-');

            if (isHeader) {
                bool collapsed = m_collapsedSections.count(i) > 0;
                const char* arrow = collapsed ? "[+]" : "[-]";
                std::string headerLabel = std::string(arrow) + " " + (items[i].label + 2);
                DrawText(headerLabel.c_str(), centerX - MeasureText(headerLabel.c_str(), 16) / 2, y + 6, 16, Color{180, 180, 200, 180});
                DrawLine(centerX - 260, y + itemH - 2, centerX + 260, y + itemH - 2, Color{180, 180, 200, 40});
                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && hovered == i) {
                    if (collapsed) m_collapsedSections.erase(i);
                    else m_collapsedSections.insert(i);
                }
                continue;
            }

            bool isSelected = (i == m_settingsIndex);
            bool isHovered = (i == hovered);
            bool isEditing = m_editingValue && i == m_settingsIndex;

            Color textColor = isSelected ? hexToColor(m_config.accentColor) : (isHovered ? WHITE : LIGHTGRAY);
            Color bgColor = isEditing ? Color{255, 255, 255, 20} : (isHovered ? Color{255, 255, 255, 16} : BLANK);

            std::string label;
            if (m_waitingForKey && m_rebindingAction >= 0 && items[i].actionId == m_rebindingAction) {
                label = std::string(items[i].label) + ": ...";
            } else if (isEditing) {
                label = makeSettingLabel(m_settingsTab, i, m_config);
                size_t colon = label.find(':');
                if (colon != std::string::npos) {
                    label = label.substr(0, colon + 2) + "[" + m_editBuffer + ((int)(GetTime() * 2) % 2 ? "_" : " ") + "]";
                } else {
                    label = "[" + m_editBuffer + ((int)(GetTime() * 2) % 2 ? "_" : " ") + "]";
                }
            } else {
                label = makeSettingLabel(m_settingsTab, i, m_config);
            }

            int tw = MeasureText(label.c_str(), fontSize);
            Rectangle rect;
            if (m_settingsTab == 0 && i == 5) {
                rect = { (float)(centerX - 180), (float)(y - 5), 360, (float)(itemH - 10) };
            } else {
                rect = { (float)(centerX - tw / 2 - 20), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10) };
            }
            DrawRectangleRounded(rect, 0.1f, 8, bgColor);
            DrawText(label.c_str(), centerX - tw / 2, y, fontSize, textColor);
            // The volume rows get a bar instead of the selection underline --
            // the two sit four pixels apart and read as one smudged double
            // line. Selection shows as the knob growing instead.
            if (isVolumeSetting(m_settingsTab, i)) {
                drawVolumeSlider(i, y, centerX, isSelected);
            } else if (isSelected && !isEditing) {
                int lineW = (m_settingsTab == 0 && i == 5) ? 320 : tw + 20;
                DrawRectangle(centerX - lineW / 2, y + fontSize + 4, lineW, 2, hexToColor(m_config.accentColor));
            }

            // Draw accent color swatch
            if (m_settingsTab == 0 && i == 6) {
                int swatchSize = 28;
                int swatchX = centerX + tw / 2 + 14;
                int swatchY = y + (itemH - swatchSize) / 2;
                Color ac = hexToColor(m_config.accentColor);
                DrawRectangle(swatchX, swatchY, swatchSize, swatchSize, ac);
                DrawRectangleLines(swatchX, swatchY, swatchSize, swatchSize, LIGHTGRAY);
            }

            // Draw reset button for value/toggle items
            if (!isEditing && (items[i].isValue ||
                strcmp(items[i].label, "Fullscreen") == 0 ||
                strcmp(items[i].label, "Show Actual Flags") == 0 ||
                strcmp(items[i].label, "Debug Mode") == 0 ||
                strcmp(items[i].label, "Accent Color") == 0 ||
                strcmp(items[i].label, "Display FPS") == 0 ||
                strcmp(items[i].label, "Display Zoom") == 0 ||
                strcmp(items[i].label, "Console Window") == 0 ||
                strcmp(items[i].label, "AI Debug") == 0 ||
                strcmp(items[i].label, "AI Learning") == 0 ||
                strcmp(items[i].label, "AI Difficulty") == 0 ||
                strcmp(items[i].label, "Resolution") == 0 ||
                strcmp(items[i].label, "FPS") == 0 ||
                strcmp(items[i].label, "Now Playing Toast") == 0 ||
                strcmp(items[i].label, "Map Atmosphere") == 0 ||
                isVolumeSetting(m_settingsTab, i) ||
                (m_settingsTab == 3 && items[i].actionId >= 0))) {
                int smFont = 24;
                const char* rl = "R";
                int rw = MeasureText(rl, smFont);
                float rx = (m_settingsTab == 0 && i == 5) ? (centerX + 175) : (centerX + tw / 2 + 14);
                float ry = (float)(y + 5);
                Color rc = (resetHovered == i) ? hexToColor(m_config.accentColor) : Color{180, 180, 180, 255};
                Rectangle rr = { rx, ry, (float)(rw + 16), (float)(smFont + 8) };
                DrawRectangleRounded(rr, 0.2f, 6, Color{255, 255, 255, 12});
                DrawText(rl, (int)(rx + 5), (int)(ry + 2), smFont, rc);
            }

            // FPS slider -- the same widget as the volumes, stepped rather
            // than continuous. It used to be a second implementation with its
            // own geometry, and a thumb drawn at (idx+0.5)/14 over ticks laid
            // out at idx/13, so the handle never sat on a stop.
            if (m_settingsTab == 0 && i == 5) {
                const Rectangle bar = sliderBarRect(y, centerX);
                const int idx = fpsTargetToIndex(m_config.fpsTarget);
                drawSliderWidget(bar, (float)idx / (float)(FPS_STEPS - 1),
                                 m_settingsIndex == i || m_draggingFpsSlider,
                                 FPS_STEPS);
                const int lbl = 14;
                const int ly = (int)(bar.y + bar.height) + 16;
                DrawText("Unlimited", (int)bar.x, ly, lbl, Color{180, 180, 180, 200});
                DrawText("VSync", (int)(bar.x + bar.width) - MeasureText("VSync", lbl),
                         ly, lbl, Color{180, 180, 180, 200});
            }

        }
    } else {
        // Main pause menu
        int count = MENU_COUNT;
        int itemH = 50;
        int startY = m_screenH / 2 - (count * itemH) / 2;

        Vector2 mouse = getMouse();
        int hovered = -1;
        for (int i = 0; i < count; ++i) {
            int y = startY + i * itemH;
            int tw = MeasureText(MENU_ITEMS[i], fontSize);
            Rectangle rect = { (float)(centerX - tw / 2 - 20), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10) };
            if (CheckCollisionPointRec(mouse, rect)) { hovered = i; break; }
        }

        for (int i = 0; i < count; ++i) {
            int y = startY + i * itemH;
            bool isSelected = (i == m_menuIndex);
            bool isHovered = (i == hovered);
            Color textColor = isSelected ? hexToColor(m_config.accentColor) : (isHovered ? WHITE : LIGHTGRAY);
            Color bgColor = isHovered ? Color{255, 255, 255, 16} : BLANK;

            int tw = MeasureText(MENU_ITEMS[i], fontSize);
            Rectangle rect = { (float)(centerX - tw / 2 - 20), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10) };
            DrawRectangleRounded(rect, 0.1f, 8, bgColor);
            DrawText(MENU_ITEMS[i], centerX - tw / 2, y, fontSize, textColor);

            if (isSelected) {
                int lineW = tw + 20;
                DrawRectangle(centerX - lineW / 2, y + fontSize + 4, lineW, 2, hexToColor(m_config.accentColor));
            }
        }
    }

    // Unsaved changes warning dialog (drawn on top of pause menu)
    if (m_showUnsavedWarning) {
        DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 180});
        int dlgW = 540, dlgH = 220;
        int dlgX = (m_screenW - dlgW) / 2;
        int dlgY = (m_screenH - dlgH) / 2;
        DrawRectangle(dlgX, dlgY, dlgW, dlgH, {20, 20, 30, 240});

        const char* msg = "You have unsaved changes!";
        int msgW = MeasureText(msg, 26);
        DrawText(msg, centerX - msgW / 2, dlgY + 20, 26, hexToColor(m_config.accentColor));

        const char* sub = "Quitting to the main menu will lose your progress.";
        int subW = MeasureText(sub, 16);
        DrawText(sub, centerX - subW / 2, dlgY + 58, 16, LIGHTGRAY);

        const char* sub2 = "What would you like to do?";
        int sub2W = MeasureText(sub2, 16);
        DrawText(sub2, centerX - sub2W / 2, dlgY + 82, 16, LIGHTGRAY);

        // Buttons
        const char* choices[] = {"Save and Quit", "Quit Without Saving", "Cancel"};
        int btnW = 180, btnH = 40;
        int btnY = dlgY + dlgH - 58;
        Vector2 mouse = getMouse();

        for (int c = 0; c < 3; ++c) {
            int bx = centerX - ((3 * btnW + 20) / 2) + c * (btnW + 10);
            Rectangle btn = {(float)bx, (float)btnY, (float)btnW, (float)btnH};
            bool hover = CheckCollisionPointRec(mouse, btn);
            bool active = (c == m_unsavedChoice);

            Color bg;
            if (c == 0) bg = active ? (hover ? Color{50, 130, 80, 255} : Color{40, 100, 60, 255}) : (hover ? Color{40, 100, 60, 200} : Color{30, 70, 45, 200});
            else if (c == 1) bg = active ? (hover ? Color{180, 60, 60, 255} : Color{130, 40, 40, 255}) : (hover ? Color{130, 40, 40, 200} : Color{90, 30, 30, 200});
            else bg = active ? (hover ? Color{80, 80, 90, 255} : Color{60, 60, 70, 255}) : (hover ? Color{60, 60, 70, 200} : Color{40, 40, 50, 200});

            DrawRectangleRounded(btn, 0.15f, 6, bg);
            DrawText(choices[c], bx + (btnW - MeasureText(choices[c], 16)) / 2, btnY + 12, 16, WHITE);

            if (active && m_unsavedChoice == c) {
                DrawRectangleRoundedLines(btn, 0.15f, 6, ColorAlpha(hexToColor(m_config.accentColor), 200.0f/255.0f));
            }
        }
    }
}

void Game::drawHybridText(int x, int y, int fontSize, const char* text, Color color) {
    if (!text) return;
    Font defFont = GetFontDefault();
    int penX = x;
    for (const char* p = text; *p; ) {
        int codepoint = 0;
        int sz = 1;
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) { codepoint = c; sz = 1; }
        else if (c < 0xE0 && p[1]) { codepoint = ((c & 0x1F) << 6) | (p[1] & 0x3F); sz = 2; }
        else if (c < 0xF0 && p[1] && p[2]) { codepoint = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); sz = 3; }
        else if (p[1] && p[2] && p[3]) { codepoint = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); sz = 4; }
        bool isAscii = (codepoint >= 32 && codepoint <= 126);
        Font& f = isAscii ? defFont : m_gameFont;
        if (f.texture.id <= 0) f = defFont;
        int gi = GetGlyphIndex(f, codepoint);
        float advance = 0;
        if (gi >= 0 && gi < f.glyphCount) {
            advance = (float)f.glyphs[gi].advanceX;
            if (advance <= 0) advance = (float)f.recs[gi].width * fontSize / (float)f.baseSize;
        }
        if (advance <= 0) advance = (float)fontSize * 0.6f;
        DrawTextCodepoint(f, codepoint, {(float)penX, (float)y}, (float)fontSize, color);
        penX += (int)(advance + 1);
        p += sz;
    }
}

float Game::glyphAdvance(Font font, int glyphIndex, int fontSize) {
    if (glyphIndex >= 0 && glyphIndex < font.glyphCount) {
        float adv = (float)font.glyphs[glyphIndex].advanceX;
        if (adv > 0) return adv * fontSize / (float)font.baseSize;
        return (float)font.recs[glyphIndex].width * fontSize / (float)font.baseSize;
    }
    return (float)fontSize * 0.6f;
}

std::string flagPatternToSvg(const FlagPattern& fp, int w, int h, const std::unordered_map<std::string, std::string>* odmData) {
    std::string svg;
    svg += "<svg xmlns='http://www.w3.org/2000/svg' width='" + std::to_string(w) + "' height='" + std::to_string(h) + "'>";
    Color col{100, 100, 100, 255};
    if (!fp.colors.empty()) col = fp.colors[0];
    char buf[64];
    snprintf(buf, sizeof(buf), "<rect width='%d' height='%d' fill='#%02x%02x%02x'/>", w, h, col.r, col.g, col.b);
    svg += buf;
    svg += "</svg>";
    return svg;
}

void Game::runMapScripts() {
    if (!m_loadedMapHasScripts) return;
    if (m_scriptEngine) delete m_scriptEngine;
    m_scriptEngine = new ScriptEngine(this);
    m_scriptErrors.clear();
    bool ok = m_scriptEngine->runScripts(m_odmJsonData);
    if (!ok) {
        m_scriptErrors = m_scriptEngine->getErrors();
        m_scriptErrorTimer = 3.0f; // show for 3 seconds
        for (auto& e : m_scriptErrors)
            printf("[SCRIPT] Failed to load: %s.txt error: %s (line %d)\n", e.scriptName.c_str(), e.message.c_str(), e.lineNum);
    }
}

void Game::drawScriptErrors() {
    if (m_scriptErrorTimer <= 0 || m_scriptErrors.empty()) return;
    m_scriptErrorTimer -= GetFrameTime();
    if (m_scriptErrorTimer <= 0) return;

    int y = m_screenH - 20;
    for (int i = (int)m_scriptErrors.size() - 1; i >= 0 && y > 60; i--) {
        auto& e = m_scriptErrors[i];
        std::string msg = "Failed to load script: " + e.scriptName + ".txt error: " + e.message;
        if (e.lineNum > 0) msg += " (line " + std::to_string(e.lineNum) + ")";
        int tw = MeasureText(msg.c_str(), 12);
        DrawRectangle(m_screenW - tw - 16, y - 14, tw + 12, 18, {0, 0, 0, 200});
        DrawText(msg.c_str(), m_screenW - tw - 10, y - 12, 12, {255, 100, 100, 255});
        y -= 20;
    }
}



