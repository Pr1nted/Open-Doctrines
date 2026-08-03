#include "Gamepad.h"
#include "Keybinds.h"

#include <algorithm>
#include <cmath>

namespace odPad {
namespace {

// Pad 0 only. Multiplayer here is over the network, not the sofa: a second
// controller has nothing to control.
constexpr int PAD = 0;

// Below this the stick is at rest. Cheap sticks idle around 0.08-0.12, and a
// cursor that drifts on its own is worse than no controller support.
constexpr float DEAD_ZONE = 0.18f;

// Cursor speed in pixels per second at full deflection, and the exponent that
// makes small pushes fine and large pushes fast. Tuned against a 1600x900
// screen: about half a second corner to corner at full tilt, which is quick
// enough to cross the map and slow enough to hit a 28px button.
constexpr float CURSOR_SPEED = 1400.0f;
constexpr float CURSOR_CURVE = 2.2f;

// Menu repeat: the delay before a held direction starts repeating, and the
// interval after that. Same feel as a text cursor, for the same reason.
constexpr float REPEAT_DELAY = 0.42f;
constexpr float REPEAT_RATE  = 0.11f;

struct State {
    bool  active = false;
    Vector2 cursor = {0, 0};
    bool  down[8] = {false};        // synthetic mouse buttons, this frame
    bool  prev[8] = {false};        // ...and last frame, for edges
    float wheel = 0.0f;

    // One repeat timer per direction, so holding up and tapping left behave.
    float navHeld[4] = {0, 0, 0, 0};
    bool  navFired[4] = {false, false, false, false};
    bool  nav[4] = {false, false, false, false};   // fired THIS frame
    bool  activate = false, back = false;

    const int* binds = nullptr;     // Config::keybinds, borrowed
    int   bindCount = 0;
} g;

// Which pad button stands in for which ACTION. Chosen so the two-handed
// gestures stay two-handed: a shoulder button is held while the stick aims,
// exactly as a key is held while the mouse aims.
struct PadAction { int button; int action; };
constexpr PadAction PAD_ACTIONS[] = {
    {GAMEPAD_BUTTON_RIGHT_FACE_UP,      ACTION_ARTILLERY_WHEEL},  // Y
    {GAMEPAD_BUTTON_LEFT_TRIGGER_2,     ACTION_SHIP_MOVE},        // LT
    {GAMEPAD_BUTTON_RIGHT_TRIGGER_2,    ACTION_BOX_SELECT},       // RT
    {GAMEPAD_BUTTON_LEFT_THUMB,         ACTION_SHIP_WHEEL},       // L3
};

/** Axis with the dead zone removed and rescaled, so it starts at zero. */
float axis(int a) {
    float v = GetGamepadAxisMovement(PAD, a);
    if (std::fabs(v) < DEAD_ZONE) return 0.0f;
    float s = (std::fabs(v) - DEAD_ZONE) / (1.0f - DEAD_ZONE);
    return (v < 0 ? -1.0f : 1.0f) * std::min(1.0f, s);
}

bool held(int button) { return IsGamepadButtonDown(PAD, button); }

/** A held direction, with keyboard-style repeat. Index picks the timer. */
bool repeated(int idx, bool isHeld, float dt) {
    if (!isHeld) { g.navHeld[idx] = 0.0f; g.navFired[idx] = false; return false; }
    if (!g.navFired[idx]) {                      // the initial press
        g.navFired[idx] = true;
        g.navHeld[idx] = 0.0f;
        return true;
    }
    g.navHeld[idx] += dt;
    float threshold = REPEAT_DELAY;
    if (g.navHeld[idx] >= threshold) {
        g.navHeld[idx] -= REPEAT_RATE;           // keeps firing at REPEAT_RATE
        g.navHeld[idx] = std::max(g.navHeld[idx], threshold - REPEAT_RATE);
        return true;
    }
    return false;
}

}  // namespace

void update(float dt, int screenW, int screenH) {
    for (int i = 0; i < 8; ++i) g.prev[i] = g.down[i];
    g.wheel = 0.0f;

    if (!IsGamepadAvailable(PAD)) {
        g.active = false;
        for (int i = 0; i < 8; ++i) g.down[i] = false;
        g.activate = g.back = false;
        for (int i = 0; i < 4; ++i) { g.nav[i] = false; g.navFired[i] = false; }
        return;
    }

    const float lx = axis(GAMEPAD_AXIS_LEFT_X);
    const float ly = axis(GAMEPAD_AXIS_LEFT_Y);
    const float ry = axis(GAMEPAD_AXIS_RIGHT_Y);

    // WHO IS DRIVING. Any pad activity claims the cursor; any real mouse
    // movement takes it straight back. Checked in that order so that a player
    // holding the stick keeps it even if the mouse is jostled.
    const bool padMoved = lx != 0.0f || ly != 0.0f || ry != 0.0f ||
                          IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_RIGHT_FACE_DOWN) ||
                          IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ||
                          IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_LEFT_FACE_UP) ||
                          IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_LEFT_FACE_DOWN) ||
                          IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_LEFT_FACE_LEFT) ||
                          IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_LEFT_FACE_RIGHT);
    if (padMoved) {
        if (!g.active) {
            // Start where the mouse was, so the cursor does not teleport the
            // first time somebody picks the pad up.
            Vector2 m = GetMousePosition();
            g.cursor = {m.x, m.y};
            g.active = true;
        }
    } else {
        Vector2 d = GetMouseDelta();
        if (d.x != 0.0f || d.y != 0.0f) g.active = false;
    }

    if (g.active) {
        // A curve rather than a straight scale: precision near the centre is
        // what makes a stick usable for aiming at buttons.
        const float mag = std::min(1.0f, std::sqrt(lx * lx + ly * ly));
        if (mag > 0.0f) {
            const float speed = CURSOR_SPEED * std::pow(mag, CURSOR_CURVE) / mag;
            g.cursor.x += lx * speed * dt;
            g.cursor.y += ly * speed * dt;
        }
        g.cursor.x = std::clamp(g.cursor.x, 0.0f, (float)screenW);
        g.cursor.y = std::clamp(g.cursor.y, 0.0f, (float)screenH);

        // The right stick scrolls, because every list in this game does and the
        // map zooms on the same input.
        g.wheel = -ry * 6.0f * dt * 10.0f;
        if (held(GAMEPAD_BUTTON_LEFT_TRIGGER_1))  g.wheel -= 0.35f;
        if (held(GAMEPAD_BUTTON_RIGHT_TRIGGER_1)) g.wheel += 0.35f;
    }

    // A is the left button, X is the right one. Right-drag is how armies move,
    // so it needs a button that can be HELD, not a chord.
    g.down[MOUSE_BUTTON_LEFT]  = g.active && held(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
    g.down[MOUSE_BUTTON_RIGHT] = g.active && held(GAMEPAD_BUTTON_RIGHT_FACE_LEFT);

    // Menu walking. The d-pad only; the stick is the cursor, and a stick that
    // did both would move a pointer and a selection at once.
    g.activate = IsGamepadButtonPressed(PAD, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
    g.back     = IsGamepadButtonPressed(PAD, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
    g.nav[0] = repeated(0, held(GAMEPAD_BUTTON_LEFT_FACE_UP),    dt);
    g.nav[1] = repeated(1, held(GAMEPAD_BUTTON_LEFT_FACE_DOWN),  dt);
    g.nav[2] = repeated(2, held(GAMEPAD_BUTTON_LEFT_FACE_LEFT),  dt);
    g.nav[3] = repeated(3, held(GAMEPAD_BUTTON_LEFT_FACE_RIGHT), dt);
}

bool active() { return g.active; }
Vector2 cursor() { return g.cursor; }
void placeCursor(Vector2 p) { g.cursor = p; }

bool mouseDown(int b)     { return b >= 0 && b < 8 && g.down[b]; }
bool mousePressed(int b)  { return b >= 0 && b < 8 && g.down[b] && !g.prev[b]; }
bool mouseReleased(int b) { return b >= 0 && b < 8 && !g.down[b] && g.prev[b]; }
float wheel() { return g.wheel; }

// Edges, recorded by update(): true for exactly the frame the event happened.
bool navUp()    { return g.nav[0]; }
bool navDown()  { return g.nav[1]; }
bool navLeft()  { return g.nav[2]; }
bool navRight() { return g.nav[3]; }
bool navActivate() { return g.activate; }
bool navBack()     { return g.back; }

void setBindings(const int* keybinds, int count) { g.binds = keybinds; g.bindCount = count; }

// The action's key is looked up per query rather than cached, because a rebind
// takes effect the instant it is made and nothing here should need telling.
bool keyDown(int key) {
    if (!g.active || !g.binds) return false;
    for (const auto& pa : PAD_ACTIONS)
        if (pa.action < g.bindCount && g.binds[pa.action] == key && held(pa.button)) return true;
    return false;
}

bool keyPressed(int key) {
    if (!g.active || !g.binds) return false;
    for (const auto& pa : PAD_ACTIONS)
        if (pa.action < g.bindCount && g.binds[pa.action] == key &&
            IsGamepadButtonPressed(PAD, pa.button)) return true;
    return false;
}

bool present() { return IsGamepadAvailable(PAD); }
const char* name() { return IsGamepadAvailable(PAD) ? GetGamepadName(PAD) : nullptr; }

}  // namespace odPad
