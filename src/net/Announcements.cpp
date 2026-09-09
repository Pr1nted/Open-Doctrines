#include "Announcements.h"

#include "HttpClient.h"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace odnews {
namespace {

/// Trim, and refuse anything with a control character in it.
bool cleanText(std::string& v, size_t maxLen) {
    if (v.size() > maxLen) return false;
    // Control characters are refused rather than stripped. They cannot appear
    // in anything legitimate here, and a document containing them is a document
    // doing something other than what it says -- which is a reason to drop the
    // entry, not to tidy it up and render it anyway.
    for (unsigned char c : v)
        if (c < 0x20 && c != '\n' && c != '\t') return false;
    const size_t a = v.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { v.clear(); return true; }
    const size_t b = v.find_last_not_of(" \t\r\n");
    v = v.substr(a, b - a + 1);
    return true;
}

/**
 * The objects inside the "items" array, as substrings.
 *
 * Written by hand rather than with a JSON library ON PURPOSE: this parses a
 * reply from the network, and src/net/HttpClient.h explains why that path does
 * not get a general parser. It walks braces, respects strings and escapes, and
 * stops at the first thing it does not understand.
 */
std::vector<std::string> objectsIn(const std::string& json, const std::string& key,
                                   size_t maxItems) {
    std::vector<std::string> out;
    const size_t k = json.find("\"" + key + "\"");
    if (k == std::string::npos) return out;
    const size_t open = json.find('[', k);
    if (open == std::string::npos) return out;

    size_t i = open + 1;
    while (i < json.size() && out.size() < maxItems) {
        while (i < json.size() && (isspace((unsigned char)json[i]) || json[i] == ',')) ++i;
        if (i >= json.size() || json[i] == ']') break;
        if (json[i] != '{') break;             // not an object: stop, do not guess

        const size_t start = i;
        int depth = 0;
        bool inStr = false, esc = false;
        for (; i < json.size(); ++i) {
            const char c = json[i];
            if (esc) { esc = false; continue; }
            if (inStr) {
                if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == '{') ++depth;
            else if (c == '}') {
                if (--depth == 0) { ++i; break; }
            }
        }
        if (depth != 0) break;                 // unterminated: refuse the rest
        out.push_back(json.substr(start, i - start));
    }
    return out;
}

/**
 * Is `key` written in this object at all?
 *
 * Needed because httpJsonString returns an EMPTY STRING for a value it could
 * not read -- one longer than the cap it was given, or with a broken escape in
 * it. Empty and "too long to accept" are the same answer from the helper, and
 * they must not be the same answer here: the first means the field was left
 * out, the second means the document is wrong and the entry goes.
 */
bool fieldPresent(const std::string& obj, const std::string& key) {
    return obj.find("\"" + key + "\"") != std::string::npos;
}

}  // namespace

Action actionFromName(const std::string& name) {
    if (name == "join") return Action::JoinGame;
    if (name == "community") return Action::Community;
    if (name == "account") return Action::Account;
    return Action::None;                       // including "", and anything new
}

const char* actionName(Action a) {
    switch (a) {
        case Action::JoinGame:  return "join";
        case Action::Community: return "community";
        case Action::Account:   return "account";
        case Action::None:      break;
    }
    return "";
}

Item::TimeStyle timeStyleFromName(const std::string& name) {
    if (name == "local") return Item::TimeStyle::Local;
    if (name == "countdown") return Item::TimeStyle::Countdown;
    return Item::TimeStyle::None;
}

std::string formatLocal(long long unixSeconds) {
    if (unixSeconds <= 0) return {};
    const std::time_t when = (std::time_t)unixSeconds;
    std::tm tmv{};
#ifdef _WIN32
    if (localtime_s(&tmv, &when) != 0) return {};
#else
    if (!localtime_r(&when, &tmv)) return {};
#endif
    char out[64];
    // Weekday included, because "is that this Saturday?" is the question
    // somebody actually has about a tournament, and a bare date makes them
    // count. 24-hour, because the game's own clock is.
    if (std::strftime(out, sizeof(out), "%a %d %b, %H:%M", &tmv) == 0) return {};
    return out;
}

std::string formatCountdown(long long when, long long now) {
    if (when <= 0) return {};
    const long long delta = when - now;
    const long long a = delta < 0 ? -delta : delta;

    // ── THE UNITS SHRINK AS IT GETS CLOSE ──
    //
    // "in 3 days" is what you want a week out and useless in the last hour;
    // "in 10800 seconds" is useless always. Each band is chosen so the number
    // stays small and the precision arrives exactly when it starts to matter.
    std::string span;
    if (a >= 172800) {                      // 2 days or more
        span = std::to_string(a / 86400) + " days";
    } else if (a >= 86400) {
        span = "1 day";
    } else if (a >= 3600) {
        const long long h = a / 3600, m = (a % 3600) / 60;
        span = std::to_string(h) + "h";
        if (m > 0) span += " " + std::to_string(m) + "m";
    } else if (a >= 60) {
        span = std::to_string(a / 60) + "m";
    } else {
        span = std::to_string(a) + "s";
    }

    // AFTER IS NOT NOTHING. A countdown that empties itself the moment it
    // reaches zero takes the answer away at the point most people are looking:
    // somebody arriving late wants to know how late.
    return delta >= 0 ? "in " + span : span + " ago";
}

std::vector<Item> live(const std::vector<Item>& items, long long now) {
    std::vector<Item> out;
    for (const Item& it : items)
        if (!it.expired(now)) out.push_back(it);
    return out;
}

bool looksLikeInviteCode(const std::string& param) {
    if (param.empty() || param.size() > 32) return false;
    for (unsigned char c : param) {
        const bool ok = std::isalnum(c) || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

std::vector<Item> parseDocument(const std::string& json, std::string& error) {
    error.clear();
    std::vector<Item> out;

    if (json.size() > Limits::kDocument) {
        error = "announcement document too large";
        return out;
    }
    if (json.find("\"items\"") == std::string::npos) {
        // Not an error: a service with nothing to say answers with a document
        // that has no items in it, and an empty board is the correct outcome.
        return out;
    }

    for (const std::string& obj : objectsIn(json, "items", Limits::kMaxItems)) {
        Item it;
        // ── READ ONE BYTE PAST THE LIMIT, DELIBERATELY ──
        //
        // httpJsonString TRUNCATES at the cap it is given. Asking it for
        // exactly the limit would hand back a value of exactly the limit's
        // length, which then passes the length check -- so an over-long title
        // would be silently shortened and shown rather than refused. Asking for
        // one more byte is what lets cleanText see that it was too long.
        it.id    = httpJsonString(obj, "id",    (uint32_t)Limits::kId + 1);
        it.title = httpJsonString(obj, "title", (uint32_t)Limits::kTitle + 1);
        it.body  = httpJsonString(obj, "body",  (uint32_t)Limits::kBody + 1);

        // Present but unreadable is a refusal, not a missing field. See
        // fieldPresent.
        if (fieldPresent(obj, "id") && it.id.empty()) continue;
        if (fieldPresent(obj, "title") && it.title.empty()) continue;
        if (fieldPresent(obj, "body") && it.body.empty()) continue;

        if (!cleanText(it.id, Limits::kId)) continue;
        if (!cleanText(it.title, Limits::kTitle)) continue;
        if (!cleanText(it.body, Limits::kBody)) continue;
        // An entry with nothing to read is not an entry. The id is required
        // because the game keys "already seen" off it.
        if (it.id.empty() || (it.title.empty() && it.body.empty())) continue;

        // ── WHEN IT WAS POSTED, AND WHEN IT STOPS BEING TRUE ──
        //
        // Unix seconds, because a board that says "Saturday" is wrong by Sunday
        // and there is nobody awake to take it down. `postedAt` is shown to the
        // player; `until` is the entry taking itself off the board, which is the
        // difference between announcing a tournament and having to remember to
        // un-announce it.
        //
        // Both are refused if they are not plausible: a timestamp before the
        // game existed, or centuries out, is a broken document rather than a
        // very old announcement.
        it.postedAt = httpJsonNumber(obj, "postedAt", 0);
        it.until    = httpJsonNumber(obj, "until", 0);
        constexpr long long kEarliest = 1600000000LL;   // 2020
        constexpr long long kLatest   = 4102444800LL;   // 2100
        if (it.postedAt != 0 && (it.postedAt < kEarliest || it.postedAt > kLatest)) continue;
        if (it.until != 0 && (it.until < kEarliest || it.until > kLatest)) continue;

        it.eventAt = httpJsonNumber(obj, "eventAt", 0);
        if (it.eventAt != 0 && (it.eventAt < kEarliest || it.eventAt > kLatest)) continue;
        it.timeStyle = timeStyleFromName(httpJsonString(obj, "timeStyle", 16));
        // A style with nothing to show, or an instant with no style, is a
        // document that means something the game cannot see. Refused rather
        // than half-drawn -- the usual rule here.
        if (it.timeStyle != Item::TimeStyle::None && it.eventAt == 0) continue;
        if (fieldPresent(obj, "timeStyle") && it.timeStyle == Item::TimeStyle::None) continue;

        // ── THE BUTTON ──
        //
        // Optional, and refused as a whole if any part of it is wrong. A button
        // that is drawn but does nothing, or does something other than its
        // label says, is worse than no button.
        const std::string label  = httpJsonString(obj, "buttonLabel",  (uint32_t)Limits::kLabel + 1);
        const std::string action = httpJsonString(obj, "buttonAction", 33);
        const std::string param  = httpJsonString(obj, "buttonParam",  (uint32_t)Limits::kParam + 1);
        if (!label.empty() || !action.empty()) {
            Button b;
            b.label = label;
            b.action = actionFromName(action);
            b.param = param;
            if (fieldPresent(obj, "buttonLabel") && b.label.empty()) continue;
            if (fieldPresent(obj, "buttonParam") && b.param.empty()) continue;
            if (!cleanText(b.label, Limits::kLabel)) continue;
            if (!cleanText(b.param, Limits::kParam)) continue;
            // An unknown action is the case that matters: a document naming
            // something this build has never heard of gets no button at all,
            // rather than a button wired to whatever happens to be nearby.
            if (b.action == Action::None) continue;
            if (b.label.empty()) continue;
            // Each action decides what its own parameter must look like. The
            // document does not get to define that.
            if (b.action == Action::JoinGame && !looksLikeInviteCode(b.param)) continue;
            if (b.action != Action::JoinGame && !b.param.empty()) continue;
            it.button = b;
        }

        out.push_back(std::move(it));
    }
    return out;
}

}  // namespace odnews
