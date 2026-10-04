#pragma once

// Achievements: what the player has done, what the account service has
// confirmed, and the difference between the two.
//
// ── TWO THINGS THAT LOOK LIKE ONE ──
//
// EARNED   the game saw it happen. Recorded in <data>/achievements/progress.json
//          with the counters that led to it. That file is plain JSON on the
//          player's disk, so it proves nothing, and nothing treats it as proof:
//          an earned achievement is a CLAIM, sent to the account service.
//
// GRANTED  the service signed it (net/src/achievements/claim.ts). The grant is
//          an Ed25519 statement naming the account and the achievement, kept in
//          <data>/achievements/grants.json and verified against the public keys
//          compiled into this build every time it is read. This is the only
//          state that counts: the collection, Steam, the launcher and a .odstate
//          all carry grants and nothing else.
//
// So editing progress.json produces claims, and claims still face the
// service's clock and budget; editing grants.json, or hand-building a .odstate,
// produces signatures that do not verify, and those are dropped on load.
//
// ── WHEN IT RUNS ──
//
// Only for a real person playing. AI training, simulation, screenshot tours,
// benches, the dedicated server and the tutorial walk all report nothing --
// see Game::achievementsLive(). A spectator earns only the spectator one.
//
// ── THREADS ──
//
// Counting is on the game thread and cheap. Talking to the service is on a
// worker on desktop and inline on the web, exactly as AccountClient does.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace odach {

struct Def;

/** One row of the collection as the screen draws it. */
struct Entry {
    const Def* def = nullptr;
    bool earned = false;       // the game saw it (claim made or pending)
    bool granted = false;      // the service signed it, for THIS account
    bool modded = false;       // granted under non-release rule tables
    long long when = 0;        // unix seconds: grant time, else earn time
    double progress = 0;       // current stat value, for a progress bar
};

/** Something to announce. Drawn as a toast on any screen. */
struct Toast {
    std::string id;
    bool verified = false;     // false: "earned"; true: the service confirmed it
};

class Tracker {
public:
    static Tracker& get();

    /**
     * `dataDir` ends in '/'. `build` is the version string; `seal` is od_t4 of
     * the rule tables (see odseal.h), reported so the service can mark a grant
     * earned under different rules. Safe to call once; later calls are ignored.
     */
    void init(const std::string& dataDir, const std::string& build, unsigned long long seal);
    bool initialised() const;

    // ---- counting. Each is a no-op until init(). ---------------------------

    /** A cumulative counter, kept across games: wars declared, turns ended. */
    void add(const char* stat, double n = 1);
    /** A value measured now: provinces owned, treasury. Replaces, never sums. */
    void set(const char* stat, double v);
    /** A count of distinct things: countries played, languages used. */
    void addToSet(const char* stat, const std::string& member);

    /** Check every definition against the counters; claim what is newly met. */
    void evaluate();

    /** A world began or was loaded: per-world measurements start again. */
    void resetWorld();

    // ---- the service --------------------------------------------------------

    /** Once a frame. Starts sync work when there is some and none is running. */
    void update();

    /** Ask the service now, rather than at the next scheduled time. */
    void syncSoon();

    /**
     * Why the collection is not fully confirmed, as a CODE: "sign_in",
     * "no_keys", "not_issuing", "unreachable", "waiting:<n>", or empty. A code
     * rather than a sentence because translation must happen on the game
     * thread and this is written on the worker. Game_Achievements.cpp words it.
     */
    std::string syncStatus() const;

    // ---- reading ------------------------------------------------------------

    std::vector<Entry> entries() const;
    int grantedCount() const;
    int earnedCount() const;

    /** Next toast to show, if any. */
    bool popToast(Toast& out);

    /** Whether grants can be verified at all in this build. */
    static bool keysBaked();

    /**
     * Merge a grants file written by another install (a .odstate import) into
     * this one. Every grant is verified first; one that fails is dropped and
     * counted. Returns how many new verified grants were added.
     */
    int mergeGrantsFrom(const std::string& otherGrantsJson, int* rejected = nullptr);

    /** Verify one grant token. Fills account and achievement ids on success. */
    static bool verifyGrant(const std::string& token, const std::string& issuer,
                            std::string* sub, std::string* ach, bool* modded, long long* iat);

    struct Impl;
private:
    Tracker();
    std::unique_ptr<Impl> m_impl;
};

}  // namespace odach
