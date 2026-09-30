#pragma once
// The line beside the title, and what falls past it on a few days of the year.
//
// WHY THIS IS ITS OWN HEADER AND TOUCHES NO RAYLIB
//
// The only part worth getting wrong is the DATE arithmetic -- "is today
// Halloween", "does a window that starts in December and ends in January
// contain the 1st of January" -- and date arithmetic is exactly the kind of
// thing that is written once, looks right, and is wrong for four days a year
// in a place nobody looks. Kept pure, it can be tested at every boundary
// without a window, a clock, or a running game. tests/splash_test.cpp does.
//
// The lines themselves live in data/splashes.json rather than in this file.
// A string literal here would be collected by tools/i18n_extract.py and would
// then want translating into forty-four languages -- and jokes do not survive
// that, nor should a new one block a release on translation work. See the
// comment at the top of that file.
#include <cstdint>
#include <string>
#include <vector>

namespace odsplash {

/** A few days of the year that get their own lines and their own weather. */
struct Occasion {
    std::string id;          // "halloween"
    std::string fall;        // "snow" | "confetti" | "bats" | "" for none
    int fromMonth = 0, fromDay = 0;
    int toMonth = 0, toDay = 0;
    std::vector<std::string> lines;
};

struct Splashes {
    std::vector<std::string> any;
    std::vector<Occasion> occasions;
};

/**
 * Is (month, day) inside this occasion's window, inclusive at both ends?
 *
 * WINDOWS MAY WRAP THE YEAR. New year runs 12-30 to 01-02, so the usual
 * "from <= today <= to" is false on every day of it. When the window wraps,
 * the test is the OTHER way round: today is in it if it is at or after the
 * start, OR at or before the end.
 *
 * Compared as month*100 + day, which orders dates correctly within a year and
 * needs no calendar: no leap years, no month lengths, and 02-30 -- a date that
 * does not exist -- simply never matches rather than doing something strange.
 */
inline bool inWindow(const Occasion& o, int month, int day) {
    const int today = month * 100 + day;
    const int from  = o.fromMonth * 100 + o.fromDay;
    const int to    = o.toMonth * 100 + o.toDay;
    if (from <= to) return today >= from && today <= to;
    return today >= from || today <= to;     // wraps December into January
}

/** The occasion covering (month, day), or nullptr. First match wins. */
inline const Occasion* occasionFor(const Splashes& s, int month, int day) {
    for (const Occasion& o : s.occasions)
        if (inWindow(o, month, day)) return &o;
    return nullptr;
}

/** What should be falling past the title today: "" on an ordinary day. */
inline std::string fallFor(const Splashes& s, int month, int day) {
    const Occasion* o = occasionFor(s, month, day);
    return o ? o->fall : std::string();
}

/**
 * The line to draw. An occasion REPLACES the everyday lines rather than being
 * mixed in with them -- a birthday that says "Try not to annex Belgium" three
 * times out of four is not a birthday.
 *
 * `seed` decides which line, so the caller owns the randomness: the game seeds
 * it from the clock once per launch, and the test passes fixed values and gets
 * the same answer twice. A splash that changed every frame would be unreadable.
 *
 * Empty only when there is genuinely nothing to say -- an empty file.
 */
inline std::string pick(const Splashes& s, int month, int day, uint32_t seed) {
    const Occasion* o = occasionFor(s, month, day);
    const std::vector<std::string>& from =
        (o && !o->lines.empty()) ? o->lines : s.any;
    if (from.empty()) return {};
    return from[seed % (uint32_t)from.size()];
}

}  // namespace odsplash
