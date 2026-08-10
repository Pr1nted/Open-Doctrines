#pragma once

#include <string>

/**
 * Makes the browser build's player data survive the tab.
 *
 * WHY THIS EXISTS
 *
 * The web build's data/ is an Emscripten MEMFS preloaded from the package. It
 * lives in the tab and nowhere else, so closing it, reloading it, or letting
 * the browser reclaim it takes the config, every save, every custom map and
 * every installed mod with it -- and nothing warns anybody, because as far as
 * the game is concerned every write succeeded. OdState.h says all this already;
 * .odstate is the manual way out, a zip the player downloads and loads back.
 *
 * This is the automatic one. IndexedDB survives a reload, so the same archive
 * OdState already knows how to write is kept there and unpacked on startup.
 *
 * WHY IT GOES THROUGH OdState RATHER THAN MOVING THE FILES
 *
 * The obvious implementation is to mount IDBFS over the directories the player
 * writes to. It is also the wrong one here. Those paths are built inline from
 * m_dataDir at about forty call sites across the game, the map editor and the
 * mod manager, and a single one missed is a file that silently stops
 * persisting. Worse, moving them changes where they sit inside a .odstate
 * archive, so every archive a player has already downloaded would stop
 * restoring what it used to.
 *
 * OdState::save() already answers "what did the player make rather than the
 * build ship", by the inverse-allowlist rule in kShipped -- anything new under
 * data/ counts as the player's unless it is named as content. Reusing it means
 * this cannot drift out of step with what the game stores, and means the
 * automatic archive and a hand-downloaded one are the same format.
 *
 * The cost is that a flush rewrites the whole archive, so flushes are marked
 * and coalesced rather than done on every write. See odPersistMark().
 *
 * Every function here is a no-op off the web.
 */

/**
 * Mount the persistent store and restore anything in it over `dataDir`.
 *
 * BLOCKS until IndexedDB has answered -- the config is read moments later, and
 * a restore that landed after that would be a restore the player does not see.
 * ASYNCIFY turns the block into a yield, as it does for the account calls and
 * the streamed music.
 *
 * Call once, before Config::load().
 */
void odPersistInit(const std::string& dataDir);

/** Note that player state changed. Cheap; the write happens in odPersistTick. */
void odPersistMark();

/**
 * Write the archive if it is due. Call once a frame; it decides for itself.
 *
 * Rate-limited because a flush serialises every save the player owns, and
 * settings screens can mark on every frame a slider moves.
 */
void odPersistTick(const std::string& dataDir);

/**
 * Write now, if anything is pending. For the moments worth not losing -- a
 * finished save, leaving a world for the menu.
 */
void odPersistFlush(const std::string& dataDir);
