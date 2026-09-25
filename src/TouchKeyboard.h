#pragma once

/**
 * A keyboard drawn by the game, for the platforms that have none.
 *
 * WHY THIS EXISTS. Android has no keyboard as far as this game is concerned:
 * raylib's NativeActivity backend delivers no soft keyboard and there is no
 * IME, so every text field in the game -- an invite code, a server address, a
 * chat line, a world's name -- was unusable on a phone. That is why the APK
 * shipped with multiplayer compiled out: joining a game means typing a code.
 *
 * WHAT IT IS. Keys drawn with the rest of the frame and tapped like any other
 * button. What they produce is fed to the game through the same two calls
 * every text field already reads -- GetCharPressed and IsKeyPressed -- so no
 * field had to be rewritten and nothing has to know where the character came
 * from. The shim in TouchInput.h is how those two calls are redirected; this
 * header is the keyboard itself.
 *
 * WHEN IT SHOWS. Only while something is actually reading text: a frame in
 * which the game asked for a character is a frame in which a field has focus,
 * and that is a better signal than any flag the screens would have to remember
 * to set. It follows focus automatically, including in screens written years
 * before it existed.
 *
 * The layout is plain data with no raylib in it, so the mapping from a tap to
 * a character is tested without a window (tests/touch_keyboard_test.cpp).
 */

#include <cstddef>
#include <vector>

namespace osk {

/** Keys that do something other than produce a character. */
enum class Special {
    None = 0,
    Backspace,
    Enter,
    Shift,
    Symbols,   ///< swap between letters and punctuation
    Space,
    Close,     ///< put the keyboard away without changing the text
};

/**
 * One key, in GRID UNITS: x and width are in key-widths, y is the row.
 *
 * Grid units rather than pixels because the same layout has to sit on a phone
 * held in one hand and on a tablet, and because a test should be able to ask
 * what is at a position without opening a window.
 */
struct Key {
    /// What is drawn on it. Owned, so the layout is one self-contained value.
    char        cap[8] = {0};
    char        ch = 0;      ///< what it types; 0 for a Special
    Special     special = Special::None;
    float       x = 0;       ///< left edge, in key widths
    float       row = 0;     ///< which row, from the top
    float       w = 1;       ///< width, in key widths
};

/** Widest row, in key widths. Everything else is measured against this. */
inline constexpr float kGridWidth = 10.0f;
/**
 * How many rows the keyboard has.
 *
 * FIVE, because the top one is digits and they are on BOTH boards. An invite
 * code is four characters, a dash and four more out of `ABCDEFGHJKMNPQRSTUVWXYZ
 * 23456789` -- so a code with a digit in it, which most have, meant switching
 * boards in the middle of typing the one thing every player has to type.
 */
inline constexpr float kGridRows = 5.0f;

/** The keys, for the mode asked for. Pure; no raylib, no state. */
const std::vector<Key>& layout(bool shifted, bool symbols);

/** The key at a point in grid units, or nullptr where there is no key. */
const Key* keyAt(float gx, float gy, bool shifted, bool symbols);

// ── The running keyboard ────────────────────────────────────────────────────

/** Is this a build and a platform that draws one? */
bool enabled();

/**
 * The next character for a text field: a tapped one first, then the real
 * keyboard's. Calling this is ALSO what says a field is reading text, which is
 * what brings the keyboard up.
 */
int charPressed();

/** As IsKeyPressed, with the keys this keyboard can produce folded in. */
bool keyPressed(int key);

/** Draw it, if it is wanted. Call once a frame, after the rest of the frame. */
void draw();

/** True while the keyboard is on screen, so callers can keep clear of it. */
bool visible();

/** The fraction of the screen height the keyboard covers when it is up. */
float coverage();

/** Forget the frame's focus signal. Called from the frame loop. */
void endFrame();

/**
 * Show it regardless, for the screenshot tour.
 *
 * The tour runs on a desktop, where the keyboard is off -- and a photograph of
 * it is the only check on how it LOOKS that exists, since the layout test
 * cannot see. Nothing else calls this.
 */
void forceForShot(bool on);

}  // namespace osk
