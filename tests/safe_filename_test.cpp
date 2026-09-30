// A WORLD'S NAME BECOMES A FILENAME, AND WINDOWS IS FUSSIER THAN POSIX.
//
// Every world-name field in the game blocked "/\:" and nothing else. That is
// the right list for macOS and Linux and five characters short on Windows:
// * ? " < > | are illegal there and the file cannot be created at all, so a
// world called "What If?" was unsaveable from the moment it was named -- on
// one platform, silently, while it worked everywhere the developers looked.
//
// Two more the field filter could never have caught, because they are made of
// legal characters:
//
//   CON, PRN, AUX, NUL, COM1-9, LPT1-9 are DOS devices. Windows resolves them
//   as devices whatever the extension, so "CON.odsv" is not a file. Measured
//   on Windows 11: the write reports success and the path does not exist
//   afterwards.
//
//   Trailing dots and spaces are stripped by the OS, so "Test." and "Test"
//   are the same file -- while the duplicate check, which compares the names
//   it was handed, sees two different worlds and overwrites one with the other.
//
// The cases below are the ones that were actually run against Windows 11; see
// the table in the commit that added this.

#include "TextInput.h"

#include <cstdio>
#include <string>

static int g_checks = 0, g_failed = 0;
static void ok(bool cond, const std::string& what, const std::string& detail = "") {
    ++g_checks;
    printf("  %-4s  %s%s\n", cond ? "ok" : "FAIL", what.c_str(),
           detail.empty() ? "" : ("  [" + detail + "]").c_str());
    if (!cond) ++g_failed;
}
static void section(const char* n) { printf("\n== %s ==\n", n); }

static void survives(const std::string& in) {
    ok(odSafeFileName(in) == in, "\"" + in + "\" is left alone", odSafeFileName(in));
}
static void becomes(const std::string& in, const std::string& want) {
    const std::string got = odSafeFileName(in);
    ok(got == want, "\"" + in + "\" -> \"" + want + "\"", got);
}

int main() {
    section("names a player may keep");
    survives("Normal World");
    survives("My 1939 Run");
    survives("Cold War (2)");
    survives("a.b.c");                  // interior dots are fine
    survives("Reich");

    section("characters Windows refuses");
    // Each of these failed to create a file at all on Windows 11.
    becomes("What If?",     "What If");
    becomes("Plan A*B",     "Plan AB");
    becomes("He said \"go\"", "He said go");
    becomes("a<b>c",        "abc");
    becomes("Red|Blue",     "RedBlue");

    section("path separators, which were already blocked");
    becomes("a/b",  "ab");
    becomes("a\\b", "ab");
    becomes("C:x",  "Cx");

    section("the DOS devices");
    // Suffixed rather than rejected: the player gets the world they asked for
    // under a name that is a file, instead of an error about a name that looks
    // perfectly ordinary to them.
    becomes("CON",  "CON_");
    becomes("con",  "con_");
    becomes("NUL",  "NUL_");
    becomes("PRN",  "PRN_");
    becomes("AUX",  "AUX_");
    becomes("COM1", "COM1_");
    becomes("LPT9", "LPT9_");
    // Only the whole stem is a device. These are ordinary names.
    survives("CONTROL");
    survives("Concord");
    survives("COM10");

    section("what Windows strips behind your back");
    becomes("Trailing ",  "Trailing");
    becomes("Trailing.",  "Trailing");
    becomes("Trailing. ", "Trailing");
    becomes("Test...",    "Test");

    section("a name that is nothing at all");
    // "" + ".odsv" is a hidden file on POSIX and a refused name on Windows;
    // either way it is not the world the player asked for.
    becomes("???",   "World");
    becomes("...",   "World");
    becomes("",      "World");
    becomes("   ",   "World");

    section("control characters");
    becomes(std::string("a\tb"), "ab");
    becomes(std::string("a\nb"), "ab");

    section("splitting a path the OS handed us");
    {
        // The dialog and the dropped-file list both return NATIVE paths, so on
        // Windows they contain backslashes. Splitting on '/' alone finds
        // nothing, find_last_of gives npos, npos + 1 is 0, and substr(0) is
        // the whole path -- which is what the map importer used to pre-fill
        // its name prompt with, and then made a directory out of.
        auto base = [](const std::string& p) { return odBaseName(p); };
        auto stem = [](const std::string& p) { return odStemName(p); };

        ok(base("C:\\Users\\me\\world.odmap") == "world.odmap",
           "a Windows path gives its filename", base("C:\\Users\\me\\world.odmap"));
        ok(stem("C:\\Users\\me\\world.odmap") == "world",
           "and its stem", stem("C:\\Users\\me\\world.odmap"));
        ok(base("/home/me/world.odmap") == "world.odmap", "a POSIX path still works");
        ok(stem("/home/me/world.odmap") == "world", "and so does its stem");
        ok(base("C:/mixed\\separators/file.odmap") == "file.odmap",
           "a path with both separators splits on the last of either",
           base("C:/mixed\\separators/file.odmap"));
        ok(stem("world.odmap") == "world", "a bare filename needs no directory");
        ok(stem("no-extension") == "no-extension", "a name without an extension keeps all of it");
        ok(stem("archive.tar.gz") == "archive.tar", "only the LAST extension goes");
        ok(base("C:\\Users\\me\\") == "", "a trailing separator leaves nothing after it");
        ok(stem(".hidden") == ".hidden", "a leading dot is a name, not an extension");

        // The two together are what the importer does: name a map from the
        // path the OS gave, then make that name safe to be a directory.
        ok(odSafeFileName(stem("C:\\Users\\me\\My Map?.odmap")) == "My Map",
           "path -> stem -> safe name, which is what the importer needs",
           odSafeFileName(stem("C:\\Users\\me\\My Map?.odmap")));
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
