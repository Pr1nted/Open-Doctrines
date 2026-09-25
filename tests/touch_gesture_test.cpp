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
#include "UiScale.h"

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

    // ── THE REPORTED BUG ──
    //
    // "The fix for android swiping did not work." It did not, and nothing here
    // could tell: the wheel was set in exactly ONE place, the two-finger
    // pinch, so the shipped answer to "scroll this list" was "pinch it".
    // Sixteen panels read the wheel and none of them could be reached with one
    // finger. These are the properties that were missing.
    //
    // A swipe is many frames: a real finger crosses TAP_SLOP (14px) over
    // several, and the notch threshold over several more, so each case drives
    // the gesture the way a hand does rather than teleporting.
    const Rectangle list = {100, 100, 300, 400};   // a panel's list area

    section("a swipe on a list scrolls it");
    {
        lift(); frame();
        odTouch::armScrollRegion(list);
        oneFinger(200, 150); frame();              // lands inside the list
        float notches = 0.0f;
        for (int i = 0; i < 10; ++i) {             // 10 x 10px = 100px down
            odTouch::armScrollRegion(list);
            oneFinger(200, 150.0f + 10.0f * (i + 1));
            frame();
            notches += odTouch::takeDragScroll();
        }
        ok(notches > 0.0f, "a swipe DOWN produces wheel notches");
        // 100px at 26px a notch is 3 whole ones; the point is the sign and
        // the rough scale, not the exact figure.
        ok(notches >= 3.0f && notches <= 4.0f, "and about one per 26px of travel");
        ok(odTouch::dragScrolling(), "the gesture knows it is scrolling");
    }

    section("and does not press the row it started on");
    {
        // THE ONE THAT MAKES IT USABLE. A drag holds the left button once it
        // passes TAP_SLOP, so before this a swipe opened whatever was under
        // the finger at 14px of travel -- scrolling a list by opening one of
        // its rows is worse than not scrolling at all.
        lift(); frame();
        odTouch::armScrollRegion(list);
        oneFinger(200, 150); frame();
        bool pressed = false, down = false;
        for (int i = 0; i < 10; ++i) {
            odTouch::armScrollRegion(list);
            oneFinger(200, 150.0f + 10.0f * (i + 1));
            frame();
            if (odTouch::mousePressed(MOUSE_BUTTON_LEFT)) pressed = true;
            if (odTouch::mouseDown(MOUSE_BUTTON_LEFT))    down = true;
        }
        ok(!pressed, "no left click during the swipe");
        ok(!down, "and the button is never held either");
    }

    section("a tap on a list is still a tap");
    {
        // A swipe must not eat the click: a list you cannot scroll is a bug,
        // and a list you cannot press is a worse one.
        lift(); frame();
        odTouch::armScrollRegion(list);
        oneFinger(200, 150); frame();
        odTouch::armScrollRegion(list);
        oneFinger(202, 151); frame();              // within TAP_SLOP
        lift(); frame();
        ok(odTouch::mousePressed(MOUSE_BUTTON_LEFT), "a short touch still clicks");
    }

    section("a swipe on the MAP still pans it");
    {
        // The reason the region matters. The map wants one finger to drag, and
        // a rule that turned every drag into a wheel would zoom the map every
        // time somebody moved it.
        lift(); frame();
        odTouch::armScrollRegion(list);
        oneFinger(600, 150); frame();              // OUTSIDE the list
        float notches = 0.0f;
        bool down = false;
        Vector2 lastDelta = {0, 0};
        for (int i = 0; i < 10; ++i) {
            odTouch::armScrollRegion(list);
            oneFinger(600, 150.0f + 10.0f * (i + 1));
            frame();
            notches += odTouch::takeDragScroll();
            if (odTouch::mouseDown(MOUSE_BUTTON_LEFT)) down = true;
            lastDelta = odTouch::delta();
        }
        ok(notches == 0.0f, "a drag outside the list produces no wheel");
        ok(down, "it holds the left button, as a mouse drag would");
        ok(!isZero(lastDelta), "and still reports the travel the pan needs");
        ok(!odTouch::dragScrolling(), "and is not a scroll");
    }

    section("a list left behind does not keep claiming every drag");
    {
        // WHAT ACTUALLY SHIPPED. Every case above re-arms the region on every
        // frame, so none of them could see that the flag was never cleared --
        // and the screens on the way into a game arm the WHOLE SCREEN
        // (odScrollWheelScreen passes {0,0,1e6,1e6}) every frame they are up.
        // So after picking a world or loading a save, the arm stayed on for
        // the rest of the session and every drag anywhere became a list
        // scroll: no left button, and therefore no map pan and no slider,
        // for as long as the game was open.
        lift(); frame();
        const Rectangle wholeScreen = {0.0f, 0.0f, 1.0e6f, 1.0e6f};
        odTouch::armScrollRegion(wholeScreen);     // a browser screen, once
        frame();                                   // ... which is then left
        for (int i = 0; i < 5; ++i) frame();       // several frames on the map

        oneFinger(600, 150); frame();
        bool down = false;
        float notches = 0.0f;
        Vector2 lastDelta = {0, 0};
        for (int i = 0; i < 10; ++i) {
            oneFinger(600, 150.0f + 10.0f * (i + 1));
            frame();
            notches += odTouch::takeDragScroll();
            if (odTouch::mouseDown(MOUSE_BUTTON_LEFT)) down = true;
            lastDelta = odTouch::delta();
        }
        ok(down, "a drag after leaving that screen holds the left button");
        ok(!isZero(lastDelta), "and reports travel, so the map can pan");
        ok(notches == 0.0f, "and is not turned into wheel notches");
        ok(!odTouch::dragScrolling(), "and is not a scroll");
    }

    section("a pointer lands where it is drawn, whatever the scale");
    {
        // Two copies of this sum disagreed: every panel divided by the UI
        // scale and the map did not. On a desktop the scale is 1 and nothing
        // showed; on a phone it never is, so the map was hit-tested and
        // panned a scale factor away from the finger that every panel saw in
        // the right place.
        const Vector2 raw = {600.0f, 300.0f};
        const Vector2 plain = odPointerIn(raw, 1.0f, 1.0f);
        ok(plain.x == 600.0f && plain.y == 300.0f, "with no scaling it is itself");

        const Vector2 scaled = odPointerIn(raw, 1.0f, 1.5f);
        ok(scaled.x == 400.0f && scaled.y == 200.0f,
           "a magnified frame divides the pointer by the magnification");

        const Vector2 hidpi = odPointerIn(raw, 2.0f, 1.0f);
        ok(hidpi.x == 1200.0f && hidpi.y == 600.0f,
           "and a high-density window multiplies it by the density");

        const Vector2 both = odPointerIn(raw, 2.0f, 2.0f);
        ok(both.x == 600.0f && both.y == 300.0f, "both together cancel out");

        const Vector2 safe = odPointerIn(raw, 1.0f, 0.0f);
        ok(safe.x == 600.0f && safe.y == 300.0f,
           "and a scale of zero is treated as one rather than dividing by it");
    }

    section("a list still on screen keeps scrolling");
    {
        // The other side of the same rule: a list re-arms itself every frame,
        // so expiring the arm must not cost it its scroll.
        lift(); frame();
        float notches = 0.0f;
        odTouch::armScrollRegion(list);
        oneFinger(150, 150); frame();
        for (int i = 0; i < 10; ++i) {
            odTouch::armScrollRegion(list);        // as the list does, per frame
            oneFinger(150, 150.0f + 12.0f * (i + 1));
            frame();
            notches += odTouch::takeDragScroll();
        }
        ok(notches != 0.0f, "a swipe on a live list still scrolls it");
        ok(!odTouch::mouseDown(MOUSE_BUTTON_LEFT),
           "and still withholds the button, so it does not press a row");
    }

    section("the origin decides, once");
    {
        // A finger that starts on a list and wanders off the edge of it is
        // still scrolling that list -- which is what a thumb actually does.
        lift(); frame();
        odTouch::armScrollRegion(list);
        oneFinger(390, 150); frame();              // inside, near the right edge
        float notches = 0.0f;
        for (int i = 0; i < 10; ++i) {
            odTouch::armScrollRegion(list);
            oneFinger(390.0f + 10.0f * (i + 1), 150.0f + 10.0f * (i + 1));
            frame();                               // drifts out past x=400
            notches += odTouch::takeDragScroll();
        }
        ok(notches > 0.0f, "it keeps scrolling after leaving the rectangle");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
