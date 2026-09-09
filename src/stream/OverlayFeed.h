#pragma once

// What the streaming software can read out of a running game.
//
// ── FILES, NOT A LOCAL SERVER ──
//
// The obvious build is an HTTP endpoint on localhost that OBS polls. It is also
// the wrong one for this: it opens a port, it triggers a firewall prompt on
// first run (during a stream, on camera), and it is one more listening socket
// to get right in a game that already has enough of them. None of that buys
// anything, because OBS reads files perfectly well.
//
// So the game writes two files into its own data directory and OBS points at
// them:
//
//   overlay.txt   one line per fact, for a Text source. No setup at all.
//   overlay.json  the same facts structured, for a Browser source that wants
//                 to lay them out itself.
//
// Both are rewritten when something changes. Nothing is ever read back, and
// nothing outside the game's own directory is touched.
//
// ── WHAT IS IN THEM, AND WHAT IS NOT ──
//
// Only what is already on screen: the turn, the date, who is being played, how
// the vote is going. NEVER an invite code, an account id or a file path -- an
// overlay is the one part of the game that is guaranteed to be on camera, so
// the stream-safe rules apply here whether or not the mode is on. See
// tests/overlay_feed_test.cpp.

#include <string>
#include <vector>

namespace overlay {

/** One line of the feed: a name and a value. */
struct Fact {
    std::string key;     ///< "turn", "country", "vote.1"
    std::string value;
};

/** Everything the overlay is told, in the order it should read. */
struct Feed {
    std::vector<Fact> facts;
    void add(const std::string& key, const std::string& value) {
        facts.push_back(Fact{key, value});
    }
};

/// "turn: 12" per line, for a Text source.
std::string toText(const Feed& feed);

/// The same, as a flat JSON object, for a Browser source.
std::string toJson(const Feed& feed);

/**
 * Whether a value may go into the feed at all.
 *
 * The overlay is guaranteed to be on camera, so this refuses the things
 * stream-safe mode hides -- a path with somebody's name in it, an account id --
 * regardless of whether the mode is on. A feed is not a place to be careful in;
 * it is a place to be unable to make the mistake.
 */
bool safeToShow(const std::string& value);

}  // namespace overlay
