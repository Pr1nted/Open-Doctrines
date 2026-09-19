// What a world remembers about what it was loaded with.
//
//   WorldProvenanceTest
//
// WHY THIS TEST EXISTS
//
// A mod can add content that ends up referenced by id from inside a save.
// Load that world without the mod and the references point at nothing -- and
// until this record existed the game could not tell, because nothing in a save
// said which mods wrote it.
//
// The properties that matter are about what the warning SAYS, not that one
// appears. A load that cries wolf about an added mod, or that reports a
// missing hollow mod as loudly as a missing persisted one, is a warning
// players learn to dismiss -- and the one time it matters is the time they
// lose a campaign.

#include "../src/WorldProvenance.h"

#include <algorithm>
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

odprov::ModRecord mod(const std::string& id, const std::string& ver, bool persisted) {
    odprov::ModRecord m;
    m.id = id; m.version = ver; m.persisted = persisted;
    return m;
}

odprov::Provenance saved(std::vector<odprov::ModRecord> mods,
                         const std::string& ver = "1.2.1a") {
    odprov::Provenance p;
    p.present = true;
    p.gameVersion = ver;
    p.mods = std::move(mods);
    return p;
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

bool mentions(const std::vector<std::string>& lines, const std::string& needle) {
    for (const std::string& l : lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

int main() {
    printf("World provenance\n");

    section("a world that still has everything says nothing");
    {
        auto running = std::vector<odprov::ModRecord>{mod("a.mod", "1.0.0", true)};
        auto m = odprov::compare(saved({mod("a.mod", "1.0.0", true)}), running, "1.2.1a");
        ok(!m.any(), "no mismatch at all");
        ok(odprov::describe(m).empty(), "and therefore nothing to say");
    }

    section("a save from before this existed reports nothing");
    {
        // THE ONE THAT WOULD CRY WOLF ON EVERY OLD SAVE. A save with no record
        // says nothing about what it was made with, and the honest reading is
        // "unknown", not "it was made with no mods and you have added three".
        odprov::Provenance none;               // present == false
        auto running = std::vector<odprov::ModRecord>{mod("a.mod", "1.0.0", true)};
        auto m = odprov::compare(none, running, "9.9.9z");
        ok(!m.any(), "an unrecorded world is not a changed world");

        // AND IT MUST STAY FALSE EVEN IF THE FIELDS ARE POPULATED. `present`
        // is the authority, not emptiness. A half-parsed save -- a truncated
        // file, a future loader that fills fields before it decides the record
        // is usable -- can carry content with present still false, and reading
        // it would report a world that was never recorded. The first version
        // of this case used an entirely empty Provenance, which produces an
        // empty Mismatch with or without the guard and so proved nothing.
        odprov::Provenance halfRead;
        halfRead.present = false;
        halfRead.gameVersion = "0.0.1";
        halfRead.mods.push_back(mod("ghost.mod", "1.0.0", true));
        auto hm = odprov::compare(halfRead, running, "9.9.9z");
        ok(!hm.any(), "a record marked absent is ignored whatever it contains");
    }

    section("adding a mod is not a warning");
    {
        // Adding a mod to a world is the ordinary way to use one. Reporting it
        // would put a warning on the most common action there is, and a
        // warning that fires every time means nothing when it matters.
        auto running = std::vector<odprov::ModRecord>{
            mod("a.mod", "1.0.0", true), mod("new.mod", "0.1.0", false)};
        auto m = odprov::compare(saved({mod("a.mod", "1.0.0", true)}), running, "1.2.1a");
        ok(!m.any(), "a mod the world has not seen before is unremarkable");
    }

    section("a missing mod is reported by what it costs");
    {
        auto m = odprov::compare(
            saved({mod("persist.mod", "1.0.0", true), mod("hollow.mod", "1.0.0", false)}),
            {}, "1.2.1a");
        ok(has(m.missingPersisted, "persist.mod"), "the persisting one is persisted-missing");
        ok(has(m.missingHollow, "hollow.mod"), "the hollow one is hollow-missing");
        ok(!has(m.missingPersisted, "hollow.mod"), "and the two are not confused");
        ok(m.dangling(), "something in the save now points at nothing");

        // THE DISTINCTION IS THE WHOLE POINT. Persisted content is inside the
        // save with nothing left to read it; hollow content was never written
        // down. Telling a player they will lose a campaign when they will not
        // is how a warning gets ignored.
        auto lines = odprov::describe(m);
        ok(lines.size() >= 2, "both are described");
        ok(lines[0].find("persist.mod") != std::string::npos,
           "the one that can cost the world is said FIRST");
        ok(mentions(lines, "lose them permanently"), "and says what is at stake");
        ok(mentions(lines, "will load normally"),
           "while the hollow one says the world is fine");
    }

    section("a hollow mod alone is not a dangling world");
    {
        auto m = odprov::compare(saved({mod("hollow.mod", "1.0.0", false)}), {}, "1.2.1a");
        ok(m.any(), "it is still worth mentioning");
        ok(!m.dangling(), "but nothing in the save is broken");
    }

    section("a version change is not an absence");
    {
        auto running = std::vector<odprov::ModRecord>{mod("a.mod", "1.1.0", true)};
        auto m = odprov::compare(saved({mod("a.mod", "1.0.0", true)}), running, "1.2.1a");
        ok(m.missingPersisted.empty() && m.missingHollow.empty(),
           "an upgraded mod is not a missing mod");
        ok(m.versionChanged.size() == 1, "it is reported as a change");
        ok(m.versionChanged[0].find("1.0.0 -> 1.1.0") != std::string::npos,
           "and says both versions, in that order");
    }

    section("the game version is reported when it moved");
    {
        auto m = odprov::compare(saved({}, "1.2.0a"), {}, "1.2.1a");
        ok(m.savedGameVersion == "1.2.0a", "the version it was last loaded by");
        auto same = odprov::compare(saved({}, "1.2.1a"), {}, "1.2.1a");
        ok(same.savedGameVersion.empty(), "and nothing when it did not move");
    }

    section("the same world always reports the same way");
    {
        // An unordered_map walk orders by hash, which differs between builds.
        // A warning that lists three mods in a different order every load
        // reads as three different warnings.
        auto s = saved({mod("z.mod", "1", false), mod("a.mod", "1", false),
                        mod("m.mod", "1", false)});
        auto m = odprov::compare(s, {}, "1.2.1a");
        auto sorted = m.missingHollow;
        std::sort(sorted.begin(), sorted.end());
        ok(m.missingHollow == sorted, "missing mods come out sorted");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
