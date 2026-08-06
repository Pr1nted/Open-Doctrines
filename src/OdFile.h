#pragma once
#include <string>

/**
 * Whole-file reads and writes that work on Android as well as everywhere else.
 *
 * WHY THIS EXISTS. On Android the game's data lives inside the APK and is
 * reachable only through AAssetManager; a path handed to open(2) finds nothing.
 * raylib solves this for its own code by redirecting fopen to android_fopen in
 * its internal utils.h -- reads come from the asset manager, and writes (mode
 * "w") are redirected to the app's internal storage, which is the only place an
 * Android app may write at all. That macro is private to raylib, so the game's
 * own fopen calls never saw it, and std::ifstream would not have been covered
 * even if they had.
 *
 * So everything funnels through raylib's PUBLIC file API instead, which does go
 * down that path: LoadFileData for reads, SaveFileData for writes. On desktop
 * and web both are ordinary stdio, so this changes nothing there.
 *
 * Read-entire-file is the only shape the game actually uses -- .odmap archives
 * are slurped and then parsed from memory by miniz, and the loose JSON files
 * are parsed from a string -- so a streaming interface would be unused weight.
 */
namespace odFile {

/** Whole file as bytes. Empty string if it does not exist or cannot be read. */
std::string readAll(const std::string& path);

/** True if the file exists and can be opened for reading. */
bool exists(const std::string& path);

/**
 * Write a whole file, creating or truncating.
 *
 * On Android this lands in the app's internal storage regardless of the path
 * given, because that is the only writable location -- so callers must not
 * assume the file appears where they asked, only that reading the same name
 * back returns what they wrote.
 */
bool writeAll(const std::string& path, const std::string& data);

/**
 * Where the game may read AND write, with a trailing separator.
 *
 * On Android this is the app's internal storage, which is the only writable
 * location and the place first-run extraction puts everything. Elsewhere it is
 * the directory passed in, unchanged.
 */
std::string writableRoot(const std::string& desktopDataDir);

/**
 * Copy everything the APK ships into internal storage, once.
 *
 * AN APK'S ASSETS CANNOT BE LISTED. AAssetManager opens a name it is given; it
 * cannot answer "what is in here", and raylib's LoadDirectoryFiles does not
 * work on assets either. The save browser, the map browser and the mod list all
 * walk directories, so rather than teach each of them an asset-shaped API, the
 * game copies its content out on first run and then works against an ordinary
 * filesystem. That also settles writes, since internal storage is the only
 * place an Android app may write.
 *
 * The list comes from assets_manifest.txt, generated at configure time from the
 * same OD_SHIPPED_DATA allowlist the web preload and the installers use.
 *
 * A stamp file records which build did the extraction, so an upgrade re-copies
 * rather than leaving the player on last version's maps. No-op off Android.
 * Returns the number of files written.
 */
int extractAssetsOnce(const std::string& stamp);

}  // namespace odFile
