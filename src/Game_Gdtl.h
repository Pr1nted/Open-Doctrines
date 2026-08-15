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
std::vector<std::string> findGd5Installations();

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
