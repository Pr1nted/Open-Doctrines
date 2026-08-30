#pragma once
#include "raylib.h"
#include <string>

/**
 * Text that is not English, drawn by code that has never heard of Unicode.
 *
 * THE PROBLEM. Nine hundred and seventy DrawText calls, every one of them
 * against raylib's built-in font, which contains ASCII and nothing else. Point
 * any of them at Українська or 日本語 and the result is a row of boxes. There is
 * no per-site fix that is not nine hundred and seventy edits, and the font a
 * string needs is a property of the string rather than of the call.
 *
 * THE TRICK IS THE ONE UiScale.h ALREADY USES: shadow raylib's own functions
 * here and decide in one place.
 *
 *   DrawText      draws through the Unicode atlas when the string needs it
 *   MeasureText   measures the same way, so layout and drawing agree
 *
 * ASCII IS THE FAST PATH AND IS UNTOUCHED. A string with no byte above 127 --
 * which is every string in the English build, every number, every filename --
 * goes straight to raylib and is drawn exactly as it was before this file
 * existed. So the language the game is written in cannot regress: there is no
 * new code on its path at all.
 *
 * Anything else is drawn glyph by glyph out of the atlas built from the active
 * language (see od::i18n::glyphs), with one advance calculation shared between
 * drawing and measuring -- the older drawHybridText has its own, a pixel wider
 * per glyph than MeasureText's, which is exactly the kind of drift that puts a
 * translated label half over the button beside it.
 */
namespace odText {

/// The atlas non-ASCII is drawn from. Called at startup and on a language change.
void setFont(Font unicode);
// Route Arabic-script text through the HarfBuzz shaper (Urdu). Off by default.
void setComplexArabic(bool on);
/// True once a font with more than ASCII in it has been handed over.
bool ready();

void drawText(const char* text, int x, int y, int fontSize, Color color);
int  measureText(const char* text, int fontSize);

/// Report a string drawn wider than the space it was given.
///
/// A translation is not wrong for being long, but a BUTTON is. drawActBtn
/// centres its label with (w - tw) / 2, which goes negative once the label is
/// wider than the button: the text spills out of both ends and over whatever
/// sits beside it. "Break NAP" is nine characters; French says "Rompre le
/// pacte de non-agression", three and a half times as long, and nothing in the
/// pipeline noticed because the translation is correct.
///
/// OD_I18N_FIT=<path> collects them. Off, this is one null check per call --
/// the same bargain as OD_I18N_AUDIT.
void fitAudit(const char* text, int width, int fontSize, const char* where);

/// True if any byte is above 127 -- i.e. this string needs the atlas.
bool needsAtlas(const char* text);

/**
 * The first `chars` CHARACTERS of `text`, cut on a codepoint boundary.
 *
 * Six places shortened a name to fit a column with `s.substr(0, n)`, which
 * counts BYTES. That was correct for as long as every name was ASCII. It
 * stopped being correct the moment a country could be called 南モンゴリアン帝国:
 * five bytes is one character and two thirds of the next, and the leftover
 * two thirds is not valid UTF-8, so the economy screen's bar was labelled
 * "南?" -- the question mark being the renderer's answer to a byte that
 * decodes to nothing.
 *
 * The caller appends its own ellipsis, because the six sites disagree about
 * what it should be and all six are right about their own column.
 */
std::string firstChars(const std::string& text, int chars);

/// Make `text` fit `width`: shrink the type first, cut it only as a last
/// resort.
///
/// A button sized for English is a button sized for the shortest language in
/// the set. "Cancel Request Mutual Guarantee" did not fit its own button in
/// ENGLISH -- 170px of label in 154px of button -- and drawActBtn centres what
/// it is given, so the text hung out of both ends rather than clipping.
/// Ninety-three translations were in the same position, which is the shape of
/// the problem: the widget was wrong and only translation made it obvious.
/// (That act reads "Mutual Guarantee" now; this still catches the rest.)
///
/// `fontSize` comes back as the size that fits. The returned string is the
/// original wherever possible, and an ellipsised prefix where even the floor
/// size is not enough.
std::string fitToWidth(const std::string& text, int width, int& fontSize, int floorSize = 9);
/// How many characters -- not bytes -- `text` is.
int charCount(const std::string& text);

/**
 * Draw a paragraph inside `maxWidth`, breaking it into lines here rather than
 * in the source. Returns the height used.
 *
 * WHY THIS MATTERS FOR TRANSLATION. Paragraphs in this game were written as
 * one DrawText per line, which makes each LINE a translatable string --
 * "It was installed by a package manager, a store or an installer," and then
 * "so updating it here would put the two out of step. Update it the". Nobody
 * can translate half a sentence: German moves the verb to the end and Japanese
 * moves it further, so the break that was in the right place in English is in
 * the middle of a word somewhere else. One string, wrapped at draw time, is
 * the only shape a paragraph can have in fourteen languages.
 *
 * Breaks on spaces, and per character when a single run is wider than the box
 * -- which is every line of Japanese, where there are no spaces to break on.
 */
int drawWrapped(const char* text, int x, int y, int maxWidth, int fontSize, Color color);
/// The height drawWrapped would use, without drawing.
int measureWrapped(const char* text, int maxWidth, int fontSize);

}  // namespace odText

// After the declarations, so the definitions above are not rewritten by them.
#define DrawText     odText::drawText
#define MeasureText  odText::measureText
