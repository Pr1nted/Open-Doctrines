#include "WinFatalDialog.h"

#include <cstdlib>

namespace {
bool g_suppressed = false;

// Nobody to click OK means no dialog. A message box blocks until dismissed, so
// raising one where there is no user turns a reported failure into a hang.
bool noOneIsWatching() {
    if (g_suppressed) return true;
    // Every CI provider sets this, GitHub Actions included. Belt and braces
    // behind the explicit call from main(), because the cost of being wrong in
    // this direction is a job that hangs rather than one that fails.
    const char* ci = std::getenv("CI");
    return ci && *ci;
}
}  // namespace

void odSuppressFatalDialogs() { g_suppressed = true; }

#ifdef _WIN32

// The full header, with no NOUSER and no NOGDI. That is the entire point of
// this file: nothing in it uses raylib, so there is no Rectangle and no
// DrawText here for winuser.h and wingdi.h to collide with. Do not add raylib
// code to this translation unit -- the collisions come back immediately, and
// they arrive as errors about types nobody edited.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

void odFatalDialog(const char* title, const char* body) {
    if (noOneIsWatching()) return;
    // Owner-less on purpose: this is called when no window could be created,
    // so there is no parent to be modal to.
    MessageBoxA(nullptr, body, title, MB_OK | MB_ICONERROR);
}

#else

void odFatalDialog(const char*, const char*) {}

#endif
