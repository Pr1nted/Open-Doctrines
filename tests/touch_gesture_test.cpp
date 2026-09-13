// What the finger did, as the map reads it.
//
// The map pans on a POINTER DELTA, and until this test existed there was only
// one source for that: raylib's GetMouseDelta(). On Android that is not a
// per-frame quantity at all. rcore_android.c advances Mouse.previousPosition
// only inside an ACTION_MOVE event, and the Android PollInputEvents never
// touches it -- so a finger resting on the glass sends no events, the last
// delta stays standing, and whatever reads it keeps panning on its own. The
// map slides away under a stationary thumb.
//
// odTouch::delta() exists to be the honest answer to that question, and these
// are the properties the pan depends on:
//
//   the frame a finger lands   zero -- it has not moved yet
//   a frame it drags           exactly the travel, in screen pixels
//   a frame it rests           ZERO, and this is the one Android got wrong
//   two fingers                zero -- a pinch is a zoom, not a pan
//   coming off a pinch         zero -- lifting the second finger is not a swipe
//
// Nothing here touches raylib. The three entry points odTouch actually calls
// are defined below, so the whole gesture layer runs against a screen this
// test makes up -- which is the only way to hold a finger still for a frame.

#include "raylib.h"
#include "Touch.h"

#include <cmath>
#include <cstdio>
#include <string>

// ── The screen, as this test pretends it is ──
//
// Plain definitions rather than an extern "C" block: raylib.h has already
// declared these with C linkage, which is what src/server/ServerRaylibStubs.cpp
// relies on for the same trick.
static int     s_count = 0;
static Vector2 s_pts[4] = {};
static Vector2 s_mouse = {0, 0};

int GetTouchPointCount(void) { return s_count; }
Vector2 GetTouchPosition(int i) { return (i >= 0 && i < s_count) ? s_pts[i] : Vector2{0, 0}; }
Vector2 GetMousePosition(void) { return s_mouse; }

static int checks = 0, fails = 0;
static void ok(bool c, const std::string& what) {
    ++checks;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!c) ++fails;
}
static void section(const char* t) { printf("\n== %s ==\n", t); }

static const float DT = 1.0f / 60.0f;
static const int   SW = 800, SH = 600;

// One frame of the gesture layer, with N fingers already placed.
static void frame() { odTouch::update(DT, SW, SH); }

static void oneFinger(float x, float y) {
    s_count = 1;
    s_pts[0] = {x, y};
    // Android's own backend maps touch point zero onto the mouse. Mirrored
    // here so the "did the mouse take it back" arbitration sees what it would
    // see on a phone rather than a pointer that never moves.
    s_mouse = {x, y};
}
static void twoFingers(float ax, float ay, float bx, float by) {
    s_count = 2;
    s_pts[0] = {ax, ay};
    s_pts[1] = {bx, by};
    s_mouse = {ax, ay};
}
static void lift() { s_count = 0; }

static bool isZero(Vector2 v) { return v.x == 0.0f && v.y == 0.0f; }
static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

int main() {
    printf("Touch gestures\n");

    section("a finger that has just landed has not moved");
    {
        oneFinger(100, 100);
        frame();
        ok(isZero(odTouch::delta()), "the contact frame reports no travel");
        ok(odTouch::active(), "and touch is now driving");
        ok(odTouch::suppressesMouse(), "so raylib's own mouse is ignored");
    }

    section("a dragging finger reports exactly its travel");
    {
        oneFinger(140, 130);
        frame();
        const Vector2 d = odTouch::delta();
        ok(near(d.x, 40.0f) && near(d.y, 30.0f), "40,30 of finger becomes 40,30 of delta");

        oneFinger(130, 130);
        frame();
        const Vector2 back = odTouch::delta();
        ok(near(back.x, -10.0f) && near(back.y, 0.0f), "and it is signed, not a distance");
    }

    // ── THE ONE ANDROID GOT WRONG ──
    section("a finger held still reports nothing");
    {
        // Same coordinates, three frames running. On Android GetMouseDelta()
        // would still be handing back the last movement here, because no
        // ACTION_MOVE arrives to advance the previous position -- and the map
        // would keep panning while the thumb sits perfectly still.
        for (int i = 0; i < 3; ++i) {
            oneFinger(130, 130);
            frame();
            ok(isZero(odTouch::delta()), "a resting finger does not pan the map");
        }
    }

    section("a pinch is a zoom, not a pan");
    {
        twoFingers(200, 300, 300, 300);
        frame();                      // establishes the span
        twoFingers(180, 300, 320, 300);
        frame();                      // 40px wider
        ok(isZero(odTouch::delta()), "two fingers contribute no pan");
        ok(odTouch::wheel() > 0.0f, "spreading them zooms in");

        twoFingers(220, 300, 280, 300);
        frame();
        ok(odTouch::wheel() < 0.0f, "and closing them zooms out");
    }

    section("coming off a pinch is not a swipe");
    {
        // The second finger lifts and the first is somewhere quite different
        // from where the pan last saw it. Treating that gap as travel would
        // throw the map across the screen.
        oneFinger(600, 500);
        frame();
        ok(isZero(odTouch::delta()), "the first frame after a pinch reports no travel");

        oneFinger(610, 500);
        frame();
        ok(near(odTouch::delta().x, 10.0f), "and the next frame resumes normally");
    }

    section("the cursor is where the finger is");
    {
        oneFinger(321, 234);
        frame();
        const Vector2 c = odTouch::cursor();
        ok(near(c.x, 321.0f) && near(c.y, 234.0f), "absolute, not accumulated");
    }

    section("a lifted finger leaves nothing moving");
    {
        lift();
        frame();
        ok(isZero(odTouch::delta()), "no fingers, no pan");
        ok(odTouch::cursor().x > 0.0f, "but the cursor persists, for hover");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
