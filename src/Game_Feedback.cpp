// The report form, the rating prompt, and the diagnostics the game offers to
// attach.
//
// THE FORM IS DELIBERATELY SHORT. Two fields, a row of categories and one tick
// box. Every field added to a bug report is a field a player can be wrong
// about, and the thing that actually makes a report useful -- the version, the
// platform, the turn, the mods -- the game already knows and should never ask
// for.
//
// It opens over whatever is behind it, including the map editor, and it takes
// the keyboard while it is open. A form that loses what you typed because a
// hotkey fired underneath is worse than no form.

#include "Game.h"
#include "GameInternals.h"
#include "Feedback.h"
#include "Audio.h"
#include "i18n/Locale.h"
#include "i18n/Text.h"
#include "ai/AIVersion.h"
#include "mods/ModManager.h"
#include "net/AccountClient.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include "OdState.h"
#include "util/OpenLink.h"
#include "util/WebPersist.h"
#include "MpWidgets.h"   // wrapText, shared with the multiplayer screens
#include "TextInput.h"

#include <cstdio>

std::string Game::feedbackDiagnostics() const {
    std::ostringstream o;
    o << "OpenDoctrines " << GAME_VERSION << "  (" << feedback::platform() << ")\n";
    o << "AI: " << ai::versionString() << "\n";
    o << "Window: " << m_screenW << "x" << m_screenH << " @ " << GetFPS() << " fps\n";
    o << "Language: " << od::i18n::language() << "\n";
    o << "Accent: " << m_config.accent() << ", UI scale " << m_config.uiScale << "\n";

    // The world, when there is one. A report sent from the menu has no turn
    // number, and printing "turn 0" would be a claim rather than an absence.
    if (m_currentScreen == SCREEN_PLAYING || m_turnNumber > 0) {
        o << "\nWorld\n";
        o << "  turn " << m_turnNumber
          << ", date " << (m_mapDate.empty() ? "-" : m_mapDate) << "\n";
        o << "  " << m_countries.getAll().size() << " countries, "
          << m_provinces.getAllProvinces().size() << " provinces\n";
        if (const Country* c = m_countries.getCountry(m_playerCountryId)) {
            o << "  playing " << c->name << " (" << c->isoA3 << "), treasury "
              << (long long)c->treasury << "\n";
        }
        o << "  difficulty " << m_config.aiDifficulty
          << ", multiplayer " << (m_netSession ? "yes" : "no") << "\n";
        if (!m_currentSavePath.empty()) {
            // The FILENAME, not the path: which save is useful, where it lives
            // is a directory that has the player's name in it.
            const size_t slash = m_currentSavePath.find_last_of("/\\");
            o << "  save: "
              << (slash == std::string::npos ? m_currentSavePath
                                             : m_currentSavePath.substr(slash + 1)) << "\n";
        }
    }

    // "It broke after I installed something" is the commonest report there is,
    // and this list answers it before anybody has to ask.
    {
        const auto& mods = ModManager::get().mods();
        o << "\nMods (" << mods.size() << ")\n";
        size_t shown = 0;
        for (const auto& m : mods) {
            if (shown++ >= 24) { o << "  ... and " << (mods.size() - 24) << " more\n"; break; }
            o << "  " << m.id << " " << m.manifest.version
              << (m.enabled ? " [on]" : " [off]") << "\n";
        }
        if (mods.empty()) o << "  (none)\n";
    }

    // What the map's own scripts complained about. A scripting report with the
    // engine's own error in it is most of the way to fixed.
    if (!m_scriptErrors.empty()) {
        o << "\nScript errors (" << m_scriptErrors.size() << ")\n";
        size_t shown = 0;
        for (const auto& e : m_scriptErrors) {
            if (shown++ >= 10) break;
            o << "  " << e.scriptName << ":" << e.lineNum << "  " << e.message << "\n";
        }
    }

    return feedback::scrubPersonalPaths(o.str());
}

void Game::openFeedbackForm(feedback::Kind kind, feedback::Category category) {
    std::string why;
    if (!feedback::canSend(m_config, why)) {
        m_feedbackNotice = why;          // said BEFORE they type, not after
        m_feedbackNoticeUntil = GetTime() + 6.0;
        Audio::get().playSfx("deny");
        return;
    }
    m_feedbackOpen = true;
    m_feedbackKind = kind;
    m_feedbackCategory = category;
    m_feedbackTitle.clear();
    m_feedbackBody.clear();
    m_feedbackField = 0;
    m_feedbackAttach = (kind == feedback::Kind::Bug);   // a bug wants them; a wish does not
    m_feedbackPreview = false;
    m_feedbackSwallowClick = true;
    m_feedbackDiag = feedbackDiagnostics();
    feedback::clearMessage();
    Audio::get().playSfx("panel_open");
}

void Game::closeFeedbackForm() {
    m_feedbackOpen = false;
    m_feedbackPreview = false;
    Audio::get().playSfx("panel_close");
}

void Game::submitFeedbackForm() {
    feedback::Report r;
    r.kind = m_feedbackKind;
    r.category = m_feedbackCategory;
    r.title = m_feedbackTitle;
    r.body = m_feedbackBody;
    r.anonymous = m_feedbackAnonymous;
    // Exactly the text that was on screen. Rebuilding it here would mean
    // sending something the player never saw.
    if (m_feedbackAttach) r.diagnostics = m_feedbackDiag;

    if (feedback::send(r, m_config, m_configPath, GAME_VERSION)) {
        m_config.save(m_configPath);     // the install id and the day's count
        Audio::get().playSfx("confirm");
        return;
    }

    // Refused, and the player is looking at a report they have just written. A
    // Send that does nothing is the worst possible answer here: say which limit
    // it hit, on the form, above the button they pressed. (The cooldown is the
    // usual one -- a failed send is still counted, so a retry waits.)
    std::string why;
    feedback::canSend(m_config, why);
    m_feedbackNotice = why.empty() ? std::string(T("Could not send. Please try again shortly."))
                                   : why;
    m_feedbackNoticeUntil = GetTime() + 8.0;
    Audio::get().playSfx("deny");
}

// ─────────────────────────────────────────────────────────────── the form ────

// Why a report could not be sent, said where the player is.
//
// Drawn from endFrame() AFTER the form, not inside it: it is raised both when
// the form refuses to open (no form to draw it in) and when Send is refused
// (the form's own dim overlay is over the bottom of the screen). One place that
// works in both cases beats two that each work in one.
void Game::drawFeedbackNotice() {
    if (m_feedbackNotice.empty()) return;
    if (GetTime() > m_feedbackNoticeUntil) { m_feedbackNotice.clear(); return; }

    const int tw = MeasureText(m_feedbackNotice.c_str(), 14);
    const int bw = std::min(tw + 32, m_screenW - 40);
    const int bx = (m_screenW - bw) / 2, by = m_screenH - 120;
    DrawRectangleRounded({(float)bx, (float)by, (float)bw, 40}, 0.3f, 8, Color{40, 26, 28, 245});
    DrawRectangleRoundedLines({(float)bx, (float)by, (float)bw, 40}, 0.3f, 8,
                              Color{170, 110, 110, 230});
    int fs = 14;
    const std::string fit = odText::fitToWidth(m_feedbackNotice, bw - 24, fs, 10);
    DrawText(fit.c_str(), bx + 16, by + 20 - fs / 2, fs, Color{235, 195, 195, 255});
}

void Game::drawFeedbackForm() {
    if (!m_feedbackOpen) return;

    const Vector2 mouse = getMouse();
    const bool click = IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !m_feedbackSwallowClick;
    const Color accent = hexToColor(m_config.accent());

    DrawRectangle(0, 0, m_screenW, m_screenH, Color{6, 7, 11, 232});

    const int w = std::min(760, m_screenW - 80);
    const int h = std::min(560, m_screenH - 80);
    const int x = (m_screenW - w) / 2, y = (m_screenH - h) / 2;
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.02f, 8,
                         Color{16, 18, 24, 250});
    DrawRectangleRoundedLines({(float)x, (float)y, (float)w, (float)h}, 0.02f, 8,
                              Color{70, 74, 96, 220});

    const bool bug = m_feedbackKind == feedback::Kind::Bug;
    DrawText(bug ? T("Report a problem") : T("Send a suggestion"), x + 24, y + 20, 24, accent);

    // ── The diagnostics preview takes the whole panel while it is open ──
    //
    // Shown in full, scrollable, before anything is sent. A summary would defeat
    // the point: the promise is that the player can read exactly what leaves
    // their machine, and a promise about a summary is not that promise.
    if (m_feedbackPreview) {
        DrawText(T("This is everything that would be attached:"), x + 24, y + 56, 13,
                 Color{170, 176, 196, 255});
        const Rectangle box = {(float)(x + 24), (float)(y + 78), (float)(w - 48), (float)(h - 150)};
        DrawRectangleRec(box, Color{10, 11, 16, 255});
        DrawRectangleLinesEx(box, 1, Color{60, 64, 84, 200});
        BeginScissorMode((int)box.x, (int)box.y, (int)box.width, (int)box.height);
        int ly = (int)box.y + 8 - m_feedbackPreviewScroll;
        std::istringstream lines(m_feedbackDiag);
        std::string line;
        while (std::getline(lines, line)) {
            if (ly > box.y - 14 && ly < box.y + box.height)
                DrawText(line.c_str(), (int)box.x + 10, ly, 12, Color{190, 196, 216, 255});
            ly += 15;
        }
        EndScissorMode();
        if (CheckCollisionPointRec(mouse, box)) {
            const float wheel = odScrollWheel(box);
            if (wheel != 0.0f) {
                const int maxScroll = std::max(0, ly - (int)box.y - (int)box.height + 20 +
                                                    m_feedbackPreviewScroll);
                m_feedbackPreviewScroll =
                    std::clamp(m_feedbackPreviewScroll - (int)(wheel * 45), 0, maxScroll);
            }
        }
        const Rectangle back = {(float)(x + 24), (float)(y + h - 56), 160, 34};
        const bool bh = CheckCollisionPointRec(mouse, back);
        DrawRectangleRounded(back, 0.2f, 6, bh ? Color{44, 48, 66, 240} : Color{28, 30, 42, 220});
        DrawRectangleRoundedLines(back, 0.2f, 6, Color{90, 96, 130, 200});
        DrawText(T("Back to the report"), (int)back.x + 12, (int)back.y + 10, 13, WHITE);
        if (bh && click) { m_feedbackPreview = false; Audio::get().playSfx("back"); }
        return;
    }

    int cy = y + 56;

    // ── What kind of thing is it ──
    {
        const char* kinds[2] = {T("Something is broken"), T("An idea")};
        for (int k = 0; k < 2; ++k) {
            const Rectangle r = {(float)(x + 24 + k * 190), (float)cy, 180, 28};
            const bool on = ((int)m_feedbackKind == k);
            const bool hov = CheckCollisionPointRec(mouse, r);
            DrawRectangleRounded(r, 0.25f, 6, on ? Color{40, 60, 48, 245}
                                                 : (hov ? Color{34, 36, 48, 235} : Color{22, 24, 32, 220}));
            DrawRectangleRoundedLines(r, 0.25f, 6, on ? accent : Color{70, 74, 96, 180});
            int fs = 13;
            const std::string fit = odText::fitToWidth(kinds[k], (int)r.width - 16, fs, 10);
            DrawText(fit.c_str(), (int)r.x + 10, (int)r.y + 7, fs, on ? WHITE : Color{170, 176, 196, 255});
            if (hov && click && !on) {
                m_feedbackKind = (feedback::Kind)k;
                m_feedbackAttach = (m_feedbackKind == feedback::Kind::Bug);
                Audio::get().playSfx("click_light", 0.1f);
            }
        }
        cy += 40;
    }

    // ── Which part of the game ──
    DrawText(T("Which part of the game?"), x + 24, cy, 12, Color{150, 156, 176, 255});
    cy += 18;
    {
        int cx = x + 24;
        for (int i = 0; i < (int)feedback::Category::Count; ++i) {
            const auto cat = (feedback::Category)i;
            const char* label = T(feedback::categoryLabel(cat));
            const int cw = MeasureText(label, 12) + 20;
            if (cx + cw > x + w - 24) { cx = x + 24; cy += 28; }
            const Rectangle r = {(float)cx, (float)cy, (float)cw, 24};
            const bool on = (m_feedbackCategory == cat);
            const bool hov = CheckCollisionPointRec(mouse, r);
            // Security is coloured apart, because picking it changes where the
            // report goes and the player should be able to see that it is not
            // an ordinary chip.
            const Color onCol = (cat == feedback::Category::Security)
                              ? Color{70, 40, 44, 245} : Color{40, 52, 64, 245};
            DrawRectangleRounded(r, 0.3f, 6, on ? onCol
                                               : (hov ? Color{32, 34, 44, 230} : Color{20, 22, 30, 210}));
            DrawRectangleRoundedLines(r, 0.3f, 6,
                on ? (cat == feedback::Category::Security ? Color{210, 110, 110, 230} : accent)
                   : Color{62, 66, 86, 170});
            DrawText(label, cx + 10, (int)r.y + 6, 12, on ? WHITE : Color{160, 166, 186, 255});
            if (hov && click) { m_feedbackCategory = cat; Audio::get().playSfx("click_light", 0.1f); }
            cx += cw + 8;
        }
        cy += 34;
    }

    if (m_feedbackCategory == feedback::Category::Security) {
        DrawText(T("Security reports go to a private advisory, never a public issue."),
                 x + 24, cy, 12, Color{220, 160, 160, 235});
        cy += 20;
    }

    // ── Title and description ──
    auto field = [&](const char* label, std::string& text, int index, int height,
                     const char* hint) {
        DrawText(label, x + 24, cy, 12, Color{150, 156, 176, 255});
        cy += 16;
        const Rectangle box = {(float)(x + 24), (float)cy, (float)(w - 48), (float)height};
        const bool active = (m_feedbackField == index);
        const bool hov = CheckCollisionPointRec(mouse, box);
        DrawRectangleRec(box, active ? Color{22, 25, 34, 255} : Color{15, 17, 23, 255});
        DrawRectangleLinesEx(box, 1, active ? accent : Color{60, 64, 84, 200});
        if (hov && click) m_feedbackField = index;

        if (text.empty() && !active) {
            DrawText(hint, (int)box.x + 8, (int)box.y + 7, 12, Color{96, 100, 118, 255});
        } else {
            // Wrapped by hand: the description is the one field where a player
            // writes more than fits on a line, and a box that scrolls sideways
            // hides what they already wrote.
            drawFieldText(box, text, 13, 8, 16, WHITE, active);
        }
        cy += height + 12;
    };

    field(T("Title"), m_feedbackTitle, 0, 28,
          bug ? T("What went wrong, in a few words") : T("Your idea, in a few words"));
    // Everything left between here and the buttons. The description is the only
    // field whose value grows with the room it is given, so it gets the room:
    // a fixed height left a hand's width of nothing above Send.
    // The rows below it, in order: the byline tickbox (26), the diagnostics
    // tickbox (26), the status line (20), and the gap above the buttons (12).
    // Every row added here has to be subtracted here too, or the field grows
    // into the buttons.
    const int bodyH = std::max(90, (y + h - 52) - cy - 16 - 26 - 26 - 20 - 12);
    field(T("What happened, and what did you expect?"), m_feedbackBody, 1, bodyH,
          T("The more precisely you can say what you did, the sooner it is fixed."));

    // ── Who this is from, and where it is going ──
    //
    // Said on the form, next to the tickbox that changes it, rather than in a
    // policy nobody opens. A person about to describe something they did wrong
    // in a game deserves to know it is about to appear on a public tracker with
    // their name on it BEFORE they write it, not after.
    {
        const std::string nick = AccountClient::get().account().nickname;
        const bool sec = (m_feedbackCategory == feedback::Category::Security);

        const Rectangle box = {(float)(x + 24), (float)cy, 18, 18};
        const bool hov = CheckCollisionPointRec(mouse, {box.x, box.y, 260, 20});
        DrawRectangleRec(box, m_feedbackAnonymous ? Color{40, 70, 50, 255} : Color{18, 20, 28, 255});
        DrawRectangleLinesEx(box, 1, m_feedbackAnonymous ? accent : Color{80, 84, 104, 200});
        if (m_feedbackAnonymous) DrawText("x", (int)box.x + 5, (int)box.y + 2, 14, WHITE);
        DrawText(T("Do not show my name"), (int)box.x + 26, (int)box.y + 3, 13,
                 hov ? WHITE : Color{180, 186, 206, 255});
        if (hov && click) {
            m_feedbackAnonymous = !m_feedbackAnonymous;
            Audio::get().playSfx("click_light", 0.1f);
        }

        // The consequence of that tick, spelled out either way.
        std::string where;
        if (sec) {
            where = T("Private. This one is never posted publicly.");
        } else if (m_feedbackAnonymous) {
            where = bug ? T("Posted publicly, as Anonymous.")
                        : T("Posted to the community channel, as Anonymous.");
        } else {
            where = (bug ? T("Posted publicly, signed ") : T("Posted to the community channel, signed "))
                  + (nick.empty() ? std::string(T("with your nickname")) : nick);
        }
        int fs = 12;
        const std::string fit = odText::fitToWidth(where, w - 48 - 300, fs, 10);
        DrawText(fit.c_str(), x + w - 24 - MeasureText(fit.c_str(), fs), (int)box.y + 4, fs,
                 sec ? Color{220, 160, 160, 235} : Color{150, 170, 200, 235});
        cy += 26;
    }

    // ── Diagnostics ──
    {
        const Rectangle box = {(float)(x + 24), (float)cy, 18, 18};
        const bool hov = CheckCollisionPointRec(mouse, {box.x, box.y, (float)(w - 48), 20});
        DrawRectangleRec(box, m_feedbackAttach ? Color{40, 70, 50, 255} : Color{18, 20, 28, 255});
        DrawRectangleLinesEx(box, 1, m_feedbackAttach ? accent : Color{80, 84, 104, 200});
        if (m_feedbackAttach) DrawText("x", (int)box.x + 5, (int)box.y + 2, 14, WHITE);
        DrawText(T("Attach what the game knows about itself"), (int)box.x + 26, (int)box.y + 3, 13,
                 hov ? WHITE : Color{180, 186, 206, 255});
        if (hov && click) { m_feedbackAttach = !m_feedbackAttach; Audio::get().playSfx("click_light", 0.1f); }

        const int lw = MeasureText(T("see exactly what"), 12);
        const Rectangle see = {(float)(x + w - 24 - lw - 8), (float)cy, (float)(lw + 8), 20};
        const bool sh = CheckCollisionPointRec(mouse, see);
        DrawText(T("see exactly what"), (int)see.x + 4, (int)see.y + 3, 12,
                 sh ? accent : Color{130, 150, 190, 235});
        if (sh && click) { m_feedbackPreview = true; m_feedbackPreviewScroll = 0; }
        cy += 26;
    }

    // ── Send, cancel, and whatever the service last said ──
    {
        const auto st = feedback::status();
        const std::string& msg = feedback::message();
        if (!msg.empty()) {
            DrawText(msg.c_str(), x + 24, cy, 12,
                     st == feedback::Status::Failed ? Color{230, 140, 140, 255}
                                                    : Color{150, 210, 165, 255});
        }
        cy += 20;

        const bool sending = (st == feedback::Status::Sending);
        const bool enough = m_feedbackBody.size() >= 8;
        const Rectangle send = {(float)(x + 24), (float)(y + h - 52), 170, 34};
        const Rectangle cancel = {(float)(x + 204), (float)(y + h - 52), 120, 34};
        const bool sh = CheckCollisionPointRec(mouse, send) && enough && !sending;
        const bool ch = CheckCollisionPointRec(mouse, cancel);

        DrawRectangleRounded(send, 0.2f, 6, !enough ? Color{24, 26, 34, 200}
                                                    : (sh ? Color{46, 92, 60, 250} : Color{34, 68, 46, 235}));
        DrawRectangleRoundedLines(send, 0.2f, 6, enough ? Color{110, 180, 130, 220} : Color{60, 64, 84, 180});
        const char* sendLabel = sending ? T("Sending...") : T("Send");
        DrawText(sendLabel, (int)send.x + 14, (int)send.y + 10, 14,
                 enough ? WHITE : Color{110, 114, 132, 255});
        if (!enough) {
            DrawText(T("Say a little more first"), (int)send.x + 180, (int)send.y + 11, 12,
                     Color{120, 124, 142, 255});
        }

        DrawRectangleRounded(cancel, 0.2f, 6, ch ? Color{44, 46, 60, 240} : Color{26, 28, 38, 220});
        DrawRectangleRoundedLines(cancel, 0.2f, 6, Color{80, 84, 104, 200});
        DrawText(T("Close"), (int)cancel.x + 14, (int)cancel.y + 10, 14, Color{200, 205, 225, 255});

        if (sh && click) submitFeedbackForm();
        if (ch && click) closeFeedbackForm();
        // Sent, and the player has read the thank-you: close for them.
        if (st == feedback::Status::Sent && GetTime() - m_feedbackSentAt > 2.0 &&
            m_feedbackSentAt > 0) {
            closeFeedbackForm();
            feedback::clearMessage();
            m_feedbackSentAt = 0;
        }
        if (st == feedback::Status::Sent && m_feedbackSentAt == 0) m_feedbackSentAt = GetTime();
    }
}

// ── Typing into the form ──
//
// This does NOT use odTextEditKeys, which the rest of the game's fields use.
// That helper is built for a hostname or a filename: ASCII only, no newlines,
// and it deletes one BYTE at a time. All three are wrong here. A bug report is
// prose, often pasted out of a log, often not in English -- and backspacing a
// byte off a Cyrillic or Japanese character leaves a broken sequence on screen
// rather than deleting the letter.

namespace {

/// Append a codepoint as UTF-8. raylib's CodepointToUTF8 is stubbed out in the
/// headless server build, so this does not go through it.
void appendUtf8(std::string& out, unsigned int cp) {
    if (cp < 0x80) { out += (char)cp; }
    else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

/// Delete one CHARACTER: the trailing byte plus any continuation bytes under
/// it, so one press removes one thing the player can see.
void popUtf8(std::string& text) {
    if (text.empty()) return;
    size_t i = text.size() - 1;
    while (i > 0 && (unsigned char)text[i] >= 0x80 && (unsigned char)text[i] < 0xC0) --i;
    text.erase(i);
}

}  // namespace

void Game::updateFeedbackForm() {
    if (!m_feedbackOpen) return;
    feedback::pump();
    m_feedbackSwallowClick = false;   // a whole frame has passed

    if (IsKeyPressed(KEY_ESCAPE)) {
        if (m_feedbackPreview) m_feedbackPreview = false;
        else closeFeedbackForm();
        return;
    }
    if (m_feedbackPreview) return;

    if (IsKeyPressed(KEY_TAB)) m_feedbackField = (m_feedbackField + 1) % 2;

    std::string& text = (m_feedbackField == 0) ? m_feedbackTitle : m_feedbackBody;
    // The service's own caps, so a report is never clipped after it is written.
    const size_t cap = (m_feedbackField == 0) ? 140u : 4000u;

    int key = GetCharPressed();
    while (key > 0) {
        if (key >= 32 && text.size() + 4 <= cap) appendUtf8(text, (unsigned)key);
        key = GetCharPressed();
    }

    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE) ||
        IsKeyPressed(KEY_DELETE)    || IsKeyPressedRepeat(KEY_DELETE)) {
        if (!text.empty()) { popUtf8(text); Audio::get().playSfx("key_type", 0.12f); }
    }

    // Typed above by hand because these fields accept more than ASCII; the
    // paste comes through the shared path so it behaves the same everywhere.
    const std::string pasted = odTakePaste();
    if (!pasted.empty() && odTextAppendPaste(text, pasted, cap))
        Audio::get().playSfx("key_type", 0.12f);

    // Enter breaks a line in the description and does nothing in the title,
    // which is one line by definition.
    if (m_feedbackField == 1 && IsKeyPressed(KEY_ENTER) && text.size() < cap) text += '\n';

    // Paste keeps newlines and every byte of what was copied. Somebody pasting
    // a stack trace or a script error is handing over the most useful thing in
    // the report, and the field should not be the thing that mangles it.
    const bool paste = (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                        IsKeyDown(KEY_LEFT_SUPER)   || IsKeyDown(KEY_RIGHT_SUPER)) &&
                       IsKeyPressed(KEY_V);
    if (paste) {
        if (const char* clip = GetClipboardText()) {
            for (const char* q = clip; *q && text.size() < cap; ++q) {
                const unsigned char c = (unsigned char)*q;
                if (c == '\r') continue;
                if (c == '\n' && m_feedbackField == 0) break;   // a title is one line
                if (c < 32 && c != '\n' && c != '\t') continue; // the Worker strips these anyway
                text += *q;
            }
            Audio::get().playSfx("key_type", 0.12f);
        }
    }
}

// ────────────────────────────────────────────────────── the rating prompt ────
//
// Shown once, in the corner, after enough play that the answer means something,
// and never again whether or not it is answered. A prompt that returns is a
// prompt that gets a worse answer each time.

void Game::drawRatingPrompt() {
    if (!m_ratingPromptOpen || promptsAreHidden()) return;

    const Vector2 mouse = getMouse();
    const Color accent = hexToColor(m_config.accent());
    const Rectangle box = ratingPromptRect();

    DrawRectangleRounded(box, 0.08f, 8, Color{18, 20, 27, 245});
    DrawRectangleRoundedLines(box, 0.08f, 8, Color{70, 74, 96, 220});
    const int x = (int)box.x, y = (int)box.y;

    DrawText(T("Enjoying OpenDoctrines?"), x + 16, y + 14, 15, accent);
    DrawText(T("A rating helps other people find it."), x + 16, y + 34, 12,
             Color{160, 166, 186, 255});

    const Rectangle rate = ratingRateRect();
    const Rectangle wrong = ratingWrongRect();
    const bool rh = CheckCollisionPointRec(mouse, rate);
    const bool wh = CheckCollisionPointRec(mouse, wrong);

    DrawRectangleRounded(rate, 0.2f, 6, rh ? Color{46, 92, 60, 250} : Color{34, 68, 46, 235});
    DrawRectangleRoundedLines(rate, 0.2f, 6, Color{110, 180, 130, 220});
    DrawText(T("Rate on itch.io"), (int)rate.x + 12, (int)rate.y + 9, 13, WHITE);

    DrawRectangleRounded(wrong, 0.2f, 6, wh ? Color{60, 46, 46, 240} : Color{34, 28, 30, 220});
    DrawRectangleRoundedLines(wrong, 0.2f, 6, Color{140, 100, 100, 200});
    DrawText(T("Something's wrong"), (int)wrong.x + 12, (int)wrong.y + 9, 13,
             Color{225, 190, 190, 255});

    const bool nh = CheckCollisionPointRec(mouse, ratingDismissRect());
    DrawText(T("Not now"), (int)box.x + (int)box.width - 76, (int)box.y + 96, 12,
             nh ? WHITE : Color{130, 134, 152, 255});
}

Rectangle Game::ratingPromptRect() const {
    return {(float)(m_screenW - 360 - 24), (float)(m_screenH - 128 - 24), 360, 128};
}
Rectangle Game::ratingRateRect() const {
    const Rectangle b = ratingPromptRect();
    return {b.x + 16, b.y + 58, 150, 32};
}
Rectangle Game::ratingWrongRect() const {
    const Rectangle b = ratingPromptRect();
    return {b.x + 176, b.y + 58, 168, 32};
}
Rectangle Game::ratingDismissRect() const {
    const Rectangle b = ratingPromptRect();
    return {b.x + b.width - 84, b.y + 92, 72, 22};
}

/**
 * The prompt's input, run from update() so a click on it is not ALSO a click on
 * the map underneath.
 *
 * Why itch.io rather than five stars in the corner: a number only the
 * maintainer sees does nothing for anybody. A rating on the page is public, and
 * it is what somebody deciding whether to try the game actually reads. It also
 * removed the one path into the report service that needed no account.
 *
 * "Something's wrong" is the half worth keeping from the old star widget. A
 * player who is unhappy is about to say so somewhere; a bug report is a better
 * destination for that than a one-star review, for them and for the game.
 *
 * @return true when the pointer is over the prompt.
 */
bool Game::updateRatingPrompt() {
    if (!m_ratingPromptOpen || promptsAreHidden()) return false;
    const Vector2 mouse = getMouse();
    if (!CheckCollisionPointRec(mouse, ratingPromptRect())) return false;
    if (!IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) return true;

    auto answered = [&] {
        m_config.ratingAsked = true;
        m_config.save(m_configPath);
        m_ratingPromptOpen = false;
    };

    if (CheckCollisionPointRec(mouse, ratingRateRect())) {
        m_config.ratingGiven = true;
        answered();
        odlink::open(feedback::ratingUrl());
        Audio::get().playSfx("confirm");
    } else if (CheckCollisionPointRec(mouse, ratingWrongRect())) {
        answered();
        openFeedbackForm(feedback::Kind::Bug, feedback::Category::Other);
    } else if (CheckCollisionPointRec(mouse, ratingDismissRect())) {
        // "Not now" means never. Asking again is how a prompt becomes nagging,
        // and the second answer is always worse than the first.
        answered();
        Audio::get().playSfx("back");
    }
    return true;
}

// Two doors into the prompt, and the first is the one that carries it.
//
// A single 45-minute clock asked at a moment chosen by arithmetic: whatever the
// player happened to be doing when the timer expired, which for most of them was
// nothing in particular and for some was losing. It also asked almost nobody --
// most people meet this game in a browser tab, and 45 minutes of cumulative play
// is a bar the large majority never reach, so the question was put mainly to the
// few who were always going to answer it anyway.
//
// So: a low floor plus a good moment. MOMENT_MINUTES is long enough to have an
// opinion and short enough that a browser session can reach it, and the moment
// itself is the guard against asking too early -- the player has just come out
// of a war with ground to show for it, which is when they think well of the game
// and when what they say is worth reading.
//
// FALLBACK_MINUTES keeps the old behaviour for the player whose game never
// produces such a moment: a builder, a peaceful run, somebody losing slowly.
// They are asked eventually, on the clock, exactly as before.
/**
 * ── THE USAGE-REPORTING QUESTION ──
 *
 * Asked once, early, and answered either way for good.
 *
 * WHY IT EXISTS. The setting has been shipping since 1.2.0a, defaults to off,
 * and lives in a menu. In ninety days it produced one report, on a day before
 * the release, which was almost certainly a developer testing it. A consent
 * toggle nobody is ever shown is not a privacy feature, it is a feature that
 * does not work: the people who would have said yes were never asked.
 *
 * WHY EARLY, when the rating prompt waits. Nothing is sent until this is
 * answered, so every minute it waits is a session that went uncounted and
 * cannot be recovered. The rating prompt is waiting for an opinion to form;
 * this one only has to wait long enough to be sure somebody is playing rather
 * than looking. Three minutes is that.
 *
 * WHY THE TWO NEVER OVERLAP. Each holds off while the other is open, and this
 * one comes first by a wide margin. Two boxes in the same corner is how both
 * get clicked away unread, and the rating prompt is the more valuable of the
 * two -- it must not be spent covering for this one.
 *
 * WHAT MAKES IT CONSENT rather than a dark pattern: the two answers are the
 * same size, in the same place, with no default and no pre-selection; the text
 * says what is collected before the buttons are reachable; and declining is
 * permanent rather than a snooze. Nothing has been sent at the moment the
 * question is asked.
 */
constexpr int USAGE_ASK_MINUTES = 3;

/**
 * The play clock both prompts wait on.
 *
 * It used to be counted inside maybeOfferRating(), AFTER that function's early
 * returns -- so the minute the rating was answered, the clock stopped. Nothing
 * depended on it afterwards, so nothing was visibly wrong, and it stayed that
 * way until a second prompt started reading the same number and would have sat
 * waiting forever for a value that no longer moved.
 *
 * Counted unconditionally here instead, which also makes `minutesPlayed` mean
 * what its name says rather than "minutes played before the rating was
 * answered". Saved as it rolls over so the count survives a crash, not only a
 * clean quit.
 */
void Game::tickPlayClock(float dt) {
    if (m_feedbackOpen || m_paused || m_currentScreen != SCREEN_PLAYING) return;
    m_playedSeconds += dt;
    if (m_playedSeconds >= 60.0f) {
        m_config.minutesPlayed += (int)(m_playedSeconds / 60.0f);
        m_playedSeconds = std::fmod(m_playedSeconds, 60.0f);
        m_config.save(m_configPath);
    }
}

void Game::maybeOfferUsage(float dt) {
    (void)dt;   // tickPlayClock() keeps the clock; this only reads it
    if (m_config.usageAsked || m_usagePromptOpen) return;
    // Never on top of the other one, and never over a form or a paused game.
    if (m_ratingPromptOpen || m_feedbackOpen || m_paused) return;
    if (m_currentScreen != SCREEN_PLAYING) return;
    if (m_turnState != TURN_NORMAL) return;

    // No endpoint, no question. A build with no account service configured has
    // nowhere to send a report, so asking would be collecting an answer to a
    // question that cannot be acted on.
    if (m_config.accountIssuer.empty()) return;

    if (m_config.minutesPlayed >= USAGE_ASK_MINUTES) m_usagePromptOpen = true;
}

Rectangle Game::usagePromptRect() const {
    return {(float)(m_screenW - 360 - 24), (float)(m_screenH - 150 - 24), 360, 150};
}
Rectangle Game::usageYesRect() const {
    const Rectangle b = usagePromptRect();
    return {b.x + 16, b.y + 104, 160, 32};
}
Rectangle Game::usageNoRect() const {
    const Rectangle b = usagePromptRect();
    // The same width as yes, beside it. A "no" that is smaller or quieter than
    // the "yes" is not a freely given choice, whatever the text says.
    return {b.x + 184, b.y + 104, 160, 32};
}


/**
 * Open is not the same as showing.
 *
 * maybeOffer*() refuses to OPEN a prompt over a paused game or a form, which
 * is not the same as refusing to DRAW one that is already open: a player who
 * is asked and then presses Escape gets the box laid over "Continue / Settings
 * / Save", and a click meant for the menu can land on an answer. Found in a
 * screenshot-tour frame, where the pause menu and the usage question are both
 * on screen at once.
 *
 * Hidden rather than answered. The question has not been put to anybody while
 * it is behind a menu, so it must not count as asked; it comes back on the
 * next quiet frame with the answer still owed.
 */
bool Game::promptsAreHidden() const {
    return m_paused || m_feedbackOpen || m_currentScreen != SCREEN_PLAYING;
}

void Game::drawUsagePrompt() {
    if (!m_usagePromptOpen || promptsAreHidden()) return;

    const Vector2 mouse = getMouse();
    const Color accent = hexToColor(m_config.accent());
    const Rectangle box = usagePromptRect();

    DrawRectangleRounded(box, 0.08f, 8, Color{18, 20, 27, 245});
    DrawRectangleRoundedLines(box, 0.08f, 8, Color{70, 74, 96, 220});
    const int x = (int)box.x, y = (int)box.y;

    DrawText(T("Share anonymous usage?"), x + 16, y + 14, 15, accent);
    // Three short lines rather than a paragraph: this is the part that has to
    // be read for the answer to mean anything.
    DrawText(T("That a session happened, and roughly how long."), x + 16, y + 38, 12,
             Color{160, 166, 186, 255});
    DrawText(T("No account, no identifier, nothing linkable to you."), x + 16, y + 56, 12,
             Color{160, 166, 186, 255});
    DrawText(T("Change it any time in Settings."), x + 16, y + 74, 12,
             Color{130, 134, 152, 255});

    const Rectangle yes = usageYesRect();
    const Rectangle no  = usageNoRect();
    const bool yh = CheckCollisionPointRec(mouse, yes);
    const bool nh = CheckCollisionPointRec(mouse, no);

    DrawRectangleRounded(yes, 0.2f, 6, yh ? Color{46, 92, 60, 250} : Color{34, 68, 46, 235});
    DrawRectangleRoundedLines(yes, 0.2f, 6, Color{110, 180, 130, 220});
    DrawText(T("Share"), (int)yes.x + 12, (int)yes.y + 9, 13, WHITE);

    DrawRectangleRounded(no, 0.2f, 6, nh ? Color{52, 56, 72, 250} : Color{34, 36, 48, 235});
    DrawRectangleRoundedLines(no, 0.2f, 6, Color{110, 114, 140, 220});
    DrawText(T("No thanks"), (int)no.x + 12, (int)no.y + 9, 13, Color{225, 228, 240, 255});
}

/** @return true when the pointer is over the prompt, so the map does not also take the click. */
bool Game::updateUsagePrompt() {
    if (!m_usagePromptOpen || promptsAreHidden()) return false;
    const Vector2 mouse = getMouse();
    if (!CheckCollisionPointRec(mouse, usagePromptRect())) return false;
    if (!IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) return true;

    const bool yes = CheckCollisionPointRec(mouse, usageYesRect());
    const bool no  = CheckCollisionPointRec(mouse, usageNoRect());
    if (!yes && !no) return true;   // a click inside the box but on neither answer

    // Asked is set either way. Declining has to be remembered as an answer, or
    // the question returns next session and the "no" meant nothing.
    m_config.usageAsked   = true;
    m_config.usageReports = yes;
    m_config.save(m_configPath);
    m_usagePromptOpen = false;
    Audio::get().playSfx(yes ? "confirm" : "back");
    return true;
}

constexpr int RATING_MOMENT_MINUTES   = 10;
constexpr int RATING_FALLBACK_MINUTES = 45;

/**
 * The same fallback, for a player in a browser tab.
 *
 * ── WHY IT IS NOT 45 ──
 *
 * 45 is a number for somebody who installed this. In the month to 15 Sep 2026
 * the itch page took 3,964 browser plays and 629 downloads: six of every seven
 * people who play this game play it in a tab, and a tab is not where anybody
 * spends three quarters of an hour on their first visit. The fallback existed
 * for the player whose game never produces a good moment -- a builder, a
 * peaceful run, somebody losing slowly -- and for six sevenths of the audience
 * it silently never fired at all.
 *
 * It shows in the count: three ratings from 3,964 plays. Not three people who
 * disliked it -- three people who were asked.
 *
 * 15 is a real session rather than a lower bar for its own sake: long enough
 * to have resolved turns, fought something and formed a view, short enough to
 * be reachable before the tab closes.
 */
constexpr int RATING_FALLBACK_WEB_MINUTES = 15;

void Game::maybeOfferRating(float dt) {
    (void)dt;   // the clock is tickPlayClock()'s job now
    if (m_config.ratingAsked || m_config.ratingGiven || m_ratingPromptOpen) return;
    // Never two boxes in the same corner. The usage question is asked far
    // earlier, so in practice this only holds for the frames it is open.
    if (m_usagePromptOpen || m_persistWarnOpen) return;
    if (m_feedbackOpen || m_paused || m_currentScreen != SCREEN_PLAYING) return;

    // Between turns rather than during one: nobody wants to be asked how they
    // feel while their army is moving. Both doors are behind this.
    if (m_turnState != TURN_NORMAL) return;

    if (m_ratingMoment && m_config.minutesPlayed >= RATING_MOMENT_MINUTES) {
        m_ratingMoment = false;
        m_ratingPromptOpen = true;
        return;
    }
#ifdef __EMSCRIPTEN__
    const int fallback = RATING_FALLBACK_WEB_MINUTES;
#else
    const int fallback = RATING_FALLBACK_MINUTES;
#endif
    if (m_config.minutesPlayed >= fallback) {
        m_ratingPromptOpen = true;
    }
}

// ─────────────────────────────────────── "this tab will lose your game" ────
//
// ── THE NUMBER THAT PUT THIS HERE ──
//
// In the 30 days to 15 Sep 2026 the itch.io page recorded 3,964 browser plays,
// 629 downloads, and -- on the site those players are sent to -- 358 users of
// whom 0% came back the following week. Nothing accumulated. A player who
// spends an evening on a campaign and finds it gone is not a player who
// bounced; they are a player who was robbed by something the game knew about
// and did not mention.
//
// ── IT IS SHOWN TO ALMOST NOBODY, ON PURPOSE ──
//
// The browser build keeps everything in IndexedDB and most of the time that
// works. It fails in specific, invisible ways: a private tab, a browser
// blocking site data, an unanswered quota prompt, or a third-party iframe
// whose storage is partitioned away -- and itch.io serves this game in exactly
// such an iframe. In all of those the game plays perfectly, reports every save
// as written, and loses the lot when the tab closes.
//
// odPersistWorking() is the difference between warning the people it is true
// for and putting a scary box in front of everybody. A warning shown to the
// majority for whom it is false is a warning that teaches people to ignore
// warnings.
//
// ── AND IT OFFERS THE TWO REAL ANSWERS ──
//
// A backup file they can load back, and the installed build where this cannot
// happen. Not "OK". A warning with one button that dismisses it has told
// somebody their evening is at risk and left them holding it.

namespace {

/**
 * Minutes before there is something worth losing.
 *
 * Not at startup, when the honest answer is "nothing has happened yet" and the
 * warning is noise. Not at 30 minutes either -- by then it is an obituary.
 * Six is a few turns in: enough that the player would mind, early enough that
 * acting on it costs them nothing.
 */
constexpr int PERSIST_WARN_MINUTES = 6;

}  // namespace

void Game::maybeWarnAboutThisTab(float dt) {
    (void)dt;   // tickPlayClock() keeps the clock
    if (m_persistWarnDone || m_persistWarnOpen) return;
    if (odPersistWorking()) return;          // always true off the web
    if (m_feedbackOpen || m_paused || m_currentScreen != SCREEN_PLAYING) return;
    if (m_usagePromptOpen || m_ratingPromptOpen) return;
    if (m_turnState != TURN_NORMAL) return;
    if (m_config.minutesPlayed < PERSIST_WARN_MINUTES) return;

    m_persistWarnOpen = true;
}

Rectangle Game::persistWarnRect() const {
    return {(float)(m_screenW - 380 - 24), (float)(m_screenH - 162 - 24), 380, 162};
}
Rectangle Game::persistWarnGetRect() const {
    const Rectangle b = persistWarnRect();
    return {b.x + 16, b.y + 112, 176, 34};
}
Rectangle Game::persistWarnBackupRect() const {
    const Rectangle b = persistWarnRect();
    return {b.x + 200, b.y + 112, 164, 34};
}
Rectangle Game::persistWarnDismissRect() const {
    const Rectangle b = persistWarnRect();
    return {b.x + b.width - 92, b.y + 10, 80, 24};
}

void Game::drawPersistWarning() {
    if (!m_persistWarnOpen || promptsAreHidden()) return;

    const Vector2 mouse = getMouse();
    const Rectangle box = persistWarnRect();

    // Amber rather than the usual panel grey, and the only prompt in the game
    // that is: this one is not a question, it is a thing going wrong.
    DrawRectangleRounded(box, 0.08f, 8, Color{30, 24, 16, 248});
    DrawRectangleRoundedLines(box, 0.08f, 8, Color{190, 140, 60, 230});
    const int x = (int)box.x, y = (int)box.y;

    DrawText(T("This tab cannot save your game."), x + 16, y + 14, 15,
             Color{240, 190, 110, 255});
    wrapText(T("Your browser is refusing storage to this page, so everything here disappears when the tab closes."),
             x + 16, y + 38, (int)box.width - 32, 12, Color{180, 172, 158, 255}, true);
    wrapText(T("The game still plays. Nothing you do will be here tomorrow."),
             x + 16, y + 78, (int)box.width - 32, 12, Color{200, 165, 120, 255}, true);

    const Rectangle get = persistWarnGetRect();
    const Rectangle backup = persistWarnBackupRect();
    const bool gh = CheckCollisionPointRec(mouse, get);
    const bool bh = CheckCollisionPointRec(mouse, backup);

    DrawRectangleRounded(get, 0.2f, 6, gh ? Color{46, 92, 60, 250} : Color{34, 68, 46, 235});
    DrawRectangleRoundedLines(get, 0.2f, 6, Color{110, 180, 130, 220});
    DrawText(T("Get the free download"), (int)get.x + 12, (int)get.y + 10, 13, WHITE);

    DrawRectangleRounded(backup, 0.2f, 6, bh ? Color{52, 56, 72, 250} : Color{36, 40, 52, 235});
    DrawRectangleRoundedLines(backup, 0.2f, 6, Color{120, 130, 160, 220});
    DrawText(T("Save a backup file"), (int)backup.x + 12, (int)backup.y + 10, 13, WHITE);

    const bool nh = CheckCollisionPointRec(mouse, persistWarnDismissRect());
    DrawText(T("Dismiss"), (int)box.x + (int)box.width - 76, (int)box.y + 14, 12,
             nh ? WHITE : Color{150, 140, 125, 255});
}

/**
 * Write the whole session out as a .odstate the browser downloads.
 *
 * The same archive and the same writer the main menu's "Save .odstate" uses --
 * not a second, smaller emergency format, because a player who takes the
 * backup has to be able to load it back through the door that already exists.
 * No name is asked for: somebody being told their evening is at risk should
 * not then be made to think of a filename.
 */
void Game::backUpStateNow() {
#ifdef __EMSCRIPTEN__
    const std::string name = OdState::suggestedFilename();
    const std::string tmp = "/odstate_out.odstate";
    std::string err;
    int n = 0;
    if (OdState::save(m_dataDir, tmp, err, &n)) {
        OdState::webDownload(tmp, name);
        std::remove(tmp.c_str());
        setOdStateMsg("Downloaded " + name + "  (" + std::to_string(n) + " files)", false);
    } else {
        setOdStateMsg(err, true);
    }
#endif
}

bool Game::updatePersistWarning() {
    if (!m_persistWarnOpen || promptsAreHidden()) return false;
    const Vector2 mouse = getMouse();
    if (!CheckCollisionPointRec(mouse, persistWarnRect())) return false;
    if (!IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) return true;

    if (CheckCollisionPointRec(mouse, persistWarnGetRect())) {
        // Answered for this session either way: somebody who has been handed
        // the download has been told, and telling them again in ten minutes is
        // how a true warning starts reading as a nag for a download.
        m_persistWarnOpen = false;
        m_persistWarnDone = true;
        Audio::get().playSfx("confirm");
        odlink::open(feedback::ratingUrl());
    } else if (CheckCollisionPointRec(mouse, persistWarnBackupRect())) {
        m_persistWarnOpen = false;
        m_persistWarnDone = true;
        Audio::get().playSfx("confirm");
        backUpStateNow();
    } else if (CheckCollisionPointRec(mouse, persistWarnDismissRect())) {
        m_persistWarnOpen = false;
        m_persistWarnDone = true;
        Audio::get().playSfx("back");
    }
    return true;
}
