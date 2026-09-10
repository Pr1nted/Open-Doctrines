// The client half of bug reporting: the two rules that, if broken, are broken
// silently.
//
// Everything else about this feature announces its own failure -- a report that
// does not send says so on the form. These two do not:
//
//   1. Scrubbing the home directory out of the diagnostics. A leak here does
//      not look like a bug. It looks like a bug report, filed in a public
//      tracker, with somebody's real name in the middle of it.
//
//   2. Encoding the payload as JSON. A quote or a newline in a report that is
//      not escaped does not produce a mangled report; it produces a body the
//      Worker cannot parse at all, so the reports that fail to arrive are
//      exactly the ones written by people who quoted the error message.
//
// The functions are replicated here rather than linked, in the manner of the
// other rule tests: reaching the real ones means linking raylib and the config.
// If the rule changes, change it here in the same commit.

#include <cstdio>
#include <cstdlib>
#include <string>

// MSVC has no POSIX setenv/unsetenv, and this test needs them: it sets HOME and
// USERPROFILE to check that a personal path is scrubbed out of a bug report.
// Shimmed rather than skipped on Windows -- USERPROFILE is the variable that
// MATTERS there, so skipping would drop the coverage exactly where the rule is
// most load-bearing. _putenv_s with an empty value removes the variable, which
// is what unsetenv means here. Same shape as src/llm/Runner.cpp.
#if defined(_WIN32)
static int setenv(const char* k, const char* v, int) { return _putenv_s(k, v); }
static int unsetenv(const char* k) { return _putenv_s(k, ""); }
#endif

static int g_checks = 0, g_fails = 0;
static void ok(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { printf("  ok    %s\n", what.c_str()); return; }
    ++g_fails; printf("  FAIL  %s\n", what.c_str());
}
static void section(const char* t) { printf("\n== %s ==\n", t); }

// --- the rules, as src/Feedback.cpp has them ---

static std::string scrubPersonalPaths(std::string text) {
    const char* vars[] = {"HOME", "USERPROFILE"};
    for (const char* v : vars) {
        const char* home = std::getenv(v);
        if (!home || !*home) continue;
        const std::string h = home;
        if (h.size() < 4) continue;
        for (size_t at = text.find(h); at != std::string::npos; at = text.find(h, at + 1))
            text.replace(at, h.size(), "~");
    }
    return text;
}

static std::string escapeJson(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

int main() {
    printf("Feedback client rules\n");

    section("the home directory never leaves the machine");
    {
        // A real home, so the test exercises the same string the game will.
        setenv("HOME", "/Users/aleksandra", 1);
        unsetenv("USERPROFILE");

        const std::string diag =
            "  save: /Users/aleksandra/Library/OpenDoctrines/saves/turn40.odsave\n"
            "  mods: /Users/aleksandra/mods/greatpowers.odmod\n";
        const std::string out = scrubPersonalPaths(diag);

        ok(out.find("aleksandra") == std::string::npos,
           "the account name is gone");
        ok(out.find("~/Library/OpenDoctrines/saves/turn40.odsave") != std::string::npos,
           "and what is left still says which file");
        ok(out.find("~/mods/greatpowers.odmod") != std::string::npos,
           "EVERY occurrence, not just the first -- a diagnostics block has many");
    }
    {
        // The guard that matters most: a home of "/" would replace every slash
        // in the file and hand the maintainer an unreadable block.
        setenv("HOME", "/", 1);
        const std::string path = "/usr/share/opendoctrines/data";
        ok(scrubPersonalPaths(path) == path,
           "a root home is refused rather than applied");
    }
    {
        setenv("HOME", "/Users/lee", 1);
        ok(scrubPersonalPaths("nothing personal here") == "nothing personal here",
           "text with no path is left exactly alone");
    }

    section("the payload survives what people actually type");
    {
        // The report that breaks a naive encoder is the most useful one there
        // is: somebody pasting the error, with its quotes and its line breaks.
        const std::string body =
            "Script said: \"unknown statement\" on line 4.\n\tI expected it to run.";
        const std::string enc = escapeJson(body);

        ok(enc.find("\\\"unknown statement\\\"") != std::string::npos, "quotes are escaped");
        ok(enc.find("\\n") != std::string::npos, "newlines are escaped");
        ok(enc.find("\\t") != std::string::npos, "tabs are escaped");
        ok(enc.find('\n') == std::string::npos, "and no raw control byte is left in");
    }
    {
        const std::string win = "C:\\Users\\lee\\OpenDoctrines";
        ok(escapeJson(win) == "C:\\\\Users\\\\lee\\\\OpenDoctrines",
           "a Windows path's backslashes are escaped, not swallowed");
    }
    {
        // Non-ASCII is passed through as its own bytes. A report typed in
        // Ukrainian is a report, and \u-escaping it here would double-encode
        // what is already valid UTF-8 in a JSON document.
        const std::string cyrillic = "Гра зависла";
        ok(escapeJson(cyrillic) == cyrillic, "UTF-8 goes through untouched");
    }
    {
        char raw[] = {'a', (char)0x07, 'b', '\0'};
        ok(escapeJson(raw) == "a\\u0007b", "a stray control byte becomes a \\u escape");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
