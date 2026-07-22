#pragma once
#include "raylib.h"

enum GameAction {
    ACTION_NEXT_PROVINCE = 0,
    ACTION_PREV_PROVINCE,
    ACTION_ZOOM_IN,
    ACTION_ZOOM_OUT,
    ACTION_ZOOM_TO_PROVINCE,
    ACTION_BOX_SELECT,
    ACTION_ARMY_MOVE,
    ACTION_TAB_1,
    ACTION_TAB_2,
    ACTION_TAB_3,
    ACTION_TAB_4,
    ACTION_TAB_5,
    ACTION_TAB_6,
    ACTION_TAB_7,
    ACTION_TAB_8,
    ACTION_ARTILLERY_WHEEL,
    ACTION_SHIP_MOVE,
    ACTION_SHIP_WHEEL,
    ACTION_SHIP_ENGAGE,
    ACTION_COUNT
};

static constexpr const char* ACTION_NAMES[ACTION_COUNT] = {
    "Next Province",
    "Previous Province",
    "Zoom In",
    "Zoom Out",
    "Zoom to Province",
    "Box Select",
    "Army Move Order",
    "Tab 1 - Political",
    "Tab 2 - Industry",
    "Tab 3 - Military",
    "Tab 4 - Relations",
    "Tab 5 - Army",
    "Tab 6 - Navy",
    "Tab 7 - Resources",
    "Tab 8 - Names",
    "Artillery Wheel",
    "Ship Move Order",
    "Ship Artillery Wheel",
    "Ship Engage Order",
};

// Categories for grouping in settings
static constexpr const char* ACTION_CATEGORIES[ACTION_COUNT] = {
    "Navigation",   // ACTION_NEXT_PROVINCE
    "Navigation",   // ACTION_PREV_PROVINCE
    "Navigation",   // ACTION_ZOOM_IN
    "Navigation",   // ACTION_ZOOM_OUT
    "Navigation",   // ACTION_ZOOM_TO_PROVINCE
    "Selection",    // ACTION_BOX_SELECT
    "Combat",       // ACTION_ARMY_MOVE
    "View Tabs",    // ACTION_TAB_1
    "View Tabs",    // ACTION_TAB_2
    "View Tabs",    // ACTION_TAB_3
    "View Tabs",    // ACTION_TAB_4
    "View Tabs",    // ACTION_TAB_5
    "View Tabs",    // ACTION_TAB_6
    "View Tabs",    // ACTION_TAB_7
    "View Tabs",    // ACTION_TAB_8
    "Combat",       // ACTION_ARTILLERY_WHEEL
    "Combat",       // ACTION_SHIP_MOVE
    "Combat",       // ACTION_SHIP_WHEEL
    "Combat",       // ACTION_SHIP_ENGAGE
};

static constexpr int DEFAULT_KEYBINDS[ACTION_COUNT] = {
    KEY_RIGHT,   // ACTION_NEXT_PROVINCE
    KEY_LEFT,    // ACTION_PREV_PROVINCE
    '=',         // ACTION_ZOOM_IN
    '-',         // ACTION_ZOOM_OUT
    KEY_SPACE,   // ACTION_ZOOM_TO_PROVINCE
    KEY_LEFT_SHIFT, // ACTION_BOX_SELECT
    MOUSE_BUTTON_RIGHT, // ACTION_ARMY_MOVE
    KEY_ONE,     // ACTION_TAB_1
    KEY_TWO,     // ACTION_TAB_2
    KEY_THREE,   // ACTION_TAB_3
    KEY_FOUR,    // ACTION_TAB_4
    KEY_FIVE,    // ACTION_TAB_5
    KEY_SIX,     // ACTION_TAB_6
    KEY_SEVEN,   // ACTION_TAB_7
    KEY_EIGHT,   // ACTION_TAB_8
    KEY_W,       // ACTION_ARTILLERY_WHEEL
    KEY_Q,       // ACTION_SHIP_MOVE
    KEY_W,       // ACTION_SHIP_WHEEL
    KEY_E,       // ACTION_SHIP_ENGAGE
};
