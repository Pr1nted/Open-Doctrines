#include "Feedback.h"

#include "Config.h"
#include "GameStructs.h"
#include "i18n/Locale.h"
#include "net/AccountClient.h"
#include "net/HttpClient.h"

#include "raylib.h"

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <sstream>
#include <thread>

namespace feedback {
namespace {

struct State {
    std::atomic<bool>  busy{false};
    std::atomic<int>   status{(int)Status::Idle};
    std::mutex         lock;
    std::string        message;
    std::thread        worker;
    double             lastSendAt = -1e9;   ///< GetTime() of the last accepted send
};

/**
 * Deliberately leaked, and never destroyed.
 *
 * A plain function-local static is destroyed at exit, and destroying a
 * `std::thread` that is still joinable calls `std::terminate`. So a player who
 * sent a report and quit the game inside the request's fifteen-second timeout
 * would have the game abort on the way out -- reporting a bug would itself look
 * like a crash, which is about the worst failure this feature could have.
 *
 * Joining at exit instead would make quitting hang for up to fifteen seconds on
 * a bad connection, and detaching leaves a live thread writing into an object
 * that is being destroyed. Never destroying it costs one small allocation for
 * the life of the process and makes both problems impossible.
 */
State& state() {
    static State* s = new State();
    return *s;
}

/// Seconds between sends. Slow enough to stop a jammed key, fast enough that a
/// player who noticed a second bug on the same screen is not made to wait.
constexpr double kCooldownSeconds = 45.0;

/// Reports per day from one installation. The service enforces its own; this
/// one exists so the refusal happens before the player types the report.
constexpr int kDailyCap = 10;

std::string today() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

/**
 * Remove the player's home directory from a string.
 *
 * Every save and data path on a desktop build starts with it, and on all three
 * platforms it contains the account name -- so a diagnostics block that
 * mentions a file mentions a person. Replaced rather than dropped: "~/..." is
 * still a useful thing for a maintainer to read.
 */
}  // namespace

std::string scrubPersonalPaths(std::string text) {
    const char* vars[] = {"HOME", "USERPROFILE"};
    for (const char* v : vars) {
        const char* home = std::getenv(v);
        if (!home || !*home) continue;
        const std::string h = home;
        if (h.size() < 4) continue;             // "/" or a drive root: not a home
        for (size_t at = text.find(h); at != std::string::npos; at = text.find(h, at + 1))
            text.replace(at, h.size(), "~");
    }
    return text;
}

namespace {

const char* platformName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__EMSCRIPTEN__)
    return "web";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

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
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

}  // namespace

const char* platform() { return platformName(); }

const char* categoryId(Category c) {
    switch (c) {
        case Category::UI:          return "ui";
        case Category::AI:          return "ai";
        case Category::Data:        return "data";
        case Category::Multiplayer: return "multiplayer";
        case Category::Scripting:   return "scripting";
        case Category::Mods:        return "mods";
        case Category::Security:    return "security";
        default:                    return "other";
    }
}

const char* kindId(Kind k) {
    switch (k) {
        case Kind::Suggestion: return "suggestion";
        default:               return "bug";
    }
}

bool signedIn() {
    const AccountClient& c = AccountClient::get();
    return c.configured() && c.account().valid() && !c.sessionToken().empty();
}

const char* ratingUrl() { return "https://pr1nted.itch.io/open-doctrines"; }

const char* categoryLabel(Category c) {
    // Translated by the caller. Kept here so the form and the summary cannot
    // disagree about what a category is called.
    switch (c) {
        case Category::UI:          return "Interface";
        case Category::AI:          return "AI behaviour";
        case Category::Data:        return "Maps and data";
        case Category::Multiplayer: return "Multiplayer";
        case Category::Scripting:   return "Map scripting";
        case Category::Mods:        return "Mods and the SDK";
        case Category::Security:    return "Security";
        default:                    return "Something else";
    }
}

const std::string& installId(Config& cfg) {
    static std::string id;
    if (!id.empty()) return id;
    if (!cfg.installId.empty()) { id = cfg.installId; return id; }
    // Random, 22 characters of an alphabet with no lookalikes to worry about --
    // this is never read aloud or typed, only sent.
    static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    id.reserve(22);
    for (int i = 0; i < 22; ++i) id += kAlphabet[GetRandomValue(0, 35)];
    cfg.installId = id;      // minted once; the caller saves the config
    return id;
}

bool canSend(Config& cfg, std::string& reason) {
    reason.clear();
    State& s = state();
    if (!signedIn()) {
        // First, because it is the only one the player can do something about
        // and the only one that makes the form pointless to fill in.
        // Says WHERE, because "please sign in" with no route is a dead end --
        // the account screen is on the main menu, which is not where the player
        // is standing when they press Report.
        reason = T("Please sign in first (Main menu > Account). Reports are published, "
                   "and signed with your nickname.");
        return false;
    }
    if (s.busy.load()) {
        reason = T("A report is already being sent.");
        return false;
    }

    if (cfg.feedbackCountedOn != today()) {          // a new day resets the count
        cfg.feedbackCountedOn = today();
        cfg.feedbackSentToday = 0;
    }
    if (cfg.feedbackSentToday >= kDailyCap) {
        reason = T("You have sent several reports today. Thank you -- please continue tomorrow.");
        return false;
    }

    const double since = GetTime() - s.lastSendAt;
    if (since < kCooldownSeconds) {
        reason = TextFormat(T("Please wait %d seconds before sending another."),
                            (int)(kCooldownSeconds - since) + 1);
        return false;
    }
    if (cfg.feedbackEndpoint.empty()) {
        reason = T("This build has no reporting service configured.");
        return false;
    }
    return true;
}

Status status() { return (Status)state().status.load(); }

const std::string& message() {
    State& s = state();
    std::lock_guard<std::mutex> g(s.lock);
    static std::string copy;
    copy = s.message;
    return copy;
}

void clearMessage() {
    State& s = state();
    std::lock_guard<std::mutex> g(s.lock);
    s.message.clear();
    s.status.store((int)Status::Idle);
}

bool send(const Report& report, Config& cfg, const std::string& configPath,
          const std::string& version) {
    State& s = state();
    std::string why;
    if (!canSend(cfg, why)) return false;
    const std::string endpoint = cfg.feedbackEndpoint;
    if (endpoint.empty()) return false;
    const std::string platform = platformName();

    std::ostringstream body;
    body << "{\"kind\":\"" << kindId(report.kind) << "\""
         << ",\"category\":\"" << categoryId(report.category) << "\""
         << ",\"title\":\"" << escapeJson(report.title) << "\""
         << ",\"body\":\"" << escapeJson(report.body) << "\""
         << ",\"version\":\"" << escapeJson(version) << "\""
         << ",\"platform\":\"" << escapeJson(platform) << "\""
         << ",\"install\":\"" << escapeJson(installId(cfg)) << "\"";
    if (report.anonymous) body << ",\"anonymous\":true";
    if (!report.diagnostics.empty())
        body << ",\"diagnostics\":\"" << escapeJson(report.diagnostics) << "\"";
    body << "}";

    std::string url = endpoint;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/feedback";

    // THE TOKEN GOES ONLY TO THE SERVICE THAT ISSUED IT.
    //
    // This is the account credential -- it can rename the account, unlink a
    // provider, delete it (see AccountClient.h, which says it must never reach
    // a game server). feedbackEndpoint defaults to the account issuer but is a
    // config field, so a modified config could point it at anyone. Comparing
    // the two before attaching the token means a redirected endpoint gets a
    // report it will refuse, rather than the keys to the account.
    std::string tokenForIssuer;
    {
        std::string issuer = cfg.accountIssuer;
        while (!issuer.empty() && issuer.back() == '/') issuer.pop_back();
        std::string base = endpoint;
        while (!base.empty() && base.back() == '/') base.pop_back();
        if (!issuer.empty() && issuer == base) tokenForIssuer = AccountClient::get().sessionToken();
    }

    // Counted BEFORE the reply. A player who sends ten reports that all fail is
    // still a player who pressed Send ten times, and the cap is there to stop
    // the pressing rather than to ration successes.
    cfg.feedbackSentToday += 1;
    cfg.save(configPath);

    s.busy.store(true);
    s.status.store((int)Status::Sending);
    s.lastSendAt = GetTime();

    // The three sentences the worker might need, TRANSLATED HERE, on the game
    // thread, and captured by value.
    //
    // T() interns into a shared arena (see i18n/Locale.cpp) with no lock, and
    // the renderer calls it hundreds of times a frame. Calling it from the
    // request thread is a data race on that arena against every label the game
    // is drawing at the time -- rare, unreproducible, and a crash rather than
    // a wrong word.
    const std::string sentMsg   = T("Sent. Thank you -- this is genuinely useful.");
    const std::string failedMsg = T("Could not reach the reporting service. "
                                    "Your report was not sent.");

    auto job = [&s, url, payload = body.str(), tokenForIssuer, sentMsg, failedMsg] {
        HttpRequest req;
        req.method = "POST";
        req.url = url;
        req.body = payload;
        req.bearer = tokenForIssuer;
        req.timeoutMs = 15000;
        const HttpResponse res = httpRequest(req);

        std::lock_guard<std::mutex> g(s.lock);
        if (res.ok()) {
            s.status.store((int)Status::Sent);
            s.message = sentMsg;
        } else {
            s.status.store((int)Status::Failed);
            // The service's own sentence when it gave one: it knows why better
            // than this does ("that is twelve reports today", say).
            s.message = !res.error.empty() ? res.error : failedMsg;
        }
    };

#ifdef __EMSCRIPTEN__
    // Single-threaded, and httpRequest is a stub there: run it inline so the
    // player gets the sentence rather than a frozen tab.
    job();
    s.busy.store(false);
#else
    if (s.worker.joinable()) s.worker.join();
    s.worker = std::thread([&s, job = std::move(job)] {
        job();
        s.busy.store(false);
    });
#endif
    return true;
}

void pump() {
    State& s = state();
#ifndef __EMSCRIPTEN__
    if (!s.busy.load() && s.worker.joinable()) s.worker.join();
#endif
}

}  // namespace feedback
