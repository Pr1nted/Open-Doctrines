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

#include <cmath>
#include <cstdlib>
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

    // ── tenure: a doctrine is a commitment, not a switch ──
    //
    // The flag decides which contract holds, and the test asserts the one that
    // matches its environment rather than picking a side -- run_all.sh runs this
    // binary twice so BOTH are covered. Reading the flag from the environment is
    // also the only way to test it: policyTenure caches it in a function-local
    // static on first use, exactly as the doctrine reflex does.
    void tenure() {
        const bool on = std::getenv("OD_DOCTRINE_TENURE") &&
                        atoi(std::getenv("OD_DOCTRINE_TENURE")) != 0;
        printf("\n-- tenure (flag %s) --\n", on ? "ON" : "off");

        ActivePolicy ap;
        ap.countryId = 1;
        ap.policyId = "professional_army";
        ap.turnsRemaining = 0;          // in force

        ap.turnsHeld = 0;
        const float fresh = game.policyTenure(ap);
        check(fresh == 1.0f,
              "a freshly live doctrine is worth exactly what it always was");

        ap.turnsHeld = 30;
        const float matured = game.policyTenure(ap);
        if (on) {
            check(matured > fresh, "holding it makes it stronger");
            check(std::fabs(matured - 1.5f) < 1e-6f, "and tops out at half again");
            ap.turnsHeld = 3000;
            check(std::fabs(game.policyTenure(ap) - 1.5f) < 1e-6f,
                  "the cap holds however long it is kept");
            // The half-way point, so the curve is a ramp and not a step.
            ap.turnsHeld = 15;
            const float half = game.policyTenure(ap);
            check(half > 1.0f && half < 1.5f, "it ramps rather than stepping");
        } else {
            // THE PROPERTY THAT MATTERS WITH THE FLAG OFF: not "about the same"
            // but bit-identical, because getTotalEffect multiplies by this and
            // the decision hash pins that float (journal 373).
            check(matured == 1.0f, "with the flag off, tenure changes nothing at all");
            ap.turnsHeld = 3000;
            check(game.policyTenure(ap) == 1.0f, "...however long it is held");
        }

        // Tenure is only what being IN FORCE buys. A doctrine still being
        // enacted, or one already dropped, scales nothing -- otherwise
        // cancelling would keep paying out.
        ap.turnsHeld = 30;
        ap.turnsRemaining = 2;
        check(game.policyTenure(ap) == 1.0f, "a doctrine still implementing earns none");
        ap.turnsRemaining = -1;
        check(game.policyTenure(ap) == 1.0f, "a dropped doctrine earns none");
    }

    // ── the other half: what a doctrine costs while it is being built ──
    //
    // The bill must never be able to exceed the sticker price, and it must
    // never go DOWN as the work progresses -- a country that is charged less
    // the closer it gets to finishing would be paid to stall.
    void upkeep() {
        const bool on = game.doctrineCommitment();
        printf("\n-- the phased bill (flag %s) --\n", on ? "ON" : "off");

        Policy* p = find("professional_army");
        if (!p) { check(false, "the fixture's doctrine exists"); return; }
        p->costPerTurn = 40;
        p->implementationTurns = 4;
        const float full = 40.0f;

        ActivePolicy ap;
        ap.countryId = cid;
        ap.policyId = p->id;

        ap.turnsRemaining = 0;                       // in force
        check(game.policyUpkeep(ap, *p) == full, "in force, it costs its full price");
        ap.turnsRemaining = -1;                      // repealed
        check(game.policyUpkeep(ap, *p) == full, "a repealed doctrine is not discounted");
        ap.turnsRemaining = -3;                      // propaganda still running
        check(game.policyUpkeep(ap, *p) == full, "a running propaganda term is not either");

        // Walk the build from signed to finished. turnsRemaining counts DOWN.
        float prev = -1.0f;
        bool monotone = true, bounded = true;
        for (int left = p->implementationTurns; left >= 1; --left) {
            ap.turnsRemaining = left;
            const float bill = game.policyUpkeep(ap, *p);
            if (bill < prev) monotone = false;
            if (bill > full) bounded = false;
            prev = bill;
        }
        check(monotone, "the bill never falls as the work goes on");
        check(bounded, "and never rises above the price on the tin");

        ap.turnsRemaining = p->implementationTurns;  // signed this turn, nothing built
        const float first = game.policyUpkeep(ap, *p);
        ap.turnsRemaining = 1;                       // one turn from being in force
        const float last = game.policyUpkeep(ap, *p);
        if (on) {
            check(first == 0.0f, "the turn you sign it, it costs nothing yet");
            check(last > first && last < full, "and the bill is nearly full by the end");
            // A doctrine with no implementation period cannot be part-built.
            p->implementationTurns = 0;
            ap.turnsRemaining = 3;
            check(game.policyUpkeep(ap, *p) == full,
                  "a doctrine with no build period pays in full from the start");
            p->implementationTurns = 4;
        } else {
            // Bit-identical, not merely close: policyCosts feeds the austerity
            // rules and the decision hash pins what they do.
            check(first == full && last == full,
                  "with the flag off, a doctrine costs the same every turn of its life");
        }
    }

    // ── political capital: the room a government did not use ──
    //
    // This rule RELIEVES a gate, so the only way it can be wrong is by
    // refusing something that used to be allowed. Every case below is
    // therefore about the ceiling never going DOWN, and about the bank never
    // going negative -- a negative bank would be a debt, which is a new gate,
    // which is the one thing this must not add.
    void capital() {
        const bool on = game.politicalCapitalOn();
        printf("\n-- political capital (rule %s) --\n", on ? "ON" : "off");

        CountryIncomeSnapshot inc;
        inc.total = 200.0f;
        inc.policyCosts = 4.0f;
        inc.minorityCosts = 5.0f;
        inc.pacificationCost = 1.0f;
        check(Game::politicsCommitted(inc) == 10.0f,
              "what politics costs is doctrines, minorities and pacification");
        // Research is the biggest bill in the game and is deliberately NOT in
        // it: charging it here would price a quarter of gross against a
        // ceiling research alone already exceeds.
        inc.researchCost = 90.0f;
        check(Game::politicsCommitted(inc) == 10.0f, "and not research");

        const float bare = 200.0f * Game::kPoliticsShare;
        game.m_politicalCapital.clear();
        check(game.politicsCeiling(inc, cid) == bare,
              "with nothing banked the ceiling is exactly the share");

        game.m_politicalCapital[cid] = 12.0f;
        const float withBank = game.politicsCeiling(inc, cid);
        if (on) {
            check(withBank == bare + 12.0f, "a bank raises the ceiling by what it holds");
            check(game.politicalCapital(cid) == 12.0f, "and can be read back");
        } else {
            // BIT-IDENTICAL, not approximately: this ceiling is what decides
            // whether the AI is offered a doctrine at all, and the decision
            // hash pins that.
            check(withBank == bare, "with the rule off a bank cannot change the ceiling");
            check(game.politicalCapital(cid) == 0.0f, "and reads as empty whatever is stored");
        }

        // Accrual, through the real per-turn entry point.
        game.m_politicalCapital.clear();
        // Against the REAL snapshot, because a fresh map's country may have no
        // income at all and then there is nothing to bank -- asserting "it
        // banks something" would then be a claim about the fixture, not the
        // rule. This asserts the rule either way round.
        const CountryIncomeSnapshot real = game.computeCountryIncome(cid);
        const float realShare = std::max(0.0f, real.total * Game::kPoliticsShare);
        const bool hadRoom = realShare > Game::politicsCommitted(real);
        printf("      (fixture: total %.2f, committed %.2f, room %s)\n",
               real.total, Game::politicsCommitted(real), hadRoom ? "yes" : "no");
        game.updatePoliticalCapital(cid);
        const float afterQuiet = game.politicalCapital(cid);
        if (on) {
            check(hadRoom ? afterQuiet > 0.0f : afterQuiet == 0.0f,
                  hadRoom ? "a turn with room to spare banks some of it"
                          : "a turn with no room to spare banks nothing");
            // Many quiet turns must stop somewhere, or one old country would
            // carry a ceiling nothing else could reach.
            for (int i = 0; i < 400; ++i) game.updatePoliticalCapital(cid);
            const float capped = game.politicalCapital(cid);
            check(capped >= afterQuiet, "the bank does not shrink on a quiet turn");
            for (int i = 0; i < 50; ++i) game.updatePoliticalCapital(cid);
            check(game.politicalCapital(cid) == capped, "and stops at a cap");
        } else {
            check(afterQuiet == 0.0f, "with the rule off nothing is ever banked");
        }

        // A bank is drawn down, never driven below empty.
        game.m_politicalCapital[cid] = 0.0f;
        for (int i = 0; i < 20; ++i) game.updatePoliticalCapital(cid);
        check(game.politicalCapital(cid) >= 0.0f, "a bank never goes negative");

        // ── the arithmetic itself ──
        //
        // Driven directly, because the fixture's country has no income (see
        // the line printed above) and so cannot reach the accrual through
        // updatePoliticalCapital. This is where banking, the cap and the
        // draw-down are actually pinned; the rule's flag does not reach here.
        const float share = 40.0f;
        check(Game::bankAfterTurn(0.0f, share, 10.0f) > 0.0f,
              "unused room is banked");
        check(Game::bankAfterTurn(0.0f, share, share) == 0.0f,
              "a turn that used all its room banks nothing");
        // Monotone in what was left over: banking less when MORE was spare
        // would pay a government for spending.
        check(Game::bankAfterTurn(0.0f, share, 5.0f) >
              Game::bankAfterTurn(0.0f, share, 25.0f),
              "the quieter the turn, the more is banked");
        // Overspending draws it down, by the whole overspend rather than a
        // fraction of it -- the discount is on saving, not on borrowing.
        const float drawn = Game::bankAfterTurn(30.0f, share, share + 10.0f);
        check(drawn == 20.0f, "going over the share spends the bank, in full");
        check(Game::bankAfterTurn(3.0f, share, share + 100.0f) == 0.0f,
              "and cannot take it below empty");
        // A cap, and one that is reached by repeating quiet turns rather than
        // asserted from the constant -- the constant may change; the property
        // that it stops must not.
        float b = 0.0f;
        for (int i = 0; i < 500; ++i) b = Game::bankAfterTurn(b, share, 0.0f);
        const float settled = b;
        for (int i = 0; i < 50; ++i) b = Game::bankAfterTurn(b, share, 0.0f);
        check(b == settled && settled > 0.0f, "quiet turns fill the bank and then stop");
        // No income means no ceiling and no bank: a collapsed country cannot
        // bank political room it has no economy to generate.
        check(Game::bankAfterTurn(settled, 0.0f, 0.0f) == 0.0f,
              "a country with no income holds no bank");
    }

    // Tenure is only worth anything if it OUTLIVES A SAVE. A number that
    // resets every time the player loads is a mechanic that punishes closing
    // the game, so the round trip is part of the rule, not part of the I/O.
    //
    // Run last: loadStateJsonBody replaces the world.
    void survivesASave() {
        printf("\n-- tenure across a save --\n");
        reset();
        makeAlwaysAffordable("professional_army");   // the gate under test is the save, not the budget
        game.enactPolicy(cid, "professional_army");
        if (game.m_activePolicies.empty()) { check(false, "the fixture enacted one"); return; }
        game.m_activePolicies[0].turnsRemaining = 0;
        game.m_activePolicies[0].turnsHeld = 17;

        const std::string saved = game.saveStateJson();
        game.loadStateJsonBody(saved);

        const ActivePolicy* back = nullptr;
        for (const auto& ap : game.m_activePolicies)
            if (ap.countryId == cid && ap.policyId == "professional_army") back = &ap;
        check(back != nullptr, "the doctrine comes back");
        if (back) check(back->turnsHeld == 17, "and remembers how long it has been held");

        // A campaign saved before tenure existed has no such field. It must
        // read as new-to-the-rule rather than as garbage -- never as a number
        // inherited from whatever ActivePolicy happened to be there.
        std::string older = saved;
        for (size_t at = older.find("\"turnsHeld\":"); at != std::string::npos;
             at = older.find("\"turnsHeld\":", at))
            older.replace(at, 12, "\"turnsOld_\":");
        game.loadStateJsonBody(older);
        const ActivePolicy* old = nullptr;
        for (const auto& ap : game.m_activePolicies)
            if (ap.countryId == cid && ap.policyId == "professional_army") old = &ap;
        check(old && old->turnsHeld == 0,
              "a save written before the rule existed starts at zero");
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
    t.tenure();
    t.upkeep();
    t.capital();
    t.survivesASave();

    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
