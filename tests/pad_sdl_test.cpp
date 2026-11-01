// A Steam Deck's controls, all the way through: SDL -> raylib -> odPad.
//
// On a Deck in Game Mode the controls ARE a gamepad -- Steam Input presents the
// built-in sticks and buttons to the game as one -- and a player has nothing
// else to hold. src/Gamepad.cpp turns that pad into the cursor and clicks the
// rest of the game was written for, and nothing showed it ever received one.
//
// So this opens a real window on the backend the desktop game ships on
// (raylib on SDL3, OD_DESKTOP_SDL) and attaches a pad through SDL's PUBLIC
// virtual-joystick API, as a Steam Deck controller (Valve, 28de:1205, the IDs
// SDL's own controller database knows the Deck by). Unlike the touch test this
// needs nothing internal: SDL_AttachVirtualJoystick is the supported way in.
//
// What it checks, in the order a player's hand would do it:
//   a pad appears        raylib sees a gamepad; odPad reports it present
//   the left stick       moves odPad's cursor, and odPad takes over driving
//   A                    a left click
//   X                    a right click
//   B                    "back" in the menus
//   the d-pad            one menu step per press
//   the bumpers          wheel notches (map zoom)
//
// Needs a display (Xvfb on Linux CI). Without one it says so and exits 0,
// unless OD_PAD_SDL_REQUIRE is set, which CI does.
#include "raylib.h"
#include "Gamepad.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

static int checks = 0, fails = 0;
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++fails;
    std::printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
}

static const int W = 1280, H = 800;   // a Deck's panel
static SDL_Joystick* g_pad = nullptr;

// One frame of the game's loop, as far as input goes: raylib polls its events
// at the end of the frame, then the pad layer reads what raylib now holds.
static void frame() {
    BeginDrawing();
    ClearBackground(BLACK);
    EndDrawing();
    odPad::update(1.0f / 60.0f, W, H);
}
static void frames(int n) { for (int i = 0; i < n; ++i) frame(); }

static void button(SDL_GamepadButton b, bool down) { SDL_SetJoystickVirtualButton(g_pad, b, down); }
static void axis(SDL_GamepadAxis a, Sint16 v) { SDL_SetJoystickVirtualAxis(g_pad, a, v); }

// Press and release over a few frames, returning whether `seen` fired on any.
template <class F> static bool tap(SDL_GamepadButton b, F seen) {
    bool fired = false;
    button(b, true);
    for (int i = 0; i < 3; ++i) { frame(); fired |= seen(); }
    button(b, false);
    for (int i = 0; i < 3; ++i) { frame(); fired |= seen(); }
    return fired;
}

int main() {
    std::printf("a Steam Deck's controls through SDL (raylib %s)\n", RAYLIB_VERSION);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(W, H, "pad_sdl_test");
    if (!IsWindowReady()) {
        const bool require = std::getenv("OD_PAD_SDL_REQUIRE") != nullptr;
        std::printf("%s: no window could be opened (no display?)\n", require ? "FAIL" : "SKIPPED");
        return require ? 1 : 0;
    }
    frames(2);
    check(!odPad::present(), "no pad before one is attached");

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x28DE;     // Valve
    desc.product_id = 0x1205;    // Steam Deck controller
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Steam Deck";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    check(id != 0, "a Steam Deck controller can be attached");
    g_pad = SDL_OpenJoystick(id);
    check(g_pad != nullptr, "and opened");
    frames(10);   // raylib opens it on the add event
    check(IsGamepadAvailable(0), "raylib sees a gamepad");
    check(odPad::present(), "odPad reports it present");
    if (const char* n = odPad::name()) std::printf("  (pad name: %s)\n", n);

    // ── the left stick drives the cursor ──
    odPad::placeCursor({W * 0.5f, H * 0.5f});
    const Vector2 c0 = odPad::cursor();
    axis(SDL_GAMEPAD_AXIS_LEFTX, 32767);
    frames(30);
    axis(SDL_GAMEPAD_AXIS_LEFTX, 0);
    frames(2);
    check(odPad::active(), "the stick puts the pad in charge of the cursor");
    check(odPad::cursor().x > c0.x + 40, "and the cursor moves right with it");

    // ── A clicks, X right-clicks ──
    check(tap(SDL_GAMEPAD_BUTTON_SOUTH, [] { return odPad::mousePressed(MOUSE_BUTTON_LEFT); }),
          "A is a left click");
    check(tap(SDL_GAMEPAD_BUTTON_WEST, [] { return odPad::mousePressed(MOUSE_BUTTON_RIGHT); }),
          "X is a right click");

    // ── B and the d-pad walk the menus ──
    check(tap(SDL_GAMEPAD_BUTTON_EAST, [] { return odPad::navBack(); }), "B is back");
    int downs = 0;
    tap(SDL_GAMEPAD_BUTTON_DPAD_DOWN, [&] { downs += odPad::navDown() ? 1 : 0; return false; });
    check(downs == 1, "one d-pad press is one menu step");

    // ── the bumpers zoom ──
    float wheel = 0.0f;
    button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
    for (int i = 0; i < 10; ++i) { frame(); wheel += odPad::wheel(); }
    button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
    frames(2);
    check(wheel > 0.0f, "the right bumper zooms (wheel notches)");

    SDL_CloseJoystick(g_pad);
    SDL_DetachVirtualJoystick(id);
    CloseWindow();
    std::printf("a Steam Deck's controls through SDL: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
