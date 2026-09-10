// The board on the main menu.
//
// WHAT THIS FILE IS RESPONSIBLE FOR: asking the service once, holding what came
// back, and drawing it. What may be IN an announcement is decided in
// src/net/Announcements.h, which is the sealed part -- nothing here is allowed
// to widen it.
//
// NO INTERNET MEANS NO BOARD, and that is not an error path. A player on a
// train has nothing to be told about, and a game that shows them a red box
// saying it could not reach a server has made its own plumbing their problem.
// Every failure here ends with an empty list.

#include "Game.h"

#include "GameInternals.h"
#include "dialog/DialogScript.h"
#include "i18n/Text.h"

#include "net/AccountClient.h"
#include "llm/Advisor.h"
#include "net/Announcements.h"
#include "net/HttpClient.h"
#include "util/Async.h"

#include <algorithm>
#include <ctime>
#include <mutex>

namespace {

// Handed between the fetch and the game thread. The fetch runs on a worker (or,
// on web, on the ASYNCIFY queue) and must not touch Game.
std::mutex g_lock;
std::vector<odnews::Item> g_fetched;
bool g_ready = false;
bool g_asked = false;

}  // namespace

void Game::pumpAnnouncements() {
    // Collect first, so a reply that arrived since the last frame is on screen
    // this frame rather than the next one.
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_ready) {
            g_ready = false;
            m_announcements = g_fetched;
        }
    }

    if (g_asked) return;
    // Asked ONCE per run, and only when there is a service to ask. A menu that
    // polls is a menu that talks to a server for as long as it is left open.
    if (m_config.accountIssuer.empty()) return;
    g_asked = true;

    std::string url = m_config.accountIssuer;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/announcements";

    odasync::run([url]() {
        HttpRequest req;
        req.method = "GET";
        req.url = url;
        // Short, because nothing here is worth making a player wait for. A
        // service that is slow is a service with nothing to say.
        req.timeoutMs = 6000;
        // ── PLAIN HTTP ONLY ON LOOPBACK ──
        //
        // The real service is https and stays that way: an announcement is
        // fetched by every player and drawn on their menu, so it is exactly the
        // thing not to accept over a connection anybody on the path can rewrite.
        // A service running on this machine is the one place that does not
        // apply -- there is no path -- and it is what makes the board testable
        // without deploying anything.
        req.allowInsecure = llm::isLocal(url);
        const HttpResponse res = httpRequest(req);

        std::vector<odnews::Item> got;
        if (res.ok()) {
            std::string why;
            got = odnews::parseDocument(res.body, why);
            // `why` is for a log, never for the player: see the header.
        }
        std::lock_guard<std::mutex> g(g_lock);
        g_fetched = std::move(got);
        g_ready = true;
    });
}

void Game::announcementBoardChanged() {
    // Only the gate is cleared, not the items: the board on screen stays as it
    // is until the new answer arrives, rather than blinking empty first.
    std::lock_guard<std::mutex> g(g_lock);
    g_asked = false;
}

std::vector<odnews::Item> Game::liveAnnouncements() const {
    return odnews::live(m_announcements, (long long)std::time(nullptr));
}

void Game::runAnnouncementAction(const odnews::Button& b) {
    switch (b.action) {
        case odnews::Action::JoinGame:
            // ── SIGNED IN FIRST, BECAUSE JOINING NEEDS AN ACCOUNT ──
            //
            // The code is remembered and the player is sent to the Account
            // screen; the join is theirs to finish afterwards. Doing it the
            // other way round -- opening multiplayer and failing there -- tells
            // somebody they cannot join without telling them why.
            m_pendingJoinCode = b.param;
            if (!AccountClient::get().account().valid()) {
                m_currentScreen = SCREEN_ACCOUNT;
                return;
            }
            m_currentScreen = SCREEN_MULTIPLAYER;
            m_mpPage = MpPage::Join;
            m_mpCodeField = b.param;
            return;
        case odnews::Action::Community:
            m_currentScreen = SCREEN_COMMUNITY;
            return;
        case odnews::Action::Account:
            m_currentScreen = SCREEN_ACCOUNT;
            return;
        case odnews::Action::None:
            return;   // parseDocument never produces one; nothing to do if it did
    }
}

// ─────────────────────────────────────────────────────────────── the board ──
//
// Drawn to the right of the menu items, and ONLY when there is something to
// say. An empty board is not drawn at all -- no frame, no placeholder, no "no
// announcements" -- because a permanent empty box on the main menu is worse
// than the feature not existing.
//
// The body is written in the dialogue markup the tutorial uses, so it is parsed
// by dlg::parse and drawn as styled runs. That parser produces text and styles
// and can do nothing else, which is what makes it safe to point at a document
// somebody fetched over the network.

void Game::drawAnnouncementBoard(int x, int y, int w, int h, Vector2 mouse, bool click) {
    const std::vector<odnews::Item> items = liveAnnouncements();
    if (items.empty()) return;

    // ── NO PANEL ──
    //
    // The first version was a filled, bordered card 460px tall, and it looked
    // like a window from another screen had been left open on the menu. Nothing
    // else here has a frame: the menu is centred type on the map, and the two
    // things that DO have chrome (the music chip, the tutorial button) are
    // small, cornered, and out of the way.
    //
    // So the board stops being a box. A thin rule in the player's accent down
    // the left edge marks the column, the type does the rest, and the whole
    // thing is as tall as its contents rather than a fixed height with an
    // empty half. On a menu made of words, an announcement should read as more
    // words rather than as a widget.
    const Color accent = hexToColor(m_config.accent());
    BeginScissorMode(x, y, w, h);
    const int pad = 18;          // room to the right of the rule
    const int innerW = w - pad - 8;
    int cy = y + 4 - m_announcementScroll;

    for (const odnews::Item& it : items) {
        if (!it.title.empty()) {
            int ts = 17;
            const std::string fit = odText::fitToWidth(it.title, innerW, ts, 12);
            DrawText(fit.c_str(), x + pad, cy, ts, accent);
            cy += ts + 6;
        }

        // ── WHEN ──
        //
        // Two different questions, and an announcement may want either. A
        // stored instant is rendered in the PLAYER's timezone, so nobody has to
        // do arithmetic on "18:00 UTC"; a countdown answers "how long have I
        // got" and keeps answering after the moment, because somebody arriving
        // late still wants to know how late.
        std::string when;
        if (it.timeStyle == odnews::Item::TimeStyle::Local) {
            when = odnews::formatLocal(it.eventAt);
        } else if (it.timeStyle == odnews::Item::TimeStyle::Countdown) {
            when = odnews::formatCountdown(it.eventAt, (long long)std::time(nullptr));
        } else if (it.postedAt > 0) {
            when = odnews::formatLocal(it.postedAt);
        }
        if (!when.empty()) {
            // In the accent when it is the event, dim when it is only the
            // posting date: one is the point of the notice and the other is
            // filing information.
            const bool isEvent = it.timeStyle != odnews::Item::TimeStyle::None;
            DrawText(when.c_str(), x + pad, cy, 11,
                     isEvent ? accent : Color{112, 118, 140, 255});
            cy += 17;
        }

        if (!it.body.empty()) {
            // The tutorial's own markup. Parsed here, drawn below; nothing in
            // the document is executed, and a tag this build does not know
            // becomes a parser warning rather than anything happening.
            const dlg::Script script = dlg::parse(it.body);
            // ── SPANS FLOW, THEY DO NOT EACH START A LINE ──
            //
            // "**Sixteen seats**, {accent}this Saturday{/accent}." is FOUR
            // spans: a bold one, the comma, an accented one, the full stop.
            // Drawing each on its own line -- which is what the first version
            // did -- put a line containing nothing but "," under the heading,
            // and another containing nothing but ".". Punctuation between two
            // styled runs is the commonest thing in any of this markup.
            //
            // So the pen carries across spans and only moves down when a word
            // will not fit or the text says to.
            int penX = x + pad;
            int lineH = 17;
            // ── A SPACE ONLY WHERE THE SOURCE HAD ONE ──
            //
            // Spans break wherever the markup does, and "**seats**," has no
            // space in it: the comma is simply the next span. Prepending a
            // space to the first word of every span produced "Sixteen seats ,
            // this Saturday ." -- punctuation floating off the word it belongs
            // to. So this carries ACROSS spans and is set only by an actual
            // space character in the text.
            bool pendingSpace = false;
            auto newline = [&]() { penX = x + pad; cy += lineH; };
            for (const dlg::Page& page : script.pages) {
                for (const dlg::Span& span : page.spans) {
                    int fs = 13 + span.style.sizeDelta;
                    if (fs < 9) fs = 9;
                    lineH = std::max(lineH, fs + 4);
                    Color c = Color{198, 205, 226, 255};
                    if (span.style.useAccent) {
                        c = accent;
                    } else if (span.style.hasColor) {
                        c = Color{span.style.color.r, span.style.color.g,
                                  span.style.color.b, 255};
                    }
                    if (span.style.fx & dlg::FX_BOLD) c = WHITE;
                    if (span.style.fx & dlg::FX_ACTION) c = Color{140, 146, 168, 255};

                    // Word by word, so a break lands between words; a chunk of
                    // punctuation with no space before it stays welded to what
                    // it follows, which is the whole point of the pen.
                    std::string word;
                    for (size_t i = 0; i <= span.text.size(); ++i) {
                        const bool end = (i == span.text.size());
                        const char ch = end ? '\0' : span.text[i];
                        if (!end && ch != ' ' && ch != '\n') { word += ch; continue; }
                        if (!word.empty()) {
                            // ── THE SPACE IS PART OF THE WORD ──
                            //
                            // It was advanced separately, by MeasureText(" "),
                            // which this font reports as no width at all: every
                            // word ran into the next and the board read
                            // "Sixteenseats,thisSaturday". Drawing the leading
                            // space means what is MEASURED is exactly what is
                            // DRAWN, which is the only way the two cannot
                            // disagree.
                            const bool atStart = (penX <= x + pad);
                            const bool sep = pendingSpace && !atStart;
                            pendingSpace = false;
                            std::string piece = sep ? " " + word : word;
                            int ww = MeasureText(piece.c_str(), fs);
                            if (!atStart && penX + ww > x + w - pad) {
                                newline();
                                piece = word;
                                ww = MeasureText(piece.c_str(), fs);
                            }
                            DrawText(piece.c_str(), penX, cy, fs, c);
                            penX += ww;
                            word.clear();
                        }
                        if (end) break;
                        if (ch == '\n') { newline(); pendingSpace = false; }
                        else if (ch == ' ') pendingSpace = true;
                    }
                }
            }
            if (penX > x + pad) cy += lineH;
            cy += 4;
        }

        if (it.button.ok()) {
            int bs = 13;
            const std::string label = odText::fitToWidth(it.button.label, innerW - 24, bs, 10);
            const int bw = std::min(innerW, MeasureText(label.c_str(), bs) + 28);
            const Rectangle btn = {(float)(x + pad), (float)cy, (float)bw, 30};
            const bool hov = CheckCollisionPointRec(mouse, btn) &&
                             CheckCollisionPointRec(mouse, {(float)x, (float)y,
                                                            (float)w, (float)h});
            // Outlined in the player's accent rather than filled green. The
            // green box belonged to a different screen; every other control on
            // this menu is either plain type or a thin accent outline.
            if (hov) DrawRectangleRounded(btn, 0.25f, 6, Color{255, 255, 255, 18});
            DrawRectangleRoundedLines(btn, 0.25f, 6,
                                      hov ? accent : Color{accent.r, accent.g, accent.b, 150});
            DrawText(label.c_str(), (int)btn.x + 14, (int)btn.y + 8, bs,
                     hov ? WHITE : Color{206, 212, 232, 255});
            if (hov && click) {
                Audio::get().playSfx("click_soft");
                runAnnouncementAction(it.button);
                EndScissorMode();
                return;
            }
            cy += 38;
        }

        // Space between entries rather than a rule across them: the left rule
        // already says these belong together, and a second line boxes them in
        // again.
        cy += 22;
    }
    // The rule, drawn LAST because its length is the content's -- a fixed one
    // would be back to declaring a box and filling part of it.
    {
        const int top = y + 2;
        const int bottom = std::min(y + h, cy - 14 + m_announcementScroll);
        if (bottom > top) DrawRectangle(x, top, 2, bottom - top,
                                        Color{accent.r, accent.g, accent.b, 170});
    }
    EndScissorMode();

    // Scroll only when there is more than fits, so the wheel does nothing on a
    // board with one short notice on it.
    const int contentH = (cy + m_announcementScroll) - y;
    if (contentH > h && CheckCollisionPointRec(mouse, {(float)x, (float)y, (float)w, (float)h})) {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f)
            m_announcementScroll = std::clamp(m_announcementScroll - (int)(wheel * 40),
                                              0, contentH - h + 20);
    }
}
