#pragma once

#include <string>
#include <vector>

/**
 * The game in another language.
 *
 * THE ENGLISH TEXT IS THE KEY. There was never a string table here: the words
 * are written where they are drawn, as literals, across nine hundred and
 * seventy draw calls. Re-keying all of those to identifiers would be a
 * thousand-line diff whose only product is a chance to mistype one, so instead
 * `T("New World")` looks "New World" up in the active language and falls back
 * to itself. That has three consequences worth knowing:
 *
 *   * the English build is the identity function, so nothing can regress in
 *     the language the game is written in;
 *   * a missing translation is English rather than a blank or a key, which is
 *     the right failure for a player;
 *   * data/lang/en.json is generated FROM the source by tools/i18n_extract.py,
 *     so the list of things to translate is a consequence of the code rather
 *     than a second thing to keep in step.
 *
 * A returned pointer stays valid for the life of the process even across a
 * language change -- see the arena in Locale.cpp. Callers hold `const char*`
 * from raylib-shaped drawing code all over the game and none of them expect a
 * lifetime.
 */
namespace od::i18n {

/// One offered language. `flagIso` names the file in data/flags.
struct Language {
    const char* code;      ///< "uk", "ja", ... and "en"
    const char* endonym;   ///< what its speakers call it, in it
    const char* english;   ///< what an English speaker calls it
    const char* flagIso;   ///< ISO-3 of the flag drawn beside it
};

/// Every language the game offers, English first.
const std::vector<Language>& languages();

/// The active code. "en" until something says otherwise.
const std::string& language();

/// The active language's own entry.
const Language& current();

/**
 * Load a language. `dataDir` is the usual data root; the file is
 * data/lang/<code>.json. Returns false and keeps the previous language if the
 * file is missing or unreadable -- switching to a language that is not there
 * must not leave the interface half translated.
 *
 * "en" always succeeds and needs no file.
 */
bool setLanguage(const std::string& code, const std::string& dataDir);

/// The translation of `english`, or `english` itself.
const char* tr(const char* english);
const char* tr(const std::string& english);

/**
 * A proper noun -- a country, a province, a generated place.
 *
 * Two steps, in this order. An explicit entry in data/lang/<code>.names.json
 * wins, which is how real places get their real names ("Germany" is Deutschland
 * and not Джермани). Anything left is TRANSLITERATED into the language's
 * script, because the names this game generates are invented -- "Brelland" has
 * no German translation, and a Ukrainian player reading a map of Latin words
 * among Cyrillic ones is reading a half-translated game.
 *
 * Languages written in Latin get their names back unchanged.
 *
 * Returns a REFERENCE into a cache that lives until the language changes, so
 * `properName(c->name).c_str()` is safe to hold for the frame -- which is what
 * every drawing site in this game wants. The cache is also why this can be
 * called per label per frame: transliterating a map's worth of names on every
 * frame would otherwise be real work for an answer that never changes.
 */
const std::string& properName(const std::string& name);

/// True while the active language has anything machine-translated in it.
bool machineTranslated();

/// The warning to show beside the language picker, in the chosen language.
const char* disclaimer();

/**
 * Every codepoint the active language needs drawn.
 *
 * The font atlas is built from a fixed list of ranges -- Latin, Latin
 * Extended-A, Cyrillic -- and a Japanese build would draw a screen of boxes
 * with it. The atlas is rebuilt on a language change from this, which is the
 * union of those ranges and every character actually present in the loaded
 * file, so a language costs exactly the glyphs it uses.
 */
const std::vector<int>& glyphs();

/**
 * The part of glyphs() that comes from THIS language's own text -- its strings,
 * its proper nouns, its dialogue and its script's blocks -- and not from the
 * ranges every build carries or from the other twenty languages' names.
 *
 * Empty for English, whose text is inside the always-present ranges.
 *
 * WHY IT IS SEPARATE. glyphs() deliberately includes every language's endonym,
 * so the picker can write 日本語 to somebody who cannot read the word
 * "Japanese". That makes it the wrong set to ask "can the font here serve the
 * language being read": measured against it, an English session on the web
 * looks 289 glyphs short and would fetch the complete 11 MB unifont before
 * drawing a menu whose every character it already had. See Game::reloadFonts().
 */
const std::vector<int>& contentGlyphs();

/// How many of the strings in en.json the active language has. For the UI.
void coverage(int& translated, int& total);

/**
 * Is `text` something this table PRODUCED, rather than something it was asked
 * about?
 *
 * Only the OD_I18N_AUDIT hook in i18n/Text.cpp needs this, and it needs it
 * because of a path the interface takes all the time: a helper translates a
 * label, measures it, and then draws it -- so the already-German string
 * arrives at the lookup a second time, misses (it is a value, not a key), and
 * looks exactly like something nobody translated. Eighty-four of the audit's
 * first hundred and eighty lines were that, and a report that is half noise
 * does not get read.
 *
 * The value set is built on first use and thrown away on a language change,
 * so a build that never audits never pays for it.
 */
bool isTranslation(const std::string& text);

}  // namespace od::i18n

/**
 * The wrapper every drawing site uses. Short on purpose: it appears about a
 * thousand times, and `od::i18n::tr("Back")` at every one of them would be
 * more punctuation than text.
 *
 * A FUNCTION, NOT A MACRO. It was a macro for about a minute, and json.hpp --
 * which is included in most of the same files -- writes `template <typename T>`
 * some hundreds of times. A macro has no idea what a template parameter is and
 * rewrote every one of them; a function is simply shadowed by the parameter,
 * which is legal, silent and correct.
 */
inline const char* T(const char* english) { return od::i18n::tr(english); }
inline const char* T(const std::string& english) { return od::i18n::tr(english); }
