#include "Lfg.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "Announcements.h"   // looksLikeInviteCode: the game's one rule for a code
#include "HttpClient.h"

namespace odlfg {
namespace {

std::string clipped(const std::string& value, size_t max) {
    return value.size() <= max ? value : value.substr(0, max);
}

std::string trimmed(const std::string& value) {
    size_t a = 0, b = value.size();
    while (a < b && (unsigned char)value[a] <= ' ') a++;
    while (b > a && (unsigned char)value[b - 1] <= ' ') b--;
    return value.substr(a, b - a);
}

/**
 * Anything that could be a link.
 *
 * The rule is "promote Open Doctrines games only", and a listing carries an
 * invite code rather than an address, so a link has nothing legitimate to do
 * here. Refusing the SHAPE means nobody has to judge where a link points.
 */
bool looksLikeLink(const std::string& text) {
    std::string lower;
    lower.reserve(text.size());
    for (char c : text) lower += (char)std::tolower((unsigned char)c);
    // URL needles matched against what a player typed, never drawn. Without
    // the marker the extractor offers ".com" and ".gg" to 44 translations.
    // i18n-ignore
    static const char* kNeedles[] = {
        "http://", "https://", "www.", "discord.gg", ".com", ".net", ".org", ".io",
        ".gg", ".dev", ".xyz", ".ru", ".me",
    };
    for (const char* needle : kNeedles) {
        if (lower.find(needle) != std::string::npos) return true;
    }
    return false;
}

/** Letters, digits, spaces and light punctuation. No markup, no mentions. */
bool plainText(const std::string& text) {
    for (unsigned char c : text) {
        if (c >= 0x80) continue;   // UTF-8 continuation bytes: other scripts are fine
        const bool allowed = std::isalnum(c) || std::isspace(c) ||
                             std::strchr(" .,!?'()/+:;-", c) != nullptr;
        if (!allowed) return false;
    }
    return true;
}

Kind kindFromName(const std::string& name) {
    return name == "looking" ? Kind::Looking : Kind::Hosting;
}

Mode modeFromName(const std::string& name) {
    return name == "longform" ? Mode::Longform : Mode::Rapid;
}

/**
 * One listing, from the text of ONE object.
 *
 * The caller passes the object alone rather than an offset into the whole
 * document, because httpJsonString() searches FORWARD from where it is told to
 * start and does not stop at the end of an object. Given an offset, a listing
 * that omits a field would silently read the NEXT listing's -- and for `code`
 * that means a "looking" entry showing a stranger's invite code. Cutting the
 * object out first makes that impossible rather than merely unlikely.
 */
bool readListing(const std::string& json, Listing& out) {
    Listing item;
    item.id = httpJsonString(json, "id", Limits::kIdChars);
    item.nick = httpJsonString(json, "nick", Limits::kNickChars);
    item.map = httpJsonString(json, "map", Limits::kMapChars);
    if (item.id.empty() || item.map.empty()) return false;

    item.kind = kindFromName(httpJsonString(json, "kind", 16));
    item.mode = modeFromName(httpJsonString(json, "mode", 16));
    item.code = httpJsonString(json, "code", Limits::kCodeChars);
    item.language = httpJsonString(json, "language", Limits::kLanguageChars);
    item.region = httpJsonString(json, "region", Limits::kRegionChars);
    item.note = httpJsonString(json, "note", Limits::kNoteChars);
    item.turnSeconds = (int)httpJsonNumber(json, "turnSeconds", 0);
    item.turnHours = (int)httpJsonNumber(json, "turnHours", 0);
    item.slotsTaken = (int)httpJsonNumber(json, "slotsTaken", 0);
    item.slotsTotal = (int)httpJsonNumber(json, "slotsTotal", 0);
    item.createdAt = httpJsonNumber(json, "createdAt", 0);
    item.expiresAt = httpJsonNumber(json, "expiresAt", 0);
    item.reports = (int)httpJsonNumber(json, "reports", 0);

    // A hosting listing with no code cannot be joined and would draw a button
    // that does nothing, so it is refused rather than shown half-working.
    if (item.kind == Kind::Hosting && item.code.empty()) return false;
    // A code that could not have been issued is dropped here, before it can be
    // handed to the join path. The rule is odnews::looksLikeInviteCode and not
    // a second copy of it: the announcement board and this board hand codes to
    // the SAME join path, so they must agree on what a code is.
    if (!item.code.empty() && !odnews::looksLikeInviteCode(item.code)) return false;
    // A pace we cannot put into words is a listing we cannot describe, so it is
    // dropped rather than drawn as "0 s a turn".
    if (item.mode == Mode::Rapid) {
        if (item.turnSeconds < Limits::kTurnSecondsMin || item.turnSeconds > Limits::kTurnSecondsMax) return false;
    } else if (item.turnHours < Limits::kTurnHoursMin || item.turnHours > Limits::kTurnHoursMax) {
        return false;
    }
    if (item.note.size() > Limits::kNoteChars || looksLikeLink(item.note)) item.note.clear();
    if (item.reports < 0) item.reports = 0;
    if (item.slotsTotal < 0 || item.slotsTotal > Limits::kSlotsMax) item.slotsTotal = 0;
    if (item.slotsTaken < 0 || item.slotsTaken > item.slotsTotal) item.slotsTaken = 0;

    out = item;
    return true;
}

}  // namespace

std::string Listing::paceLine() const {
    if (mode == Mode::Rapid) {
        if (turnSeconds >= 120 && turnSeconds % 60 == 0) {
            return "Rapid, " + std::to_string(turnSeconds / 60) + " min a turn";
        }
        return "Rapid, " + std::to_string(turnSeconds) + " s a turn";
    }
    if (turnHours % 24 == 0 && turnHours >= 24) {
        const int days = turnHours / 24;
        return "Longform, " + std::to_string(days) + (days == 1 ? " day a turn" : " days a turn");
    }
    return "Longform, " + std::to_string(turnHours) + " h a turn";
}

std::string Listing::seatsLine() const {
    if (slotsTotal <= 0) return {};
    return std::to_string(slotsTaken) + "/" + std::to_string(slotsTotal) + " players";
}

std::string Listing::closesIn(long long now) const {
    if (expiresAt <= 0) return {};
    const long long left = expiresAt - now;
    if (left <= 0) return "closing";
    if (left < 60) return "closes in under a minute";
    if (left < 3600) return "closes in " + std::to_string(left / 60) + " min";
    const long long hours = left / 3600;
    return "closes in " + std::to_string(hours) + (hours == 1 ? " hour" : " hours");
}

std::vector<Listing> parseBoard(const std::string& json, std::string& error) {
    error.clear();
    std::vector<Listing> out;
    if (json.empty()) {
        error = "no reply";
        return out;
    }
    if (json.size() > Limits::kDocument) {
        error = "board document too large";
        return out;
    }
    const size_t listAt = json.find("\"listings\"");
    if (listAt == std::string::npos) {
        error = "no listings in the reply";
        return out;
    }
    // Walk object by object. A listing that does not read is skipped whole; a
    // board that renders somebody's half-parsed entry is worse than a short one.
    size_t at = json.find('[', listAt);
    if (at == std::string::npos) {
        error = "listings is not a list";
        return out;
    }
    int depth = 0;
    size_t objectStart = std::string::npos;
    for (size_t i = at; i < json.size() && out.size() < Limits::kItems; i++) {
        const char c = json[i];
        if (c == '{') {
            if (depth == 0) objectStart = i;
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0 && objectStart != std::string::npos) {
                Listing item;
                if (readListing(json.substr(objectStart, i - objectStart + 1), item)) {
                    out.push_back(item);
                }
                objectStart = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return out;
}

std::vector<Listing> live(const std::vector<Listing>& all, long long now) {
    std::vector<Listing> out;
    out.reserve(all.size());
    for (const Listing& item : all) {
        if (item.expiresAt == 0 || item.expiresAt > now) out.push_back(item);
    }
    return out;
}

std::string problemWith(const Draft& draft) {
    const std::string map = trimmed(draft.map);
    if (map.empty()) return "Say which map the game is on.";
    if (map.size() > Limits::kMapChars) return "That map name is too long.";
    if (!plainText(map)) return "The map name may use letters, digits and simple punctuation.";

    if (draft.mode == Mode::Rapid) {
        if (draft.turnSeconds < Limits::kTurnSecondsMin || draft.turnSeconds > Limits::kTurnSecondsMax) {
            return "A rapid game's turn is between 30 seconds and an hour.";
        }
    } else if (draft.turnHours < Limits::kTurnHoursMin || draft.turnHours > Limits::kTurnHoursMax) {
        return "A longform game's turn is between an hour and a week.";
    }

    if (draft.kind == Kind::Hosting) {
        const std::string code = trimmed(draft.code);
        if (code.empty()) return "A hosting listing needs the invite code from your lobby.";
        if (!odnews::looksLikeInviteCode(code)) return "That invite code does not look right.";
        if (draft.slotsTotal < Limits::kSlotsMin || draft.slotsTotal > Limits::kSlotsMax) {
            return "Say how many players the game seats.";
        }
        if (draft.slotsTaken < 0 || draft.slotsTaken > draft.slotsTotal) {
            return "Players in the game cannot exceed the seats.";
        }
    } else if (!trimmed(draft.code).empty()) {
        return "A looking-for-a-game listing carries no invite code. Tag it as hosting instead.";
    }

    if (draft.minutes != 0 &&
        (draft.minutes < Limits::kMinutesMin || draft.minutes > Limits::kMinutesMax)) {
        return "A listing lasts between 15 minutes and 6 hours.";
    }

    const std::string note = trimmed(draft.note);
    if (note.size() > Limits::kNoteChars) return "That note is too long.";
    if (looksLikeLink(note)) {
        return "Listings carry an invite code, not links. Post the code from your lobby instead.";
    }
    if (!plainText(note)) return "The note may use letters, digits and simple punctuation.";
    for (const std::string& value : {trimmed(draft.language), trimmed(draft.region)}) {
        if (!plainText(value)) return "Language and region may use letters and digits.";
    }
    return {};
}

std::vector<Listing> view(const std::vector<Listing>& all, Filter f) {
    std::vector<Listing> out;
    out.reserve(all.size());
    for (const Listing& l : all) {
        if (f == Filter::Hosting && l.kind != Kind::Hosting) continue;
        if (f == Filter::Looking && l.kind != Kind::Looking) continue;
        out.push_back(l);
    }
    // A stable sort, so everything else keeps the order the service chose --
    // which is newest first, and the only ordering the board promises.
    std::stable_sort(out.begin(), out.end(), [](const Listing& a, const Listing& b) {
        const bool af = a.kind == Kind::Hosting && a.slotsTotal > 0 &&
                        a.slotsTaken >= a.slotsTotal;
        const bool bf = b.kind == Kind::Hosting && b.slotsTotal > 0 &&
                        b.slotsTaken >= b.slotsTotal;
        return !af && bf;
    });
    return out;
}

Counts count(const std::vector<Listing>& all) {
    Counts c;
    for (const Listing& l : all) {
        if (l.kind == Kind::Hosting) c.hosting++;
        else c.looking++;
    }
    return c;
}

std::string postBody(const Draft& draft) {
    std::string out = "{";
    out += "\"kind\":\"" + std::string(kindName(draft.kind)) + "\"";
    out += ",\"map\":\"" + httpJsonEscape(clipped(trimmed(draft.map), Limits::kMapChars)) + "\"";
    out += ",\"mode\":\"" + std::string(modeName(draft.mode)) + "\"";
    if (draft.mode == Mode::Rapid) {
        out += ",\"turnSeconds\":" + std::to_string(draft.turnSeconds);
    } else {
        out += ",\"turnHours\":" + std::to_string(draft.turnHours);
    }
    if (draft.kind == Kind::Hosting) {
        out += ",\"code\":\"" + httpJsonEscape(clipped(trimmed(draft.code), Limits::kCodeChars)) + "\"";
        out += ",\"slotsTotal\":" + std::to_string(draft.slotsTotal);
        out += ",\"slotsTaken\":" + std::to_string(draft.slotsTaken);
    }
    const std::string language = clipped(trimmed(draft.language), Limits::kLanguageChars);
    const std::string region = clipped(trimmed(draft.region), Limits::kRegionChars);
    const std::string note = clipped(trimmed(draft.note), Limits::kNoteChars);
    if (!language.empty()) out += ",\"language\":\"" + httpJsonEscape(language) + "\"";
    if (!region.empty()) out += ",\"region\":\"" + httpJsonEscape(region) + "\"";
    if (!note.empty()) out += ",\"note\":\"" + httpJsonEscape(note) + "\"";
    if (draft.minutes != 0) out += ",\"minutes\":" + std::to_string(draft.minutes);
    out += "}";
    return out;
}

std::string closeBody(const std::string& id) {
    return "{\"id\":\"" + httpJsonEscape(clipped(id, Limits::kIdChars)) + "\"}";
}

std::string reportBody(const std::string& id, const std::string& reason, const std::string& note) {
    return "{\"id\":\"" + httpJsonEscape(clipped(id, Limits::kIdChars)) +
           "\",\"reason\":\"" + httpJsonEscape(clipped(reason, 32)) +
           "\",\"note\":\"" + httpJsonEscape(clipped(trimmed(note), 400)) + "\"}";
}

std::vector<std::string> guidelines() {
    // The channel's rules, in the channel's order, because a player who reads
    // them here and then goes to Discord must not find a different list.
    return {
        "Be respectful.",
        "Post the parameters of the game you are hosting.",
        "Open Doctrines games only.",
        "Use the right tag: hosting a game, or looking for one.",
    };
}

const char* const kDiscordInvite = "https://discord.gg/wqS65jzVv5";

const char* kindName(Kind kind) { return kind == Kind::Looking ? "looking" : "hosting"; }
const char* modeName(Mode mode) { return mode == Mode::Longform ? "longform" : "rapid"; }

}  // namespace odlfg
