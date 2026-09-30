// THE FOUR DAYS A YEAR THIS CAN BE WRONG ON.
//
// A splash line is a joke and does not matter. The date arithmetic underneath
// it does, because it is the same arithmetic any seasonal thing needs, it is
// written once, it looks right, and it is wrong only on days nobody is testing
// on. The new year window is the whole problem in one: it runs from the 30th
// of December to the 2nd of January, so a plain "from <= today <= to" is false
// on EVERY day of it -- including new year's day itself.
//
// So every boundary gets a check: the day before a window, both its ends, the
// days inside, and the day after.
#include "Splash.h"

#include <cstdio>
#include <string>

static int checks = 0, failed = 0;
static void ok(bool c, const std::string& what, const std::string& got = "") {
    ++checks;
    printf("  %-4s  %s%s\n", c ? "ok" : "FAIL", what.c_str(),
           got.empty() ? "" : ("  [" + got + "]").c_str());
    if (!c) ++failed;
}
static void section(const char* n) { printf("\n== %s ==\n", n); }

static odsplash::Splashes build() {
    odsplash::Splashes s;
    s.any = {"everyday one", "everyday two"};
    s.occasions = {
        {"halloween", "bats",     10, 29, 11, 1,  {"spooky"}},
        {"newyear",   "snow",     12, 30,  1, 2,  {"happy new turn"}},
        {"firstlight","confetti",  8,  2,  8, 2,  {"it went live today"}},
    };
    return s;
}

int main() {
    const odsplash::Splashes s = build();
    auto on = [&](int m, int d) {
        const odsplash::Occasion* o = odsplash::occasionFor(s, m, d);
        return o ? o->id : std::string("-");
    };

    section("an ordinary day is ordinary");
    ok(on(6, 15) == "-", "15 June belongs to no occasion", on(6, 15));
    ok(odsplash::fallFor(s, 6, 15).empty(), "and nothing falls on it");
    ok(odsplash::pick(s, 6, 15, 0) == "everyday one", "it draws an everyday line",
       odsplash::pick(s, 6, 15, 0));

    section("a window that does not wrap");
    ok(on(10, 28) == "-",         "28 Oct is the day before Halloween");
    ok(on(10, 29) == "halloween", "29 Oct is its first day");
    ok(on(10, 31) == "halloween", "31 Oct is Halloween itself");
    ok(on(11,  1) == "halloween", "1 Nov is its last day");
    ok(on(11,  2) == "-",         "2 Nov is the day after");

    section("a window that wraps the year");
    // The one the obvious implementation gets wrong on all four days.
    ok(on(12, 29) == "-",       "29 Dec is the day before");
    ok(on(12, 30) == "newyear", "30 Dec is its first day");
    ok(on(12, 31) == "newyear", "31 Dec is inside it");
    ok(on(1,   1) == "newyear", "1 JANUARY is inside it, on the far side of the wrap");
    ok(on(1,   2) == "newyear", "2 Jan is its last day");
    ok(on(1,   3) == "-",       "3 Jan is the day after");

    section("a window of one day");
    ok(on(8, 1) == "-",          "1 Aug is the day before");
    ok(on(8, 2) == "firstlight", "2 Aug is the day the game first went live");
    ok(on(8, 3) == "-",          "3 Aug is the day after");

    section("an occasion replaces the everyday lines, it does not join them");
    // A birthday that says "Try not to annex Belgium" three times in four is
    // not a birthday. Every seed must give the occasion's own line.
    bool always = true;
    for (uint32_t seed = 0; seed < 64; ++seed)
        if (odsplash::pick(s, 8, 2, seed) != "it went live today") always = false;
    ok(always, "every seed on 2 August draws the occasion's line");
    ok(odsplash::fallFor(s, 8, 2) == "confetti", "and confetti falls",
       odsplash::fallFor(s, 8, 2));
    ok(odsplash::fallFor(s, 12, 31) == "snow", "snow at new year");
    ok(odsplash::fallFor(s, 10, 31) == "bats", "bats at Halloween");

    section("the same seed gives the same line");
    // It is drawn every frame. A line that re-rolled per frame would strobe.
    ok(odsplash::pick(s, 6, 15, 12345) == odsplash::pick(s, 6, 15, 12345),
       "picking twice with one seed gives one answer");
    ok(odsplash::pick(s, 6, 15, 0) != odsplash::pick(s, 6, 15, 1),
       "and different seeds can differ");

    section("nothing to say");
    odsplash::Splashes empty;
    ok(odsplash::pick(empty, 6, 15, 0).empty(), "an empty file draws nothing");
    ok(odsplash::fallFor(empty, 12, 31).empty(), "and rains nothing");
    odsplash::Splashes noLines = build();
    noLines.occasions[0].lines.clear();
    ok(odsplash::pick(noLines, 10, 31, 0) == "everyday one",
       "an occasion with no lines falls back to the everyday ones",
       odsplash::pick(noLines, 10, 31, 0));

    section("a date that does not exist");
    // 30 February cannot be reached by a real clock, but a hand-edited
    // splashes.json can ask for it. It must simply never match.
    odsplash::Occasion impossible{"nope", "", 2, 30, 2, 30, {"x"}};
    ok(!odsplash::inWindow(impossible, 2, 28), "30 February matches nothing");
    ok(!odsplash::inWindow(impossible, 3, 1),  "and does not leak into March");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed == 0 ? 0 : 1;
}
