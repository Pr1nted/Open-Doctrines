// The keyboard for platforms that have none. See TouchKeyboard.h.

#include "TouchKeyboard.h"

// raylib's own calls, not the shim's: this file IS the shim's other side, and
// redirecting its own reads to itself would be a loop.
#ifdef GetCharPressed
  #undef GetCharPressed
#endif
#ifdef IsKeyPressed
  #undef IsKeyPressed
#endif

#include "raylib.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

namespace osk {
namespace {

/**
 * THE LETTERS ARE IN ALPHABETICAL ORDER, not QWERTY.
 *
 * What gets typed here is an invite code, a tunnel hostname or a short line of
 * chat, hunted for one character at a time with a thumb. Alphabetical is
 * faster to hunt than QWERTY for exactly that, and it is what a console
 * on-screen keyboard uses for the same reason. Somebody writing an essay on a
 * phone would want their phone's own keyboard, which this cannot be.
 */
// i18n-ignore: these are KEYS, not words. What a key types has to be a
// character the game's text fields accept, and those are ASCII by design --
// the same reason the find panel refuses anything else: the atlas can draw any
// script, but raylib gives no IME, so a name is typed in Latin wherever the
// player is. A translated keyboard would type characters no field would take.
// i18n-ignore
const char* const kRow1 = "ABCDEFGHIJ";
// i18n-ignore
const char* const kRow2 = "KLMNOPQRST";
// i18n-ignore
const char* const kRow3 = "UVWXYZ";

/**
 * Digits, on both boards: see kGridRows. A code is letters AND digits.
 */
const char* const kDigits = "1234567890";

/** The punctuation an address, a code or a chat line actually needs. */
const char* const kSym2 = ".,:/-_@#?!";
const char* const kSym3 = "'\"()+=%&";
const char* const kSym4 = "*;<>[]{}";

Key named(const char* cap, char ch, Special special, float x, float row, float w) {
    Key k;
    std::snprintf(k.cap, sizeof(k.cap), "%s", cap);
    k.ch = ch;
    k.special = special;
    k.x = x;
    k.row = row;
    k.w = w;
    return k;
}

std::vector<Key> build(bool shifted, bool symbols) {
    std::vector<Key> keys;
    auto addRow = [&keys, shifted](const char* chars, float row, float x0) {
        float x = x0;
        for (const char* p = chars; *p; ++p) {
            const char c = shifted ? *p
                                   : (char)(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
            const char cap[2] = {c, 0};
            keys.push_back(named(cap, c, Special::None, x, row, 1.0f));
            x += 1.0f;
        }
    };

    addRow(kDigits, 0, 0);          // on both boards, for codes
    if (symbols) {
        addRow(kSym2, 1, 0);
        addRow(kSym3, 2, 0);
        addRow(kSym4, 3, 0);
    } else {
        addRow(kRow1, 1, 0);
        addRow(kRow2, 2, 0);
        addRow(kRow3, 3, 0);
    }

    // The last letter row is short, so the keys that need room live beside it.
    const float tail = symbols ? (float)std::strlen(kSym4) : (float)std::strlen(kRow3);
    const float shiftW = symbols ? 0.0f : 2.0f;
    if (!symbols) keys.push_back(named(shifted ? "abc" : "ABC", 0, Special::Shift,
                                       tail, 3, shiftW));
    keys.push_back(named("<--", 0, Special::Backspace, tail + shiftW, 3,
                         kGridWidth - tail - shiftW));

    keys.push_back(named(symbols ? "ABC" : "?123", 0, Special::Symbols, 0, 4, 2.0f));
    keys.push_back(named("space", ' ', Special::Space, 2, 4, 5.0f));
    keys.push_back(named("done", 0, Special::Enter, 7, 4, 2.0f));
    keys.push_back(named("v", 0, Special::Close, 9, 4, 1.0f));
    return keys;
}

// ── what the game will read next ──
std::deque<int>  g_chars;
std::deque<int>  g_keys;
bool  g_wantedThisFrame = false;
bool  g_wantedLastFrame = false;
bool  g_shift = true;          // a code is upper case, so start there
bool  g_symbols = false;
bool  g_dismissed = false;     // closed by the player; stays down until refocus
double g_lastTap = 0.0;

bool forced() {
    static const bool on = [] {
        const char* s = getenv("OD_TOUCH_KEYBOARD");
        return s && *s && *s != '0';
    }();
    return on;
}

}  // namespace

const std::vector<Key>& layout(bool shifted, bool symbols) {
    // Four, because that is how many combinations there are, and they never
    // change once built.
    static const std::vector<Key> lower      = build(false, false);
    static const std::vector<Key> upper      = build(true,  false);
    static const std::vector<Key> symLower   = build(false, true);
    static const std::vector<Key> symUpper   = build(true,  true);
    if (symbols) return shifted ? symUpper : symLower;
    return shifted ? upper : lower;
}

const Key* keyAt(float gx, float gy, bool shifted, bool symbols) {
    const std::vector<Key>& keys = layout(shifted, symbols);
    for (const Key& k : keys) {
        if (gy < k.row || gy >= k.row + 1.0f) continue;
        if (gx < k.x || gx >= k.x + k.w) continue;
        return &k;
    }
    return nullptr;
}

bool g_forShot = false;

void forceForShot(bool on) {
    g_forShot = on;
    g_wantedLastFrame = on;
    g_dismissed = false;
}

bool enabled() {
    if (g_forShot) return true;
#if defined(__ANDROID__)
    return true;
#else
    // Everywhere else it is off unless asked for: a desktop has a keyboard,
    // and one drawn over the game would be in the way. The switch exists so
    // this can be looked at and photographed on a machine with a screen big
    // enough to see it (the screenshot tour uses it).
    return forced();
#endif
}

int charPressed() {
    // ASKING IS THE SIGNAL. A frame in which the game wants a character is a
    // frame in which a field has focus; see the header.
    if (enabled()) g_wantedThisFrame = true;
    if (!g_chars.empty()) {
        const int c = g_chars.front();
        g_chars.pop_front();
        return c;
    }
    return ::GetCharPressed();
}

bool keyPressed(int key) {
    for (auto it = g_keys.begin(); it != g_keys.end(); ++it) {
        if (*it == key) { g_keys.erase(it); return true; }
    }
    return ::IsKeyPressed(key);
}

bool visible() { return enabled() && g_wantedLastFrame && !g_dismissed; }

float coverage() { return visible() ? 0.42f : 0.0f; }

void endFrame() {
    if (g_forShot) { g_wantedLastFrame = true; g_wantedThisFrame = false; return; }
    // A field that stopped reading has stopped being focused, and a keyboard
    // that was put away comes back when the next one is.
    if (!g_wantedThisFrame && g_wantedLastFrame) g_dismissed = false;
    g_wantedLastFrame = g_wantedThisFrame;
    g_wantedThisFrame = false;
}

void draw() {
    if (!visible()) return;

    const float screenW = (float)GetScreenWidth();
    const float screenH = (float)GetScreenHeight();
    const float boardH = screenH * coverage();
    const float top = screenH - boardH;
    const float keyW = screenW / kGridWidth;
    const float keyH = boardH / kGridRows;

    DrawRectangle(0, (int)top, (int)screenW, (int)boardH, Color{16, 18, 24, 245});
    DrawRectangle(0, (int)top, (int)screenW, 2, Color{70, 80, 100, 255});

    // One touch at a time, released rather than pressed: a finger that lands
    // on the wrong key can slide off it.
    const Vector2 m = GetMousePosition();
    const bool tapped = IsMouseButtonReleased(MOUSE_BUTTON_LEFT) &&
                        GetTime() - g_lastTap > 0.06;   // ignore a double report

    const std::vector<Key>& keys = layout(g_shift, g_symbols);
    for (const Key& k : keys) {
        const Rectangle r{k.x * keyW + 3, top + k.row * keyH + 3,
                          k.w * keyW - 6, keyH - 6};
        const bool over = CheckCollisionPointRec(m, r);
        const bool wide = k.special != Special::None;
        DrawRectangleRounded(r, 0.2f, 6,
                             over ? Color{58, 66, 84, 255}
                                  : wide ? Color{34, 38, 48, 255} : Color{44, 48, 60, 255});
        const int fs = k.special == Special::None ? (int)(keyH * 0.42f) : (int)(keyH * 0.30f);
        const int tw = MeasureText(k.cap, fs);
        DrawText(k.cap, (int)(r.x + (r.width - tw) / 2), (int)(r.y + (r.height - fs) / 2),
                 fs, over ? WHITE : Color{215, 220, 235, 255});

        if (!tapped || !over) continue;
        g_lastTap = GetTime();
        switch (k.special) {
            case Special::None:
            case Special::Space:
                g_chars.push_back(k.ch);
                break;
            case Special::Backspace: g_keys.push_back(KEY_BACKSPACE); break;
            case Special::Enter:     g_keys.push_back(KEY_ENTER); break;
            case Special::Shift:     g_shift = !g_shift; break;
            case Special::Symbols:   g_symbols = !g_symbols; break;
            case Special::Close:     g_dismissed = true; break;
        }
    }
}

}  // namespace osk
