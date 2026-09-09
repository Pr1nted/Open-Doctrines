// The two lines Discord shows under somebody's name.
//
// Build target: PresenceTest. Non-zero exit means a case failed.

#include "stream/DiscordRpc.h"
#include "stream/Presence.h"

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
using presence::Where;
}  // namespace

int main() {
    printf("Discord presence\n");

    section("what each screen says");
    {
        auto a = presence::describe(Where::Menu, "", "");
        ok(a.details == "In the main menu", "the menu");

        a = presence::describe(Where::MapEditor, "Baltic 1904", "");
        ok(a.details == "Working on a map!", "the editor says what was asked for");
        ok(a.state == "Baltic 1904", "with the map's name under it");

        a = presence::describe(Where::MapEditor, "", "");
        ok(a.details == "Working on a map!" && a.state.empty(),
           "and an unsaved map simply has no name");

        a = presence::describe(Where::Playing, "1939", "French Republic");
        ok(a.details == "Playing 1939", "a world names its scenario");
        ok(a.state == "as French Republic", "and the country under it");
    }
    {
        // Neither half may leave a sentence dangling.
        auto a = presence::describe(Where::Playing, "", "France");
        ok(a.details == "Playing a world", "a world with no scenario name still reads");
        a = presence::describe(Where::Playing, "1939", "");
        ok(a.details == "Playing 1939" && a.state.empty(),
           "and one with no country does not say \"as\" nothing");
    }

    section("what it never says");
    {
        // This line is read by everybody in every server they are in.
        auto a = presence::describe(Where::Multiplayer, "ABCD-1234", "France");
        ok(a.details.find("ABCD") == std::string::npos,
           "a lobby never shows the invite code");
        ok(a.state.find("ABCD") == std::string::npos, "not on either line");
    }

    section("a name somebody chose, and Discord's limits");
    {
        // A scenario name is a file name: it can be anything.
        auto a = presence::describe(Where::Playing, "  spaced  ", "");
        ok(a.details == "Playing spaced", "outer space is trimmed");

        a = presence::describe(Where::MapEditor, "two\nlines", "");
        ok(a.state.find('\n') == std::string::npos,
           "a newline cannot break the line in two on Discord's side");

        const std::string huge(400, 'x');
        a = presence::describe(Where::MapEditor, huge, "");
        ok(a.state.size() <= presence::kMaxField,
           "an over-long name is cut to what Discord will show");

        // A truncated UTF-8 sequence renders as a replacement glyph, and most
        // of this game's languages are multi-byte.
        std::string cyrillic;
        for (int i = 0; i < 100; ++i) cyrillic += "\xD0\x9F";   // П
        a = presence::describe(Where::MapEditor, cyrillic, "");
        ok(a.state.size() <= presence::kMaxField, "a multi-byte name is capped too");
        bool wellFormed = true;
        for (size_t i = 0; i < a.state.size();) {
            const unsigned char c = a.state[i];
            const size_t len = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2
                             : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            if (len == 0 || i + len > a.state.size()) { wellFormed = false; break; }
            i += len;
        }
        ok(wellFormed, "and is not cut in the middle of a character");
    }

    section("the JSON Discord is actually sent");
    {
        const auto a = presence::describe(Where::Playing, "1939", "France");
        const std::string j = discordrpc::activityPayload(a, 4242, 1789000000, "n1");
        ok(j.find("\"cmd\":\"SET_ACTIVITY\"") != std::string::npos, "the command");
        ok(j.find("\"pid\":4242") != std::string::npos,
           "the pid, which Discord requires to attach the presence to a process");
        ok(j.find("\"details\":\"Playing 1939\"") != std::string::npos, "the first line");
        ok(j.find("\"state\":\"as France\"") != std::string::npos, "the second");
        ok(j.find("\"start\":1789000000") != std::string::npos,
           "and a start time, so Discord shows elapsed time");
    }
    {
        // An empty field sent as "" renders as a blank line under the name,
        // which reads as a bug in the game to everybody who sees it.
        const auto a = presence::describe(Where::Menu, "", "");
        const std::string j = discordrpc::activityPayload(a, 1, 0, "n");
        ok(j.find("\"state\"") == std::string::npos, "an empty line is omitted, not sent empty");
        ok(j.find("timestamps") == std::string::npos, "and so is a missing start time");
    }
    {
        // A map name is a file name and can contain anything.
        presence::Activity a;
        a.details = "Working on \"a\" map\\";
        const std::string j = discordrpc::activityPayload(a, 1, 0, "n");
        ok(j.find("\\\"a\\\"") != std::string::npos, "quotes in a name are escaped");
        ok(j.find("map\\\\") != std::string::npos, "and backslashes");
    }
    {
        const std::string h = discordrpc::handshakePayload("123456789012345678");
        ok(h == "{\"v\":1,\"client_id\":\"123456789012345678\"}",
           "the handshake is exactly what Discord expects");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
