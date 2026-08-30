#pragma once

// The dialogue markup: what a .oddlg file says, and what it parses into.
//
// WHY A LANGUAGE AND NOT A JSON BLOB
//
// Dialogue is written by people, in prose, and edited far more often than any
// other data in the game. A format where the words are buried in quotes and
// escapes is a format nobody wants to write a tutorial in. So a .oddlg file is
// mostly just the text, with markdown for the three emphases everybody already
// knows and braces for the rest.
//
// NOTHING IN THIS HEADER KNOWS ABOUT RAYLIB. The parser and the line breaker
// are the two parts with real edge cases -- nesting, escapes, where a sentence
// may be cut -- and both are testable without a window (tests/dialog_test.cpp).
// Colours are a POD here and become raylib Colors one layer up, in DialogBox.

#include <cstdint>
#include <string>
#include <vector>

namespace dlg {

struct Rgb { uint8_t r = 255, g = 255, b = 255; };

// Effects are a BITMASK because they combine. "{shake}{rainbow}**doom**{/rainbow}{/shake}"
// is one glyph run that is bold, shaking and cycling hue at once, and any
// design where a glyph has one effect makes that impossible to express.
enum Fx : uint32_t {
    FX_BOLD      = 1u << 0,
    FX_ITALIC    = 1u << 1,
    FX_UNDERLINE = 1u << 2,
    FX_SHAKE     = 1u << 3,
    FX_WAVE      = 1u << 4,
    FX_RAINBOW   = 1u << 5,
    FX_OBFUSCATE = 1u << 6,   ///< renders as churning gibberish, not as itself
    FX_STRIKE    = 1u << 7,   ///< struck through
    /**
     * A stage direction -- *dialed in*, *sighs* -- rather than speech. Set
     * apart from the words actually said: italic and dimmed, so a reader can
     * tell at a glance what was spoken aloud and what merely happened.
     */
    FX_ACTION    = 1u << 8,
};

struct Style {
    uint32_t fx = 0;
    bool hasColor = false;
    Rgb color{};
    /// Painted in the player's accent rather than a fixed colour. `accentTone`
    /// scales it: 1.0 as configured, 0.6 a darker shade of the same hue, 1.4 a
    /// lighter one -- so a script can key off the player's palette without
    /// being limited to exactly one colour from it.
    bool useAccent = false;
    float accentTone = 1.0f;
    /// Points relative to the page's size, so a shout can be bigger without the
    /// script having to know what the base size is.
    int sizeDelta = 0;
};

struct Span {
    std::string text;    ///< UTF-8
    Style style;
    /**
     * Stands in for whatever the player last picked.
     *
     * Written {choice} in a script. The text is not known until the pick
     * happens, so the span carries no words of its own and the box fills it
     * in at layout time -- which is what lets a script answer in the player's
     * own words without every branch being written out twice.
     */
    bool echoChoice = false;
    /**
     * Stands in for whatever key is currently bound to this action.
     *
     * Written {key=army_move}. A tutorial that hardcodes "press M" is wrong
     * for every player who rebound it, and wrong silently -- they press M,
     * nothing happens, and the game looks broken rather than the text.
     *
     * Holds the action's script id; the box resolves it through the resolver
     * the game installs, because a dialogue box has no business knowing what
     * a keybind is.
     */
    std::string keyAction;
};

enum class Anchor { Left, Center, Right };

enum class Enter {
    Typewriter,   ///< the default: one glyph at a time
    Fade,         ///< the whole page fades up together
    Rise,         ///< fades up while sliding from below
    Instant,
};

struct PageStyle {
    Anchor anchor = Anchor::Left;
    int    size = 20;
    float  letterSpacing = 1.0f;   ///< extra pixels between glyphs
    float  lineSpacing = 1.35f;    ///< multiple of the line height
    int    padding = 24;
    Enter  enter = Enter::Typewriter;
    float  speed = 45.0f;          ///< glyphs per second, for Typewriter
    /**
     * Seconds of silence before the page starts typing.
     *
     * A beat, not a slow-down. Writing "... you know? my bad..." at half
     * speed makes the whole line laboured; what the moment actually wants is
     * a pause and then normal speech, which is how somebody sounds when they
     * have just realised something.
     */
    float  delay = 0.0f;
};

/**
 * One thing the player may pick at the end of a page.
 *
 * `key` is what the GAME hears -- a stable identifier a script or the tutorial
 * can branch on -- and `label` is what the player reads. They are separate so
 * the wording can be rewritten, or translated, without breaking the branch.
 */
struct Choice {
    std::string label;
    std::string key;
};

struct Page {
    std::string speaker;           ///< empty for narration
    std::vector<Span> spans;
    PageStyle style;
    /**
     * What the character is doing while this page is on screen.
     *
     * A pose name from the speaker's .odrig, or empty to hold whatever the
     * previous page set. Carried across pages like the other directives,
     * because a speaker who shrugs usually goes on standing that way.
     */
    std::string pose;
    /**
     * Where the speaker stands, normalised across the screen, and which way
     * they face. Empty means "leave them where they are" -- so a script only
     * writes it when somebody actually moves.
     */
    bool  hasPlacement = false;
    float atX = 0.5f, atY = 0.5f, atScale = 1.0f;
    bool  flip = false;

    /**
     * What on screen this page is about: a name the GAME resolves to a
     * rectangle, not a rectangle itself.
     *
     * A script cannot hold coordinates. The economy tab is somewhere
     * different on every window size, and it moves whenever the sidebar is
     * touched -- a script full of pixel positions is a script that is wrong
     * by the next build and silently points at empty space.
     *
     * So the game registers the things a tutorial may point at under names
     * ("tab.economy", "button.end_turn"), and this is one of those names.
     * Empty means the page points at nothing.
     */
    std::string pointAt;
    /// Draw the pointer as an ellipse rather than a rectangle. Most of the UI
    /// is rectangular and a ring around it reads as sloppy; a few things --
    /// a flag, a fleet on the map -- are not.
    bool pointRound = false;
    /**
     * While this page is up, the player may click ONLY inside `pointAt`.
     *
     * A tutorial that says "press End Turn" and lets the player press
     * anything else is a tutorial that spends most of its life recovering
     * from where they went instead.
     */
    bool gate = false;

    /**
     * What has to be TRUE before this page will turn.
     *
     * Without this a tutorial is a slideshow: it says "take those six
     * provinces" and then advances on a click whether or not anything
     * happened, and by page four it is describing a situation the player is
     * not in.
     *
     * The string is not interpreted here. The dialogue box has no idea what a
     * province is; the game evaluates the condition and tells the box whether
     * it holds, via setConditionMet. Empty means the page turns on a click,
     * as pages normally do.
     */
    std::string until;

    /**
     * Something for the game to DO when this page arrives, once.
     *
     * The tutorial's rebellion is the reason this exists. It cannot be in the
     * map file -- a rebellion that is there at load can be lost to before
     * unrest has been explained -- so it has to be staged at the moment the
     * script is ready to talk about it.
     */
    std::string act;

    /**
     * Options offered at the end of this page. Non-empty means the page does
     * not advance on a click: it waits for a pick, and what was picked is
     * available to the game as the Choice's `key`.
     */
    std::vector<Choice> choices;
};

struct Script {
    std::vector<Page> pages;
    std::string language;          ///< the directory it was loaded from
    /// Anything the parser did not understand, with a line number. Never
    /// silently dropped: a tag typo that vanishes is a tag typo that gets
    /// shipped, and the only symptom is prose that is missing its emphasis.
    std::vector<std::string> warnings;
};

/**
 * Parse a .oddlg source into pages.
 *
 * THE FORMAT, in full:
 *
 *   // a comment, whole line
 *   @Speaker Name          sets who is talking, until the next @ or page
 *   @                      no speaker -- narration
 *   :: anchor=center size=22 enter=type speed=40 pad=28 spacing=1 line=1.4
 *                          page directives; any subset, in any order
 *   ---                    page break: the player clicks to get past it
 *   > Label -> key         an option; a run of them makes the page a choice
 *                          and it waits for a pick instead of a click
 *
 * Page directives for the tutorial:
 *
 *   :: point=tab.economy   ring the named UI element
 *   :: point=... round=1   ring it with an ellipse instead of a rectangle
 *   :: gate=1              and let the player click nothing else
 *   :: until=<cond>        hold the page until the game says this is true
 *   :: act=<name>          tell the game to do something, once, on arrival
 *
 * Inline, and freely nestable. The short forms are the ones dialogue is
 * actually written in:
 *
 *   **bold**   /italic/   _underline_   -struck-
 *   *action*   <wave>     |rainbow|     'shaking'    §accent§
 *
 * THE TIGHTNESS RULE, and why it has to exist. Three of those delimiters are
 * punctuation that ordinary prose is full of: "don't", "well-known", "and/or".
 * A naive scan turns the apostrophe in "don't" into the start of a shake that
 * runs until the next apostrophe, which is usually several sentences later.
 *
 * So a delimiter only counts when it sits where an opening or closing mark
 * plausibly could: an OPENER must follow a space (or start the line) and be
 * followed by a non-space; a CLOSER must follow a non-space and be followed
 * by a space, the end of the line, or closing punctuation. An apostrophe with
 * letters on both sides is neither, so "don't" is left alone, while
 * "Come 'on'!" shakes exactly the word meant.
 *
 * The long forms still work and still nest freely:
 *
 *   **bold**  *italic*  __underline__
 *   {shake}   {wave}    {rainbow}   {obf}   {strike}   {action}
 *   {color=#ff8844}      {accent}   {accent=60}
 *   {size=+4}            {b} {i} {u}
 *   {choice}             what the player last picked, echoed back
 *   {key=army_move}      the key currently bound to that action
 *   \{ \* \_ \\          literal
 *
 * A closing tag is {/name}; {/} closes the innermost open tag.
 */
Script parse(const std::string& source);

// ── Line breaking ─────────────────────────────────────────────────────────
//
// Separated from the renderer and given advances rather than a font, because
// where a line may be cut is a language question and not a drawing one -- and
// because it is the part worth testing exhaustively.

/**
 * Choose where the lines break.
 *
 * `codepoints` and `advances` are parallel and already include letter spacing.
 * Returns the index at which each line STARTS; the first is always 0.
 *
 * The rules, in order of how badly they read when broken:
 *
 *   1. Never inside a word. Only a space is a break opportunity, plus an
 *      explicit newline in the source.
 *   2. Never leave an opening bracket or quote stranded at the end of a line,
 *      and never start a line with the punctuation that closes a clause --
 *      a line ending in "(" or beginning with "," reads as a mistake.
 *   3. Never split a number from its unit, nor an abbreviation from what it
 *      abbreviates: "1914" and "Mr." keep what follows them.
 *   4. No widow. A last line holding one short word is pulled a word down from
 *      the line above instead.
 *
 * A word longer than the whole line is cut rather than allowed to overflow;
 * there is nothing better to do with it.
 */
std::vector<int> breakLines(const std::vector<int>& codepoints,
                            const std::vector<float>& advances,
                            float maxWidth);

/** True if `cp` may not start a line (closing punctuation). Exposed for tests. */
bool isClosingPunct(int cp);
/** True if `cp` may not end a line (opening bracket or quote). Exposed for tests. */
bool isOpeningPunct(int cp);

}  // namespace dlg
