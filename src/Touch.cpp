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
// ── ONE FINGER, ON A LIST ──
//
// The gesture set had no one-finger scroll. A swipe on a list produced a
// held left button and a cursor move, and the WHEEL -- which is what all
// sixteen scrollable panels read -- was set in exactly one place: the
// two-finger pinch. So the shipped answer to "scroll this list" on a phone
// was "pinch it", which is also the zoom gesture, and the bug report was
// simply that swiping does nothing.
//
// A drag whose origin is inside a region a list armed this frame becomes
// wheel notches instead of a button drag. Travel ACCUMULATES, so a slow swipe
// still scrolls rather than being rounded away frame by frame.
constexpr float DRAG_PER_NOTCH  = 26.0f;  // px of finger travel per wheel notch

/// Point in rectangle. Written out rather than taken from raylib so this
/// file links in touch_gesture_test, which stubs the library away.
inline bool inRect(Vector2 p, Rectangle r) {
    return p.x >= r.x && p.x <= r.x + r.width &&
           p.y >= r.y && p.y <= r.y + r.height;
}

bool  s_active = false;
bool  s_present = false;
Vector2 s_cursor = {0, 0};
Vector2 s_delta = {0, 0};

// Where the real mouse was last frame, so a mouse that MOVES can take the
// cursor back off touch. See the note in update().
Vector2 s_prevMouse = {0, 0};
bool    s_prevMouseKnown = false;

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
Rectangle s_scrollArm = {0, 0, 0, 0};   ///< armed by odScrollWheel, one frame behind
bool  s_scrollArmed = false;
Vector2 s_dragOrigin = {0, 0};
bool  s_dragScrolling = false;          ///< this drag is scrolling a list, not dragging
float s_dragAccum = 0.0f;               ///< sub-notch travel carried between frames
float s_dragNotches = 0.0f;             ///< whole notches waiting to be read

float dist(Vector2 a, Vector2 b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

void update(float dt, int screenW, int screenH) {
    s_lPressed = s_lReleased = s_rPressed = s_rReleased = false;
    s_wheel = 0.0f;
    // Notches expire with the frame, exactly as s_wheel does, and for the same
    // reason. They were only cleared when somebody read them -- so a swipe on
    // a panel that closed before the read left them standing, and the next
    // list to ask got a jolt of scroll from a gesture made in another screen.
    // The sub-notch remainder is NOT cleared here: that is what lets a slow
    // swipe accumulate across frames, and it is reset when a finger lands.
    s_dragNotches = 0.0f;
    s_delta = {0.0f, 0.0f};

    // ── THE ARMED REGION LASTS ONE FRAME, WHICH IS WHAT IT ALWAYS CLAIMED ──
    //
    // armScrollRegion set a flag that nothing ever cleared, and the screens on
    // the way into a game -- pick a world, load a save, settings -- arm the
    // WHOLE SCREEN every frame through odScrollWheelScreen. So after visiting
    // any of them, every one-finger drag anywhere was classified as a list
    // scroll for the rest of the session. A scroll deliberately withholds the
    // left button, and the map pan is gated on that button, so on Android the
    // map simply could not be dragged: no button, no wheel, and a delta
    // nothing was allowed to read. Sliders went the same way.
    //
    // Consumed here, at the top: odScrollWheel arms during the update and draw
    // that follow, so an arm from frame N is read by frame N+1 and then
    // forgotten. A list still on screen re-arms itself every frame and keeps
    // scrolling exactly as before. NOT cleared at the end of this function --
    // it has two early returns below.
    const bool armedLastFrame = s_scrollArmed;
    s_scrollArmed = false;

    const Vector2 mouseNow = GetMousePosition();
    const int n = GetTouchPointCount();
    if (n > 0) {
        s_present = true;
        s_active = true;
    } else if (s_active && s_prevMouseKnown &&
               (mouseNow.x != s_prevMouse.x || mouseNow.y != s_prevMouse.y)) {
        // ── THE MOUSE TAKES IT BACK ──
        //
        // Nothing here ever did. s_active latched true on the first touch
        // point of the session and stayed true for the rest of it, and
        // suppressesMouse() is the same flag -- so from that moment every shim
        // IGNORED the real mouse and getMouse() answered with this cursor,
        // parked wherever the last finger left it. Every click in the game
        // then hit-tested at that spot instead of the pointer: buttons stop
        // responding, the map stops answering, and what the player sees is a
        // game that has stopped working. It is the behaviour the header
        // promises ("On desktop this arbitrates with the mouse exactly as
        // odPad does") and the only half that was never written; odPad gives
        // control back on the same condition, in Gamepad.cpp.
        //
        // Only while no finger is down: on Android raylib synthesises the
        // mouse FROM touch point zero, so during a gesture the mouse
        // "moves" with the finger and testing it there would hand control
        // back on every drag.
        s_active = false;
    }
    s_prevMouse = mouseNow;
    s_prevMouseKnown = true;

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
            s_delta = {dx, dy};
        } else {
            s_downTime = 0.0f;
            s_travel = 0.0f;
            s_holdFired = false;
            // Where the finger landed decides what this drag IS, once, at
            // contact -- not per frame. A swipe that starts on a list stays a
            // scroll even when it wanders off the edge of it, which is what a
            // finger actually does.
            s_dragOrigin = p;
            s_dragAccum = 0.0f;
            s_dragScrolling = armedLastFrame && inRect(p, s_scrollArm);
        }
        s_cursor = p;
        s_prevP0 = p;
        s_downTime += dt;

        // Held still, long enough: right click, once.
        if (!s_holdFired && s_travel <= TAP_SLOP && s_downTime >= HOLD_SECONDS) {
            s_rPressed = true;
            s_holdFired = true;
        }

        if (s_dragScrolling) {
            // Content follows the finger: dragging DOWN reveals what is above,
            // which is a positive wheel, because every panel here scrolls with
            // `scroll -= wheel`.
            s_dragAccum += s_delta.y;
            const float whole = std::trunc(s_dragAccum / DRAG_PER_NOTCH);
            if (whole != 0.0f) {
                s_dragNotches += whole;
                s_dragAccum -= whole * DRAG_PER_NOTCH;
            }
            // AND NO BUTTON. Without this the swipe presses the row it started
            // on the moment it passes TAP_SLOP -- touch a list to scroll it and
            // you have opened whatever was under your finger. A tap is
            // unaffected: it never reaches TAP_SLOP, so it still clicks.
        } else if (s_travel > TAP_SLOP) {
            // A drag holds the left button down, so dragging the map,
            // box-select and the ship action overlay all behave as they do
            // with a mouse.
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
    s_dragScrolling = false;
    s_dragAccum = 0.0f;
}

bool active() { return s_active; }
bool suppressesMouse() { return s_active; }
bool present() { return s_present; }
Vector2 cursor() { return s_cursor; }
Vector2 delta() { return s_delta; }
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

void armScrollRegion(Rectangle area) { s_scrollArm = area; s_scrollArmed = true; }
bool dragScrolling() { return s_dragScrolling; }
float takeDragScroll() { const float n = s_dragNotches; s_dragNotches = 0.0f; return n; }

}  // namespace odTouch
