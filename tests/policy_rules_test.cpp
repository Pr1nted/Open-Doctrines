// Whether two doctrines can be held at once.
//
//   PolicyRulesTest <data-dir>
//
// WHY THIS TEST EXISTS
//
// Incompatibility is a property of a PAIR, but the game only ever read it off
// one side. Twelve of the shipped pairs name each other once -- Land Reform
// names Flat Tax, Flat Tax names nobody -- so which of the two a country could
// hold depended on the order it picked them in. Adopt Flat Tax first and Land
// Reform was refused; adopt Land Reform first and Flat Tax went straight
// through, and the country finished holding both halves of a contradiction,
// collecting both sets of levers. The map editor had always read the pair both
// ways, so a start position the editor refused to build was reachable in play.
//
// The data now states every pair on both sides, which means the shipped game
// cannot reach the bug -- and would make a test against shipped data pass
// whether or not the rule was ever fixed. So the fixture BREAKS the data on
// purpose: it strips one side of a real pair and requires the refusal to hold
// anyway. That is the case a mod or a hand-edited .odmap can still produce,
// and it is the only thing here that distinguishes a working rule from a
// symmetric data file.
//
// Everything is checked through enactPolicy, not just through the sentence the
// policy screen prints. enactPolicy is what the AI and the mod API call; a rule
// enforced only where the screen greys out a button binds the player alone.
//
// It runs against the SERVER build (no GL), which is what lets a test load a
// real map with no window.

#include "../src/Game.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  %-68s %s\n", what.c_str(), ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

}  // namespace

struct PolicyRules {
    Game game;
    int cid = 0;

    bool load(const std::string& dataDir) {
        game.m_headless = true;
        game.m_dataDir = dataDir;
        if (!game.loadFromODM(dataDir + "STDmaps/1914.odmap")) return false;
        game.loadGameDataStep1();
        game.loadGameDataStep2();
        for (const auto& [id, c] : game.m_countries.getAll())
            if (id > 0 && id < 65530) { cid = id; break; }
        return cid != 0 && !game.m_allPolicies.empty();
    }

    Policy* find(const std::string& id) {
        for (auto& p : game.m_allPolicies)
            if (p.id == id) return &p;
        return nullptr;
    }

    bool holds(const std::string& id) const {
        for (const auto& ap : game.m_activePolicies)
            if (ap.countryId == cid && ap.policyId == id && ap.turnsRemaining >= 0)
                return true;
        return false;
    }

    /// Forget everything this country has enacted, so each case starts clean.
    void reset() {
        game.m_activePolicies.clear();
        game.m_countryActivePolicyIndices.clear();
    }

    /// Remove every gate EXCEPT the conflict, so a refusal can only mean one
    /// thing. Otherwise a doctrine refused for want of income reads as a pass.
    void makeAlwaysAffordable(const std::string& id) {
        Policy* p = find(id);
        if (!p) return;
        p->costPerTurn = 0;
        p->minEcon = -1000; p->maxEcon = 1000;
        p->minSoc  = -1000; p->maxSoc  = 1000;
    }

    void run() {
        // ── 1. the shipped data states every pair on both sides ──
        {
            std::vector<std::string> oneWay;
            for (const auto& p : game.m_allPolicies) {
                for (const auto& other : p.incompatibleWith) {
                    const Policy* q = nullptr;
                    for (const auto& r : game.m_allPolicies)
                        if (r.id == other) { q = &r; break; }
                    if (!q) { oneWay.push_back(p.id + " -> (missing) " + other); continue; }
                    if (std::find(q->incompatibleWith.begin(), q->incompatibleWith.end(),
                                  p.id) == q->incompatibleWith.end())
                        oneWay.push_back(p.id + " -> " + other);
                }
            }
            check(oneWay.empty(), "every shipped conflict is stated on both doctrines"
                  + (oneWay.empty() ? std::string() : " (" + oneWay.front() + ")"));
        }

        // ── 2. the pair, with one side of the declaration deleted ──
        //
        // This is the whole test. land_reform still names flat_tax; flat_tax
        // now names nothing, exactly like the shipped data before this change
        // and exactly like a mod that states a conflict once.
        const std::string A = "land_reform", B = "flat_tax";
        Policy* a = find(A);
        Policy* b = find(B);
        check(a && b, "the fixture's two doctrines exist");
        if (!a || !b) return;

        b->incompatibleWith.erase(
            std::remove(b->incompatibleWith.begin(), b->incompatibleWith.end(), A),
            b->incompatibleWith.end());
        check(std::find(a->incompatibleWith.begin(), a->incompatibleWith.end(), B)
                  != a->incompatibleWith.end() &&
              std::find(b->incompatibleWith.begin(), b->incompatibleWith.end(), A)
                  == b->incompatibleWith.end(),
              "fixture: the pair is now declared on one side only");

        makeAlwaysAffordable(A);
        makeAlwaysAffordable(B);

        check(game.policiesConflict(A, B) && game.policiesConflict(B, A),
              "a one-way declaration conflicts in both directions");
        check(!game.policiesConflict(A, A), "a doctrine does not conflict with itself");
        check(!game.policiesConflict(A, "professional_army"),
              "unrelated doctrines do not conflict");

        // The declared direction: the side that names the conflict is refused.
        reset();
        game.enactPolicy(cid, B);
        check(holds(B), "the undeclared side can be enacted on its own");
        game.enactPolicy(cid, A);
        check(!holds(A), "declared side is refused while its partner is in force");

        // The undeclared direction. This is the one that used to go through.
        reset();
        game.enactPolicy(cid, A);
        check(holds(A), "the declaring side can be enacted on its own");
        game.enactPolicy(cid, B);
        check(!holds(B), "undeclared side is refused too -- order cannot decide it");

        // And the screen says so from the side that never declared it.
        {
            const auto names = game.conflictingPolicyNames(*b);
            check(std::find(names.begin(), names.end(), a->name) != names.end(),
                  "the undeclared side lists its partner as a conflict");
        }

        // ── 3. a start position cannot hand a country both halves ──
        reset();
        game.m_startingPolicies[game.m_countries.getAll().at(cid).isoA3] = {A, B};
        game.applyStartingPolicies();
        check(!(holds(A) && holds(B)),
              "a start position holding both halves keeps only one");
        check(holds(A) || holds(B), "...and does keep one of them");
    }
};

int main(int argc, char** argv) {
    const std::string dataDir = argc > 1 ? argv[1] : "data/";
    printf("Doctrine rules (can two doctrines be held at once)\n");

    PolicyRules t;
    if (!t.load(dataDir)) {
        fprintf(stderr, "could not load %sSTDmaps/1914.odmap\n", dataDir.c_str());
        return 2;
    }
    t.run();

    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
