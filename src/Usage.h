#pragma once

// The one thing the game reports about how it is played, and the switch it is
// behind.
//
// ── WHY THIS IS SO SMALL ──
//
// PRIVACY.md said, for the whole life of this project, that there is no usage
// reporting anywhere in the game. This is the exception to that sentence, and
// the sentence is only worth anything if the exception stays this narrow: one
// message at the end of a session, carrying a duration as one of five ranges
// and which platform it was. No account, no installation id, no device, no
// address, nothing that links one report to another.
//
// That last part is the expensive one. Without linkage there is no way to
// count returning players, which is the number anybody actually wants. It is
// given up on purpose, because the alternative is an identifier, and an
// identifier is the thing being refused.
//
// ── AND WHY IT LIVES IN THE SHELL ON WEB ──
//
// The report is sent when a session ENDS, which on the web means the moment
// the tab is closed -- after which no wasm runs. Only the browser can be
// relied on at that point, through pagehide and sendBeacon, so the C++ side
// does not send anything: it tells the page whether it may, and the page does
// the rest. See odUsagePushConsent.

#include <string>

struct Config;

namespace odusage {

/** The five ranges, and the only durations this game will ever report. */
const char* bucketFor(double seconds);

/** Which build this is, as the service's fixed vocabulary spells it. */
const char* surface();

}  // namespace odusage

/**
 * Tell the page whether the player has agreed, and where to send it.
 *
 * Called at startup and whenever the setting changes, so switching it off
 * takes effect at once rather than after a restart. A no-op off the web.
 */
void odUsagePushConsent(const Config& cfg);
