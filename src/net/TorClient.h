#pragma once

// A Tor the game starts for itself, so a player never has to.
//
// WHY
//
// Reaching an onion, or hiding your IP, needs a Tor client running on this
// machine. Asking players to install one and leave it open is the step that
// makes the feature not get used -- and on macOS a game opened from Finder
// does not even search Homebrew's directory, so an installed tor is invisible
// to it anyway. So releases carry the Tor Project's own `tor` (the "Expert
// Bundle", fetched and checksum-verified at release time by
// tools/fetch_tor.py), and when a connection wants Tor and none is answering,
// the game starts that one:
//
//   on a free loopback port of its own -- never 9050, which may be a system
//     tor somebody configured, or 9150, which is Tor Browser's;
//   with its own data directory, so circuits are cached between sessions;
//   tied to the game's process id (__OwningControllerProcess), so it exits
//     when the game does, crash included -- nothing is left running.
//
// A Tor already running (the tor service, Tor Browser) is used in preference:
// it has circuits already and is the player's own choice.
//
// Nothing here downloads anything. The binary is either in the release or
// already installed; if neither, the player is told what to install.

#include <string>

namespace torclient {

/**
 * Where to look and where to keep state: `dataDir` is the game's data
 * directory. The bundled binary is <dataDir>/tor/tor (tor.exe on Windows),
 * and the client keeps its cache in <dataDir>/tor-client.
 */
void setDataDir(const std::string& dataDir);

/**
 * Whether ensure() may start the game's own Tor. On by default; tests turn it
 * off to check what happens with no Tor at all.
 */
void setAutoStart(bool on);

/** The tor to run: the bundled one, then PATH and the usual install places. */
std::string findBinary();

/**
 * A SOCKS port with Tor behind it: one already running, or one started now.
 * BLOCKING, for up to `timeoutSec` while Tor connects -- call it from a worker
 * thread, never the frame. 0 with `why` when there is no Tor to be had.
 */
int ensure(std::string& why, int timeoutSec = 150);

/** "Starting Tor: 45%" while the game's own Tor is connecting; empty otherwise. */
std::string status();

/** Stop the game's own Tor, if it started one. Safe to call when it did not. */
void shutdown();

}  // namespace torclient
