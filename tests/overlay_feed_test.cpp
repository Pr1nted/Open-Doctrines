// What the streaming software is told, and what it is never told.
//
// Build target: OverlayFeedTest. Non-zero exit means a case failed.

#include "stream/OverlayFeed.h"

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
    printf("Overlay feed\n");

    section("the two shapes");
    {
        overlay::Feed f;
        f.add("turn", "12");
        f.add("date", "May 1939");
        f.add("country", "French Republic");
        ok(overlay::toText(f) == "turn: 12\ndate: May 1939\ncountry: French Republic\n",
           "a line per fact, for a Text source");
        ok(overlay::toJson(f) ==
               "{\"turn\":\"12\",\"date\":\"May 1939\",\"country\":\"French Republic\"}",
           "and a flat object, for a Browser source");
        ok(overlay::toJson(overlay::Feed{}) == "{}", "an empty feed is an empty object");
        ok(overlay::toText(overlay::Feed{}).empty(), "and no text at all");
    }
    {
        // A country name can contain a quote or a backslash; the JSON must
        // survive it, because an overlay that will not parse is an overlay that
        // shows nothing for the rest of the stream.
        overlay::Feed f;
        f.add("country", "The \"Republic\"");
        ok(overlay::toJson(f).find("\\\"Republic\\\"") != std::string::npos,
           "quotes in a value are escaped");
    }

    section("what never reaches the camera");
    {
        // The overlay is the one part of the game GUARANTEED to be on stream,
        // so these are refused whether or not stream-safe mode is on.
        ok(!overlay::safeToShow("/Users/anna/saves/x.odsv"),
           "a path with somebody's name in it");
        ok(!overlay::safeToShow("could not open /home/vlad/x.odsv"),
           "even inside a sentence");
        ok(!overlay::safeToShow(std::string(32, 'a')),
           "something shaped like an account id");
        ok(!overlay::safeToShow("0123456789abcdef0123456789abcdef"),
           "and a real-looking one");
        ok(overlay::safeToShow("French Republic"), "an ordinary name is fine");
        ok(overlay::safeToShow("12"), "and a number");
        // 32 characters that are not all hex is just a long label.
        ok(overlay::safeToShow("The Most Serene Republic of Venice"),
           "a long country name is not an id");
    }
    {
        overlay::Feed f;
        f.add("turn", "12");
        f.add("save", "/Users/anna/saves/x.odsv");
        f.add("country", "France");
        const std::string text = overlay::toText(f);
        ok(text.find("anna") == std::string::npos, "an unsafe fact is dropped");
        ok(text.find("turn: 12") != std::string::npos, "and the safe ones stay");
        ok(text.find("country: France") != std::string::npos, "all of them");
        ok(overlay::toJson(f).find("anna") == std::string::npos,
           "dropped from the JSON too, not only the text");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
