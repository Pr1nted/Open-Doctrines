#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * Core.Protected: the three facts a mod can learn about the machine.
 *
 * ── WHY IT IS FENCED OFF ──
 *
 * Every other reading a mod can take is about the GAME. The WASI shim goes out
 * of its way to keep it that way -- the clock reports the turn number, the
 * entropy source is seeded from the mod's own id, there is no filesystem and
 * no subprocess -- because those are the things that fingerprint a player
 * rather than describe a world.
 *
 * Resident memory, executable size and the installed mod list are the
 * exceptions worth making: a profiler, a build reporter and a compatibility
 * checker cannot be written without them. So they exist, behind their own
 * capability, which a player can refuse and the mod menu warns about.
 *
 * ── THE LOG IS UNCONDITIONAL, AND THAT IS THE WHOLE DESIGN ──
 *
 * Every call is recorded. Not "when the debug console is open" -- ALWAYS.
 *
 * If recording depended on whether anyone was looking, a mod could find out
 * whether it was being watched and behave differently while it was. That is
 * the oldest trick there is and the reason emissions software passes tests it
 * fails on the road. So there is no branch to detect: the counters move on
 * every call whatever the player has open, and opening the console reads
 * numbers that were already there.
 *
 * A mod cannot time the difference either. It has no clock: clock_time_get
 * returns the turn number, so a few nanoseconds of bookkeeping is not
 * measurable from inside the sandbox even in principle.
 */
namespace odprotected {

/** What one mod has asked for, since it was loaded. */
struct Usage {
    std::string modId;
    uint64_t processBytes = 0;   ///< calls to process_bytes
    uint64_t imageBytes = 0;
    uint64_t modCount = 0;
    uint64_t modId_ = 0;         ///< calls to mod_id
    uint64_t modName = 0;
    uint64_t total() const {
        return processBytes + imageBytes + modCount + modId_ + modName;
    }
};

/** Which call is being recorded. */
enum class Call { ProcessBytes, ImageBytes, ModCount, ModId, ModName };

/**
 * Record one call. Called from the host function itself, before it answers.
 *
 * Cheap and unconditional -- see the note above. A mod that calls this ten
 * thousand times a turn shows up as a mod that calls it ten thousand times a
 * turn, which is exactly what the console exists to reveal.
 */
void record(const std::string& modId, Call which);

/** What every mod has asked for, for the debug console. Sorted by id. */
std::vector<Usage> usage();

/** Forget everything. Called when mods are reloaded, not when the console opens. */
void reset();

/** Resident memory of the whole process in bytes, or 0 if unknown. */
uint64_t processBytes();

/** Size of the running executable in bytes, or 0 if unknown. */
uint64_t imageBytes();

}  // namespace odprotected
