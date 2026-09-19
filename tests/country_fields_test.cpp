// Fields a mod adds to a country.
//
//   CountryFieldsTest
//
// WHY THIS TEST EXISTS
//
// Two properties carry the whole design, and both are about what happens when
// something goes wrong rather than when it goes right.
//
// UNINSTALLING A MOD MUST NOT DESTROY DATA. A persisted field's values are
// held and written back out even with the owning mod gone, so removing a mod
// to see whether it was causing something is an undoable experiment rather
// than a decision. The easy implementation -- drop what has no owner -- would
// silently delete a player's campaign state the first time they tried it.
//
// ONE MOD MUST NOT REACH ANOTHER'S. Fields are keyed by (mod, name), so two
// mods may both call a field "morale" and neither can see the other's. The ABI
// never lets a mod supply the owner id, but the store is what has to enforce
// it, and a store that keyed on the name alone would let the second mod to
// declare it silently inherit the first's numbers.

#include "../src/CountryFields.h"

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

using odcountry::Mode;
using odcountry::Type;

}  // namespace

int main() {
    printf("Country fields\n");

    section("a field is declared, written and read");
    {
        odcountry::Store s;
        ok(s.add("a.mod", "morale", Mode::Persist, Type::Number), "a field is added");
        ok(s.has("a.mod", "morale"), "and is there");
        ok(s.setNumber("a.mod", "morale", 7, 62.5), "a country's value is set");
        ok(s.number("a.mod", "morale", 7) == 62.5, "and reads back");
        ok(s.number("a.mod", "morale", 8) == 0.0, "a country never set reads 0");
        ok(s.number("a.mod", "nosuch", 7, -1.0) == -1.0,
           "and an undeclared field gives the caller's fallback");
    }

    section("one mod cannot reach another's");
    {
        // THE ISOLATION THE ABI RELIES ON. A store keyed on the name alone
        // would let the second mod to declare "morale" inherit the first's
        // numbers, and the ABI could do nothing about it.
        odcountry::Store s;
        s.add("a.mod", "morale", Mode::Persist, Type::Number);
        s.add("b.mod", "morale", Mode::Persist, Type::Number);
        s.setNumber("a.mod", "morale", 7, 10.0);
        s.setNumber("b.mod", "morale", 7, 99.0);
        ok(s.number("a.mod", "morale", 7) == 10.0, "a.mod keeps its own value");
        ok(s.number("b.mod", "morale", 7) == 99.0, "and b.mod keeps its own");

        ok(!s.remove("b.mod", "nosuch"), "removing what is not there says so");
        s.remove("b.mod", "morale");
        ok(s.number("a.mod", "morale", 7) == 10.0,
           "and removing b.mod's field leaves a.mod's standing");
    }

    section("redeclaring");
    {
        odcountry::Store s;
        s.add("a.mod", "morale", Mode::Hollow, Type::Number);
        s.setNumber("a.mod", "morale", 1, 5.0);
        // A hollow field is redeclared on EVERY load. Making that an error
        // would mean every mod writing "have I done this already" bookkeeping.
        ok(s.add("a.mod", "morale", Mode::Hollow, Type::Number),
           "declaring the same field again is not an error");
        ok(s.number("a.mod", "morale", 1) == 5.0, "and does not wipe it");
        // Changing the type would leave the stored values meaning nothing.
        ok(!s.add("a.mod", "morale", Mode::Hollow, Type::Text),
           "but changing its type is refused");
    }

    section("a type is not a suggestion");
    {
        odcountry::Store s;
        s.add("a.mod", "num", Mode::Persist, Type::Number);
        s.add("a.mod", "txt", Mode::Persist, Type::Text);
        ok(!s.setText("a.mod", "num", 1, "hello"), "text into a number field is refused");
        ok(!s.setNumber("a.mod", "txt", 1, 1.0), "and a number into a text one");
        ok(s.setText("a.mod", "txt", 1, "hello"), "the right type works");
        ok(s.text("a.mod", "txt", 1) == "hello", "and reads back");
    }

    section("a name that could break something is refused");
    {
        odcountry::Store s;
        ok(!s.add("a.mod", "", Mode::Hollow, Type::Number), "empty");
        ok(!s.add("a.mod", std::string(65, 'x'), Mode::Hollow, Type::Number),
           "longer than the bound");
        ok(!s.add("a.mod", "has space", Mode::Hollow, Type::Number), "a space");
        ok(!s.add("a.mod", "quote\"d", Mode::Hollow, Type::Number), "a quote");
        // THE ONE THAT MATTERS: the store keys on modId + '\0' + name, so a
        // name carrying a NUL could be crafted to collide with another mod's
        // field. This is the only place that can refuse it.
        ok(!s.add("a.mod", std::string("a\0b", 3), Mode::Hollow, Type::Number),
           "and a NUL, which could forge another mod's key");
        ok(!s.add("", "morale", Mode::Hollow, Type::Number), "a mod with no id");
    }

    section("UNINSTALLING A MOD DOES NOT DESTROY ITS DATA");
    {
        odcountry::Store s;
        s.add("a.mod", "morale", Mode::Persist, Type::Number);
        s.setNumber("a.mod", "morale", 7, 62.5);

        // Save, then load into a world where a.mod is not installed.
        const auto saved = s.toSave();
        odcountry::Store later;
        later.fromSave(saved);
        later.setLoadedMods({"someone.else"});

        ok(!later.has("a.mod", "morale"), "the field is inert without its mod");
        ok(!later.setNumber("a.mod", "morale", 7, 1.0),
           "and nothing can write it while the owner is away");

        // ...and it is still written out, which is the whole point.
        const auto resaved = later.toSave();
        ok(resaved.size() == 1, "it is saved again anyway");
        ok(!resaved.empty() && resaved[0].values.size() == 1 &&
           resaved[0].values[0].number == 62.5,
           "with the value untouched");

        // Reinstall: declaring the field is what brings it back.
        odcountry::Store back;
        back.fromSave(resaved);
        back.setLoadedMods({"a.mod"});
        back.add("a.mod", "morale", Mode::Persist, Type::Number);
        ok(back.has("a.mod", "morale"), "reinstalling the mod restores the field");
        ok(back.number("a.mod", "morale", 7) == 62.5,
           "and the value it had before it was uninstalled");
    }

    section("hollow fields are not in the save at all");
    {
        odcountry::Store s;
        s.add("a.mod", "cached", Mode::Hollow, Type::Number);
        s.add("a.mod", "kept", Mode::Persist, Type::Number);
        s.setNumber("a.mod", "cached", 1, 1.0);
        s.setNumber("a.mod", "kept", 1, 2.0);

        const auto saved = s.toSave();
        ok(saved.size() == 1, "only the persisted one is written");
        ok(saved[0].field.name == "kept", "and it is the right one");

        ok(s.persistsAnything("a.mod"), "the mod does persist something");
        ok(!s.persistsAnything("b.mod"), "and another mod does not");

        // Loading must not disturb a hollow field the mod has already declared
        // this session. The order here is the one Game_UI uses: fromSave, then
        // setLoadedMods -- a persisted field comes back INERT and is only made
        // live by knowing its owner is here, so that a save loaded without its
        // mod cannot have its values quietly rewritten.
        s.fromSave(saved);
        ok(s.has("a.mod", "cached"), "a hollow field survives a load");
        ok(!s.has("a.mod", "kept"),
           "and a persisted one is inert until its owner is known to be here");
        s.setLoadedMods({"a.mod"});
        ok(s.has("a.mod", "kept"), "which setLoadedMods is what settles");

        s.clearHollow();
        ok(!s.has("a.mod", "cached"), "and clearHollow is what drops the hollow one");
        // The persisted one is NOT dropped with it: clearHollow means what it
        // says, and a load that took the persisted fields with it would lose
        // the save's own data every time a mod redeclared a cache.
        ok(s.has("a.mod", "kept"), "while the persisted one is left alone");
    }

    section("the same world saves the same way twice");
    {
        odcountry::Store s;
        for (const char* n : {"zeta", "alpha", "mid"})
            s.add("a.mod", n, Mode::Persist, Type::Number);
        const auto a = s.toSave();
        const auto b = s.toSave();
        ok(a.size() == 3 && b.size() == 3, "three fields");
        bool same = true;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].field.name != b[i].field.name) same = false;
        ok(same, "in the same order both times");
        ok(a[0].field.name == "alpha", "sorted, not in declaration order");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
