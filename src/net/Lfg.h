#pragma once

// The looking-for-a-game board, in the game.
//
// ── THE SAME RULES, IN TWO PLACES, ON PURPOSE ──
//
// The service enforces the board's rules (net/src/lfg/board.ts). This file
// repeats them, and that duplication is deliberate: a player typing a listing
// should be told "a hosting listing needs your invite code" while they are
// typing it, not after a round trip. The service is what DECIDES; this is what
// EXPLAINS. If the two ever disagree, the service wins and the player sees its
// message.
//
// ── AND THE PARSER IS SEALED, FOR THE ANNOUNCEMENTS REASON ──
//
// A listing is written by a stranger and drawn on your screen, which is the
// shape of the bug that Announcements.h exists to refuse. So the same five
// rules apply here: it is data, every field is bounded, anything unrecognised
// is dropped whole, a listing can never name a code path, and no network means
// an empty board rather than an error.
//
// The one action a listing can cause is JOINING A GAME BY ITS INVITE CODE, and
// the code goes through the same path as a code typed by hand -- including
// signing in first. There is no address field, so a listing cannot point the
// game at a machine of the poster's choosing.

#include <cstdint>
#include <string>
#include <vector>

namespace odlfg {

/** Which kind of listing this is. The channel calls these "tags". */
enum class Kind : uint8_t { Hosting = 0, Looking };

/** Turn mode, as the lobby means it. */
enum class Mode : uint8_t { Rapid = 0, Longform };

struct Limits {
    static constexpr size_t kMapChars = 48;
    static constexpr size_t kNoteChars = 240;
    static constexpr size_t kLanguageChars = 24;
    static constexpr size_t kRegionChars = 24;
    static constexpr size_t kNickChars = 32;
    static constexpr size_t kCodeChars = 32;
    static constexpr size_t kIdChars = 32;
    /** Listings kept from one reply. The service sends at most 40. */
    static constexpr size_t kItems = 40;
    /** A whole board. Past anything real, far short of a memory bomb. */
    static constexpr size_t kDocument = 64 * 1024;
    static constexpr int kTurnSecondsMin = 30;
    static constexpr int kTurnSecondsMax = 3600;
    static constexpr int kTurnHoursMin = 1;
    static constexpr int kTurnHoursMax = 168;
    static constexpr int kSlotsMin = 2;
    static constexpr int kSlotsMax = 32;
    static constexpr int kMinutesMin = 15;
    static constexpr int kMinutesMax = 360;
};

/** One entry on the board, after every field has been bounded. */
struct Listing {
    std::string id;
    Kind kind = Kind::Hosting;
    std::string nick;
    std::string code;        ///< hosting only; empty otherwise
    std::string map;
    Mode mode = Mode::Rapid;
    int turnSeconds = 0;     ///< rapid only
    int turnHours = 0;       ///< longform only
    int slotsTaken = 0;
    int slotsTotal = 0;      ///< 0 when the listing does not say
    std::string language;
    std::string region;
    std::string note;
    long long createdAt = 0;
    long long expiresAt = 0;
    int reports = 0;

    bool joinable() const { return kind == Kind::Hosting && !code.empty(); }
    /** How the pace reads in one short line: "Rapid, 2 min a turn". */
    std::string paceLine() const;
    /** "3/6 players", or empty when the listing does not say. */
    std::string seatsLine() const;
    /** "closes in 40 min", or "closing" once it is past. */
    std::string closesIn(long long now) const;
};

/** What a player is filling in. Strings, because that is what a text field is. */
struct Draft {
    Kind kind = Kind::Hosting;
    std::string code;
    std::string map;
    Mode mode = Mode::Rapid;
    int turnSeconds = 120;
    int turnHours = 24;
    int slotsTaken = 1;
    int slotsTotal = 6;
    std::string language;
    std::string region;
    std::string note;
    int minutes = 0;         ///< 0 means the service's default
};

/**
 * The board, parsed from the service's reply.
 *
 * Fails closed: a malformed document is an empty board and a message in
 * `error`, never a partly-read list.
 */
std::vector<Listing> parseBoard(const std::string& json, std::string& error);

/** Drop what has expired since the reply arrived. */
std::vector<Listing> live(const std::vector<Listing>& all, long long now);

/**
 * The rules, checked locally so the player is told before a request.
 *
 * Returns an empty string when the draft is fine, and otherwise the same
 * sentence the service would have sent back.
 */
std::string problemWith(const Draft& draft);

/** The request body for posting a draft. Only called when problemWith() is empty. */
std::string postBody(const Draft& draft);

/** Bodies for the other two calls, kept here so the JSON lives in one place. */
std::string closeBody(const std::string& id);
std::string reportBody(const std::string& id, const std::string& reason, const std::string& note);

/**
 * Which tag the board is showing.
 *
 * Both kinds on one board is right -- half a dozen people do not fill two
 * rooms -- but the two are read for opposite reasons: somebody who wants to
 * play now is looking for HOSTING and nothing else, and a host with an empty
 * lobby wants the people who said they are LOOKING. Scrolling past the other
 * half to find them is the whole of the complaint.
 */
enum class Filter : uint8_t { All = 0, Hosting, Looking };

/**
 * The listings a filter shows, in the order they should be drawn.
 *
 * Newest first, as the service sends them, EXCEPT that a hosting listing with
 * no seat left sinks below the ones you can still join: a full game is not a
 * reason to open the board, and it is not what the top of a list is for.
 * Pure, so it is tested without a board (tests/lfg_test.cpp).
 */
std::vector<Listing> view(const std::vector<Listing>& all, Filter f);

/** How many of each tag are on the board, for the chips that switch filters. */
struct Counts { int hosting = 0; int looking = 0; };
Counts count(const std::vector<Listing>& all);

/** The guidelines, in the words the channel uses. Drawn above the form. */
std::vector<std::string> guidelines();

/**
 * The invite to the other half of this board.
 *
 * Here rather than only in the Community screen's link table because the board
 * and the Discord channel are one feature: a listing posted in the game appears
 * in #looking-for-a-game and the other way round, so an in-game board that
 * pointed people at a DIFFERENT server than the Community button would be
 * pointing them away from their own listings. One constant, both call sites.
 */
extern const char* const kDiscordInvite;

const char* kindName(Kind kind);
const char* modeName(Mode mode);

}  // namespace odlfg
