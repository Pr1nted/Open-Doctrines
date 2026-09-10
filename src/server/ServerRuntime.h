#pragma once

// What a running server is in the middle of, between one tick and the next.
//
// WHY THIS IS NOT A PILE OF LOCALS
//
// It was, and that was fine while the console loop was the only caller: the
// state lived in Game::runDedicatedServer and every command lambda captured it
// by reference. A front end cannot use a function shaped like that. A window has
// to draw a frame, then advance the server, then draw again -- so the server's
// progress has to survive the return.
//
// Splitting it out is what makes serverBegin / serverTick / serverEnd possible,
// and those are what make the UI mode and the Android app possible. The console
// server is now one caller of them rather than the only shape the server has.
//
// It holds POINTERS to the config and the console rather than copies. Both
// outlive the server by construction -- main owns them -- and copying the config
// would mean `config` in the console editing one object while the loop read
// another, which is the bug this shape exists to make impossible.

#include <cstdint>
#include <string>

struct ServerConfig;
class ServerConsole;

struct ServerRuntime {
    ServerConfig*  config = nullptr;
    ServerConsole* console = nullptr;
    std::string    configPath;

    /** Monotonic seconds, 0 meaning "has not happened yet". */
    double firstArrivalAt = 0.0;      // nobody has ever joined
    double gameStartedAt = 0.0;
    double lastAutoAdvanceAt = 0.0;

    bool     stepRequested = false;   // `step-go`
    bool     startRequested = false;  // `start`
    uint32_t lastAutosaveTurn = 0;

    /** Said once each, not every tick. */
    bool announcedCode = false;
    /// Said once, when an open that was still in flight turns out to have failed.
    bool announcedFailure = false;

    /**
     * What the process exits with.
     *
     * Non-zero when the run ended because something was wrong rather than
     * because it was asked to stop. A session that never opened used to exit 0,
     * which tells a supervisor -- systemd, a container runtime, a shell loop --
     * that the server did its job and shut down cleanly. It would not be
     * restarted, and nothing would be alerted.
     */
    int exitCode = 0;
    bool announcedTunnel = false;
};
