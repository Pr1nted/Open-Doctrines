// What a map says its countries did, and the one property that matters most:
// that saying nothing changes nothing.
//
// The doctrine leans on the AI's existing winnability score. A bias that is not
// exactly 1 when the feature is off, or when the map is silent, would quietly
// move every benched number in the game -- so inertness is asserted per call
// here rather than inferred from a screenshot of one battle.

#include "map/HistoryFile.h"

#include <cmath>
#include <cstdio>
#include <string>

static int checks = 0, fails = 0;
static void ok(bool cond, const char* what) {
    ++checks;
    if (!cond) { ++fails; printf("  FAIL  %s\n", what); }
    else printf("  ok    %s\n", what);
}
static void section(const char* name) { printf("\n== %s ==\n", name); }
static bool near1(float v) { return std::fabs(v - 1.0f) < 1e-6f; }

int main() {
    const std::string doc = R"({
      "_comment": "keys that are not three letters are ignored",
      "GER": [
        { "from": 1938, "to": 1941, "expand": ["POL","FRA","cze"],
          "restrain": ["ITA"], "press": 2.0, "avoid": 0.5,
          "note": "east first, and not against the other Axis" },
        { "from": 1941, "to": 1945, "expand": ["SOV"] }
      ],
      "ITA": [ { "expand": ["ETH"] } ],
      "EMPTY": [ { "from": 1900, "to": 1910 } ]
    })";

    history::Doctrines d;
    section("reading a map's history");
    {
        ok(history::parse(doc, d), "the document parses");
        ok(d.count("GER") == 1, "a country with two windows is read");
        ok(d.count("ITA") == 1, "and one with a window that has no dates");
        ok(d.count("_COMMENT") == 0, "a non-ISO key is not a country");
        ok(d.count("EMPTY") == 0, "a window saying nothing is dropped entirely");
        ok(d["GER"].size() == 2, "both of Germany's windows survive");
        ok(d["GER"][0].expand.size() == 3, "lower-case ISO codes are accepted");
    }

    section("the bias is exactly 1 when nothing is said");
    {
        history::Doctrines none;
        ok(near1(history::pressure(none, "GER", "POL", 1939)), "no doctrine at all");
        ok(near1(history::pressure(d, "USA", "JPN", 1941)), "a country the map omits");
        ok(near1(history::pressure(d, "GER", "POL", 1925)), "a year before every window");
        ok(near1(history::pressure(d, "GER", "POL", 1990)), "a year after every window");
        ok(near1(history::pressure(d, "GER", "USA", 1939)),
           "a target the open window does not mention");
        // A window that reaches back past any real date, so "the year is
        // unknown" is the ONLY thing that can keep this at 1. Without it the
        // check passes by coincidence -- kNoYear is below every ordinary
        // window's start -- and a deleted guard goes unnoticed. It did.
        history::Doctrines always;
        history::parse(R"({"GER":[{"from":-2000000000,"expand":["POL"]}]})", always);
        ok(history::pressure(always, "GER", "POL", 1939) > 1.0f,
           "a window with no real start is open in 1939");
        ok(near1(history::pressure(always, "GER", "POL", history::kNoYear)),
           "but a map whose date is prose gets no doctrine at all");
    }

    section("and says something when the map does");
    {
        ok(history::pressure(d, "GER", "POL", 1939) > 1.5f, "an expansion target is pressed");
        ok(history::pressure(d, "GER", "ITA", 1939) < 0.9f, "a restrained one is discouraged");
        ok(history::pressure(d, "GER", "SOV", 1942) > 1.0f, "the later window takes over");
        ok(near1(history::pressure(d, "GER", "SOV", 1939)),
           "and says nothing before it opens");
        // Windows overlap at 1941: press 2.0 from the first, press 1.6 (the
        // default) from the second. Both are heard.
        ok(history::pressure(d, "GER", "POL", 1941) > 1.9f, "overlapping windows compound");
    }

    section("a map author cannot make a country mad");
    {
        const std::string wild = R"({"GER":[{"expand":["POL"],"press":1000.0},
                                      {"expand":["POL"],"press":1000.0}]})";
        history::Doctrines w;
        history::parse(wild, w);
        ok(history::pressure(w, "GER", "POL", 1939) <= 3.0f, "pressure is capped");
        const std::string dead = R"({"GER":[{"restrain":["POL"],"avoid":0.0001}]})";
        history::Doctrines k;
        history::parse(dead, k);
        ok(history::pressure(k, "GER", "POL", 1939) >= 0.15f, "and floored");
    }

    section("the year a map date carries");
    {
        ok(history::yearOf("August 1940 AD") == 1940, "a month and a year");
        ok(history::yearOf("Spring 1936 AD") == 1936, "a season counts as a month");
        ok(history::yearOf("March 44 BC") == -44, "BC comes back negative");
        ok(history::yearOf("Spring of the Third Age") == history::kNoYear,
           "prose is refused rather than guessed at");
        ok(history::yearOf("") == history::kNoYear, "and so is nothing at all");
    }

    section("a broken file is a quiet one");
    {
        history::Doctrines bad;
        ok(!history::parse("{ not json", bad), "malformed json is reported");
        ok(bad.empty(), "and leaves nothing behind");
        ok(near1(history::pressure(bad, "GER", "POL", 1939)), "so the game is unchanged");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
