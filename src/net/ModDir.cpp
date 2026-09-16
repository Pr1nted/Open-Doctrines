#include "ModDir.h"

#include "HttpClient.h"

namespace odmoddir {
namespace {

std::string clipped(const std::string& value, size_t max) {
    return value.size() <= max ? value : value.substr(0, max);
}

/** One field, read and bounded in the same breath so neither can be forgotten. */
std::string field(const std::string& obj, const char* key, size_t max) {
    return clipped(httpJsonString(obj, key, (int)max + 1), max);
}

Side readSide(const std::string& obj) {
    // The wire value is the manifest's own word. "both" is shown as
    // "synchronised" because that is what it MEANS to a player -- everyone in
    // the game needs the same file -- and the manifest's word does not say so.
    const std::string s = field(obj, "side", 16);
    if (s == "server") return Side::Server;
    if (s == "both")   return Side::Synchronised;
    return Side::Client;                       // unrecognised reads as the mildest
}

Scan readScan(const std::string& obj, int& flagged, int& engines) {
    flagged = engines = 0;
    const size_t at = httpJsonScope(obj, "scan");
    if (at == std::string::npos) return Scan::Unscanned;
    const std::string scope = obj.substr(at);

    const std::string state = field(scope, "state", 16);
    flagged = (int)httpJsonNumber(scope, "malicious", 0)
            + (int)httpJsonNumber(scope, "suspicious", 0);
    engines = (int)httpJsonNumber(scope, "engines", 0);

    if (state == "flagged") return Scan::Flagged;
    if (state == "clean")   return Scan::Clean;
    if (state == "unknown") return Scan::Unknown;
    // Anything else, including a state this build has never heard of, reads as
    // "nobody asked". Never as clean: a word we do not recognise is not a
    // reassurance, and rendering it as one is the failure this enum exists for.
    return Scan::Unscanned;
}

bool readListing(const std::string& obj, Listing& out) {
    out = Listing{};
    out.id      = field(obj, "id", Limits::kIdChars);
    out.name    = field(obj, "name", Limits::kNameChars);
    out.version = field(obj, "version", Limits::kVersionChars);
    out.by      = field(obj, "by", Limits::kByChars);
    out.summary = field(obj, "summary", Limits::kSummaryChars);
    out.page    = clipped(httpJsonString(obj, "page", (int)Limits::kUrlChars + 1),
                          Limits::kUrlChars);

    // A listing with no id or no name is not a listing. Dropped whole rather
    // than drawn half: a row with a blank title is somebody's entry rendered
    // wrong, which is worse than a shorter list.
    if (out.id.empty() || out.name.empty()) return false;

    // A page that is not https is simply not offered. The button is what would
    // act on it, so the check belongs where the value enters, not there.
    if (!openable(out.page)) out.page.clear();

    out.side = readSide(obj);
    out.scan = readScan(obj, out.flagged, out.engines);

    for (std::string& m : httpJsonStringArray(obj, "modules", (int)Limits::kModules)) {
        if (m.empty()) continue;
        m = clipped(m, Limits::kModuleChars);
        if (m == "GameProcess" || m == "GameState.Write") out.changesTurns = true;
        out.modules.push_back(m);
    }
    return true;
}

}  // namespace

bool openable(const std::string& url) {
    if (url.size() < 9 || url.size() > Limits::kUrlChars) return false;
    if (url.compare(0, 8, "https://") != 0) return false;
    // Nothing that could end a shell word or start another, matching the rule
    // the game already applies to a mod's updateUrl in ModUpdates.cpp. odlink
    // does not use a shell, so this is the second of two checks rather than the
    // only one -- but a URL carrying these is malformed whatever opens it.
    for (unsigned char c : url) {
        if (c < 0x21 || c > 0x7E) return false;
        if (std::string("\"'`\\<>|;&$(){}[]^").find((char)c) != std::string::npos) return false;
    }
    return true;
}

const char* Listing::sideWord() const {
    switch (side) {
        case Side::Server:       return "Host only";
        case Side::Synchronised: return "Everyone needs it";
        default:                 return "Client";
    }
}

std::vector<Listing> parse(const std::string& json, std::string& error) {
    error.clear();
    std::vector<Listing> out;
    if (json.empty())                    { error = "no reply"; return out; }
    if (json.size() > Limits::kDocument) { error = "directory document too large"; return out; }

    const size_t listAt = json.find("\"mods\"");
    if (listAt == std::string::npos) { error = "no mods in the reply"; return out; }
    size_t at = json.find('[', listAt);
    if (at == std::string::npos) { error = "mods is not a list"; return out; }

    // Object by object, exactly as odlfg::parseBoard walks its board, and for
    // the same reason: one entry that does not read is skipped whole.
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
                if (readListing(json.substr(objectStart, i - objectStart + 1), item))
                    out.push_back(item);
                objectStart = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return out;
}

}  // namespace odmoddir
