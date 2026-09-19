// Commands a mod adds to the map script language.
//
//   ScriptCommandsTest
//
// WHY THIS TEST EXISTS
//
// A registered command runs on ANY map's scripts. Scripts ship inside .odmap
// files and mods are enabled globally, so a mod that claimed `if` would take
// over every script in the game -- including maps whose authors never heard of
// the mod. That is the failure this registry exists to prevent, and it is not
// a failure anybody would notice as a crash: the scripts would simply start
// meaning something else.
//
// The other property is that two mods cannot both own a name. A script writes
// `reinforce FRA 3` with no mod id in it, so if the second registration
// silently won, which mod ran a line would depend on load order -- and load
// order is not something a map author can see, let alone control.

#include "../src/ScriptCommands.h"

#include <cstdio>
#include <string>

namespace {

int checks = 0, fails = 0;
void ok(bool c, const std::string& what) {
    ++checks;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!c) ++fails;
}
void section(const char* t) { printf("\n== %s ==\n", t); }

}  // namespace

int main() {
    printf("Script commands\n");

    section("a mod claims a name");
    {
        odscript::Registry r;
        ok(r.add("a.mod", "reinforce"), "a name is claimed");
        ok(r.ownerOf("reinforce") == "a.mod", "and belongs to that mod");
        ok(r.ownerOf("nothing").empty(), "an unclaimed name has no owner");
        ok(r.commandsOf("a.mod").size() == 1, "the mod owns one");
        ok(r.commandsOf("b.mod").empty(), "and another mod owns none");
    }

    section("A MOD CANNOT REDEFINE THE LANGUAGE");
    {
        // THE ONE THAT MATTERS. Registered commands apply to every map's
        // scripts, so a mod taking a keyword does not break its own maps -- it
        // breaks everyone's, silently, by making existing lines mean something
        // else.
        odscript::Registry r;
        for (const char* kw : {"if", "set", "print", "while", "foreach",
                               "spawn", "label", "jump", "else", "endif",
                               "include", "try", "catch", "stop"}) {
            if (r.add("a.mod", kw)) {
                ok(false, std::string("keyword taken over: ") + kw);
                break;
            }
        }
        ok(r.commandsOf("a.mod").empty(), "no language keyword can be claimed");

        // Case matters, because it is the parser's business and not the
        // registry's. A registry that only refused lowercase would leave "IF"
        // free the day anybody made the parser case-insensitive.
        ok(!r.add("a.mod", "IF"), "nor a keyword in another case");
        ok(!r.add("a.mod", "Set"), "nor one in mixed case");

        // Words the language does not use yet but plainly might. Refusing one
        // costs a mod author a rename; missing one costs a future keyword.
        ok(!r.add("a.mod", "function"), "nor a word the language will want");
        ok(!r.add("a.mod", "return"), "nor another");
    }

    section("first come, and not silently");
    {
        odscript::Registry r;
        ok(r.add("a.mod", "reinforce"), "the first mod claims it");
        ok(!r.add("b.mod", "reinforce"), "the second is REFUSED, not ignored");
        ok(r.ownerOf("reinforce") == "a.mod", "and the first still owns it");

        // A mod redeclares its commands on every load, so claiming your own
        // again has to succeed or every mod needs "have I done this" state.
        ok(r.add("a.mod", "reinforce"), "claiming your own again is fine");
        ok(r.commandsOf("a.mod").size() == 1, "and does not double-register it");
    }

    section("a mod cannot give up what is not its own");
    {
        odscript::Registry r;
        r.add("a.mod", "reinforce");
        ok(!r.remove("b.mod", "reinforce"), "another mod cannot unregister it");
        ok(r.ownerOf("reinforce") == "a.mod", "and it is still owned");
        ok(r.remove("a.mod", "reinforce"), "its owner can");
        ok(r.ownerOf("reinforce").empty(), "and then it is free");
        ok(!r.remove("a.mod", "reinforce"), "removing it twice says no");
    }

    section("a name that could not be a command is refused");
    {
        odscript::Registry r;
        ok(!r.add("a.mod", ""), "empty");
        ok(!r.add("a.mod", std::string(49, 'x')), "longer than the bound");
        ok(!r.add("a.mod", "2fast"), "starting with a digit");
        ok(!r.add("a.mod", "has space"), "containing a space");
        ok(!r.add("a.mod", "semi;colon"), "containing punctuation");
        ok(!r.add("", "reinforce"), "from a mod with no id");
        ok(r.add("a.mod", "re_inforce2"), "but letters, digits and underscores are fine");
    }

    section("unloading a mod takes its commands with it");
    {
        odscript::Registry r;
        r.add("a.mod", "one");
        r.add("a.mod", "two");
        r.add("b.mod", "three");
        r.removeAll("a.mod");
        ok(r.commandsOf("a.mod").empty(), "the unloaded mod's commands are gone");
        ok(r.ownerOf("three") == "b.mod", "and the other mod's are untouched");
        // ...which is what frees the name for somebody else.
        ok(r.add("b.mod", "one"), "a freed name can be claimed by another mod");
    }

    section("the list is stable");
    {
        odscript::Registry r;
        for (const char* n : {"zeta", "alpha", "mid"}) r.add("a.mod", n);
        const auto c = r.commandsOf("a.mod");
        ok(c.size() == 3 && c[0] == "alpha" && c[1] == "mid" && c[2] == "zeta",
           "sorted, not in registration order");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
