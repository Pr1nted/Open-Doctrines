// Greater Diplomacy translation layer.
//
// Converts a world between this game's .odmap and Greater Diplomacy 5's map
// directories, through open-dragoman -- a separate MIT library with no code
// from either game in it. Everything that talks to that library is behind this
// header, so the rest of the game never includes it and compiles the same way
// whether or not the build has it.
//
// WITHOUT -DOD_ENABLE_GDTL=ON every function here is a stub: available()
// returns false, the conversions fail with a message saying so, and the
// interface hides the buttons. That is why the file is in GAME_SOURCES
// unconditionally -- the call sites stay ordinary code.
//
// Nothing here runs unless the player has turned GDTL on in the Experimental
// tab AND confirmed the warning in front of the button they pressed. The
// conversion is lossy in ways that depend on the map, and it writes into a
// second game's data directory when asked to, so it asks first.
#ifndef OD_GAME_GDTL_H
#define OD_GAME_GDTL_H

#include <string>
#include <vector>

namespace Gdtl {

// Was this build made with the translation layer in it?
bool available();

// Version of the library doing the work, for the warning dialog. "unavailable"
// in a build without it.
std::string version();

// What a conversion had to say for itself. `ok` is whether a map was written;
// the notes are open-dragoman's own diagnostics, worst first, already made into
// sentences. A conversion can succeed with notes -- that is the normal case,
// because the two games do not hold the same set of facts.
struct Result {
    bool ok = false;
    std::string error;                 // set only when ok is false
    std::vector<std::string> notes;    // warnings and remarks, may be non-empty on success
    std::string outputPath;            // what was written
};

// .odmap -> a Greater Diplomacy 5 map directory.
Result toGd5(const std::string& odmapPath, const std::string& outDir);

// A Greater Diplomacy 5 map directory -> .odmap.
Result toOdmap(const std::string& gd5Dir, const std::string& odmapPath);

// Can the player choose where the map goes?
//
// False on the web and on Android, where there is no file chooser and no
// second game to install into. There the destination is decided for them --
// the browser is handed a download, Android gets a file in the game's own
// storage -- and the interface skips the step rather than showing a dialog
// whose every button does nothing.
bool canChooseLocation();

// Where a map goes on a platform that cannot ask. `dataDir` is the game's own
// data directory; the file is a zip, because a GD5 map is a folder and neither
// of these platforms gives a player one to open.
std::string unattendedDestination(const std::string& dataDir, const std::string& mapName);

// ---------------------------------------------------------------- finding GD5
//
// The game does not look for other software on this computer on its own. These
// run only when a player presses "Search", and they are deliberately cheap and
// shallow: a fixed list of the places that game is normally installed, each
// checked one level deep. No recursive walk of the disk, no elevated
// permissions, nothing outside the user's own directories and the standard
// install roots.

// Does this directory look like a Greater Diplomacy 5 installation?
bool looksLikeGd5(const std::string& dir);

// The candidate paths, in the order they are tried. Exposed so the dialog can
// tell the player exactly where it is about to look before it looks.
std::vector<std::string> searchLocations();

// Search those locations. Returns every installation found, which may be empty
// and is not an error -- the player then picks a folder by hand, or does not.
//
// A locator file is read first, and when it checks out nothing is searched at
// all -- see below.
std::vector<std::string> findGd5Installations();

// ------------------------------------------------------------------ locators
//
// A game writing down where it is, so the other one does not have to guess.
// One small JSON file per game in a shared per-user directory; the format is
// specified in open-dragoman's docs/locator.md and both games implement it.
//
// This is a shortcut, never a requirement. A player who has never launched the
// other game has no locator to read, and the search above still exists for
// exactly that case.

// Say where THIS game is. Called once at startup, best effort: a read-only or
// missing directory is not worth interrupting anyone over.
void writeLocator(const std::string& gameRoot);

// Where Greater Diplomacy 5 says it is, or empty.
//
// A locator is a CLAIM, not a fact. A game that was moved corrects its file on
// its next launch and one that was deleted never does, so a file pointing at
// nothing is the normal state rather than an edge case. The path is checked
// with looksLikeGd5() before it is returned, and a file that fails is ignored
// and left alone -- it is not ours to delete.
std::string gd5FromLocator();

// Where a translated map belongs inside an installation, ready to be listed by
// that game. Empty if the path is not one.
std::string mapDestination(const std::string& gd5Dir, const std::string& mapName);

// ------------------------------------------------------------------ packaging

// Zip a directory into a single file, so the web build has something to hand
// the browser -- a GD5 map is a folder, and a download is one file.
bool zipDirectory(const std::string& dir, const std::string& zipPath, std::string& error);

// Hand a file to the browser as a download. Desktop builds do nothing and
// return false: there is a real filesystem there and the caller writes to it.
bool offerBrowserDownload(const std::string& path, const std::string& suggestedName);

}  // namespace Gdtl

#endif
