#include "Game.h"
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
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <ctime>
#include <random>
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
const char* MAIN_MENU_ITEMS[] = {"Play Singleplayer", "Play Multiplayer", "Map Editor", "Mod Menu", "Community", "Credits"};
const int MAIN_MENU_COUNT = 6;
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
const Setting AUDIO_ITEMS[] = {{"Back", false, -1}};
const int AUDIO_COUNT = 1;

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
    {"Back", false, -1},
};
const int ADVANCED_COUNT = 5;

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

void applyFpsTarget(int target) {
    ClearWindowState(FLAG_VSYNC_HINT);
    SetTargetFPS(0);
    if (target == 0) {
        SetWindowState(FLAG_VSYNC_HINT);
    } else if (target > 0) {
        SetTargetFPS(target);
    }
    // target == -1 = unlimited, already set above
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
    } else if (tab == 4 && index == 0) {
        label += cfg.showFps ? ": On" : ": Off";
    } else if (tab == 4 && index == 1) {
        label += cfg.showZoom ? ": On" : ": Off";
    } else if (tab == 4 && index == 2) {
        label += cfg.showConsole ? ": On" : ": Off";
    } else if (tab == 4 && index == 3) {
        label += cfg.aiDebug ? ": On" : ": Off";
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
    emscripten_run_script("console.log('[OD] Game::init() entered')");
    // On Emscripten, GetApplicationDirectory() returns a URL, not a filesystem path.
    // Data is preloaded into the virtual FS at /data/ via --preload-file.
    m_dataDir = "/data/";
    // Debug: list preloaded files
    emscripten_run_script("try { var dir = FS.readdir('/data/'); console.log('[OD] /data/ contents: ' + JSON.stringify(dir)); } catch(e) { console.log('[OD] Failed to read /data/: ' + e.toString()); }");
#else
    std::string appDir = GetApplicationDirectory();
    m_dataDir = appDir + "../data/";
#endif
    m_configPath = m_dataDir + "config.json";
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
    SetExitKey(0);
    m_dpiScale = GetWindowScaleDPI().x;

    // Redirect stdout/stderr to in-game console
    m_consoleBuf = new ConsoleBuf(this);
    m_origCout = std::cout.rdbuf(m_consoleBuf);
    m_origCerr = std::cerr.rdbuf(m_consoleBuf);

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
    m_screenW = GetScreenWidth();
    m_screenH = GetScreenHeight();
    emscripten_run_script(
        "(function(){"
        "var c=document.getElementById('canvas');"
        "console.log('[OD] canvas:',c?'element='+c.width+'x'+c.height+' css='+c.clientWidth+'x'+c.clientHeight:'NO CANVAS');"
        "console.log('[OD] window.inner:',window.innerWidth+'x'+window.innerHeight);"
        "})()"
    );
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

    applyFpsTarget(m_config.fpsTarget);

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

    std::cout << "OpenDoctrines initialized. " << m_screenW << "x" << m_screenH << std::endl;
#ifdef __EMSCRIPTEN__
    emscripten_run_script("console.log('[OD] Game::init() SUCCESS')");
#endif
    m_running = true;
    m_currentScreen = SCREEN_SPLASH;
    m_splashTimer = 0.0f;
    initMenuBackground(); // ready so the splash's fade-out can reveal it
    m_menuBgScroll = 0;
    return true;
}

void Game::shutdown() {
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

void Game::cycleShip(int direction) {
    if (m_countryShipIndices.empty()) return;
    int newIdx = m_countryShipIndex + direction;
    if (newIdx < 0) newIdx = (int)m_countryShipIndices.size() - 1;
    if (newIdx >= (int)m_countryShipIndices.size()) newIdx = 0;
    m_countryShipIndex = newIdx;
    int shipIdx = m_countryShipIndices[m_countryShipIndex];
    if (shipIdx >= 0 && shipIdx < (int)m_ships.size()) {
        m_selectedShipIndices = {shipIdx};
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
    DrawText("x", (int)closeX + 6, (int)c.rect.y + 4, 14, closeHov ? WHITE : (Color){180, 180, 200, 200});
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
    // Show FPS with GetFPS() when enabled
    if (m_config.showFps) {
        int fps = GetFPS();
        const char* fpsStr = TextFormat("FPS: %d", fps);
        Color fpsColor = fps >= 55 ? (Color){100, 255, 100, 220} : (fps >= 30 ? (Color){255, 255, 100, 220} : (Color){255, 100, 100, 220});
        DrawText(fpsStr, 10, 10, 18, fpsColor);
    }

    if (m_config.showZoom) {
        int provCount = (int)m_provinces.getAllProvinces().size();
        float zoom = m_renderer ? m_renderer->getZoom() : 1.0f;
        DrawText(TextFormat("Provinces: %d", provCount), 10, 32, 14, (Color){180, 220, 255, 200});
        DrawText(TextFormat("Zoom: %.2f", zoom), 10, 48, 14, (Color){180, 220, 255, 200});
        DrawText(TextFormat("DPI: %.2f", m_dpiScale), 10, 64, 14, (Color){140, 160, 180, 160});
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
#ifdef __EMSCRIPTEN__
    emscripten_run_script("console.log('[OD] Game::run() entered')");
#endif
    int frameCount = 0;
    while (m_running && !WindowShouldClose()) {
        frameCount++;
#ifdef __EMSCRIPTEN__
        static int frameCount = 0;
        if (frameCount % 60 == 0) {
            Vector2 mp = GetMousePosition();
            Vector2 ms = getMouse();
            emscripten_run_script(("console.log('[OD] mouse: GetMousePos=" + std::to_string(mp.x) + "," + std::to_string(mp.y) + " getMouse=" + std::to_string(ms.x) + "," + std::to_string(ms.y) + " screen=" + std::to_string(m_screenW) + "x" + std::to_string(m_screenH) + "')").c_str());
        }
        frameCount++;
#endif
        float dt = GetFrameTime();
        
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
            if (m_config.debugMode) drawDebugOverlay();
            EndDrawing();
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
            } else if (m_currentScreen == SCREEN_FILE_BROWSER || m_currentScreen == SCREEN_MAP_SELECT || m_currentScreen == SCREEN_COUNTRY_SELECT || m_currentScreen == SCREEN_CREDITS || m_currentScreen == SCREEN_COMMUNITY || m_currentScreen == SCREEN_MAP_EDITOR || m_currentScreen == SCREEN_MODS) {
                if (m_screenW != m_menuBgInitScreenW || m_screenH != m_menuBgInitScreenH) {
                    initMenuBackground();
                }
            }
        }

        // On entering any menu screen, re-init background if screen size changed
        // (covers transitions back from gameplay/country-select without a resize event)
        // Handle all menu screen types, but only init once per resize
        if ((m_currentScreen == SCREEN_MENU || m_currentScreen == SCREEN_SINGLEPLAYER ||
             m_currentScreen == SCREEN_FILE_BROWSER || m_currentScreen == SCREEN_MAP_SELECT || m_currentScreen == SCREEN_CREDITS || m_currentScreen == SCREEN_COMMUNITY || m_currentScreen == SCREEN_MAP_EDITOR || m_currentScreen == SCREEN_MODS) &&
            !IsWindowResized() &&
            (m_screenW != m_menuBgInitScreenW || m_screenH != m_menuBgInitScreenH)) {
            initMenuBackground();
        }

        if (m_currentScreen == SCREEN_SPLASH) {
            updateSplashScreen(dt);
            BeginDrawing();
            ClearBackground(BLACK);
            drawSplashScreen();
            EndDrawing();
        } else if (m_currentScreen == SCREEN_MENU) {
            if (m_inSettings) {
                updateMenuBackground();
                updateSettingsFromMenu();
                BeginDrawing();
                ClearBackground(BLACK);
                drawSettingsFromMenu();
                if (m_config.showConsole) drawConsoleWindow();
                if (m_config.debugMode) drawDebugOverlay();
                EndDrawing();
            } else {
                updateMenuBackground();
                updateMainMenu();
                BeginDrawing();
                ClearBackground(BLACK);
                drawMainMenu();
                if (m_config.showConsole) drawConsoleWindow();
                if (m_config.debugMode) drawDebugOverlay();
                EndDrawing();
            }
        } else if (m_currentScreen == SCREEN_SINGLEPLAYER) {
            updateMenuBackground();
            updateSingleplayerMenu();
            BeginDrawing();
            ClearBackground(BLACK);
            drawSingleplayerMenu();
            if (m_config.showConsole) drawConsoleWindow();
            if (m_config.debugMode) drawDebugOverlay();
            EndDrawing();
        } else if (m_currentScreen == SCREEN_FILE_BROWSER) {
            if (m_inHistory) {
                // Turn History opened from a save's World Settings takes over
                // the whole screen until closed.
                updateHistoryScreen();
                BeginDrawing();
                ClearBackground(BLACK);
                drawHistoryScreen();
                if (m_config.showConsole) drawConsoleWindow();
                EndDrawing();
            } else if (m_browsingSaves) {
                updateMenuBackground();
                updateWorldBrowser();
                BeginDrawing();
                ClearBackground(BLACK);
                drawWorldBrowser();
                if (m_config.showConsole) drawConsoleWindow();
                if (m_config.debugMode) drawDebugOverlay();
                EndDrawing();
            } else {
                updateMenuBackground();
                updateFileBrowser();
                BeginDrawing();
                ClearBackground(BLACK);
                drawFileBrowser();
                if (m_config.showConsole) drawConsoleWindow();
                if (m_config.debugMode) drawDebugOverlay();
                EndDrawing();
            }
        } else if (m_currentScreen == SCREEN_MAP_SELECT) {
            updateMenuBackground();
            updateMapBrowser();
            BeginDrawing();
            ClearBackground(BLACK);
            drawMapBrowser();
            if (m_config.showConsole) drawConsoleWindow();
            if (m_config.debugMode) drawDebugOverlay();
            EndDrawing();
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
                    updateLoading();
                }
            }
            BeginDrawing();
            ClearBackground(BLACK);
            if (m_showLoadingScreen) {
                drawLoadingScreen();
            } else {
                // Loading completed — present one final frame then transition
                EndDrawing();
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
            if (m_config.debugMode) drawDebugOverlay();
            EndDrawing();
        } else if (m_currentScreen == SCREEN_CREDITS) {
            updateMenuBackground();
            updateCredits();
            BeginDrawing();
            ClearBackground(BLACK);
            drawCredits();
            if (m_config.showConsole) drawConsoleWindow();
            if (m_config.debugMode) drawDebugOverlay();
            EndDrawing();
        } else if (m_currentScreen == SCREEN_COMMUNITY) {
            updateMenuBackground();
            updateCommunityMenu();
            BeginDrawing();
            ClearBackground(BLACK);
            drawCommunityMenu();
            if (m_config.showConsole) drawConsoleWindow();
            if (m_config.debugMode) drawDebugOverlay();
            EndDrawing();
        } else if (m_currentScreen == SCREEN_MODS) {
            updateMenuBackground();
            updateModsMenu();
            BeginDrawing();
            ClearBackground(BLACK);
            drawModsMenu();
            if (m_config.showConsole) drawConsoleWindow();
            if (m_config.debugMode) drawDebugOverlay();
            EndDrawing();
        } else if (m_currentScreen == SCREEN_MAP_EDITOR) {
            if (m_mapEditor) {
                updateMapEditor();
                BeginDrawing();
                ClearBackground(BLACK);
                drawMapEditor();
                if (m_config.showConsole) drawConsoleWindow();
                if (m_config.debugMode) drawDebugOverlay();
                EndDrawing();
            } else {
                m_currentScreen = SCREEN_MENU;
            }
        } else {
            update(dt);
            draw();
        }
    }
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
            Color sbBg = m_keybindFilterActive ? (Color){255, 255, 255, 30} : (Color){255, 255, 255, 16};
            DrawRectangle(sbX, sbY, sbW, sbH, sbBg);
            Color sbBorder = m_keybindFilterActive ? ColorAlpha(hexToColor(m_config.accentColor), 180.0f/255.0f) : (Color){255, 255, 255, 50};
            DrawRectangleLines(sbX, sbY, sbW, sbH, sbBorder);
            std::string searchText = m_keybindFilter.empty() ? "Search keybinds..." : m_keybindFilter;
            Color sc = m_keybindFilter.empty() ? (Color){120, 120, 140, 180} : WHITE;
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
                DrawText(headerLabel.c_str(), centerX - MeasureText(headerLabel.c_str(), 16) / 2, y + 6, 16, (Color){180, 180, 200, 180});
                DrawLine(centerX - 260, y + itemH - 2, centerX + 260, y + itemH - 2, (Color){180, 180, 200, 40});
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
            Color bgColor = isEditing ? (Color){255, 255, 255, 20} : (isHovered ? (Color){255, 255, 255, 16} : BLANK);

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
            if (isSelected && !isEditing) {
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
                (m_settingsTab == 3 && items[i].actionId >= 0))) {
                int smFont = 24;
                const char* rl = "R";
                int rw = MeasureText(rl, smFont);
                float rx = (m_settingsTab == 0 && i == 5) ? (centerX + 175) : (centerX + tw / 2 + 14);
                float ry = (float)(y + 5);
                Color rc = (resetHovered == i) ? hexToColor(m_config.accentColor) : (Color){180, 180, 180, 255};
                Rectangle rr = { rx, ry, (float)(rw + 16), (float)(smFont + 8) };
                DrawRectangleRounded(rr, 0.2f, 6, (Color){255, 255, 255, 12});
                DrawText(rl, (int)(rx + 5), (int)(ry + 2), smFont, rc);
            }

            // FPS slider
            if (m_settingsTab == 0 && i == 5) {
                int sliderW = 300;
                int sliderH = 8;
                int sliderX = centerX - sliderW / 2;
                int sliderY = y + 28;
                int thumbR = 9;

                DrawRectangle(sliderX, sliderY, sliderW, sliderH, (Color){80, 80, 80, 200});
                int idx = fpsTargetToIndex(m_config.fpsTarget);
                float fillFrac = (idx + 0.5f) / 14.0f;
                DrawRectangle(sliderX, sliderY, (int)(sliderW * fillFrac), sliderH, ColorAlpha(hexToColor(m_config.accentColor), 200.0f/255.0f));

                for (int t = 0; t < 14; ++t) {
                    int tx = sliderX + t * sliderW / 13;
                    int th = (t == 0 || t == 13) ? 12 : 6;
                    DrawRectangle(tx, sliderY + sliderH + 2, 2, th, (Color){140, 140, 140, 200});
                }

                int lblSize = 14;
                DrawText("Unlimited", sliderX, sliderY + sliderH + 16, lblSize, (Color){180, 180, 180, 200});
                DrawText("VSync", sliderX + sliderW - MeasureText("VSync", lblSize), sliderY + sliderH + 16, lblSize, (Color){180, 180, 180, 200});

                int thumbX = sliderX + (int)(fillFrac * sliderW);
                DrawCircle(thumbX, sliderY + sliderH / 2, (float)thumbR, hexToColor(m_config.accentColor));
                DrawCircle(thumbX, sliderY + sliderH / 2, (float)(thumbR - 3), (Color){255, 255, 255, 220});
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
            Color bgColor = isHovered ? (Color){255, 255, 255, 16} : BLANK;

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



