#pragma once

// Opening a web page from inside the game, on every surface including one
// where the obvious way silently does nothing.
//
// raylib's OpenURL is `window.open(url, '_blank')` on the web. Inside a
// Discord Activity the page is a sandboxed iframe and that call is refused
// without an error, a console message or a return value -- so the Account
// screen said "Finish signing in in your browser", the player pressed "Open
// the page again", and nothing whatsoever happened. There is no way to detect
// the refusal after the fact; the only fix is to not make that call.
//
// Discord's own route is the SDK's openExternalLink command, which shows the
// player a confirmation and then opens the link outside Discord. The shell
// exposes it as window.odOpenExternal and falls back to window.open when
// there is no Activity, so this is one call for all four platforms.

#include <string>

namespace odlink {

/**
 * Open `url` in whatever counts as the player's browser here.
 *
 * Desktop and Android: raylib's OpenURL. Web: window.odOpenExternal, which is
 * Discord's openExternalLink inside an Activity and window.open everywhere
 * else.
 *
 * Nothing is reported back, deliberately -- none of the four platforms tells
 * us whether a page actually opened, and a return value nobody can compute
 * honestly would only invite a caller to branch on a guess.
 */
void open(const std::string& url);

}  // namespace odlink
