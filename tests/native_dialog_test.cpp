// The system file chooser, on whatever platform is running this.
//
// A dialog is a modal window waiting for a person, and CI has no person. What
// CI can check is everything up to the window, which is where this actually
// breaks: a quote in the wrong place, a flag the installed helper does not
// have, a title that ends the string it was pasted into. Three layers:
//
//   1. The command is built correctly, on every platform, including for a
//      title full of shell punctuation. Runs everywhere, opens nothing.
//   2. The command's HELPER accepts it -- the real zenity, the real
//      PowerShell -- checked by running the helper in a way that returns
//      rather than waits. Only where that helper exists.
//   3. The output is read back correctly, by putting a stub on PATH that
//      prints a path and asserting the answer comes back intact. Unix only,
//      because that is where PATH shadowing is a one-line trick.
//
// What is left is a person clicking OK, and nothing here pretends otherwise.
#include "util/NativeDialog.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;

static void check(bool ok, const std::string& what) {
    std::printf("  %-4s  %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int main() {
    std::printf("=== the system file chooser ===\n");

    // ---- 1. what gets built ----
    const std::string file = NativeDialog::commandFor(NativeDialog::Kind::File,
                                                      "Select a map file", "odmap");
    const std::string folder = NativeDialog::commandFor(NativeDialog::Kind::Folder,
                                                        "Select a folder", "");

    if (!NativeDialog::available()) {
        // The web and Android have no chooser, and must not build a command
        // that some later change might decide to run.
        check(file.empty() && folder.empty(), "no chooser here, so no command is built");
        check(NativeDialog::openFile("x", "odmap").empty(), "openFile answers empty");
        check(NativeDialog::openFolder("x").empty(), "openFolder answers empty");
        std::printf("%s\n", failures == 0 ? "all passed" : "SOMETHING FAILED");
        return failures == 0 ? 0 : 1;
    }

    // A desktop with neither zenity nor kdialog builds nothing, which is not a
    // failure -- it is the "" that callers already treat as a cancelled
    // dialog. Everything below is conditional on there being a helper at all.
    const bool haveHelper = !file.empty();
    if (!haveHelper) {
        std::printf("  skip  no file chooser helper is installed here\n");
        std::printf("%s\n", failures == 0 ? "all passed" : "SOMETHING FAILED");
        return failures == 0 ? 0 : 1;
    }

    check(contains(file, "Select a map file"), "the file command carries its title");
    check(contains(folder, "Select a folder"), "the folder command carries its title");
    check(file != folder, "picking a file and picking a folder are not the same command");
    check(contains(file, "odmap"), "the extension reaches the command");

    // The one that matters. A title is a string this code pastes into a shell
    // command and, on two platforms, into a script inside that command. If a
    // caller ever passes a map name through, none of this may survive into the
    // command as punctuation.
    const std::string nasty = "a'b\"c`d$e;f&g|h(i)j<k>l\nm";
    const std::string cleaned = NativeDialog::safeTitle(nasty);

    // Asserted on the CLEANED title rather than by hunting for pairs in the
    // finished command. The command legitimately puts quotes around the title
    // -- $d.Title='...' on Windows, --title='...' for zenity -- so a search of
    // the command for a quote next to a letter finds the one the command added
    // and calls it a leak. That is not a subtle distinction to get wrong: it
    // reported a hole in the sanitiser where there was none, on one platform
    // and not the others, purely because of how each quotes.
    for (char c : std::string("'\"`$;&|()<>\n\r\\")) {
        check(cleaned.find(c) == std::string::npos,
              std::string("the sanitiser removes ") + (c == '\n' ? std::string("a newline")
                                                     : c == '\r' ? std::string("a return")
                                                     : std::string(1, c)));
    }
    check(cleaned.size() == nasty.size(), "and removes it by replacement, not by deletion");

    // And what the command carries is that cleaned title, nothing else.
    const std::string hostile = NativeDialog::commandFor(NativeDialog::Kind::Folder, nasty, "");
    check(contains(hostile, cleaned), "the command carries the cleaned title");
    check(NativeDialog::safeTitle("plain words 123-_.") == "plain words 123-_.",
          "an ordinary title is left alone");

    // ---- 2. does the real helper accept it? ----
    //
    // The command is run in a form that returns instead of waiting. What is
    // being asserted is only that the helper understood its own flags: a usage
    // error is a command this code got wrong, and it is the failure that would
    // otherwise reach a player as a dialog that never appears.
#if defined(__linux__) && !defined(__ANDROID__)
    if (std::getenv("DISPLAY") && contains(file, "zenity")) {
        // zenity's own timeout, so nothing waits on a person. 5 is its exit
        // code for "timed out", which means it got as far as showing a window.
        std::string timed = file;
        const size_t redirect = timed.find(" 2>/dev/null");
        if (redirect != std::string::npos) timed = timed.substr(0, redirect);
        const int rc = std::system((timed + " --timeout=1 >/dev/null 2>&1").c_str());
        check(WEXITSTATUS(rc) == 5 || WEXITSTATUS(rc) == 1,
              "the installed zenity accepts these flags");
    } else {
        std::printf("  skip  no DISPLAY or no zenity: cannot ask the real helper\n");
    }
#elif defined(_WIN32)
    // Everything the real command does except show the window: load the
    // assembly, construct the dialog, set the properties from OUR quoted
    // strings. A quoting mistake fails here exactly as it would there.
    {
        std::string probe = file;
        const size_t call = probe.find("if($d.ShowDialog()");
        check(call != std::string::npos, "the command is shaped as expected");
        if (call != std::string::npos) {
            probe = probe.substr(0, call) + "'ok'\" 2>NUL";
            const int rc = std::system((probe + " >NUL").c_str());
            check(rc == 0, "PowerShell loads the dialog and takes these properties");
        }
    }
#else
    std::printf("  note  no helper probe on this platform\n");
#endif

    // ---- 3. is the answer read back? ----
#if !defined(_WIN32)
    {
        // A stub named after whichever helper this platform builds a command
        // for, first on PATH, printing a path with a trailing newline the way
        // a real one does.
        const char* helper = contains(file, "zenity") ? "zenity"
                           : contains(file, "kdialog") ? "kdialog"
                           : contains(file, "osascript") ? "osascript" : nullptr;
        if (helper) {
            const std::string dir = "/tmp/od-dialog-stub";
            const std::string want = "/tmp/a folder with spaces";
            std::string setup = "rm -rf " + dir + " && mkdir -p " + dir +
                                " && printf '#!/bin/sh\\necho \"" + want + "\"\\n' > " + dir +
                                "/" + helper + " && chmod +x " + dir + "/" + helper;
            if (std::system(setup.c_str()) == 0) {
                const std::string old = std::getenv("PATH") ? std::getenv("PATH") : "";
                setenv("PATH", (dir + ":" + old).c_str(), 1);
                const std::string got = NativeDialog::openFolder("Pick one");
                setenv("PATH", old.c_str(), 1);
                check(got == want, "the chosen path comes back whole, spaces and all");
                std::system(("rm -rf " + dir).c_str());
            }
        }
    }
#endif

    std::printf("%s\n", failures == 0 ? "all passed" : "SOMETHING FAILED");
    return failures == 0 ? 0 : 1;
}
