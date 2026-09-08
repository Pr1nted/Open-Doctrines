// Mail: the box, the delivery, and the screen a player reads it on.
//
// The rules live in src/Mail.h and are tested without a world. This file is
// what connects them to the game: whose box is whose, when letters move, and
// what it looks like.

#include "Game.h"
#include "GameInternals.h"
#include "Audio.h"
#include "TextInput.h"
#include "i18n/Locale.h"
#include "i18n/Text.h"
#include "llm/Advisor.h"
#include "llm/Runner.h"

#include <algorithm>

// ────────────────────────────────────────────────────────────── the box ────

const mail::Box* Game::mailboxIfAny(int countryId) const {
    auto it = m_mail.find(countryId);
    return it == m_mail.end() ? nullptr : &it->second;
}

mail::Rules Game::mailRules() const {
    mail::Rules r;
    r.policy = (mail::Policy)m_config.mailPolicy;
    // Set when the language-model module is loaded AND answering. Until that
    // module exists this is simply false, which makes every bot path dead
    // rather than broken -- the button does not appear, and nothing pretends
    // there is somebody to write to.
    r.llmAvailable = m_llmAvailable;
    r.multiplayer = (m_netHost != nullptr || m_netSession != nullptr);
    return r;
}

bool Game::mailIsBot(int countryId) const {
    if (!m_llmAvailable) return false;
    // Every country that is not held by a person is a candidate correspondent
    // once the module is loaded. The player's own country is never one.
    if (countryId == m_playerCountryId) return false;
    return m_llmCountries.empty() || m_llmCountries.count(countryId) > 0;
}

mail::Lock Game::mailLockOf(int countryId) const {
    if (countryId == m_playerCountryId) {
        // A player who answered the age prompt as under age is shut, and it is
        // not a preference they can turn off from the mail screen. Above their
        // own setting for that reason; below nothing, because it is the most
        // restrictive answer either way.
        if (m_config.agePromptOn && m_config.ageAnswer == 2) return mail::Lock::Nobody;
        return (mail::Lock)m_config.mailLock;
    }
    auto it = m_mailLocks.find(countryId);
    return it == m_mailLocks.end() ? mail::Lock::Open : it->second;
}

/**
 * Move every pending letter, everywhere, once.
 *
 * Called from processTurn. Two passes on purpose: mark and copy are separated
 * so that a letter written this turn cannot be answered in the same turn's
 * delivery -- if the recipient's box were filled while it was still being
 * walked, a bot replying on receipt would land its answer in the same tick and
 * the whole "a letter takes a turn" property would quietly stop holding.
 */
int Game::deliverMail() {
    // Pass one: everything pending becomes history, in its own box.
    std::vector<mail::Message> moving;
    for (auto& [cid, box] : m_mail) {
        (void)cid;
        box.deliver(m_turnNumber);
        for (const mail::Message* m : box.arrivedOn(m_turnNumber)) moving.push_back(*m);
    }

    // Pass two: each lands in the box of whoever it was addressed to.
    int arrivedForPlayer = 0;
    for (const mail::Message& m : moving) {
        if (m.fromCountry == m.toCountry) continue;
        mailbox(m.toCountry).receive(m);
        if (m.toCountry == m_playerCountryId) ++arrivedForPlayer;
    }
    m_mailArrived = arrivedForPlayer;
    if (arrivedForPlayer > 0) Audio::get().playSfx("notify");
    return (int)moving.size();
}

// ───────────────────────────────────────────────────────────── the form ────

void Game::openLlmSetup() {
    // THE ONE ENTRY THAT DOES NOT CHECK mailAvailable(). Everything else about
    // Mail is gated on there being somebody to write to; this is the screen
    // that CREATES somebody to write to, so gating it the same way made the
    // installer reachable only once the install had already happened.
    //
    // Opens straight into settings and, on the way out, decides where to go by
    // asking whether the setup worked: a player who has just installed a
    // runner and pulled a model lands in their new mailbox, and one who
    // changed their mind is put back where they were.
    m_mailOpen = true;
    m_mailSettingsOpen = true;
    m_mailSetupOnly = true;
    m_mailSettingsScroll = 0;
    m_mailLlmField = -1;
    m_mailThread = 0;
    m_mailPicking = false;
    m_mailDraft.clear();
    m_mailEditing = 0;
    m_mailScroll = m_mailListScroll = m_mailPickerScroll = 0;
    m_mailComposeFocus = false;
    Audio::get().playSfx("panel_open");
}

/// Leaving the settings pane: back to the mailbox if there is one, else out.
void Game::closeMailSettings() {
    m_mailSettingsOpen = false;
    m_mailLlmField = -1;
    if (m_mailSetupOnly && !mailAvailable()) closeMail();
    else m_mailSetupOnly = false;
}

void Game::openMail() {
    if (!mailAvailable()) {
        m_mailNotice = T("Mail is not available in this game.");
        m_mailNoticeUntil = GetTime() + 5.0;
        Audio::get().playSfx("deny");
        return;
    }
    m_mailOpen = true;
    m_mailThread = 0;
    m_mailPicking = false;
    m_mailDraft.clear();
    m_mailEditing = 0;
    m_mailScroll = m_mailListScroll = m_mailPickerScroll = 0;
    m_mailComposeFocus = false;
    Audio::get().playSfx("panel_open");
}

void Game::closeMail() {
    m_mailOpen = false;
    m_mailComposeFocus = false;
    Audio::get().playSfx("panel_close");
}

/**
 * Put the draft on the desk, or say why not.
 *
 * Rewriting a pending letter is an edit rather than a second letter, which is
 * the behaviour a person expects from something that has not been sent yet.
 */
bool Game::mailSendDraft() {
    if (m_mailThread == 0) return false;
    const mail::Refusal why = mail::check(
        m_mailDraft, mailRules(), mailIsBot(m_mailThread), mailLockOf(m_mailThread),
        m_config.mailBlacklist);
    if (why != mail::Refusal::None) {
        m_mailNotice = T(mail::refusalText(why));
        m_mailNoticeUntil = GetTime() + 6.0;
        Audio::get().playSfx("deny");
        return false;
    }

    mail::Box& box = mailbox(m_playerCountryId);
    if (m_mailEditing != 0) {
        box.edit(m_mailEditing, m_mailDraft);
        m_mailEditing = 0;
    } else {
        std::string me;
        if (const Country* c = m_countries.getCountry(m_playerCountryId)) me = c->name;
        box.write(m_playerCountryId, m_mailThread, m_mailDraft, m_turnNumber,
                  mail::Author::Human, me);
    }
    m_mailDraft.clear();
    Audio::get().playSfx("confirm");
    return true;
}

// ───────────────────────────────────────────────────────────── the screen ────
//
// Three views in one panel: the list of correspondents, one correspondence, and
// the picker for starting a new one. Laid out like a messenger because that is
// the thing everybody already knows how to use -- the only unfamiliar part is
// that letters sit in an outbox until the turn resolves, so that is the part
// the screen says out loud.

namespace {

/// The colour a letter is drawn in, by who wrote it and whether it has gone.
struct Ink {
    Color bubble, text, edge;
};

Ink inkFor(const mail::Message& m, bool mine, Color accent) {
    if (mine && m.status == mail::Status::Pending) {
        // Visibly unfinished: a pending letter is still yours, and it should
        // not look like something that has been said.
        return Ink{Color{34, 38, 52, 235}, Color{200, 206, 226, 255}, accent};
    }
    if (mine) return Ink{Color{30, 52, 44, 240}, Color{215, 235, 222, 255}, Color{70, 110, 90, 200}};
    if (m.author == mail::Author::Bot)
        return Ink{Color{44, 36, 56, 240}, Color{224, 212, 240, 255}, Color{120, 96, 150, 210}};
    return Ink{Color{30, 34, 46, 240}, Color{214, 220, 238, 255}, Color{70, 76, 100, 200}};
}

}  // namespace

void Game::drawMailNotice() {
    if (m_mailNotice.empty()) return;
    if (GetTime() > m_mailNoticeUntil) { m_mailNotice.clear(); return; }
    const int tw = MeasureText(m_mailNotice.c_str(), 14);
    const int bw = std::min(tw + 32, m_screenW - 40);
    const int bx = (m_screenW - bw) / 2, by = m_screenH - 150;
    DrawRectangleRounded({(float)bx, (float)by, (float)bw, 38}, 0.3f, 8, Color{40, 30, 26, 245});
    DrawRectangleRoundedLines({(float)bx, (float)by, (float)bw, 38}, 0.3f, 8,
                              Color{170, 130, 100, 230});
    int fs = 14;
    const std::string fit = odText::fitToWidth(m_mailNotice, bw - 24, fs, 10);
    DrawText(fit.c_str(), bx + 16, by + 19 - fs / 2, fs, Color{235, 210, 190, 255});
}

void Game::drawMail() {
    if (!m_mailOpen) return;

    const Vector2 mouse = getMouse();
    const bool click = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    const Color accent = hexToColor(m_config.accent());

    DrawRectangle(0, 0, m_screenW, m_screenH, Color{6, 7, 11, 232});
    const int w = std::min(920, m_screenW - 80);
    const int h = std::min(640, m_screenH - 80);
    const int x = (m_screenW - w) / 2, y = (m_screenH - h) / 2;
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.02f, 8, Color{16, 18, 24, 250});
    DrawRectangleRoundedLines({(float)x, (float)y, (float)w, (float)h}, 0.02f, 8,
                              Color{70, 74, 96, 220});

    DrawText(T("Mail"), x + 24, y + 20, 24, accent);

    // Close, top right, always in the same place whichever view is open.
    const Rectangle close = {(float)(x + w - 104), (float)(y + 18), 80, 28};
    const bool ch = CheckCollisionPointRec(mouse, close);
    DrawRectangleRounded(close, 0.2f, 6, ch ? Color{44, 46, 60, 240} : Color{26, 28, 38, 220});
    DrawRectangleRoundedLines(close, 0.2f, 6, Color{80, 84, 104, 200});
    DrawText(T("Close"), (int)close.x + 14, (int)close.y + 7, 13, Color{200, 205, 225, 255});
    if (ch && click) { closeMail(); return; }

    // Settings, and -- for a host -- the complaints their players have sent.
    {
        const Rectangle gear = {(float)(x + w - 104 - 96), (float)(y + 18), 88, 28};
        const bool gh = CheckCollisionPointRec(mouse, gear);
        DrawRectangleRounded(gear, 0.2f, 6, gh ? Color{44, 46, 60, 240} : Color{26, 28, 38, 220});
        DrawRectangleRoundedLines(gear, 0.2f, 6, Color{80, 84, 104, 200});
        DrawText(T("Settings"), (int)gear.x + 10, (int)gear.y + 7, 12,
                 m_mailSettingsOpen ? accent : Color{200, 205, 225, 255});
        if (gh && click) {
            if (m_mailSettingsOpen) closeMailSettings();
            else { m_mailSettingsOpen = true; m_mailSettingsScroll = 0; }
            Audio::get().playSfx("click_light", 0.1f);
        }
        if (m_netHost) {
            const int n = (int)m_hostReports.size();
            const char* label = n ? TextFormat(T("Reports (%d)"), n) : T("Reports");
            const int bw = MeasureText(label, 12) + 20;
            const Rectangle rb = {(float)(gear.x - bw - 8), (float)(y + 18), (float)bw, 28};
            const bool rh = CheckCollisionPointRec(mouse, rb);
            DrawRectangleRounded(rb, 0.2f, 6, rh ? Color{60, 44, 44, 240} : Color{30, 24, 26, 220});
            DrawRectangleRoundedLines(rb, 0.2f, 6,
                                      m_hostReportUnread ? Color{220, 140, 140, 235}
                                                         : Color{92, 76, 76, 200});
            DrawText(label, (int)rb.x + 10, (int)rb.y + 8, 12,
                     m_hostReportUnread ? Color{240, 190, 190, 255} : Color{200, 180, 180, 255});
            if (rh && click) {
                m_hostReportsOpen = true;
                m_hostReportScroll = 0;
                Audio::get().playSfx("panel_open");
                return;
            }
        }
    }
    if (m_mailSettingsOpen) { drawMailSettings(x, y, w, h, mouse, click, accent); return; }

    const mail::Box& box = mailbox(m_playerCountryId);
    auto nameOf = [&](int cid) -> std::string {
        if (const Country* c = m_countries.getCountry(cid)) return c->name;
        return T("Unknown");
    };

    // ── Picking somebody to write to ──
    if (m_mailPicking) {
        DrawText(T("Write to..."), x + 24, y + 56, 14, Color{170, 176, 196, 255});
        const Rectangle list = {(float)(x + 24), (float)(y + 82), (float)(w - 48), (float)(h - 150)};
        BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
        int ry = (int)list.y - m_mailPickerScroll;
        for (const auto& [cid, country] : m_countries.getAll()) {
            if (cid == m_playerCountryId) continue;
            const bool isBot = mailIsBot(cid);
            if (mail::mayWrite(mailRules(), isBot, mailLockOf(cid)) != mail::Refusal::None) continue;

            const Rectangle row = {list.x, (float)ry, list.width, 34};
            const bool rh = CheckCollisionPointRec(mouse, row) &&
                            CheckCollisionPointRec(mouse, list);
            if (ry > list.y - 40 && ry < list.y + list.height) {
                DrawRectangleRec(row, rh ? Color{34, 38, 50, 230} : Color{20, 22, 30, 180});
                DrawText(country.name.c_str(), (int)row.x + 12, (int)row.y + 9, 14,
                         rh ? WHITE : Color{200, 206, 226, 255});
                if (isBot) {
                    const int tw = MeasureText(T(mail::botTag()), 11);
                    DrawRectangleRounded({row.x + row.width - tw - 30, row.y + 9, (float)tw + 14, 17},
                                         0.4f, 6, Color{60, 48, 78, 230});
                    DrawText(T(mail::botTag()), (int)(row.x + row.width - tw - 23),
                             (int)row.y + 12, 11, Color{206, 186, 232, 255});
                }
            }
            if (rh && click) {
                m_mailThread = cid;
                m_mailPicking = false;
                m_mailDraft.clear();
                m_mailEditing = 0;
                m_mailComposeFocus = true;
                Audio::get().playSfx("click_light", 0.1f);
            }
            ry += 36;
        }
        EndScissorMode();
        if (CheckCollisionPointRec(mouse, list)) {
            const float wheel = GetMouseWheelMove();
            if (wheel != 0.0f) {
                const int maxScroll = std::max(0, ry - (int)list.y - (int)list.height + 20 +
                                                    m_mailPickerScroll);
                m_mailPickerScroll = std::clamp(m_mailPickerScroll - (int)(wheel * 40), 0, maxScroll);
            }
        }
        const Rectangle back = {(float)(x + 24), (float)(y + h - 52), 140, 32};
        const bool bh = CheckCollisionPointRec(mouse, back);
        DrawRectangleRounded(back, 0.2f, 6, bh ? Color{44, 48, 66, 240} : Color{28, 30, 42, 220});
        DrawRectangleRoundedLines(back, 0.2f, 6, Color{90, 96, 130, 200});
        DrawText(T("Back"), (int)back.x + 14, (int)back.y + 9, 13, WHITE);
        if (bh && click) { m_mailPicking = false; Audio::get().playSfx("back"); }
        return;
    }

    // ── The list of correspondents ──
    if (m_mailThread == 0) {
        const Rectangle newBtn = {(float)(x + w - 104 - 150), (float)(y + 18), 140, 28};
        const bool nh = CheckCollisionPointRec(mouse, newBtn);
        DrawRectangleRounded(newBtn, 0.2f, 6, nh ? Color{46, 92, 60, 250} : Color{34, 68, 46, 235});
        DrawRectangleRoundedLines(newBtn, 0.2f, 6, Color{110, 180, 130, 220});
        DrawText(T("New letter"), (int)newBtn.x + 12, (int)newBtn.y + 7, 13, WHITE);
        if (nh && click) { m_mailPicking = true; m_mailPickerScroll = 0; }

        const auto threads = box.threads();
        if (threads.empty()) {
            DrawText(T("No letters yet. Start one with \"New letter\"."),
                     x + 24, y + 70, 14, Color{130, 136, 156, 255});
            return;
        }

        const Rectangle list = {(float)(x + 24), (float)(y + 62), (float)(w - 48), (float)(h - 100)};
        BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
        int ry = (int)list.y - m_mailListScroll;
        for (const mail::Thread* t : threads) {
            const Rectangle row = {list.x, (float)ry, list.width, 52};
            const bool rh = CheckCollisionPointRec(mouse, row) &&
                            CheckCollisionPointRec(mouse, list);
            if (ry > list.y - 60 && ry < list.y + list.height) {
                DrawRectangleRec(row, rh ? Color{34, 38, 50, 235} : Color{20, 22, 30, 190});
                DrawText(nameOf(t->otherCountry).c_str(), (int)row.x + 12, (int)row.y + 8, 15,
                         rh ? WHITE : Color{206, 212, 232, 255});

                // The last thing said, and whether anything is still waiting.
                if (!t->messages.empty()) {
                    const mail::Message& last = t->messages.back();
                    int fs = 12;
                    std::string line = odText::fitToWidth(last.body, (int)row.width - 200, fs, 10);
                    DrawText(line.c_str(), (int)row.x + 12, (int)row.y + 29, fs,
                             Color{140, 146, 166, 255});
                }
                const size_t waiting = t->pendingCount();
                if (waiting > 0) {
                    const char* label = TextFormat(T("%d waiting to send"), (int)waiting);
                    const int tw = MeasureText(label, 11);
                    DrawText(label, (int)(row.x + row.width - tw - 14), (int)row.y + 10, 11,
                             Color{220, 190, 120, 255});
                }
                if (mailIsBot(t->otherCountry)) {
                    const int tw = MeasureText(T(mail::botTag()), 11);
                    DrawText(T(mail::botTag()), (int)(row.x + row.width - tw - 14),
                             (int)row.y + 30, 11, Color{186, 162, 220, 255});
                }
            }
            if (rh && click) {
                m_mailThread = t->otherCountry;
                m_mailScroll = 0;
                m_mailDraft.clear();
                m_mailEditing = 0;
                Audio::get().playSfx("click_light", 0.1f);
            }
            ry += 56;
        }
        EndScissorMode();
        if (CheckCollisionPointRec(mouse, list)) {
            const float wheel = GetMouseWheelMove();
            if (wheel != 0.0f) {
                const int maxScroll = std::max(0, ry - (int)list.y - (int)list.height + 20 +
                                                    m_mailListScroll);
                m_mailListScroll = std::clamp(m_mailListScroll - (int)(wheel * 40), 0, maxScroll);
            }
        }
        return;
    }

    // ── One correspondence ──
    drawMailThread(x, y, w, h, mouse, click, accent);
}

/**
 * One correspondence: the letters, and the box you write the next one in.
 *
 * Pending letters are drawn in the flow rather than in a separate outbox, so
 * you read the conversation as the other side will eventually read it -- but
 * they carry an "unsent" mark and the buttons to change or tear them up, which
 * is the difference this whole system exists to make visible.
 */
void Game::drawMailThread(int x, int y, int w, int h, Vector2 mouse, bool click, Color accent) {
    const int other = m_mailThread;
    const bool isBot = mailIsBot(other);
    std::string them = T("Unknown");
    if (const Country* c = m_countries.getCountry(other)) them = c->name;

    // Header: who, and a way back.
    const Rectangle back = {(float)(x + 24), (float)(y + 54), 90, 26};
    const bool bh = CheckCollisionPointRec(mouse, back);
    DrawRectangleRounded(back, 0.2f, 6, bh ? Color{44, 48, 66, 240} : Color{28, 30, 42, 220});
    DrawRectangleRoundedLines(back, 0.2f, 6, Color{90, 96, 130, 200});
    DrawText(T("Back"), (int)back.x + 12, (int)back.y + 6, 12, WHITE);
    if (bh && click) {
        m_mailThread = 0;
        m_mailDraft.clear();
        m_mailEditing = 0;
        Audio::get().playSfx("back");
        return;
    }
    DrawText(them.c_str(), x + 126, y + 56, 17, WHITE);
    if (isBot) {
        const int nw = MeasureText(them.c_str(), 17);
        DrawRectangleRounded({(float)(x + 134 + nw), (float)(y + 57), 44, 18}, 0.4f, 6,
                             Color{60, 48, 78, 235});
        DrawText(T(mail::botTag()), x + 142 + nw, y + 60, 11, Color{206, 186, 232, 255});
    }

    const mail::Box& box = mailbox(m_playerCountryId);
    const mail::Thread* t = box.thread(other);

    const int composeH = 106;
    const Rectangle view = {(float)(x + 24), (float)(y + 90), (float)(w - 48),
                            (float)(h - 90 - composeH - 34)};
    DrawRectangleRec(view, Color{11, 12, 17, 255});
    DrawRectangleLinesEx(view, 1, Color{50, 54, 72, 180});

    BeginScissorMode((int)view.x, (int)view.y, (int)view.width, (int)view.height);
    int ly = (int)view.y + 10 - m_mailScroll;
    const int bubbleMax = (int)view.width - 120;

    if (t) {
        for (const mail::Message& m : t->messages) {
            const bool mine = (m.fromCountry == m_playerCountryId);
            const Ink ink = inkFor(m, mine, accent);

            // Wrap by hand into a bubble of at most bubbleMax.
            std::vector<std::string> lines;
            {
                std::string line;
                for (size_t i = 0; i <= m.body.size(); ++i) {
                    const bool end = (i == m.body.size());
                    if (!end && m.body[i] != '\n') {
                        line += m.body[i];
                        if (MeasureText(line.c_str(), 13) < bubbleMax - 20) continue;
                    }
                    lines.push_back(line);
                    line.clear();
                }
            }
            int widest = 0;
            for (const std::string& l : lines) widest = std::max(widest, MeasureText(l.c_str(), 13));
            const int bw = std::min(bubbleMax, widest + 24);
            const int bh2 = (int)lines.size() * 17 + 28;
            const int bx = mine ? (int)(view.x + view.width - bw - 12) : (int)view.x + 12;

            if (ly + bh2 > view.y - 20 && ly < view.y + view.height) {
                DrawRectangleRounded({(float)bx, (float)ly, (float)bw, (float)bh2}, 0.12f, 6,
                                     ink.bubble);
                DrawRectangleRoundedLines({(float)bx, (float)ly, (float)bw, (float)bh2}, 0.12f, 6,
                                          ink.edge);
                int ty = ly + 8;
                for (const std::string& l : lines) {
                    DrawText(l.c_str(), bx + 12, ty, 13, ink.text);
                    ty += 17;
                }
                // The footer says what a reader needs: when, and from a machine
                // or a person. The bot tag rides on every single one.
                std::string foot;
                if (m.status == mail::Status::Pending) {
                    foot = T("not sent yet");
                } else {
                    foot = TextFormat(T("turn %d"), m.deliverTurn);
                }
                if (m.author == mail::Author::Bot) foot += std::string(" · ") + T(mail::botTag());
                if (!mine && !m.authorName.empty()) foot += " · " + m.authorName;
                // Read against the bubble it sits on, not against the panel.
                // Grey at 120 is legible on the dark blue of a player's letter
                // and nearly invisible on the purple of a machine's -- and the
                // bot tag lives in this line, which is the one piece of it that
                // must never be hard to read.
                Color footCol = Color{120, 126, 146, 255};
                if (m.status == mail::Status::Pending) footCol = Color{226, 196, 128, 255};
                else if (m.author == mail::Author::Bot) footCol = Color{178, 158, 208, 255};
                DrawText(foot.c_str(), bx + 12, ly + bh2 - 16, 10, footCol);

                // Reporting is offered only on letters somebody else wrote, and
                // only where there is somewhere to send it. Deliberately quiet:
                // a prominent accuse button beside every message invites use as
                // a weapon, and the people who need it will look for it.
                if (!mine && m.status == mail::Status::Delivered &&
                    (m_netSession != nullptr || canReportToIssuer(other))) {
                    const int rw = MeasureText(T("report"), 10) + 12;
                    const Rectangle rr = {(float)(bx + bw - rw - 8), (float)(ly + bh2 - 18),
                                          (float)rw, 15};
                    const bool rh = CheckCollisionPointRec(mouse, rr);
                    DrawText(T("report"), (int)rr.x + 6, (int)rr.y + 3, 10,
                             rh ? Color{240, 170, 170, 255} : Color{110, 96, 96, 255});
                    if (rh && click) {
                        EndScissorMode();
                        openReportDialog(m.id, other);
                        return;
                    }
                }

                // Change or tear up, while it is still yours.
                if (mine && m.status == mail::Status::Pending) {
                    const int ew = MeasureText(T("edit"), 10) + 12;
                    const int dw = MeasureText(T("discard"), 10) + 12;
                    const Rectangle er = {(float)(bx + bw - ew - dw - 14), (float)(ly + bh2 - 18),
                                          (float)ew, 15};
                    const Rectangle dr = {(float)(bx + bw - dw - 8), (float)(ly + bh2 - 18),
                                          (float)dw, 15};
                    const bool eh = CheckCollisionPointRec(mouse, er);
                    const bool dh = CheckCollisionPointRec(mouse, dr);
                    DrawText(T("edit"), (int)er.x + 6, (int)er.y + 3, 10,
                             eh ? WHITE : Color{150, 170, 200, 255});
                    DrawText(T("discard"), (int)dr.x + 6, (int)dr.y + 3, 10,
                             dh ? Color{240, 170, 170, 255} : Color{180, 130, 130, 255});
                    if (eh && click) {
                        m_mailDraft = m.body;
                        m_mailEditing = m.id;
                        m_mailComposeFocus = true;
                        Audio::get().playSfx("click_light", 0.1f);
                    }
                    if (dh && click) {
                        mailbox(m_playerCountryId).discard(m.id);
                        if (m_mailEditing == m.id) { m_mailEditing = 0; m_mailDraft.clear(); }
                        Audio::get().playSfx("back");
                        EndScissorMode();
                        return;
                    }
                }
            }
            ly += bh2 + 8;
        }
    }
    EndScissorMode();

    if (CheckCollisionPointRec(mouse, view)) {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            const int maxScroll = std::max(0, ly - (int)view.y - (int)view.height + 20 + m_mailScroll);
            m_mailScroll = std::clamp(m_mailScroll - (int)(wheel * 45), 0, maxScroll);
        }
    }

    // ── Writing the next one ──
    const Rectangle field = {(float)(x + 24), (float)(y + h - composeH - 20),
                             (float)(w - 48 - 130), (float)(composeH - 44)};
    const bool fh = CheckCollisionPointRec(mouse, field);
    DrawRectangleRec(field, m_mailComposeFocus ? Color{22, 25, 34, 255} : Color{15, 17, 23, 255});
    DrawRectangleLinesEx(field, 1, m_mailComposeFocus ? accent : Color{60, 64, 84, 200});
    if (fh && click) m_mailComposeFocus = true;

    if (m_mailDraft.empty() && !m_mailComposeFocus) {
        DrawText(T("Write a letter. It leaves when the turn is processed."),
                 (int)field.x + 8, (int)field.y + 8, 12, Color{96, 100, 118, 255});
    } else {
        int ty = (int)field.y + 6;
        std::string line;
        for (size_t i = 0; i <= m_mailDraft.size(); ++i) {
            const bool end = (i == m_mailDraft.size());
            if (!end && m_mailDraft[i] != '\n') {
                line += m_mailDraft[i];
                if (MeasureText(line.c_str(), 13) < field.width - 20) continue;
            }
            if (ty + 16 < field.y + field.height)
                DrawText(line.c_str(), (int)field.x + 8, ty, 13, WHITE);
            ty += 16;
            line.clear();
        }
        if (m_mailComposeFocus && (int)(GetTime() * 2) % 2)
            DrawRectangle((int)field.x + 8 + MeasureText(line.c_str(), 13),
                          std::min(ty, (int)(field.y + field.height - 18)), 2, 14, WHITE);
    }

    const Rectangle send = {(float)(x + w - 24 - 118), field.y, 118, field.height};
    const bool enough = !m_mailDraft.empty();
    const bool sh = CheckCollisionPointRec(mouse, send) && enough;
    DrawRectangleRounded(send, 0.15f, 6, !enough ? Color{24, 26, 34, 200}
                                                 : (sh ? Color{46, 92, 60, 250} : Color{34, 68, 46, 235}));
    DrawRectangleRoundedLines(send, 0.15f, 6, enough ? Color{110, 180, 130, 220} : Color{60, 64, 84, 180});
    const char* label = m_mailEditing ? T("Save") : T("Put in outbox");
    int fs = 13;
    const std::string fit = odText::fitToWidth(label, (int)send.width - 16, fs, 10);
    DrawText(fit.c_str(), (int)send.x + 10, (int)(send.y + send.height / 2 - fs / 2), fs,
             enough ? WHITE : Color{110, 114, 132, 255});
    if (sh && click) mailSendDraft();

    DrawText(T("Letters leave when you process the turn. Until then you can change them."),
             x + 24, y + h - 30, 11, Color{120, 126, 146, 255});
}

// ────────────────────────────────────────────────────────────── the keys ────

void Game::updateMail() {
    if (!m_mailOpen) return;
    pumpLlmTest();   // the worker's answer, collected on this thread
    pumpLlmPull();

    // ── Typing into the runner settings ──
    if (m_mailSettingsOpen && m_mailLlmField >= 0) {
        std::string* target = m_mailLlmField == 0 ? &m_config.llmEndpoint
                            : m_mailLlmField == 1 ? &m_config.llmModel
                                                  : &m_config.llmApiKey;
        bool changed = false;
        int key = GetCharPressed();
        while (key > 0) {
            // ASCII only: a URL, a model name and an API key are all ASCII, and
            // a stray pasted character in an endpoint is a request that fails
            // for a reason nobody can see.
            if (key >= 32 && key < 127 && target->size() < 300) {
                *target += (char)key;
                changed = true;
            }
            key = GetCharPressed();
        }
        if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) &&
            !target->empty()) {
            target->pop_back();
            changed = true;
        }
        const bool paste = (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                            IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER)) &&
                           IsKeyPressed(KEY_V);
        if (paste) {
            if (const char* clip = GetClipboardText()) {
                for (const char* q = clip; *q && target->size() < 300; ++q) {
                    if ((unsigned char)*q >= 32 && (unsigned char)*q < 127) *target += *q;
                }
                changed = true;
            }
        }
        if (changed) {
            // Saved as it is typed. A setting that needs a separate Save button
            // is a setting somebody changes and then loses.
            m_config.save(m_configPath);
            m_llmTestResult.clear();      // a new address deserves a new test
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_TAB)) {
            m_mailLlmField = IsKeyPressed(KEY_TAB) ? (m_mailLlmField + 1) % 3 : -1;
        }
        if (IsKeyPressed(KEY_ESCAPE)) { m_mailLlmField = -1; return; }
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        // Step back one view at a time rather than closing outright: losing a
        // half-written letter to a stray Escape is the kind of thing that stops
        // people writing long ones.
        if (m_mailSettingsOpen) { closeMailSettings(); return; }
        if (m_mailPicking) m_mailPicking = false;
        else if (m_mailThread != 0) { m_mailThread = 0; m_mailDraft.clear(); m_mailEditing = 0; }
        else closeMail();
        return;
    }
    if (m_mailPicking || m_mailThread == 0) return;
    if (!m_mailComposeFocus) return;

    int key = GetCharPressed();
    while (key > 0) {
        // Same UTF-8 handling as the report form, and for the same reason: a
        // letter written in Ukrainian is a letter.
        if (key >= 32 && m_mailDraft.size() + 4 <= mail::kMaxBody) {
            unsigned cp = (unsigned)key;
            if (cp < 0x80) { m_mailDraft += (char)cp; }
            else if (cp < 0x800) {
                m_mailDraft += (char)(0xC0 | (cp >> 6));
                m_mailDraft += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                m_mailDraft += (char)(0xE0 | (cp >> 12));
                m_mailDraft += (char)(0x80 | ((cp >> 6) & 0x3F));
                m_mailDraft += (char)(0x80 | (cp & 0x3F));
            } else {
                m_mailDraft += (char)(0xF0 | (cp >> 18));
                m_mailDraft += (char)(0x80 | ((cp >> 12) & 0x3F));
                m_mailDraft += (char)(0x80 | ((cp >> 6) & 0x3F));
                m_mailDraft += (char)(0x80 | (cp & 0x3F));
            }
        }
        key = GetCharPressed();
    }

    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (!m_mailDraft.empty()) {
            // One CHARACTER, continuation bytes and all.
            size_t i = m_mailDraft.size() - 1;
            while (i > 0 && (unsigned char)m_mailDraft[i] >= 0x80 &&
                   (unsigned char)m_mailDraft[i] < 0xC0) --i;
            m_mailDraft.erase(i);
            Audio::get().playSfx("key_type", 0.12f);
        }
    }

    // Enter sends; Shift+Enter breaks a line. The messenger convention, and the
    // one people try first.
    if (IsKeyPressed(KEY_ENTER)) {
        if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
            if (m_mailDraft.size() < mail::kMaxBody) m_mailDraft += '\n';
        } else {
            mailSendDraft();
        }
    }

    const bool paste = (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                        IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER)) &&
                       IsKeyPressed(KEY_V);
    if (paste) {
        if (const char* clip = GetClipboardText()) {
            for (const char* q = clip; *q && m_mailDraft.size() < mail::kMaxBody; ++q) {
                const unsigned char c = (unsigned char)*q;
                if (c == '\r') continue;
                if (c < 32 && c != '\n' && c != '\t') continue;
                m_mailDraft += *q;
            }
        }
    }
}

// ─────────────────────────────────────────────────── mail settings ────
//
// Kept HERE rather than in the settings screen, and that is a deliberate
// choice rather than a shortcut. The settings screen is an index-driven table
// whose rows are wired through four separate chains; adding six entries to it
// is both laborious and the kind of change that silently shifts a neighbouring
// row's meaning. These options are also all about mail, and a player looking
// for "who may write to me" looks in the mail window before they look in
// Settings > Advanced.

void Game::drawMailSettings(int x, int y, int w, int h, Vector2 mouse, bool click,
                            Color accent) {
    DrawText(T("Mail settings"), x + 24, y + 56, 15, accent);

    // ── SCROLLED AND CLIPPED ──
    //
    // This pane had neither, and grew past the bottom of its own panel once the
    // runner installer and the model list were added to it: the last rows drew
    // over the map and the sidebar, outside the frame they belong to. The age
    // prompt -- which a host has to be able to reach -- was among the rows that
    // fell off.
    const int topY = y + 76;
    const int botY = y + h - 16;
    BeginScissorMode(x, topY, w, botY - topY);
    int cy = y + 84 - m_mailSettingsScroll;

    auto row = [&](const char* label, const char* help) {
        DrawText(T(label), x + 24, cy, 13, Color{206, 212, 232, 255});
        if (help && *help) {
            int fs = 11;
            const std::string fit = odText::fitToWidth(T(help), w - 48, fs, 9);
            DrawText(fit.c_str(), x + 24, cy + 17, fs, Color{124, 130, 150, 255});
            cy += 16;
        }
        cy += 20;
    };

    /// A row of mutually exclusive choices. Returns the one picked, or -1.
    auto choices = [&](const std::vector<const char*>& labels, int current,
                       const std::vector<bool>& enabled) {
        int picked = -1;
        int cx = x + 24;
        for (size_t i = 0; i < labels.size(); ++i) {
            const char* label = T(labels[i]);
            const int cw = MeasureText(label, 12) + 20;
            if (cx + cw > x + w - 24) { cx = x + 24; cy += 28; }
            const Rectangle r = {(float)cx, (float)cy, (float)cw, 24};
            const bool on = ((int)i == current);
            const bool live = i < enabled.size() ? enabled[i] : true;
            const bool hov = CheckCollisionPointRec(mouse, r) && live;
            DrawRectangleRounded(r, 0.3f, 6, !live ? Color{18, 19, 24, 180}
                                          : on ? Color{40, 52, 64, 245}
                                          : (hov ? Color{32, 34, 44, 230} : Color{20, 22, 30, 210}));
            DrawRectangleRoundedLines(r, 0.3f, 6, on ? accent : Color{62, 66, 86, 170});
            DrawText(label, cx + 10, (int)r.y + 6, 12,
                     !live ? Color{88, 90, 104, 255} : (on ? WHITE : Color{160, 166, 186, 255}));
            if (hov && click) picked = (int)i;
            cx += cw + 8;
        }
        cy += 34;
        return picked;
    };

    auto tick = [&](const char* label, bool value, const char* help) {
        const Rectangle cb = {(float)(x + 24), (float)cy, 18, 18};
        const bool hov = CheckCollisionPointRec(mouse, {cb.x, cb.y, (float)(w - 48), 20});
        DrawRectangleRec(cb, value ? Color{40, 70, 50, 255} : Color{18, 20, 28, 255});
        DrawRectangleLinesEx(cb, 1, value ? accent : Color{80, 84, 104, 200});
        if (value) DrawText("x", (int)cb.x + 5, (int)cb.y + 2, 14, WHITE);
        DrawText(T(label), (int)cb.x + 26, (int)cb.y + 3, 13,
                 hov ? WHITE : Color{190, 196, 216, 255});
        cy += 22;
        if (help && *help) {
            int fs = 11;
            const std::string fit = odText::fitToWidth(T(help), w - 76, fs, 9);
            DrawText(fit.c_str(), x + 50, cy, fs, Color{124, 130, 150, 255});
            cy += 16;
        }
        cy += 8;
        return hov && click;
    };

    // ── My own door ──
    row("Who may write to me",
        "Yours, and it always wins. A host can narrow who may speak on their "
        "server; nothing a host allows puts a letter through a door you shut.");
    {
        const int picked = choices({"Anyone", "Advisors only", "Nobody"},
                                   m_config.mailLock, {true, true, true});
        if (picked >= 0) {
            m_config.mailLock = picked;
            m_config.save(m_configPath);
            Audio::get().playSfx("click_light", 0.1f);
        }
    }

    // ── The host's setting ──
    const bool amHost = (m_netHost != nullptr) || (m_netSession == nullptr);
    row(amHost ? "Who may write, in games you run" : "Who may write here (set by the host)",
        amHost ? "Advisor options need the language-model module loaded."
               : "Only the host can change this.");
    {
        // ALL FOUR SELECTABLE, INCLUDING THE ADVISOR ONES WITHOUT A MODEL.
        //
        // The advisor choices used to be greyed until a model was answering,
        // which is a trap rather than a safeguard: a player who picked "Players
        // only" could not pick their way back to an advisor policy without
        // already having what the policy is for. A setting is an INTENT, and it
        // is allowed to describe a game the player is still setting up -- the
        // line above says what is still missing.
        const std::vector<bool> live = {amHost, amHost, amHost, amHost};
        const int picked = choices({"Nobody", "Players only", "Advisors only", "Everyone"},
                                   m_config.mailPolicy, live);
        // The one combination that silently produces no Mail button at all.
        if (m_config.mailPolicy == (int)mail::Policy::PlayersOnly &&
            m_netHost == nullptr && m_netSession == nullptr) {
            int fs = 11;
            const std::string warn = odText::fitToWidth(
                T("In a single-player game this means no mail at all. Choose "
                  "Advisors only or Everyone to write to countries."),
                w - 74, fs, 9);
            DrawText(warn.c_str(), x + 24, cy, fs, Color{190, 160, 110, 255});
            cy += fs + 8;
        }
        if (picked >= 0 && amHost) {
            m_config.mailPolicy = picked;
            m_config.save(m_configPath);
            Audio::get().playSfx("click_light", 0.1f);
        }
    }

    // ── The advisors ──
    row("AI advisors", "Countries answer their own mail, in your language. "
                       "They are allowed to lie, and every letter is tagged as theirs.");
    if (tick("Use a language model for diplomacy", m_config.llmEnabled,
             "Needs a runner on this machine, or an API key.")) {
        m_config.llmEnabled = !m_config.llmEnabled;
        m_config.save(m_configPath);
        rebuildLlmCountries();
        Audio::get().playSfx("click_light", 0.1f);
    }

    // Where it lives, and what to ask for. Editable HERE rather than only in
    // config.json: a feature whose only configuration is a text file somebody
    // has to find is a feature almost nobody turns on.
    if (m_config.llmEnabled) {
        // ── WHAT TO DO NEXT, IN ONE SENTENCE ──
        //
        // This pane used to be a wall of controls in no particular order: three
        // text fields, a Test button, an installer and a model list, all shown
        // at once and all equally prominent. A player could install a runner,
        // never start it, and be told only "Nothing answered there."
        //
        // So the state is worked out and named. The steps are still all
        // visible -- hiding them would make the screen feel like it was
        // deciding for you -- but exactly one is called out as the next thing.
        {
            const bool have    = llm::installed(m_dataDir);
            const bool running = llmServerRunning();

            // A RUNNER IN THE GAME'S OWN FOLDER IMPLIES ITS OWN ADDRESS. There
            // is exactly one place it can be reached, this is the code that put
            // it there, and making the player type that back in is asking them
            // to supply a fact the game already knows. Set once, when it is
            // missing, from every path into this pane rather than only after an
            // install -- a runner installed by an earlier version, or by
            // --llm-install on the command line, arrives here with nothing set.
            if (have && m_config.llmEndpoint.empty()) {
                m_config.llmEndpoint = llm::localEndpoint();
                m_config.save(m_configPath);
            }
            const bool haveModel = !m_config.llmModel.empty();
            // A REMOTE SERVICE SKIPS THE MIDDLE. Somebody who has typed their
            // own endpoint is not installing or starting anything, and telling
            // them to press Start it would be advice for a different setup.
            const bool remote = !m_config.llmEndpoint.empty() &&
                                !llm::isLocal(m_config.llmEndpoint);
            const char* next;
            Color tone{150, 156, 176, 255};
            if (remote && !haveModel)            next = "Next: name the model this service expects.";
            else if (remote && !m_llmAvailable)  next = "Next: press Test the runner.";
            else if (remote) { next = "Ready. Countries will answer their own mail.";
                               tone = Color{120, 190, 140, 255}; }
            else if (!have && llm::canInstall()) next = "Next: install the runner below.";
            else if (!have)                      next = "Next: install Ollama yourself, then set Runner below.";
            // Checked BEFORE the model, and without asking whether the endpoint
            // looks local: an installed-but-stopped runner is the state this
            // whole screen was failing at, and while it is stopped a model
            // cannot be pulled either -- the pull talks to the runner.
            else if (!running)                   next = "Next: press Start it.";
            else if (!haveModel)                 next = "Next: pull a model below.";
            else if (!m_llmAvailable)            next = "Next: press Test the runner.";
            else { next = "Ready. Countries will answer their own mail.";
                   tone = Color{120, 190, 140, 255}; }
            DrawText(T(next), x + 50, cy, 12, tone);
            cy += 22;
        }

        auto field = [&](const char* label, std::string& value, int which,
                         const char* placeholder, bool secret) {
            DrawText(T(label), x + 50, cy, 11, Color{130, 136, 156, 255});
            const Rectangle f = {(float)(x + 50 + 96), (float)cy - 4, (float)(w - 74 - 96 - 24), 22};
            const bool focused = (m_mailLlmField == which);
            const bool hov = CheckCollisionPointRec(mouse, f);
            DrawRectangleRec(f, focused ? Color{24, 26, 34, 255} : Color{15, 17, 23, 255});
            DrawRectangleLinesEx(f, 1, focused ? accent : Color{58, 60, 74, 175});
            // An API key is never drawn back: a screenshot or a stream should
            // not carry it, and the player already knows what they typed.
            std::string shown = value.empty() ? std::string(T(placeholder))
                              : (secret ? std::string(value.size(), '*') : value);
            if (secret && !value.empty() && shown.size() > 32) shown.resize(32);
            DrawText(shown.c_str(), (int)f.x + 7, (int)f.y + 5, 11,
                     value.empty() ? Color{92, 94, 108, 255} : Color{206, 212, 232, 255});
            if (focused && (int)(GetTime() * 2) % 2) {
                const int cw = MeasureText(secret ? shown.c_str() : value.c_str(), 11);
                DrawRectangle((int)f.x + 7 + cw, (int)f.y + 4, 2, 13, WHITE);
            }
            if (hov && click) m_mailLlmField = which;
            cy += 26;
        };
        field("Runner", m_config.llmEndpoint, 0, "http://127.0.0.1:11434/v1", false);
        // NOT "local-model". That was the old default, it is not a model any
        // runner has, and offering it back as a hint invites the player to
        // type in the exact string that made this screen fail.
        field("Model",  m_config.llmModel,    1, "pull one below, or type a model name", false);
        // Only meaningful for a remote endpoint -- a local runner needs none,
        // and the game refuses to send one there. See Game_Llm.cpp.
        if (!llm::isLocal(m_config.llmEndpoint)) {
            field("API key", m_config.llmApiKey, 2, "for a remote service", true);
        } else {
            DrawText(T("A runner on this machine needs no key, and none is sent."),
                     x + 50, cy, 11, Color{110, 128, 112, 255});
            cy += 22;
        }
        cy += 6;

        // ── Is anything actually there? ──
        //
        // A "Test" button rather than leaving it to be discovered on turn
        // three. The failure modes here are all invisible until a letter goes
        // unanswered -- runner not started, wrong port, model name that does
        // not exist -- and each of them is a different fix.
        {
            const Rectangle t = {(float)(x + 50), (float)cy, 130, 26};
            const bool th = CheckCollisionPointRec(mouse, t);
            DrawRectangleRounded(t, 0.2f, 6, th ? Color{46, 62, 84, 245} : Color{26, 30, 40, 225});
            DrawRectangleRoundedLines(t, 0.2f, 6, Color{80, 90, 116, 200});
            DrawText(T("Test the runner"), (int)t.x + 10, (int)t.y + 7, 12, WHITE);
            if (th && click) testLlmRunner();

            if (!m_llmTestResult.empty()) {
                int fs = 11;
                const std::string fit =
                    odText::fitToWidth(m_llmTestResult, w - 74 - 140, fs, 9);
                DrawText(fit.c_str(), (int)t.x + 142, (int)t.y + 8, fs,
                         m_llmTestOk ? Color{140, 195, 150, 255} : Color{224, 160, 160, 255});
            }
            cy += 34;
        }

        // ── The runner itself ──
        {
            const bool have = llm::installed(m_dataDir);
            DrawText(have ? T("Ollama is installed in the game's own folder.")
                          : (llm::canInstall() ? T("No runner installed here yet.")
                                               : T("This platform installs Ollama its own way.")),
                     x + 50, cy, 11, Color{130, 136, 156, 255});
            cy += 18;

            int fs = 11;
            if (!have) {
                // Everything about it BEFORE a byte moves. A game that quietly
                // downloads an executable has done something on the player's
                // behalf that they never agreed to.
                const std::string what = odText::fitToWidth(
                    llm::canInstall() ? llm::describeDownload()
                                      : std::string(llm::manualInstructions()),
                    w - 74, fs, 9);
                DrawText(what.c_str(), x + 50, cy, fs, Color{116, 122, 142, 255});
                cy += 18;
                if (llm::canInstall()) {
                    DrawText(T("About 160 MB. It does not include a model -- pull one "
                               "afterwards with: ollama pull gemma3:4b"),
                             x + 50, cy, 11, Color{116, 122, 142, 255});
                    cy += 18;
                }
            }

            int bx = x + 50;
            if (llm::canInstall() && !have) {
                const Rectangle b = {(float)bx, (float)cy, 170, 26};
                const bool bh = CheckCollisionPointRec(mouse, b) && !m_llmInstalling;
                DrawRectangleRounded(b, 0.2f, 6, m_llmInstalling ? Color{26, 30, 34, 220}
                                               : (bh ? Color{46, 92, 60, 250}
                                                     : Color{34, 68, 46, 235}));
                DrawRectangleRoundedLines(b, 0.2f, 6, Color{110, 180, 130, 220});
                DrawText(m_llmInstalling ? T("Installing...") : T("Download and install"),
                         (int)b.x + 10, (int)b.y + 7, 12, WHITE);
                if (bh && click) installLlmRunner();
                bx += 178;
            }
            if (have) {
                // THE BUTTON THAT WAS MISSING. Installing ended with "start it,
                // then press Test the runner" and gave nothing to start it
                // with -- and the whole point of installing here rather than
                // system-wide is that ollama is NOT on the player's PATH, so
                // the instruction could not be followed except from a terminal.
                const bool running = llmServerRunning();
                const Rectangle sb = {(float)bx, (float)cy, 110, 26};
                const bool sh = CheckCollisionPointRec(mouse, sb);
                DrawRectangleRounded(sb, 0.2f, 6,
                    running ? (sh ? Color{74, 60, 40, 245} : Color{34, 30, 24, 225})
                            : (sh ? Color{46, 92, 60, 250} : Color{34, 68, 46, 235}));
                DrawRectangleRoundedLines(sb, 0.2f, 6,
                    running ? Color{170, 140, 90, 210} : Color{110, 180, 130, 220});
                DrawText(running ? T("Stop it") : T("Start it"),
                         (int)sb.x + 10, (int)sb.y + 7, 12, WHITE);
                if (sh && click) { running ? stopLlmServer() : startLlmServer(); }
                bx += 118;

                DrawText(running ? T("running") : T("not running"),
                         bx, (int)cy + 8, 11,
                         running ? Color{120, 190, 140, 255} : Color{140, 130, 110, 255});
                bx += MeasureText(running ? T("running") : T("not running"), 11) + 16;

                const Rectangle b = {(float)bx, (float)cy, 110, 26};
                const bool bh = CheckCollisionPointRec(mouse, b);
                DrawRectangleRounded(b, 0.2f, 6, bh ? Color{74, 44, 44, 245} : Color{30, 26, 28, 220});
                DrawRectangleRoundedLines(b, 0.2f, 6, Color{140, 100, 100, 200});
                DrawText(T("Remove it"), (int)b.x + 10, (int)b.y + 7, 12,
                         bh ? Color{240, 190, 190, 255} : Color{200, 176, 176, 255});
                if (bh && click) {
                    stopLlmServer();   // before the binary goes, not after
                    m_llmTestOk = llm::uninstall(m_dataDir);
                    m_llmTestResult = m_llmTestOk ? T("Removed.") : T("Could not remove it.");
                }
            }
            cy += 34;
        }

        // ── The weights ──
        //
        // Offered as a shortlist with the SIZE and the LICENCE beside each,
        // because pulling one is accepting its terms and they differ. Sizes are
        // Ollama's registry's own figures. Anything not listed can still be
        // typed into the Model field above and pulled.
        {
            DrawText(T("Models"), x + 50, cy, 12, Color{170, 176, 196, 255});
            cy += 18;

            if (m_llmPulling) {
                // A multi-gigabyte download, so it says how far it has got
                // rather than sitting on "please wait".
                const Rectangle bar = {(float)(x + 50), (float)cy, (float)(w - 100), 16};
                DrawRectangleRec(bar, Color{18, 20, 28, 255});
                DrawRectangleRec({bar.x, bar.y, bar.width * m_llmPullFraction, bar.height},
                                 Color{46, 92, 60, 245});
                DrawRectangleLinesEx(bar, 1, Color{70, 96, 78, 200});
                DrawText(TextFormat("%s  %s  %d%%", m_llmPullModel.c_str(),
                                    m_llmPullStatus.c_str(),
                                    (int)(m_llmPullFraction * 100.0f)),
                         (int)bar.x + 8, (int)bar.y + 2, 11, Color{206, 226, 210, 255});
                cy += 26;
            } else {
                int count = 0;
                const llm::Model* models = llm::offeredModels(&count);
                for (int i = 0; i < count; ++i) {
                    const llm::Model& mo = models[i];
                    const bool chosen = (m_config.llmModel == mo.name);
                    const Rectangle row = {(float)(x + 50), (float)cy, (float)(w - 100), 34};
                    const bool hov = CheckCollisionPointRec(mouse, row);
                    DrawRectangleRec(row, chosen ? Color{26, 34, 30, 225}
                                                 : (hov ? Color{24, 26, 34, 220}
                                                        : Color{17, 19, 25, 190}));
                    DrawRectangleLinesEx(row, 1, chosen ? Color{90, 140, 105, 200}
                                                        : Color{48, 50, 62, 165});
                    DrawText(TextFormat("%s  ·  %s", mo.label, mo.size),
                             (int)row.x + 8, (int)row.y + 4, 12,
                             chosen ? WHITE : Color{198, 204, 224, 255});
                    DrawText(TextFormat("%s  ·  %s", mo.note, mo.licence),
                             (int)row.x + 8, (int)row.y + 19, 10, Color{120, 126, 146, 255});

                    const char* action = chosen ? T("in use") : T("Pull");
                    const int aw = MeasureText(action, 11) + 16;
                    const Rectangle b = {row.x + row.width - aw - 8, row.y + 6,
                                         (float)aw, 21};
                    const bool bh = CheckCollisionPointRec(mouse, b);
                    if (!chosen) {
                        DrawRectangleRounded(b, 0.3f, 6, bh ? Color{46, 78, 60, 245}
                                                           : Color{26, 34, 30, 220});
                        DrawRectangleRoundedLines(b, 0.3f, 6, Color{80, 120, 92, 190});
                    }
                    DrawText(action, (int)b.x + 8, (int)b.y + 5, 11,
                             chosen ? Color{140, 190, 150, 255}
                                    : (bh ? WHITE : Color{170, 190, 176, 255}));
                    if (!chosen && bh && click) pullLlmModel(mo.name);
                    cy += 38;
                }
                DrawText(T("Any other Ollama model can be typed into Model above and pulled."),
                         x + 50, cy, 10, Color{110, 116, 136, 255});
                cy += 16;
                const Rectangle other = {(float)(x + 50), (float)cy, 150, 22};
                const bool oh = CheckCollisionPointRec(mouse, other) &&
                                !m_config.llmModel.empty();
                DrawRectangleRounded(other, 0.25f, 6, oh ? Color{46, 62, 84, 245}
                                                         : Color{24, 28, 36, 215});
                DrawRectangleRoundedLines(other, 0.25f, 6, Color{72, 82, 104, 185});
                DrawText(T("Pull what is typed"), (int)other.x + 9, (int)other.y + 5, 11,
                         oh ? WHITE : Color{160, 172, 196, 255});
                if (oh && click) pullLlmModel(m_config.llmModel);
                cy += 30;
            }
        }
    }

    // ── The age prompt, described as exactly what it is ──
    if (amHost) {
        if (tick("Ask players to confirm their age", m_config.agePromptOn,
                 "A local question, stored on their own machine and sent nowhere. "
                 "It is NOT a verified age check and does not make this server "
                 "compliant with any law by itself.")) {
            m_config.agePromptOn = !m_config.agePromptOn;
            m_config.save(m_configPath);
            Audio::get().playSfx("click_light", 0.1f);
        }
    }

    // ── The host's word list ──
    if (amHost) {
        DrawText(TextFormat(T("Words this server will not carry: %d"),
                            (int)m_config.mailBlacklist.size()),
                 x + 24, cy, 12, Color{150, 156, 176, 255});
        cy += 16;
        int fs = 11;
        const std::string note = odText::fitToWidth(
            T("Edited in config.json under \"mailBlacklist\". Blunt on purpose: it is "
              "a handful of words you never want to see, not moderation."),
            w - 48, fs, 9);
        DrawText(note.c_str(), x + 24, cy, fs, Color{124, 130, 150, 255});
        cy += fs + 6;
    }

    EndScissorMode();

    // Measured from where the content actually ended rather than from a
    // constant, because how tall this pane is depends on what is installed:
    // the model list and the install button are only drawn some of the time.
    const int contentBottom = cy + m_mailSettingsScroll;
    const int maxScroll = std::max(0, contentBottom - botY + 24);
    const Rectangle pane = {(float)x, (float)topY, (float)w, (float)(botY - topY)};
    if (CheckCollisionPointRec(mouse, pane)) {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f)
            m_mailSettingsScroll = std::clamp(m_mailSettingsScroll - (int)(wheel * 40),
                                              0, maxScroll);
    }
    // Clamped every frame, not only on the wheel: ticking the language-model
    // box removes a screenful of rows, and a scroll left over from the taller
    // layout would otherwise strand the pane below its own content.
    m_mailSettingsScroll = std::clamp(m_mailSettingsScroll, 0, maxScroll);

    if (maxScroll > 0) {
        const float frac = (float)(botY - topY) / (float)(contentBottom - topY + 24);
        const float barH = std::max(24.0f, (botY - topY) * frac);
        const float t = (float)m_mailSettingsScroll / (float)maxScroll;
        const float barY = topY + t * ((botY - topY) - barH);
        DrawRectangleRounded({(float)(x + w - 10), barY, 4, barH}, 1.0f, 4,
                             Color{90, 95, 115, 200});
    }
}
