// What a legislature is allowed to do to a government.
//
//   PartyRulesTest [data-dir]
//
// WHY THIS TEST EXISTS
//
// The party rules steer the government compass, and the compass feeds
// getProvinceRebellionChance -- which is rolled against simRand. So a party
// that pulls a fraction too hard is not a balance question, it is a different
// world. These are the properties the pull has to have for that to be safe:
//
//   it dies out as it arrives      or a party pushes past its own stance and
//                                  oscillates for the rest of the game
//   it never overshoots in a turn  the compass must not cross the stance
//   shares are a partition         they are shares of one electorate
//   ties are deterministic         a tie broken by hash order is a tie broken
//                                  differently on two machines
//
// It also checks data/parties.json, because a party table is data and the
// failure mode of data is being silently wrong: a stance outside the compass,
// shares that do not add up, or a scenario key no shipped map has.

#include "../src/Parties.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>

#include "../src/json.hpp"

namespace {

int checks = 0, fails = 0;
void ok(bool c, const std::string& what) {
    ++checks;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!c) ++fails;
}
void section(const char* t) { printf("\n== %s ==\n", t); }

odparty::Legislature twoParty(float aSupport, float aEcon, float bSupport, float bEcon) {
    odparty::Legislature leg;
    odparty::Party a;
    a.name = "A"; a.support = aSupport; a.stance = makeCompass(aEcon, 0.0f);
    odparty::Party b;
    b.name = "B"; b.support = bSupport; b.stance = makeCompass(bEcon, 0.0f);
    leg.parties = {a, b};
    odparty::chooseRuling(leg);
    return leg;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dataDir = argc > 1 ? argv[1] : "data/";
    printf("Party rules\n");

    section("the pull moves the government toward whoever governs");
    {
        odparty::Legislature leg = twoParty(60.0f, 80.0f, 40.0f, -80.0f);
        PoliticalCompass gov = makeCompass(0.0f, 0.0f);
        const PoliticalCompass d = odparty::pull(leg, gov);
        ok(d.economic > 0.0f, "a right-wing ruling party pulls the compass right");
        ok(std::fabs(d.social) < 1e-6f, "and not sideways, when it agrees on the other axis");

        // The opposition governing instead must pull the other way. Same
        // legislature, different winner: this is the whole mechanic.
        leg.parties[0].support = 30.0f;
        leg.parties[1].support = 70.0f;
        odparty::chooseRuling(leg);
        ok(odparty::pull(leg, gov).economic < 0.0f, "and a left-wing one pulls it left");
    }

    section("it dies out as it arrives, and never overshoots");
    {
        // THE PROPERTY THAT KEEPS IT STABLE. A step of fixed size would carry
        // the compass past the party's stance and back, forever.
        odparty::Legislature leg = twoParty(100.0f, 50.0f, 0.0f, -50.0f);
        PoliticalCompass gov = makeCompass(0.0f, 0.0f);
        float previous = 1.0e9f;
        bool shrinking = true, crossed = false;
        for (int turn = 0; turn < 400; ++turn) {
            const PoliticalCompass d = odparty::pull(leg, gov);
            if (std::fabs(d.economic) > previous + 1e-6f) shrinking = false;
            previous = std::fabs(d.economic);
            gov = makeCompass(gov.economic + d.economic, gov.social + d.social);
            if (gov.economic > 50.0f + 1e-4f) crossed = true;
        }
        ok(shrinking, "each turn's pull is no larger than the last");
        ok(!crossed, "the compass never crosses the stance it is heading for");
        ok(gov.economic > 40.0f, "and it does get most of the way there");
        // Arrived means arrived: no residual jitter to keep restyling on.
        ok(std::fabs(odparty::pull(leg, gov).economic) < 0.01f,
           "a government that has arrived is barely pulled at all");
    }

    section("a mandate pulls harder than a plurality");
    {
        PoliticalCompass gov = makeCompass(0.0f, 0.0f);
        const float big = odparty::pull(twoParty(80.0f, 60.0f, 20.0f, -60.0f), gov).economic;
        const float small = odparty::pull(twoParty(55.0f, 60.0f, 45.0f, -60.0f), gov).economic;
        ok(big > small, "governing on 80% moves the country faster than on 55%");

        // And below the floor, not at all: a fringe government must not steer
        // a country as hard as one with a mandate.
        odparty::Legislature fringe;
        odparty::Party tiny;
        tiny.name = "Fringe"; tiny.support = 5.0f; tiny.stance = makeCompass(90.0f, 0.0f);
        fringe.parties = {tiny};
        fringe.ruling = 0;
        ok(odparty::pull(fringe, gov).economic == 0.0f,
           "a party under the steering floor moves nothing");
    }

    section("nobody governing is no pull, not a default");
    {
        odparty::Legislature empty;
        ok(odparty::pull(empty, makeCompass(0.0f, 0.0f)).economic == 0.0f,
           "an empty legislature pulls nothing");
        odparty::Legislature leg = twoParty(60.0f, 80.0f, 40.0f, -80.0f);
        leg.ruling = -1;
        ok(odparty::pull(leg, makeCompass(0.0f, 0.0f)).economic == 0.0f,
           "and neither does one with no ruling party");
        // An index past the end must read as nobody, not as memory.
        leg.ruling = 99;
        ok(leg.rulingParty() == nullptr, "a ruling index off the end is nobody");
    }

    section("support is a partition of one electorate");
    {
        odparty::Legislature leg = twoParty(140.0f, 10.0f, 60.0f, -10.0f);
        odparty::renormalise(leg);
        float total = 0.0f;
        for (const auto& p : leg.parties) total += p.support;
        ok(std::fabs(total - 100.0f) < 1e-3f, "shares that summed to 200 are rescaled to 100");
        ok(leg.parties[0].support > leg.parties[1].support, "and keep their order of size");

        // Nobody with any support at all: an equal split, because leaving them
        // at zero makes every share-weighted number downstream divide by zero.
        odparty::Legislature dead = twoParty(0.0f, 10.0f, 0.0f, -10.0f);
        odparty::renormalise(dead);
        ok(std::fabs(dead.parties[0].support - 50.0f) < 1e-3f,
           "a legislature with no support at all splits evenly");
    }

    section("a tie is broken the same way on every machine");
    {
        odparty::Legislature leg = twoParty(50.0f, 10.0f, 50.0f, -10.0f);
        odparty::chooseRuling(leg);
        ok(leg.ruling == 0, "an exact tie keeps the party the file listed first");
    }

    section("a generated legislature is usable, and says it is generated");
    {
        const PoliticalCompass gov = makeCompass(-70.0f, -60.0f);
        odparty::Legislature a = odparty::generate(gov, 12345u);
        odparty::Legislature b = odparty::generate(gov, 12345u);
        ok(a.parties.size() == b.parties.size() && a.ruling == b.ruling,
           "the same seed generates the same legislature");
        ok(!a.parties.empty(), "and it is not empty");
        ok(a.ruling >= 0, "somebody governs it");
        ok(!a.anyHistorical(), "nothing generated claims to be historical");

        float total = 0.0f;
        for (const auto& p : a.parties) {
            total += p.support;
            ok(!p.name.empty(), "every generated party has a name");
            ok(!p.shortName.empty(), "and something short to show in a list");
        }
        ok(std::fabs(total - 100.0f) < 1e-3f, "generated shares are a partition too");

        // Consistent with the world it is in: a hard-left government should
        // not generate a right-wing incumbent.
        const odparty::Party* r = a.rulingParty();
        ok(r && r->stance.economic < 0.0f,
           "a left-wing government generates a left-wing ruling party");

        // A different seed is a different country, or generation is a constant.
        odparty::Legislature c = odparty::generate(gov, 999u);
        bool anyDifferent = c.parties.size() != a.parties.size();
        for (size_t i = 0; !anyDifferent && i < a.parties.size(); ++i)
            if (std::fabs(a.parties[i].support - c.parties[i].support) > 1e-4f ||
                std::fabs(a.parties[i].stance.economic - c.parties[i].stance.economic) > 1e-4f)
                anyDifferent = true;
        ok(anyDifferent, "a different seed generates a different one");
    }

    section("names describe rather than claim");
    {
        // A generated name must never read as a real party's proper noun. The
        // whole reason data/parties.json exists is that inventing "Freedom
        // Alliance of Bolivia" would be a claim about Bolivia.
        ok(std::string(odparty::describeStance(makeCompass(-60.0f, -40.0f))) == "Workers' Party",
           "left-authoritarian is a Workers' Party");
        ok(std::string(odparty::describeStance(makeCompass(60.0f, 40.0f))) == "Liberal Party",
           "right-libertarian is a Liberal Party");
        ok(std::string(odparty::describeStance(makeCompass(0.0f, 0.0f))) == "Centre Party",
           "and the centre is a Centre Party");
    }

    // ── THE DATA ──
    section("data/parties.json says only things it can say");
    {
        const std::string path = dataDir + "parties.json";
        std::ifstream f(path);
        if (!f) {
            ok(false, "parties.json opens: " + path);
        } else {
            nlohmann::json doc;
            bool parsed = true;
            try { doc = nlohmann::json::parse(f, nullptr, true, true); }
            catch (const std::exception& e) { parsed = false; printf("      %s\n", e.what()); }
            ok(parsed, "parties.json parses");

            if (parsed && doc.contains("scenarios")) {
                // Only keys a shipped map actually has. A table keyed "1940"
                // is a table that silently never loads, and the game cannot
                // tell that from a scenario nobody has written yet.
                const std::set<std::string> shipped = {
                    "1914", "1918", "1936cp", "1939", "1945", "1962",
                    "1962ax", "1984", "map", "mars", "tutorial"};
                int badKey = 0, badStance = 0, badShare = 0, countries = 0, parties = 0;
                std::string firstBadKey, firstBadStance;
                for (auto it = doc["scenarios"].begin(); it != doc["scenarios"].end(); ++it) {
                    if (!shipped.count(it.key())) {
                        if (!badKey++) firstBadKey = it.key();
                        continue;
                    }
                    for (auto c = it.value().begin(); c != it.value().end(); ++c) {
                        if (!c.value().is_object() || !c.value().contains("parties")) continue;
                        ++countries;
                        float total = 0.0f;
                        for (const auto& p : c.value()["parties"]) {
                            ++parties;
                            const float e = p.value("econ", 0.0f);
                            const float sc = p.value("soc", 0.0f);
                            // The compass is a bounded space and every entry
                            // point has to make it one -- one training run
                            // logged econ=9988 because a loader did not.
                            if (e < -100.0f || e > 100.0f || sc < -100.0f || sc > 100.0f) {
                                if (!badStance++)
                                    firstBadStance = it.key() + "/" + c.key() + " " +
                                                     p.value("name", std::string("?"));
                            }
                            total += p.value("support", 0.0f);
                        }
                        // Not renormalised in the file -- the loader does that
                        // -- but a table summing to 30 or 300 means somebody
                        // mixed vote share with seat counts, which is a
                        // mistake about what the numbers ARE.
                        if (total < 40.0f || total > 130.0f) ++badShare;
                    }
                }
                ok(badKey == 0, "every scenario key names a shipped map" +
                   (badKey ? " (" + firstBadKey + ")" : ""));
                ok(badStance == 0, "every stance is inside the compass" +
                   (badStance ? " (" + firstBadStance + ")" : ""));
                ok(badShare == 0, "no country's shares are a different unit from the rest");
                ok(countries > 0 && parties > 0, "and there is actually data in it");
                printf("      %d countries, %d parties on record\n", countries, parties);
            }
        }
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
