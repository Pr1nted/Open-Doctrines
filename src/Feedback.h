#pragma once

// Sending a bug report, a suggestion or a rating from inside the game.
//
// WHY THE GAME AND NOT A FORUM
//
// A bug reported where it happened arrives with the version, the platform and
// the state the game was in; the same bug reported an hour later on a forum
// arrives as "it crashed sometimes". The gap between those two is most of the
// work of fixing it, and it is paid by whoever is least able to pay: the player
// who has to remember, find the right place, and describe a build they can no
// longer see.
//
// WHAT IS SENT, AND WHAT IS NEVER SENT
//
// The title and the description are typed by the player. The version, platform
// and a random install id go with them. Nothing else, unless the player ticks
// the diagnostics box -- and then they are shown the exact text first, in a
// scrollable box, before anything leaves the machine. There is no hidden
// payload and no silent telemetry: this file has one network call and it
// happens when somebody presses Send.
//
// Diagnostics are scrubbed of the home directory before they are shown, because
// a save path is "/Users/<real name>/..." on every desktop platform and a
// person pasting a bug report should not have to notice that.
//
// RATE LIMITING, IN TWO PLACES
//
// Here, so that a stuck key or a frustrated player cannot fire fifty reports
// (a cooldown between sends and a daily cap that survives a restart), and again
// in the Worker, because a client-side limit is a courtesy rather than a
// defence. See net/src/feedback/report.ts.

#include <string>
#include <vector>

class Game;
struct Config;

namespace feedback {

/// What a report is about. Order matters: it is the order of the chips.
enum class Category {
    UI = 0,        ///< menus, panels, text, layout
    AI,            ///< how the computer players behave
    Data,          ///< maps, saves, numbers that look wrong
    Multiplayer,
    Scripting,     ///< the map script engine
    Mods,          ///< the Gearbox SDK and installed mods
    Security,      ///< never becomes a public issue -- see the Worker
    Other,
    Count
};

// No Rating. The rating prompt sends people to the itch.io page now, where a
// rating is public and useful to other players -- and removing it from the wire
// removed the one path into this service that needed no account, which was an
// open pipe into a chat channel rationed only by a string the client invents.
enum class Kind { Bug = 0, Suggestion };

/// Stable wire values. Never translated: the Worker matches on these.
const char* categoryId(Category c);
const char* kindId(Kind k);

/// The label a player reads. Translated at the call site.
const char* categoryLabel(Category c);

/**
 * One report, as the form fills it in.
 *
 * `diagnostics` is empty unless the player asked for it, and is exactly the
 * text they were shown.
 */
struct Report {
    Kind        kind = Kind::Bug;
    Category    category = Category::UI;
    std::string title;
    std::string body;
    std::string diagnostics;
    /// The reporter asked not to be named in what gets published.
    bool        anonymous = false;
};

enum class Status {
    Idle = 0,
    Sending,
    Sent,
    Failed,
};

/**
 * Strip the player's home directory out of a string.
 *
 * Every save and data path on a desktop build starts with it, and on all three
 * platforms it contains the account name -- so a diagnostics block that
 * mentions a file mentions a person. Exposed because Game builds the
 * diagnostics (it is the only thing that can see the world) and this is the
 * rule that block has to obey.
 */
std::string scrubPersonalPaths(std::string text);

/// This platform, as the report labels it: "macos", "windows", "linux", "web".
const char* platform();

/**
 * Whether a report may be sent right now, and why not when it may not.
 *
 * `reason` is a translated sentence for the player, empty when the answer is
 * yes. Checked before the form is even opened, so a player who has run out of
 * reports today is told so instead of typing one out and losing it.
 */
bool canSend(Config& cfg, std::string& reason);

/// Start sending. False when a send is already in flight or canSend() said no.
bool send(const Report& report, Config& cfg, const std::string& configPath,
          const std::string& version);

/**
 * Whether a report can be sent at all: there has to be an account behind it.
 *
 * Every report is published -- to a channel, and a bug to a public tracker --
 * and every one is signed, so there has to be something to sign it with that a
 * stranger cannot invent. Checked here so the form can say so BEFORE anything
 * is typed; the service checks it again and is the one that decides.
 */
bool signedIn();

/** Where a rating belongs: public, and useful to somebody choosing the game. */
const char* ratingUrl();

Status status();

/** The last thing worth telling the player: a thank-you or a failure. */
const std::string& message();
void clearMessage();

/**
 * A stable, meaningless id for this installation.
 *
 * Random, generated once, kept in the config file. It is what lets the service
 * apply a per-install daily cap without knowing who anybody is: it identifies
 * a copy of the game, not a person, it is never derived from hardware or from
 * an account, and clearing it costs the player nothing but their quota.
 */
const std::string& installId(Config& cfg);

/** Called once a frame from the game thread. Cheap when nothing is in flight. */
void pump();

}  // namespace feedback
