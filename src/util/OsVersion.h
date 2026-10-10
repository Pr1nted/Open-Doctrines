#pragma once
#include <string>

namespace od {

// The OS and its version, one line, for bug reports: "macOS 26.0 arm64",
// "Windows 10.0 build 26100 x64", "Android 15 (API 35), Google Pixel 8", or the
// browser's user agent on web. Nothing personal -- no hostname, no account.
//
// Lives in its own translation unit with no raylib.h, because on Windows it
// needs <windows.h>, and the two headers collide (Rectangle, DrawText,
// CloseWindow, LoadImage...).
std::string osVersion();

}  // namespace od
