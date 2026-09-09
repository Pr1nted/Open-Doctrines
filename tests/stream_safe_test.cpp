// Hiding the things a stream should not carry.
//
// Build target: StreamSafeTest. Non-zero exit means a case failed.

#include "StreamSafe.h"

#include <cstdio>
#include <string>

namespace {
int g_checks = 0, g_fails = 0;
void ok(bool c, const std::string& what) {
    ++g_checks;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!c) ++g_fails;
}
void section(const char* t) { printf("\n== %s ==\n", t); }
}  // namespace

int main() {
    printf("Stream-safe mode\n");

    section("a secret, as the camera should see it");
    {
        const std::string masked = streamsafe::maskSecret("ODT-9F4K");
        ok(masked.find("ODT") == std::string::npos, "the code itself is gone");
        ok(!masked.empty(), "but the field is not blank, which would look broken");
        // The LENGTH is a hint too: a short code is a smaller thing to guess.
        ok(streamsafe::maskSecret("A") == streamsafe::maskSecret("A-MUCH-LONGER-CODE"),
           "and two codes of different lengths mask identically");
        ok(streamsafe::maskSecret("").empty(), "nothing masks to nothing");
    }

    section("a path with the name taken out");
    {
        const std::string home = "/Users/anna";
        ok(streamsafe::redactPath("/Users/anna/saves/Bench.odsv", home)
               == "~/saves/Bench.odsv",
           "the home directory becomes a tilde");
        ok(streamsafe::redactPath("/Users/anna", home) == "~",
           "and the home directory alone is just the tilde");
        // The boundary case: a prefix match that is not a directory boundary.
        ok(streamsafe::redactPath("/Users/annabel/saves/x.odsv", home)
               .find("annabel") == std::string::npos,
           "a longer name that starts the same is still redacted");
        ok(streamsafe::redactPath("/opt/games/od/save.odsv", home)
               == "/opt/games/od/save.odsv",
           "a path with no name in it is left alone");
    }
    {
        // Somebody else's machine: no home to compare against, so the shape of
        // the path is what gives it away.
        ok(streamsafe::redactPath("/Users/vlad/x.odsv", "") == "~/x.odsv",
           "macOS home shape is recognised with no home given");
        ok(streamsafe::redactPath("/home/vlad/x.odsv", "") == "~/x.odsv",
           "and the Linux one");
        ok(streamsafe::redactPath("C:\\Users\\vlad\\x.odsv", "") == "~\\x.odsv",
           "and the Windows one");
        ok(streamsafe::redactPath("", "").empty(), "nothing redacts to nothing");
    }

    section("spotting one inside somebody else's sentence");
    {
        ok(streamsafe::looksSensitive("could not open /Users/anna/saves/x.odsv"),
           "a path inside an error message is spotted");
        ok(!streamsafe::looksSensitive("could not reach the service"),
           "an ordinary message is not");
        ok(!streamsafe::looksSensitive(""), "and neither is nothing");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
