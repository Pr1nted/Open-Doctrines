#pragma once
#include <cmath>
#include "raylib.h"

/**
 * The accessibility settings that are not colour, and the small helpers that
 * apply them in one place each. High contrast is in Theme.h, which shadows
 * raylib's drawing; these are read where they apply. All of them are off by
 * default and leave the game exactly as it was until a player turns them on
 * under Settings -> Accessibility. See ACCESSIBILITY.md.
 *
 *   reduce motion     the interface's pulsing highlights hold still: every
 *                     one of them is odAccess::wave(), which is a sine of the
 *                     clock, or a constant 0 -- the middle of its swing
 *   text size         odText::drawText and odText::measureText scale every
 *                     size they are handed, so a screen that measures before
 *                     it draws makes room for the bigger text by itself
 *   confirm actions   declaring war and deleting a world or a map ask for a
 *                     second, deliberate gesture rather than a single click, so
 *                     a tremor or a slipped click cannot do it. Two methods: a
 *                     HOLD for kHoldSeconds, or a second CLICK on the same
 *                     button within kTwoClickWindow. gate() decides which fires,
 *                     drawHoldProgress() shows the hold filling or the armed
 *                     second-click waiting
 *   longer messages   notifications stay three times as long
 *   sound cues        a refusal sound ("deny") also flashes a marker at the
 *                     pointer, for a player who cannot hear it
 */
namespace odAccess {

inline bool  g_reduceMotion  = false;
// How a destructive button asks for confirmation. 0 off (a plain click), 1 hold
// for kHoldSeconds, 2 a second click within kTwoClickWindow. See Config::confirmMode.
enum { CONFIRM_OFF = 0, CONFIRM_HOLD = 1, CONFIRM_TWOCLICK = 2 };
inline int   g_confirmMode   = CONFIRM_OFF;
inline bool  g_longMessages  = false;
inline bool  g_soundCues     = false;
inline float g_textScale     = 1.0f;

/// A pulse: sin(time * speed), or 0 -- steady, mid-swing -- with reduced motion.
inline float wave(float speed) {
    return g_reduceMotion ? 0.0f : sinf((float)GetTime() * speed);
}

/// How long a notification of `seconds` actually stays.
inline float messageSeconds(float seconds) { return g_longMessages ? seconds * 3.0f : seconds; }

/// A font size, scaled by the player's text size.
inline int textSize(int size) {
    return g_textScale == 1.0f ? size : (int)lroundf((float)size * g_textScale);
}

// ── Sound cues ──
inline double g_cueAt = -100.0;
/// Called where the game plays a refusal; flashes the marker if cues are on.
inline void cue() { if (g_soundCues) g_cueAt = GetTime(); }
constexpr double kCueSeconds = 0.7;

/// The marker, drawn last in the frame, at the pointer: a ring and a bar, so
/// it reads without colour too.
inline void drawCue(Vector2 at) {
    const double age = GetTime() - g_cueAt;
    if (!g_soundCues || age < 0 || age > kCueSeconds) return;
    const unsigned char a = (unsigned char)(255.0 * (1.0 - age / kCueSeconds));
    DrawRing(at, 14.0f, 18.0f, 0.0f, 360.0f, 32, Color{235, 70, 70, a});
    DrawLineEx({at.x - 9, at.y - 9}, {at.x + 9, at.y + 9}, 3.0f, Color{235, 70, 70, a});
}

// ── Confirm actions (hold, or a second click) ──
constexpr double kHoldSeconds    = 0.8;
constexpr double kTwoClickWindow = 2.5;   ///< how long the first click stays armed
/// The buttons that confirm, by address: a string literal used twice is not
/// promised to be the same object, these are.
inline const char kDeleteWorld = 0, kDeleteMap = 0, kDeclareWar = 0;

struct Hold { const void* id = nullptr; double start = 0; bool fired = false; };
inline Hold g_hold;

/// The button armed by a first click in CONFIRM_TWOCLICK, and when.
struct Arm { const void* id = nullptr; double at = 0; };
inline Arm g_arm;

/// Whether the two-click arming on `id` is still live (armed and unexpired).
inline bool twoClickArmed(const void* id) {
    return g_confirmMode == CONFIRM_TWOCLICK && g_arm.id == id &&
           GetTime() - g_arm.at <= kTwoClickWindow;
}

/**
 * Whether a destructive button fires this frame. Call it every frame the
 * button exists, with `clicked` being what the button reported. Off: the
 * click, unchanged. Hold: true once, when the button has been held for
 * kHoldSeconds; letting go early, or sliding off, cancels. Click twice: the
 * first click arms and does nothing; a second click on the same button within
 * kTwoClickWindow fires. A click elsewhere, or the window lapsing, re-arms.
 *
 * `id` names the button: one of the k... objects above.
 */
inline bool gate(const void* id, Rectangle r, Vector2 mouse, bool clicked) {
    if (g_confirmMode == CONFIRM_OFF) return clicked;
    if (g_confirmMode == CONFIRM_TWOCLICK) {
        if (clicked) {
            if (twoClickArmed(id)) { g_arm.id = nullptr; return true; }
            g_arm.id = id; g_arm.at = GetTime();   // first click (or a re-arm) only arms
        }
        return false;
    }
    // CONFIRM_HOLD
    const bool over = CheckCollisionPointRec(mouse, r);
    const bool down = IsMouseButtonDown(MOUSE_LEFT_BUTTON);
    if (over && down) {
        if (g_hold.id != id) { g_hold.id = id; g_hold.start = GetTime(); g_hold.fired = false; }
        if (!g_hold.fired && GetTime() - g_hold.start >= kHoldSeconds) {
            g_hold.fired = true;
            return true;
        }
    } else if (g_hold.id == id) {
        g_hold.id = nullptr;
    }
    return false;
}

/// How far along the hold on `id` is, 0..1.
inline float holdProgress(const void* id) {
    if (g_confirmMode != CONFIRM_HOLD || g_hold.id != id || g_hold.fired) return 0.0f;
    const float p = (float)((GetTime() - g_hold.start) / kHoldSeconds);
    return p < 0 ? 0 : (p > 1 ? 1 : p);
}

/// The hold filling along the bottom of the button, or -- once armed for a
/// second click -- a bright outline and a bar that drains as the window closes.
inline void drawHoldProgress(const void* id, Rectangle r) {
    if (g_confirmMode == CONFIRM_HOLD) {
        const float p = holdProgress(id);
        DrawRectangle((int)r.x + 2, (int)(r.y + r.height) - 5, (int)((r.width - 4) * p), 3,
                      Color{255, 255, 255, 230});
    } else if (g_confirmMode == CONFIRM_TWOCLICK && twoClickArmed(id)) {
        const float left = (float)((kTwoClickWindow - (GetTime() - g_arm.at)) / kTwoClickWindow);
        DrawRectangleLinesEx(r, 2.0f, Color{255, 255, 255, 230});
        DrawRectangle((int)r.x + 2, (int)(r.y + r.height) - 5,
                      (int)((r.width - 4) * (left < 0 ? 0 : left)), 3, Color{255, 255, 255, 230});
    }
}

}  // namespace odAccess
