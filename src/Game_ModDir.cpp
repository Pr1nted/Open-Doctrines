// The mod directory, drawn inside the mod menu.
//
// ── WHY THIS EXISTS ──
//
// The mod menu could show what you had already installed, and nothing else. A
// player who had never been told the directory existed had no way to discover
// it from inside the game -- so the honest answer to "are there mods for this?"
// was "yes, on a website I have not mentioned".
//
// ── IT LISTS, IT DOES NOT FETCH ──
//
// Every row offers a page to open in a browser. The game does not download a
// mod and does not install one: that is ModUpdates.h's "LOOKS, NEVER TOUCHES",
// and it is the rule the whole capability sandbox rests on. A directory is a
// reason to make mods findable, not a reason to start fetching code.
//
// So this screen ends at odlink::open, exactly as the "Can be updated" button
// does, and the player adds the file the same way they add any other.
//
// ── WHAT THIS FILE IS RESPONSIBLE FOR ──
//
// Asking, holding, drawing, and turning clicks into requests. What a listing
// may CONTAIN, and whether its page may be opened at all, is decided in
// src/net/ModDir.h, which is the sealed part. Nothing here may widen it.
//
// NO SERVICE MEANS NO DIRECTORY, and that is not an error path: a player with
// no internet sees an empty list and a sentence about where mods come from,
// not a red box about a server they never asked about.

#include "Game.h"

#include "GameInternals.h"
#include "MpWidgets.h"
#include "i18n/Text.h"

#include "llm/Advisor.h"          // isLocal: plain HTTP on loopback only
#include "net/HttpClient.h"
#include "net/ModDir.h"
#include "util/Async.h"
#include "util/OpenLink.h"

#include <mutex>

namespace {

// Handed between the worker and the game thread, as the board and the
// announcements do. The worker must never touch Game: it runs on a thread on
// desktop and on the ASYNCIFY queue on web, and neither of those is the frame.
std::mutex g_lock;
std::vector<odmoddir::Listing> g_fetched;
bool g_ready = false;
bool g_inFlight = false;
double g_lastFetch = -1.0e9;

/** Seconds between automatic refreshes while the directory is open. */
constexpr double kRefreshEvery = 60.0;

std::string trimSlashes(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

/** One fetch. Runs on the worker; every failure ends in an empty list. */
void fetchInto(const std::string& base) {
    HttpRequest req;
    req.method = "GET";
    req.url = base + "/mods";
    // Short, for the board's reason: nothing here is worth making somebody
    // wait for, and a directory slow to answer is an empty one as far as this
    // screen is concerned.
    req.timeoutMs = 6000;
    req.allowInsecure = llm::isLocal(base);
    req.maxResponseBytes = (uint32_t)odmoddir::Limits::kDocument;
    const HttpResponse res = httpRequest(req);

    std::vector<odmoddir::Listing> got;
    if (res.ok()) {
        std::string why;
        got = odmoddir::parse(res.body, why);
        // `why` is for a log and never for the player: see ModDir.h.
    }
    std::lock_guard<std::mutex> g(g_lock);
    g_fetched = std::move(got);
    g_ready = true;
}

/** The colour a scan result is allowed to be drawn in. */
Color scanTint(odmoddir::Scan s) {
    switch (s) {
        case odmoddir::Scan::Clean:   return Color{120, 190, 130, 255};
        case odmoddir::Scan::Flagged: return Color{225, 120, 110, 255};
        // UNKNOWN IS NOT GREEN. "Nobody has ever scanned this file" is the
        // ordinary state of a new mod and also the state of one written this
        // morning to attack somebody; painting it as clean would be the screen
        // telling a lie the service is careful not to tell.
        case odmoddir::Scan::Unknown: return Color{210, 175, 95, 255};
        default:                      return Color{130, 135, 148, 255};
    }
}

const char* scanWord(odmoddir::Scan s) {
    switch (s) {
        case odmoddir::Scan::Clean:   return "checked, nothing reported";
        case odmoddir::Scan::Flagged: return "flagged";
        case odmoddir::Scan::Unknown: return "never scanned";
        default:                      return "not checked";
    }
}

}  // namespace

void Game::modDirOpen() {
    m_modDirPage = true;
    m_modDirScroll = 0;
    m_modDirNote.clear();
    modDirRefresh(true);
}

void Game::modDirRefresh(bool force) {
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

void Game::pumpModDir() {
    std::vector<odmoddir::Listing> got;
    bool have = false;
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_ready) { g_ready = false; got.swap(g_fetched); have = true; }
        m_modDirBusy = g_inFlight;
    }
    if (have) m_modDirListings = std::move(got);
    if (m_modDirPage) modDirRefresh(false);
}

void Game::drawModDirectory(Vector2 mouse, bool click) {
    const Color accent = hexToColor(m_config.accent());
    const int left = 40;
    const int listW = m_screenW - 80;
    int y = 128;

    DrawText(T("Mods people have published"), left, 78, 26, WHITE);

    const int btnY = m_screenH - 56;
    const MpButton back = buttonAt((float)left, (float)btnY, 150.0f, 36.0f, mouse);
    drawButton(back, "Back to my mods", 16, Color{34, 36, 44, 220}, Color{90, 95, 110, 190});
    if (click && back.hovered) { m_modDirPage = false; return; }

    const MpButton again = buttonAt((float)(left + 162), (float)btnY, 134.0f, 36.0f, mouse);
    drawButton(again, m_modDirBusy ? "Refreshing..." : "Refresh", 16,
               Color{34, 40, 52, 230}, Color{100, 120, 150, 200}, !m_modDirBusy);
    if (click && again.hovered && !m_modDirBusy) modDirRefresh(true);

    const MpButton site = buttonAt((float)(m_screenW - 230), (float)btnY, 190.0f, 36.0f, mouse);
    drawButton(site, "Open the directory", 16, Color{40, 44, 58, 230},
               Color{110, 125, 160, 200});
    if (click && site.hovered) {
        odlink::open("https://opendoctrines.pages.dev/mods");
        m_modFeedback = "Opened the mod directory in your browser";
        m_modFeedbackTimer = 3.0f;
    }

    if (m_config.accountIssuer.empty()) {
        DrawText(T("This build has no account service, so there is no directory."),
                 left, y, 17, Color{130, 135, 150, 255});
        return;
    }

    if (m_modDirListings.empty()) {
        DrawText(m_modDirBusy ? T("Looking...") : T("No mods have been published yet."),
                 left, y, 19, Color{170, 180, 200, 255});
        y += 30;
        wrapText(T("Anyone with an account can publish one. The file stays on the author's own host -- this is a list of what exists, and the game never downloads a mod by itself."),
                 left, y, listW - 40, 15, Color{120, 126, 140, 255}, true);
        return;
    }

    // ── the rows ──
    const int rowH = 86;
    const int listBottom = btnY - 14;
    int maxVisible = std::max(1, (listBottom - y) / rowH);
    int maxScroll = std::max(0, (int)m_modDirListings.size() - maxVisible);
    m_modDirScroll = std::clamp(m_modDirScroll, 0, maxScroll);
    if (maxScroll > 0) {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) m_modDirScroll = std::clamp(
            m_modDirScroll - (int)wheel, 0, maxScroll);
    }

    for (int i = m_modDirScroll; i < (int)m_modDirListings.size(); ++i) {
        const int ry = y + (i - m_modDirScroll) * rowH;
        if (ry + rowH > listBottom) break;
        const odmoddir::Listing& l = m_modDirListings[(size_t)i];

        Rectangle row{(float)left, (float)ry, (float)listW, (float)rowH - 8};
        const bool hovered = CheckCollisionPointRec(mouse, row);
        DrawRectangleRounded(row, 0.08f, 8,
                             Color{255, 255, 255, (unsigned char)(hovered ? 18 : 8)});

        DrawText(l.name.c_str(), left + 16, ry + 12, 19, WHITE);
        const int nameW = MeasureText(l.name.c_str(), 19);
        if (!l.version.empty())
            DrawText(("v" + l.version).c_str(), left + 26 + nameW, ry + 15, 15,
                     Color{130, 136, 150, 255});
        if (!l.by.empty())
            DrawText(("by " + l.by).c_str(), left + 16, ry + 36, 14,
                     Color{140, 146, 162, 255});
        if (!l.summary.empty())
            DrawText(l.summary.c_str(), left + 16, ry + 56, 14,
                     Color{155, 160, 175, 255});

        // The three things worth knowing before installing somebody's code:
        // where it runs, whether it changes how turns resolve, and what the
        // check found. Right-aligned so they line up down the list.
        int cx = left + listW - 16;
        auto chip = [&](const char* text, Color tint) {
            const int w = MeasureText(text, 13) + 16;
            cx -= w + 8;
            DrawRectangleRounded({(float)cx, (float)(ry + 12), (float)w, 22.0f}, 0.4f, 6,
                                 Color{255, 255, 255, 12});
            DrawText(text, cx + 8, ry + 16, 13, tint);
        };
        chip(scanWord(l.scan), scanTint(l.scan));
        if (l.changesTurns) chip("changes turns", accent);
        chip(l.sideWord(), Color{140, 150, 170, 255});

        // Opening a page is as far as this goes. A listing with no https page
        // simply does not offer the button -- see ModDir.h.
        if (!l.page.empty()) {
            const MpButton open = buttonAt((float)(left + listW - 150),
                                           (float)(ry + rowH - 44), 134.0f, 30.0f, mouse);
            drawButton(open, "Open its page", 15, Color{38, 48, 40, 230},
                       Color{120, 170, 130, 200});
            if (click && open.hovered) {
                odlink::open(l.page.c_str());
                m_modFeedback = "Opened " + l.name + " in your browser";
                m_modFeedbackTimer = 3.0f;
            }
        }
    }

    if (maxScroll > 0)
        DrawText(TextFormat("%d / %d", m_modDirScroll + 1, (int)m_modDirListings.size() ),
                 left + listW - 70, y - 22, 13, Color{110, 116, 130, 255});
}
