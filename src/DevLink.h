#pragma once
#include <string>

#include "raylib.h"

/**
 * The game's half of the live modding loop.
 *
 * WHAT THIS IS FOR. A modder edits, rebuilds, and then has to alt-tab into the
 * game and press "Reload modloader" before they can see anything. That last
 * step is the whole distance between modding and a REPL, and it is the step a
 * tool can take for them -- but only with the game's cooperation, because
 * nothing outside this process can make it reload, nothing outside it can see
 * what it is drawing, and nothing outside it can make it respond.
 *
 * So this offers three things, all off unless asked for:
 *
 *   a WATCH   the mods directory is polled, and a changed .odmod means the
 *             modloader reloads by itself.
 *   a VIEW    a small copy of the frame is published where another process can
 *             read it, so an editor can show the game inside its own window.
 *   INPUT     that editor can send the pointer and keyboard back, so the view
 *             is something you can play rather than only watch.
 *
 * OFF BY DEFAULT, AND ONLY FOR DEVELOPMENT. All three are enabled by an
 * environment variable, because a player has no use for any of them and should
 * not pay for them: the view costs a framebuffer readback, and the input shim
 * costs a branch on every key the game reads.
 *
 *   OD_DEV_WATCH=1                reload when a mod file changes
 *   OD_DEV_VIEW=/path/to/frame    publish frames, and accept input, there
 *
 * WHY ONE MAPPED FILE AND NOT A SOCKET. A file needs no handshake, no port, no
 * ordering between the two processes, and survives either of them restarting.
 * Mapped rather than read and written, because a megabyte through the kernel
 * thirty times a second is the difference between a view you can play in and
 * one you can only watch. Both directions live in the same mapping: the frame
 * going out, the pointer and keys coming back.
 */
namespace devlink {

/// True when OD_DEV_WATCH asked for the mods directory to be watched.
bool watchEnabled();

/// True when a `.odmod` in `modsDir` has appeared, vanished or changed since
/// the last call. Polled, not notified: a poll is twenty lines and works the
/// same on every platform, and the thing being watched is a directory with a
/// handful of files in it.
///
/// Rate-limited internally, so calling it every frame costs nothing.
bool modsChanged(const std::string& modsDir);

/// True when OD_DEV_VIEW asked for frames to be published.
bool viewEnabled();

/**
 * Publishes the frame currently in the back buffer, downscaled.
 *
 * MUST BE CALLED BEFORE EndDrawing, because that is when the back buffer still
 * holds this frame -- and after rlDrawRenderBatchActive, which it does itself,
 * because raylib has not sent this frame's triangles anywhere until then.
 */
void publishFrame();

/**
 * True when the editor asked this window to come forward.
 *
 * Still offered even though the view now takes input: a view is a good place
 * to watch and a poor place to read a tooltip, and sometimes you just want the
 * real window.
 */
bool focusRequested();

}  // namespace devlink

/**
 * The pointer and keyboard, arriving from the editor.
 *
 * WHY A SHIM AND NOT AN EVENT QUEUE. The game reads input through some three
 * hundred separate raylib calls spread across forty files -- `IsKeyPressed`
 * here, `GetMousePosition` there -- so there is no single place an injected
 * event could be put where all of them would see it. Rewriting those call
 * sites would be a worse change than this one.
 *
 * So the calls themselves are redirected, by macro, to functions that answer
 * with the editor's input when the editor is driving and with raylib's own
 * otherwise. The macros are in DevInput.h and are applied to the whole target
 * by a forced include, so no source file has to know.
 *
 * WHAT IS OR-ED AND WHAT IS REPLACED. Keys and buttons are OR-ed: the game's
 * own window keeps working while the editor drives, which matters because the
 * two are often side by side. The pointer POSITION is replaced while the
 * editor is driving, because two cursors cannot both be where the mouse is.
 */
namespace devinput {

/// Reads whatever the editor left in the mapping. Once a frame, before
/// anything asks a question.
void poll();

/// True when the editor is driving: the pointer is over its view and it is
/// sending. False the moment it stops, and the game's own input is all there
/// is again.
bool driving();

bool keyDown(int key);
bool keyPressed(int key);
bool keyReleased(int key);
bool mouseDown(int button);
bool mousePressed(int button);
bool mouseReleased(int button);
Vector2 mousePosition();
float wheelMove();
int charPressed();

}  // namespace devinput
