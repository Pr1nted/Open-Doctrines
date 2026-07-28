#pragma once

// Getting the host's world onto everybody else's machine.
//
// WHAT IS AND IS NOT SENT
//
// The MAP IS NOT SENT. A .odmap is tens of megabytes of image data, and every
// player already has the standard ones installed. So a snapshot NAMES the map
// and carries only what cannot be derived from it. A player who does not have
// that map is told which one they need, by name, rather than the join failing
// for no visible reason.
//
// What is carried:
//
//   - the map's name, so the client loads the same world;
//   - the turn number;
//   - the state JSON -- orders, claims, research, alignment: everything the
//     save format keeps outside the per-turn deltas;
//   - every turn delta so far, in the SAME binary form `.odsv` uses.
//
// The deltas are what make a mid-game join work: loading the named map gives
// the world as it started, and replaying the deltas moves it to now. For a game
// that has not started there are none, and the map's own initial state is
// already correct.
//
// WHY REUSE THE SAVE FORMAT
//
// Because it already exists, is already exercised every turn by every
// singleplayer game, and is already the thing the game knows how to apply. A
// second "network world format" would be a second thing to keep in step with
// the game's state, and the two would drift.
//
// EVERY FIELD HERE ARRIVES FROM THE NETWORK. Decoding is fail-closed: a short,
// truncated or hostile payload returns false and changes nothing, rather than
// half-loading a world.

#include <cstdint>
#include <string>
#include <vector>

/** One turn's changes, as `.odsv` packs them. */
struct NetTurnDelta {
    uint32_t             turn = 0;
    std::vector<uint8_t> packed;
};

struct NetWorldSnapshot {
    /** The map to load. A name, not a path: paths are not portable. */
    std::string mapName;

    /** The turn the world is at once everything below has been applied. */
    uint32_t turnNumber = 0;

    /** Game state outside the deltas; see Game::saveStateJson(). */
    std::string stateJson;

    /** Turns 1..N, in order. Empty when the game has not started. */
    std::vector<NetTurnDelta> turns;

    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t size, NetWorldSnapshot& out);
};

// Ceilings, applied while decoding and before allocating. A snapshot is large
// by the standards of this protocol, so "large" has to be bounded rather than
// trusted -- these are what stop a hostile host from making a client allocate
// arbitrarily on the strength of a length field.
inline constexpr size_t kNetMaxStateJson = 64u * 1024 * 1024;
inline constexpr size_t kNetMaxTurns = 100000;
inline constexpr size_t kNetMaxDeltaBytes = 64u * 1024 * 1024;
