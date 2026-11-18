#pragma once
#include "raylib.h"
#include "Config.h"

/**
 * One palette the whole interface reads, chosen by Config::uiStyle, so a future
 * update can offer Classic / Current / Modern skins without editing the ~500
 * colour literals at the draw sites one at a time.
 *
 * THIS IS GROUNDWORK, NOT THE SKINS. `Current` reproduces today's chrome
 * exactly -- selecting it, or leaving the default, is byte-for-byte what the
 * game already draws. `Classic` is a placeholder equal to Current for now;
 * `Modern` is a deliberately small, provisional flat/rounded preview so the
 * switch is visibly doing something. The real palettes are a 1.4.0b decision.
 *
 * It follows the accent() pattern (Config.h): one value, chosen by config, read
 * everywhere, with a mod override that is never written to disk. A free draw
 * function with no Config in scope -- drawButton, shared by the account /
 * multiplayer / LFG / mod-directory screens across ~90 call sites -- reaches the
 * active theme the same process-wide way odUi::scale() is reached: set once from
 * config, read at the draw site, rather than threaded through every caller.
 */
enum UiStyle {
    UI_CURRENT = 0,   // today's look; the default, inert
    UI_CLASSIC = 1,   // placeholder == Current until 1.4.0b fills it
    UI_MODERN  = 2,   // provisional flat/rounded preview
};

struct UiTheme {
    // ── chrome ──
    Color panelBg;             // a generic panel fill
    Color panelBgDeep;         // a recessed / disabled fill
    Color panelBorder;

    // ── buttons (the shared drawButton primitive reads these) ──
    Color buttonDisabledFace;
    Color buttonDisabledBorder;
    int   buttonHoverLift;     // added to r,g,b on hover
    int   buttonHoverAlpha;    // alpha forced on hover
    float buttonRadius;        // DrawRectangleRounded roundness
    int   buttonSegments;

    // ── text ──
    Color textBright;          // hovered / primary
    Color textBody;            // normal
    Color textMuted;
    Color textDisabled;
    Color heading;

    // ── semantic ──
    Color positive;
    Color warning;
    Color danger;

    // ── the player's accent, resolved through Config::accent() ──
    Color accent;
};

namespace odui_detail {
inline Color rgba(int r, int g, int b, int a) {
    return Color{(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a};
}
inline Color fromHex(int hex) {   // 0xRRGGBB, as Config stores the accent
    return Color{(unsigned char)((hex >> 16) & 0xFF),
                 (unsigned char)((hex >> 8) & 0xFF),
                 (unsigned char)(hex & 0xFF), 255};
}
}  // namespace odui_detail

/// Build a theme for a style and an accent (0xRRGGBB). Pure; touches no global.
inline UiTheme themeFor(int style, int accentHex) {
    using odui_detail::rgba;

    // CURRENT -- the values the draw sites use today. The button fields mirror
    // the literals in drawButton (Game_Account.cpp); do not change them without
    // changing those. This block is the inert baseline.
    UiTheme t;
    t.panelBg              = rgba(28, 30, 38, 235);
    t.panelBgDeep          = rgba(20, 20, 25, 200);
    t.panelBorder          = rgba(90, 95, 110, 190);
    t.buttonDisabledFace   = rgba(30, 30, 34, 200);   // drawButton: !enabled bg
    t.buttonDisabledBorder = rgba(70, 70, 80, 180);   // drawButton: !enabled border
    t.buttonHoverLift      = 20;                       // drawButton: +20 on hover
    t.buttonHoverAlpha     = 240;                      // drawButton: hover alpha
    t.buttonRadius         = 0.15f;                    // drawButton roundness
    t.buttonSegments       = 8;
    t.textBright           = rgba(255, 255, 255, 255); // WHITE
    t.textBody             = rgba(200, 200, 200, 255); // LIGHTGRAY
    t.textMuted            = rgba(150, 150, 160, 255);
    t.textDisabled         = rgba(110, 110, 120, 255); // drawButton disabled text
    t.heading              = rgba(230, 230, 235, 255);
    t.positive             = rgba(130, 190, 140, 255);
    t.warning              = rgba(210, 200, 150, 255);
    t.danger               = rgba(190, 120, 120, 255);
    t.accent               = odui_detail::fromHex(accentHex);

    switch (style) {
        case UI_MODERN:
            // PROVISIONAL preview only -- flatter, rounder, brighter body text.
            // The final Modern palette is a 1.4.0b decision, not this.
            t.panelBg         = rgba(24, 26, 32, 245);
            t.panelBorder     = rgba(70, 78, 96, 160);
            t.buttonRadius    = 0.35f;
            t.buttonHoverLift = 28;
            t.textBody        = rgba(222, 226, 232, 255);
            break;
        case UI_CLASSIC:   // placeholder: identical to Current until 1.4.0b
        case UI_CURRENT:
        default:
            break;
    }
    return t;
}

namespace odUi {
/**
 * The process-wide active theme, mirroring odUi::scale(): set once from config
 * (and again whenever a setting changes), read at draw sites that hold no
 * Config. Defaults to Current, so a draw before the first apply is still inert.
 * An inline function-local static is one instance across all translation units.
 */
inline UiTheme& activeThemeMut() {
    static UiTheme t = themeFor(UI_CURRENT, 0xFFD700);
    return t;
}
inline const UiTheme& activeTheme() { return activeThemeMut(); }
inline void setActiveTheme(const UiTheme& t) { activeThemeMut() = t; }
}  // namespace odUi

/// The active theme for a config: its persisted style (or a mod's override) and
/// the player's accent. The one call sites with a Config in scope should use.
inline UiTheme themeOf(const Config& c) {
    return themeFor(c.uiStyleActive(), c.accent());
}
