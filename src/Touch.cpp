#include "Touch.h"
#include <cmath>

namespace odTouch {
namespace {

// ── Tuning ───────────────────────────────────────────────────────────────────
//
// TAP_SLOP is generous because a finger rolls a few pixels on release and a tap
// that turns into a one-pixel drag should still click.
constexpr float TAP_SLOP      = 14.0f;   // px of finger travel still counted a tap
constexpr float TAP_SECONDS   = 0.35f;   // longer than this is not a tap
constexpr float HOLD_SECONDS  = 0.55f;   // and past this it is a right click
constexpr float PINCH_PER_NOTCH = 44.0f; // px of pinch travel per wheel notch
constexpr float PINCH_DEADZONE  = 6.0f;

bool  s_active = false;
bool  s_present = false;
Vector2 s_cursor = {0, 0};

int   s_prevCount = 0;
Vector2 s_prevP0 = {0, 0};
float s_prevPinch = 0.0f;

// The single-finger gesture in progress.
float s_downTime = 0.0f;     // seconds held
float s_travel   = 0.0f;     // px moved since contact
bool  s_holdFired = false;   // right click already emitted for this press

// Edge-triggered output for this frame. Cleared at the top of every update, so
// a screen that reads them twice sees the same answer both times.
bool s_lDown = false, s_lPressed = false, s_lReleased = false;
bool s_rPressed = false, s_rReleased = false;
float s_wheel = 0.0f;

float dist(Vector2 a, Vector2 b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

void update(float dt, int screenW, int screenH) {
    s_lPressed = s_lReleased = s_rPressed = s_rReleased = false;
    s_wheel = 0.0f;

    const int n = GetTouchPointCount();
    if (n > 0) {
        s_present = true;
        s_active = true;
    }

    // ── Two fingers: pinch to zoom ──
    if (n >= 2) {
        const Vector2 a = GetTouchPosition(0), b = GetTouchPosition(1);
        const float d = dist(a, b);
        if (s_prevCount >= 2 && s_prevPinch > 0.0f) {
            const float delta = d - s_prevPinch;
            if (std::fabs(delta) > PINCH_DEADZONE)
                s_wheel = delta / PINCH_PER_NOTCH;
        }
        s_prevPinch = d;
        // A second finger cancels whatever the first was doing: a pinch that
        // began as a drag must not also fire a tap when the fingers lift.
        s_lDown = false;
        s_holdFired = true;
        s_travel = TAP_SLOP * 2.0f;
        s_prevCount = n;
        return;
    }
    s_prevPinch = 0.0f;

    // ── One finger: the cursor goes where the finger is ──
    //
    // ABSOLUTE, NOT TRACKPAD-RELATIVE. The first version moved the cursor by
    // the finger's delta and left it where it was on contact, so a tap clicked
    // wherever the cursor happened to be -- which on the first tap of a session
    // is (0,0), the top-left corner. Verified on the emulator: the menu drew
    // correctly and nothing was clickable.
    //
    // Absolute keeps the one property that mattered about the trackpad idea:
    // the cursor PERSISTS after the finger lifts, so the hover state that the
    // ship overlay, the tooltips and the artillery wheel all read stays alive
    // between touches. It just also puts it where the player pointed.
    if (n == 1) {
        const Vector2 p = GetTouchPosition(0);
        if (s_prevCount == 1) {
            const float dx = p.x - s_prevP0.x;
            const float dy = p.y - s_prevP0.y;
            s_travel += std::sqrt(dx * dx + dy * dy);
        } else {
            s_downTime = 0.0f;
            s_travel = 0.0f;
            s_holdFired = false;
        }
        s_cursor = p;
        s_prevP0 = p;
        s_downTime += dt;

        // Held still, long enough: right click, once.
        if (!s_holdFired && s_travel <= TAP_SLOP && s_downTime >= HOLD_SECONDS) {
            s_rPressed = true;
            s_holdFired = true;
        }

        // A drag holds the left button down, so dragging the map, box-select
        // and the ship action overlay all behave as they do with a mouse.
        if (s_travel > TAP_SLOP) {
            if (!s_lDown) { s_lPressed = true; s_lDown = true; }
        }

        if (s_cursor.x < 0) s_cursor.x = 0;
        if (s_cursor.y < 0) s_cursor.y = 0;
        if (s_cursor.x > (float)screenW) s_cursor.x = (float)screenW;
        if (s_cursor.y > (float)screenH) s_cursor.y = (float)screenH;
        s_prevCount = n;
        return;
    }

    // ── Lifted ──
    if (s_prevCount >= 1) {
        if (s_lDown) {
            s_lDown = false;
            s_lReleased = true;
        } else if (!s_holdFired && s_travel <= TAP_SLOP && s_downTime <= TAP_SECONDS) {
            // A tap is a press and a release in the same frame. Every screen
            // here tests Pressed or Released rather than the level, so one
            // frame is enough and it avoids inventing a fake second frame.
            s_lPressed = true;
            s_lReleased = true;
        } else if (s_rPressed || s_holdFired) {
            s_rReleased = true;
        }
    }
    s_prevCount = 0;
    s_downTime = 0.0f;
    s_travel = 0.0f;
}

bool active() { return s_active; }
bool suppressesMouse() { return s_active; }
bool present() { return s_present; }
Vector2 cursor() { return s_cursor; }
void placeCursor(Vector2 p) { s_cursor = p; }

bool mouseDown(int button) {
    return button == MOUSE_BUTTON_LEFT ? s_lDown : false;
}
bool mousePressed(int button) {
    if (button == MOUSE_BUTTON_LEFT)  return s_lPressed;
    if (button == MOUSE_BUTTON_RIGHT) return s_rPressed;
    return false;
}
bool mouseReleased(int button) {
    if (button == MOUSE_BUTTON_LEFT)  return s_lReleased;
    if (button == MOUSE_BUTTON_RIGHT) return s_rReleased;
    return false;
}
float wheel() { return s_wheel; }

}  // namespace odTouch
