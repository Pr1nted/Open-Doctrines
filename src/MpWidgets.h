#pragma once

// The four drawing helpers the multiplayer screens are built from.
//
// They lived in Game_Multiplayer.cpp's anonymous namespace until the
// looking-for-a-game board needed them from a second file. Copying them would
// have produced a board whose buttons were ALMOST the game's buttons -- a
// slightly different radius, a hover that lights a shade brighter, a field
// whose caret blinks out of step. That is the sort of difference nobody can
// name and everybody notices, and it is exactly how a screen starts to look
// bolted on. One definition, two callers.
//
// Everything here draws and reports; nothing here decides. The call sites test
// `click && b.hovered` themselves, because what a press MEANS is their business.

#include "raylib.h"

#include <string>

#include "Audio.h"
#include "i18n/Text.h"

struct MpButton {
    Rectangle rect;
    bool      hovered = false;
};

inline MpButton buttonAt(float x, float y, float w, float h, Vector2 mouse) {
    MpButton b{{x, y, w, h}, false};
    b.hovered = CheckCollisionPointRec(mouse, b.rect);
    return b;
}

inline void drawButton(const MpButton& b, const char* label, int fontSize,
                       Color base, Color border, bool enabled = true) {
    const Color bg = !enabled ? Color{30, 30, 34, 200}
                   : b.hovered ? Color{(unsigned char)(base.r + 20), (unsigned char)(base.g + 20),
                                       (unsigned char)(base.b + 20), 240}
                               : base;
    DrawRectangleRounded(b.rect, 0.15f, 8, bg);
    DrawRectangleRoundedLines(b.rect, 0.15f, 8, enabled ? border : Color{70, 70, 80, 180});
    const char* shown = T(label);
    // THE LABEL IS TRANSLATED HERE, not at the hundred call sites.
    //
    // Every button on these screens comes through this function, so this is the
    // one place that has to know about the language -- the same reasoning that
    // put the shadowed DrawText in i18n/Text.h rather than editing 970 draw
    // sites. A caller passing a literal gets it translated for free; the
    // extractor is told about this function so those literals reach en.json.
    const int tw = MeasureText(shown, fontSize);
    DrawText(shown, (int)(b.rect.x + (b.rect.width - tw) / 2),
             (int)(b.rect.y + (b.rect.height - fontSize) / 2), fontSize,
             enabled ? (b.hovered ? WHITE : LIGHTGRAY) : Color{110, 110, 120, 255});

    // Every multiplayer button is drawn through here, and the call sites test
    // `click && b.hovered` themselves -- so pressing inside a drawn button is
    // exactly the event they act on, and this is the one place to say so.
    if (b.hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
        Audio::get().playSfx(enabled ? "click_heavy" : "deny");
}

inline void drawField(float x, float y, float w, float h, const std::string& text,
                      const char* placeholder, bool focused, int fontSize = 18) {
    DrawRectangleRounded({x, y, w, h}, 0.15f, 8, Color{22, 24, 30, 230});
    DrawRectangleRoundedLines({x, y, w, h}, 0.15f, 8,
                              focused ? Color{150, 180, 220, 230} : Color{80, 85, 100, 200});
    const int ty = (int)(y + (h - fontSize) / 2);
    if (text.empty() && !focused) {
        DrawText(placeholder, (int)x + 12, ty, fontSize, Color{110, 115, 130, 255});
        return;
    }
    // Show the tail when it overflows: the end is what someone is typing.
    std::string shown = text;
    while (!shown.empty() && MeasureText(shown.c_str(), fontSize) > (int)w - 26)
        shown.erase(shown.begin());
    DrawText(shown.c_str(), (int)x + 12, ty, fontSize, RAYWHITE);
    if (focused && ((int)(GetTime() * 2) % 2) == 0) {
        DrawText("_", (int)x + 12 + MeasureText(shown.c_str(), fontSize), ty, fontSize, RAYWHITE);
    }
}

/** Wraps text to a width and draws it, returning the y below the last line. */
inline int wrapText(const std::string& text, int x, int y, int width, int fontSize,
                    Color color, bool draw) {
    std::string line;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t space = text.find(' ', at);
        const std::string word = text.substr(at, space == std::string::npos
                                                 ? std::string::npos : space - at);
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (MeasureText(candidate.c_str(), fontSize) > width && !line.empty()) {
            if (draw) DrawText(line.c_str(), x, y, fontSize, color);
            y += fontSize + 5;
            line = word;
        } else {
            line = candidate;
        }
        if (space == std::string::npos) break;
        at = space + 1;
    }
    if (!line.empty()) {
        if (draw) DrawText(line.c_str(), x, y, fontSize, color);
        y += fontSize + 5;
    }
    return y;
}
