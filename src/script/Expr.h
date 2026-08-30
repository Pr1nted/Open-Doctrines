#pragma once
// ── The map-script expression language ──
//
// Pulled out of ScriptEngine on purpose, and it does not know what a Game is.
// Identifiers reach the caller through a resolver callback, so this file can
// be unit-tested with `2 + 3 * 4` and no world loaded -- which is what the
// engine's own single-comparison evaluator could never be.
//
// It produces a TREE rather than a value, for the block editor: a Scratch-style
// block is a tree, text is a tree, and `unparse()` turns one back into the
// other. Round-tripping blocks to text is only honest if both sides agree on
// the same structure, and this is that structure.
#include "../ScriptEngine.h"   // ScriptValue (no Game dependency)
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace odscript {

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct Node {
    enum Kind { LITERAL, REF, UNARY, BINARY, CALL };
    Kind kind = LITERAL;
    ScriptValue literal;      // LITERAL
    std::string text;         // REF: the reference; UNARY/BINARY: the operator;
                              // CALL: the function name (kids are the arguments)
    std::vector<NodePtr> kids;
};

// What an identifier means. Returning NONE marks it unresolvable, which the
// evaluator reports rather than silently treating as zero.
using Resolver = std::function<ScriptValue(const std::string&)>;

// Returns null on a parse error, with why in `err`.
NodePtr parse(const std::string& src, std::string& err);

// Evaluates a parsed tree. Sets `err` and returns NONE on a runtime error
// (unresolvable reference, divide by zero).
ScriptValue eval(const Node& n, const Resolver& resolve, std::string& err);

// parse + eval. `ok` is false if either step failed.
ScriptValue evaluate(const std::string& src, const Resolver& resolve,
                     bool& ok, std::string& err);

// Tree back to text. Parenthesised only where precedence requires it, so
// unparse(parse(x)) is stable: running it twice changes nothing.
std::string unparse(const Node& n);

}  // namespace odscript
