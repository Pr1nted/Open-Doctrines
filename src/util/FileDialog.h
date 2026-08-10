#pragma once
#include <string>
#include <vector>

/**
 * The system's own open/save file picker.
 *
 * WHY THIS EXISTS. Everything the map editor writes used to land in the game's
 * own data directory and nowhere else: projects in data/projects, exports in
 * data/custom_maps. A map you were handed had to be copied into that directory
 * by hand before the editor could see it, and a map you made had to be dug out
 * of it before you could send it to anyone -- on a packaged build most players
 * cannot even find that folder. So the editor could not get a file in or out
 * without the player doing filesystem surgery around it.
 *
 * There was already a picker in Game_Browsers.cpp, but it was macOS-only
 * (osascript) and open-only, which is no use to the Windows players who hit
 * this hardest. This is that call generalised: open AND save, on the three
 * desktop platforms, behind one signature.
 *
 * Each platform gets its native dialog rather than a drawn-in-game one:
 * the file picker is the one piece of UI a player already knows, it can reach
 * places the game has no business listing (Downloads, a USB stick, a synced
 * folder), and it is the OS's job to say what a sandbox permits.
 *
 * Web and Android have no such dialog and no user-visible filesystem to point
 * one at -- available() answers false there and the editor keeps its in-game
 * browser instead of offering a button that would do nothing.
 */
namespace fileDialog {

/** False where the platform has no native picker (web, Android). */
bool available();

/**
 * Pick an existing file. Empty if cancelled or unavailable.
 *
 * `extensions` are bare, without the dot ({"odmap", "uodmap"}); an empty list
 * means any file.
 */
std::string open(const std::string& title, const std::vector<std::string>& extensions);

/**
 * Pick a destination path. Empty if cancelled or unavailable.
 *
 * `defaultName` is the filename to start with, `extension` the bare extension
 * appended when the player does not type one.
 */
std::string save(const std::string& title, const std::string& defaultName,
                 const std::string& extension);

}  // namespace fileDialog
