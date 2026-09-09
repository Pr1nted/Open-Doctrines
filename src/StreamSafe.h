#pragma once

// Playing in front of an audience, without handing them the keys.
//
// ── WHAT ACTUALLY LEAKS ON A STREAM ──
//
// Not the things people expect. A streamer's password is behind a password
// manager; what goes out over the wire is whatever the game happened to draw
// on screen while somebody was recording it, and viewers pause and zoom.
//
//   AN INVITE CODE is a key. It is on screen for the whole lobby, and anybody
//   reading it can walk into a private game -- during a tournament, that is the
//   tournament.
//   A TUNNEL ADDRESS is a route to the machine. It outlives the session.
//   AN ACCOUNT ID identifies the person across every server they have played
//   on, which is the one identifier the account system exists to keep local.
//   A FILE PATH is usually "/Users/<their real name>/..." and turns up in save
//   dialogs, error messages and log overlays. It is the commonest real-name
//   leak in any game with a file browser, and nobody thinks of it.
//
// ── AND WHAT THIS IS NOT ──
//
// It is not a security boundary and cannot be. Anything the game knows, the
// person at the keyboard can still see; this hides things from the CAMERA, not
// from the player. It also cannot help with what a streamer types into their
// own chat. What it does is remove the accidents -- the code left on screen
// during a scene transition, the path in the corner of a crash message -- which
// is where this actually goes wrong in practice.
//
// The helpers are pure so every one of them is testable, and because the rule
// "what does a redacted path look like" must have exactly one answer. See
// tests/stream_safe_test.cpp.

#include <string>

namespace streamsafe {

/**
 * A secret, as the camera should see it: the shape, and nothing else.
 *
 * Not blanked entirely. A field that goes empty looks broken and invites
 * somebody to turn the mode off to check it is still there; "••••••••" reads
 * as deliberate, and the length is not worth hiding.
 */
std::string maskSecret(const std::string& value);

/**
 * A path with the person's name taken out of it: "~/saves/Bench 1914.odsv".
 *
 * The home directory is the part that carries a real name, and it is also the
 * part nobody needs to read. Everything below it is kept, because "which save
 * is that?" is a real question a viewer or a clip might have to answer.
 *
 * `home` is passed in rather than read from the environment so this stays pure
 * and testable on a machine that is not the one being redacted.
 */
std::string redactPath(const std::string& path, const std::string& home);

/**
 * Whether `text` looks like something that should not be on camera.
 *
 * Used for the places that print a line the game did not compose itself -- a
 * status message, an error from a service -- where the risk is a code or a path
 * arriving inside somebody else's sentence.
 */
bool looksSensitive(const std::string& text);

}  // namespace streamsafe
