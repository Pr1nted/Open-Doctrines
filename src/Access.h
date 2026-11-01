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
 *   hold to confirm   declaring war and deleting a world or a map need the
 *                     button HELD for kHoldSeconds rather than clicked, so a
 *                     tremor or a slipped click cannot do it. gate() decides,
 *                     drawHoldProgress() shows how far along the hold is
 *   longer messages   notifications stay three times as long
 *   sound cues        a refusal sound ("deny") also flashes a marker at the
 *                     pointer, for a player who cannot hear it
 */
namespace odAccess {

inline bool  g_reduceMotion  = false;
inline bool  g_holdToConfirm = false;
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

// ── Hold to confirm ──
constexpr double kHoldSeconds = 0.8;
/// The buttons that hold, by address: a string literal used twice is not
/// promised to be the same object, these are.
inline const char kDeleteWorld = 0, kDeleteMap = 0, kDeclareWar = 0;

struct Hold { const void* id = nullptr; double start = 0; bool fired = false; };
inline Hold g_hold;

/**
 * Whether a destructive button fires this frame. Call it every frame the
 * button exists, with `clicked` being what the button reported. Off: the
 * click, unchanged. On: true once, when the button has been held for
 * kHoldSeconds; letting go early, or sliding off, cancels.
 *
 * `id` names the button: one of the k... objects above.
 */
inline bool gate(const void* id, Rectangle r, Vector2 mouse, bool clicked) {
    if (!g_holdToConfirm) return clicked;
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
    if (!g_holdToConfirm || g_hold.id != id || g_hold.fired) return 0.0f;
    const float p = (float)((GetTime() - g_hold.start) / kHoldSeconds);
    return p < 0 ? 0 : (p > 1 ? 1 : p);
}

/// The hold's progress, as a bar filling along the bottom of the button.
inline void drawHoldProgress(const void* id, Rectangle r) {
    if (!g_holdToConfirm) return;
    const float p = holdProgress(id);
    DrawRectangle((int)r.x + 2, (int)(r.y + r.height) - 5, (int)((r.width - 4) * p), 3,
                  Color{255, 255, 255, 230});
}

}  // namespace odAccess
