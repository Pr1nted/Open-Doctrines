// The dedicated server: the game's own hosting path, with nobody at the keyboard.
//
// WHY THIS IS A Game METHOD AND NOT A PROGRAM OF ITS OWN
//
// Hosting is already written. mpOpenHost() opens the session, mpDrainEvents()
// admits people, mpHostTurnUpdate() runs the clock and mpResolveTurn() resolves
// a turn and broadcasts the delta -- and the host is the AUTHORITY for all of
// it. A dedicated server that reimplemented any of that would be a second set
// of rules; the two would drift, and the symptom would be a game that played
// differently depending on who hosted it. So this file drives the existing path
// and adds only what a server needs that a player does not: a console, a config
// file, and answers to the decisions a host would otherwise make by clicking.
//
// WHAT REPLACES THE HOST'S CLICKS
//
//   Starting the game    -- ServerAutomation: player count, or a deadline.
//   Advancing a turn     -- the rule clock, an interval, or `step-go`.
//   Ending the game      -- turn count, wall clock, or an empty server.
//   Admitting people     -- Lobby's own rules, unchanged.
//
// Every one of those has an explicit default in ServerConfig.h, and the
// cautious value is the default: a server does not start games nobody joined
// and does not end games nobody asked it to end.
//
// THE SERVER HOLDS NO COUNTRY
//
// m_playerCountryId stays 0 and cfg.dedicated is set, so every seat belongs to
// a player or to the AI. A server that quietly held France would be a server
// with an opinion about who should win.

#include "Game.h"

#include "Audio.h"
#include "net/Host.h"
#include "net/Lobby.h"
#include "net/ModAttest.h"
#include "net/Tunnel.h"
#include "mods/ModManager.h"
#include "server/ServerConfig.h"
#include "server/ServerConsole.h"
#include "server/ServerRuntime.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include "net/AccountClient.h"

namespace fs = std::filesystem;

namespace {

/** Monotonic seconds, for every deadline this file keeps. */
double nowSeconds() {
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

std::string secondsAsWords(uint32_t s) {
    if (s == 0) return "off";
    if (s % 3600 == 0) return std::to_string(s / 3600) + "h";
    if (s % 60 == 0) return std::to_string(s / 60) + "m";
    return std::to_string(s) + "s";
}

}  // namespace

// ════════════════════════════════════════════════════════════════
//  Shared by every headless mode of this binary
// ════════════════════════════════════════════════════════════════

/**
 * Find the data directory the way Game::init() does, without opening a window.
 *
 * init() is where this normally happens on the client, and it is also where
 * InitWindow() is called -- so no headless mode of this binary can reach it.
 * The probe is the same one: an explicit override wins, then OD_DATA_DIR, then
 * data/ and ../data/ beside the executable.
 *
 * It looks for data/FONTS rather than data/, copied from init() and for the
 * reason recorded there: a build tree can hold a build/data/ left behind by the
 * map generator, a directory with the right name and none of the right
 * contents. Choosing that one starts with no maps, and it DOES start.
 */
bool Game::srvResolveDataDir(const std::string& override) {
    if (!override.empty()) {
        m_dataDir = override;
    } else if (const char* over = getenv("OD_DATA_DIR")) {
        m_dataDir = over;
    } else {
        std::string appDir = GetApplicationDirectory();
        if (!appDir.empty() && appDir.back() != '/' && appDir.back() != '\\') appDir += '/';
        m_dataDir = appDir + "../data/";
        // Probing for data/STDmaps and not data/fonts.
        //
        // init() looks for fonts, because a build tree can hold a build/data/
        // left behind by the map generator -- the right name, none of the right
        // contents -- and choosing it starts the game with no fonts and no
        // maps. That marker is wrong here: a SERVER package deliberately ships
        // no fonts at all, since it never draws, so the probe fell through to
        // ../data/ and the released server could not find its own maps.
        // STDmaps is the right discriminator: it is what a server actually
        // needs, and the generator's leftover directory does not contain it.
        for (const char* rel : {"data/", "../data/"}) {
            std::error_code ec;
            if (fs::is_directory(appDir + rel + "STDmaps", ec)) { m_dataDir = appDir + rel; break; }
        }
    }
    if (!m_dataDir.empty() && m_dataDir.back() != '/' && m_dataDir.back() != '\\')
        m_dataDir += '/';
    std::error_code ec;
    return fs::is_directory(m_dataDir, ec);
}

/**
 * Self-play training and measurement, on a machine with no graphics card.
 *
 * WHY THIS IS IN THE SERVER BINARY
 *
 * Training is the longest-running, least interactive thing this project does,
 * and until now it could only run on a machine that could open an OpenGL 3.3
 * window -- because every mode of the game binary goes through Game::init().
 * That is precisely backwards: the boxes you want running a week of self-play
 * are headless ones, and they were the boxes that could not.
 *
 * Nothing here is a second copy of the trainer. It is the same
 * runAITraining/runAIEvaluation the game calls, in a binary that never links a
 * renderer. See src/server/ServerRaylib.cpp for why that is possible at all.
 */
int Game::runHeadlessAI(const HeadlessAIOptions& o) {
    m_headless = true;
    Audio::s_disabled = true;

    if (!srvResolveDataDir(o.dataDir)) {
        fprintf(stderr, "no data directory at %s -- pass --data <dir>\n", m_dataDir.c_str());
        return 2;
    }
    printf("[AI] data: %s\n", m_dataDir.c_str());

    if (o.workerCount > 1) setAIWorker(o.workerId, o.workerCount);

    if (o.train) {
        runAITraining(o.maps, o.turns, o.countries, o.seed);
        return 0;
    }
    return runAIEvaluation(o.maps, o.turns, o.seed, o.difficulty, o.vsRandom,
                           o.vsModel, o.scenarios) ? 0 : 1;
}

// ════════════════════════════════════════════════════════════════
//  The server
// ════════════════════════════════════════════════════════════════

// ── queries both halves of the server ask ──
//
// Member functions rather than lambdas because serverBegin registers the
// commands and serverTick runs the automation, and both need the same answers.
// They touch nothing but the lobby, which is what makes that safe.

bool Game::srvInLobby() const {
    return m_netHost && m_netHost->lobby().state() == NetSessionState::Lobby;
}

bool Game::srvInGame() const {
    return m_netHost && m_netHost->lobby().state() == NetSessionState::Game;
}

/** Players who hold a country. The host's own seat is not one of them. */
uint32_t Game::srvPlayersHoldingCountries() const {
    uint32_t n = 0;
    if (!m_netHost) return n;
    for (const NetPeer& p : m_netHost->lobby().roster())
        if (!p.spectator && p.countryId != 0 && p.peerId != m_netHost->lobby().hostPeerId())
            ++n;
    return n;
}

/** Anyone still connected, seated or spectating. */
uint32_t Game::srvConnectedPlayers() const {
    uint32_t n = 0;
    if (!m_netHost) return n;
    for (const NetPeer& p : m_netHost->lobby().roster())
        if (p.connected && p.peerId != m_netHost->lobby().hostPeerId()) ++n;
    return n;
}

int Game::serverBegin(ServerConfig& config, ServerConsole& console,
                      const std::string& configPath) {
    // No GPU work anywhere in this process. The flag already guards the flag
    // rasteriser and the political texture; setting it here is what makes the
    // shared simulation safe to run against ServerRaylib's no-ops.
    m_headless = true;
    Audio::s_disabled = true;

    console.info("OpenDoctrines dedicated server " OD_VERSION_STRING);

    // ── where the content is ──
    if (!srvResolveDataDir(config.dataDir)) {
        console.error("no data directory at " + m_dataDir +
                      ". Pass --data <dir>, or set `data-dir` in the config.");
        return 2;
    }
    console.info("data: " + m_dataDir);

    // ── THE GAME'S OWN CONFIG, AND THE ACCOUNT IT HOSTS AS ──
    //
    // The client does both of these in Game::init(), which a dedicated server
    // never calls -- init() opens an OpenGL window, which is the whole reason
    // this entry point exists. So the server ran with an EMPTY m_config and an
    // uninitialised AccountClient, which meant issuer, token and
    // serverCredential were all blank when it finally tried to open a session.
    //
    // It failed at the last possible moment, having already loaded the map,
    // generated a world and written a save, with "Sign in and register this
    // server before hosting." -- advice that cannot be followed, because the
    // machine WAS signed in and registered and this process had simply never
    // looked. A dedicated server has never opened a session.
    m_configPath = m_dataDir + "config.json";
    m_config.load(m_configPath);
    AccountClient::get().init(m_config.accountIssuer, m_dataDir + "account.json");

    // init() only says WHERE the session is kept. bootstrap() is what reads it
    // -- the stored token is loaded synchronously, so the credentials needed to
    // open a session exist the moment this returns.
    //
    // The rest of the account (nickname, badges) is refreshed on a worker, and
    // it is worth a short wait: the host's own row in its own lobby is drawn
    // from it, and opening before it lands is exactly how that row came to read
    // "someone". Bounded, and not fatal -- a server that cannot reach the
    // account service to refresh a name can still host under one.
    if (AccountClient::get().bootstrap()) {
        for (int i = 0; i < 100 && !AccountClient::get().account().valid(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!AccountClient::get().account().valid()) {
        console.warn("the stored account did not refresh; hosting under whatever "
                     "name the service has on file.");
    }

    // ── configuration ──
    for (const std::string& p : config.problems()) console.warn(p);

    ServerLogLevel level = ServerLogLevel::Info;
    if (serverLogLevelFromName(config.logLevel, level)) console.setLevel(level);

    // ── state this run keeps ──
    //
    // On a runtime object rather than in locals because this loop is no
    // longer the only caller. serverTick() runs one iteration of it, and a
    // UI front end drives that between draws -- which only works if the
    // state outlives a single call. See src/server/ServerRuntime.h.
    m_srv = std::make_unique<ServerRuntime>();
    m_srv->config = &config;
    m_srv->console = &console;
    m_srv->configPath = configPath;

    /** A roster row by name or by peer id, so `kick 3` and `kick Ada` both work. */
    auto findPeer = [&](const std::string& who, NetPeer& out) {
        if (!m_netHost) return false;
        for (const NetPeer& p : m_netHost->lobby().roster()) {
            if (p.name == who || std::to_string(p.peerId) == who) { out = p; return true; }
        }
        // Case-insensitive second pass: an operator reading a name off a log
        // should not have to match its capitals.
        std::string lower = who;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return (char)tolower(c); });
        for (const NetPeer& p : m_netHost->lobby().roster()) {
            std::string pn = p.name;
            std::transform(pn.begin(), pn.end(), pn.begin(),
                           [](unsigned char c) { return (char)tolower(c); });
            if (pn == lower) { out = p; return true; }
        }
        return false;
    };

    // ── commands ──
    //
    // Registered here because this is where the things they act on live. The
    // table drives `help`, so a command cannot be added without documenting it.

    console.add({"help", "help [command]", "list the commands, or explain one", 0, 1,
                 [&](const std::vector<std::string>& a, ServerConsole& c) {
                     c.printHelp(a.empty() ? "" : a[0]);
                 }});

    console.add({"say", "say <message>", "send a message to everyone in the session", 1, -1,
                 [&](const std::vector<std::string>& a, ServerConsole& c) {
                     std::string text;
                     for (const std::string& w : a) { if (!text.empty()) text += " "; text += w; }
                     if (!m_netHost || m_netHost->phase() != NetHost::Phase::Live) {
                         c.warn("nobody to say that to: the session is not open.");
                         return;
                     }
                     m_netHost->sendChat(text);
                     c.raw("[Server] " + text);
                 }});

    console.add({"list", "list", "who is connected, and what they hold", 0, 0,
                 [&](const std::vector<std::string>&, ServerConsole& c) {
                     if (!m_netHost) { c.warn("the session is not open."); return; }
                     const std::vector<NetPeer> roster = m_netHost->lobby().roster();
                     c.raw("Players (" + std::to_string(roster.size()) + "/" +
                           std::to_string(config.maxPlayers) + "):");
                     for (const NetPeer& p : roster) {
                         if (p.peerId == m_netHost->lobby().hostPeerId()) continue;
                         const Country* country =
                             p.countryId ? m_countries.getCountry(p.countryId) : nullptr;
                         c.raw("  #" + std::to_string(p.peerId) + "  " +
                               (p.name.empty() ? "(anonymous)" : p.name) +
                               (p.spectator ? "  [spectating]"
                                            : "  " + std::string(country ? country->name
                                                                         : "no country")) +
                               (p.connected ? "" : "  [disconnected]") +
                               (p.submitted ? "  [orders in]" : ""));
                     }
                     if (roster.size() <= 1) c.raw("  nobody yet.");
                 }});

    console.add({"kick", "kick <player> [reason]", "disconnect somebody; they may come back",
                 1, -1,
                 [&](const std::vector<std::string>& a, ServerConsole& c) {
                     NetPeer p;
                     if (!findPeer(a[0], p)) { c.warn("no player called '" + a[0] + "'."); return; }
                     std::string reason;
                     for (size_t i = 1; i < a.size(); ++i) {
                         if (!reason.empty()) reason += " ";
                         reason += a[i];
                     }
                     if (reason.empty()) reason = "Kicked by the server operator";
                     m_netHost->kick(p.peerId, reason);
                     c.info("kicked " + p.name + " (" + reason + ")");
                 }});

    // Ban and kick are SEPARATE, as Lobby.h explains: collapsing them would
    // make every kick permanent. A ban also disconnects, because a ban that
    // left somebody sitting in the lobby would not be one.
    console.add({"ban", "ban <player> [reason]", "disconnect somebody and refuse them again",
                 1, -1,
                 [&](const std::vector<std::string>& a, ServerConsole& c) {
                     NetPeer p;
                     if (!findPeer(a[0], p)) { c.warn("no player called '" + a[0] + "'."); return; }
                     std::string reason;
                     for (size_t i = 1; i < a.size(); ++i) {
                         if (!reason.empty()) reason += " ";
                         reason += a[i];
                     }
                     if (reason.empty()) reason = "Banned by the server operator";
                     m_netHost->lobby().ban(p.psid);
                     m_netHost->kick(p.peerId, reason);
                     m_netHost->broadcastLobby();
                     c.info("banned " + p.name + " (" + reason + ")");
                 }});

    console.add({"unban", "unban <psid>", "let a banned player back in", 1, 1,
                 [&](const std::vector<std::string>& a, ServerConsole& c) {
                     if (!m_netHost) { c.warn("the session is not open."); return; }
                     m_netHost->lobby().unban(a[0]);
                     c.info("unbanned " + a[0]);
                 }});

    console.add({"banlist", "banlist", "who is banned", 0, 0,
                 [&](const std::vector<std::string>&, ServerConsole& c) {
                     if (!m_netHost) { c.warn("the session is not open."); return; }
                     const std::vector<std::string>& bans = m_netHost->lobby().bans();
                     if (bans.empty()) { c.raw("nobody is banned."); return; }
                     c.raw("Banned (" + std::to_string(bans.size()) + "):");
                     for (const std::string& b : bans) c.raw("  " + b);
                 }});

    console.add({"step-go", "step-go", "resolve the current turn now, without waiting", 0, 0,
                 [&](const std::vector<std::string>&, ServerConsole& c) {
                     if (!srvInGame()) { c.warn("no game in progress."); return; }
                     m_srv->stepRequested = true;
                     c.info("advancing turn " + std::to_string(m_turnNumber + 1) + " now");
                 }});

    console.add({"start", "start", "start the game with whoever is here", 0, 0,
                 [&](const std::vector<std::string>&, ServerConsole& c) {
                     if (!srvInLobby()) { c.warn("the game has already started."); return; }
                     m_srv->startRequested = true;
                 }});

    console.add({"status", "status", "what this server is doing right now", 0, 0,
                 [&](const std::vector<std::string>&, ServerConsole& c) {
                     c.raw("Session : " + config.sessionName);
                     c.raw("Map     : " + (m_mpMapId.empty() ? config.map : m_mpMapId));
                     c.raw("State   : " + std::string(srvInGame() ? "playing" :
                                                      srvInLobby() ? "lobby" : "opening"));
                     if (srvInGame()) c.raw("Turn    : " + std::to_string(m_turnNumber));
                     c.raw("Players : " + std::to_string(srvConnectedPlayers()) + " connected, " +
                           std::to_string(srvPlayersHoldingCountries()) + " holding a country");
                     if (m_netHost && m_netHost->phase() == NetHost::Phase::Live) {
                         c.raw("Code    : " + m_netHost->code());
                         c.raw("Port    : " + std::to_string(m_netHost->listenPort()));
                     }
                     if (m_mpTunnel && m_mpTunnel->state() == Tunnel::State::Up)
                         c.raw("Address : " + m_mpTunnel->address());
                 }});

    console.add({"config", "config [key] [value]", "show or change a setting", 0, 2,
                 [&](const std::vector<std::string>& a, ServerConsole& c) {
                     if (a.empty()) {
                         c.raw("Settings (config <key> <value> to change):");
                         for (const ServerConfig::Entry& e : config.entries())
                             c.raw("  " + e.key + std::string(28 - std::min<size_t>(27, e.key.size()), ' ') +
                                   e.value + "   [" + serverSettingScopeName(e.scope) + "] " + e.help);
                         return;
                     }
                     if (a.size() == 1) {
                         bool ok = false;
                         const std::string v = config.get(a[0], ok);
                         if (ok) c.raw(a[0] + " = " + v);
                         else    c.warn("no setting called '" + a[0] + "'.");
                         return;
                     }
                     std::string why;
                     if (!config.set(a[0], a[1], srvInLobby(), why)) { c.warn(why); return; }
                     c.info(a[0] + " = " + a[1]);
                     // Written back immediately: a change that survived only
                     // until the next restart is a change an operator will make
                     // twice and then stop trusting.
                     std::string saveWhy;
                     if (!configPath.empty() && !config.save(configPath, saveWhy))
                         c.warn("changed for this run, but could not write the file: " + saveWhy);
                     // Settings the open lobby can take right now.
                     if (m_netHost && srvInLobby()) {
                         LobbySettings ls = m_netHost->lobby().settings();
                         ls.maxPlayers = config.maxPlayers;
                         m_netHost->lobby().configure(ls);
                         m_netHost->broadcastLobby();
                     }
                     if (a[0] == "log-level") {
                         ServerLogLevel lv = ServerLogLevel::Info;
                         if (serverLogLevelFromName(config.logLevel, lv)) c.setLevel(lv);
                     }
                 }});

    // No "write the world now": there is nothing to flush. Turn resolution
    // appends each turn to the .odsv as it resolves (SaveManager::appendTurn),
    // so the file on disk is never behind by more than the turn in progress.
    // This reports where that file is, which is the question `save` was really
    // being asked -- a command that pretended to do a save the engine does not
    // have would be worse than not having one.
    console.add({"save", "save", "where this world is being written", 0, 0,
                 [&](const std::vector<std::string>&, ServerConsole& c) {
                     if (m_currentSavePath.empty()) { c.warn("no world yet."); return; }
                     c.raw(m_currentSavePath);
                     c.raw("Written turn by turn as each one resolves; nothing to flush.");
                 }});

    console.add({"stop", "stop", "save and shut the server down", 0, 0,
                 [&](const std::vector<std::string>&, ServerConsole& c) {
                     c.info("stopping.");
                     c.requestStop();
                 }});

    // ── the world ──

    std::string mapPath = config.map, mapName = config.map;
    if (config.loadSave.empty()) {
        if (!mpResolveMap(config.map, mapPath, mapName)) {
            console.error("no map called '" + config.map + "'. Set `map` in " + configPath +
                          " to a shipped id (1914, 1918, 1939, 1945, 1962, map) or a "
                          ".odmap path.");
            return 2;
        }
        console.info("map: " + mapName);
        m_mpMapId = config.map;
        startNewGameWithName(mapPath, config.worldName);
    } else {
        console.info("resuming " + config.loadSave);
        startLoadedGame(fs::path(config.loadSave).filename().string());
    }

    // The async loader normally runs a step per frame from Game::run(). There
    // is no run() here, so drive it -- the same shape runHeadlessSimulation
    // uses, and for the same reason.
    while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE) {
        if (console.stopping()) { console.warn("stopped while loading."); return 1; }
        updateLoading();
    }
    if (m_loadingFailed) {
        console.error("could not load the world.");
        return 2;
    }
    hideLoadingScreen();
    m_currentScreen = SCREEN_PLAYING;

    // ── A WORLD NOBODY IS GOING TO PLAY ──
    //
    // Loading a map auto-creates the save it will be played in, and that has
    // already happened by here. --check had a hand-written cleanup for this
    // (the comment below it counts 2,668 abandoned worlds, 1.7 GB) but --check
    // is only ONE of six ways out of this function, and the other five leaked:
    // an unknown map, a failed load, a stop during loading, a required mod
    // that is not installed, and a session that would not open. Running the
    // shipped smoke test on a misconfigured server left one world per run.
    //
    // So the cleanup is a scope guard instead of a statement somebody has to
    // remember at each return, and `keep` is set in exactly one place: once
    // the session is actually open and somebody can join it.
    struct DropUnplayedWorld {
        Game* g;
        bool keep = false;
        ~DropUnplayedWorld() { if (!keep) g->dropAutoCreatedSave(); }
    } unplayed{this};
    m_playerCountryId = 0;      // the server holds nothing; see the header

    // ── mods ──
    //
    // The server loads whatever is installed and enabled in its data directory,
    // exactly as the game does, and then REQUIRES the subset the config names.
    // Requiring is a separate step from running: a server can run a mod without
    // insisting joiners have it (a server-side balance mod), and can insist on
    // one the config names but the machine does not have -- which is a
    // misconfiguration worth refusing to start over rather than discovering as
    // every client being turned away.
    //
    // ModAttest.h is emphatic that this is an INTEGRITY check and not an
    // anti-tamper one: a client can claim any mod list it likes. What stops
    // cheating is authority -- orders are validated and re-attributed here, and
    // game state only ever flows server to client. This catches the ordinary
    // case, which is somebody on version 1.2 against a server running 1.3.
    ModManager::get().init(m_dataDir + "mods", m_dataDir + "mods.json");
    {
        const std::vector<ModAttestEntry> installed = ModManager::get().attestation();
        for (const ModAttestEntry& e : installed)
            console.info("mod loaded: " + e.id + " " + e.version);
        if (installed.empty() && !config.requiredMods.empty())
            console.warn("no mods are installed, but `mods` names some.");

        ModAttestation required;
        for (const std::string& want : config.requiredMods) {
            bool found = false;
            for (const ModAttestEntry& e : installed)
                if (e.id == want) { required.entries.push_back(e); found = true; break; }
            if (!found) {
                console.error("`mods` requires '" + want + "', which is not installed "
                              "and enabled here. Install it, or take it out of the "
                              "config -- a server cannot require a mod it does not "
                              "have, because it could never satisfy its own check.");
                return 2;
            }
        }
        if (!required.entries.empty()) {
            required.sort();
            m_mpRequiredMods = modAttestEncode(required);
            std::string names;
            for (const ModAttestEntry& e : required.entries) {
                if (!names.empty()) names += ", ";
                names += e.id + " " + e.version;
            }
            console.info("required of every player: " + names);
        }
    }

    // ── the session ──

    m_mpNameField  = config.sessionName;
    m_mpPortField  = std::to_string(config.port);
    m_mpBindAll    = config.bindAll;
    m_mpListed     = config.listed;
    m_mpAnonymous  = config.anonymous;
    m_mpDedicated  = true;
    m_mpViaRelay   = config.relay;
    m_mpMaxPlayers = (int)config.maxPlayers;
    m_mpAssignment = (config.assignment == "host") ? 0 : 1;
    m_mpLateJoin   = (config.lateJoin == "refuse") ? 0 : 1;
    m_mpAbsent     = (config.absent == "idle") ? 1 : 0;

    if (config.checkOnly) {
        console.info("world loaded: " + std::to_string(m_provinces.getAllProvinces().size()) +
                     " provinces, " + std::to_string(m_countries.getAll().size()) + " countries");
        console.info("config and content are good. Not opening a session (--check).");

        // ── AND LEAVE NOTHING BEHIND, WHICH IT DID NOT ──
        //
        // Loading a map auto-creates the save the world will be played in, and
        // that happens before this branch. So --check, which exists to answer
        // "is this box configured correctly" and says in the line above that it
        // is not opening a session, wrote a full world into data/saves/ on
        // every run. An operator health-checking a server in a loop filled
        // their disk with worlds nobody ever played -- this tree has 2,668 of
        // them, 1.7 GB, and the game's own save browser shows them all.
        //
        // Only the file this run just created, and only under --check.
        //
        // m_autoCreatedSave IS that distinction, and leaving it out of the
        // condition meant this deleted the save it had been asked to open:
        // --load names an existing world, startLoadedGame puts that path in
        // m_currentSavePath and sets this flag false, and then a health check
        // erased somebody's game. The comment above was already right; the
        // code below it was not.
        // The removal itself is DropUnplayedWorld's, above: this return is
        // one of six and they all need it.
        unloadGameData();
        return 0;
    }

    mpOpenHost();
    if (!m_netHost || m_netHost->phase() == NetHost::Phase::Closed) {
        console.error("could not open the session" +
                      (m_netHost && !m_netHost->error().empty()
                           ? ": " + m_netHost->error() : std::string(".")));
        return 3;
    }
    // Open. Somebody can join, so the world is theirs now and stays on disk
    // when this returns -- the shutdown path prints where it is.
    unplayed.keep = true;

    // ── reachability ──
    //
    // A dedicated server's whole job is being reachable, so this is reported in
    // full rather than left for someone to infer from silence.
    // NO TUNNEL FOR A RELAYED HOST. There is no listening port to point one
    // at, so `tunnel: auto` would start cloudflared, spend half a minute
    // getting an address, and publish a hostname that answers nothing. The
    // relay is already the route in.
    if (config.relay && config.tunnel != ServerTunnelMode::Off) {
        console.info("no tunnel: this server is relayed, so there is no port to "
                     "expose and no address to publish.");
    } else if (config.tunnel != ServerTunnelMode::Off) {
        // The ONLY place a dedicated server opens a tunnel. mpOpenHost has its
        // own start for the host screen; it is gated on !m_headless precisely so
        // that this stays the only one, because that block reads a checkbox this
        // function does not set.

        TunnelProvider want = TunnelProvider::None;
        if (config.tunnel == ServerTunnelMode::Cloudflared)  want = TunnelProvider::Cloudflared;
        if (config.tunnel == ServerTunnelMode::LocalhostRun) want = TunnelProvider::LocalhostRun;
        if (config.tunnel == ServerTunnelMode::Auto) {
            for (TunnelProvider p : tunnelProvidersAvailable())
                if (tunnelProviderWorksUnattended(p)) { want = p; break; }
        }
        if (want == TunnelProvider::None) {
            console.warn("no tunnel program on this machine. Players can only reach this "
                         "server if its port is forwarded, or set `tunnel` to off to stop "
                         "this warning. Install cloudflared to have one made for you.");
        } else {
            if (!m_mpTunnel) m_mpTunnel = new Tunnel();
            std::string why;
            if (!m_mpTunnel->start(want, m_netHost->listenPort(), why))
                console.warn(std::string("could not start ") + tunnelProviderName(want) +
                             ": " + why);
            else
                console.info(std::string("starting a ") + tunnelProviderName(want) + " tunnel...");
        }
    }

    if (config.relay) {
        // No port was bound, so there is none to report. Reporting one anyway
        // ("listening on port 0") reads as a server that failed to bind.
        console.info("hosting through the account service; no port is open.");
    } else {
        console.info("listening on port " + std::to_string(m_netHost->listenPort()) +
                     (config.bindAll ? " (all interfaces)" : " (loopback only)"));
    }
    if (!m_netHost->listenNote().empty()) console.warn(m_netHost->listenNote());

    return 0;
}

/**
 * One iteration of the server. False once it has been asked to stop.
 *
 * Split out of the loop so something other than a loop can drive it: a UI
 * front end calls this once per frame, between drawing. The console server
 * is the same thing with nothing drawn.
 */
bool Game::serverTick() {
    if (!m_srv || !m_srv->console || !m_netHost) return false;
    ServerConfig& config = *m_srv->config;
    ServerConsole& console = *m_srv->console;
    if (console.stopping()) return false;
    {
        // 1. Anything typed.
        std::string line;
        while (console.poll(line)) console.dispatch(line);

        // 2. The network, and the game's own host logic. Unchanged from the
        //    client's: this is the point of running the same code.
        mpDrainEvents();
        mpHostTurnUpdate();
        if (m_mpTunnel) m_mpTunnel->update();

        // ── A SESSION THAT FAILED AFTER OPENING WAS ASKED FOR ──
        //
        // Opening is ASYNCHRONOUS: mpOpenHost() starts a worker that posts to
        // the account service, fetches the issuer key and only then goes Live.
        // serverBegin checks for failure the instant it returns, which is
        // before that worker can possibly have finished -- so it only ever
        // caught the synchronous refusals, and a failure a second later was
        // never looked at again.
        //
        // The result was a server that sat silent forever: no join code, no
        // error, nothing in the log after "listening on port". The reason was
        // in m_netHost->error() the whole time with nobody reading it. Say it
        // once, and stop -- a dedicated server whose session did not open has
        // no job left to do, and staying up pretends otherwise.
        if (!m_srv->announcedFailure && m_netHost->phase() == NetHost::Phase::Closed) {
            m_srv->announcedFailure = true;
            m_srv->exitCode = 4;
            const std::string why = m_netHost->error();
            console.error("the session did not open" +
                          (why.empty() ? std::string(".") : ": " + why));
            return false;
        }

        // 3. Say the things an operator is waiting to be told, once each.
        if (!m_srv->announcedCode && m_netHost->phase() == NetHost::Phase::Live) {
            m_srv->announcedCode = true;
            console.info("session open. Join code: " + m_netHost->code());
            if (config.relay) {
                // The code IS the invite here: there is no address half to
                // pair it with, which is also why a browser can join at all.
                console.info("players join with that code alone -- leave the "
                             "address blank. Web players can only join this way.");
            } else if (config.tunnel == ServerTunnelMode::Off && !config.bindAll) {
                console.warn("bind-all is false and no tunnel is running, so only this "
                             "machine can reach the server.");
            }
        }
        if (!m_srv->announcedTunnel && m_mpTunnel) {
            if (m_mpTunnel->state() == Tunnel::State::Up) {
                m_srv->announcedTunnel = true;
                console.info("address: " + m_mpTunnel->address());
                console.info("players need BOTH the address and the join code above.");
            } else if (m_mpTunnel->state() == Tunnel::State::Failed) {
                m_srv->announcedTunnel = true;
                console.warn("the tunnel failed: " + m_mpTunnel->error());
            }
        }

        // 4. Automation. Every branch is a decision a human host would make by
        //    clicking, with the config saying what the answer is.
        const uint32_t holding = srvPlayersHoldingCountries();
        if (holding > 0 && m_srv->firstArrivalAt == 0.0) m_srv->firstArrivalAt = nowSeconds();

        if (srvInLobby()) {
            const ServerAutomation& au = config.automation;
            bool go = m_srv->startRequested;
            std::string because = "asked from the console";

            if (!go && au.startAtPlayers > 0 && holding >= au.startAtPlayers) {
                go = true;
                because = std::to_string(holding) + " players are ready";
            }
            if (!go && au.startAfterSeconds > 0 && m_srv->firstArrivalAt > 0.0 &&
                nowSeconds() - m_srv->firstArrivalAt >= au.startAfterSeconds &&
                holding >= au.startMinPlayers) {
                go = true;
                because = "the lobby deadline passed with " + std::to_string(holding) +
                          " players";
            }
            // The floor applies to the deadline, not to `start`: an operator
            // typing the command has decided, and a server arguing with a
            // person at its own console is not being careful, it is being
            // annoying.
            if (go && !m_srv->startRequested && holding < au.startMinPlayers) go = false;

            if (go) {
                m_srv->startRequested = false;
                std::string why;
                // force: anyone still choosing becomes a spectator rather than
                // holding up a server nobody is watching. Lobby::start says so.
                if (m_netHost->startGame(why, /*force=*/true)) {
                    m_srv->gameStartedAt = nowSeconds();
                    m_srv->lastAutoAdvanceAt = m_srv->gameStartedAt;
                    console.info("game started -- " + because + ".");
                    if (!config.motd.empty()) m_netHost->sendChat(config.motd);
                } else {
                    console.warn("could not start: " + why);
                }
            }
        }

        // 5. Turns.
        if (srvInGame()) {
            const ServerAutomation& au = config.automation;
            bool advance = m_srv->stepRequested;
            if (!advance && au.advanceEverySeconds > 0 &&
                nowSeconds() - m_srv->lastAutoAdvanceAt >= au.advanceEverySeconds) {
                advance = true;
            }
            if (advance) {
                m_srv->stepRequested = false;
                m_srv->lastAutoAdvanceAt = nowSeconds();
                mpResolveTurn();
                console.info("turn " + std::to_string(m_turnNumber) + " resolved.");
            }

            // 6. Is this game over?
            std::string ending;
            if (au.endAtTurn > 0 && (uint32_t)m_turnNumber >= au.endAtTurn)
                ending = "turn " + std::to_string(au.endAtTurn) + " reached";
            else if (au.endAfterHours > 0 && m_srv->gameStartedAt > 0.0 &&
                     nowSeconds() - m_srv->gameStartedAt >= au.endAfterHours * 3600.0)
                ending = std::to_string(au.endAfterHours) + "h of play";
            else if (au.endWhenEmpty && m_srv->firstArrivalAt > 0.0 && srvConnectedPlayers() == 0)
                ending = "everybody left";

            if (!ending.empty()) {
                console.info("game over: " + ending + ".");
                if (au.returnToLobbyWhenDone) {
                    m_netHost->returnToLobby();
                    m_netHost->broadcastLobby();
                    m_srv->firstArrivalAt = 0.0;
                    m_srv->gameStartedAt = 0.0;
                    console.info("back in the lobby, waiting for players.");
                } else {
                    console.requestStop();
                }
            }
        }

    }
    return !console.stopping();
}

/** Save, close the tunnel and tell everyone. Safe to call once. */
void Game::serverEnd() {
    if (!m_srv || !m_srv->console) return;
    ServerConsole& console = *m_srv->console;
    // ── shutdown ──
    console.info("shutting down.");
    if (!m_currentSavePath.empty())
        console.info("world is at " + m_currentSavePath);
    if (m_mpTunnel) { m_mpTunnel->stop(); delete m_mpTunnel; m_mpTunnel = nullptr; }
    mpShutdown();
    unloadGameData();
    m_srv.reset();
}

/**
 * The console server: begin, tick until told to stop, end.
 *
 * The sleep is HERE and not in serverTick() because it is the console's
 * answer to having nothing to do. A UI front end is already paced by its
 * own frame rate, and adding 20ms to that would halve it.
 */
int Game::runDedicatedServer(ServerConfig& config, ServerConsole& console,
                             const std::string& configPath) {
    const int rc = serverBegin(config, console, configPath);
    if (rc != 0) return rc;
    // --check returns 0 from serverBegin having deliberately left no
    // runtime behind, so this loop does not run and there is nothing to end.
    int rc2 = 0;
    while (m_srv && serverTick()) {
        // A server spends nearly all its life with nothing to do, and a spin
        // loop would burn a core to discover that -- on a VPS that is the
        // whole machine, and on a metered one it is a bill.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // Read BEFORE serverEnd(), which tears the runtime down.
    if (m_srv) rc2 = m_srv->exitCode;
    serverEnd();
    return rc2;
}
