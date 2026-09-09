// Writing an announcement from inside the game.
//
// The board is edited in a hurry, usually minutes before a tournament, by
// somebody who is already in the game rather than at a terminal. So this is a
// screen and not a curl command -- and it authenticates with the SAME session
// token and badge the reports queue uses, which means there is no secret to
// type into a text field and nothing sensitive written to disk.
//
// WHAT IS VALIDATED WHERE. The service checks everything again, and so does
// every client that draws the board (src/net/Announcements.h). Checking here as
// well is not redundancy for its own sake: it is the only copy that can tell
// the author what is wrong WHILE THEY ARE TYPING IT, instead of leaving them
// with an entry that silently never appears.

#include "Game.h"

#include "GameInternals.h"
#include "i18n/Text.h"
#include "net/AccountClient.h"
#include "net/Announcements.h"
#include "net/HttpClient.h"
#include "util/Async.h"

#include <algorithm>
#include <ctime>
#include <mutex>

namespace {

std::mutex g_lock;
std::string g_payload;      // the last successful list, as JSON
std::string g_status;
bool g_fresh = false;
bool g_busy = false;

std::string issuerBase(const std::string& raw) {
    std::string base = raw;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base;
}

/// JSON string escaping for the fields the composer sends.
std::string esc(const std::string& v) {
    std::string out;
    for (char c : v) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': break;
            case '\t': out += "\\t";  break;
            default:
                // Control characters are dropped rather than escaped: nothing
                // legitimate types one, and the client refuses an entry that
                // contains one anyway.
                if ((unsigned char)c >= 0x20) out += c;
        }
    }
    return out;
}

/**
 * "3d", "6h 30m", "90m", "45" -- how far off the event is, in seconds.
 *
 * A relative offset rather than a date, because "in three days" is what the
 * person writing it actually knows, and typing an absolute timestamp means
 * doing timezone arithmetic by hand at the exact moment they are least likely
 * to get it right. A bare number is HOURS, because that is the unit a
 * tournament is usually a few of.
 */
long long parseOffsetSeconds(const std::string& text) {
    long long total = 0;
    bool any = false;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && !isdigit((unsigned char)text[i])) ++i;
        if (i >= text.size()) break;
        long long n = 0;
        while (i < text.size() && isdigit((unsigned char)text[i]))
            n = n * 10 + (text[i++] - '0');
        char unit = 'h';
        while (i < text.size() && text[i] == ' ') ++i;
        if (i < text.size() && isalpha((unsigned char)text[i]))
            unit = (char)tolower((unsigned char)text[i++]);
        switch (unit) {
            case 'w': total += n * 604800; break;
            case 'd': total += n * 86400;  break;
            case 'm': total += n * 60;     break;
            case 's': total += n;          break;
            default:  total += n * 3600;   break;   // hours
        }
        any = true;
    }
    return any ? total : 0;
}

}  // namespace

void Game::fetchAdminAnnouncements() {
    const AccountClient& account = AccountClient::get();
    if (!account.account().valid()) return;
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_busy) return;
        g_busy = true;
        g_status = T("Fetching...");
    }
    const std::string url = issuerBase(m_config.accountIssuer) + "/moderation/announcements";
    const std::string token = account.sessionToken();
    // Resolved on the game thread: the arena behind T() is not locked.
    const std::string failed = T("Could not reach the service.");
    const std::string refused = T("This account may not edit the board.");

    odasync::run([url, token, failed, refused]() {
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
            g_status = (res.status == 404) ? refused : failed;
        }
    });
}

void Game::editAnnouncement(const std::string& op, const std::string& id,
                            const std::string& itemJson) {
    const AccountClient& account = AccountClient::get();
    if (!account.account().valid()) return;
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_busy) return;
        g_busy = true;
        g_status = T("Saving...");
    }
    const std::string url = issuerBase(m_config.accountIssuer) + "/moderation/announcement";
    const std::string token = account.sessionToken();
    const std::string failed = T("Could not reach the service.");

    std::string body = "{\"op\":\"" + esc(op) + "\"";
    if (!id.empty()) body += ",\"id\":\"" + esc(id) + "\"";
    if (!itemJson.empty()) body += ",\"item\":" + itemJson;
    body += "}";

    odasync::run([url, token, body, failed]() {
        HttpRequest req;
        req.method = "POST";
        req.url = url;
        req.bearer = token;
        req.body = body;
        req.timeoutMs = 20000;
        const HttpResponse res = httpRequest(req);

        std::lock_guard<std::mutex> g(g_lock);
        g_busy = false;
        if (res.ok()) {
            // The reply is the whole list, so a save refreshes the view without
            // a second round trip.
            g_payload = res.body;
            g_fresh = true;
            g_status.clear();
        } else {
            // The service explains a refusal in words meant for whoever is
            // typing -- "a join button needs buttonParam to be an invite code"
            // -- so it is shown rather than replaced with a generic failure.
            const std::string said = httpJsonString(res.body, "message", 200);
            g_status = said.empty() ? failed : said;
        }
    });
}

void Game::postComposedAnnouncement() {
    // Checked here so the author is told immediately; the service and every
    // client check again.
    if (m_annId.empty()) { m_adminAnnStatus = T("An announcement needs an id."); return; }
    if (m_annTitle.empty() && m_annBody.empty()) {
        m_adminAnnStatus = T("An announcement needs a title or a body.");
        return;
    }
    static const char* kActions[] = {"", "join", "community", "account"};
    static const char* kStyles[]  = {"", "local", "countdown"};

    std::string j = "{\"id\":\"" + esc(m_annId) + "\"";
    if (!m_annTitle.empty()) j += ",\"title\":\"" + esc(m_annTitle) + "\"";
    if (!m_annBody.empty())  j += ",\"body\":\""  + esc(m_annBody)  + "\"";
    if (m_annBtnAction > 0) {
        if (m_annBtnLabel.empty()) {
            m_adminAnnStatus = T("A button needs a label.");
            return;
        }
        j += ",\"buttonLabel\":\"" + esc(m_annBtnLabel) + "\"";
        j += ",\"buttonAction\":\"" + std::string(kActions[m_annBtnAction]) + "\"";
        if (m_annBtnAction == 1) {
            if (!odnews::looksLikeInviteCode(m_annBtnParam)) {
                m_adminAnnStatus = T("A join button needs an invite code.");
                return;
            }
            j += ",\"buttonParam\":\"" + esc(m_annBtnParam) + "\"";
        }
    }
    if (m_annTimeStyle > 0) {
        const long long off = parseOffsetSeconds(m_annEventIn);
        if (off == 0) {
            m_adminAnnStatus = T("Say when, e.g. 3d or 6h 30m.");
            return;
        }
        // Sent as an INSTANT, computed from this machine's clock. Every player
        // then sees it in their own timezone, which is the entire reason the
        // wire format is unix seconds rather than a written-out time.
        j += ",\"eventAt\":" + std::to_string((long long)std::time(nullptr) + off);
        j += ",\"timeStyle\":\"" + std::string(kStyles[m_annTimeStyle]) + "\"";
    }
    j += "}";

    m_adminAnnStatus.clear();
    editAnnouncement("put", "", j);
}

void Game::collectAdminAnnouncements() {
    std::string payload;
    {
        std::lock_guard<std::mutex> g(g_lock);
        m_adminAnnStatus = g_status.empty() ? m_adminAnnStatus : g_status;
        if (!g_fresh) return;
        g_fresh = false;
        payload = g_payload;
    }
    // The SAME parser the menu board uses, so nothing can be edited into
    // existence here that a player's game would refuse to draw.
    std::string why;
    m_adminAnnouncements = odnews::parseDocument(
        "{\"items\":" + payload.substr(payload.find('[') == std::string::npos
                                           ? 0 : payload.find('['))
            + "}", why);

    // "hidden" is an editing concept and not part of what a client is served,
    // so it is read alongside rather than through the sealed parser.
    m_adminAnnHidden.assign(m_adminAnnouncements.size(), false);
    size_t at = 0;
    for (size_t i = 0; i < m_adminAnnouncements.size(); ++i) {
        at = payload.find("\"id\"", at);
        if (at == std::string::npos) break;
        const size_t next = payload.find("\"id\"", at + 4);
        const std::string slice = payload.substr(at, (next == std::string::npos)
                                                         ? std::string::npos : next - at);
        m_adminAnnHidden[i] = httpJsonBool(slice, "hidden", false);
        at += 4;
    }
}

// ────────────────────────────────────────────────────────────── the screen ──

void Game::updateAdminAnnouncements() {
    collectAdminAnnouncements();
    if (m_adminField < 0) return;

    std::string* field = nullptr;
    switch (m_adminField) {
        case 0: field = &m_annId;       break;
        case 1: field = &m_annTitle;    break;
        case 2: field = &m_annBody;     break;
        case 3: field = &m_annBtnLabel; break;
        case 4: field = &m_annBtnParam; break;
        case 5: field = &m_annEventIn;  break;
        default: return;
    }
    int k = GetCharPressed();
    while (k > 0) {
        // The body is the only field long enough to need a real cap; the rest
        // are bounded by what the service will accept anyway.
        const size_t cap = (m_adminField == 2) ? 1200u : 120u;
        if (k >= 32 && field->size() + 4 <= cap) odText::utf8Append(*field, k);
        k = GetCharPressed();
    }
    if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && !field->empty())
        odText::utf8PopBack(*field);
    // Shift+Enter breaks a line in the body, exactly as the letter box does.
    if (m_adminField == 2 && IsKeyPressed(KEY_ENTER) &&
        (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) &&
        field->size() < 1200)
        *field += '\n';
}

void Game::drawAdminAnnouncements(int x, int y, int w, int h, Vector2 mouse, bool click) {
    const Color accent = hexToColor(m_config.accent());
    const int colW = (w - 24) / 2;

    auto textBtn = [&](Rectangle r, const char* label, bool on) {
        const bool hov = CheckCollisionPointRec(mouse, r);
        if (hov || on) DrawRectangleRounded(r, 0.25f, 6,
                                            on ? Color{accent.r, accent.g, accent.b, 40}
                                               : Color{255, 255, 255, 16});
        DrawRectangleRoundedLines(r, 0.25f, 6,
                                  on ? accent : Color{92, 98, 126, 200});
        int fs = 12;
        const std::string fit = odText::fitToWidth(label, (int)r.width - 12, fs, 9);
        DrawText(fit.c_str(), (int)(r.x + r.width / 2 - MeasureText(fit.c_str(), fs) / 2),
                 (int)(r.y + r.height / 2 - fs / 2), fs, hov || on ? WHITE : Color{198, 204, 226, 255});
        return hov && click;
    };

    // ── LEFT: what is on the board now ──
    DrawText(T("On the board"), x, y, 13, accent);
    int ly = y + 24;
    if (m_adminAnnouncements.empty())
        DrawText(T("Nothing posted."), x, ly, 12, Color{120, 126, 148, 255});

    for (size_t i = 0; i < m_adminAnnouncements.size() && ly < y + h - 60; ++i) {
        const odnews::Item& it = m_adminAnnouncements[i];
        const bool hidden = i < m_adminAnnHidden.size() && m_adminAnnHidden[i];
        int ts = 13;
        const std::string title = odText::fitToWidth(
            it.title.empty() ? it.id : it.title, colW - 150, ts, 9);
        DrawText(title.c_str(), x, ly + 6, ts,
                 hidden ? Color{110, 116, 138, 255} : Color{214, 220, 240, 255});
        if (hidden)
            DrawText(T("(hidden)"), x + MeasureText(title.c_str(), ts) + 8, ly + 7, 10,
                     Color{150, 120, 90, 255});

        // Hide and show are the same control in two states: taking something
        // down and putting it back are the same decision, reversed.
        if (textBtn({(float)(x + colW - 142), (float)ly, 62, 24},
                    hidden ? T("Show") : T("Hide"), false))
            editAnnouncement(hidden ? "show" : "hide", it.id, "");
        if (textBtn({(float)(x + colW - 74), (float)ly, 74, 24}, T("Delete"), false))
            editAnnouncement("purge", it.id, "");
        ly += 30;
    }

    // ── RIGHT: writing one ──
    const int cx = x + colW + 24;
    DrawText(T("Write an announcement"), cx, y, 13, accent);
    int fy = y + 24;

    auto field = [&](int index, const char* label, std::string& value, int rows) {
        DrawText(label, cx, fy, 11, Color{140, 146, 168, 255});
        fy += 15;
        const Rectangle box = {(float)cx, (float)fy, (float)colW, (float)(rows * 16 + 10)};
        const bool hov = CheckCollisionPointRec(mouse, box);
        const bool on = (m_adminField == index);
        DrawRectangleRec(box, on ? Color{22, 25, 34, 255} : Color{15, 17, 23, 255});
        DrawRectangleLinesEx(box, 1, on ? accent : Color{60, 64, 84, 200});
        if (hov && click) m_adminField = index;
        else if (click && !hov && on) m_adminField = -1;

        // Wrapped only for the body; the rest are one line and are clipped.
        int ty = (int)box.y + 5;
        std::string line;
        for (size_t i = 0; i <= value.size(); ++i) {
            const bool end = (i == value.size());
            if (!end && value[i] != '\n') {
                line += value[i];
                if (MeasureText(line.c_str(), 12) < colW - 16) continue;
            }
            DrawText(line.c_str(), (int)box.x + 6, ty, 12, WHITE);
            ty += 16;
            line.clear();
            if (ty > box.y + box.height - 16) break;
        }
        if (on && (int)(GetTime() * 2) % 2)
            DrawRectangle((int)box.x + 6 + MeasureText(line.c_str(), 12),
                          std::min(ty, (int)(box.y + box.height - 14)), 2, 12, WHITE);
        fy += (int)box.height + 8;
    };

    field(0, T("Id (how the game remembers it)"), m_annId, 1);
    field(1, T("Title"), m_annTitle, 1);
    field(2, T("Body (tutorial formatting)"), m_annBody, 4);

    // The action is CYCLED, not typed: it is a closed set in the game, and a
    // free-text field here would invite naming one that does not exist.
    static const char* kActionNames[] = {"No button", "Join a game", "Community", "Account"};
    DrawText(T("Button"), cx, fy, 11, Color{140, 146, 168, 255});
    fy += 15;
    if (textBtn({(float)cx, (float)fy, 130, 26}, kActionNames[m_annBtnAction], m_annBtnAction > 0))
        m_annBtnAction = (m_annBtnAction + 1) % 4;
    fy += 32;
    if (m_annBtnAction > 0) {
        field(3, T("Button label"), m_annBtnLabel, 1);
        if (m_annBtnAction == 1) field(4, T("Invite code"), m_annBtnParam, 1);
    }

    static const char* kStyleNames[] = {"No time shown", "Show the time", "Count down"};
    if (textBtn({(float)cx, (float)fy, 150, 26}, kStyleNames[m_annTimeStyle], m_annTimeStyle > 0))
        m_annTimeStyle = (m_annTimeStyle + 1) % 3;
    fy += 32;
    if (m_annTimeStyle > 0) field(5, T("When, from now (3d, 6h 30m)"), m_annEventIn, 1);

    if (textBtn({(float)cx, (float)fy, 110, 30}, T("Post it"), false))
        postComposedAnnouncement();
    if (textBtn({(float)(cx + 120), (float)fy, 90, 30}, T("Refresh"), false))
        fetchAdminAnnouncements();

    if (!m_adminAnnStatus.empty()) {
        int fs = 11;
        const std::string fit = odText::fitToWidth(m_adminAnnStatus, colW, fs, 9);
        DrawText(fit.c_str(), cx, fy + 36, fs, Color{220, 160, 130, 255});
    }
}
