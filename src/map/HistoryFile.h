#pragma once

// What a country actually did, and when.
//
// Stored as history.json inside the .odmap, beside policies.json and the rest,
// because a scenario's history belongs to that scenario: 1914 and 1939 are not
// the same world and must not share a script. A map with no history.json plays
// exactly as it always has.
//
// This does NOT tell the AI what to do. It leans on choices the AI is already
// weighing -- see AISystem::attackCandidates, whose `margin` is the resolver's
// own winnability score. A doctrine scales that score; it never adds a target
// the rules would refuse, and never removes one. Masking an action off a trained
// policy has cost us rating every time it has been tried, and a country that has
// been cornered into an ahistorical war should still be able to fight it.
//
// EVERY FIELD IS OPTIONAL, and an unparseable file is not an error -- it is a
// map that gets the behaviour it had before this existed.

#include <string>
#include <unordered_map>
#include <vector>

namespace history {

/// One country's outlook over one stretch of time.
struct Window {
    int fromYear = -100000;   ///< inclusive; absent means "since the beginning"
    int toYear   =  100000;   ///< inclusive; absent means "for ever after"

    /// ISO A3 codes this country pushed into while the window is open. Their
    /// winnability score is multiplied by `press`.
    std::vector<std::string> expand;
    /// ISO A3 codes it did not attack in this period -- a friend, a patron, or
    /// somebody it was not yet ready for. Multiplied by `avoid`.
    std::vector<std::string> restrain;

    float press   = 1.6f;   ///< > 1 makes an expansion target more attractive
    float avoid   = 0.35f;  ///< < 1 makes a restrained target less attractive
    std::string note;       ///< free text, for the map author and the debug view
};

/// iso A3 -> the windows that country has. Countries absent from the file are
/// untouched, which is what lets a map script one power and leave the rest.
using Doctrines = std::unordered_map<std::string, std::vector<Window>>;

/// Parse the contents of a history.json. Returns false if the document could
/// not be read at all; `out` is then left empty.
bool parse(const std::string& json, Doctrines& out);

/// The year in a stored map date -- "August 1940 AD", the string the save and
/// the .odmap actually carry. BC comes back negative.
///
/// Prose a scenario invented ("Spring of the Third Age") gives kNoYear, which
/// is INT_MIN+1 and therefore earlier than the start of any window a map could
/// write -- so such a map simply has no doctrine open, ever. That is the whole
/// mechanism; there is no separate check for it.
constexpr int kNoYear = -2147483647;
int yearOf(const std::string& mapDate);

/// The multiplier `attacker` applies to a war against `defender` in `year`.
/// Exactly 1.0 when there is no doctrine, no open window, or nothing to say --
/// which is what makes the whole feature inert on a map without a history.json.
float pressure(const Doctrines& d, const std::string& attacker,
               const std::string& defender, int year);

}  // namespace history
