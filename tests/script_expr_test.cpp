/**
 * The map-script expression language: precedence, short-circuit, round-trip.
 *
 * Before this existed a condition was one comparison and nothing else -- no
 * arithmetic, no and/or, no parentheses -- and the evaluator that enforced
 * that could not be tested at all, because it lived inside ScriptEngine and
 * ScriptEngine needs a loaded Game. So the first property checked here is
 * simply that `2 + 3 * 4` is 14: it is the thing the old engine could not say.
 *
 * The round-trip block matters for a different reason. A block editor is a
 * tree, text is a tree, and the two are only interchangeable if unparse(parse(x))
 * is stable -- otherwise opening a script in blocks and saving it rewrites the
 * author's file. That is checked here by parsing, unparsing, and parsing again.
 *
 *     ScriptExprTest
 */

#include "script/Blocks.h"
#include "script/Expr.h"

#include <cstdio>
#include <string>

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) { printf("  FAIL %s\n", what.c_str()); ++g_failures; }
    else printf("  ok    %s\n", what.c_str());
}

/// Every reference a test needs, so the evaluator can run with no Game.
static ScriptValue testResolver(const std::string& name) {
    if (name == "var.gold")    return ScriptValue::makeInt(100);
    if (name == "var.ratio")   return ScriptValue::makeFloat(2.5);
    if (name == "var.name")    return ScriptValue::makeStr("France");
    if (name == "var.yes")     return ScriptValue::makeBool(true);
    if (name == "var.no")      return ScriptValue::makeBool(false);
    if (name == "country.USA.treasury") return ScriptValue::makeInt(5000);
    if (name == "province.42.population") return ScriptValue::makeInt(750000);
    if (name == "boom") { printf("      (evaluated the right-hand side)\n"); return ScriptValue::makeInt(1); }
    return {};   // NONE == unknown
}

static ScriptValue run(const std::string& src, bool& ok, std::string& err) {
    return odscript::evaluate(src, testResolver, ok, err);
}

static void expectInt(const std::string& src, long long want) {
    bool ok = false; std::string err;
    ScriptValue v = run(src, ok, err);
    check(ok && v.asInt() == want,
          src + " == " + std::to_string(want) + (ok ? "" : "  [" + err + "]"));
}

static void expectBool(const std::string& src, bool want) {
    bool ok = false; std::string err;
    ScriptValue v = run(src, ok, err);
    check(ok && v.asBool() == want,
          src + " is " + (want ? "true" : "false") + (ok ? "" : "  [" + err + "]"));
}

static void expectError(const std::string& src) {
    bool ok = true; std::string err;
    run(src, ok, err);
    check(!ok, src + " is refused");
}

/// parse -> unparse -> parse must not drift; that is what makes blocks safe.
static void expectStable(const std::string& src) {
    std::string err;
    auto a = odscript::parse(src, err);
    if (!a) { check(false, src + " parses  [" + err + "]"); return; }
    const std::string once = odscript::unparse(*a);
    auto b = odscript::parse(once, err);
    if (!b) { check(false, src + " re-parses  [" + err + "]"); return; }
    const std::string twice = odscript::unparse(*b);
    check(once == twice, src + "  ->  " + once + "  (stable)");
}

int main() {
    printf("map-script expressions\n\n");

    printf("  -- arithmetic, which the old engine had none of --\n");
    expectInt("2 + 3 * 4", 14);              // precedence, not left-to-right
    expectInt("(2 + 3) * 4", 20);            // parentheses override it
    expectInt("10 - 2 - 3", 5);              // left-associative, not 11
    expectInt("var.gold + 50", 150);
    expectInt("country.USA.treasury / 2", 2500);
    expectInt("7 % 3", 1);
    expectInt("-var.gold + 150", 50);        // unary minus

    printf("\n  -- comparison and boolean --\n");
    expectBool("var.gold > 50", true);
    expectBool("var.gold + 1 > 100", true);  // arithmetic binds tighter
    expectBool("var.gold > 50 and var.gold < 200", true);
    expectBool("var.gold > 500 or var.gold == 100", true);
    expectBool("not var.no", true);
    expectBool("var.yes and not var.no", true);
    expectBool("(var.gold > 50) and (var.ratio < 2)", false);
    expectBool("var.name == \"France\"", true);
    expectBool("province.42.population > 500000", true);

    printf("\n  -- short-circuit: the right side must not run --\n");
    printf("      expecting NO line below this one\n");
    expectBool("var.no and boom", false);
    expectBool("var.yes or boom", true);

    printf("\n  -- errors are reported, not silently zero --\n");
    expectError("var.unknown > 1");          // unresolvable reference
    expectError("1 / 0");
    expectError("2 +");                      // truncated
    expectError("(1 + 2");                   // unbalanced
    expectError("1 2");                      // trailing junk

    printf("\n  -- functions --\n");
    expectInt("min(3, 7)", 3);
    expectInt("max(3, 7, 5)", 7);            // variadic
    expectInt("abs(0 - 9)", 9);
    expectInt("round(2.6)", 3);
    expectInt("floor(2.9)", 2);
    expectInt("ceil(2.1)", 3);
    expectInt("clamp(150, 0, 100)", 100);
    expectInt("clamp(var.gold, 0, 50)", 50);
    expectInt("min(var.gold, country.USA.treasury)", 100);
    expectInt("max(1, 2) + min(10, 3)", 5);  // nested in arithmetic
    expectError("abs(1, 2)");                // wrong arity is refused
    expectError("nope(1)");                  // unknown function

    printf("\n  -- text <-> tree round trip, for the block editor --\n");
    expectStable("2 + 3 * 4");
    expectStable("(2 + 3) * 4");
    expectStable("10 - 2 - 3");
    expectStable("10 - (2 - 3)");            // right child of equal precedence keeps its parens
    expectStable("var.gold > 50 and var.gold < 200");
    expectStable("not var.no");
    expectStable("var.name == \"France\"");
    expectStable("country.USA.treasury / 2 + 1");
    expectStable("min(3, 7)");
    expectStable("clamp(var.gold + 1, 0, 100)");
    expectStable("max(1, 2) + min(10, 3)");

    printf("\n  -- script text <-> blocks --\n");
    {
        // Everything the block editor has to survive in one script: the engine
        // header, comments, a blank line, nesting, all three arm forms, both
        // loop kinds, and a statement this file does not know.
        const std::string src =
            "#OD/MapEngine/2\n"
            "# setup\n"
            "\n"
            "set country.USA.treasury = 10000 + 500\n"
            "for var.i = 1 to 3\n"
            "    if var.i == 2\n"
            "        print \"halfway\"\n"
            "        continue\n"
            "    elseif var.i > 2\n"
            "        break\n"
            "    else\n"
            "        set var.flag true\n"
            "    endif\n"
            "next\n"
            "foreach province in country.SYR\n"
            "    # thin these out\n"
            "    set province.population = province.population / 2\n"
            "next\n"
            "someFutureCommand with args\n"
            "waitUntil map.turn >= 10\n";

        odscript::Doc d = odscript::parseScript(src);
        check(d.error.empty(), "a well-formed script parses  [" + d.error + "]");
        check(d.blocks.size() == 5, "five top-level blocks, got " + std::to_string(d.blocks.size()));

        const std::string once = odscript::unparseScript(d);
        check(once == src, "blocks -> text is byte-identical to the source");

        odscript::Doc d2 = odscript::parseScript(once);
        check(odscript::unparseScript(d2) == once, "and stable on a second pass");

        // An unknown statement must survive a visit to the editor.
        bool sawRaw = false;
        for (const auto& b : d.blocks)
            if (b.kind == odscript::Block::RAW && b.head.rfind("someFutureCommand", 0) == 0) sawRaw = true;
        check(sawRaw, "an unrecognised statement is carried through, not dropped");

        // Comments belong to the statement below them, so they do not migrate.
        check(once.find("    # thin these out\n    set province.population") != std::string::npos,
              "a comment stays attached to the line it introduces");
    }
    {
        odscript::Doc bad = odscript::parseScript("if var.x == 1\n    set var.y 2\n");
        check(!bad.error.empty(), "an unclosed if is reported: " + bad.error);
    }

    printf("\n  -- moving blocks around (what the editor's drag does) --\n");
    {
        const std::string src =
            "set var.a 1\n"
            "for var.i = 1 to 3\n"
            "    set var.b 2\n"
            "    set var.c 3\n"
            "next\n";
        odscript::Doc d = odscript::parseScript(src);

        // Reach a nested block by path: [1,0,1] = block 1, body 0, index 1.
        odscript::Block* nested = odscript::blockAt(d.blocks, {1, 0, 1});
        check(nested && nested->head == "set var.c 3",
              "a nested block is reachable by path");

        // Drag it out to the top, after everything.
        odscript::Block moved;
        check(odscript::removeBlockAt(d.blocks, {1, 0, 1}, moved), "it lifts out");
        check(odscript::insertBlockAt(d.blocks, {2}, std::move(moved)), "and drops in at the top level");
        const std::string after = odscript::unparseScript(d);
        check(after ==
              "set var.a 1\n"
              "for var.i = 1 to 3\n"
              "    set var.b 2\n"
              "next\n"
              "set var.c 3\n",
              "the move lands exactly where it was dropped");

        // A block must never be dropped inside itself.
        check(odscript::pathContains({1}, {1, 0, 0}), "a body counts as inside its opener");
        check(!odscript::pathContains({1}, {0}), "a sibling does not");

        // Out-of-range paths are refused rather than corrupting the tree.
        odscript::Block dummy;
        check(!odscript::removeBlockAt(d.blocks, {99}, dummy), "an out-of-range path is refused");
        check(odscript::blockAt(d.blocks, {1, 9, 0}) == nullptr, "so is a bad body index");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
