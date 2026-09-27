// The two things a stream link is allowed to say, and how little either does.
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

    section("where the game is, for a host that is not on the relay");
    {
        // A code alone is joined THROUGH THE RELAY. A link to a host that
        // listens instead used to open the game, fill in the code, and fail.
        const joinlink::Invite i =
            joinlink::parse("opendoctrines://join/ABCD-1234?at=play.example.com:27015");
        ok(i.valid() && i.code == "ABCD-1234", "the code still comes through");
        ok(i.address == "play.example.com:27015", "and the address beside it");

        ok(joinlink::parse("opendoctrines://join/ABCD-1234?at=tidy-otter.trycloudflare.com")
               .address == "tidy-otter.trycloudflare.com", "a tunnel hostname, no port");
        ok(joinlink::parse("opendoctrines://join/ABCD-1234?utm=twitch&at=play.example.com")
               .address == "play.example.com", "found beside a parameter we ignore");
        ok(joinlink::parse("opendoctrines://join/ABCD-1234").address.empty(),
           "a relayed game's link carries none, and that is not an error");
    }

    section("an address the link may not carry");
    {
        // Each of these would make `at` somewhere to send the player, which is
        // the whole reason the field is allowed to exist at all. A refused
        // address leaves the CODE alone: the player gets the relay attempt an
        // older build would have given them, rather than nothing.
        for (const char* bad : {"https://evil.example", "example.com/join",
                                "example.com%2fjoin", "user@example.com",
                                "localhost", "example.com:0", "example.com:99999",
                                "192.168.0.5%3a27015"}) {
            const joinlink::Invite i =
                joinlink::parse(std::string("opendoctrines://join/ABCD-1234?at=") + bad);
            ok(i.code == "ABCD-1234" && i.address.empty(),
               std::string("refused, code kept: ") + bad);
        }
        // The fragment is cut before the query is read, or it would ride along
        // inside the address and be refused for the wrong reason.
        ok(joinlink::parse("opendoctrines://join/ABCD-1234?at=play.example.com#top")
               .address == "play.example.com", "a fragment is not part of the address");
        // An older build cuts the code at the '?' and ignores the rest, which
        // is what makes this link safe to hand to anybody.
        ok(joinlink::codeFrom("opendoctrines://join/ABCD-1234?at=play.example.com") == "ABCD-1234",
           "and the old accessor still answers with just the code");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
