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
 * The characters a player-typed name may not contain when it is going to
 * become a filename.
 *
 * Every world-name field blocked "/\\:" and nothing else, which is the right
 * list for POSIX and five characters short on Windows: * ? " < > | are illegal
 * there, and the file simply cannot be created. A world called "What If?"
 * saved on macOS and Linux and, on Windows, produced a save path the OS
 * refuses -- so the archive was never written and the world was unsaveable
 * from the moment it was named. Verified on Windows 11: all five fail.
 */
inline constexpr char OD_FILENAME_FORBIDDEN[] = "/\\:*?\"<>|";

/**
 * A player-typed name made safe to use as a filename on every platform.
 *
 * Belt to OD_FILENAME_FORBIDDEN's braces: the field filter stops the
 * characters being typed, and this catches what typing cannot -- the names
 * that are legal to type and still do not work.
 *
 *  - CON, PRN, AUX, NUL, COM1-9 and LPT1-9 are DOS devices, and Windows
 *    resolves them as devices WHATEVER the extension. "CON.odsv" is not a
 *    file; a world named CON writes somewhere that is not disk and can never
 *    be opened again. Verified on Windows 11: the write reports success and
 *    the path does not exist afterwards.
 *  - Windows silently strips trailing dots and spaces from a name, so "Test."
 *    and "Test" are one file, and the game's duplicate check -- which compares
 *    the names it was given -- does not see the collision coming.
 *
 * Returns a name that is never empty, so a field of nothing but forbidden
 * characters cannot produce ".odsv".
 */
inline std::string odSafeFileName(const std::string& name) {
    std::string out;
    for (char c : name) {
        const unsigned char u = (unsigned char)c;
        if (u < 32) continue;                                  // control characters
        bool bad = false;
        for (const char* f = OD_FILENAME_FORBIDDEN; *f; ++f)
            if (*f == c) { bad = true; break; }
        if (!bad) out += c;
    }
    // Windows drops these on its own, which turns "Test." and "Test" into one
    // file behind the duplicate check's back.
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();

    // The DOS devices, matched on the stem and case-insensitively, because
    // that is how Windows matches them.
    // The DOS devices, matched on the stem and case-insensitively, because
    // that is how Windows matches them. CON, PRN, AUX, NUL, and COM1-9 and
    // LPT1-9 -- the digit is checked rather than listed, so this is six short
    // literals instead of twenty-three.
    std::string upper;
    for (char c : out) upper += (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    // i18n-ignore -- device names the OS matches on, never shown to anybody
    const bool named = (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL");
    // i18n-ignore
    const bool numbered = upper.size() == 4 && upper[3] >= '1' && upper[3] <= '9' &&
                          (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0);
    if (named || numbered) out += "_";

    if (out.empty()) out = "World";
    return out;
}

/**
 * The filename part of a path, whichever separator the OS used.
 *
 * Windows hands back backslashes -- GetOpenFileNameW does, and so does
 * raylib's dropped-file list -- and splitting on '/' alone finds nothing in
 * "C:\\Users\\me\\world.odmap". find_last_of returns npos, npos + 1 is 0, and
 * substr(0) is the WHOLE PATH. Importing a map that way named the import
 * after its full path instead of its filename, and that name then became a
 * directory under custom_maps/.
 *
 * Splits on both separators, because a path on Windows may legitimately hold
 * either and often holds both.
 */
inline std::string odBaseName(const std::string& path) {
    const std::string::size_type cut = path.find_last_of("/\\");
    return (cut == std::string::npos) ? path : path.substr(cut + 1);
}

/**
 * The filename part of a path with its last extension removed.
 *
 * What a "name this import" prompt should be pre-filled with.
 */
inline std::string odStemName(const std::string& path) {
    std::string base = odBaseName(path);
    const std::string::size_type dot = base.find_last_of('.');
    // A leading dot is the whole name of a hidden file, not an extension.
    if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
    return base;
}

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
