#pragma once

#include <string>
#include "raylib.h"

struct Config;
struct Setting { const char* label; bool isValue; int actionId; };

Color hexToColor(int hex);
std::string formatPop(long long pop);

extern const char* MENU_ITEMS[5];
extern const int MENU_COUNT;
extern const char* MAIN_MENU_ITEMS[6];
extern const int MAIN_MENU_COUNT;
extern const char* SINGLEPLAYER_ITEMS[2];
extern const int SINGLEPLAYER_COUNT;
extern const char* GAME_VERSION;
extern const char* TAB_NAMES[5];
extern const int TAB_COUNT;
extern const int RESOLUTIONS[5][2];
extern const int RES_COUNT;
extern const Setting DISPLAY_ITEMS[8];
extern const int DISPLAY_COUNT;
extern const int ACCENT_PRESETS[10];
extern const int ACCENT_PRESETS_COUNT;
extern const Setting CONTROLS_ITEMS[2];
extern const int CONTROLS_COUNT;
extern const Setting AUDIO_ITEMS[1];
extern const int AUDIO_COUNT;
extern const Setting KEYBINDS_ITEMS[27];
extern const int KEYBINDS_COUNT;
extern const Setting ADVANCED_ITEMS[4];
extern const int ADVANCED_COUNT;
extern const Setting* TAB_ITEMS[5];
extern const int TAB_ITEM_COUNTS[5];
extern float FLY_SPEED_VALS[6];
extern const int FLY_SPEED_COUNT;
extern float MAX_ZOOM_VALS[7];
extern const int MAX_ZOOM_COUNT;

const char* keyName(int key);
int fpsTargetToIndex(int target);
int indexToFpsTarget(int idx);
int nearestIndex(float val, float* vals, int count);
std::string makeSettingLabel(int tab, int index, const Config& cfg);
void applyFpsTarget(int target);
void forceWindowResize(int w, int h);
void setFullscreenAttrs(bool fullscreen, int* x, int* y, int* w, int* h);

#include <unordered_map>
#include "renderer/FlagRenderer.h"
std::string flagPatternToSvg(const FlagPattern& fp, int w, int h,
                             const std::unordered_map<std::string, std::string>* odmData = nullptr);
