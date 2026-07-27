#pragma once

// The in-game updater: "is there a newer OpenDoctrines, and can I have it?"
//
// This is a DIFFERENT question from the mod update check in mods/ModUpdates.h,
// and the difference is the reason the two behave differently. A mod check
// talks to a server the MOD AUTHOR runs, so it discloses to a stranger which
// mods a player has installed -- that is opt-in and off by default. This one
// talks to the game's own release host about the game itself. It is the same
// conversation every launcher and browser has, so it is on by default, with a
// switch in Settings > Advanced for players who would rather it did not.
//
// WHAT IS AND IS NOT TRUSTED
//
// The only host ever contacted is api.github.com, over https, hardcoded below
// and checked again at download time. A release asset is only fetched from
// that host. TLS to GitHub is therefore the trust anchor -- there is no code
// signature to check, because the project deliberately ships without a paid
// Developer ID. The SHA-256 check exists to catch a truncated or corrupted
// download, not a compromised GitHub, and is described that way rather than
// being dressed up as more than it is.
//
// WHY macOS IS DIFFERENT
//
// Without a Developer ID the shipped app is not notarized. Players get past
// Gatekeeper once, by hand, for the copy they downloaded. If the game silently
// swapped its own binary, the replacement would carry no such approval and
// macOS would refuse to launch it -- the update would break the install. So on
// macOS the button opens the release page and the player installs the new copy
// the same way they installed the first one. Windows and Linux have no
// equivalent gate, so there the update installs itself.
//
// WHAT AN INSTALL MAY TOUCH
//
// Only files the new release actually contains. The installer copies the
// staged tree over the install and DELETES NOTHING, so a save, a custom map,
// a mod, or a config file cannot be removed by an update even if some future
// packaging bug ships a tree that omits them. The cost is that a file dropped
// between versions lingers; that is the right way round.

#include <atomic>
#include <string>
#include <vector>

// MAJOR.MINOR.PATCH<state>[counter] -- the scheme tools/odver.py defines.
// Kept in step with odver.Version.sort_key() by tests/game_updates_test.cpp,
// which compares this against the Python for a shared table of versions.
struct GameVersion {
    int  major = 0, minor = 0, patch = 0;
    char state = 'a';     // a alpha, b beta, r release, s snapshot
    int  counter = 0;     // only meaningful for snapshots

    // Strict: "1.0.2", "1.0.2x" and "v1.0.2a" are all rejected, matching the
    // regex in odver.py. A version we cannot parse is never "older", so a
    // malformed reply from the server cannot invent an update.
    static bool parse(const std::string& text, GameVersion& out);

    // <0, 0, >0. Ordering is odver's: within one MAJOR.MINOR.PATCH,
    // snapshot < alpha < beta < release, because a snapshot is work toward
    // that version rather than past it.
    static int compare(const GameVersion& a, const GameVersion& b);

    std::string str() const;
};

class GameUpdates {
public:
    static GameUpdates& get();

    enum class Stage {
        Idle,          // nothing has been asked
        Checking,
        UpToDate,
        Available,     // a newer release exists
        Downloading,
        Installing,
        Restart,       // installed; takes effect on next launch
        OpenedPage,    // macOS: handed off to the browser
        Failed,
    };

    struct Status {
        Stage       stage = Stage::Idle;
        std::string latest;       // version of the newest release
        std::string pageUrl;      // where a human should be sent
        std::string assetUrl;     // the archive for THIS platform, if any
        long long   assetSize = 0;// what the API says it weighs, for progress
        std::string sha256;       // as reported by the API, may be empty
        std::string notes;        // first lines of the release body
        std::string error;        // why it failed, in a player's words
        int         percent = 0;  // download progress, 0-100
    };

    // Starts a check in the background. Does nothing at all when `enabled` is
    // false, and nothing when a check has already run this session.
    void check(bool enabled);

    // Acts on whatever check() found: downloads and installs on Windows and
    // Linux, opens the release page on macOS. Safe to call when there is no
    // update -- it simply does nothing.
    void beginUpdate();

    Status status() const;
    bool   updateAvailable() const;   // drives the "!" in the main menu
    bool   busy() const;

    // True where the game may replace its own files. False on macOS, for the
    // notarization reason at the top of this file.
    static bool canSelfInstall();

    // Where the game is installed -- the directory holding the executable, not
    // the process's working directory. A double-clicked app is often launched
    // with the working directory set somewhere else entirely, and staging an
    // update there would unpack the game into a stranger's folder. Game::init
    // sets this from the platform's own answer; without it the two fall back
    // to the working directory, which is right only when running from a build
    // tree.
    static void setInstallDir(const std::string& dir);
    static std::string installDir();

    // Deletes the staging directory and the displaced old binary left by a
    // previous update. Called once at startup, when nothing is using them.
    static void cleanUpAfterUpdate();

    // ---- public for testing ----------------------------------------------

    // Pulls tag_name / html_url / body and this platform's asset out of a
    // GitHub releases reply without trusting any of it. False if the reply is
    // not usable, in which case nothing is reported as available.
    static bool parseRelease(const std::string& body, const std::string& platform,
                             Status& out);

    // The asset basename this build wants: "OpenDoctrines-linux-x64" etc.
    static std::string platformKey();

    // Only ever https on the release host. Every URL taken from the reply
    // passes through here before it is used.
    static bool isReleaseHostUrl(const std::string& url);

    static std::string sha256Hex(const std::string& bytes);

    // Copies `staged` over `install`, overwriting what the release contains
    // and removing nothing. Returns false and changes nothing further on the
    // first failure. `skipName` is the running binary, handled separately.
    static bool installOver(const std::string& staged, const std::string& install,
                            const std::string& skipName, std::string& error);

private:
    mutable std::atomic<int> m_lock{0};
    Status m_status;
    std::atomic<bool> m_asked{false};

    void setStatus(const Status& s);
    void runUpdate();
};
