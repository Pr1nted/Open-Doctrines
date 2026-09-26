// ─────────────────────────────────────────────────────────────────────────────
// The language picker, in the two places a player looks for it.
//
// On the main menu it is a flag beside the gear, because the first thing
// somebody who does not read English needs is not buried three screens deep in
// a settings list written in English. In the settings it is a tab of its own,
// because that is where a player who has already started looks.
//
// Both draw the same list from the same code: two entry points, one picker.
// ─────────────────────────────────────────────────────────────────────────────

#include "Game.h"
#include "GameInternals.h"
#include "renderer/FlagRenderer.h"
#include "Audio.h"

#include <algorithm>
#include <string>

namespace {

// One row per language. FOUR columns, not the two this started with.
//
// The list went from twenty-one languages to forty-four. At two columns that
// is twenty-two rows: 64 + 22*46 + 92 + 48 = 1216px, which runs off a 900px
// window, off a 1280x720 desktop by 174px, and off a portrait phone. Three
// columns still overflow both of those. Four is eleven rows and 710px --
// exactly the height the picker had at twenty-one languages, so nothing that
// fitted before stops fitting now.
//
// It matters more than it looks: NEITHER SURFACE SCROLLS. A list that does not
// fit is not merely ugly, it is a list whose bottom rows cannot be clicked,
// and the languages added last would be the unreachable ones.
constexpr int kRowH = 46;
constexpr int kFlagW = 40;
constexpr int kFlagH = 26;
constexpr int kCols = 4;

int rowsFor(int count) { return (count + kCols - 1) / kCols; }

}  // namespace

Texture2D Game::languageFlag(const char* iso) {
    auto it = m_langFlags.find(iso);
    if (it != m_langFlags.end()) return it->second;

    // The flag as shipped, not a generated one: the file is already on disk
    // for the map to use.
    //
    // THE PNG FIRST. Every flag in data/flags was pre-rendered to PNG by the
    // pipeline precisely because nanosvg, which is what reads the .svg here,
    // cannot draw a clipPath or a negative viewBox -- and the flags that need
    // one include the United Kingdom's, which is the flag beside English. The
    // .svg is kept as the fallback for anything the pipeline has not rendered.
    FlagPattern fp;
    const std::string png = std::string("flags/") + iso + ".png";
    fp.imagePath = FileExists((m_dataDir + png).c_str())
                       ? png
                       : std::string("flags/") + iso + ".svg";
    Texture2D tex = FlagRenderer::render(fp, kFlagW * 2, kFlagH * 2, m_dataDir, nullptr);
    m_langFlags[iso] = tex;
    return tex;
}

void Game::unloadLanguageFlags() {
    for (auto& [iso, tex] : m_langFlags)
        if (tex.id > 0) UnloadTexture(tex);
    m_langFlags.clear();
}

// Exposed because the settings tab draws this list too, and computed its own
// row count with a hardcoded two -- which had already drifted from rowsFor()
// and would have put the disclaimer eleven rows adrift at four columns. One
// answer, one place.
int Game::languageListRows() const {
    return rowsFor((int)od::i18n::languages().size());
}

Rectangle Game::languagePickerBounds() const {
    const auto& ls = od::i18n::languages();
    const float w = std::min(760.0f, (float)m_screenW - 80.0f);
    // The list, the heading above it and the disclaimer below.
    // +48 for the Back button row along the bottom.
    const float h = 64.0f + rowsFor((int)ls.size()) * (float)kRowH + 92.0f + 48.0f;
    return {(m_screenW - w) * 0.5f, (m_screenH - h) * 0.5f, w, h};
}

// The Back button, bottom-left of the picker. Its own function because the
// drawing and the hit test have to agree and they live in different places.
Rectangle Game::languageBackButton() const {
    const Rectangle b = languagePickerBounds();
    const float w = 104.0f, h = 34.0f;
    return {b.x + 24.0f, b.y + b.height - h - 14.0f, w, h};
}

void Game::drawLanguageList(Rectangle area, bool withHeading) {
    const auto& ls = od::i18n::languages();
    const Color accent = hexToColor(m_config.accent());
    const Vector2 mouse = getMouse();

    float y = area.y;
    if (withHeading) {
        DrawText(T("Language"), (int)area.x, (int)y, 26, Color{230, 230, 240, 255});
        y += 40.0f;
    }

    const float colW = area.width / (float)kCols;
    for (int i = 0; i < (int)ls.size(); ++i) {
        const od::i18n::Language& l = ls[i];
        const int col = i / rowsFor((int)ls.size());
        const int row = i % rowsFor((int)ls.size());
        const Rectangle r{area.x + col * colW, y + row * (float)kRowH,
                          colW - 12.0f, (float)kRowH - 6.0f};
        const bool active = (od::i18n::language() == l.code);
        const bool hover = CheckCollisionPointRec(mouse, r);

        if (active) {
            DrawRectangleRounded(r, 0.25f, 6, ColorAlpha(accent, 0.18f));
            DrawRectangleRoundedLines(r, 0.25f, 6, ColorAlpha(accent, 0.9f));
        } else if (hover) {
            DrawRectangleRounded(r, 0.25f, 6, Color{255, 255, 255, 18});
        }

        // The flag first: it is what somebody scanning for their own language
        // finds before they read anything.
        Texture2D flag = languageFlag(l.flagIso);
        const Rectangle dst{r.x + 10.0f, r.y + (r.height - kFlagH) * 0.5f,
                            (float)kFlagW, (float)kFlagH};
        if (flag.id > 0) {
            DrawTexturePro(flag, {0, 0, (float)flag.width, (float)flag.height}, dst,
                           {0, 0}, 0.0f, WHITE);
            DrawRectangleLinesEx(dst, 1, Color{0, 0, 0, 90});
        } else {
            DrawRectangleRec(dst, Color{40, 44, 52, 255});
        }

        // The endonym, because a picker that says "German" to somebody looking
        // for Deutsch is a picker for people who already read English.
        //
        // FITTED TO THE CELL, because four columns are narrower than two and
        // two names no longer fit at 18pt: "Norsk bokmål", and "Slovenščina"
        // -- which has been in this list all along and would have begun
        // clipping the moment the columns narrowed. Truncating the name a
        // reader is scanning for is the one thing this picker must not do, so
        // the type shrinks instead, the way the main menu handles its items.
        const int tx = (int)(dst.x + dst.width + 12.0f);
        int endoSize = 18;
        const int endoRoom = (int)(r.x + r.width - (float)tx - 8.0f);
        const std::string endo = odText::fitToWidth(l.endonym, endoRoom, endoSize);
        DrawText(endo.c_str(), tx, (int)(r.y + 8.0f + (18 - endoSize) * 0.5f), endoSize,
                 active ? WHITE : Color{215, 215, 225, 255});

        // How much of it exists. An honest number beats a flag that promises a
        // translated game and delivers four menus.
        if (std::string(l.code) != "en") {
            int done = 0, total = 0;
            if (active) od::i18n::coverage(done, total);
            const std::string note =
                active && total > 0
                    ? std::to_string(100 * done / total) + "%"
                    : std::string(l.english);
            DrawText(note.c_str(), tx, (int)(r.y + 26.0f), 12, Color{140, 145, 158, 255});
        }
    }
}

void Game::drawLanguageDisclaimer(Rectangle area) {
    // THE WARNING IS NOT SMALL PRINT.
    //
    // Every language here but English was produced by a machine, and a player
    // who hits a mistranslated button should know that is what happened rather
    // than doubting their own reading. It sits under the list, in the language
    // being offered, at the size of the text around it.
    const Color warn{225, 190, 110, 255};
    DrawRectangleRounded({area.x, area.y, area.width, area.height}, 0.15f, 6,
                         Color{40, 34, 20, 180});
    DrawRectangleRoundedLines({area.x, area.y, area.width, area.height}, 0.15f, 6,
                              ColorAlpha(warn, 0.55f));

    // One string, wrapped by odText: the warning is a whole sentence in every
    // language, and the hand-rolled wrapper this used to carry could not break
    // Japanese at all -- there are no spaces in it to break on.
    odText::drawWrapped(
        T("Most of this translation was made by a machine and will contain mistakes. The English text is the original."),
        (int)area.x + 14, (int)area.y + 12, (int)area.width - 28, 14, warn);
}

bool Game::updateLanguageList(Rectangle area, bool withHeading) {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return false;
    const auto& ls = od::i18n::languages();
    const Vector2 mouse = getMouse();
    const float y0 = area.y + (withHeading ? 40.0f : 0.0f);
    const float colW = area.width / (float)kCols;

    for (int i = 0; i < (int)ls.size(); ++i) {
        const int col = i / rowsFor((int)ls.size());
        const int row = i % rowsFor((int)ls.size());
        const Rectangle r{area.x + col * colW, y0 + row * (float)kRowH,
                          colW - 12.0f, (float)kRowH - 6.0f};
        if (!CheckCollisionPointRec(mouse, r)) continue;
        Audio::get().playSfx("click_light");
        if (!applyLanguage(ls[i].code))
            addNotification(T("That language could not be loaded."), RED);
        return true;
    }
    return false;
}

void Game::drawLanguagePicker() {
    const Rectangle b = languagePickerBounds();
    // Everything behind it goes dark: this is a modal choice and the menu
    // underneath must not look clickable.
    DrawRectangle(0, 0, m_screenW, m_screenH, Color{0, 0, 0, 170});
    DrawRectangleRounded(b, 0.06f, 8, Color{18, 20, 26, 250});
    DrawRectangleRoundedLines(b, 0.06f, 8, ColorAlpha(hexToColor(m_config.accent()), 0.7f));

    const Rectangle inner{b.x + 24.0f, b.y + 22.0f, b.width - 48.0f, b.height - 44.0f};
    drawLanguageList(inner, /*withHeading=*/true);

    const auto& ls = od::i18n::languages();
    const float listBottom = inner.y + 40.0f + rowsFor((int)ls.size()) * (float)kRowH;
    drawLanguageDisclaimer({inner.x, listBottom + 8.0f, inner.width, 64.0f});

    // A BUTTON, not just Escape.
    //
    // The panel closed on the Escape key or on a click outside it. Neither
    // exists on a phone: there is no Escape key, and "tap outside the panel"
    // is not something anybody discovers -- so an Android player who opened
    // the language list had no way back out of it. The hint stays for the
    // people who do have a keyboard.
    const Rectangle back = languageBackButton();
    const bool hot = CheckCollisionPointRec(getMouse(), back);
    DrawRectangleRounded(back, 0.3f, 8, hot ? Color{40, 44, 54, 245}
                                            : Color{26, 28, 36, 235});
    DrawRectangleRoundedLines(back, 0.3f, 8,
                              ColorAlpha(hexToColor(m_config.accent()), hot ? 0.95f : 0.6f));
    const char* label = T("< Back");
    DrawText(label, (int)(back.x + (back.width - MeasureText(label, 15)) * 0.5f),
             (int)(back.y + (back.height - 15) * 0.5f), 15,
             hot ? WHITE : Color{190, 195, 205, 235});

    const char* hint = T("Press Escape to close");
    DrawText(hint, (int)(b.x + b.width - MeasureText(hint, 12) - 16),
             (int)(b.y + 12), 12, Color{120, 124, 136, 255});
}

void Game::updateLanguagePicker() {
    if (IsKeyPressed(KEY_ESCAPE)) { m_languageOpen = false; return; }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        CheckCollisionPointRec(getMouse(), languageBackButton())) {
        m_languageOpen = false;
        return;
    }
    const Rectangle b = languagePickerBounds();
    const Rectangle inner{b.x + 24.0f, b.y + 22.0f, b.width - 48.0f, b.height - 44.0f};
    if (updateLanguageList(inner, /*withHeading=*/true)) return;
    // A click outside the panel closes it, which is what every other overlay
    // in this game does.
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !CheckCollisionPointRec(getMouse(), b))
        m_languageOpen = false;
}
