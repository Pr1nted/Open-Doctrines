#pragma once

// "opendoctrines://join/<code>" — a viewer arriving from a link on a stream.
//
// A URL scheme handler is INPUT FROM OUTSIDE THE GAME: anything on the machine
// can hand the game one of these, and on macOS a web page can too. So the only
// thing it is allowed to express is "join this code" -- no paths, no files, no
// settings, no second parameter that might mean something later.
//
// Pure, so every way of getting it wrong is a test rather than a thing somebody
// has to click. See tests/join_link_test.cpp.

#include <string>

namespace joinlink {

/// The code in a join URL, or empty for anything that is not exactly one.
std::string codeFrom(const std::string& url);

}  // namespace joinlink
