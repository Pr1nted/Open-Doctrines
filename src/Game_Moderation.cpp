// Reporting a letter, and reviewing what has been reported.
//
// TWO BUTTONS THAT DO DIFFERENT THINGS
//
// "Tell the host" reaches whoever runs this game and can remove somebody from
// it. "Tell OpenDoctrines" reaches the account service and can ban the account
// everywhere. Most bad behaviour belongs to the first: it is faster, it is
// local, and the person who can actually see the game is better placed to judge
// it. The screen says which is which rather than offering one "report" button
// that does something the reporter did not choose.
//
// THE REVIEW QUEUE IS THE SAME SCREEN FOR ONE PERSON
//
// An account carrying the `developer` badge gets a third view: everything
// filed, the message, whatever context came with it, and three outcomes. The
// badge is checked by the SERVICE, not here -- this screen only draws what the
// service agreed to send, and the service answers 404 to anyone else.

#include "Game.h"
#include "GameInternals.h"
#include "Audio.h"
#include "net/AccountClient.h"
#include "net/HttpClient.h"
#include "net/Session.h"
#include "net/Host.h"
#include "net/Lobby.h"
#include "i18n/Locale.h"
#include "i18n/Text.h"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <thread>

namespace {

std::mutex g_lock;
std::string g_result;          ///< what the service last said, for the screen
std::string g_queueJson;       ///< the review queue, as fetched
bool        g_busy = false;

std::string escapeJson(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += (char)c;
        }
    }
    return out;
}

}  // namespace

const char* Game::reportReasonId(int index) {
    // Wire values. Never translated -- the service matches on these.
    static const char* kIds[] = {
        "harassment", "hate", "threats", "sexual", "spam", "cheating", "other",
    };
    return kIds[std::clamp(index, 0, 6)];
}

const char* Game::reportReasonLabel(int index) {
    static const char* kLabels[] = {
        "Harassment", "Hate speech", "Threats", "Sexual content",
        "Spam", "Cheating", "Something else",
    };
    return kLabels[std::clamp(index, 0, 6)];
}

void Game::openReportDialog(int messageId, int aboutCountry) {
    m_reportOpen = true;
    m_reportMessageId = messageId;
    m_reportCountry = aboutCountry;
    m_reportReason = 0;
    m_reportNote.clear();
    m_reportWithContext = true;
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_result.clear();
    }
    Audio::get().playSfx("panel_open");
}

void Game::closeReportDialog() {
    m_reportOpen = false;
    Audio::get().playSfx("panel_close");
}

/**
 * Send it to the account service.
 *
 * Needs an account, because an accusation nobody can attribute is one anybody
 * can make. The reported message and, if the reporter agreed, the lines around
 * it travel with it -- a report the service cannot read is a report it cannot
 * act on, and PRIVACY.md says so plainly.
 */
void Game::sendReportToIssuer() {
    const AccountClient& account = AccountClient::get();
    if (!account.configured() || !account.account().valid()) {
        std::lock_guard<std::mutex> g(g_lock);
        g_result = T("Please sign in first (Main menu > Account).");
        return;
    }
    const mail::Box* box = mailboxIfAny(m_playerCountryId);
    if (!box) return;
    const mail::Message* reported = box->find(m_reportMessageId);
    if (!reported) return;

    // The lines around it, only if the reporter ticked the box. Their choice,
    // because context helps a maintainer and also hands over more of a private
    // conversation than the one message they objected to.
    std::vector<std::string> context;
    if (m_reportWithContext) {
        if (const mail::Thread* t = box->thread(m_reportCountry)) {
            for (const mail::Message& m : t->messages) {
                if (m.id == reported->id) continue;
                if (m.status != mail::Status::Delivered) continue;
                context.push_back(m.body);
            }
            // The whole exchange up to the service's cap. The argument BEFORE
            // the message is usually what decides whether it was one bad
            // moment or a pattern, and a maintainer who cannot see it guesses.
            if (context.size() > 40) context.erase(context.begin(), context.end() - 40);
        }
    }

    // THE PSEUDONYM, WHICH IS THE ONLY IDENTITY A CLIENT EVER HAS.
    //
    // A roster carries no account id and no provider identity on purpose (see
    // NetProtocol.h). The service resolves this back to an account through the
    // note its ticket mint left; if that note has expired it says so rather
    // than guessing. In a single-player game there is no roster, so there is no
    // account behind the correspondent and this button is not offered at all.
    const std::string accusedPsid = mpPsidForCountry(m_reportCountry);
    if (accusedPsid.empty()) {
        std::lock_guard<std::mutex> g(g_lock);
        g_result = T("There is no account behind this correspondent.");
        return;
    }

    std::ostringstream body;
    body << "{\"accusedPsid\":\"" << escapeJson(accusedPsid) << "\""
         << ",\"reason\":\"" << reportReasonId(m_reportReason) << "\""
         << ",\"note\":\"" << escapeJson(m_reportNote) << "\""
         << ",\"message\":\"" << escapeJson(reported->body) << "\""
         << ",\"server\":\"" << escapeJson(mpServerLabel()) << "\""
         << ",\"context\":[";
    for (size_t i = 0; i < context.size(); ++i) {
        if (i) body << ",";
        body << "\"" << escapeJson(context[i]) << "\"";
    }
    body << "]}";

    std::string url = m_config.accountIssuer;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/moderation/report";

    const std::string token = account.sessionToken();
    const std::string sent = T("Reported. Thank you.");
    const std::string failed = T("Could not send that report.");
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_busy) return;
        g_busy = true;
    }
    std::thread([url, payload = body.str(), token, sent, failed]() {
        HttpRequest req;
        req.method = "POST";
        req.url = url;
        req.body = payload;
        req.bearer = token;
        req.timeoutMs = 20000;
        const HttpResponse res = httpRequest(req);
        std::lock_guard<std::mutex> g(g_lock);
        g_busy = false;
        g_result = res.ok() ? sent : (res.error.empty() ? failed : res.error);
    }).detach();
}

/**
 * Tell the host.
 *
 * Goes over the existing game connection rather than the account service: the
 * host is right there, and routing a complaint about their game through a
 * third party would be slower and stranger. The host's client decides what to
 * do with it; this only delivers it.
 */
void Game::sendReportToHost() {
    if (!m_netSession && !m_netHost) {
        std::lock_guard<std::mutex> g(g_lock);
        g_result = T("There is no host to tell in a single-player game.");
        return;
    }
    const mail::Box* box = mailboxIfAny(m_playerCountryId);
    const mail::Message* reported = box ? box->find(m_reportMessageId) : nullptr;
    if (!reported) return;

    // The peer, not the country: a country can change hands mid-game, and the
    // host needs to act on the person who wrote it.
    uint16_t about = 0;
    if (m_netSession) {
        for (const NetPeer& p : m_netSession->roster())
            if ((int)p.countryId == m_reportCountry) { about = p.peerId; break; }
    }
    if (about == 0) {
        std::lock_guard<std::mutex> g(g_lock);
        g_result = T("That correspondent is not a player in this game.");
        return;
    }
    if (m_netSession) {
        m_netSession->sendPlayerReport(about, reportReasonId(m_reportReason),
                                       m_reportNote, reported->body);
    }
    std::lock_guard<std::mutex> g(g_lock);
    g_result = T("Sent to the host of this game.");
}

const std::string& Game::moderationResult() {
    std::lock_guard<std::mutex> g(g_lock);
    static std::string copy;
    copy = g_result;
    return copy;
}

/**
 * The pseudonym of whoever is playing a country, or empty.
 *
 * Empty in single player, and empty for a country an advisor speaks for: there
 * is no account behind either, and nothing for the account service to act on.
 * That is what makes "Tell OpenDoctrines" a multiplayer-only button rather than
 * one that fails after being pressed.
 */
std::string Game::mpPsidForCountry(int countryId) const {
    if (!m_netSession) return "";
    for (const NetPeer& p : m_netSession->roster()) {
        if ((int)p.countryId == countryId) return p.psid;
    }
    return "";
}

/// Whether reporting to the account service is possible for this correspondent.
bool Game::canReportToIssuer(int countryId) const {
    return !mpPsidForCountry(countryId).empty() &&
           AccountClient::get().configured() &&
           AccountClient::get().account().valid();
}


/**
 * A label for which game this happened in.
 *
 * Best effort and untrusted at the far end -- it is what the client believes,
 * and a maintainer reading a report treats it as a hint rather than a fact.
 */
std::string Game::mpServerLabel() const {
    if (m_netHost) return "hosted locally";
    if (m_netSession) return m_mpNameField.empty() ? "a multiplayer game" : m_mpNameField;
    return "single player";
}

// ───────────────────────────────────────────────────────── the dialog ────

void Game::drawReportDialog() {
    if (!m_reportOpen) return;

    const Vector2 mouse = getMouse();
    const bool click = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    const Color accent = hexToColor(m_config.accent());

    DrawRectangle(0, 0, m_screenW, m_screenH, Color{6, 7, 11, 238});
    const int w = std::min(660, m_screenW - 80);
    const int h = std::min(520, m_screenH - 80);
    const int x = (m_screenW - w) / 2, y = (m_screenH - h) / 2;
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.02f, 8, Color{16, 18, 24, 252});
    DrawRectangleRoundedLines({(float)x, (float)y, (float)w, (float)h}, 0.02f, 8,
                              Color{110, 78, 78, 220});

    DrawText(T("Report a message"), x + 24, y + 20, 22, Color{234, 170, 170, 255});

    // What is being reported, quoted back. A reporter should be able to see
    // exactly which message they picked before they accuse somebody of it.
    int cy = y + 56;
    if (const mail::Box* box = mailboxIfAny(m_playerCountryId)) {
        if (const mail::Message* m = box->find(m_reportMessageId)) {
            const Rectangle q = {(float)(x + 24), (float)cy, (float)(w - 48), 62};
            DrawRectangleRec(q, Color{12, 13, 18, 255});
            DrawRectangleLinesEx(q, 1, Color{60, 50, 50, 200});
            int fs = 12;
            std::string line = odText::fitToWidth(m->body, (int)q.width - 20, fs, 10);
            DrawText(line.c_str(), (int)q.x + 10, (int)q.y + 10, fs, Color{206, 200, 200, 255});
            std::string who = T("Unknown");
            if (const Country* c = m_countries.getCountry(m_reportCountry)) who = c->name;
            if (m->author == mail::Author::Bot) who += std::string(" · ") + T(mail::botTag());
            DrawText(who.c_str(), (int)q.x + 10, (int)q.y + 40, 11, Color{140, 130, 130, 255});
        }
    }
    cy += 76;

    DrawText(T("What is wrong with it?"), x + 24, cy, 12, Color{150, 156, 176, 255});
    cy += 18;
    {
        int rx = x + 24;
        for (int i = 0; i < 7; ++i) {
            const char* label = T(reportReasonLabel(i));
            const int cw = MeasureText(label, 12) + 20;
            if (rx + cw > x + w - 24) { rx = x + 24; cy += 28; }
            const Rectangle r = {(float)rx, (float)cy, (float)cw, 24};
            const bool on = (m_reportReason == i);
            const bool hov = CheckCollisionPointRec(mouse, r);
            DrawRectangleRounded(r, 0.3f, 6, on ? Color{64, 40, 44, 245}
                                               : (hov ? Color{32, 28, 32, 230} : Color{20, 20, 26, 210}));
            DrawRectangleRoundedLines(r, 0.3f, 6, on ? Color{210, 120, 120, 235} : Color{62, 60, 66, 170});
            DrawText(label, rx + 10, (int)r.y + 6, 12, on ? WHITE : Color{162, 158, 166, 255});
            if (hov && click) { m_reportReason = i; Audio::get().playSfx("click_light", 0.1f); }
            rx += cw + 8;
        }
        cy += 36;
    }

    DrawText(T("Anything you want to add (optional)"), x + 24, cy, 12, Color{150, 156, 176, 255});
    cy += 16;
    {
        const Rectangle box = {(float)(x + 24), (float)cy, (float)(w - 48), 74};
        const bool hov = CheckCollisionPointRec(mouse, box);
        DrawRectangleRec(box, m_reportNoteFocus ? Color{22, 25, 34, 255} : Color{15, 17, 23, 255});
        DrawRectangleLinesEx(box, 1, m_reportNoteFocus ? accent : Color{60, 64, 84, 200});
        if (hov && click) m_reportNoteFocus = true;
        int ty = (int)box.y + 6;
        std::string line;
        for (size_t i = 0; i <= m_reportNote.size(); ++i) {
            const bool end = (i == m_reportNote.size());
            if (!end && m_reportNote[i] != '\n') {
                line += m_reportNote[i];
                if (MeasureText(line.c_str(), 13) < box.width - 20) continue;
            }
            if (ty + 16 < box.y + box.height)
                DrawText(line.c_str(), (int)box.x + 8, ty, 13, WHITE);
            ty += 16; line.clear();
        }
        if (m_reportNoteFocus && (int)(GetTime() * 2) % 2)
            DrawRectangle((int)box.x + 8 + MeasureText(line.c_str(), 13),
                          std::min(ty, (int)(box.y + box.height - 18)), 2, 14, WHITE);
        cy += 86;
    }

    // Sending the surrounding letters is the reporter's choice: it helps
    // whoever judges it, and it also hands over more of a private conversation
    // than the one message they objected to.
    {
        const Rectangle cb = {(float)(x + 24), (float)cy, 18, 18};
        const bool hov = CheckCollisionPointRec(mouse, {cb.x, cb.y, 340, 20});
        DrawRectangleRec(cb, m_reportWithContext ? Color{40, 70, 50, 255} : Color{18, 20, 28, 255});
        DrawRectangleLinesEx(cb, 1, m_reportWithContext ? accent : Color{80, 84, 104, 200});
        if (m_reportWithContext) DrawText("x", (int)cb.x + 5, (int)cb.y + 2, 14, WHITE);
        DrawText(T("Include the messages around it"), (int)cb.x + 26, (int)cb.y + 3, 13,
                 hov ? WHITE : Color{180, 186, 206, 255});
        if (hov && click) { m_reportWithContext = !m_reportWithContext; Audio::get().playSfx("click_light", 0.1f); }
        cy += 28;
    }

    const std::string& said = moderationResult();
    if (!said.empty()) {
        int fs = 12;
        const std::string fit = odText::fitToWidth(said, w - 48, fs, 10);
        DrawText(fit.c_str(), x + 24, cy, fs, Color{180, 200, 180, 255});
    }

    // ── The two destinations, named for what they do ──
    const bool haveHost = (m_netSession != nullptr);
    const bool haveIssuer = canReportToIssuer(m_reportCountry);

    const Rectangle hostBtn = {(float)(x + 24), (float)(y + h - 56), 190, 34};
    const Rectangle issuerBtn = {(float)(x + 24 + 200), (float)(y + h - 56), 220, 34};
    const Rectangle cancel = {(float)(x + w - 24 - 110), (float)(y + h - 56), 110, 34};

    auto button = [&](Rectangle r, const char* label, bool enabled, Color on) {
        const bool hov = CheckCollisionPointRec(mouse, r) && enabled;
        DrawRectangleRounded(r, 0.2f, 6, !enabled ? Color{22, 24, 30, 200}
                                                  : (hov ? on : Color{30, 32, 42, 235}));
        DrawRectangleRoundedLines(r, 0.2f, 6, enabled ? Color{120, 110, 110, 210}
                                                      : Color{54, 56, 66, 170});
        int fs = 13;
        const std::string fit = odText::fitToWidth(label, (int)r.width - 16, fs, 9);
        DrawText(fit.c_str(), (int)r.x + 10, (int)(r.y + r.height / 2 - fs / 2), fs,
                 enabled ? WHITE : Color{104, 106, 120, 255});
        return hov && click;
    };

    if (button(hostBtn, T("Tell the host"), haveHost, Color{60, 52, 40, 245}))
        sendReportToHost();
    if (button(issuerBtn, T("Tell OpenDoctrines"), haveIssuer, Color{62, 42, 46, 245}))
        sendReportToIssuer();
    if (button(cancel, T("Close"), true, Color{44, 46, 60, 240}))
        closeReportDialog();

    // Why a button is unavailable, rather than a dead control with no reason.
    if (!haveHost || !haveIssuer) {
        const char* why = !haveHost && !haveIssuer
            ? T("In a single-player game there is nobody to report to.")
            : (!haveHost ? T("There is no host in this game.")
                         : T("Reporting to OpenDoctrines needs an account and a multiplayer game."));
        DrawText(why, x + 24, y + h - 78, 11, Color{140, 130, 130, 255});
    }
}

void Game::updateReportDialog() {
    if (!m_reportOpen) return;
    if (IsKeyPressed(KEY_ESCAPE)) { closeReportDialog(); return; }
    if (!m_reportNoteFocus) return;

    int key = GetCharPressed();
    while (key > 0) {
        if (key >= 32 && m_reportNote.size() + 4 <= 1000) {
            unsigned cp = (unsigned)key;
            if (cp < 0x80) m_reportNote += (char)cp;
            else if (cp < 0x800) {
                m_reportNote += (char)(0xC0 | (cp >> 6));
                m_reportNote += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                m_reportNote += (char)(0xE0 | (cp >> 12));
                m_reportNote += (char)(0x80 | ((cp >> 6) & 0x3F));
                m_reportNote += (char)(0x80 | (cp & 0x3F));
            }
        }
        key = GetCharPressed();
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (!m_reportNote.empty()) {
            size_t i = m_reportNote.size() - 1;
            while (i > 0 && (unsigned char)m_reportNote[i] >= 0x80 &&
                   (unsigned char)m_reportNote[i] < 0xC0) --i;
            m_reportNote.erase(i);
        }
    }
}

// ────────────────────────────────────────── what the host was told ────
//
// A host sees complaints about their own game and can act on their own game.
// Nothing here reaches the account service, and nothing acts automatically: a
// host who kicks because a stranger asked is a host who can be made to kick
// anybody, so every outcome is a person pressing a button.

void Game::drawHostReports() {
    if (!m_hostReportsOpen) return;

    const Vector2 mouse = getMouse();
    const bool click = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    const Color accent = hexToColor(m_config.accent());

    DrawRectangle(0, 0, m_screenW, m_screenH, Color{6, 7, 11, 236});
    const int w = std::min(880, m_screenW - 80);
    const int h = std::min(620, m_screenH - 80);
    const int x = (m_screenW - w) / 2, y = (m_screenH - h) / 2;
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.02f, 8, Color{16, 18, 24, 252});
    DrawRectangleRoundedLines({(float)x, (float)y, (float)w, (float)h}, 0.02f, 8,
                              Color{70, 74, 96, 220});

    DrawText(T("Reports from your players"), x + 24, y + 20, 22, accent);

    const Rectangle close = {(float)(x + w - 104), (float)(y + 18), 80, 28};
    const bool ch = CheckCollisionPointRec(mouse, close);
    DrawRectangleRounded(close, 0.2f, 6, ch ? Color{44, 46, 60, 240} : Color{26, 28, 38, 220});
    DrawRectangleRoundedLines(close, 0.2f, 6, Color{80, 84, 104, 200});
    DrawText(T("Close"), (int)close.x + 14, (int)close.y + 7, 13, Color{200, 205, 225, 255});
    if (ch && click) {
        m_hostReportsOpen = false;
        m_hostReportUnread = false;
        Audio::get().playSfx("panel_close");
        return;
    }

    if (m_hostReports.empty()) {
        DrawText(T("Nobody has reported anything."), x + 24, y + 70, 14,
                 Color{130, 136, 156, 255});
        return;
    }

    auto peerName = [&](uint16_t id) -> std::string {
        if (m_netHost) {
            // Through the lobby, which is where the host keeps its roster.
            for (const NetPeer& p : m_netHost->lobby().roster())
                if (p.peerId == id) return p.name;
        }
        return TextFormat("player %d", (int)id);
    };

    const Rectangle list = {(float)(x + 24), (float)(y + 62), (float)(w - 48), (float)(h - 100)};
    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    int ry = (int)list.y - m_hostReportScroll;

    for (size_t i = 0; i < m_hostReports.size(); ++i) {
        HostReport& r = m_hostReports[i];
        const int rowH = 108;
        if (ry + rowH > list.y - 20 && ry < list.y + list.height) {
            const Rectangle row = {list.x, (float)ry, list.width, (float)rowH - 8};
            DrawRectangleRec(row, r.dealtWith ? Color{16, 20, 17, 200} : Color{22, 20, 24, 220});
            DrawRectangleLinesEx(row, 1, r.dealtWith ? Color{50, 70, 54, 180}
                                                     : Color{74, 58, 58, 190});

            DrawText(TextFormat(T("%s reported %s  ·  %s  ·  turn %d"),
                                peerName(r.fromPeer).c_str(), peerName(r.aboutPeer).c_str(),
                                T(r.reason.c_str()), r.atTurn),
                     (int)row.x + 12, (int)row.y + 10, 13, Color{224, 200, 200, 255});

            int fs = 12;
            const std::string quoted =
                odText::fitToWidth("\"" + r.message + "\"", (int)row.width - 28, fs, 10);
            DrawText(quoted.c_str(), (int)row.x + 12, (int)row.y + 32, fs,
                     Color{198, 194, 200, 255});
            if (!r.note.empty()) {
                int nfs = 11;
                const std::string note =
                    odText::fitToWidth(r.note, (int)row.width - 28, nfs, 9);
                DrawText(note.c_str(), (int)row.x + 12, (int)row.y + 52, nfs,
                         Color{146, 142, 150, 255});
            }

            if (r.dealtWith) {
                DrawText(T("dealt with"), (int)row.x + 12, (int)row.y + 76, 11,
                         Color{140, 190, 150, 255});
            } else {
                // Kick, ban from this game, or decide it needs nothing. The
                // third is a real outcome and is offered as plainly as the
                // other two.
                struct Act { const char* label; int kind; Color on; };
                const Act acts[] = {
                    {T("Kick"),          0, Color{72, 56, 40, 245}},
                    {T("Ban from game"), 1, Color{74, 42, 44, 245}},
                    {T("Nothing to do"), 2, Color{40, 48, 60, 245}},
                };
                int bx = (int)row.x + 12;
                for (const Act& a : acts) {
                    const int bw = MeasureText(a.label, 12) + 20;
                    const Rectangle b = {(float)bx, (float)(row.y + 72), (float)bw, 24};
                    const bool bh = CheckCollisionPointRec(mouse, b) &&
                                    CheckCollisionPointRec(mouse, list);
                    DrawRectangleRounded(b, 0.25f, 6, bh ? a.on : Color{26, 26, 34, 220});
                    DrawRectangleRoundedLines(b, 0.25f, 6, Color{80, 76, 84, 190});
                    DrawText(a.label, bx + 10, (int)b.y + 6, 12,
                             bh ? WHITE : Color{176, 172, 180, 255});
                    if (bh && click) {
                        if (a.kind == 0 && m_netHost) m_netHost->kick(r.aboutPeer, "Reported.");
                        if (a.kind == 1 && m_netHost) {
                            m_netHost->kick(r.aboutPeer, "Removed by the host.");
                            m_hostBanned.insert(peerName(r.aboutPeer));
                        }
                        r.dealtWith = true;
                        Audio::get().playSfx(a.kind == 2 ? "back" : "confirm");
                    }
                    bx += bw + 8;
                }
            }
        }
        ry += rowH;
    }
    EndScissorMode();

    if (CheckCollisionPointRec(mouse, list)) {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            const int maxScroll = std::max(0, ry - (int)list.y - (int)list.height + 20 +
                                                m_hostReportScroll);
            m_hostReportScroll = std::clamp(m_hostReportScroll - (int)(wheel * 45), 0, maxScroll);
        }
    }
}

void Game::updateHostReports() {
    if (!m_hostReportsOpen) return;
    if (IsKeyPressed(KEY_ESCAPE)) {
        m_hostReportsOpen = false;
        m_hostReportUnread = false;
        Audio::get().playSfx("panel_close");
    }
}
