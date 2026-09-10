#pragma once
#include <string>
#include <cstddef>

/**
 * The editing keys every text field in the game shares.
 *
 * Typing has always played a click at each call site. Deleting played nothing,
 * which makes a field feel broken the first time you correct a typo -- the
 * keyboard answers when you add a letter and goes silent when you remove one.
 * And no field accepted a paste, so a server address or an account id had to
 * be typed out by hand with the clipboard sitting right there.
 *
 * Both live here rather than at the call sites because there are two dozen of
 * these fields across the game and the map editor, and they would drift apart
 * immediately if each grew its own copy.
 *
 * @param field      the string being edited, modified in place
 * @param maxLen     hard cap; a paste is truncated to fit, never overflows it
 * @param forbidden  characters this field refuses (path separators, say)
 * @param digitsOnly numeric fields; a paste of anything else is dropped rather
 *                   than letting the clipboard put letters somewhere typing
 *                   never could
 * @return           true if the field changed
 */
bool odTextEditKeys(std::string& field, size_t maxLen,
                    const char* forbidden = "", bool digitsOnly = false);

/**
 * Text the player has just asked to paste, or empty.
 *
 * Consumes it: calling twice in a frame gives the second caller nothing, which
 * is what stops a paste landing in two fields at once.
 *
 * DESKTOP is Ctrl+V or Cmd+V plus GetClipboardText(). WEB cannot work that
 * way -- reading the clipboard in a browser is asynchronous, permission-gated,
 * and refused outright inside a sandboxed iframe -- so there the shell listens
 * for the browser's own `paste` event, which needs no permission because the
 * keypress IS the consent, and this collects what it left behind.
 */
std::string odTakePaste();

/**
 * Append pasted text to a UTF-8 field, honouring a BYTE cap.
 *
 * For the fields that do their own typing because they accept more than ASCII
 * (the announcement body, a bug report, a moderator's note). Stops at the
 * first line break -- text copied out of a terminal brings one along, and it
 * is never meant as part of the value -- and never splits a UTF-8 sequence.
 *
 * @return true if anything was added
 */
bool odTextAppendPaste(std::string& field, const std::string& pasted, size_t maxBytes);
