#pragma once

// The PlayStation Vita, where the game's own folder is read-only.
//
// WHY THE DATA IS COPIED
//
// Everywhere else data/ sits beside the program and is written to as well as
// read: config.json, saves, the account file and the mod folder all live in
// it. A Vita app is installed from its .vpk and mounted at app0:, which no
// app may write to. So on the first start of each version the shipped data is
// copied to ux0:data/OpenDoctrines/data/ -- the memory card, where homebrew
// keeps its files -- and the game is pointed there with OD_DATA_DIR, the same
// override a second copy of the game on one desktop uses. Files the player
// made (config, saves) are not in the shipped set, so a new version's copy
// leaves them alone; shipped files are replaced.
//
// The whole file is a no-op on every other platform.

#include <string>

/// Copy app0:data/ to the memory card if this version has not been copied
/// yet, and point OD_DATA_DIR at the copy. Returns the data directory, or ""
/// if the copy failed (the game then starts and says it has no data, rather
/// than writing into a folder it cannot write to).
std::string odVitaPrepareData();
