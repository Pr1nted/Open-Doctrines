// The foreign-dialect translators, which are pure functions from text to text.
//
// Links Dialects.cpp and nothing else of the engine -- no ScriptEngine, no
// Game, no raylib -- which is the same property that makes Expr.cpp testable
// and the reason a dialect is a translator rather than an evaluator. If this
// file ever needs a world to link, a dialect has grown semantics of its own
// and the design has been lost.
#include "script/Dialects.h"

#include <cstdio>
#include <string>

static int checks = 0, failed = 0;

static void ok(bool cond, const std::string& what) {
    ++checks;
    if (cond) { printf("  ok    %s\n", what.c_str()); return; }
    ++failed;
    printf("  FAIL  %s\n", what.c_str());
}

static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    using namespace odscript;

    // ── the header declaration ──
    {
        std::string name;
        ok(!parseVolatile("#OD/MapEngine/5", name), "a native header declares no dialect");
        ok(parseVolatile("#OD/MapEngine/5 volatile gd5", name) && name == "gd5",
           "`volatile gd5` is read off the header");
        ok(parseVolatile("#OD/MapEngine/5 VOLATILE GD5", name) && name == "GD5",
           "the keyword is case-insensitive");
        ok(!parseVolatile("#OD/MapEngine/5 volatile", name),
           "`volatile` with nothing after it is not a declaration");
        ok(headerLineOf("\n\n  #OD/MapEngine/5 volatile gd5\nbody\n") ==
               "#OD/MapEngine/5 volatile gd5",
           "the header is the first non-blank line, trimmed");
    }

    // ── the registry ──
    {
        ok(findDialect("gd5") != nullptr, "gd5 is registered");
        ok(findDialect("GD5") != nullptr, "a dialect is found whatever its case");
        ok(findDialect("eu4") == nullptr, "an unknown dialect is not invented");
        ok(has(dialectNames(), "gd5"), "the names are listable for an error message");
    }

    // ── gd5: a whole event ──
    {
        const char* src = R"([{
            "name": "Winter War",
            "owner": "SUN",
            "trigger_type": "Both",
            "fire_once": true,
            "conditions": [{"type": "Turn Number", "operator": ">=", "value": "12"}],
            "actions":    [{"type": "Declare War", "target": "FIN"},
                           {"type": "Send Custom Message", "message": "The border moves west."}]
        }])";
        DialectResult r = translateGd5(src);
        ok(r.ok, "an event with a counterpart translates");
        ok(has(r.nativeText, "spawn gd5_0"), "the event becomes a spawned task");
        ok(has(r.nativeText, "label gd5_0"), "with a label to spawn at");
        ok(has(r.nativeText, "waitUntil map.turn >= 12"), "Turn Number becomes map.turn");
        ok(has(r.nativeText, "set country.SUN.at_war_with.FIN true"),
           "Declare War becomes a writable at_war_with");
        ok(has(r.nativeText, "print \"The border moves west.\""),
           "a custom message becomes print");
        ok(has(r.nativeText, "stop"), "fire_once ends the task rather than re-arming");
        ok(!has(r.nativeText, "jump gd5_0"), "and does not loop");
        // The main flow must NOT end with `stop`: that returns out of runLines
        // before the pending-task drain, and every spawned task is lost.
        const size_t spawnAt = r.nativeText.find("spawn gd5_0");
        const size_t labelAt = r.nativeText.find("label gd5_0");
        const std::string between = r.nativeText.substr(spawnAt, labelAt - spawnAt);
        ok(has(between, "jump gd5_done") && !has(between, "stop"),
           "the main flow jumps past the bodies instead of stopping, "
           "or the spawned tasks would never run");
        ok(has(r.nativeText, "label gd5_done"), "and the label it jumps to exists");
    }

    // ── gd5: fire_once false re-arms, with a turn in between ──
    {
        const char* src = R"([{
            "name": "Yearly",
            "owner": "FRA",
            "fire_once": false,
            "conditions": [{"type": "True"}],
            "actions":    [{"type": "Set Variable", "target": "tick", "value": 1}]
        }])";
        DialectResult r = translateGd5(src);
        ok(r.ok && has(r.nativeText, "jump gd5_0"), "a repeating event loops");
        ok(has(r.nativeText, "wait 1 turns"),
           "and waits a turn first -- a condition that stays true would "
           "otherwise fire forever inside one turn");
        ok(has(r.nativeText, "set var.tick = 1"), "Set Variable becomes a var");
    }

    // ── gd5: the chain operators, which GD5 has and OD must build ──
    {
        auto gate = [](const char* chain) {
            std::string src = std::string(R"([{"owner":"FRA","conditions":[
                {"type":"Turn Number","operator":">","value":1},
                {"type":"Turn Number","operator":"<","value":9,"chain":")") + chain +
                R"("}],"actions":[{"type":"Send Custom Message","message":"x"}]}])";
            return translateGd5(src).nativeText;
        };
        ok(has(gate("AND"), "and"), "AND folds into `and`");
        ok(has(gate("OR"), "or"), "OR folds into `or`");
        ok(has(gate("NAND"), "not ("), "NAND becomes not(a and b)");
        ok(has(gate("NOR"), "not ("), "NOR becomes not(a or b)");
        const std::string x = gate("XOR");
        ok(has(x, " or ") && has(x, "not ("),
           "XOR is built from or/and/not, which the expression language has");
    }

    // ── gd5: a condition with no counterpart kills the EVENT ──
    // Dropping a condition would turn a gate into an unconditional fire,
    // which is worse than not running the event at all.
    {
        const char* src = R"([{
            "name": "Unknown gate", "owner": "FRA",
            "conditions": [{"type": "Has Nuclear Weapons", "value": "true"}],
            "actions":    [{"type": "Declare War", "target": "DEU"}]
        }])";
        DialectResult r = translateGd5(src);
        ok(!r.ok, "an untranslatable condition refuses the whole file here");
        bool named = false;
        for (const DialectDiag& d : r.diags) named |= has(d.message, "Has Nuclear Weapons");
        ok(named, "and the diagnostic names the condition");
        ok(!has(r.nativeText, "at_war_with"), "the action is NOT emitted ungated");
    }

    // ── gd5: an action with no counterpart is dropped, not fatal ──
    {
        const char* src = R"([{
            "name": "Mixed", "owner": "FRA",
            "conditions": [{"type": "True"}],
            "actions":    [{"type": "Spawn Unit", "target": "FRA"},
                           {"type": "Declare War", "target": "DEU"}]
        }])";
        DialectResult r = translateGd5(src);
        ok(r.ok, "an event keeps running when one of its actions cannot be carried");
        ok(has(r.nativeText, "set country.FRA.at_war_with.DEU true"),
           "the action that does translate is still there");
        bool named = false;
        for (const DialectDiag& d : r.diags) named |= has(d.message, "Spawn Unit");
        ok(named, "and the one that does not is reported");
    }

    // ── gd5: the legacy single-condition shape ──
    {
        const char* src = R"([{
            "name": "Old", "owner": "FRA",
            "condition_type": "Turn Number", "condition_val": "5",
            "action_type": "Declare War", "action_target": "DEU"
        }])";
        DialectResult r = translateGd5(src);
        ok(r.ok && has(r.nativeText, "map.turn == 5"),
           "the pre-list shape, which GD5 still reads, is read here too");
    }

    // ── gd5: the object form names one owner for every event ──
    {
        const char* src = R"({"owner": "ITA", "events": [
            {"name": "A", "conditions": [{"type":"True"}],
             "actions": [{"type":"Declare War","target":"ETH"}]}]})";
        DialectResult r = translateGd5(src);
        ok(r.ok && has(r.nativeText, "country.ITA.at_war_with.ETH"),
           "an object form supplies the owner the events lack");
    }

    // ── gd5: an event with no owner anywhere cannot be written ──
    {
        const char* src = R"([{"name":"Ownerless","conditions":[{"type":"True"}],
                               "actions":[{"type":"Declare War","target":"DEU"}]}])";
        DialectResult r = translateGd5(src);
        ok(!r.ok, "an event with no owner is refused rather than guessed at");
    }

    // ── gd5: hostile input ──
    {
        ok(!translateGd5("this is not json").ok, "a non-JSON file fails with a reason");
        ok(!translateGd5("[]").ok, "an empty array produces no script");
        ok(!translateGd5("42").ok, "a bare number is not a list of events");
        // A quote inside a message would end the token early and leave the
        // rest as stray words; the tokenizer has no escapes.
        const char* q = R"([{"owner":"FRA","conditions":[{"type":"True"}],
                             "actions":[{"type":"Send Custom Message",
                                         "message":"he said \"go\""}]}])";
        DialectResult r = translateGd5(q);
        ok(r.ok, "a message with a quote in it still translates");
        const size_t line = r.nativeText.find("print ");
        const size_t eol = r.nativeText.find('\n', line);
        const std::string printed = r.nativeText.substr(line, eol - line);
        int quotes = 0;
        for (char c : printed) if (c == '"') ++quotes;
        ok(quotes == 2, "and the print line carries exactly two quotes");
        // An ISO code is pasted into a reference, so it must be an identifier.
        const char* inj = R"([{"owner":"FRA","conditions":[{"type":"True"}],
                               "actions":[{"type":"Declare War",
                                           "target":"DEU true\nset country.FRA.treasury 999999"}]}])";
        DialectResult bad = translateGd5(inj);
        // ident() strips the newline, the spaces and the dots, so the payload
        // collapses into ONE identifier -- a reference that names nothing.
        // Counting `set` statements is the property that matters; looking for
        // the word "treasury" is not, because the flattened identifier still
        // contains it harmlessly.
        int sets = 0;
        for (size_t i = 0; (i = bad.nativeText.find("\nset ", i)) != std::string::npos; ++i) ++sets;
        ok(sets == 1, "a target cannot smuggle a second statement into the translation");
        ok(bad.nativeText.find("at_war_with.DEUtrue") != std::string::npos,
           "the payload is flattened into one inert identifier");
    }

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
