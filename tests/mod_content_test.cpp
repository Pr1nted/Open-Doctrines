// Content a mod adds to the game's catalogues.
//
//   ModContentTest
//
// WHY THIS TEST EXISTS
//
// Three properties, each about a way this could go quietly wrong.
//
// IDS ARE GLOBAL WITHIN A CATALOGUE, unlike country fields. A country holds a
// doctrine BY ID and the save records it that way, so two mods meaning
// different things by one id would make a save ambiguous -- and which meaning
// applied would depend on load order, which nobody can see.
//
// AI VISIBILITY DEFAULTS TO FALSE. Content the model was never trained against
// is an option it cannot evaluate, and for research it changes the shape of the
// model's input. A default of "visible" would make every mod an AI change by
// accident.
//
// A PERSISTED DEFINITION SURVIVES ITS MOD. Otherwise a save recording "this
// country holds com.example:land_reform" becomes a file full of ids nothing
// can read the moment the mod is uninstalled.

#include "../src/ModContent.h"

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

using odcontent::Kind;
using odcontent::Mode;
const std::string JSON = R"({"name":"Land Reform"})";

}  // namespace

int main() {
    printf("Mod content\n");

    section("an entry is added, listed and removed");
    {
        odcontent::Registry r;
        ok(r.add(Kind::Doctrine, "a.mod", "land_reform", JSON, Mode::Persist, false),
           "a doctrine is added");
        ok(r.ownerOf(Kind::Doctrine, "land_reform") == "a.mod", "and is owned");
        ok(r.countOf(Kind::Doctrine, "a.mod") == 1, "and counted");
        // A kind is a separate catalogue: the same id in another one is fine.
        ok(r.add(Kind::Research, "a.mod", "land_reform", JSON, Mode::Persist, false),
           "the same id in a different catalogue is a different thing");
        ok(r.countOf(Kind::Doctrine, "a.mod") == 1, "and does not touch the first");
        ok(r.remove(Kind::Doctrine, "a.mod", "land_reform"), "it is removed");
        ok(r.ownerOf(Kind::Doctrine, "land_reform").empty(), "and unowned");
        ok(r.ownerOf(Kind::Research, "land_reform") == "a.mod", "the other survives");
    }

    section("IDS ARE GLOBAL WITHIN A CATALOGUE");
    {
        // THE ONE THAT WOULD MAKE A SAVE AMBIGUOUS. A country records the
        // doctrine it holds by id; two meanings for that id and the save no
        // longer says which.
        odcontent::Registry r;
        ok(r.add(Kind::Doctrine, "a.mod", "shared", JSON, Mode::Persist, false),
           "the first mod claims an id");
        ok(!r.add(Kind::Doctrine, "b.mod", "shared", JSON, Mode::Persist, false),
           "the second is REFUSED, not silently overwritten");
        ok(r.ownerOf(Kind::Doctrine, "shared") == "a.mod", "the first still owns it");
        ok(!r.remove(Kind::Doctrine, "b.mod", "shared"), "nor can it remove it");

        // Re-adding your own is how a mod redeclares on load, and must update.
        ok(r.add(Kind::Doctrine, "a.mod", "shared", R"({"name":"Changed"})",
                 Mode::Persist, true), "re-adding your own updates it");
        ok(r.countOf(Kind::Doctrine, "a.mod") == 1, "without duplicating it");
        ok(r.ofKind(Kind::Doctrine)[0].aiVisible, "including its visibility");
    }

    section("AI visibility is carried, and is the mod's statement");
    {
        odcontent::Registry r;
        r.add(Kind::Research, "a.mod", "quiet", JSON, Mode::Hollow, false);
        r.add(Kind::Research, "a.mod", "loud", JSON, Mode::Hollow, true);
        const auto all = r.ofKind(Kind::Research);
        ok(all.size() == 2, "two nodes");
        // Sorted by id: "loud" before "quiet".
        ok(all[0].id == "loud" && all[0].aiVisible, "the visible one says so");
        ok(all[1].id == "quiet" && !all[1].aiVisible, "and the invisible one says so");
    }

    section("a definition that could not work is refused");
    {
        odcontent::Registry r;
        ok(!r.add(Kind::Doctrine, "a.mod", "", JSON, Mode::Hollow, false), "an empty id");
        ok(!r.add(Kind::Doctrine, "a.mod", std::string(65, 'a'), JSON, Mode::Hollow, false),
           "an id past the bound");
        ok(!r.add(Kind::Doctrine, "a.mod", "Has_Capitals", JSON, Mode::Hollow, false),
           "capitals, which the data files never use");
        ok(!r.add(Kind::Doctrine, "a.mod", "has space", JSON, Mode::Hollow, false), "a space");
        ok(!r.add(Kind::Doctrine, "a.mod", "a:b:c", JSON, Mode::Hollow, false),
           "two colons, which have no obvious split");
        ok(!r.add(Kind::Doctrine, "a.mod", ":lead", JSON, Mode::Hollow, false),
           "a leading colon");
        ok(r.add(Kind::Doctrine, "a.mod", "com_example:thing", JSON, Mode::Hollow, false),
           "but one colon namespaces fine");
        ok(!r.add(Kind::Doctrine, "a.mod", "empty_def", "", Mode::Hollow, false),
           "an empty definition");
        ok(!r.add(Kind::Doctrine, "", "orphan", JSON, Mode::Hollow, false),
           "a mod with no id");
        ok(!r.add(Kind::Count_, "a.mod", "bad_kind", JSON, Mode::Hollow, false),
           "a kind that does not exist");
    }

    section("A PERSISTED DEFINITION SURVIVES ITS MOD");
    {
        odcontent::Registry r;
        r.add(Kind::Doctrine, "a.mod", "land_reform", JSON, Mode::Persist, true);
        const auto saved = r.toSave();

        odcontent::Registry later;
        later.fromSave(saved);
        later.setLoadedMods({"someone.else"});
        const auto after = later.ofKind(Kind::Doctrine);
        ok(after.size() == 1, "the entry is read back");
        ok(!after[0].ownerPresent, "but is inert without its mod");
        ok(after[0].json == JSON, "and its definition is intact");

        const auto resaved = later.toSave();
        ok(resaved.size() == 1, "it is written out again anyway");
        ok(resaved[0].aiVisible, "with its visibility preserved");

        // Reinstall.
        odcontent::Registry back;
        back.fromSave(resaved);
        back.setLoadedMods({"a.mod"});
        ok(back.ofKind(Kind::Doctrine)[0].ownerPresent,
           "and comes back live when its mod returns");
    }

    section("hollow entries are not in the save");
    {
        odcontent::Registry r;
        r.add(Kind::Doctrine, "a.mod", "kept", JSON, Mode::Persist, false);
        r.add(Kind::Doctrine, "a.mod", "temp", JSON, Mode::Hollow, false);
        ok(r.toSave().size() == 1, "only the persisted one");
        ok(r.toSave()[0].id == "kept", "and it is the right one");
        ok(r.persistsAnything("a.mod"), "so the mod persists something");
        r.clearHollow();
        ok(r.ownerOf(Kind::Doctrine, "temp").empty(), "clearHollow drops the hollow one");
        ok(r.ownerOf(Kind::Doctrine, "kept") == "a.mod", "and keeps the other");
    }

    section("unloading a mod takes its content");
    {
        odcontent::Registry r;
        r.add(Kind::Doctrine, "a.mod", "one", JSON, Mode::Hollow, false);
        r.add(Kind::Doctrine, "b.mod", "two", JSON, Mode::Hollow, false);
        r.removeAllOf("a.mod");
        ok(r.ownerOf(Kind::Doctrine, "one").empty(), "its entries are gone");
        ok(r.ownerOf(Kind::Doctrine, "two") == "b.mod", "and the other mod's are not");
        ok(r.add(Kind::Doctrine, "b.mod", "one", JSON, Mode::Hollow, false),
           "and the freed id can be claimed");
    }

    section("the kind names round-trip");
    {
        for (int k = 0; k < (int)Kind::Count_; ++k) {
            const char* n = odcontent::kindName((Kind)k);
            if (odcontent::kindFromName(n) != (Kind)k) {
                ok(false, std::string("kind does not round-trip: ") + n);
                break;
            }
        }
        ok(true, "every kind name maps back to its kind");
        ok(odcontent::kindFromName("nonsense") == Kind::Count_,
           "and an unknown name is not silently kind 0");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
