#pragma once
#include <string>

// Pluggable, LOCALISED calendar/era display for the game date.
//
// The game stores its date as "<Month> <Year> <AD|BC>" and advances it one
// month per turn (src/Game_TurnLogic.cpp). This module turns that into whatever
// dating system the player chose, written in the script of the interface's
// active language.
//
// STILL DEPENDENCY-FREE. No raylib, no i18n link -- the word-for-word
// translation of a label is INJECTED as a function pointer (TrFn), so the pure
// arithmetic and the script tables can be pinned by a unit test that links only
// this .cpp (tests/date_systems_test.cpp). The render layer passes
// od::i18n::tr; the test passes nullptr and gets the English/romanised base.
//
// WHY A SCRIPT PER LANGUAGE, not one label for everybody. The sexagenary year
// is 庚辰 in Chinese, 경진 in Korean, Canh Thìn in Vietnamese and "gengchen"
// romanised for a Latin reader. Each native script is only ever drawn while its
// own language's interface is active -- which is exactly when the font atlas
// already carries that script -- so this never asks the atlas for a glyph it
// does not have. Latin romanisation (plain ASCII, no tone marks) is the base
// and renders in every language.
//
// THE CANONICAL YEAR is the ASTRONOMICAL one (1 BC = 0, 2 BC = -1, 1 AD = 1):
// the stored string has no year zero, astronomicalYear() bridges to the axis
// where the epoch arithmetic (AUC, Kōki, Holocene, the sexagenary cycle) comes
// out right across the BC/AD boundary.

namespace od {

enum class DateSystem {
    BC_AD = 0,   // 50 BC / 44 AD
    BCE_CE,      // 50 BCE / 44 CE
    AUC,         // 704 AUC            -- Ab urbe condita, from 753 BC
    CHINESE,     // 庚辰年 / Gengchen    -- the 60-year sexagenary (ganzhi) cycle
    JAPANESE,    // 皇紀2660           -- Kōki, the imperial year, from 660 BC
    JUCHE,       // 주체 89            -- North Korean, from 1912 AD
    HEBREW,      // 3711 AM           -- Anno Mundi
    HOLOCENE,    // 9951 HE           -- the Holocene / Human Era
    COUNT
};

// The script a system's native label is written in. Chosen from the active
// language, never from the system, so a native script is only drawn when its
// own atlas is loaded. Latin is the romanised base for every other language.
enum class Script { Latin = 0, Han, Hangul, Vietnamese };

// Map an interface language code ("zh", "ja", "ko", "vi", ...) to the script
// its sexagenary (and other CJK) labels should use. Everything not CJK/vi is
// Latin.
Script scriptForLanguage(const std::string& lang);

struct ParsedDate {
    bool valid = false;        // false => not the canonical "<Month> <Year> <AD|BC>" shape
    std::string month;         // the month/season word exactly as written
    long long year = 0;        // the year as written (positive; the era carries the sign)
    bool isBC = false;
};

ParsedDate parseMapDate(const std::string& raw);
long long astronomicalYear(const ParsedDate& d);   // 1 BC -> 0, 1 AD -> 1

// Translate a label KEY ("AUC", "Kōki", "BCE", ...) to the active language.
// A render caller passes a thin wrapper over od::i18n::tr; the test passes
// nullptr, which means "identity": the key is its own English label.
using TrFn = std::string (*)(const char* key);

// The year-and-era field for a system, localised: tr() supplies the label
// words, `script` the sexagenary characters. e.g. "50 BC", "704 AUC",
// "庚辰年"/"Gengchen", "皇紀2660"/"Kōki 2660", "주체 89"/"Juche 89", "9951 HE".
// For JUCHE before its 1912 epoch this is "—". With tr == nullptr and
// script == Latin it is the plain English/romanised base the test pins.
std::string yearField(const ParsedDate& d, DateSystem sys,
                      Script script = Script::Latin, TrFn tr = nullptr);

// The name of the system for the settings menu, as an English tr() KEY. The
// settings code translates it. (A switch of literal tr() calls in Game.cpp is
// what makes these eight keys visible to tools/i18n_extract.py.)
const char* systemName(DateSystem sys);

// Stable token for config storage ("bc_ad", "auc", ...). The enum value is NOT
// written to config so the list can be reordered later without breaking saves.
const char* systemToken(DateSystem sys);
DateSystem systemFromToken(const std::string& token);  // unknown -> BC_AD

}  // namespace od
