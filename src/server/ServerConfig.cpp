#include "ServerConfig.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

#include "json.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

const char* serverSettingScopeName(ServerSettingScope s) {
    switch (s) {
        case ServerSettingScope::Anytime:   return "anytime";
        case ServerSettingScope::LobbyOnly: return "lobby only";
        case ServerSettingScope::Restart:   return "needs restart";
    }
    return "anytime";
}

const char* serverTunnelModeName(ServerTunnelMode m) {
    switch (m) {
        case ServerTunnelMode::Off:          return "off";
        case ServerTunnelMode::Auto:         return "auto";
        case ServerTunnelMode::Cloudflared:  return "cloudflared";
        case ServerTunnelMode::LocalhostRun: return "localhost.run";
    }
    return "auto";
}

bool serverTunnelModeFromName(const std::string& name, ServerTunnelMode& out) {
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)tolower(c); });
    if (n == "off" || n == "none" || n == "false") { out = ServerTunnelMode::Off; return true; }
    if (n == "auto" || n == "true")                { out = ServerTunnelMode::Auto; return true; }
    if (n == "cloudflared" || n == "cloudflare")   { out = ServerTunnelMode::Cloudflared; return true; }
    if (n == "localhost.run" || n == "localhostrun" || n == "ssh") {
        out = ServerTunnelMode::LocalhostRun;
        return true;
    }
    return false;
}

namespace {

bool parseBool(const std::string& v, bool& out) {
    std::string s = v;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    if (s == "true" || s == "yes" || s == "on" || s == "1")   { out = true;  return true; }
    if (s == "false" || s == "no" || s == "off" || s == "0")  { out = false; return true; }
    return false;
}

bool parseUint(const std::string& v, uint32_t& out) {
    if (v.empty()) return false;
    char* end = nullptr;
    const unsigned long long n = strtoull(v.c_str(), &end, 10);
    if (!end || *end != '\0' || n > 0xFFFFFFFFull) return false;
    out = (uint32_t)n;
    return true;
}

/** One of a fixed set of words, case-insensitively. */
bool parseChoice(const std::string& v, std::initializer_list<const char*> allowed,
                 std::string& out) {
    std::string s = v;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    for (const char* a : allowed)
        if (s == a) { out = s; return true; }
    return false;
}

std::string joinList(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) {
        if (!out.empty()) out += ",";
        out += s;
    }
    return out;
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        // Trim, so "a, b" and "a,b" mean the same thing. A mod id with a stray
        // space would otherwise never match and the server would refuse every
        // client for a reason nobody could see.
        size_t b = part.find_first_not_of(" \t");
        size_t e = part.find_last_not_of(" \t");
        if (b != std::string::npos) out.push_back(part.substr(b, e - b + 1));
    }
    return out;
}

/**
 * The one table every console operation reads.
 *
 * `set`, `get` and `entries` all walk this, so a setting cannot exist in the
 * file and be missing from `config`, or be listed and not settable. Adding a
 * setting is one row.
 */
/** How a value is written back to the file, which is not guessable from it. */
enum class FieldKind : uint8_t { Text, Number, Boolean };

struct Field {
    const char* key;
    ServerSettingScope scope;
    const char* help;
    FieldKind kind;
    std::function<std::string(const ServerConfig&)> get;
    std::function<bool(ServerConfig&, const std::string&, std::string&)> set;
};

#define UINT_FIELD(NAME, MEMBER, SCOPE, HELP)                                          \
    {NAME, SCOPE, HELP, FieldKind::Number,                                                  \
     [](const ServerConfig& c) { return std::to_string(c.MEMBER); },                    \
     [](ServerConfig& c, const std::string& v, std::string& why) {                      \
         uint32_t n = 0;                                                                \
         if (!parseUint(v, n)) { why = "expects a whole number"; return false; }        \
         c.MEMBER = n;                                                                  \
         return true;                                                                   \
     }}

#define BOOL_FIELD(NAME, MEMBER, SCOPE, HELP)                                          \
    {NAME, SCOPE, HELP, FieldKind::Boolean,                                                  \
     [](const ServerConfig& c) { return std::string(c.MEMBER ? "true" : "false"); },    \
     [](ServerConfig& c, const std::string& v, std::string& why) {                      \
         bool b = false;                                                                \
         if (!parseBool(v, b)) { why = "expects true or false"; return false; }         \
         c.MEMBER = b;                                                                  \
         return true;                                                                   \
     }}

#define TEXT_FIELD(NAME, MEMBER, SCOPE, HELP)                                          \
    {NAME, SCOPE, HELP, FieldKind::Text,                                                  \
     [](const ServerConfig& c) { return c.MEMBER; },                                    \
     [](ServerConfig& c, const std::string& v, std::string&) {                          \
         c.MEMBER = v;                                                                  \
         return true;                                                                   \
     }}

#define CHOICE_FIELD(NAME, MEMBER, SCOPE, HELP, ...)                                   \
    {NAME, SCOPE, HELP, FieldKind::Text,                                                   \
     [](const ServerConfig& c) { return c.MEMBER; },                                    \
     [](ServerConfig& c, const std::string& v, std::string& why) {                       \
         std::string picked;                                                             \
         if (!parseChoice(v, {__VA_ARGS__}, picked)) {                                   \
             why = "expects one of: " + std::string(#__VA_ARGS__);                        \
             return false;                                                               \
         }                                                                               \
         c.MEMBER = picked;                                                              \
         return true;                                                                    \
     }}

const std::vector<Field>& fields() {
    static const std::vector<Field> table = {
        TEXT_FIELD("name", sessionName, ServerSettingScope::LobbyOnly,
                   "what the server calls itself in the browser"),
        TEXT_FIELD("motd", motd, ServerSettingScope::Anytime,
                   "message shown to each player once, on joining"),
        BOOL_FIELD("anonymous", anonymous, ServerSettingScope::Restart,
                   "host without publishing an account name"),
        BOOL_FIELD("listed", listed, ServerSettingScope::Restart,
                   "appear in the public server directory"),

        TEXT_FIELD("map", map, ServerSettingScope::LobbyOnly,
                   "shipped map id (1914, 1939, map) or a path to a .odmap"),
        TEXT_FIELD("load-save", loadSave, ServerSettingScope::LobbyOnly,
                   "resume this .odsv instead of starting a new world"),
        TEXT_FIELD("world-name", worldName, ServerSettingScope::LobbyOnly,
                   "name given to the world this server creates"),
        UINT_FIELD("max-players", maxPlayers, ServerSettingScope::LobbyOnly,
                   "seats in the lobby"),
        UINT_FIELD("turn-seconds", turnSeconds, ServerSettingScope::LobbyOnly,
                   "rule turn length; 0 is long-form with no countdown"),
        CHOICE_FIELD("assignment", assignment, ServerSettingScope::LobbyOnly,
                     "who picks countries", "host", "players"),
        CHOICE_FIELD("late-join", lateJoin, ServerSettingScope::LobbyOnly,
                     "what happens to someone arriving mid-game", "refuse", "spectate"),
        CHOICE_FIELD("absent", absent, ServerSettingScope::LobbyOnly,
                     "who plays a country whose orders did not arrive", "ai", "idle"),

        {"port", ServerSettingScope::Restart, "TCP port to listen on", FieldKind::Number,
         [](const ServerConfig& c) { return std::to_string(c.port); },
         [](ServerConfig& c, const std::string& v, std::string& why) {
             uint32_t n = 0;
             if (!parseUint(v, n) || n > 65535) { why = "expects a port, 0-65535"; return false; }
             c.port = (uint16_t)n;
             return true;
         }},
        BOOL_FIELD("bind-all", bindAll, ServerSettingScope::Restart,
                   "listen on every interface rather than loopback only"),
        {"tunnel", ServerSettingScope::Restart,
         "off, auto, cloudflared or localhost.run", FieldKind::Text,
         [](const ServerConfig& c) { return std::string(serverTunnelModeName(c.tunnel)); },
         [](ServerConfig& c, const std::string& v, std::string& why) {
             ServerTunnelMode m{};
             if (!serverTunnelModeFromName(v, m)) {
                 why = "expects off, auto, cloudflared or localhost.run";
                 return false;
             }
             c.tunnel = m;
             return true;
         }},

        {"mods", ServerSettingScope::LobbyOnly,
         "comma-separated mod ids every client must have", FieldKind::Text,
         [](const ServerConfig& c) { return joinList(c.requiredMods); },
         [](ServerConfig& c, const std::string& v, std::string&) {
             c.requiredMods = splitList(v);
             return true;
         }},

        UINT_FIELD("auto.start-at-players", automation.startAtPlayers,
                   ServerSettingScope::Anytime, "start once this many hold a country; 0 off"),
        UINT_FIELD("auto.start-after-seconds", automation.startAfterSeconds,
                   ServerSettingScope::Anytime, "start this long after the first arrival; 0 off"),
        UINT_FIELD("auto.start-min-players", automation.startMinPlayers,
                   ServerSettingScope::Anytime, "never auto-start below this many"),
        UINT_FIELD("auto.advance-every-seconds", automation.advanceEverySeconds,
                   ServerSettingScope::Anytime, "advance the turn on this schedule; 0 off"),
        UINT_FIELD("auto.end-at-turn", automation.endAtTurn,
                   ServerSettingScope::Anytime, "end the game at this turn; 0 never"),
        UINT_FIELD("auto.end-after-hours", automation.endAfterHours,
                   ServerSettingScope::Anytime, "end the game after this long; 0 never"),
        BOOL_FIELD("auto.end-when-empty", automation.endWhenEmpty,
                   ServerSettingScope::Anytime, "end when the last player leaves"),
        BOOL_FIELD("auto.return-to-lobby", automation.returnToLobbyWhenDone,
                   ServerSettingScope::Anytime, "open a new lobby when a game ends"),
        UINT_FIELD("auto.autosave-every-turns", automation.autosaveEveryTurns,
                   ServerSettingScope::Anytime, "save the world every N turns; 0 only at the end"),

        TEXT_FIELD("data-dir", dataDir, ServerSettingScope::Restart,
                   "where maps, mods and saves live; empty finds it beside the binary"),
        CHOICE_FIELD("log-level", logLevel, ServerSettingScope::Anytime,
                     "how much the console prints", "debug", "info", "warn", "error"),
    };
    return table;
}

}  // namespace

bool ServerConfig::load(const std::string& path, std::string& why) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return true;   // all defaults; see the header note

    std::ifstream f(path);
    if (!f) { why = "could not open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();

    json j;
    try {
        // Comments allowed: the default file is commented, and a config people
        // are told to edit that rejects their notes is a config people stop
        // annotating.
        j = json::parse(ss.str(), nullptr, true, true);
    } catch (const std::exception& e) {
        why = std::string("could not read ") + path + ": " + e.what();
        return false;
    }
    if (!j.is_object()) { why = path + " is not a JSON object"; return false; }

    for (const Field& fld : fields()) {
        if (!j.contains(fld.key)) continue;
        const json& v = j.at(fld.key);
        std::string text;
        if (v.is_string())            text = v.get<std::string>();
        else if (v.is_boolean())      text = v.get<bool>() ? "true" : "false";
        else if (v.is_number())       text = std::to_string(v.get<long long>());
        else if (v.is_array()) {
            for (const auto& e : v) {
                if (!e.is_string()) continue;
                if (!text.empty()) text += ",";
                text += e.get<std::string>();
            }
        } else {
            why = std::string("setting '") + fld.key + "' has a value of the wrong kind";
            return false;
        }
        std::string sub;
        // Loading is not the lobby check: the file describes a server that has
        // not started yet, so every scope is settable here.
        if (!fld.set(*this, text, sub)) {
            why = std::string("setting '") + fld.key + "': " + (sub.empty() ? "bad value" : sub);
            return false;
        }
    }
    return true;
}

bool ServerConfig::save(const std::string& path, std::string& why) const {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) { why = "could not write " + path; return false; }

    f << "// OpenDoctrines dedicated server.\n"
      << "// Every setting, its meaning and when it may be changed, is listed by\n"
      << "// the `config` command in the server console. Comments are allowed here.\n"
      << "{\n";
    const std::vector<Field>& table = fields();
    for (size_t i = 0; i < table.size(); ++i) {
        const Field& fld = table[i];
        const std::string v = fld.get(*this);
        f << "    // " << fld.help << " (" << serverSettingScopeName(fld.scope) << ")\n";
        f << "    \"" << fld.key << "\": ";
        // Written by the field's DECLARED kind, never by what the value looks
        // like. Guessing from the text quoted `map: "1914"` as the number 1914,
        // which round-trips by luck -- and would have turned a map called 007
        // into 7 the first time anybody made one.
        if (fld.kind == FieldKind::Text) f << json(v).dump();
        else                             f << v;
        f << (i + 1 < table.size() ? ",\n" : "\n");
    }
    f << "}\n";
    return true;
}

std::string ServerConfig::defaultFileText() {
    ServerConfig defaults;
    std::string why;
    // Rendered through save() into a temporary so there is exactly one
    // definition of what the file looks like.
    const fs::path tmp = fs::temp_directory_path() / "od_server_default_config.json";
    if (!defaults.save(tmp.string(), why)) return {};
    std::ifstream f(tmp);
    std::stringstream ss;
    ss << f.rdbuf();
    f.close();
    std::error_code ec;
    fs::remove(tmp, ec);
    return ss.str();
}

bool ServerConfig::set(const std::string& key, const std::string& value, bool inLobby,
                       std::string& why) {
    for (const Field& fld : fields()) {
        if (key != fld.key) continue;
        if (fld.scope == ServerSettingScope::Restart) {
            why = "'" + key + "' only takes effect when the server restarts. "
                  "Edit the config file and restart.";
            return false;
        }
        if (fld.scope == ServerSettingScope::LobbyOnly && !inLobby) {
            why = "'" + key + "' describes the game being played, so it can only "
                  "change in the lobby. Finish or end the game first.";
            return false;
        }
        std::string sub;
        if (!fld.set(*this, value, sub)) {
            why = "'" + key + "' " + (sub.empty() ? "did not accept that value" : sub);
            return false;
        }
        return true;
    }
    why = "no setting called '" + key + "'. `config` lists them all.";
    return false;
}

std::string ServerConfig::get(const std::string& key, bool& ok) const {
    for (const Field& fld : fields())
        if (key == fld.key) { ok = true; return fld.get(*this); }
    ok = false;
    return {};
}

std::vector<ServerConfig::Entry> ServerConfig::entries() const {
    std::vector<Entry> out;
    out.reserve(fields().size());
    for (const Field& fld : fields())
        out.push_back({fld.key, fld.get(*this), fld.scope, fld.help});
    return out;
}

std::vector<std::string> ServerConfig::problems() const {
    std::vector<std::string> out;

    if (maxPlayers < 1 || maxPlayers > 64)
        out.push_back("max-players is " + std::to_string(maxPlayers) +
                      "; the lobby holds between 1 and 64.");
    if (map.empty() && loadSave.empty())
        out.push_back("no map and no save to load: nothing to host.");

    // An auto-start floor above the seats can never be reached, so the server
    // would wait for players it has nowhere to put. Caught here because the
    // symptom -- a lobby that never starts -- looks like a network fault.
    if (automation.startAtPlayers > maxPlayers)
        out.push_back("auto.start-at-players (" + std::to_string(automation.startAtPlayers) +
                      ") is above max-players (" + std::to_string(maxPlayers) +
                      "), so the game would never start.");
    if (automation.startMinPlayers > maxPlayers)
        out.push_back("auto.start-min-players (" + std::to_string(automation.startMinPlayers) +
                      ") is above max-players (" + std::to_string(maxPlayers) + ").");
    if (automation.startAfterSeconds > 0 && automation.startMinPlayers == 0)
        out.push_back("auto.start-after-seconds is set with auto.start-min-players at 0, "
                      "so the server would start a game with nobody in it.");

    // Long-form with no schedule and no operator is a server that never takes
    // a turn. Legal -- somebody may drive it by hand -- but worth saying.
    if (turnSeconds == 0 && automation.advanceEverySeconds == 0)
        out.push_back("note: turn-seconds is 0 and auto.advance-every-seconds is 0, "
                      "so turns only advance when someone types `step-go`.");

    if (!loadSave.empty()) {
        std::error_code ec;
        if (!fs::exists(loadSave, ec))
            out.push_back("load-save points at " + loadSave + ", which does not exist.");
    }
    return out;
}
