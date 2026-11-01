#pragma once

#include <string>

// ── A SECOND CHANCE AT A WINDOW, IN SOFTWARE ──
//
// When a machine has a display but its GPU driver cannot give OpenGL 3.3 -- a
// VM without 3D acceleration, a forwarded X session, an old or broken driver --
// raylib logs a warning and then faults inside InitWindow(). The trace-log
// hooks in Game.cpp catch that warning and, rather than let the fault bury it,
// exit with an explanation.
//
// That explanation is the end of the road for a player on those machines, and
// it does not have to be: Mesa ships a software rasteriser (llvmpipe) that can
// draw everything the game needs, slowly, through the SAME X11 window GLFW
// already opened. So before giving up, re-exec this very process with
// LIBGL_ALWAYS_SOFTWARE set. A fresh process picks software GL from its first
// InitWindow -- which is far more robust than tearing a half-initialised GLFW
// down and retrying in place, the thing the fault makes impossible anyway.
//
// WINDOWS and macOS are out of scope: neither ships llvmpipe, so there is
// nothing to fall back TO. Linux, the BSDs and Solaris all have Mesa.

/// Remember argv and an absolute path to this executable, before anything can
/// chdir. Call once, first thing in main().
void odRememberRelaunch(int argc, char** argv);

// This program's absolute path, as resolved by odRememberRelaunch (empty if
// nothing could be). And its directory, with a trailing slash: raylib's
// GetApplicationDirectory() where raylib knows the platform, this path's
// directory where it does not (OpenBSD, NetBSD), so the data folder is found
// beside the binary however the game was started.
const std::string& odExecutablePath();
std::string odApplicationDirectory();

/// Re-exec this process with software OpenGL forced on. Returns ONLY on
/// failure (nothing to exec, or exec refused); on success it never returns
/// because the process image is replaced. Returns false immediately -- without
/// re-exec -- if software was already tried this chain (the OD_GL_SOFTWARE_TRIED
/// guard), so a machine that cannot draw even in software fails once, not
/// forever.
bool odTrySoftwareGlRelaunch();

/// Whether this process is itself the software-GL retry. The GL-failure message
/// reads differently then: a second failure is a missing display, not a driver.
bool odSoftwareGlAlreadyTried();
