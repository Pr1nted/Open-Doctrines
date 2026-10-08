#pragma once

// When a turn is due, in WALL-CLOCK time.
//
// WHY THIS IS NOT TurnRunner'S CLOCK
//
// TurnRunner counts milliseconds from whatever monotonic clock the caller
// hands it, which is right for a game that lives as long as one process: a
// monotonic clock cannot be dragged backwards by NTP or a user changing the
// time zone. It is wrong for a campaign that outlives the process. A server
// restarted halfway through a 24-hour turn has a new monotonic origin, so the
// deadline it held is meaningless, and before this file existed the turn simply
// started again from zero -- every restart handed every player another day.
//
// So the DEADLINE is stored as Unix epoch milliseconds, beside the save, and
// the monotonic clock is only used to count down to it within one run. A
// restart reads the epoch deadline back and asks how far away it is.
//
// THE SCHEDULE
//
// Two shapes, both common in long tournaments:
//
//   interval   every turn lasts `turnSeconds` from when it opened.
//   anchored   turns resolve on a fixed grid -- "every day at 18:00 UTC" --
//              so players can plan around a time rather than around whenever
//              the previous turn happened to end.
//
// An anchored turn is never shorter than half an interval. A turn that
// resolved early because everyone was in at 17:55 would otherwise be followed
// by a five-minute turn, which nobody would get to play.
//
// Pure arithmetic: no clock is read except by nowEpochMs(), so all of it is
// testable with fixed numbers (tests/turn_clock_test.cpp).

#include <cstdint>
#include <string>

namespace turnclock {

/** Unix epoch milliseconds, from the system clock. */
int64_t nowEpochMs();

/**
 * "HH:MM" (UTC) as minutes after midnight, or -1 for empty or malformed text.
 * Accepts "18:00", "6:05" and "0:00"; refuses "24:00", "18", "18:60".
 */
int parseAnchor(const std::string& text);

/** Minutes after midnight back to "HH:MM". Empty for a negative value. */
std::string formatAnchor(int minutes);

/**
 * When a turn opening at `nowMs` should resolve.
 *
 * `turnSeconds` 0 means no deadline at all (long-form), and returns 0.
 * `anchorMinutes` < 0 means a plain interval; otherwise deadlines lie on the
 * grid `midnight UTC + anchorMinutes + k * turnSeconds`, and the first grid
 * point at least half an interval away is chosen.
 */
int64_t nextDeadline(int64_t nowMs, uint32_t turnSeconds, int anchorMinutes);

/**
 * Milliseconds left on a deadline read back after a restart.
 *
 * A deadline still in the future keeps exactly its remaining time: the restart
 * gives nobody extra. One that passed while the server was down is NOT
 * resolved on the spot -- players reconnecting and orders arriving need a
 * moment to land -- so it gets `graceMs`, and resolves once. Only one: three
 * missed days resolve one turn, not three turns nobody was present for.
 *
 * `savedMs` 0 means "no deadline was stored", and returns -1 so the caller
 * can tell that apart from "due now".
 */
int64_t resumeRemainingMs(int64_t savedMs, int64_t nowMs, int64_t graceMs);

/**
 * Clamp for the wire, which carries a turn's remaining time as uint32
 * milliseconds -- 49.7 days. The longest turn a server accepts is 30.
 */
uint32_t clampForWire(int64_t ms);

/** "3h 20m", "45s", "2d 4h" -- for logs and the server console. */
std::string describe(int64_t ms);

}  // namespace turnclock
