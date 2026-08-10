#pragma once

// What a dedicated server is set to do, and when it may be changed.
//
// Pure data and pure rules -- no sockets, no game headers, no raylib -- so the
// whole of it can be tested by loading text and asking questions, which is what
// tests/server_config_test.cpp does.
//
// WHY A FILE AND NOT FLAGS
//
// A server is configured once and then runs for weeks. Command-line flags are
// the wrong shape for that: they live in whatever shell history or unit file
// started the process, so "what is this server set to" has no answer you can
// read, and changing one thing means retyping all of them. The file is the
// answer, it is commented, and the console can edit it in place.
//
// Flags still exist, and they WIN over the file, because that is what makes a
// one-off run possible without editing anything. See ServerMain.cpp.
//
// WHEN A SETTING MAY CHANGE
//
// Not everything can safely change at any moment, and the difference is not a
// matter of taste:
//
//   Anytime    -- motd, log level, automation deadlines. Nothing in flight
//                 depends on the old value.
//   LobbyOnly  -- map, max players, turn length, mods, assignment rules. These
//                 describe the game about to be played; changing them mid-game
//                 would mean the server and every client disagreeing about the
//                 rules they are already playing under.
//   Restart    -- port, bind address, tunnel. The socket is already open and
//                 players are already connected through it.
//
// `set` refuses with that reason rather than silently accepting a value that
// will not take effect, which is the failure people mistake for a bug.

#include <cstdint>
#include <string>
#include <vector>

/** When a setting can be changed without breaking something already running. */
enum class ServerSettingScope : uint8_t {
    Anytime = 0,
    LobbyOnly,
    Restart,
};

const char* serverSettingScopeName(ServerSettingScope s);

/** How the server makes itself reachable. */
enum class ServerTunnelMode : uint8_t {
    /** No tunnel. The port must be reachable some other way -- forwarded, LAN. */
    Off = 0,
    /** Pick whatever this machine has, best first. */
    Auto,
    Cloudflared,
    LocalhostRun,
};

const char* serverTunnelModeName(ServerTunnelMode m);
bool serverTunnelModeFromName(const std::string& name, ServerTunnelMode& out);

/**
 * Unattended operation: when to start, when to advance, when to stop.
 *
 * A dedicated server has nobody at the keyboard, so every decision a host would
 * otherwise make by clicking has to have an answer here. The defaults are the
 * cautious ones -- wait for people, never end a game on its own -- because a
 * server that ended games by itself when nobody asked it to is a server that
 * loses campaigns.
 */
struct ServerAutomation {
    /** Start once this many players hold a country. 0 disables the rule. */
    uint32_t startAtPlayers = 0;

    /**
     * Start this many seconds after the FIRST player arrives, regardless.
     *
     * Counted from the first arrival rather than from boot: a server that has
     * been up for a week should not start a game the moment one person joins
     * because its timer expired six days ago.
     */
    uint32_t startAfterSeconds = 0;

    /** Refuse to start below this many players even when a deadline passes. */
    uint32_t startMinPlayers = 2;

    /**
     * Advance the turn automatically every N seconds of real time. 0 = never,
     * which leaves `step-go` from the console as the only way a turn moves.
     *
     * Distinct from the RULE turn length (turnSeconds), which is the countdown
     * players see. This is the server's own schedule for a long-form game where
     * there is no countdown at all -- a turn a day, say.
     */
    uint32_t advanceEverySeconds = 0;

    /** End the game after this many turns. 0 = never. */
    uint32_t endAtTurn = 0;

    /** End the game after this many hours of play. 0 = never. */
    uint32_t endAfterHours = 0;

    /** End when the last player disconnects, rather than holding the world. */
    bool endWhenEmpty = false;

    /**
     * What happens when a game ends: back to the lobby for another, or stop.
     *
     * Returning to the lobby is the default because that is what a public
     * server is for. `false` suits a server brought up for one campaign.
     */
    bool returnToLobbyWhenDone = true;

    /** Save the world every N turns. 0 = only when the game ends. */
    uint32_t autosaveEveryTurns = 1;
};

/** Everything a dedicated server is set to. */
struct ServerConfig {
    // ── identity ──
    std::string sessionName = "OpenDoctrines dedicated server";
    /** Shown to anyone who joins, once. Empty for none. */
    std::string motd;
    /** Host without publishing an account name. Still declared -- see Host.h. */
    bool anonymous = false;
    /** List in the public directory so people can find it. */
    bool listed = false;

    // ── the game ──
    /** Map id or path: a shipped name like "1914", or a path to any .odmap. */
    std::string map = "1914";
    /** Resume this save instead of starting a new world. Empty = new. */
    std::string loadSave;
    std::string worldName = "Dedicated";
    uint32_t maxPlayers = 8;
    /** Rule turn length in seconds. 0 = long-form, no countdown. */
    uint32_t turnSeconds = 0;
    /** "host" or "players": who picks countries. */
    std::string assignment = "players";
    /** "refuse" or "spectate": what happens to someone arriving mid-game. */
    std::string lateJoin = "spectate";
    /** "ai" or "idle": who plays a country whose orders did not arrive. */
    std::string absent = "ai";

    // ── network ──
    uint16_t port = 27015;
    /** Listen on every interface. False binds loopback, for a tunnelled setup. */
    bool bindAll = false;
    ServerTunnelMode tunnel = ServerTunnelMode::Auto;

    // ── mods ──
    /** `both`-side mods a client must match to join. See ModAttest.h. */
    std::vector<std::string> requiredMods;

    // ── operation ──
    ServerAutomation automation;
    /** Directory the server reads content from and writes saves into. */
    std::string dataDir;
    /** debug | info | warn | error */
    std::string logLevel = "info";

    /**
     * Load everything and exit without opening a session (`--check`).
     *
     * Deliberately NOT a file setting: it describes one run, not a server, and
     * a config that could permanently disable hosting is a foot-gun. It exists
     * because everything before the session -- the config, the data directory,
     * the map, the whole world load -- can be verified with no account and no
     * network, which makes it the part CI can actually test, and the answer to
     * an operator asking "is my config right" without taking a server down.
     */
    bool checkOnly = false;

    // ── file ──

    /**
     * Read a config file. False with `why` set when it cannot be parsed.
     *
     * A MISSING file is not an error: it means "all defaults", which is what
     * lets a first run work with no setup at all. Only a file that exists and
     * is malformed fails, because silently falling back to defaults there would
     * hide a typo in a setting somebody thought they had changed.
     */
    bool load(const std::string& path, std::string& why);

    /** Write the file, comments and all. False with `why` on failure. */
    bool save(const std::string& path, std::string& why) const;

    /** The commented default file, as text. */
    static std::string defaultFileText();

    // ── console editing ──

    /**
     * Set one setting by dotted name, as the console's `config` command does.
     *
     * `inLobby` decides whether a LobbyOnly setting is accepted. False with
     * `why` when the name is unknown, the value will not parse, or the scope
     * forbids it now -- each of which gets a different sentence, because "no"
     * with no reason is what makes a config file feel broken.
     */
    bool set(const std::string& key, const std::string& value, bool inLobby,
             std::string& why);

    /** One setting's current value as text. Empty `ok` when the name is unknown. */
    std::string get(const std::string& key, bool& ok) const;

    /** Every settable name, with its scope and current value, for `config`. */
    struct Entry {
        std::string key;
        std::string value;
        ServerSettingScope scope;
        std::string help;
    };
    std::vector<Entry> entries() const;

    /**
     * Anything wrong with this configuration, in words. Empty when it is sound.
     *
     * Checked at startup rather than discovered at the moment it matters: a
     * server that boots, waits an hour for players and only then finds its map
     * does not exist has wasted the one thing it was there to provide.
     */
    std::vector<std::string> problems() const;
};
