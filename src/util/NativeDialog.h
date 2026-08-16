// The system's own file chooser, on the three desktops.
//
// There was one of these before, private to Game_Browsers.cpp and written in
// osascript -- so "Import .odmap" opened a picker on macOS and did nothing at
// all on Windows and Linux. This is that function, generalised: PowerShell on
// Windows, zenity or kdialog on Linux, osascript on macOS.
//
// Every one of these shells out to a program that is part of the desktop the
// player is already running. Nothing is bundled, nothing is installed, and a
// missing helper is not an error -- it returns empty, exactly as a player who
// pressed Cancel would, and the caller carries on.
//
// Returns "" on cancel, on failure, and on the web, where the browser's own
// download and upload are used instead.
#ifndef OD_UTIL_NATIVE_DIALOG_H
#define OD_UTIL_NATIVE_DIALOG_H

#include <string>

namespace NativeDialog {

// Is there a system file chooser here at all?
//
// False on the web and on Android, which have no such thing: a browser will
// not hand a page a path, and an Android app has no desktop to ask. Callers
// must check rather than treating "" as a cancelled dialog, because on those
// two the dialog was never offered and the interface should not have shown a
// button that leads to one.
bool available();

enum class Kind { File, Folder };

// Is there actually a chooser program here to run?
//
// Different from available(). A Linux desktop HAS the concept of a file
// chooser and may still have neither zenity nor kdialog installed, and then
// every dialog returns "" -- which callers read as "the player pressed
// Cancel", so the button does nothing at all and says nothing about why.
// Check this to tell them instead.
bool helperInstalled();

// The exact command openFile/openFolder would run, built but not run.
//
// Exposed because a modal dialog is the one part of this that CI cannot click
// -- and the part that actually breaks is not the clicking. It is a quote in
// the wrong place, a flag the installed helper does not have, a title that
// ends the string it was pasted into. Those live here, and a test can read
// them on a machine that cannot open a window.
//
// Empty when there is no helper to run: the web, Android, and a Linux desktop
// with neither zenity nor kdialog installed.
std::string commandFor(Kind kind, const std::string& title, const std::string& extension);

// A title with the shell's punctuation taken out of it. Exposed for the same
// reason as commandFor: this is the function standing between a caller's
// string and a shell, and it should be tested as one.
std::string safeTitle(const std::string& title);

// Pick one existing file. `extension` is without the dot ("odmap"); empty
// offers everything.
std::string openFile(const std::string& title, const std::string& extension);

// Pick one existing directory.
std::string openFolder(const std::string& title);

}  // namespace NativeDialog

#endif
