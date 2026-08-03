#pragma once

#include <string>
#include "raylib.h"

struct Config;
struct Setting { const char* label; bool isValue; int actionId; };

// NOTE ON THE ARRAYS BELOW: their bounds are deliberately left off.
//
// A bound written here does not describe the array -- it DEFINES it. C++ takes
// an omitted bound in the definition from the earlier declaration, so if this
// header said [27] and Game.cpp listed 25 rows, the array really would be 27,
// the last two zero-filled with a NULL label, and sizeof()/sizeof([0]) would
// report 27. The settings menu then walked onto those rows and dereferenced
// the null label. Leaving the bound off makes the initializer the single
// source of truth, and the static_asserts in Game.cpp keep each hand-written
// *_COUNT honest.

Color hexToColor(int hex);
std::string formatPop(long long pop);
std::string formatTroops(long long men);
// FlagPattern -> the JSON shape CountryMap::parseFlag() reads back.
std::string flagPatternToJsonString(const FlagPattern& fp);

extern const char* MENU_ITEMS[];
extern const int MENU_COUNT;
extern const char* MAIN_MENU_ITEMS[];
extern const int MAIN_MENU_COUNT;
extern const char* SINGLEPLAYER_ITEMS[];
extern const int SINGLEPLAYER_COUNT;
extern const char* GAME_VERSION;
extern const char* TAB_NAMES[];
extern const int TAB_COUNT;
extern const int RESOLUTIONS[][2];
extern const int RES_COUNT;
extern const Setting DISPLAY_ITEMS[];
extern const char* AI_DIFFICULTY_NAMES[];
extern const int AI_DIFFICULTY_COUNT;
extern const int DISPLAY_COUNT;
extern const int ACCENT_PRESETS[];
extern const int ACCENT_PRESETS_COUNT;
extern const Setting CONTROLS_ITEMS[];
extern const int CONTROLS_COUNT;
extern const Setting AUDIO_ITEMS[];
extern const int AUDIO_COUNT;
extern const float VOLUME_DEFAULTS[];
extern const Setting KEYBINDS_ITEMS[];
extern const int KEYBINDS_COUNT;
extern const Setting ADVANCED_ITEMS[];
extern const int ADVANCED_COUNT;
extern const Setting EXPERIMENTAL_ITEMS[];
extern const int EXPERIMENTAL_COUNT;
extern const Setting* TAB_ITEMS[6];
extern const int TAB_ITEM_COUNTS[];
extern float FLY_SPEED_VALS[];
extern const int FLY_SPEED_COUNT;
extern float MAX_ZOOM_VALS[];
extern const int MAX_ZOOM_COUNT;

// The Audio tab, and the volume rows at the top of it. Both settings screens
// (the one reached from the main menu and the one reached in-game) draw these
// rows as sliders rather than as plain text, so both have to recognise them;
// naming the tab here is what keeps that from being a bare 2 in a dozen places.
constexpr int AUDIO_TAB = 2;
constexpr int VOLUME_COUNT = 3;

bool isVolumeSetting(int tab, int index);
/** The Config field a volume row edits, or nullptr when the row is not one. */
float* volumeSettingPtr(Config& cfg, int tab, int index);
/** Pushes all three config volumes into the audio device. */
void applyVolumes(const Config& cfg);

const char* keyName(int key);
// Unlimited, 10..120 in tens, VSync. One name, because the count was spelled
// out as a bare 14 (and its last index as a bare 13) in five separate places.
constexpr int FPS_STEPS = 14;
int fpsTargetToIndex(int target);
int indexToFpsTarget(int idx);
int nearestIndex(float val, float* vals, int count);
std::string makeSettingLabel(int tab, int index, const Config& cfg);
void applyFpsTarget(int target);

// ── Resource limiter (runtime panel, F10 / Ctrl+L) ──
// The budget lives in a translation-unit global rather than being threaded
// through applyFpsTarget's signature because the frame cap has to hold for the
// callers that pass a fixed target and know nothing about the config — the
// training loop asks for unlimited FPS, and an unlimited request is exactly the
// one a throttled machine must not honour.
constexpr float RESOURCE_BUDGET_MIN = 0.10f;
/** Clamped to [RESOURCE_BUDGET_MIN, 1]. Call applyFpsTarget after changing it. */
void setResourceBudget(float budget);
/** Reseed the turn resolver's PRNG; called per map so a seed replays a world. */
void seedSimRng(unsigned int seed);
float resourceBudget();
/** Frame ceiling the budget implies, or 0 when the budget is unlimited. */
int budgetedFpsCeiling();

void forceWindowResize(int w, int h);
void setFullscreenAttrs(bool fullscreen, int* x, int* y, int* w, int* h);

#include <unordered_map>
#include "renderer/FlagRenderer.h"
std::string flagPatternToSvg(const FlagPattern& fp, int w, int h,
                             const std::unordered_map<std::string, std::string>* odmData = nullptr);
