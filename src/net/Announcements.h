#pragma once

// The board on the main menu: what the game will accept from the announcement
// service, and what it flatly will not.
//
// ── THIS IS A SEALED FORMAT, AND THAT IS THE WHOLE POINT ──
//
// An announcement is written elsewhere, fetched over the network, and drawn on
// every player's main menu. That is the exact shape of a remote code execution
// bug: content from a server, rendered by a client. So the rules here are not
// conveniences, they are the security boundary.
//
//   1. IT IS DATA. There is no script, no expression, no template, nothing
//      evaluated. A body is TEXT, marked up with the same dialogue markup the
//      tutorial uses (src/dialog/DialogScript.h) -- a parser that produces
//      styled glyph runs and cannot do anything else.
//   2. A BUTTON CANNOT NAME AN ACTION. It picks one from a closed enum defined
//      HERE, in the game. The document supplies at most one short parameter,
//      and what that parameter means is decided by the game, not by the
//      document. Nothing can ask the game to open a URL, run a file, load a
//      mod, or reach any code path this header did not already list.
//   3. EVERY FIELD IS BOUNDED. Lengths, counts, and the number of
//      announcements are all capped, so a hostile or broken document costs a
//      fixed amount of memory and nothing more.
//   4. ANYTHING UNRECOGNISED IS DROPPED, never guessed at. An unknown action,
//      an unknown field, a malformed entry: the entry is refused whole. A board
//      that silently half-renders somebody's tampering is worse than an empty
//      board.
//   5. IT MAY ONLY EVER FAIL CLOSED. No network, a bad reply, an empty list --
//      all of them mean "no board", which is what a player with no internet
//      sees and is not an error worth showing them.
//
// The parser is pure and takes a string, so every one of those refusals is
// testable without a socket. See tests/announcements_test.cpp.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace odnews {

/**
 * What a button may do. A CLOSED SET, defined in the game.
 *
 * The document chooses one of these by name and may supply `param`; it can
 * never introduce a new one. Adding an action is a code change made by somebody
 * who can read what it does -- which is the property that makes a remote button
 * safe to draw at all.
 */
enum class Action : uint8_t {
    None = 0,
    /**
     * Join a multiplayer game by its invite code. `param` is the code, and it
     * goes through exactly the same path a player typing it would -- including
     * signing in first if they are not signed in.
     */
    JoinGame,
    /// Open the Community screen. Takes no parameter.
    Community,
    /// Open the Account screen, for "sign in to take part". No parameter.
    Account,
};

/// The name a document uses for each action. Unknown names are refused.
Action actionFromName(const std::string& name);
const char* actionName(Action a);

/** Bounds. A hostile document costs a fixed amount and no more. */
struct Limits {
    static constexpr size_t kTitle = 80;
    static constexpr size_t kBody = 1200;
    static constexpr size_t kLabel = 32;
    static constexpr size_t kParam = 64;
    static constexpr size_t kId = 64;
    static constexpr size_t kMaxItems = 8;
    /// A whole document. Well past anything real, far short of a memory bomb.
    static constexpr size_t kDocument = 64 * 1024;
};

struct Button {
    std::string label;
    Action action = Action::None;
    std::string param;
    bool ok() const { return action != Action::None && !label.empty(); }
};

/** One announcement, after every rule above has been applied. */
struct Item {
    std::string id;       ///< stable, so a player can be shown it once
    std::string title;
    std::string body;     ///< dialogue markup; drawn, never evaluated
    Button button;        ///< optional; `ok()` false means no button

    /**
     * Unix seconds. Both optional; 0 means "not given".
     *
     * `postedAt` is shown beside the announcement. `until` is when it takes
     * ITSELF off the board -- see expired(). A board that still advertises last
     * Saturday's tournament is worse than an empty one, and nobody should have
     * to be awake to take it down.
     */
    long long postedAt = 0;
    long long until = 0;

    /**
     * The moment the announcement is ABOUT, and how to show it.
     *
     * `postedAt` answers "is this still current?"; this answers "when is the
     * thing?" -- and for a tournament those are different questions with
     * different right answers. Stored as unix seconds, which is an INSTANT and
     * not a wall-clock reading, so it renders in whatever timezone the player
     * is actually in. Writing "18:00 UTC" into the body puts the arithmetic on
     * them; this does it for them.
     */
    long long eventAt = 0;
    /// How `eventAt` is drawn. See timeStyleFromName.
    enum class TimeStyle : uint8_t {
        None = 0,   ///< not shown
        Local,      ///< "Sat 12 Sep, 20:00" in the player's own timezone
        Countdown,  ///< "in 2 days" before it, "2 hours ago" after it
    };
    TimeStyle timeStyle = TimeStyle::None;

    /// Whether this has passed its own expiry. `now` is unix seconds.
    bool expired(long long now) const { return until != 0 && now >= until; }
};

/**
 * Parse a document. Returns only the entries that passed every rule.
 *
 * `error` is for the log and never for the player: a board that cannot be shown
 * is simply not shown. It is filled in for a malformed document, and left empty
 * when the document was fine and merely had nothing in it.
 */
std::vector<Item> parseDocument(const std::string& json, std::string& error);

/// Those of `items` that have not expired at `now` (unix seconds).
std::vector<Item> live(const std::vector<Item>& items, long long now);

/// The name a document uses for each style. Unknown names mean None.
Item::TimeStyle timeStyleFromName(const std::string& name);

/**
 * A unix instant as the player's own wall clock reads it: "Sat 12 Sep, 20:00".
 *
 * NOT pure -- it asks the machine what timezone it is in, which is the entire
 * point. Everything about it that CAN be tested without a clock is in
 * formatCountdown below.
 */
std::string formatLocal(long long unixSeconds);

/**
 * How far off `when` is from `now`, in words.
 *
 * "in 3 days", "in 4h 12m", "in 45s", and afterwards "2 hours ago". Pure, so
 * every boundary is testable: the switch from days to hours, the minute either
 * side of the moment itself, and the fact that it keeps counting AFTER rather
 * than freezing or going blank -- a tournament that started twenty minutes ago
 * is a thing somebody still wants to know about.
 */
std::string formatCountdown(long long when, long long now);

/**
 * Whether `param` is a plausible invite code, checked HERE rather than trusted.
 *
 * The join path is the one action that carries data from the document into the
 * rest of the game, so the shape of that data is pinned in the game: letters,
 * digits and dashes, and short. Anything else refuses the whole button.
 */
bool looksLikeInviteCode(const std::string& param);

}  // namespace odnews
