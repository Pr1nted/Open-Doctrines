#pragma once
// ── Foreign map-script dialects ─────────────────────────────────────────
//
// A map script can declare that it is NOT written in this language:
//
//     #OD/MapEngine/5 volatile gd5
//
// and the rest of the file is then handed to that dialect's translator,
// which turns it into ordinary Open Doctrines script text before anything
// runs it.
//
// A DIALECT TRANSLATES; IT DOES NOT EXECUTE. This is the whole design and it
// is worth being strict about: every dialect's output goes through the one
// existing preprocessor and the one existing interpreter, so a dialect can
// never acquire semantics of its own. Were each dialect given its own
// evaluator instead, the engine's behaviour would be the union of all of
// them, every dialect would need its own tests against a live world, and
// tools/script_surface.py -- which records what this language can do -- would
// stop describing the thing that runs.
//
// It also bounds what a dialect can mean: the translation is native script
// text, so it is readable, it obeys the declared engine version, and anything
// a dialect cannot express becomes a comment and a diagnostic rather than a
// silent omission.
//
// Like Expr.cpp, this file does not know what a Game is. A translator is a
// pure function from text to text, which is why tests/script_dialect_test.cpp
// can check every mapping with no world loaded.
#include <string>
#include <vector>

namespace odscript {

/// Something the translator wants to say. `fatal` means no usable script came
/// out; anything else is a note about what could not be carried across.
struct DialectDiag {
    std::string message;
    bool fatal = false;
};

struct DialectResult {
    std::string nativeText;               ///< Open Doctrines script text
    std::vector<DialectDiag> diags;
    bool ok = true;                       ///< false when nothing usable came out
};

using Translator = DialectResult (*)(const std::string& source);

struct Dialect {
    const char* name;
    int version;              ///< the dialect's own revision, not the engine's
    Translator translate;
    const char* summary;
};

/// Every dialect this build carries. tools/script_surface.py reads the names
/// out of Dialects.cpp, so adding one here is noticed by the test suite.
const std::vector<Dialect>& dialects();

/// The dialect by name, or nullptr. Case-insensitive.
const Dialect* findDialect(const std::string& name);

/// The dialect names, comma-separated -- for an error message that tells the
/// mapmaker what this build actually has.
std::string dialectNames();

/**
 * Reads a `volatile <name>` declaration off a script's header line.
 *
 * Returns true when one is present, with the name in `out`. A header with no
 * `volatile` is a native script and returns false, which is the common case
 * and must stay free.
 */
bool parseVolatile(const std::string& headerLine, std::string& out);

/// The header line of a script's text (its first non-blank line), or "".
std::string headerLineOf(const std::string& content);

/**
 * Everything after the header line -- what a translator is given.
 *
 * Stripped here rather than in each translator, because a dialect must not
 * have to know how Open Doctrines spells its own header: the gd5 dialect's
 * body is JSON, and a `#OD/MapEngine/5 volatile gd5` line on the front of it
 * is a parse error on column one.
 */
std::string bodyAfterHeader(const std::string& content);

// ── Greater Diplomacy 5 ────────────────────────────────────────────────
//
// GD5's scripts are not text: they are a JSON array of declarative events,
// each a set of conditions and a set of actions, evaluated once a turn. That
// shape is exactly what Open Doctrines' `spawn` tasks already are -- a flow
// parked on `waitUntil`, retested every turn by ScriptEngine::tick() -- so
// each event becomes one task.
//
// The format was learned from dragoman's clean-room model (an MIT sibling
// repository, written from format observation), NOT from Greater Diplomacy
// 5's own source, which is GPL-3.0 and somebody else's. That distinction is
// why dragoman exists and it is kept here.
DialectResult translateGd5(const std::string& source);

}  // namespace odscript
