#pragma once
#include "raylib.h"

/**
 * The colours that MEAN something, in one place, with a palette per kind of
 * colour blindness.
 *
 * THE PROBLEM. The relations view paints every country by how it stands with
 * the one you have selected -- war, alliance, guarantee, non-aggression -- and
 * on the map that is the ONLY channel carrying it. The legend has words and
 * the country panel has words; the map has hues and nothing else.
 *
 * Those hues were red against green, which is the pair roughly one man in
 * twelve cannot separate, and the two it is worst to confuse. Measured with
 * tools/check_palette.py, war and alliance sat 11.5 apart under deuteranopia
 * on a scale where 20 is the floor for "obviously different".
 *
 * WHY A MODE AND NOT A SWITCH. One palette can be safe for all three
 * deficiencies at once -- an earlier version scored 32.4 that way -- but a
 * palette that only has to work for ONE of them can spread much further:
 * 58.3 for deuteranopia, 58.7 for protanopia, 39.5 for tritanopia. A player
 * knows which kind they have, and the difference between 32 and 58 is the
 * difference between "distinguishable" and "obvious".
 *
 * Every palette here was chosen by search rather than by eye, because the
 * author's eye is not the one having trouble. The numbers are in the tool.
 *
 * THE OTHER PROBLEM, which is why this file exists at all: the four relation
 * colours were written out as literals at twenty-one call sites across two
 * files. A palette that lives in twenty-one places cannot be swapped.
 */
namespace odPalette {

/// Which palette is in force. Ordered as the settings row cycles them, and
/// stored in the config as this integer, so do not renumber.
enum class Mode { Off = 0, Deuteranopia = 1, Protanopia = 2, Tritanopia = 3 };

/// Every colour in the game that carries meaning rather than decoration.
///
/// Good/Bad/Warning are the same three colours the map uses for alliance, war
/// and guarantee. That is deliberate: a player learns the palette once, and a
/// negative number reads as the same red as a war.
enum class Role {
    None,
    Self, War, Alliance, Guarantee, NonAggression, Neutral,
    Good,        // a positive number, a completed research, a mod that loaded
    Bad,         // a negative number, a failure, something locked
    Warning,     // needs attention but is not yet a failure
};

Mode mode();
void setMode(Mode m);
inline void setMode(int m) { setMode(static_cast<Mode>(m)); }

/// True when any colour-blind palette is active.
inline bool safe() { return mode() != Mode::Off; }

Color of(Role r);

// The relations view still reads better with its own vocabulary.
enum class Rel { None, Self, War, Alliance, Guarantee, NonAggression, Neutral };
Color relation(Rel r);

}  // namespace odPalette
