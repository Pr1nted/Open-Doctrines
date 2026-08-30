#pragma once

#include <string>

/**
 * Make `path` readable, downloading it first if this is the web build and it is
 * not there yet. Returns true when the file exists afterwards.
 *
 * WHY THIS EXISTS: everything --preload-file packs is downloaded IN FULL before
 * the menu draws. That is the right trade for the few megabytes the menu itself
 * needs and the wrong one for the tens of megabytes it does not -- the scenario
 * archives and the trained AI model are the bulk of the package and neither is
 * touched until a player has picked a country. Preloading them meant the first
 * thing a browser visitor did was wait on a game they had not yet chosen to
 * play.
 *
 * So those files are excluded from the preload, deployed next to the page under
 * the same relative path they have in the virtual filesystem, and fetched
 * through here at the moment something asks for them.
 *
 * BLOCKING, deliberately. Every caller is a loader that has already put a
 * loading screen up and opens the file on its next line, so the alternative --
 * an async fetch and a callback -- would mean restructuring those loaders for
 * no gain the player can see. ASYNCIFY (see the link options in CMakeLists.txt)
 * turns the block into a yield: the browser keeps running, and the game resumes
 * where it left off when the bytes arrive. Off the web this is a FileExists and
 * nothing else.
 */
bool odEnsureAsset(const std::string& path);

/**
 * Forget that `path` could not be downloaded, so the next odEnsureAsset() for
 * it asks the network again instead of answering from the failure cache.
 *
 * That cache exists for callers that ask every frame -- the flag renderer will
 * retry a country whose art will never arrive until the tab is closed -- and it
 * is exactly wrong for a scenario. A world is fetched because a player pressed
 * something; if the connection dropped, the player presses it again, and the
 * honest answer to the second press is a second request rather than the cached
 * "no" from the first. So the loader clears its own entry when it reports the
 * failure. Off the web this does nothing.
 */
void odForgetAsset(const std::string& path);
