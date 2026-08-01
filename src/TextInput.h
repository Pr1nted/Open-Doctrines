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
