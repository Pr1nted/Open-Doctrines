#pragma once

// Chat plays the country.
//
// ── THE SHAPE OF IT ──
//
// A streamer picks a country and hands it to their chat. Chat types commands;
// the game tallies them for a window; when the window closes the winner is
// applied and everybody sees why. That is the whole loop, and every part of it
// that can be decided without a socket is decided here.
//
// ── THE RULES THAT MATTER, AND WHY ──
//
// ONE VOTE EACH. A tally where the loudest typist wins is not a vote, it is a
// keyboard test. A viewer may change their mind -- the last thing they typed is
// what counts -- but they cannot stack.
//
// A COMMAND IS A CHOICE FROM A LIST, never free text. Chat is an untrusted
// stranger with a keyboard, and the list is built by the GAME from what that
// country could legally do this turn. So the worst a hostile chat can do is
// pick a legal order the streamer did not want, which is the game working.
//
// IT MUST BE READABLE ON A STREAM. The command is a short token -- "1", "war",
// "build" -- because viewers are typing on a phone while watching, and a syntax
// that needs a manual gets no votes at all.
//
// TIES GO TO THE EARLIEST. Not to random: a stream where the same tally can
// produce two different outcomes is a stream where chat argues about the game
// instead of playing it. First past the post, and first to be nominated wins a
// draw.
//
// The engine has no network in it and no game state; see
// tests/chat_vote_test.cpp.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace chatvote {

/** One thing chat may pick this window. Built by the game, not by chat. */
struct Option {
    std::string key;     ///< what a viewer types: "1", "war", "peace"
    std::string label;   ///< what the stream shows: "Declare war on Poland"
    int         id = 0;  ///< the game's own handle for it
};

/** A running tally. */
struct Tally {
    std::string key;
    std::string label;
    int         id = 0;
    int         votes = 0;
};

/**
 * Where a chat line came from. Kept so the same engine serves every platform:
 * the reader differs, the counting does not.
 */
enum class Source : uint8_t { Twitch = 0, YouTube, Kick, Local };

class Poll {
public:
    /**
     * Open a poll over `options`. An empty list is a closed poll: there is
     * nothing chat could pick, so nothing is counted.
     */
    void open(std::vector<Option> options, double now, double seconds);

    /// Whether a poll is running at `now`.
    bool open_at(double now) const;
    double closesAt() const { return m_closesAt; }
    double secondsLeft(double now) const;

    /**
     * Count one chat line.
     *
     * `who` is the viewer's name, which is what makes one-vote-each possible;
     * it is used as a key and is never shown, so a display name that is an
     * attempt at markup or an insult never reaches the screen.
     *
     * Returns true when the line actually moved the tally -- for the on-screen
     * "chat is voting" flicker, and so a line that matched nothing costs
     * nothing.
     */
    bool cast(const std::string& who, const std::string& message, double now);

    /// The tallies, highest first, ties broken by the order options were given.
    std::vector<Tally> standings() const;

    /**
     * The winner, or `id` 0 when nobody voted.
     *
     * A poll nobody voted in has NO winner rather than a default one: picking
     * something because a window elapsed is how a stream ends up at war by
     * accident, and "chat said nothing" is a thing the streamer should see.
     */
    Tally winner() const;

    int totalVotes() const { return (int)m_byViewer.size(); }
    void close() { m_closesAt = 0.0; }

private:
    std::vector<Option> m_options;
    /// viewer -> index into m_options. One entry each; a new vote replaces.
    std::unordered_map<std::string, size_t> m_byViewer;
    double m_closesAt = 0.0;
};

/**
 * The token a chat line is voting for, or empty.
 *
 * Lenient on purpose, because viewers type on phones: leading and trailing
 * space, a "!" in front, and any case all work. Strict about the rest -- a line
 * with anything else in it is somebody TALKING, and counting "war is stupid" as
 * a vote for war is how a chat loses faith in the tally.
 */
std::string tokenOf(const std::string& message);

}  // namespace chatvote
