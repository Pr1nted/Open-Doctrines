#include "Palette.h"

namespace odPalette {
namespace {

Mode s_mode = Mode::Off;

struct Set {
    Color self, war, alliance, guarantee, nonAgg, neutral;
};

// ─── THE PALETTES ─────────────────────────────────────────────────────────
//
// Each was found by searching the colour space for the assignment whose
// CLOSEST pair, after simulating that deficiency, is as far apart as possible.
// tools/check_palette.py runs the same simulation and will fail the build's
// check if any of these regresses below the floor.
//
// Two things are held fixed across all of them, so a player switching modes is
// not relearning the map: WAR IS RED and NEUTRAL IS THE SAME GREY. Neutral
// covers four fifths of the world and is the ABSENCE of a relationship, not
// one of them, so it stays quiet in every mode.
//
// Green appears nowhere except the ordinary palette, because green is exactly
// what a red-green eye cannot hold apart from red.

const Set kOff = {
    {  0, 100, 255, 255},   // self       blue
    {255,  50,  50, 255},   // war        red
    { 50, 200,  50, 255},   // alliance   green
    {255, 255,  50, 255},   // guarantee  yellow
    {255, 165,   0, 255},   // non-agg    orange
    { 80,  80,  80, 255},   // neutral    grey
};

// dE 58.3 under deuteranopia. Alliance is near-white and non-aggression a
// lavender: both survive the red-green collapse that turns the original
// palette's war, alliance and non-aggression into three shades of olive.
const Set kDeutan = {
    { 85,  51, 255, 255},   // self       blue
    {221,  34,  51, 255},   // war        red
    {255, 255, 238, 255},   // alliance   near-white
    {255, 255,   0, 255},   // guarantee  yellow
    {187, 153, 255, 255},   // non-agg    lavender
    { 80,  80,  80, 255},   // neutral    grey
};

// dE 58.7 under protanopia, which dims reds further than deuteranopia does,
// so war goes to the most saturated vermilion available.
const Set kProtan = {
    { 51,  85, 255, 255},   // self       blue
    {255,  51,   0, 255},   // war        vermilion
    {  0, 255, 204, 255},   // alliance   cyan
    {221, 255,   0, 255},   // guarantee  yellow-green
    { 51, 187, 255, 255},   // non-agg    light blue
    { 80,  80,  80, 255},   // neutral    grey
};

// dE 39.5 under tritanopia -- lower than the other two because the blue-yellow
// axis is the one this deficiency takes away, and the map needs both a blue
// for "you" and a yellow for "guarantee". Still twice the floor.
const Set kTritan = {
    {  0, 136, 221, 255},   // self       blue
    {255,  34,  34, 255},   // war        red
    {238, 170,  85, 255},   // alliance   amber
    {221, 255,  68, 255},   // guarantee  yellow-green
    {170,  51,  85, 255},   // non-agg    maroon
    { 80,  80,  80, 255},   // neutral    grey
};

const Set& active() {
    switch (s_mode) {
        case Mode::Deuteranopia: return kDeutan;
        case Mode::Protanopia:   return kProtan;
        case Mode::Tritanopia:   return kTritan;
        default:                 return kOff;
    }
}

}  // namespace

Mode mode() { return s_mode; }
void setMode(Mode m) { s_mode = m; }

Color of(Role r) {
    const Set& s = active();
    switch (r) {
        case Role::Self:          return s.self;
        case Role::War:           return s.war;
        case Role::Alliance:      return s.alliance;
        case Role::Guarantee:     return s.guarantee;
        case Role::NonAggression: return s.nonAgg;
        case Role::Neutral:       return s.neutral;
        // The panels borrow the map's vocabulary rather than inventing a
        // second one: a negative number is the same red as a war, a positive
        // the same colour as an alliance.
        case Role::Good:          return s.alliance;
        case Role::Bad:           return s.war;
        case Role::Warning:       return s.guarantee;
        default:                  return Color{255, 255, 255, 255};
    }
}

Color relation(Rel r) {
    switch (r) {
        case Rel::Self:          return of(Role::Self);
        case Rel::War:           return of(Role::War);
        case Rel::Alliance:      return of(Role::Alliance);
        case Rel::Guarantee:     return of(Role::Guarantee);
        case Rel::NonAggression: return of(Role::NonAggression);
        case Rel::Neutral:       return of(Role::Neutral);
        default:                 return Color{255, 255, 255, 255};
    }
}

}  // namespace odPalette
