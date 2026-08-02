#include "WinFatalDialog.h"

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
    // Owner-less on purpose: this is called when no window could be created,
    // so there is no parent to be modal to.
    MessageBoxA(nullptr, body, title, MB_OK | MB_ICONERROR);
}

#else

void odFatalDialog(const char*, const char*) {}

#endif
