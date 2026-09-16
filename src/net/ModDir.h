#pragma once

// The mod directory, in the game.
//
// ── WHAT THIS IS FOR ──
//
// Until now nothing in the game said the directory existed. The mod menu could
// list what you had already installed and open an author's page when an update
// was found, and that was all -- so a player who had never been told about the
// website had no way to learn there were mods at all.
//
// This fetches the listings so they can be shown. It does NOT fetch mods. The
// game never downloads or installs one (ModUpdates.h, "LOOKS, NEVER TOUCHES"),
// and a directory is not a reason to change that: every listing offers a page
// to open, and the player fetches the file themselves.
//
// ── THE PARSER IS SEALED, FOR THE ANNOUNCEMENTS REASON ──
//
// Same five rules as odlfg (see Lfg.h, which explains them): a listing is
// written by a stranger and drawn on your screen, so it is data, every field is
// bounded, anything unrecognised is dropped whole, a listing can never name a
// code path, and no network means an empty list rather than an error.
//
// A field longer than its bound reads as ABSENT rather than truncated -- that
// is httpJsonString's rule, not a choice made here -- so an over-long name or
// id drops the whole listing, and an over-long summary simply leaves it blank.
// Dropping is the right way round: a row drawn from a listing we could not
// fully read is a row that misrepresents somebody's mod.
//
// One rule matters more here than it does for the board, because a listing here
// carries a URL and the screen offers a button that opens it: **a page is
// opened only if it is https**. The service already refuses anything else at
// publish time; this refuses it again at the moment it would be acted on,
// because that is the check that has to hold even if the service is wrong, or
// is not the service you think it is.

#include <cstdint>
#include <string>
#include <vector>

namespace odmoddir {

struct Limits {
    static constexpr size_t kDocument     = 512 * 1024;
    static constexpr size_t kItems        = 60;
    static constexpr size_t kIdChars      = 128;
    static constexpr size_t kNameChars    = 80;
    static constexpr size_t kVersionChars = 32;
    static constexpr size_t kByChars      = 40;
    static constexpr size_t kSummaryChars = 200;
    static constexpr size_t kUrlChars     = 512;
    static constexpr size_t kModules      = 12;
    static constexpr size_t kModuleChars  = 32;
};

/** What the third-party check found. Four states, and they stay four. */
enum class Scan : uint8_t {
    Unscanned = 0,  ///< nobody asked: no scanner configured, or it could not be reached
    Unknown,        ///< asked, and the scanner has never seen this file. NOT "clean".
    Clean,          ///< engines have seen it and none report it
    Flagged         ///< engines report it
};

/** Where a mod runs, which decides whether other players need it too. */
enum class Side : uint8_t { Client = 0, Server, Synchronised };

struct Listing {
    std::string id;
    std::string name;
    std::string version;
    std::string by;
    std::string summary;
    std::string page;              ///< https, or empty. Never opened unless https.

    Side side = Side::Client;
    Scan scan = Scan::Unscanned;
    int  flagged = 0;              ///< engines reporting it, when scan == Flagged
    int  engines = 0;              ///< how many looked, so a count has a denominator

    std::vector<std::string> modules;
    /** Asks for GameProcess or GameState.Write: it changes how turns resolve. */
    bool changesTurns = false;

    /** "Client", "Host only", "Everyone needs it". Never translated here. */
    const char* sideWord() const;
};

/**
 * Parse `GET /mods`.
 *
 * `error` is for a log and never for the player: an empty list is the answer a
 * player should see when the network is down, not a sentence about JSON.
 */
std::vector<Listing> parse(const std::string& json, std::string& error);

/** Whether this is a URL the game may hand to a browser. https only. */
bool openable(const std::string& url);

}  // namespace odmoddir
