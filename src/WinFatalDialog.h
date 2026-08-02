#pragma once

// A message box for failures that happen before there is a window to report
// them in -- from a translation unit that is actually allowed to declare one.
//
// WHY THIS IS NOT JUST MessageBoxA() AT THE CALL SITE
//
// src/Game.cpp includes <windows.h> with NOGDI and NOUSER defined, and both are
// load-bearing: wingdi.h declares a function named Rectangle that collides with
// raylib's Rectangle struct, and winuser.h defines DrawText as a macro
// expanding to DrawTextA, which quietly turns every raylib DrawText() call in
// the file into a GDI one. The comment there records what happened when this
// was got wrong.
//
// NOUSER also removes MessageBoxA, MB_OK and MB_ICONERROR -- so the one file
// that most needs to report a fatal startup failure is the one file that cannot
// call it. Attempting it there fails with:
//
//   error C3861: 'MessageBoxA': identifier not found
//   error C2065: 'MB_OK': undeclared identifier
//
// This file exists to hold that call somewhere the real header can be included
// whole, because nothing here touches raylib and so nothing here can collide.
//
// A no-op everywhere but Windows, so callers need no #ifdef of their own.
//
// It is ALSO a no-op whenever nobody can click OK. A message box blocks the
// thread that raised it until it is dismissed, so on a machine with no one at
// the keyboard it does not report a failure -- it becomes one. A CI job hung
// for the full thirty-minute timeout on exactly this, and a player's batch
// script would hang the same way with no window to show for it.
//
// Silenced by odSuppressFatalDialogs(), which main() calls for every headless
// mode, and automatically when CI is set in the environment.
void odFatalDialog(const char* title, const char* body);

// Call before anything that might fail, for any run with no interactive user.
// The message still goes to stderr; only the blocking dialog is dropped.
void odSuppressFatalDialogs();
