// The review queue, for whoever holds the developer badge.
//
// WHY IT IS A GAME SCREEN AND NOT A CURL COMMAND
//
// Because moderation that requires a terminal happens late, or not at all. The
// person who can act on a report is usually the person who was just playing,
// and a queue two clicks from the main menu gets read.
//
// WHAT THIS SCREEN CAN DO, AND WHAT GUARDS IT
//
// It can ban an account, anywhere, permanently. The only thing standing in
// front of that is the `developer` badge on the signed-in account, checked BY
// THE SERVICE on every request -- this screen holds no secret, and a copy of
// the game handed to somebody else is a copy that gets a 404 from both routes.
// The menu entry is hidden without the badge, but that is a convenience: the
// hiding is not the security, the service is.
//
// EVERY OUTCOME IS A PERSON PRESSING A BUTTON. Nothing here decides anything on
// its own, and "nothing to do" is offered as plainly as the two punishments,
// because a queue whose only clearing action is a punishment is a queue that
// punishes people.

#include "Game.h"
#include "GameInternals.h"
#include "Audio.h"
#include "net/AccountClient.h"
#include "net/HttpClient.h"
#include "i18n/Locale.h"
#include "i18n/Text.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

namespace {

std::mutex g_lock;
std::string g_payload;       ///< the queue as fetched, parsed on the game thread
std::string g_status;        ///< what to tell the reader
bool        g_busy = false;
bool        g_fresh = false; ///< a payload arrived and has not been parsed yet

}  // namespace

bool Game::isDeveloper() const {
    const AccountClient& a = AccountClient::get();
    return a.configured() && a.account().valid() && a.account().hasBadge("developer");
}

/// Percent-encode a query value. A nickname may contain anything a nickname
/// may contain, and a raw one in a query string is a broken URL at best.
static std::string urlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : in) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 0x0F]; }
    }
    return out;
}

/// Base URL of the account service, without a trailing slash.
static std::string issuerBase(const std::string& raw) {
    std::string url = raw;
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

void Game::openDevReports() {
    m_devReportsOpen = true;
    m_devReportScroll = 0;
    m_devReportSelected = -1;
    fetchDevReports();
    Audio::get().playSfx("panel_open");
}

void Game::closeDevReports() {
    m_devReportsOpen = false;
    Audio::get().playSfx("panel_close");
}

void Game::fetchDevReports() {
    const AccountClient& account = AccountClient::get();
    if (!account.account().valid()) return;
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_busy) return;
        g_busy = true;
        g_status = T("Fetching...");
    }
    const std::string url = issuerBase(m_config.accountIssuer) + "/moderation/reports";
    const std::string token = account.sessionToken();
    // Resolved on the game thread: the arena behind T() is not locked.
    const std::string failed = T("Could not reach the service.");
    const std::string refused = T("This account is not a moderator.");

    std::thread([url, token, failed, refused]() {
        HttpRequest req;
        req.method = "GET";
        req.url = url;
        req.bearer = token;
        req.timeoutMs = 20000;
        const HttpResponse res = httpRequest(req);

        std::lock_guard<std::mutex> g(g_lock);
        g_busy = false;
        if (res.ok()) {
            g_payload = res.body;
            g_fresh = true;
            g_status.clear();
        } else {
            // 404 is what the service answers somebody without the badge -- it
            // does not confirm the route exists. Reported as what it means here,
            // where the reader is the one account that should have it.
            g_status = (res.status == 404) ? refused : failed;
        }
    }).detach();
}

/**
 * Read a duration a person typed: "90m", "36h", "3d", "2w", "1d 12h", "45".
 *
 * Returns days as a fraction, or 0 when nothing usable was written. A bare
 * number means DAYS, because that is what the field is labelled and guessing
 * otherwise would silently turn "3" into three minutes.
 *
 * Deliberately forgiving about spelling -- "2 weeks", "2w" and "2 w" are the
 * same thing -- because this is a field somebody types into while annoyed.
 */
double Game::parseTimeoutDays(const std::string& text) {
    double totalDays = 0.0;
    bool sawAny = false;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && !isdigit((unsigned char)text[i]) && text[i] != '.') ++i;
        if (i >= text.size()) break;

        size_t j = i;
        while (j < text.size() && (isdigit((unsigned char)text[j]) || text[j] == '.')) ++j;
        const double value = atof(text.substr(i, j - i).c_str());
        i = j;

        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        const char unit = (i < text.size()) ? (char)tolower((unsigned char)text[i]) : 'd';
        while (i < text.size() && isalpha((unsigned char)text[i])) ++i;

        double days = 0.0;
        switch (unit) {
            case 'm': days = value / 1440.0; break;   // minutes
            case 'h': days = value / 24.0;   break;
            case 'w': days = value * 7.0;    break;
            case 'y': days = value * 365.0;  break;
            default:  days = value;          break;   // days, including a bare number
        }
        if (value > 0.0) { totalDays += days; sawAny = true; }
    }
    return sawAny ? totalDays : 0.0;
}

/// The same duration said back, so the button shows what was understood.
std::string Game::describeTimeout(double days) {
    const long long minutes = (long long)(days * 1440.0 + 0.5);
    if (minutes < 1) return "";
    if (minutes < 60) return TextFormat("%lld min", minutes);
    if (minutes < 1440) {
        const double hours = (double)minutes / 60.0;
        return TextFormat(hours == (long long)hours ? "%.0f h" : "%.1f h", hours);
    }
    const double d = (double)minutes / 1440.0;
    return TextFormat(d == (long long)d ? "%.0f days" : "%.1f days", d);
}

/// The lengths offered, and what each is called.
const Game::TimeoutChoice Game::kTimeoutChoices[] = {
    {"1 day",     1},
    {"3 days",    3},
    {"1 week",    7},
    {"2 weeks",  14},
    {"1 month",  30},
    {"3 months", 90},
    {"1 year",  365},
};
const int Game::kTimeoutChoiceCount =
    (int)(sizeof(Game::kTimeoutChoices) / sizeof(Game::kTimeoutChoices[0]));

/**
 * Act on one, and take it off the list.
 *
 * The reason travels with a ban because the account is shown it. The length of
 * a timeout is chosen on the screen rather than fixed: "a week" is the common
 * answer but not the only right one, and a maintainer who can only pick the
 * default ends up banning permanently for things that wanted three days.
 */
void Game::decideDevReport(const std::string& id, const char* action, double days) {
    const AccountClient& account = AccountClient::get();
    if (!account.account().valid()) return;

    nlohmann::json body;
    body["id"] = id;
    body["action"] = action;
    if (days > 0.0) body["days"] = days;   // fractional: 1/24 is an hour
    body["reason"] = "Conduct towards another player.";

    const std::string url = issuerBase(m_config.accountIssuer) + "/moderation/decide";
    const std::string token = account.sessionToken();
    const std::string done = T("Done.");
    const std::string failed = T("That did not go through.");
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_status = T("Working...");
    }
    std::thread([url, token, payload = body.dump(), done, failed]() {
        HttpRequest req;
        req.method = "POST";
        req.url = url;
        req.body = payload;
        req.bearer = token;
        req.timeoutMs = 20000;
        const HttpResponse res = httpRequest(req);
        std::lock_guard<std::mutex> g(g_lock);
        g_status = res.ok() ? done : (res.error.empty() ? failed : res.error);
    }).detach();
    m_devReportRefetch = GetTime() + 1.2;   // let the write land, then re-read
}

/// Take whatever the worker fetched and turn it into rows. Game thread only.
void Game::parseDevReports() {
    std::string payload;
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (!g_fresh) return;
        g_fresh = false;
        payload = g_payload;
    }
    m_devReports.clear();
    try {
        const nlohmann::json j = nlohmann::json::parse(payload);
        for (const auto& e : j.value("reports", nlohmann::json::array())) {
            DevReport r;
            r.id = e.value("id", std::string());
            r.at = e.value("at", 0LL);
            r.reporter = e.value("reporterNick", std::string());
            r.accused = e.value("accusedNick", std::string());
            r.reason = e.value("reason", std::string());
            r.note = e.value("note", std::string());
            r.message = e.value("message", std::string());
            r.server = e.value("server", std::string());
            r.status = e.value("status", std::string("open"));
            r.outcome = e.value("outcome", std::string());
            for (const auto& c : e.value("context", nlohmann::json::array()))
                r.context.push_back(c.get<std::string>());
            if (!r.id.empty()) m_devReports.push_back(std::move(r));
        }
    } catch (...) {
        // A queue that will not parse is reported, not crashed on. The service
        // is the only thing that writes it, but "the only thing that writes it"
        // has been wrong before.
        std::lock_guard<std::mutex> g(g_lock);
        g_status = T("The service sent something this build could not read.");
    }
}

void Game::updateDevReports() {
    if (m_adminTab == AdminTab::Announcements) { updateAdminAnnouncements(); return; }
    if (!m_devReportsOpen) return;
    parseDevReports();
    parseProfile();
    if (m_devReportRefetch > 0 && GetTime() > m_devReportRefetch) {
        m_devReportRefetch = 0;
        fetchDevReports();
    }
    if (m_devLookupFocus) {
        int key = GetCharPressed();
        while (key > 0) {
            if (key >= 32 && key < 127 && m_devLookupText.size() < 64)
                m_devLookupText += (char)key;
            key = GetCharPressed();
        }
        if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) &&
            !m_devLookupText.empty()) {
            m_devLookupText.pop_back();
        }
        if (IsKeyPressed(KEY_ENTER)) { m_devLookupFocus = false; lookUpAccount(); }
    }

    // Typing a duration, when the field has focus.
    if (m_devTimeoutFocus) {
        int key = GetCharPressed();
        while (key > 0) {
            // Digits, a decimal point, a space and the unit letters. Nothing
            // else can mean anything here, and refusing the rest keeps the
            // field from becoming a place to paste a paragraph into.
            const char c = (char)key;
            if (m_devTimeoutText.size() < 24 &&
                (isdigit((unsigned char)c) || c == '.' || c == ' ' ||
                 strchr("mhdwyMHDWY", c) != nullptr)) {
                m_devTimeoutText += c;
            }
            key = GetCharPressed();
        }
        if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) &&
            !m_devTimeoutText.empty()) {
            m_devTimeoutText.pop_back();
        }
        if (IsKeyPressed(KEY_ENTER)) m_devTimeoutFocus = false;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        if (m_devTimeoutFocus) { m_devTimeoutFocus = false; return; }
        if (m_devLookupFocus) { m_devLookupFocus = false; return; }
        closeDevReports();
    }
}

void Game::drawDevReports() {
    if (!m_devReportsOpen) return;

    const Vector2 mouse = getMouse();
    const bool click = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    const Color accent = hexToColor(m_config.accent());

    DrawRectangle(0, 0, m_screenW, m_screenH, Color{6, 7, 11, 244});
    const int w = std::min(940, m_screenW - 60);
    const int h = std::min(680, m_screenH - 60);
    const int x = (m_screenW - w) / 2, y = (m_screenH - h) / 2;
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.02f, 8, Color{16, 18, 24, 252});
    DrawRectangleRoundedLines({(float)x, (float)y, (float)w, (float)h}, 0.02f, 8,
                              Color{70, 74, 96, 220});

    DrawText(T("Admin"), x + 24, y + 20, 24, accent);

    // ── TWO TABS BEHIND ONE MENU ENTRY ──
    //
    // The reports queue and the announcement board are the same job -- looking
    // after the game in front of other people -- and they were one menu slot
    // and no slot respectively. A tab strip costs nothing and stops the main
    // menu growing an entry every time something needs a screen.
    {
        const char* names[] = {T("Reports"), T("Announcements")};
        int tx = x + 24 + MeasureText(T("Admin"), 24) + 28;
        for (int i = 0; i < 2; ++i) {
            const int tw = MeasureText(names[i], 13);
            const Rectangle tab = {(float)tx, (float)(y + 24), (float)(tw + 20), 24};
            const bool on = ((int)m_adminTab == i);
            const bool hov = CheckCollisionPointRec(mouse, tab);
            DrawText(names[i], tx + 10, y + 29, 13,
                     on ? WHITE : (hov ? Color{206, 212, 232, 255} : Color{130, 136, 158, 255}));
            if (on) DrawRectangle(tx + 10, y + 45, tw, 2, accent);
            if (hov && click) {
                m_adminTab = (AdminTab)i;
                // Asked when the tab is opened, not on a timer: the board
                // changes when somebody changes it, which is right here.
                if (m_adminTab == AdminTab::Announcements) fetchAdminAnnouncements();
                Audio::get().playSfx("click_light");
            }
            tx += tw + 26;
        }
    }

    if (m_adminTab == AdminTab::Announcements) {
        drawAdminAnnouncements(x + 24, y + 62, w - 48, h - 100, mouse, click);
        // The close button is drawn by the shared code below; everything else
        // on this panel belongs to the reports queue.
        const Rectangle closeOnly = {(float)(x + w - 104), (float)(y + 18), 80, 28};
        const bool ch = CheckCollisionPointRec(mouse, closeOnly);
        DrawRectangleRounded(closeOnly, 0.2f, 6, ch ? Color{60, 40, 44, 240} : Color{28, 30, 42, 220});
        DrawRectangleRoundedLines(closeOnly, 0.2f, 6, Color{110, 96, 100, 200});
        DrawText(T("Close"), (int)closeOnly.x + 20, (int)closeOnly.y + 8, 13, WHITE);
        if (ch && click) closeDevReports();
        return;
    }

    std::string status;
    { std::lock_guard<std::mutex> g(g_lock); status = g_status; }
    if (!status.empty()) {
        const int tw = MeasureText(status.c_str(), 12);
        DrawText(status.c_str(), x + w - 24 - 190 - tw - 12, y + 28, 12,
                 Color{170, 176, 196, 255});
    }

    const Rectangle refresh = {(float)(x + w - 24 - 186), (float)(y + 18), 88, 28};
    const Rectangle close = {(float)(x + w - 104), (float)(y + 18), 80, 28};
    auto smallBtn = [&](Rectangle r, const char* label) {
        const bool hov = CheckCollisionPointRec(mouse, r);
        DrawRectangleRounded(r, 0.2f, 6, hov ? Color{44, 46, 60, 240} : Color{26, 28, 38, 220});
        DrawRectangleRoundedLines(r, 0.2f, 6, Color{80, 84, 104, 200});
        DrawText(label, (int)r.x + 12, (int)r.y + 7, 13, Color{200, 205, 225, 255});
        return hov && click;
    };
    if (smallBtn(refresh, T("Refresh"))) fetchDevReports();
    if (smallBtn(close, T("Close"))) { closeDevReports(); return; }

    // ── Open / Solved / Look somebody up ──
    //
    // A decided report moves out of the way rather than being deleted: it is
    // the record of a decision, and that history is most of what makes the next
    // decision easier to judge.
    int openCount = 0, solvedCount = 0;
    for (const DevReport& r : m_devReports) (r.status == "open" ? openCount : solvedCount)++;
    {
        struct Tab { const char* label; ReportTab which; int count; };
        const Tab tabs[] = {
            {T("Open"),   ReportTab::Open,   openCount},
            {T("Solved"), ReportTab::Solved, solvedCount},
            {T("Look somebody up"), ReportTab::Lookup, -1},
        };
        int tx = x + 24;
        for (const Tab& t : tabs) {
            const char* label = t.count >= 0 ? TextFormat("%s (%d)", t.label, t.count) : t.label;
            const int tw = MeasureText(label, 13) + 22;
            const Rectangle r = {(float)tx, (float)(y + 56), (float)tw, 26};
            const bool on = (m_devTab == t.which);
            const bool hov = CheckCollisionPointRec(mouse, r);
            DrawRectangleRounded(r, 0.25f, 6, on ? Color{38, 44, 58, 245}
                                                 : (hov ? Color{28, 30, 40, 230}
                                                        : Color{18, 20, 26, 200}));
            DrawRectangleRoundedLines(r, 0.25f, 6, on ? accent : Color{58, 60, 76, 170});
            DrawText(label, tx + 11, (int)r.y + 7, 13,
                     on ? WHITE : Color{156, 162, 182, 255});
            if (hov && click) { m_devTab = t.which; m_devReportScroll = 0; }
            tx += tw + 8;
        }
    }

    if (m_devTab == ReportTab::Lookup) {
        drawLookupTab(x, y, w, h, mouse, click, accent);
        return;
    }

    const bool wantOpen = (m_devTab == ReportTab::Open);
    if ((wantOpen ? openCount : solvedCount) == 0) {
        DrawText(status.empty()
                     ? (wantOpen ? T("Nothing is waiting.") : T("Nothing has been decided yet."))
                     : status.c_str(),
                 x + 24, y + 100, 14, Color{130, 136, 156, 255});
        return;
    }

    const Rectangle list = {(float)(x + 24), (float)(y + 92), (float)(w - 48), (float)(h - 128)};
    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    int ry = (int)list.y - m_devReportScroll;

    for (size_t i = 0; i < m_devReports.size(); ++i) {
        const DevReport& r = m_devReports[i];
        const bool open = (r.status == "open");
        if (open != wantOpen) continue;      // this tab holds the other kind
        const bool selected = ((int)i == m_devReportSelected);
        const int rowH = selected
                             ? (166 + (int)std::min<size_t>(r.context.size(), 14) * 16)
                             : 92;

        if (ry + rowH > list.y - 20 && ry < list.y + list.height) {
            const Rectangle row = {list.x, (float)ry, list.width, (float)(rowH - 8)};
            DrawRectangleRec(row, open ? Color{22, 20, 24, 225} : Color{16, 20, 17, 205});
            DrawRectangleLinesEx(row, 1, open ? Color{74, 58, 58, 195} : Color{50, 70, 54, 180});

            DrawText(TextFormat("%s  ·  reported by %s  ·  %s",
                                r.accused.c_str(), r.reporter.c_str(), r.reason.c_str()),
                     (int)row.x + 12, (int)row.y + 10, 14, Color{228, 204, 204, 255});
            if (!r.server.empty()) {
                const char* where = TextFormat(T("in %s"), r.server.c_str());
                DrawText(where, (int)(row.x + row.width - MeasureText(where, 11) - 12),
                         (int)row.y + 12, 11, Color{130, 126, 136, 255});
            }

            int fs = 13;
            const std::string quoted =
                odText::fitToWidth("\"" + r.message + "\"", (int)row.width - 28, fs, 10);
            DrawText(quoted.c_str(), (int)row.x + 12, (int)row.y + 34, fs,
                     Color{206, 202, 208, 255});
            if (!r.note.empty()) {
                int nfs = 11;
                const std::string note = odText::fitToWidth(r.note, (int)row.width - 28, nfs, 9);
                DrawText(note.c_str(), (int)row.x + 12, (int)row.y + 54, nfs,
                         Color{150, 146, 154, 255});
            }

            if (!open) {
                DrawText(TextFormat(T("%s — %s"), r.status.c_str(), r.outcome.c_str()),
                         (int)row.x + 12, (int)row.y + 72, 12, Color{140, 190, 150, 255});
            } else if (!selected) {
                const Rectangle look = {row.x + 12, row.y + 68, 150, 20};
                const bool lh = CheckCollisionPointRec(mouse, look) &&
                                CheckCollisionPointRec(mouse, list);
                DrawText(T("Look at this one"), (int)look.x, (int)look.y + 4, 12,
                         lh ? WHITE : Color{160, 170, 200, 255});
                if (lh && click) { m_devReportSelected = (int)i; }
            } else {
                // The surrounding conversation the reporter chose to attach.
                int cy2 = (int)row.y + 70;
                if (!r.context.empty()) {
                    DrawText(TextFormat(T("Around it (%d lines):"), (int)r.context.size()),
                             (int)row.x + 12, cy2, 11,
                             Color{130, 136, 156, 255});
                    cy2 += 15;
                    for (size_t k = 0; k < std::min<size_t>(r.context.size(), 14); ++k) {
                        int cfs = 11;
                        const std::string line =
                            odText::fitToWidth(r.context[k], (int)row.width - 40, cfs, 9);
                        DrawText(line.c_str(), (int)row.x + 24, cy2, cfs,
                                 Color{138, 134, 142, 255});
                        cy2 += 16;
                    }
                }

                // How long, chosen before the button that uses it. Drawn above
                // the actions so the length is picked first and the press is
                // the last thing that happens.
                DrawText(T("Timeout length:"), (int)row.x + 12, cy2 + 6, 11,
                         Color{130, 136, 156, 255});
                {
                    int dx = (int)row.x + 12 + MeasureText(T("Timeout length:"), 11) + 10;
                    for (int k = 0; k < kTimeoutChoiceCount; ++k) {
                        const char* label = T(kTimeoutChoices[k].label);
                        const int dw = MeasureText(label, 11) + 14;
                        const Rectangle d = {(float)dx, (float)(cy2 + 2), (float)dw, 19};
                        const bool on = (m_devTimeoutChoice == k);
                        const bool dh = CheckCollisionPointRec(mouse, d) &&
                                        CheckCollisionPointRec(mouse, list);
                        DrawRectangleRounded(d, 0.35f, 6, on ? Color{62, 52, 36, 245}
                                                            : (dh ? Color{32, 32, 40, 230}
                                                                  : Color{20, 20, 26, 200}));
                        DrawRectangleRoundedLines(d, 0.35f, 6,
                                                  on ? Color{212, 176, 108, 235}
                                                     : Color{62, 60, 66, 165});
                        DrawText(label, dx + 7, (int)d.y + 4, 11,
                                 on ? WHITE : Color{158, 154, 162, 255});
                        if (dh && click) {
                            m_devTimeoutChoice = k;
                            // A chip WRITES INTO the field rather than being a
                            // separate setting: one place holds the answer, so
                            // what the button uses is always what is displayed.
                            m_devTimeoutText = TextFormat("%dd", kTimeoutChoices[k].days);
                            m_devTimeoutFocus = false;
                        }
                        dx += dw + 5;
                    }
                    // ...or type one, for the times a chip does not cover.
                    const Rectangle f = {(float)dx + 6, (float)(cy2 + 1), 96, 21};
                    const bool fh = CheckCollisionPointRec(mouse, f) &&
                                    CheckCollisionPointRec(mouse, list);
                    DrawRectangleRec(f, m_devTimeoutFocus ? Color{24, 26, 34, 255}
                                                          : Color{15, 17, 23, 255});
                    DrawRectangleLinesEx(f, 1, m_devTimeoutFocus ? accent
                                                                 : Color{62, 60, 66, 175});
                    const std::string shown = m_devTimeoutText.empty() && !m_devTimeoutFocus
                                            ? std::string(T("or type: 36h"))
                                            : m_devTimeoutText;
                    DrawText(shown.c_str(), (int)f.x + 6, (int)f.y + 5, 11,
                             m_devTimeoutText.empty() && !m_devTimeoutFocus
                                 ? Color{92, 94, 108, 255} : WHITE);
                    if (m_devTimeoutFocus && (int)(GetTime() * 2) % 2)
                        DrawRectangle((int)f.x + 6 + MeasureText(m_devTimeoutText.c_str(), 11),
                                      (int)f.y + 4, 2, 13, WHITE);
                    if (fh && click) { m_devTimeoutFocus = true; m_devTimeoutChoice = -1; }
                    cy2 += 24;
                }

                // ONE source of truth: whatever is in the field. A chip fills
                // it, so there is never a chip saying one thing and a field
                // saying another.
                const double chosenDays = parseTimeoutDays(m_devTimeoutText);
                const std::string said = describeTimeout(chosenDays);

                struct Act { const char* label; const char* action; double days; Color on;
                             bool live; };
                const Act acts[] = {
                    {T("Ban for good"), "ban", 0.0, Color{80, 42, 44, 245}, true},
                    // Named with the duration it will actually apply, and dead
                    // when nothing usable was typed -- a button that silently
                    // falls back to a default is how somebody gets a week when
                    // they meant an hour.
                    {said.empty() ? T("Time out (enter a length)")
                                  : TextFormat(T("Time out for %s"), said.c_str()),
                     "timeout", chosenDays, Color{72, 58, 40, 245}, chosenDays > 0.0},
                    {T("Nothing to do"), "dismiss", 0.0, Color{40, 48, 60, 245}, true},
                };
                int bx = (int)row.x + 12;
                for (const Act& a : acts) {
                    const int bw = MeasureText(a.label, 12) + 20;
                    const Rectangle b = {(float)bx, (float)(cy2 + 6), (float)bw, 26};
                    const bool bh = a.live && CheckCollisionPointRec(mouse, b) &&
                                    CheckCollisionPointRec(mouse, list);
                    DrawRectangleRounded(b, 0.25f, 6, !a.live ? Color{20, 20, 26, 190}
                                                              : (bh ? a.on : Color{26, 26, 34, 220}));
                    DrawRectangleRoundedLines(b, 0.25f, 6, a.live ? Color{86, 80, 88, 195}
                                                                  : Color{50, 48, 54, 160});
                    DrawText(a.label, bx + 10, (int)b.y + 7, 12,
                             !a.live ? Color{96, 94, 102, 255}
                                     : (bh ? WHITE : Color{180, 176, 184, 255}));
                    if (bh && click) {
                        decideDevReport(r.id, a.action, a.days);
                        m_devReportSelected = -1;
                        Audio::get().playSfx(a.days || a.action[0] == 'b' ? "confirm" : "back");
                        EndScissorMode();
                        return;
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
                                                m_devReportScroll);
            m_devReportScroll = std::clamp(m_devReportScroll - (int)(wheel * 45), 0, maxScroll);
        }
    }
}

// ─────────────────────────────────────── looking somebody up ────
//
// The queue answers "what has been reported". This answers the other question a
// maintainer has, which is "who is this person and what have they done" -- and
// it is the only way to act on somebody you saw yourself, with no report behind
// it. Pardoning lives here too, because a punishment that turns out to have
// been wrong should be liftable in one action rather than waited out.

namespace {
std::mutex g_profileLock;
std::string g_profileJson;
bool g_profileFresh = false;
}  // namespace

void Game::lookUpAccount() {
    const AccountClient& account = AccountClient::get();
    if (!account.account().valid() || m_devLookupText.empty()) return;

    m_devLookupPending = true;
    m_devProfile = DevProfile{};
    const std::string url = issuerBase(m_config.accountIssuer) +
                            "/moderation/account?q=" + urlEncode(m_devLookupText);
    const std::string token = account.sessionToken();
    const std::string missing = T("Nobody by that name or id.");
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_status = T("Looking...");
    }
    std::thread([url, token, missing]() {
        HttpRequest req;
        req.method = "GET";
        req.url = url;
        req.bearer = token;
        req.timeoutMs = 20000;
        const HttpResponse res = httpRequest(req);
        {
            std::lock_guard<std::mutex> g(g_profileLock);
            g_profileJson = res.ok() ? res.body : "";
            g_profileFresh = true;
        }
        std::lock_guard<std::mutex> g(g_lock);
        g_status = res.ok() ? "" : missing;
    }).detach();
}

void Game::actOnAccount(const char* action, double days) {
    const AccountClient& account = AccountClient::get();
    if (!account.account().valid() || m_devProfile.id.empty()) return;

    nlohmann::json body;
    // By id rather than the nickname that was typed: a nickname can be changed
    // between the lookup and the button, and acting on a stale name would act
    // on whoever holds it now.
    body["q"] = m_devProfile.id;
    body["action"] = action;
    if (days > 0.0) body["days"] = days;
    body["reason"] = "Conduct towards other players.";

    const std::string url = issuerBase(m_config.accountIssuer) + "/moderation/account";
    const std::string token = account.sessionToken();
    const std::string done = T("Done.");
    const std::string failed = T("That did not go through.");
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_status = T("Working...");
    }
    std::thread([url, token, payload = body.dump(), done, failed]() {
        HttpRequest req;
        req.method = "POST";
        req.url = url;
        req.body = payload;
        req.bearer = token;
        req.timeoutMs = 20000;
        const HttpResponse res = httpRequest(req);
        {
            // The reply carries the profile as it now stands, so the screen
            // shows the result of the action rather than what it looked like
            // before it.
            std::lock_guard<std::mutex> g(g_profileLock);
            if (res.ok()) { g_profileJson = res.body; g_profileFresh = true; }
        }
        std::lock_guard<std::mutex> g(g_lock);
        g_status = res.ok() ? done : (res.error.empty() ? failed : res.error);
    }).detach();
}

void Game::parseProfile() {
    std::string payload;
    {
        std::lock_guard<std::mutex> g(g_profileLock);
        if (!g_profileFresh) return;
        g_profileFresh = false;
        payload = g_profileJson;
    }
    m_devLookupPending = false;
    m_devProfile = DevProfile{};
    if (payload.empty()) return;

    try {
        const nlohmann::json j = nlohmann::json::parse(payload);
        const auto& p = j.contains("profile") ? j["profile"] : j;
        DevProfile out;
        out.id = p.value("id", std::string());
        out.nickname = p.value("nickname", std::string());
        out.created = p.value("created", 0LL);
        out.linkedCount = p.value("linkedCount", 0);
        out.banned = p.value("banned", false);
        out.banReason = p.value("banReason", std::string());
        out.bannedUntil = p.value("bannedUntil", 0LL);
        out.bannedAt = p.value("bannedAt", 0LL);
        for (const auto& b : p.value("badges", nlohmann::json::array()))
            out.badges.push_back(b.get<std::string>());

        auto readList = [](const nlohmann::json& arr) {
            std::vector<DevReport> v;
            for (const auto& e : arr) {
                DevReport r;
                r.id = e.value("id", std::string());
                r.at = e.value("at", 0LL);
                r.reporter = e.value("reporterNick", std::string());
                r.accused = e.value("accusedNick", std::string());
                r.reason = e.value("reason", std::string());
                r.message = e.value("message", std::string());
                r.status = e.value("status", std::string("open"));
                r.outcome = e.value("outcome", std::string());
                v.push_back(std::move(r));
            }
            return v;
        };
        out.against = readList(p.value("against", nlohmann::json::array()));
        out.filed = readList(p.value("filed", nlohmann::json::array()));
        out.valid = !out.id.empty();
        m_devProfile = std::move(out);
    } catch (...) {
        std::lock_guard<std::mutex> g(g_lock);
        g_status = T("The service sent something this build could not read.");
    }
}

/**
 * One person: who they are, what is in force, and what they have been part of.
 *
 * Both directions of the history are shown. A pattern of reports AGAINST
 * somebody is the obvious signal; a pattern of reports FILED by them is the
 * less obvious one, and it is how a maintainer notices that the queue is being
 * used as a weapon.
 */
void Game::drawLookupTab(int x, int y, int w, int h, Vector2 mouse, bool click, Color accent) {
    int cy = y + 96;

    // ── Who to look for ──
    DrawText(T("Nickname or account id"), x + 24, cy, 12, Color{150, 156, 176, 255});
    cy += 18;
    {
        const Rectangle f = {(float)(x + 24), (float)cy, 320, 28};
        const bool fh = CheckCollisionPointRec(mouse, f);
        DrawRectangleRec(f, m_devLookupFocus ? Color{24, 26, 34, 255} : Color{15, 17, 23, 255});
        DrawRectangleLinesEx(f, 1, m_devLookupFocus ? accent : Color{62, 60, 66, 180});
        const std::string shown = m_devLookupText.empty() && !m_devLookupFocus
                                ? std::string(T("click to type")) : m_devLookupText;
        DrawText(shown.c_str(), (int)f.x + 8, (int)f.y + 8, 13,
                 m_devLookupText.empty() && !m_devLookupFocus ? Color{92, 94, 108, 255} : WHITE);
        if (m_devLookupFocus && (int)(GetTime() * 2) % 2)
            DrawRectangle((int)f.x + 8 + MeasureText(m_devLookupText.c_str(), 13),
                          (int)f.y + 7, 2, 15, WHITE);
        if (fh && click) m_devLookupFocus = true;

        const Rectangle go = {f.x + f.width + 8, f.y, 90, 28};
        const bool gh = CheckCollisionPointRec(mouse, go) && !m_devLookupText.empty();
        DrawRectangleRounded(go, 0.2f, 6, gh ? Color{46, 62, 84, 245} : Color{26, 30, 40, 225});
        DrawRectangleRoundedLines(go, 0.2f, 6, Color{80, 90, 116, 200});
        DrawText(T("Look up"), (int)go.x + 12, (int)go.y + 8, 13,
                 m_devLookupText.empty() ? Color{104, 106, 120, 255} : WHITE);
        if (gh && click) lookUpAccount();
        cy += 40;
    }

    if (!m_devProfile.valid) {
        DrawText(m_devLookupPending ? T("Looking...")
                                    : T("Type a nickname or an account id and press Look up."),
                 x + 24, cy, 13, Color{124, 130, 150, 255});
        return;
    }

    const DevProfile& p = m_devProfile;

    // ── Who they are, and what is in force ──
    DrawText(p.nickname.c_str(), x + 24, cy, 20, WHITE);
    {
        const int nw = MeasureText(p.nickname.c_str(), 20);
        for (size_t i = 0; i < p.badges.size(); ++i) {
            const char* b = p.badges[i].c_str();
            const int bw = MeasureText(b, 10) + 12;
            DrawRectangleRounded({(float)(x + 34 + nw + (int)i * (bw + 6)), (float)cy + 4,
                                  (float)bw, 16}, 0.4f, 6, Color{40, 52, 64, 235});
            DrawText(b, x + 40 + nw + (int)i * (bw + 6), cy + 7, 10, Color{170, 200, 226, 255});
        }
    }
    cy += 26;
    DrawText(TextFormat(T("account %s  ·  %d provider(s) linked"),
                        p.id.c_str(), p.linkedCount),
             x + 24, cy, 11, Color{124, 130, 150, 255});
    cy += 22;

    {
        // The status line, and it is the first thing a maintainer looks for.
        const bool forever = p.banned && p.bannedUntil == 0;
        std::string state;
        if (!p.banned) state = T("Not banned.");
        else if (forever) state = T("BANNED, permanently.");
        else {
            const long long left = p.bannedUntil - (long long)time(nullptr);
            state = left > 0
                  ? std::string(T("BANNED. ")) + describeTimeout((double)left / 86400.0) +
                        std::string(T(" left."))
                  : std::string(T("Ban has expired."));
        }
        const Color col = !p.banned ? Color{140, 190, 150, 255}
                                    : Color{236, 150, 150, 255};
        DrawText(state.c_str(), x + 24, cy, 14, col);
        if (p.banned && !p.banReason.empty()) {
            DrawText(TextFormat(T("reason: %s"), p.banReason.c_str()),
                     x + 24 + MeasureText(state.c_str(), 14) + 14, cy + 2, 11,
                     Color{150, 130, 130, 255});
        }
        cy += 26;
    }

    // ── What can be done about it ──
    {
        const double days = parseTimeoutDays(m_devTimeoutText);
        const std::string said = describeTimeout(days);
        DrawText(T("Timeout length:"), x + 24, cy + 6, 11, Color{130, 136, 156, 255});
        const Rectangle f = {(float)(x + 24 + MeasureText(T("Timeout length:"), 11) + 10),
                             (float)cy + 2, 96, 21};
        const bool fh = CheckCollisionPointRec(mouse, f);
        DrawRectangleRec(f, m_devTimeoutFocus ? Color{24, 26, 34, 255} : Color{15, 17, 23, 255});
        DrawRectangleLinesEx(f, 1, m_devTimeoutFocus ? accent : Color{62, 60, 66, 175});
        DrawText(m_devTimeoutText.empty() ? T("36h") : m_devTimeoutText.c_str(),
                 (int)f.x + 6, (int)f.y + 5, 11,
                 m_devTimeoutText.empty() ? Color{92, 94, 108, 255} : WHITE);
        if (fh && click) { m_devTimeoutFocus = true; m_devLookupFocus = false; }
        cy += 30;

        struct Act { const char* label; const char* action; double days; Color on; bool live; };
        const Act acts[] = {
            {T("Ban for good"), "ban", 0.0, Color{80, 42, 44, 245}, !p.banned},
            {said.empty() ? T("Time out (enter a length)")
                          : TextFormat(T("Time out for %s"), said.c_str()),
             "timeout", days, Color{72, 58, 40, 245}, days > 0.0},
            // The reverse direction, and the reason this screen can act at all
            // rather than only record: a punishment that was wrong should be
            // liftable in one press.
            {T("Pardon"), "dismiss", 0.0, Color{40, 62, 48, 245}, p.banned},
        };
        int bx = x + 24;
        for (const Act& a : acts) {
            const int bw = MeasureText(a.label, 12) + 20;
            const Rectangle b = {(float)bx, (float)cy, (float)bw, 26};
            const bool bh = a.live && CheckCollisionPointRec(mouse, b);
            DrawRectangleRounded(b, 0.25f, 6, !a.live ? Color{20, 20, 26, 190}
                                                      : (bh ? a.on : Color{26, 26, 34, 220}));
            DrawRectangleRoundedLines(b, 0.25f, 6, a.live ? Color{86, 80, 88, 195}
                                                          : Color{50, 48, 54, 160});
            DrawText(a.label, bx + 10, (int)b.y + 7, 12,
                     !a.live ? Color{96, 94, 102, 255} : (bh ? WHITE : Color{180, 176, 184, 255}));
            if (bh && click) {
                actOnAccount(a.action, a.days);
                Audio::get().playSfx(a.action[0] == 'd' ? "back" : "confirm");
            }
            bx += bw + 8;
        }
        cy += 38;
    }

    // ── What they have been part of, both ways ──
    const Rectangle list = {(float)(x + 24), (float)cy, (float)(w - 48),
                            (float)(y + h - 24 - cy)};
    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    int ry = (int)list.y - m_devReportScroll;

    auto section = [&](const char* title, const std::vector<DevReport>& rows, Color tint) {
        DrawText(TextFormat(title, (int)rows.size()), (int)list.x, ry, 12, tint);
        ry += 18;
        if (rows.empty()) {
            DrawText(T("none"), (int)list.x + 12, ry, 11, Color{100, 104, 118, 255});
            ry += 20;
            return;
        }
        for (const DevReport& r : rows) {
            if (ry > list.y - 30 && ry < list.y + list.height) {
                const char* head = TextFormat("%s  ·  %s  ·  %s", r.reason.c_str(),
                                              r.status.c_str(),
                                              r.outcome.empty() ? "-" : r.outcome.c_str());
                DrawText(head, (int)list.x + 12, ry, 11, Color{150, 146, 154, 255});
                int fs = 11;
                const std::string q =
                    odText::fitToWidth("\"" + r.message + "\"", (int)list.width - 40, fs, 9);
                DrawText(q.c_str(), (int)list.x + 12, ry + 14, fs, Color{176, 172, 180, 255});
            }
            ry += 32;
        }
        ry += 6;
    };
    section(T("Reported by others (%d)"), p.against, Color{224, 170, 170, 255});
    section(T("Reports they filed (%d)"), p.filed, Color{170, 190, 224, 255});
    EndScissorMode();

    if (CheckCollisionPointRec(mouse, list)) {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            const int maxScroll = std::max(0, ry - (int)list.y - (int)list.height + 20 +
                                                m_devReportScroll);
            m_devReportScroll = std::clamp(m_devReportScroll - (int)(wheel * 45), 0, maxScroll);
        }
    }
}
