// Looking for a game: the board, in the game.
//
// ── WHY THIS EXISTS AT ALL ──
//
// Multiplayer worked and nobody could find anybody. The invite code is a fine
// way to play with people you already know and no way whatsoever to meet
// anyone, so the honest answer to "I want to play this with humans" was "go to
// Discord, hope somebody is posting". This board is the other half: a place to
// say "I am hosting, here is the code" and a place to read what other people
// have said.
//
// ── IT IS THE SAME BOARD AS THE DISCORD CHANNEL, AND THAT IS THE POINT ──
//
// A listing posted here appears in #looking-for-a-game, and a listing posted
// there with /lfg appears here. Not two boards that resemble each other: one
// board with two windows onto it. Half a dozen people are not enough to fill
// two rooms, and a game board that is always empty because everyone is in the
// chat teaches players to stop opening it.
//
// So the Discord invite is on this screen rather than buried in Community. The
// board is the reason to go, and the ask is made at the moment somebody has
// discovered they want to play with people.
//
// ── WHAT THIS FILE IS RESPONSIBLE FOR ──
//
// Asking, holding, drawing, and turning clicks into requests. What a listing
// may CONTAIN is decided in src/net/Lfg.h, which is the sealed part, and the
// guidelines are enforced there and in the service -- never here. Nothing in
// this file is allowed to widen either.
//
// NO SERVICE MEANS NO BOARD, and that is not an error path, exactly as with
// the announcement board: a player with no internet is shown an empty board
// and an invitation to host, not a red box about a server they never asked
// about.

#include "Game.h"

#include "GameInternals.h"
#include "MpWidgets.h"
#include "i18n/Text.h"

#include "llm/Advisor.h"          // isLocal: plain HTTP on loopback only
#include "net/AccountClient.h"
#include "net/Host.h"     // the lobby a listing is prefilled from
#include "net/HttpClient.h"
#include "net/Lfg.h"
#include "util/Async.h"
#include "util/OpenLink.h"

#include <algorithm>
#include <ctime>
#include <mutex>

namespace {

// Handed between the worker and the game thread, like the announcement board.
// The worker must not touch Game: it runs on a thread on desktop and on the
// ASYNCIFY queue on web, and neither is the frame.
std::mutex g_lock;
std::vector<odlfg::Listing> g_fetched;
bool g_ready = false;
bool g_inFlight = false;
std::string g_note;          ///< a sentence for the player, or empty
bool g_noteError = false;
double g_lastFetch = -1.0e9;

/** Seconds between automatic refreshes while the board is open. */
constexpr double kRefreshEvery = 20.0;

std::string trimSlashes(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

void finish(const std::string& note, bool error) {
    std::lock_guard<std::mutex> g(g_lock);
    g_inFlight = false;
    g_note = note;
    g_noteError = error;
}

/** One board fetch. Runs on the worker; every failure ends in an empty board. */
void fetchInto(const std::string& base) {
    HttpRequest req;
    req.method = "GET";
    req.url = base + "/lfg";
    // Short. Nothing here is worth making somebody wait for, and a board that
    // is slow to answer is a board with nothing on it as far as this screen is
    // concerned.
    req.timeoutMs = 6000;
    req.allowInsecure = llm::isLocal(base);
    req.maxResponseBytes = (uint32_t)odlfg::Limits::kDocument;
    const HttpResponse res = httpRequest(req);

    std::vector<odlfg::Listing> got;
    if (res.ok()) {
        std::string why;
        got = odlfg::parseBoard(res.body, why);
        // `why` is for a log and never for the player: see the header.
    }
    std::lock_guard<std::mutex> g(g_lock);
    g_fetched = std::move(got);
    g_ready = true;
}

/**
 * The sentence the service sent back, or a fallback.
 *
 * The service is what DECIDES whether a listing is allowed, so when it refuses
 * one its words are what the player should read -- not a generic "that did not
 * work" that leaves them guessing which rule they broke.
 */
std::string refusal(const HttpResponse& res, const char* fallback) {
    const std::string said = httpJsonString(res.body, "message", 240);
    if (!said.empty()) return said;
    if (res.status == 401) return "Sign in to use the board.";
    if (!res.error.empty()) return res.error;
    return fallback;
}

}  // namespace

// ────────────────────────────────────────────────────────────────── asking ──

void Game::lfgRefresh(bool force) {
    if (m_config.accountIssuer.empty()) return;
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_inFlight) return;
        if (!force && GetTime() - g_lastFetch < kRefreshEvery) return;
        g_lastFetch = GetTime();
        g_inFlight = true;
    }
    const std::string base = trimSlashes(m_config.accountIssuer);
    odasync::run([base]() {
        fetchInto(base);
        std::lock_guard<std::mutex> g(g_lock);
        g_inFlight = false;
    });
}

void Game::pumpLfg() {
    std::vector<odlfg::Listing> got;
    std::string note;
    bool error = false, have = false;
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_ready) { g_ready = false; got.swap(g_fetched); have = true; }
        if (!g_note.empty()) { note.swap(g_note); error = g_noteError; }
        m_lfgBusy = g_inFlight;
    }
    if (have) {
        m_lfgListings = odlfg::live(got, (long long)std::time(nullptr));
        // Which one is mine, so the board offers to take it down rather than
        // making somebody remember an id.
        m_lfgMineId.clear();
        const std::string me = AccountClient::get().account().nickname;
        if (!me.empty()) {
            for (const odlfg::Listing& l : m_lfgListings) {
                if (l.nick == me) { m_lfgMineId = l.id; break; }
            }
        }
    }
    if (!note.empty()) mpNote(note, error);
    if (m_mpPage == MpPage::Board) lfgRefresh(false);
}

void Game::lfgOpenBoard() {
    m_mpPage = MpPage::Board;
    m_mpFocus = -1;
    m_lfgScroll = 0;
    m_lfgReporting.clear();
    lfgRefresh(true);
}

// ──────────────────────────────────────────────────────────────── posting ──

void Game::lfgDraftFromLobby() {
    // Prefilled from the game this player is actually hosting, because a form
    // that asks a host to retype what the lobby beside it already knows is a
    // form most people abandon. Everything stays editable.
    m_lfgDraft.kind = odlfg::Kind::Hosting;
    if (m_netHost) {
        m_lfgDraft.code = m_netHost->code();
        m_lfgDraft.slotsTaken = (int)m_netHost->lobby().roster().size();
        m_lfgDraft.slotsTotal = std::clamp(m_mpMaxPlayers, odlfg::Limits::kSlotsMin,
                                           odlfg::Limits::kSlotsMax);
        const int seconds = mpTurnSeconds();
        if (seconds > 0) {
            m_lfgDraft.mode = odlfg::Mode::Rapid;
            m_lfgDraft.turnSeconds = std::clamp(seconds, odlfg::Limits::kTurnSecondsMin,
                                                odlfg::Limits::kTurnSecondsMax);
        } else {
            // No timer is not "no pace": it is a long-form game, and the thing
            // a reader needs to know is how often people are expected to show
            // up. A day is the pace most of these actually run at.
            m_lfgDraft.mode = odlfg::Mode::Longform;
            if (m_lfgDraft.turnHours <= 0) m_lfgDraft.turnHours = 24;
        }
    }
    if (m_lfgDraft.map.empty() && !m_mpMapId.empty()) {
        std::string path, name;
        if (mpResolveMap(m_mpMapId, path, name)) m_lfgDraft.map = name;
    }
    if (m_lfgDraft.map.empty()) m_lfgDraft.map = "any";
}

void Game::lfgSubmitDraft() {
    if (!AccountClient::get().account().valid()) {
        mpNote("Sign in first -- a listing is posted under your nickname.", true);
        m_currentScreen = SCREEN_ACCOUNT;
        return;
    }
    // Checked here so the player is told while the form is still in front of
    // them. The service checks again and its answer wins; see src/net/Lfg.h.
    const std::string problem = odlfg::problemWith(m_lfgDraft);
    if (!problem.empty()) { mpNote(problem, true); return; }

    const std::string base = trimSlashes(m_config.accountIssuer);
    if (base.empty()) { mpNote("No account service is configured.", true); return; }
    const std::string body = odlfg::postBody(m_lfgDraft);
    const std::string token = AccountClient::get().sessionToken();
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_inFlight) return;
        g_inFlight = true;
    }
    odasync::run([base, body, token]() {
        HttpRequest req;
        req.method = "POST";
        req.url = base + "/lfg";
        req.body = body;
        req.bearer = token;
        req.allowInsecure = llm::isLocal(base);
        const HttpResponse res = httpRequest(req);
        if (!res.ok()) { finish(refusal(res, "The board would not take that listing."), true); return; }
        fetchInto(base);
        finish("Posted. It is on the board and in Discord, and it closes itself when it expires.", false);
    });
    m_mpPage = MpPage::Board;
    m_mpFocus = -1;
}

void Game::lfgCloseMine() {
    if (m_lfgMineId.empty()) return;
    const std::string base = trimSlashes(m_config.accountIssuer);
    const std::string body = odlfg::closeBody(m_lfgMineId);
    const std::string token = AccountClient::get().sessionToken();
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_inFlight) return;
        g_inFlight = true;
    }
    odasync::run([base, body, token]() {
        HttpRequest req;
        req.method = "POST";
        req.url = base + "/lfg/close";
        req.body = body;
        req.bearer = token;
        req.allowInsecure = llm::isLocal(base);
        const HttpResponse res = httpRequest(req);
        if (!res.ok()) { finish(refusal(res, "Could not take it down."), true); return; }
        fetchInto(base);
        finish("Taken down.", false);
    });
}

void Game::lfgSendReport(const std::string& id, const std::string& note) {
    if (!AccountClient::get().account().valid()) {
        mpNote("Sign in to report a listing.", true);
        return;
    }
    const std::string base = trimSlashes(m_config.accountIssuer);
    const std::string body = odlfg::reportBody(id, "ingame", note);
    const std::string token = AccountClient::get().sessionToken();
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_inFlight) return;
        g_inFlight = true;
    }
    odasync::run([base, body, token]() {
        HttpRequest req;
        req.method = "POST";
        req.url = base + "/lfg/report";
        req.body = body;
        req.bearer = token;
        req.allowInsecure = llm::isLocal(base);
        const HttpResponse res = httpRequest(req);
        if (!res.ok()) { finish(refusal(res, "Could not send that report."), true); return; }
        // Deliberately NOT "removed": a report is a message to a person, and
        // telling somebody their report took the listing down would be a lie
        // that makes the next one feel useless when nothing visibly happens.
        finish("Thank you. A moderator will look at it.", false);
    });
}

void Game::lfgJoin(const odlfg::Listing& listing) {
    if (!listing.joinable()) return;
    // The same path as a code typed by hand, including signing in first: see
    // runAnnouncementAction, which does this for the same reason. Being sent
    // to multiplayer and failing there tells somebody they cannot join without
    // telling them why.
    m_pendingJoinCode = listing.code;
    if (!AccountClient::get().account().valid()) {
        m_currentScreen = SCREEN_ACCOUNT;
        return;
    }
    m_mpCodeField = listing.code;
    m_mpAddressField.clear();
    m_mpIpWarningAccepted = false;
    m_mpPage = MpPage::Join;
    m_mpFocus = -1;
}

// ─────────────────────────────────────────────────────────── call to action ──

void Game::drawLfgCallToAction(int x, int y, int w, Vector2 mouse, bool click) {
    // ── ONE ASK, WHERE THE WANT IS ──
    //
    // Not a banner on the main menu. Somebody looking at this board has just
    // decided they want to play with people, which is the only moment the ask
    // means anything -- and it says what is on the other side rather than
    // "join our Discord", which is a request with nothing in it for the person
    // being asked.
    const int h = 92;
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.12f, 8,
                         Color{40, 35, 55, 200});
    DrawRectangleRoundedLines({(float)x, (float)y, (float)w, (float)h}, 0.12f, 8,
                              Color{100, 80, 140, 190});
    DrawText(T("The same board is a Discord channel"), x + 16, y + 12, 17,
             Color{200, 185, 230, 255});
    // ONE LITERAL PER CALL, not a concatenation: tools/i18n_extract.py reads
    // the source for T() literals, and the halves of a split string reach
    // en.json as two fragments a translator cannot make a sentence out of.
    wrapText(T("Listings posted here show up in #looking-for-a-game, and listings posted there show up here."),
             x + 16, y + 36, w - 180, 14, Color{150, 150, 170, 255}, true);

    const MpButton go = buttonAt((float)(x + w - 156), (float)(y + h / 2 - 19), 140.0f, 38.0f, mouse);
    drawButton(go, "Open Discord", 16, Color{60, 50, 80, 235}, Color{130, 100, 180, 230});
    if (click && go.hovered) odlink::open(odlfg::kDiscordInvite);
}

// ─────────────────────────────────────────────────────────────── the board ──

void Game::drawMpBoard(Vector2 mouse, bool click) {
    const int centerX = m_screenW / 2;
    const int listW = 660;
    const int left = centerX - listW / 2;
    const long long now = (long long)std::time(nullptr);

    int y = 122;
    const bool signedIn = AccountClient::get().account().valid();

    // The buttons that do not move: posting and leaving. Drawn first, at fixed
    // positions, because a list that grows must never push the way out off the
    // bottom of a small window.
    const int btnY = m_screenH - 108;
    const MpButton post = buttonAt((float)(left), (float)btnY, 210.0f, 44.0f, mouse);
    const bool mine = !m_lfgMineId.empty();
    drawButton(post, mine ? "Take my listing down" : "Post your game", 17,
               mine ? Color{58, 40, 40, 230} : Color{44, 62, 50, 230},
               mine ? Color{190, 130, 130, 210} : Color{130, 190, 140, 210});
    if (click && post.hovered) {
        if (mine) {
            lfgCloseMine();
        } else if (!signedIn) {
            mpNote("Sign in first -- a listing is posted under your nickname.", true);
            m_currentScreen = SCREEN_ACCOUNT;
        } else {
            lfgDraftFromLobby();
            m_mpPage = MpPage::Post;
            m_mpFocus = -1;
        }
    }

    const MpButton back = buttonAt((float)(left + listW - 130), (float)btnY, 130.0f, 44.0f, mouse);
    drawButton(back, "Back", 17, Color{34, 36, 44, 220}, Color{90, 95, 110, 190});
    if (click && back.hovered) { m_mpPage = MpPage::Hub; m_mpFocus = -1; }

    const MpButton again = buttonAt((float)(left + listW - 274), (float)btnY, 134.0f, 44.0f, mouse);
    drawButton(again, m_lfgBusy ? "Refreshing..." : "Refresh", 17,
               Color{34, 40, 52, 230}, Color{100, 120, 150, 200}, !m_lfgBusy);
    if (click && again.hovered && !m_lfgBusy) lfgRefresh(true);

    const int listBottom = btnY - 16;

    if (m_config.accountIssuer.empty()) {
        DrawText(T("This build has no account service, so there is no board."),
                 left, y, 17, Color{130, 135, 150, 255});
        drawLfgCallToAction(left, y + 40, listW, mouse, click);
        return;
    }

    if (m_lfgListings.empty()) {
        // An empty board is a thing to fix, not a thing to apologise for: the
        // person reading it is exactly the person who could fix it.
        DrawText(T("Nobody is looking for a game right now."), left, y, 19,
                 Color{170, 180, 200, 255});
        y += 28;
        wrapText(T("Post yours and it appears here and in Discord at the same time. You do not have to be hosting yet: a listing that says you are looking for a game works the same way."),
                 left, y, listW - 20, 15, Color{130, 135, 150, 255}, true);
        drawLfgCallToAction(left, y + 64, listW, mouse, click);
        return;
    }

    DrawText(T("Games open right now"), left, y, 19, Color{170, 180, 200, 255});
    y += 30;

    // The wheel scrolls the list, not the page: the buttons above stay put.
    if (CheckCollisionPointRec(mouse, {(float)left, (float)y, (float)listW,
                                       (float)(listBottom - y)})) {
        m_lfgScroll -= (int)(GetMouseWheelMove() * 42.0f);
    }

    const int top = y;
    BeginScissorMode(left - 4, top, listW + 8, listBottom - top);
    int ry = top - m_lfgScroll;

    for (const odlfg::Listing& l : m_lfgListings) {
        const bool reporting = (m_lfgReporting == l.id);
        const int rowH = (l.note.empty() ? 74 : 94) + (reporting ? 52 : 0);
        // Rows scrolled out of sight are not drawn, but their height still
        // counts, or the list would shuffle as it scrolls.
        if (ry + rowH < top || ry > listBottom) { ry += rowH + 10; continue; }

        const Rectangle row{(float)left, (float)ry, (float)listW, (float)rowH};
        const bool isMine = (l.id == m_lfgMineId);
        DrawRectangleRounded(row, 0.1f, 8, isMine ? Color{30, 38, 34, 215} : Color{24, 26, 34, 205});
        DrawRectangleRoundedLines(row, 0.1f, 8,
                                  isMine ? Color{110, 160, 120, 200} : Color{70, 75, 90, 180});

        // ── THE TAG, AS A CHIP ──
        //
        // The channel's rule is "use the correct tag", and a tag nobody can see
        // at a glance is a rule nobody follows. Green is a game you can join,
        // blue is a person waiting for one; the two are read differently and
        // look different.
        const bool hosting = (l.kind == odlfg::Kind::Hosting);
        const char* tag = hosting ? "HOSTING" : "LOOKING";
        const int tagW = MeasureText(tag, 11) + 16;
        DrawRectangleRounded({row.x + 12, row.y + 12, (float)tagW, 18.0f}, 0.4f, 8,
                             hosting ? Color{40, 70, 50, 230} : Color{38, 52, 72, 230});
        DrawText(tag, (int)row.x + 20, (int)row.y + 15, 11,
                 hosting ? Color{150, 210, 165, 255} : Color{150, 185, 225, 255});

        DrawText(l.nick.c_str(), (int)row.x + 20 + tagW, (int)row.y + 11, 18, RAYWHITE);

        std::string line = l.map + "  ·  " + l.paceLine();
        const std::string seats = l.seatsLine();
        if (!seats.empty()) line += "  ·  " + seats;
        if (!l.language.empty()) line += "  ·  " + l.language;
        if (!l.region.empty()) line += "  ·  " + l.region;
        DrawText(line.c_str(), (int)row.x + 14, (int)row.y + 38, 14, Color{150, 158, 175, 255});

        if (!l.note.empty()) {
            wrapText(l.note, (int)row.x + 14, (int)row.y + 58, listW - 210, 14,
                     Color{125, 132, 150, 255}, true);
        }

        const std::string closes = l.closesIn(now);
        if (!closes.empty()) {
            DrawText(closes.c_str(), (int)row.x + 14, (int)(row.y + rowH - (reporting ? 72 : 20)),
                     12, Color{105, 110, 128, 255});
        }

        // Join, for the listings that can be joined. A "looking" listing has no
        // code by construction, so there is no button to press and nothing to
        // explain.
        if (l.joinable()) {
            const MpButton join = buttonAt(row.x + row.width - 122, row.y + 12, 108.0f, 36.0f, mouse);
            drawButton(join, "Join", 16, Color{40, 60, 48, 230}, Color{130, 190, 140, 210});
            if (click && join.hovered) lfgJoin(l);
        }

        // Report, on every listing including a "looking" one: the rule people
        // break is as often "be respectful" as it is "wrong tag".
        const MpButton rep = buttonAt(row.x + row.width - 122, row.y + (l.joinable() ? 52 : 12),
                                      108.0f, 26.0f, mouse);
        drawButton(rep, reporting ? "Cancel" : "Report", 13,
                   Color{40, 34, 36, 220}, Color{120, 90, 95, 190});
        if (click && rep.hovered) {
            m_lfgReporting = reporting ? std::string() : l.id;
            m_lfgReportNote.clear();
            m_mpFocus = reporting ? -1 : 7;
        }

        if (reporting) {
            // ── A REPORT SAYS WHAT IS WRONG ──
            //
            // A button that files "reported" and nothing else hands a moderator
            // a listing and no reason, and they end up guessing which of four
            // rules somebody meant. One box, required, and the words go
            // straight to the same moderation channel Discord reports go to.
            const float fy = row.y + rowH - 46;
            drawField(row.x + 14, fy, row.width - 150, 34.0f, m_lfgReportNote,
                      "What is wrong with it?", m_mpFocus == 7, 15);
            if (click && CheckCollisionPointRec(mouse, {row.x + 14, fy, row.width - 150, 34.0f}))
                m_mpFocus = 7;
            const MpButton send = buttonAt(row.x + row.width - 122, fy, 108.0f, 34.0f, mouse);
            const bool ready = m_lfgReportNote.size() >= 4;
            drawButton(send, "Send", 15, Color{52, 40, 42, 230}, Color{160, 110, 115, 210}, ready);
            if (click && send.hovered && ready) {
                lfgSendReport(l.id, m_lfgReportNote);
                m_lfgReporting.clear();
                m_lfgReportNote.clear();
                m_mpFocus = -1;
            }
        }

        ry += rowH + 10;
    }

    // The invitation rides at the end of the list rather than sitting in a
    // fixed panel: on a full board it is something you reach, on an empty one
    // it is the first thing you see.
    drawLfgCallToAction(left, ry + 6, listW, mouse, click);
    ry += 104;
    EndScissorMode();

    const int span = (ry + m_lfgScroll) - top;
    m_lfgScroll = std::clamp(m_lfgScroll, 0, std::max(0, span - (listBottom - top)));
}

// ──────────────────────────────────────────────────────────────── the form ──

void Game::drawMpPost(Vector2 mouse, bool click) {
    const int centerX = m_screenW / 2;
    const int fieldW = 520;
    const int left = centerX - fieldW / 2;
    int y = 118;

    // ── THE GUIDELINES, WHERE THE POST IS WRITTEN ──
    //
    // The same four rules that are pinned in the channel, in the same order and
    // the same words, above the form rather than behind a link. A rule somebody
    // reads while typing is a rule they follow; a rule in a pinned message is a
    // rule they are told about after breaking it.
    DrawText(T("How this board works"), left, y, 15, Color{150, 160, 180, 255});
    y += 22;
    for (const std::string& rule : odlfg::guidelines()) {
        DrawText("·", left + 2, y, 15, Color{110, 150, 190, 255});
        DrawText(rule.c_str(), left + 16, y, 15, Color{130, 138, 155, 255});
        y += 20;
    }
    y += 14;

    // Tag.
    const bool hosting = m_lfgDraft.kind == odlfg::Kind::Hosting;
    const MpButton tHost = buttonAt((float)left, (float)y, (float)(fieldW / 2 - 6), 38.0f, mouse);
    drawButton(tHost, "I am hosting a game", 16,
               hosting ? Color{44, 62, 50, 235} : Color{28, 30, 38, 220},
               hosting ? Color{130, 190, 140, 220} : Color{80, 85, 100, 190});
    if (click && tHost.hovered) m_lfgDraft.kind = odlfg::Kind::Hosting;

    const MpButton tLook = buttonAt((float)(left + fieldW / 2 + 6), (float)y,
                                    (float)(fieldW / 2 - 6), 38.0f, mouse);
    drawButton(tLook, "I am looking for one", 16,
               !hosting ? Color{38, 52, 72, 235} : Color{28, 30, 38, 220},
               !hosting ? Color{120, 160, 200, 220} : Color{80, 85, 100, 190});
    if (click && tLook.hovered) {
        m_lfgDraft.kind = odlfg::Kind::Looking;
        // A looking listing carries no code, so the field is emptied rather
        // than kept and silently refused when the post is sent.
        m_lfgDraft.code.clear();
    }
    y += 50;

    // Map.
    DrawText(T("Map"), left, y, 14, Color{140, 148, 165, 255});
    y += 19;
    drawField((float)left, (float)y, (float)fieldW, 38.0f, m_lfgDraft.map,
              "1914, Modern Day, any...", m_mpFocus == 6, 16);
    if (click && CheckCollisionPointRec(mouse, {(float)left, (float)y, (float)fieldW, 38.0f}))
        m_mpFocus = 6;
    y += 50;

    // Pace. Presets rather than a text box: the useful answers are a short
    // list, and a box invites "fast", which tells a reader nothing.
    DrawText(T("How long is a turn?"), left, y, 14, Color{140, 148, 165, 255});
    y += 19;
    const bool rapid = m_lfgDraft.mode == odlfg::Mode::Rapid;
    struct Pace { const char* label; int value; };
    const Pace rapidPaces[] = {{"1 min", 60}, {"2 min", 120}, {"5 min", 300}, {"10 min", 600}};
    const Pace slowPaces[]  = {{"12 h", 12}, {"1 day", 24}, {"2 days", 48}, {"1 week", 168}};
    const int paceCount = 4;
    const float pw = (float)(fieldW - 3 * 8) / (float)paceCount;
    for (int i = 0; i < paceCount; i++) {
        const Pace& p = rapid ? rapidPaces[i] : slowPaces[i];
        const bool on = rapid ? m_lfgDraft.turnSeconds == p.value
                              : m_lfgDraft.turnHours == p.value;
        const MpButton b = buttonAt((float)left + i * (pw + 8), (float)y, pw, 34.0f, mouse);
        drawButton(b, p.label, 15, on ? Color{40, 52, 68, 235} : Color{28, 30, 38, 220},
                   on ? Color{120, 160, 200, 220} : Color{80, 85, 100, 190});
        if (click && b.hovered) {
            if (rapid) m_lfgDraft.turnSeconds = p.value; else m_lfgDraft.turnHours = p.value;
        }
    }
    y += 42;
    const MpButton swap = buttonAt((float)left, (float)y, 220.0f, 30.0f, mouse);
    drawButton(swap, rapid ? "Switch to a long-form game" : "Switch to a live game", 14,
               Color{28, 30, 38, 220}, Color{80, 85, 100, 190});
    if (click && swap.hovered) {
        m_lfgDraft.mode = rapid ? odlfg::Mode::Longform : odlfg::Mode::Rapid;
    }
    y += 44;

    // Seats, for a hosting listing. "Post the parameters of the game" is one of
    // the four rules, and how many people it takes is the parameter somebody
    // deciding whether to join actually needs.
    if (hosting) {
        DrawText(T("Players"), left, y, 14, Color{140, 148, 165, 255});
        y += 19;
        const MpButton fewer = buttonAt((float)left, (float)y, 36.0f, 34.0f, mouse);
        drawButton(fewer, "-", 18, Color{34, 36, 44, 220}, Color{90, 95, 110, 190});
        if (click && fewer.hovered)
            m_lfgDraft.slotsTotal = std::max(odlfg::Limits::kSlotsMin, m_lfgDraft.slotsTotal - 1);
        const std::string seats = std::to_string(m_lfgDraft.slotsTaken) + " of " +
                                  std::to_string(m_lfgDraft.slotsTotal) + " seats taken";
        DrawText(seats.c_str(), left + 48, y + 9, 16, RAYWHITE);
        const MpButton more = buttonAt((float)(left + 220), (float)y, 36.0f, 34.0f, mouse);
        drawButton(more, "+", 18, Color{34, 36, 44, 220}, Color{90, 95, 110, 190});
        if (click && more.hovered)
            m_lfgDraft.slotsTotal = std::min(odlfg::Limits::kSlotsMax, m_lfgDraft.slotsTotal + 1);
        if (m_lfgDraft.slotsTaken > m_lfgDraft.slotsTotal)
            m_lfgDraft.slotsTaken = m_lfgDraft.slotsTotal;
        y += 46;

        const std::string code = m_lfgDraft.code.empty()
            ? std::string("No invite code yet -- open a lobby first, then post.")
            : "Invite code " + m_lfgDraft.code;
        DrawText(code.c_str(), left, y, 15,
                 m_lfgDraft.code.empty() ? Color{200, 160, 130, 255} : Color{140, 190, 150, 255});
        y += 28;
    }

    // Note.
    DrawText(T("Anything else? (optional)"), left, y, 14, Color{140, 148, 165, 255});
    y += 19;
    drawField((float)left, (float)y, (float)fieldW, 38.0f, m_lfgDraft.note,
              "New players welcome, we explain the rules", m_mpFocus == 8, 16);
    if (click && CheckCollisionPointRec(mouse, {(float)left, (float)y, (float)fieldW, 38.0f}))
        m_mpFocus = 8;
    y += 26;
    DrawText(T("No links -- the invite code is how people join."), left, y, 12,
             Color{110, 116, 132, 255});
    y += 30;

    // What is wrong with it, said before it is sent rather than after.
    const std::string problem = odlfg::problemWith(m_lfgDraft);
    if (!problem.empty()) {
        DrawText(problem.c_str(), left, y, 14, Color{210, 150, 130, 255});
    }
    y += 24;

    const MpButton back = buttonAt((float)left, (float)y, 150.0f, 42.0f, mouse);
    drawButton(back, "Back", 17, Color{34, 36, 44, 220}, Color{90, 95, 110, 190});
    if (click && back.hovered) { m_mpPage = MpPage::Board; m_mpFocus = -1; }

    const MpButton send = buttonAt((float)(left + fieldW - 190), (float)y, 190.0f, 42.0f, mouse);
    drawButton(send, "Post it", 17, Color{44, 62, 50, 230}, Color{130, 190, 140, 210},
               problem.empty() && !m_lfgBusy);
    if (click && send.hovered && problem.empty() && !m_lfgBusy) lfgSubmitDraft();
}
