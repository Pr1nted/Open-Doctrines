#pragma once

// Making a host reachable without asking them to run a second program.
//
// The recommended way to host is a tunnel in front of a loopback port: it is
// free, needs no router configuration, and keeps the host's IP address out of
// the game. But "open another terminal and run cloudflared" is exactly the kind
// of step that makes hosting feel broken, so the game starts one itself when it
// can and shows the address players should use.
//
// RUNNING ONE, AND FETCHING ONE, ARE DIFFERENT THINGS
//
// `Tunnel` below only ever RUNS a program that is already on the machine --
// found on PATH, or installed by the game into its own directory. It never
// downloads.
//
// `TunnelInstaller`, at the bottom of this file, is the part that downloads,
// and every safeguard lives there: one hard-coded source, a publisher-supplied
// SHA-256 checked before anything is executed, and an explicit request from the
// host each time. Keeping the two apart is the point -- the thing that launches
// processes has no idea where binaries come from, and the thing that downloads
// never launches anything.
//
// WHAT A TUNNEL COSTS, STATED HONESTLY
//
// The tunnel operator can see the game traffic passing through it. That is true
// of every tunnel, ours or otherwise, and it is the trade being made: the host's
// IP address is hidden from players, in exchange for a third party carrying the
// bytes. A host who would rather not make that trade forwards a port instead,
// and the game says so rather than presenting tunnelling as free of cost.
//
// Game traffic through a tunnel is NOT the relay this project rejected. The
// difference is who chooses: a host picking a tunnel is arranging their own
// reachability, where a relay would have put our infrastructure in everybody's
// path by default.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/** A tunnel program this machine could actually run. */
enum class TunnelProvider : uint8_t {
    None = 0,
    /** `cloudflared tunnel --url` -- no account, no signup, random hostname. */
    Cloudflared,
    /**
     * localhost.run over plain ssh. Needs nothing installed on any machine that
     * has ssh, which is every Mac and Linux box and Windows 10 onwards.
     */
    LocalhostRun,
};

const char* tunnelProviderName(TunnelProvider p);

/** What the host would have to install to use this provider, in words. */
std::string tunnelProviderHowToGet(TunnelProvider p);

/** Providers that are installed and runnable right now, best first. */
std::vector<TunnelProvider> tunnelProvidersAvailable();

/**
 * Whether a provider can open a tunnel with no further setup.
 *
 * Being installed is not the same as being usable. `ssh` is on every machine,
 * but anonymous localhost.run tunnels now connect, print a banner and never
 * issue an address -- so offering it as though it worked costs a host 25
 * seconds and then fails. Only providers that return true here are started
 * automatically; the rest have to be chosen deliberately by someone who knows
 * they have what it needs.
 */
bool tunnelProviderWorksUnattended(TunnelProvider p);

/**
 * Where the game keeps tools it installed itself. Searched before PATH, so a
 * copy the game fetched is preferred over whatever else is on the machine.
 */
void tunnelSetToolsDir(const std::string& dir);

/** Resolves a tunnel program to a path, or empty when it is not present. */
std::string tunnelResolveProgram(const char* name);

/**
 * Extract the public address a tunnel printed.
 *
 * Separated from the process handling so it can be tested against real captured
 * output rather than only against a live tunnel. Returns empty when the output
 * does not contain one yet -- which is the normal state for the first second or
 * two, not an error.
 */
std::string tunnelParseAddress(TunnelProvider provider, const std::string& output);

class Tunnel {
public:
    Tunnel();
    ~Tunnel();
    Tunnel(const Tunnel&) = delete;
    Tunnel& operator=(const Tunnel&) = delete;

    enum class State : uint8_t {
        Idle = 0,
        Starting,   // the process is up; no address announced yet
        Up,         // address() is what players should be given
        Failed,     // error() says why
    };

    /**
     * Start a tunnel to a local port. Non-blocking: poll with update().
     * False only when the tunnel could not be launched at all.
     */
    bool start(TunnelProvider provider, uint16_t localPort, std::string& error);

    /** Read whatever the tunnel has said. Call once a frame. */
    void update();

    void stop();

    State       state() const { return m_state; }
    /** `wss://host`, ready to hand to a player. Empty until Up. */
    std::string address() const { return m_address; }
    std::string error() const { return m_error; }
    TunnelProvider provider() const { return m_provider; }

    /** Everything the tunnel has printed, for a host diagnosing a failure. */
    std::string log() const { return m_log; }

    /** False when this build cannot start subprocesses at all. */
    static bool available();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    State          m_state = State::Idle;
    TunnelProvider m_provider = TunnelProvider::None;
    std::string    m_address;
    std::string    m_error;
    std::string    m_log;
};

// ========================================================= installing one ====
//
// Fetching cloudflared so a host does not have to open a terminal.
//
// THIS IS DOWNLOADING AND THEN RUNNING A PROGRAM, WHICH IS THE MOST DANGEROUS
// THING THIS GAME DOES. It is defensible only because of what surrounds it:
//
//   - ONE source, fixed in code: the cloudflare/cloudflared releases on GitHub,
//     over https, with no part of the URL taken from anything downloaded.
//   - VERIFIED before it is ever executed. GitHub publishes a SHA-256 digest
//     for each asset; the download is hashed and compared, and a mismatch is
//     deleted rather than run. Without this the whole thing would just be
//     "execute whatever arrived", which is the actual malware vector.
//   - ASKED FOR, every time, by a host who is told what will be fetched, from
//     where, and how large. Never automatic, never on launch.
//   - Installed beside the game's own data, not into the system. No
//     administrator rights are requested and none are needed.
//
// If any of those stops being true, this feature should go rather than be
// weakened -- the manual instructions work and cost a host one command.

/** Progress and outcome of an install. Polled from the game thread. */
struct TunnelInstallStatus {
    enum class Phase : uint8_t {
        Idle = 0,
        Asking,      // looking up the release
        Downloading,
        Verifying,
        Installing,
        Done,
        Failed,
    } phase = Phase::Idle;

    int         percent = 0;
    std::string message;
    /** Bytes the asset is expected to be, once known. 0 before that. */
    long long   sizeBytes = 0;

    bool busy() const {
        return phase == Phase::Asking || phase == Phase::Downloading ||
               phase == Phase::Verifying || phase == Phase::Installing;
    }
};

/**
 * Installs cloudflared into the game's own directory.
 *
 * Every call is non-blocking; the work happens on its own thread and progress
 * arrives through status(). One at a time.
 */
class TunnelInstaller {
public:
    TunnelInstaller();
    ~TunnelInstaller();
    TunnelInstaller(const TunnelInstaller&) = delete;
    TunnelInstaller& operator=(const TunnelInstaller&) = delete;

    /** `toolsDir` is where the binary lands, e.g. "<data>/tools". */
    bool begin(const std::string& toolsDir);

    TunnelInstallStatus status() const;

    /** Blocks until any in-flight install finishes. Call at shutdown. */
    void shutdown();

    /** True when this platform has an asset we know how to install. */
    static bool supported();

    /** What a host is told BEFORE agreeing: what, from where, how big. */
    static std::string describe();

    /** Where an already-installed copy would be. Empty if none is there. */
    static std::string installedPath(const std::string& toolsDir);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
