#pragma once
#include <string>
#include <vector>

// A player's whole mutable state, in one file.
//
// WHY THIS EXISTS, AND WHY IT IS WEB-SHAPED
//
// On desktop the game writes into data/ and the files are still there tomorrow.
// In a browser they are not: the web build's data/ is an Emscripten MEMFS
// preloaded from the package, which lives in the tab. Closing it, reloading it,
// or letting the browser reclaim it takes every save, every custom map and the
// config with it, and nothing warns the player because as far as the game is
// concerned the writes succeeded.
//
// .odstate is the way out of that tab: one zip the player downloads and later
// loads back. It is not a save format -- a .odsv is one world -- it is
// everything data/ holds that the player made rather than the build shipped.
//
// WHAT IS IN IT
//
// Everything under the data directory EXCEPT the shipped content (see kShipped
// in the .cpp). That is deliberately the inverse of an allowlist: the risk here
// is forgetting to include something the player would miss, so anything new
// under data/ is captured by default and only the known-static content is
// skipped. In practice it holds config.json, saves/ (multiplayer saves included,
// they live in saves/multiplayer/), custom_maps/, the loose map files a loaded
// world unpacks, projects/ from the map editor, and installed mods.
namespace OdState {

// Writes `dataDir`'s player state to `outPath` as a zip. False on failure with
// a reason in `err`. `outCount` receives the number of files stored.
bool save(const std::string& dataDir, const std::string& outPath,
          std::string& err, int* outCount = nullptr);

// Unpacks an archive written by save() back over `dataDir`, creating parent
// directories as needed and overwriting what is already there. Entries with an
// absolute path or a ".." component are refused outright -- the archive comes
// from a file the player chose, so it is untrusted input and must not be able
// to write outside dataDir.
bool load(const std::string& dataDir, const std::string& archivePath,
          std::string& err, int* outCount = nullptr);

// A name to offer the player, e.g. "OpenDoctrines-20260730-1542.odstate".
std::string suggestedFilename();

// Where a desktop player's archives live: <dataDir>/exports/. Created on demand.
std::string defaultSaveDir(const std::string& dataDir);

// Archives already sitting in defaultSaveDir(), newest first, so a desktop
// player can pick one without the game needing a native file dialog it has no
// way to open.
std::vector<std::string> findArchives(const std::string& dataDir);

// How many installed-mod files an archive carries, WITHOUT unpacking it.
// Restoring mods puts executable content back into the game, so the player is
// asked first -- and asking after unpacking would be asking too late.
// Returns -1 if the archive cannot be read at all.
int countMods(const std::string& archivePath);

#ifdef __EMSCRIPTEN__
// Hands a file already written into MEMFS to the browser as a download.
void webDownload(const std::string& fsPath, const std::string& filename);

// Opens the browser's file picker. Returns immediately -- the file arrives
// later, so poll webTakeImport() for it.
void webPickFile();

// True once the player has chosen a file and the browser has written it into
// MEMFS; `outPath` receives where. Each arrival is reported once.
bool webTakeImport(std::string& outPath);
#endif

}  // namespace OdState
