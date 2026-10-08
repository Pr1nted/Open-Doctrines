#pragma once

// Steam, if it is there, and nothing at all if it is not.
//
// WHY NOTHING IS LINKED
//
// The Steamworks SDK cannot be committed to an open repository (its licence
// forbids redistributing the headers), and every non-Steam build -- itch, the
// website, the package managers, a clone -- must not depend on a library it
// does not ship. So the game links nothing: it looks for steam_api beside its
// own executable at runtime and resolves the handful of flat-API functions it
// needs by name. The Steam depot ships that library (packaging/steam); no other
// build does, so everywhere else every call here is a cheap no-op.
//
// WHAT IT IS USED FOR
//
// Achievements only, and only GRANTED ones: setAchievement() is called by
// odach::Tracker when the account service has signed a grant, never when the
// game merely saw something happen. That keeps the Steam collection exactly as
// meaningful as the in-game one -- see src/achievements/Achievements.h.

namespace odsteam {

/** Load and initialise once. False when there is no Steam (the usual case). */
bool init();

/** Whether init() found a running Steam client and an app id. */
bool active();

/** Unlock by Steamworks API name, then store. Idempotent; a no-op when inactive. */
void setAchievement(const char* apiName);

/** Pump Steam's callbacks. Cheap; called once a frame by the tracker. */
void runCallbacks();

void shutdown();

}  // namespace odsteam
