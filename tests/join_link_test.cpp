// The one thing a stream link is allowed to say.
//
// Build target: JoinLinkTest. Non-zero exit means a case failed.

#include "stream/JoinLink.h"

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
    printf("Join links\n");

    section("what a link may say");
    {
        ok(joinlink::codeFrom("opendoctrines://join/ABCD-1234") == "ABCD-1234",
           "a code comes through");
        ok(joinlink::codeFrom("opendoctrines://join/ABCD-1234/") == "ABCD-1234",
           "a trailing slash from a chat client is trimmed");
        ok(joinlink::codeFrom("opendoctrines://join/ABCD-1234?utm=twitch") == "ABCD-1234",
           "and a tracking query a browser appended");
        ok(joinlink::codeFrom("opendoctrines://join/ABCD-1234#x") == "ABCD-1234",
           "and a fragment");
    }

    section("and everything it may not");
    {
        // A scheme handler is input from outside the game; on macOS a web page
        // can hand the game one of these. So anything that is not exactly a
        // code is refused rather than interpreted.
        ok(joinlink::codeFrom("opendoctrines://settings/streamSafe=false").empty(),
           "a different verb does nothing");
        ok(joinlink::codeFrom("opendoctrines://join/../../etc/passwd").empty(),
           "a path traversal is not a code");
        ok(joinlink::codeFrom("opendoctrines://join/%2e%2e%2f").empty(),
           "and neither is a percent-escaped one, which is never decoded");
        ok(joinlink::codeFrom("opendoctrines://join/a b").empty(), "nor a space");
        ok(joinlink::codeFrom("opendoctrines://join/x.odsv").empty(),
           "nor anything that looks like a file");
        ok(joinlink::codeFrom("opendoctrines://join/").empty(), "nor an empty code");
        ok(joinlink::codeFrom("opendoctrines://join").empty(), "nor no code at all");
        ok(joinlink::codeFrom(std::string("opendoctrines://join/") +
                              std::string(40, 'a')).empty(),
           "nor one longer than any session code");
        ok(joinlink::codeFrom("https://example.com/join/ABCD").empty(),
           "another scheme is not ours");
        ok(joinlink::codeFrom("").empty(), "and nothing is nothing");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
