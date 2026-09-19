#pragma once

#include <string>
#include <vector>

/**
 * What a world was last loaded with: the build, and the mods.
 *
 * ── WHY A WORLD HAS TO CARRY THIS ──
 *
 * A mod can add content -- a doctrine, a research node, a troop type -- and
 * that content ends up referenced by id from inside the save. Load the same
 * world without the mod and those references point at nothing. Today the game
 * cannot even tell that happened: nothing in a save records which mods were
 * running when it was written, so "your world is missing a mod" is a sentence
 * it has no way to say.
 *
 * This is the record that makes it sayable, and it is the foundation the
 * `.add.persist` / `.add.hollow` distinction rests on -- persisted content
 * needs to know which mod owned it, and hollow content needs to know which
 * mod should have re-declared it.
 *
 * ── WHAT IT DELIBERATELY DOES NOT CONTAIN ──
 *
 * NO TIMESTAMP. Not "when was this saved" in any form. A wall-clock time in a
 * save is a fingerprint of the machine that wrote it, which is the same reason
 * the WASI shim reports the turn number instead of the clock and reports
 * deterministic bytes instead of OS entropy (see the WasiStub table in
 * docs/gearbox-languages.md). It is also not needed: the question this answers
 * is "what was running", not "when".
 *
 * Nothing about the player, the machine, the install path or the session
 * either. Game facts only.
 *
 * ── IT IS OVERWRITTEN, NOT APPENDED ──
 *
 * Each load rewrites it to what is running NOW. A world carries its current
 * provenance, not its history: a list that grew every time somebody opened a
 * save would become a record of everything a player ever had installed, which
 * is both useless and a privacy leak by accumulation.
 */
namespace odprov {

/** One mod that was running. Version and hash come from the attestation. */
struct ModRecord {
    std::string id;
    std::string version;
    /**
     * Whether this mod added content the world is still relying on.
     *
     * A `.add.persist` mod's content is IN the save, so removing the mod
     * leaves dangling references and the warning must be loud. A `.add.hollow`
     * mod re-declares everything on load, so its absence costs the player only
     * what the mod itself provided -- worth mentioning, not worth alarming
     * about. The distinction is the mod's own choice; this records which it
     * made so the load can say the right thing.
     */
    bool persisted = false;
};

struct Provenance {
    std::string gameVersion;          ///< GAME_VERSION at the last load
    std::vector<ModRecord> mods;      ///< sorted by id, so two saves compare
    bool present = false;             ///< false for a save written before this existed
};

/** What a load found missing, and how much it matters. */
struct Mismatch {
    std::vector<std::string> missingPersisted;  ///< content in the save has no owner
    std::vector<std::string> missingHollow;     ///< the mod is gone; nothing dangles
    std::vector<std::string> versionChanged;    ///< "id: 1.0.0 -> 1.1.0"
    std::string savedGameVersion;               ///< empty when it matched
    bool any() const {
        return !missingPersisted.empty() || !missingHollow.empty() ||
               !versionChanged.empty() || !savedGameVersion.empty();
    }
    /** Whether anything in the save is now pointing at nothing. */
    bool dangling() const { return !missingPersisted.empty(); }
};

/**
 * Compare what a world was saved with against what is running now.
 *
 * `running` is the live set. Both are matched by id; a mod present in both at
 * a different version is reported separately from one that is absent, because
 * an upgrade is ordinary and an absence is not.
 */
Mismatch compare(const Provenance& saved,
                 const std::vector<ModRecord>& running,
                 const std::string& runningGameVersion);

/** One sentence per problem, in the order a player should read them. */
std::vector<std::string> describe(const Mismatch& m);

}  // namespace odprov
