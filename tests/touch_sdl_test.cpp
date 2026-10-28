// A finger on the glass, all the way through: SDL -> raylib -> odTouch.
//
// tests/touch_gesture_test.cpp holds the gesture layer to its contract against
// a screen it makes up. What it cannot show is that a desktop build ever
// RECEIVES a finger -- and for as long as the desktop builds ran on GLFW, none
// did: GLFW has no touch API, GetTouchPointCount() was 0 on every Steam Deck,
// Windows handheld and touchscreen laptop, and odTouch never ran there.
//
// So this opens a real window on the real backend the game ships with
// (raylib on SDL3, OD_DESKTOP_SDL) and puts touches into SDL exactly where a
// platform's video driver would: SDL_AddTouch / SDL_SendTouch, the internal
// calls SDL's X11, Wayland and Windows drivers make when the OS reports a
// finger. They are not public API, but SDL is linked statically here, so they
// are there to call. SDL_PushEvent is not a substitute: raylib reads the
// finger COUNT from SDL_GetTouchFingers(), which only the internal path
// updates, so a pushed event would leave it at zero.
//
// What it checks, in the order a player's hand would do it:
//   a finger lands       raylib reports one touch point, where it landed;
//                        odTouch takes the cursor; the OS mouse did NOT move
//   it drags             the touch point and the cursor follow it
//   it lifts             no touch points left
//   a tap                a left click, from odTouch
//   two fingers spread   a pinch: wheel notches (map zoom)
//
// Needs a display (Xvfb on Linux CI). Without one it says so and exits 0,
// unless OD_TOUCH_SDL_REQUIRE is set, which CI does -- there a missing display
// is a broken job, not a pass.
#include "raylib.h"
#include "Touch.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

// SDL's own internal touch entry points (src/events/SDL_touch_c.h, 3.4). A
// timestamp of 0 means "now", as it does for every SDL event.
extern "C" {
int SDL_AddTouch(SDL_TouchID id, SDL_TouchDeviceType type, const char* name);
void SDL_SendTouch(Uint64 timestamp, SDL_TouchID id, SDL_FingerID fingerid, SDL_Window* window,
                   SDL_EventType type, float x, float y, float pressure);
void SDL_SendTouchMotion(Uint64 timestamp, SDL_TouchID id, SDL_FingerID fingerid, SDL_Window* window,
                         float x, float y, float pressure);
}

static int checks = 0, fails = 0;
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++fails;
    std::printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
}

static const int W = 640, H = 400;
static const SDL_TouchID kTouch = 4242;
static SDL_Window* g_win = nullptr;

// One frame of the game's loop, as far as input goes: raylib polls its events
// at the end of the frame, then the touch layer reads what raylib now holds.
static void frame() {
    BeginDrawing();
    ClearBackground(BLACK);
    EndDrawing();
    odTouch::update(1.0f / 60.0f, W, H);
}

static void down(SDL_FingerID f, float x, float y) { SDL_SendTouch(0, kTouch, f, g_win, SDL_EVENT_FINGER_DOWN, x, y, 1.0f); }
static void move(SDL_FingerID f, float x, float y) { SDL_SendTouchMotion(0, kTouch, f, g_win, x, y, 1.0f); }
static void up(SDL_FingerID f, float x, float y) { SDL_SendTouch(0, kTouch, f, g_win, SDL_EVENT_FINGER_UP, x, y, 1.0f); }

int main() {
    std::printf("touch through SDL (raylib %s)\n", RAYLIB_VERSION);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(W, H, "touch_sdl_test");
    if (!IsWindowReady()) {
        const bool require = std::getenv("OD_TOUCH_SDL_REQUIRE") != nullptr;
        std::printf("%s: no window could be opened (no display?)\n", require ? "FAIL" : "SKIPPED");
        return require ? 1 : 0;
    }
    g_win = SDL_GL_GetCurrentWindow();
    check(g_win != nullptr, "the window is an SDL window");
    check(SDL_AddTouch(kTouch, SDL_TOUCH_DEVICE_DIRECT, "touch_sdl_test") >= 0, "a touchscreen can be attached");

    frame();
    check(GetTouchPointCount() == 0, "no touch points before a finger lands");
    check(!odTouch::active(), "touch is not driving yet");
    const Vector2 mouseBefore = GetMousePosition();

    // ── a finger lands at a quarter of the way across, half way down ──
    down(1, 0.25f, 0.5f);
    frame();
    check(GetTouchPointCount() == 1, "one finger: raylib reports one touch point");
    const Vector2 p = GetTouchPosition(0);
    check(std::fabs(p.x - W * 0.25f) < 2 && std::fabs(p.y - H * 0.5f) < 2, "and it is where the finger landed");
    check(odTouch::active(), "odTouch takes the cursor");
    const Vector2 m = GetMousePosition();
    check(m.x == mouseBefore.x && m.y == mouseBefore.y,
          "the OS mouse did not move (SDL is not faking a mouse from it)");

    // ── it drags right ──
    const Vector2 c0 = odTouch::cursor();
    for (int i = 1; i <= 8; ++i) { move(1, 0.25f + 0.03f * i, 0.5f); frame(); }
    check(GetTouchPosition(0).x > W * 0.45f, "the touch point follows the drag");
    check(odTouch::cursor().x > c0.x + 10, "and so does the cursor");

    // ── it lifts ──
    up(1, 0.49f, 0.5f);
    frame();
    check(GetTouchPointCount() == 0, "lifted: no touch points left");

    // ── a tap is a left click ──
    bool clicked = false;
    down(2, 0.6f, 0.6f);
    frame();
    clicked |= odTouch::mousePressed(MOUSE_BUTTON_LEFT) || odTouch::mouseReleased(MOUSE_BUTTON_LEFT);
    up(2, 0.6f, 0.6f);
    for (int i = 0; i < 4; ++i) {
        frame();
        clicked |= odTouch::mousePressed(MOUSE_BUTTON_LEFT) || odTouch::mouseReleased(MOUSE_BUTTON_LEFT);
    }
    check(clicked, "a tap is a left click");

    // ── two fingers spreading are a pinch ──
    float wheel = 0.0f;
    down(3, 0.45f, 0.5f);
    down(4, 0.55f, 0.5f);
    frame();
    check(GetTouchPointCount() == 2, "two fingers: two touch points");
    for (int i = 1; i <= 10; ++i) {
        move(3, 0.45f - 0.025f * i, 0.5f);
        move(4, 0.55f + 0.025f * i, 0.5f);
        frame();
        wheel += odTouch::wheel();
    }
    up(3, 0.2f, 0.5f);
    up(4, 0.8f, 0.5f);
    frame();
    check(std::fabs(wheel) > 0.0f, "spreading two fingers zooms (wheel notches)");

    CloseWindow();
    std::printf("touch through SDL: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
