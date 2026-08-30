#include "i18n/Locale.h"
#include "util/LoadLog.h"
#include "i18n/Normalize.h"

// Defined in Text.cpp; toggles the HarfBuzz Arabic path for Urdu.
namespace odText { void setComplexArabic(bool on); }

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace od::i18n {
namespace {

// English first, then the rest in the order they were asked for. The endonym
// is what the language calls itself: a picker that lists "German" to somebody
// looking for Deutsch is a picker for people who already read English.
const std::vector<Language> kLanguages = {
    {"en", "English",      "English",    "GBR"},
    {"uk", "Українська",   "Ukrainian",  "UKR"},
    {"be", "Беларуская",   "Belarusian", "BLR"},
    {"kk", "Қазақша",      "Kazakh",     "KAZ"},
    {"ja", "日本語",         "Japanese",   "JPN"},
    {"zh", "中文",           "Chinese",    "CHN"},
    {"de", "Deutsch",      "German",     "DEU"},
    {"it", "Italiano",     "Italian",    "ITA"},
    {"fr", "Français",     "French",     "FRA"},
    {"es", "Español",      "Spanish",    "ESP"},
    {"cs", "Čeština",      "Czech",      "CZE"},
    {"sl", "Slovenščina",  "Slovenian",  "SVN"},
    {"sk", "Slovenčina",   "Slovak",     "SVK"},
    {"pl", "Polski",       "Polish",     "POL"},
    {"af", "Afrikaans",    "Afrikaans",  "ZAF"},
    {"ar", "العربية",        "Arabic",     "SAU"},
    {"hi", "हिन्दी",          "Hindi",      "IND"},
    {"ko", "한국어",         "Korean",     "KOR"},
    {"bg", "Български",     "Bulgarian",  "BGR"},
    {"tr", "Türkçe",       "Turkish",    "TUR"},
    {"ur", "اردو",           "Urdu",       "PAK"},
};

// THE ARENA NEVER GIVES ANYTHING BACK.
//
// tr() hands out `const char*` because the nine hundred drawing sites take one
// and none of them own it. Freeing a table on a language change would leave
// every pointer taken before the change dangling -- including ones held in
// static locals and in structures built at startup. The strings are kept
// instead. A language change costs a few tens of kilobytes and the game has no
// way to leak more than fourteen of them.
std::deque<std::string> g_arena;
std::unordered_map<std::string, const char*> g_table;
std::unordered_map<std::string, std::string> g_names;
/// Answers already worked out, for the language currently loaded. Cleared with it.
std::unordered_map<std::string, std::string> g_nameCache;
// The table's VALUES, for isTranslation(). Built on demand and dropped on a
// language change, so a build that never audits never pays for it.
std::unordered_set<std::string>* g_values = nullptr;
std::vector<int> g_glyphs;
// The subset of the above that this language's OWN text needs. See
// contentGlyphs() in Locale.h for why the two are not the same thing.
std::vector<int> g_contentGlyphs;
std::string g_code = "en";
int g_translated = 0, g_total = 0;

const char* keep(const std::string& s) {
    g_arena.push_back(s);
    return g_arena.back().c_str();
}

// ─── UTF-8 ──────────────────────────────────────────────────────────────────

void appendUtf8(std::string& out, unsigned int cp) {
    if (cp < 0x80) { out += (char)cp; }
    else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

void collectCodepoints(const std::string& s, std::unordered_set<int>& into) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        int cp = c, len = 1;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        if (i + len > s.size()) break;
        for (int k = 1; k < len; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        into.insert(cp);
        i += len;
    }
}

// ─── transliteration ────────────────────────────────────────────────────────
//
// For invented names only -- see properName(). The job is not to be a phonetic
// authority, it is to stop a Ukrainian map of "Brelland" and "Yarlgard" from
// being the one part of the screen the player cannot read.

struct Rule { const char* latin; const char* out; };

// Digraphs before single letters, longest first: "sh" has to be tried before
// "s" or every "sh" becomes "сх".
const Rule kCyrillic[] = {
    {"shch","щ"}, {"sch","щ"}, {"sh","ш"}, {"ch","ч"}, {"zh","ж"}, {"kh","х"},
    {"ts","ц"}, {"ya","я"}, {"yu","ю"}, {"yo","йо"}, {"ye","є"}, {"yi","ї"},
    {"th","т"}, {"ph","ф"}, {"ck","к"}, {"qu","кв"}, {"ee","і"}, {"oo","у"},
    {"a","а"}, {"b","б"}, {"c","к"}, {"d","д"}, {"e","е"}, {"f","ф"},
    {"g","г"}, {"h","г"}, {"i","і"}, {"j","й"}, {"k","к"}, {"l","л"},
    {"m","м"}, {"n","н"}, {"o","о"}, {"p","п"}, {"q","к"}, {"r","р"},
    {"s","с"}, {"t","т"}, {"u","у"}, {"v","в"}, {"w","в"}, {"x","кс"},
    {"y","и"}, {"z","з"},
};

// Belarusian and Kazakh spell two of these differently enough to notice.
std::string cyrillicTweak(const std::string& s, const std::string& code) {
    if (code == "be") {
        // Belarusian has no и; і does that work, and г is the h sound.
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (i + 1 < s.size() && s[i] == '\xd0' && s[i + 1] == '\xb8') {  // и
                out += "\xd1\x96";  // і
                ++i;
                continue;
            }
            out += s[i];
        }
        return out;
    }
    if (code == "kk") {
        // Kazakh writes і for the short front vowel and и only in loans.
        return s;
    }
    return s;
}

std::string toCyrillic(const std::string& in, const std::string& code) {
    std::string lower;
    for (char c : in) lower += (char)std::tolower((unsigned char)c);
    std::string out;
    size_t i = 0;
    bool startOfWord = true;
    while (i < lower.size()) {
        if (!std::isalpha((unsigned char)lower[i])) {
            out += lower[i++];
            startOfWord = true;
            continue;
        }
        bool matched = false;
        for (const Rule& r : kCyrillic) {
            const size_t n = std::char_traits<char>::length(r.latin);
            if (lower.compare(i, n, r.latin) != 0) continue;
            std::string piece = r.out;
            if (startOfWord) {
                // Capitalise the first letter of every word, which for the
                // two-byte Cyrillic block is the low byte minus 0x20.
                if (piece.size() >= 2 && (unsigned char)piece[0] == 0xD0 &&
                    (unsigned char)piece[1] >= 0xB0 && (unsigned char)piece[1] <= 0xBF)
                    piece[1] = (char)((unsigned char)piece[1] - 0x20);
                else if (piece.size() >= 2 && (unsigned char)piece[0] == 0xD1 &&
                         (unsigned char)piece[1] >= 0x80 && (unsigned char)piece[1] <= 0x8F) {
                    piece[0] = (char)0xD0;
                    piece[1] = (char)((unsigned char)piece[1] + 0x20);
                }
                startOfWord = false;
            }
            out += piece;
            i += n;
            matched = true;
            break;
        }
        if (!matched) { out += lower[i++]; startOfWord = false; }
    }
    return cyrillicTweak(out, code);
}

// Katakana, which is where a foreign name goes in Japanese. Built a syllable
// at a time: a consonant that is not followed by a vowel takes a u (or o after
// t and d), which is what makes "Brelland" ブレルランド rather than a row of
// isolated consonants.
const char* kKana[5][14] = {
    //        -      k      s      t      n      h      m      y      r      w      g      z      d      b
    /* a */ {"ア", "カ", "サ", "タ", "ナ", "ハ", "マ", "ヤ", "ラ", "ワ", "ガ", "ザ", "ダ", "バ"},
    /* i */ {"イ", "キ", "シ", "チ", "ニ", "ヒ", "ミ", "イ", "リ", "ウィ", "ギ", "ジ", "ヂ", "ビ"},
    /* u */ {"ウ", "ク", "ス", "ツ", "ヌ", "フ", "ム", "ユ", "ル", "ウ", "グ", "ズ", "ヅ", "ブ"},
    /* e */ {"エ", "ケ", "セ", "テ", "ネ", "ヘ", "メ", "イェ", "レ", "ウェ", "ゲ", "ゼ", "デ", "ベ"},
    /* o */ {"オ", "コ", "ソ", "ト", "ノ", "ホ", "モ", "ヨ", "ロ", "ヲ", "ゴ", "ゾ", "ド", "ボ"},
};

int consonantColumn(char c) {
    switch (c) {
        case 'k': case 'c': case 'q': return 1;
        case 's': return 2;
        case 't': return 3;
        case 'n': return 4;
        case 'h': case 'f': return 5;
        case 'm': return 6;
        case 'y': case 'j': return 7;
        case 'r': case 'l': return 8;
        case 'w': case 'v': return 9;
        case 'g': return 10;
        case 'z': return 11;
        case 'd': return 12;
        case 'b': case 'p': return 13;
        default: return -1;
    }
}

int vowelRow(char c) {
    switch (c) {
        case 'a': return 0;
        case 'i': return 1;
        case 'u': return 2;
        case 'e': return 3;
        case 'o': return 4;
        default: return -1;
    }
}

std::string toKatakana(const std::string& in) {
    std::string lower;
    for (char c : in) lower += (char)std::tolower((unsigned char)c);
    std::string out;
    size_t i = 0;
    while (i < lower.size()) {
        const char c = lower[i];
        if (!std::isalpha((unsigned char)c)) { out += c; ++i; continue; }
        const int v = vowelRow(c);
        if (v >= 0) { out += kKana[v][0]; ++i; continue; }
        const int col = consonantColumn(c);
        if (col < 0) { ++i; continue; }
        // A doubled STOP is the small tsu -- "Falkk" is ファルック. A doubled
        // liquid is not: the "ll" in Brelland is one l sound, and treating it
        // as gemination gave ブレッランド where every atlas and every map of
        // the world writes ブレルランド. So the rule is the consonant, not the
        // doubling.
        const bool stop = (c == 'k' || c == 't' || c == 'p' || c == 's' || c == 'c' ||
                           c == 'g' || c == 'd' || c == 'b' || c == 'f' || c == 'z');
        if (stop && i + 1 < lower.size() && lower[i + 1] == c) { out += "ッ"; ++i; continue; }
        // n with no vowel after it is the standalone ン.
        if (c == 'n' && (i + 1 >= lower.size() || vowelRow(lower[i + 1]) < 0)) {
            out += "ン";
            ++i;
            continue;
        }
        const int nv = (i + 1 < lower.size()) ? vowelRow(lower[i + 1]) : -1;
        if (nv >= 0) { out += kKana[nv][col]; i += 2; }
        else {
            // No vowel: t and d take o, everything else takes u.
            const int filler = (c == 't' || c == 'd') ? 4 : 2;
            out += kKana[filler][col];
            ++i;
        }
    }
    return out;
}

// Chinese takes a name by sound too, one character per syllable, from the
// small end of the standard transliteration set. A syllable that is not in
// here is left in Latin rather than guessed at: a wrong character is a word,
// and a wrong word on a map is worse than an untranslated one.
const std::unordered_map<std::string, const char*>& hanSyllables() {
    static const std::unordered_map<std::string, const char*> m = {
        {"a","阿"},{"ba","巴"},{"bo","博"},{"bu","布"},{"bi","比"},{"be","贝"},
        {"da","达"},{"de","德"},{"di","迪"},{"do","多"},{"du","杜"},
        {"fa","法"},{"fe","费"},{"fi","菲"},{"fo","福"},{"fu","富"},
        {"ga","加"},{"ge","格"},{"gi","吉"},{"go","戈"},{"gu","古"},
        {"ha","哈"},{"he","赫"},{"hi","希"},{"ho","霍"},{"hu","胡"},
        {"ka","卡"},{"ke","凯"},{"ki","基"},{"ko","科"},{"ku","库"},
        {"la","拉"},{"le","勒"},{"li","利"},{"lo","洛"},{"lu","卢"},
        {"ma","马"},{"me","梅"},{"mi","米"},{"mo","莫"},{"mu","穆"},
        {"na","纳"},{"ne","内"},{"ni","尼"},{"no","诺"},{"nu","努"},
        {"pa","帕"},{"pe","佩"},{"pi","皮"},{"po","波"},{"pu","普"},
        {"ra","拉"},{"re","雷"},{"ri","里"},{"ro","罗"},{"ru","鲁"},
        {"sa","萨"},{"se","塞"},{"si","西"},{"so","索"},{"su","苏"},
        {"ta","塔"},{"te","特"},{"ti","蒂"},{"to","托"},{"tu","图"},
        {"va","瓦"},{"ve","韦"},{"vi","维"},{"vo","沃"},{"vu","武"},
        {"wa","瓦"},{"we","韦"},{"wi","威"},{"wo","沃"},
        {"ya","亚"},{"ye","耶"},{"yi","伊"},{"yo","约"},{"yu","尤"},
        {"za","扎"},{"ze","泽"},{"zi","齐"},{"zo","佐"},{"zu","祖"},
        {"n","恩"},{"m","姆"},{"r","尔"},{"l","尔"},{"s","斯"},{"t","特"},
        {"d","德"},{"k","克"},{"g","格"},{"h","赫"},{"e","埃"},{"i","伊"},
        {"o","奥"},{"u","乌"},
    };
    return m;
}

std::string toHan(const std::string& in) {
    std::string lower;
    for (char c : in) lower += (char)std::tolower((unsigned char)c);
    const auto& tbl = hanSyllables();
    std::string out;
    size_t i = 0;
    while (i < lower.size()) {
        if (!std::isalpha((unsigned char)lower[i])) { out += lower[i++]; continue; }
        // Consonant plus vowel where there is one, else the bare letter.
        std::string syl;
        if (i + 1 < lower.size() && vowelRow(lower[i + 1]) >= 0 && vowelRow(lower[i]) < 0)
            syl = lower.substr(i, 2);
        else
            syl = lower.substr(i, 1);
        auto it = tbl.find(syl);
        if (it != tbl.end()) { out += it->second; i += syl.size(); continue; }
        it = tbl.find(lower.substr(i, 1));
        if (it != tbl.end()) { out += it->second; ++i; continue; }
        out += lower[i++];
    }
    return out;
}

// ─── generated country names ────────────────────────────────────────────────
//
// A breakaway state names itself at the moment it secedes, out of English
// pieces: a political form picked from its economic and social compass, maybe
// a direction, and a root taken from a minority or a region. The assembled
// string is what gets stored, and storing English is right -- it is the ISO
// code's source, it goes over the wire in multiplayer, and it must mean the
// same thing in every player's save.
//
// But it must not be TRANSLITERATED as one lump. "Republic of Kurdistan" run
// through the Cyrillic table comes out "Ріпаблік оф Курдистан", which is the
// sound of an English sentence spelled in Cyrillic and means nothing to
// anybody. The form is a word with a translation; only the root is invented.
//
// So the pieces are peeled off, translated as whole patterns, and the root --
// and only the root -- is transliterated. The pattern carries the placeholder,
// which is what lets a language put the form where it belongs: Japanese writes
// the republic AFTER the name, and "%s共和国" says so.
// ONLY THE PATTERN IS WRITTEN DOWN. The English prefix this matches is the
// pattern with its placeholder taken off, so the two cannot drift apart -- and
// a flat table of literals is something tools/i18n_extract.py can read, which
// a table of structs was not.
// EVERY FORM THE GAME CAN INVENT, not the four it used to list.
//
// Two places build country names at runtime -- the breakaway namer in
// Game_TurnLogic.cpp and applyName() in PoliticalIdentity.cpp, which renames a
// country when its politics move far enough. Between them they produce
// twenty-four "<form> of <place>" phrases, and only four were listed here. The
// rest fell through to the transliterator and came out as an English sentence
// spelled in the local alphabet: "Socialist Union of Brelland" was drawn on
// the Ukrainian map as "Сокіаліст Уніон Оф Брелланд" and on the Japanese one
// as "ソキアリスト ウニオン オフ ブレルランド", which is not a translation of
// anything -- it is the sound of the English words.
//
// Order does not matter: each is matched from the start of the name, and no
// form is a prefix of another.
const char* kForms[] = {
    "People's Republic of %s",
    "People's Federation of %s",
    "People's Commune of %s",
    "Socialist Republic of %s",
    "Socialist Union of %s",
    "Workers' Republic of %s",
    "Democratic Alliance of %s",
    "Merchant Republic of %s",
    "National State of %s",
    "Free Republic of %s",
    "Free Communes of %s",
    "Free Territories of %s",
    "Free Market of %s",
    "Free State of %s",
    "Commonwealth of %s",
    "Directorate of %s",
    "Cooperative of %s",
    "Autocracy of %s",
    "Dominion of %s",
    "Federation of %s",
    "Republic of %s",
    "Empire of %s",
    "State of %s",
    "Union of %s",
};

// THE SAME FORMS, BEHIND THE NAME.
//
// A country whose core is an adjective takes the form as a suffix -- "Slovak
// Republic", never "Republic of Slovak" -- and the breakaway namer has a
// suffix branch of its own. Most languages will translate these exactly as
// they translate the "of" forms above; the pattern is separate because some
// will not, and because the English text has to be matched either way.
//
// LONGEST FIRST, and this one does matter: "Brelland People's Republic" ends
// with " Republic" as well, and taking the shorter match leaves "People's"
// stranded in the root to be transliterated on its own.
const char* kSuffixForms[] = {
    "%s Merchant Republic",
    "%s People's Republic",
    "%s Workers' Republic",
    "%s Free Territories",
    "%s National State",
    "%s Socialist Union",
    "%s Free Communes",
    "%s Free Republic",
    "%s Cooperative",
    "%s Free Market",
    "%s Free State",
    "%s Federation",
    "%s Autocracy",
    "%s Republic",
    "%s Empire",
    "%s State",
    "%s Union",
};

// A NAME THAT IS ONLY WORDS.
//
// The breakaway namer's one-word branch produces states called "The Commune"
// or "Red Star" -- no invented root, nothing to transliterate, and every word
// in them already has a translation. Left to the transliterator, "The Junta"
// came out "Те Йунта".
const char* kStandalone[] = {
    "The Commune", "The Collective", "The Union", "The Alliance",
    "The Federation", "The Directorate", "The Dominion", "The Imperium",
    "The Authority", "The Junta", "The Council", "The Order",
    "The Republic", "The Commonwealth", "The Concord", "The Free State",
    "The Proletariat", "Red Star", "Soviet",
};

// Both sets, because the generator uses the bare and the -ern forms.
const char* kDirections[] = {
    "Northern %s", "Southern %s", "Eastern %s", "Western %s", "Central %s",
    "North %s", "South %s", "East %s", "West %s",
};

/// The English text a pattern begins with: "Republic of %s" -> "Republic of ".
std::string prefixOf(const char* pattern) {
    const std::string p = pattern;
    const size_t at = p.find("%s");
    return (at == std::string::npos) ? p : p.substr(0, at);
}

/// The English text a pattern ends with: "%s Republic" -> " Republic".
std::string suffixOf(const char* pattern) {
    const std::string p = pattern;
    const size_t at = p.find("%s");
    return (at == std::string::npos) ? std::string() : p.substr(at + 2);
}

/// Put `root` where the pattern's %s is. Appended if a translation lost it,
/// because a name with the form silently dropped is worse than an ugly one.
std::string fill(const std::string& pattern, const std::string& root) {
    const size_t at = pattern.find("%s");
    if (at == std::string::npos) return pattern + " " + root;
    return pattern.substr(0, at) + root + pattern.substr(at + 2);
}

// WHICH LANGUAGES HAVE A TRANSLITERATOR, listed the way round that fails safe.
//
// This was a latinScript() that named the ten Latin languages and let
// EVERYTHING ELSE fall through to toCyrillic(). That is only correct while
// every non-Latin language in the game writes Cyrillic, and it stopped being
// true the moment Japanese was added -- and then quietly went wrong four more
// times. Turkish is written in the Latin alphabet and was not on the list;
// Korean, Arabic, Hindi and Urdu have no transliterator of their own. All five
// took the fallback, so an invented country name was spelled in RUSSIAN
// LETTERS on the Turkish, Korean, Arabic, Hindi and Urdu maps.
//
// So the question asked here is "does this language have a transliterator",
// and a language that does not keeps the name in the Latin alphabet it was
// invented in -- which is what an atlas does with a foreign proper noun, and
// is in every case better than a third script the reader did not ask for.
// Adding a language now defaults to leaving names alone instead of to a bug
// nobody sees until somebody who reads that script plays the game.
bool cyrillicScript(const std::string& code) {
    return code == "uk" || code == "be" || code == "kk" || code == "bg";
}

}  // namespace

const std::vector<Language>& languages() { return kLanguages; }
const std::string& language() { return g_code; }

const Language& current() {
    for (const Language& l : kLanguages)
        if (g_code == l.code) return l;
    return kLanguages[0];
}

bool setLanguage(const std::string& code, const std::string& dataDir) {
    bool known = false;
    for (const Language& l : kLanguages) known = known || (code == l.code);
    if (!known) {
        LoadLog() << "[i18n] no such language \"" << code << "\"" << std::endl;
        return false;
    }

    // The glyph list starts from what the interface always needs, whatever
    // language it is in: Latin for the numbers and the untranslated, Cyrillic
    // because it is cheap, and the punctuation the text is written with.
    std::unordered_set<int> cps;
    odText::CoverageProbe seen;
    for (int c = 32; c <= 255; ++c) cps.insert(c);
    for (int c = 0x100; c <= 0x17F; ++c) cps.insert(c);     // Latin Extended-A
    for (int c = 0x400; c <= 0x4FF; ++c) cps.insert(c);     // Cyrillic
    for (int c : {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0xA9, 0xAE})
        cps.insert(c);

    // EVERY LANGUAGE'S OWN NAME, WHATEVER LANGUAGE WE ARE IN.
    //
    // The picker lists all fourteen at once and writes each in its own script,
    // so those characters are on screen in the English build as much as in the
    // Japanese one. Building the atlas only from the ACTIVE language drew 日本語
    // as three question marks -- to somebody looking for Japanese, the one
    // entry they can read is the one that looks broken.
    bool anyArabic = false, anyDevanagari = false;
    for (const Language& l : kLanguages) {
        collectCodepoints(l.endonym, cps);
        collectCodepoints(l.english, cps);
        for (const char* p = l.endonym; *p; ++p) {
            const unsigned char c = (unsigned char)*p;
            if (c == 0xD8 || c == 0xD9) anyArabic = true;      // U+0600..06FF lead bytes
            if (c == 0xE0) anyDevanagari = true;               // U+0900..097F lead byte
        }
    }
    // THE SHAPED FORMS, NOT JUST THE LETTERS.
    //
    // The picker writes العربية in every language, and the renderer substitutes
    // each letter for its joining form before drawing (see Arabic.cpp). Those
    // forms live in a different block, so collecting the endonym's own
    // codepoints put the BASE letters in the atlas and none of the glyphs that
    // are actually drawn -- and the one entry an Arabic speaker can read came
    // out as seven question marks.
    if (anyArabic)
        for (int c = 0xFE70; c <= 0xFEFF; ++c) cps.insert(c);
    if (anyDevanagari)
        for (int c = 0x0900; c <= 0x097F; ++c) cps.insert(c);

    std::unordered_map<std::string, const char*> table;
    std::unordered_map<std::string, std::string> names;
    int translated = 0, total = 0;

    // WHAT THIS LANGUAGE ITSELF NEEDS, kept apart from what the picker needs.
    //
    // Everything gathered above is decoration in the strict sense: the base
    // Latin/Cyrillic ranges the interface always draws, and every language's
    // endonym, which the picker writes in its own script whatever language we
    // are in. So an ENGLISH atlas legitimately asks for 日本語 and العربية.
    //
    // That is fine for building an atlas and wrong for asking "can the font on
    // this machine serve the language I am reading". Game::reloadFonts() uses
    // that question on the web to decide whether to download the complete
    // 11 MB unifont, and against the merged set the answer was yes for
    // everybody -- English included -- which would have put an 11 MB fetch in
    // front of every visitor for characters in a menu they may never open.
    std::unordered_set<int> content;

    if (code != "en") {
        const std::string path = dataDir + "lang/" + code + ".json";
        std::ifstream f(path);
        if (!f) {
            LoadLog() << "[i18n] cannot open " << path << std::endl;
            return false;
        }
        nlohmann::json j;
        try {
            f >> j;
        } catch (const std::exception& e) {
            LoadLog() << "[i18n] " << path << ": " << e.what() << std::endl;
            return false;
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!it.value().is_string()) continue;
            const std::string v = it.value().get<std::string>();
            ++total;
            // An empty value means "not translated yet", which must fall
            // through to English rather than draw nothing.
            if (v.empty()) continue;
            ++translated;
            table[it.key()] = keep(v);
            collectCodepoints(v, cps);
            collectCodepoints(v, content);
            odText::probe(v, seen);
        }

        // Proper nouns, if this language has been given any.
        const std::string npath = dataDir + "lang/" + code + ".names.json";
        std::ifstream nf(npath);
        if (nf) {
            nlohmann::json nj;
            try {
                nf >> nj;
                for (auto it = nj.begin(); it != nj.end(); ++it)
                    if (it.value().is_string()) {
                        names[it.key()] = it.value().get<std::string>();
                        collectCodepoints(names[it.key()], cps);
                        collectCodepoints(names[it.key()], content);
                        odText::probe(names[it.key()], seen);
                    }
            } catch (const std::exception& e) {
                LoadLog() << "[i18n] " << npath << ": " << e.what() << std::endl;
            }
        }

        // ─── AND THE DIALOGUE, WHICH IS WHERE THE PROSE ACTUALLY IS ───
        //
        // The atlas was built from the language FILE and nothing else, so a
        // character that appears only in a conversation had no glyph and drew
        // as a question mark. Not a rare corner: the interface is labels --
        // "Economy", "Research", "End Turn" -- and the dialogue is sentences,
        // so most of the kanji in the Japanese build appear nowhere but here.
        // Mia's diplomacy lesson came out as 「これは?んだ不具合ではなく規?です」
        // with the two rarest characters missing, on a page explaining that
        // the thing the player is looking at is not a bug.
        //
        // Whole files, directives and all: the ids and keywords in them are
        // ASCII and already covered, and reading nine small text files at a
        // language change costs nothing measurable.
        //
        // Arabic, Urdu and Hindi ask for their whole block below and never
        // needed this. Japanese, Chinese and Korean do, because none of the
        // three can: the blocks are thousands of glyphs and a translation uses
        // a few hundred.
        {
            std::error_code ec;
            const std::filesystem::path dir = dataDir + "dialog/" + code;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (e.path().extension() != ".oddlg") continue;
                std::ifstream df(e.path());
                if (!df) continue;
                std::stringstream ss;
                ss << df.rdbuf();
                const std::string text = ss.str();
                collectCodepoints(text, cps);
                collectCodepoints(text, content);
                odText::probe(text, seen);
            }
            // No directory is not an error: a language with no dialogue of its
            // own falls back to the English scripts, which are ASCII.
        }

        // The scripts a transliterated name will be written in. Asked for as
        // whole blocks rather than as the characters one particular map needs,
        // because the names are generated and the next map will need others.
        if (code == "ja") {
            for (int c = 0x30A0; c <= 0x30FF; ++c) { cps.insert(c); content.insert(c); }
            for (int c = 0x3040; c <= 0x309F; ++c) { cps.insert(c); content.insert(c); }
        }
        if (code == "zh")
            for (const auto& kv : hanSyllables()) {
                collectCodepoints(kv.second, cps);
                collectCodepoints(kv.second, content);
            }
        if (code == "ar") {
            for (int c = 0x0600; c <= 0x06FF; ++c) { cps.insert(c); content.insert(c); }   // Arabic
            // AND THE PRESENTATION FORMS. Arabic letters change shape by
            // position, and the atlas has to carry the shaped glyphs the
            // renderer substitutes in -- not just the base letters, which are
            // the isolated forms and would spell a word as disconnected
            // stumps. See odText::shapeArabic.
            for (int c = 0xFE70; c <= 0xFEFF; ++c) { cps.insert(c); content.insert(c); }
        }
        if (code == "hi")
            for (int c = 0x0900; c <= 0x097F; ++c) { cps.insert(c); content.insert(c); }
        // Korean needs no shaping: modern text is written in precomposed
        // syllable blocks, one glyph each, so the codepoints collected from
        // the language file above are already the glyphs that get drawn. The
        // block is NOT asked for wholesale -- it is eleven thousand glyphs and
        // a translation uses a few hundred of them.
    }

    // Finish the coverage and let it have the last word. normalize() folds in
    // the glyphs the atlas always needs and, in the same pass, reports whether
    // what was loaded is well formed for this code. A language that is not is
    // not switched to -- the previous one stays up, exactly as a missing file
    // would leave it. See i18n/Normalize.cpp for what "well formed" means.
    bool wellFormed = true;
    std::vector<int> glyphs = odText::normalize(code, cps, seen, wellFormed);
    if (!wellFormed) {
        LoadLog() << "[i18n] \"" << code << "\" is not a coverage this build serves"
                  << std::endl;
        return false;
    }

    g_table = std::move(table);
    g_names = std::move(names);
    g_nameCache.clear();   // every answer in it was for the old language
    delete g_values; g_values = nullptr;   // and so was every value in it
    g_code = code;
    g_translated = translated;
    g_total = total;
    g_glyphs = std::move(glyphs);
    g_contentGlyphs.assign(content.begin(), content.end());
    std::sort(g_contentGlyphs.begin(), g_contentGlyphs.end());
    // Urdu needs the shaping renderer; every other language, including Arabic,
    // uses the lighter table path.
    odText::setComplexArabic(code == "ur");
    return true;
}

// ─── A KEY THAT CARRIES A NOTE, AFTER A '|' ────────────────────────────────
//
// English-as-key has one failure mode, and it is not a small one: an English
// word that is two words. "Port" is the harbour in the province panel and the
// TCP port in the multiplayer dialog, and both drew T("Port"). One key cannot
// be both, so whichever meaning a translator chose, the other screen was
// wrong -- and every one of the twenty was wrong somewhere. Japanese drew a
// harbour as ポート, Chinese as 端口, Slovene as Vrata ("door"), Italian as
// Porta ("door" again); the thirteen that read it as a harbour told a player
// hosting a game to type a number into "Пристанище".
//
// So an ambiguous call site names its meaning:
//
//     T("Port|the harbour a ship is built in")
//     T("Port|the network port the host listens on")
//
// The WHOLE string is the key, so the note travels to the language files and
// the two meanings are separate entries. What is DRAWN is only the part before
// the '|' -- in English, and in any language that has not translated it yet.
//
// A '|' is otherwise unused in a drawn string; i18n_sync.py --lint refuses an
// untranslated context key, because the fallback here is the bare English,
// which is exactly the word that is ambiguous.
const char* stripContext(const char* key, const char* bar) {
    static std::unordered_map<std::string, std::string> cache;
    auto it = cache.find(key);
    if (it == cache.end())
        it = cache.emplace(key, std::string(key, (size_t)(bar - key))).first;
    return it->second.c_str();
}

const char* tr(const char* english) {
    if (!english) return english;
    if (!g_table.empty()) {
        auto it = g_table.find(english);
        if (it != g_table.end()) return it->second;
    }
    // Untranslated, or English: draw the text, never the note.
    if (const char* bar = std::strchr(english, '|')) return stripContext(english, bar);
    return english;
}

const char* tr(const std::string& english) { return tr(english.c_str()); }

const std::string& properName(const std::string& name) {
    auto cached = g_nameCache.find(name);
    if (cached != g_nameCache.end()) return cached->second;

    std::string out;
    auto it = g_names.find(name);
    if (it != g_names.end()) {
        out = it->second;
    } else if (name.empty()) {
        out = name;
    } else {
        // A generated state's form and direction come off first and are
        // translated as words; whatever is left is the invented part.
        for (const char* pattern : kForms) {
            const std::string pre = prefixOf(pattern);
            if (name.compare(0, pre.size(), pre) != 0) continue;
            return g_nameCache
                .emplace(name, fill(tr(pattern), properName(name.substr(pre.size()))))
                .first->second;
        }
        for (const char* pattern : kDirections) {
            const std::string pre = prefixOf(pattern);
            if (name.compare(0, pre.size(), pre) != 0) continue;
            return g_nameCache
                .emplace(name, fill(tr(pattern), properName(name.substr(pre.size()))))
                .first->second;
        }
        // BEFORE THE SUFFIXES, because "The Free State" ends in one. Matching
        // the suffix first would leave "The" as the root and transliterate it.
        bool wordsOnly = false;
        for (const char* w : kStandalone) wordsOnly = wordsOnly || (name == w);
        if (wordsOnly)
            return g_nameCache.emplace(name, tr(name)).first->second;
        for (const char* pattern : kSuffixForms) {
            const std::string suf = suffixOf(pattern);
            if (suf.empty() || name.size() <= suf.size()) continue;
            if (name.compare(name.size() - suf.size(), suf.size(), suf) != 0) continue;
            const std::string root = name.substr(0, name.size() - suf.size());
            return g_nameCache
                .emplace(name, fill(tr(pattern), properName(root)))
                .first->second;
        }
        if (cyrillicScript(g_code))   out = toCyrillic(name, g_code);
        else if (g_code == "ja")      out = toKatakana(name);
        else if (g_code == "zh")      out = toHan(name);
        else                          out = name;
    }
    return g_nameCache.emplace(name, std::move(out)).first->second;
}

bool machineTranslated() { return g_code != "en"; }

const char* disclaimer() {
    // One literal, on one line: tools/i18n_extract.py reads the source for the
    // strings to translate and a sentence split across two lines is two keys.
    return tr("Most of this translation was made by a machine and will contain mistakes. The English text is the original.");
}

const std::vector<int>& glyphs() { return g_glyphs; }
const std::vector<int>& contentGlyphs() { return g_contentGlyphs; }

bool isTranslation(const std::string& text) {
    if (!g_values) {
        g_values = new std::unordered_set<std::string>();
        g_values->reserve(g_table.size());
        for (const auto& [key, value] : g_table) g_values->insert(value);
    }
    return g_values->count(text) > 0;
}

void coverage(int& translated, int& total) {
    translated = g_translated;
    total = g_total;
}

}  // namespace od::i18n
