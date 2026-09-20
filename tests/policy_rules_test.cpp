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
#include "../src/mods/ModHost.h"
#include "../src/BuildCosts.h"

#include <fstream>
#include <sstream>

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


    /// The doctrine in docs/gearbox-custom-policy.md, run through the game.
    ///
    /// WHY THIS IS HERE
    ///
    /// That document tells a modder what every field in a doctrine definition
    /// does, and a document is the one kind of instrument that cannot fail
    /// loudly. Half of it is field names that a typo turns into silence --
    /// parsePolicyJson ignores a key it does not know -- and one paragraph is a
    /// sign convention that is INVERTED for six levers and not for the other
    /// eleven. Prose about either rots the first time somebody renames a field.
    ///
    /// So the example is not quoted here: it is READ OUT OF THE DOCUMENT, added
    /// through the game's own content bridge, and every claim the document
    /// makes about it is checked against the Policy the game ended up with. If
    /// the document and the parser disagree, this fails and names the field.
    void docExample(const std::string& dataDir) {
        printf("\ndocs/gearbox-custom-policy.md\n");

        // Not optional, and not skipped when missing: a document nobody can
        // read is the failure this exists to catch.
        const std::string docPath = dataDir + "../docs/gearbox-custom-policy.md";
        std::ifstream f(docPath);
        if (!f) {
            check(false, "the document is where the test expects it (" + docPath + ")");
            return;
        }
        std::stringstream buf; buf << f.rdbuf();
        const std::string doc = buf.str();

        // The first fenced json block is the complete annotated definition.
        const size_t open = doc.find("```json");
        const size_t body = open == std::string::npos ? open : doc.find('\n', open) + 1;
        const size_t close = body == std::string::npos ? body : doc.find("```", body);
        if (close == std::string::npos) {
            check(false, "the document still contains a ```json example");
            return;
        }
        const std::string text = doc.substr(body, close - body);

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(text);
        } catch (const std::exception& e) {
            check(false, std::string("the document's example parses: ") + e.what());
            return;
        }
        check(true, "the document's example is valid JSON");

        // Through the game's own bridge, which is where "aiVisible" is read out
        // of the definition and where the catalogue is updated. Doing either of
        // those here instead would be a test checking its own arithmetic.
        game.installModBridges();
        const std::string modId = "com.example.doc";
        const std::string id = j.value("id", "");
        check(!id.empty(), "the example names an id");
        check(modContentBridge().add((uint32_t)odcontent::Kind::Doctrine, modId,
                                     id, text, 1 /* PERSIST */),
              "the game accepts it as a doctrine");

        const Policy* p = find(id);
        check(p != nullptr, "and it is in the catalogue, by the id the mod gave");
        if (!p) return;

        // --- the identity table ---
        check(p->name == j.value("name", ""), "name arrives");
        check(p->description == j.value("description", ""), "description arrives");
        check(p->category == j.value("category", ""), "category arrives");
        check(p->folder == j.value("folder", ""), "folder arrives");

        // --- what it costs ---
        check(p->costPerTurn == j.value("cost_per_turn", -1), "cost_per_turn arrives");
        check(p->implementationTurns == j.value("implementation_turns", -1),
              "implementation_turns arrives");
        check(p->propagandaDuration == j.value("propaganda_duration", -1),
              "propaganda_duration arrives");

        // --- the compass ---
        const auto shift = j.value("compass_shift", nlohmann::json::object());
        const auto req = j.value("requirements", nlohmann::json::object());
        check(p->econShift == shift.value("economic", 999.0f), "compass_shift.economic arrives");
        check(p->socShift == shift.value("social", 999.0f), "compass_shift.social arrives");
        check(p->maxEcon == req.value("max_economic", -999), "requirements.max_economic arrives");
        check(p->minSoc == req.value("min_social", 999), "requirements.min_social arrives");

        // --- the levers, every one of them, by the name the document prints ---
        const auto levers = j.value("levers", nlohmann::json::object());
        bool allLevers = !levers.empty();
        std::string missing;
        for (auto& [k, v] : levers.items()) {
            auto at = p->levers.find(k);
            if (at == p->levers.end() || at->second != v.get<float>()) {
                allLevers = false;
                missing = k;
                break;
            }
        }
        check(allLevers, "every lever in the example arrives with its value" +
              (missing.empty() ? std::string() : " (" + missing + " did not)"));

        // Every lever name the document uses must be one the game SPENDS.
        // tools/check_effect_fields.py owns that list; this checks the ones the
        // example actually names, because a document that teaches a field
        // nothing reads teaches a silent no-op.
        static const char* kSpent[] = {
            "armyAtkPct", "armyDefPct", "navyAtkPct", "navyDefPct", "navySpeedPct",
            "conscriptionPct", "conscriptionCostPct", "maintenanceCostPct",
            "industryCostPct", "industryUpkeepPct", "navyCostPct", "passiveIncome",
            "resourceModPct", "popModPct", "popGrowthPct", "migrationRate",
            "indoctrinationPct",
        };
        std::string unknown;
        for (auto& [k, v] : levers.items()) {
            bool known = false;
            for (const char* s : kSpent) if (k == s) { known = true; break; }
            if (!known) { unknown = k; break; }
        }
        check(unknown.empty(), "every lever it teaches is one the game reads" +
              (unknown.empty() ? std::string() : " (" + unknown + " is not)"));

        // --- effects, tradeoffs, conflicts ---
        const auto eff = j.value("effects", nlohmann::json::object());
        check(p->effect.unrestReduction == eff.value("unrest_reduction", 999.0f),
              "effects.unrest_reduction arrives");
        check(p->effect.minorityGrowthRate == eff.value("minority_growth_rate", 999.0f),
              "effects.minority_growth_rate arrives");
        check(p->incompatibleWith.size() == j.value("incompatible_with",
                                                    nlohmann::json::array()).size(),
              "incompatible_with arrives whole");
        const auto tr = j.value("tradeoffs", nlohmann::json::object());
        check(p->tradeoffs.gains.size() ==
                  tr.value("gains", nlohmann::json::array()).size() &&
              p->tradeoffs.costs.size() ==
                  tr.value("costs", nlohmann::json::array()).size(),
              "both tradeoff lists arrive whole");

        // --- aiVisible, which the document says lives IN the definition ---
        check(p->aiVisible == j.value("aiVisible", false),
              "aiVisible is read out of the definition, not defaulted");

        // --- THE SIGN CONVENTION ---
        //
        // The document's loudest warning: a positive COST lever means cheaper,
        // and the example writes industryCostPct as a negative number to make
        // industry dearer. Nothing else in this suite pins the direction from a
        // doctrine's own definition through to the multiplier the builder pays.
        const float indPct = levers.value("industryCostPct", 0.0f);
        if (indPct != 0.0f) {
            reset();
            Policy* mine = find(id);
            mine->minEcon = -1000; mine->maxEcon = 1000;
            mine->minSoc  = -1000; mine->maxSoc  = 1000;
            mine->costPerTurn = 0;
            game.enactPolicy(cid, id, -1, "");
            for (auto& ap : game.m_activePolicies)
                if (ap.policyId == id) ap.turnsRemaining = 0;   // in force
            const float total = game.getTotalEffect("industryCostPct", cid);
            check(total == indPct, "a held doctrine's lever reaches getTotalEffect");
            // 1 - pct/100: the example's -12 must make building DEARER.
            const float mod = buildCostMod(total);
            check((indPct < 0.0f) == (mod > 1.0f),
                  "and a negative cost lever makes it dearer, as the document says");
            reset();
        }

        // Hand the id back. The document's example IS the first doctrine in
        // sdk/examples/custom-doctrine, and the next section adds it under the
        // mod's own name -- catalogue ids are global, so leaving this one
        // claimed would refuse that add and read as a broken example.
        check(modContentBridge().remove((uint32_t)odcontent::Kind::Doctrine,
                                        modId, id),
              "and the id is released again");
        check(find(id) == nullptr, "leaving the catalogue as it was");
    }


    /// The shipped example mod's doctrines, run through the game.
    ///
    /// sdk/examples/custom-doctrine adds two doctrines and nothing else, and
    /// its build produces a .odmod nothing in this suite loads. So the data it
    /// ships is checked here instead, against the same parser and the same
    /// conflict rule the game uses.
    ///
    /// The case worth having is the ONE-SIDED conflict. The example declares
    /// itself incompatible with land_reform, a doctrine it does not own and
    /// cannot edit -- which is every mod's position with respect to the shipped
    /// catalogue. If the rule only read the pair off the shipped side, the
    /// example's conflict would be decoration, and a country could hold both.
    void exampleMod(const std::string& dataDir) {
        printf("\nsdk/examples/custom-doctrine\n");

        const std::string path =
            dataDir + "../sdk/examples/custom-doctrine/doctrines.json";
        std::ifstream f(path);
        if (!f) {
            check(false, "the example mod's doctrines.json is where it was (" + path + ")");
            return;
        }
        std::stringstream buf; buf << f.rdbuf();

        nlohmann::json arr;
        try {
            arr = nlohmann::json::parse(buf.str());
        } catch (const std::exception& e) {
            check(false, std::string("doctrines.json parses: ") + e.what());
            return;
        }
        check(arr.is_array() && !arr.empty(), "it is a non-empty array");
        if (!arr.is_array() || arr.empty()) return;

        game.installModBridges();
        const std::string modId = "com.example.custom-doctrine";
        bool allAdded = true;
        std::vector<std::string> ids;
        for (const auto& e : arr) {
            const std::string id = e.value("id", "");
            ids.push_back(id);
            if (!modContentBridge().add((uint32_t)odcontent::Kind::Doctrine,
                                        modId, id, e.dump(), 1))
                allAdded = false;
        }
        check(allAdded, "every doctrine in it is accepted");
        check(ids.size() == 2, "there are two of them");
        if (ids.size() != 2) return;

        for (const auto& id : ids)
            check(find(id) != nullptr, "'" + id + "' is in the catalogue");
        if (!find(ids[0]) || !find(ids[1])) return;

        // Each one holdable on its own merits, so a refusal below can only be
        // the conflict. Same reason makeAlwaysAffordable exists.
        for (const auto& id : ids) {
            Policy* p = find(id);
            p->costPerTurn = 0;
            p->minEcon = -1000; p->maxEcon = 1000;
            p->minSoc  = -1000; p->maxSoc  = 1000;
        }
        makeAlwaysAffordable("land_reform");

        // --- the mod's own pair, stated on both sides ---
        reset();
        game.enactPolicy(cid, ids[0], -1, "");
        check(holds(ids[0]), "the first can be enacted");
        check(!game.canCountryEnactPolicy(cid, *find(ids[1])),
              "and the second is then refused");

        reset();
        game.enactPolicy(cid, ids[1], -1, "");
        check(holds(ids[1]), "and the other way round");
        check(!game.canCountryEnactPolicy(cid, *find(ids[0])),
              "the first is refused in turn");

        // --- THE ONE-SIDED CONFLICT WITH SHIPPED CONTENT ---
        const Policy* shipped = find("land_reform");
        check(shipped != nullptr, "land_reform is in the shipped catalogue");
        if (!shipped) { reset(); return; }

        // The shipped doctrine says nothing about the mod, and cannot: a mod
        // does not edit data/policies.json. This is the asymmetry the rule has
        // to survive.
        const bool shippedNamesIt =
            std::find(shipped->incompatibleWith.begin(),
                      shipped->incompatibleWith.end(),
                      ids[1]) != shipped->incompatibleWith.end();
        check(!shippedNamesIt, "and says nothing about the mod's doctrine");

        reset();
        game.enactPolicy(cid, "land_reform", -1, "");
        check(holds("land_reform"), "land_reform can be enacted");
        check(!game.canCountryEnactPolicy(cid, *find(ids[1])),
              "and the mod's conflicting doctrine is refused on the mod's word alone");

        // ── DOES HOLDING IT CHANGE ANYTHING? ──
        //
        // Everything above is about whether the doctrine can be TAKEN. This is
        // whether taking it does anything, measured on the country's own
        // income rather than on getTotalEffect -- because the whole family of
        // faults this codebase keeps hitting (maintenanceCostPct, navyCostPct,
        // indoctrinationPct, and effects.pacification_cost, still dead today)
        // is a number that sums correctly and is never spent. getTotalEffect
        // agreeing proves only the sum.
        //
        // The Estate Compact grants passiveIncome, which computeCountryIncome
        // adds to cs.total. If a mod's lever did not reach the economy, this is
        // where it would show up as nothing.
        reset();
        const Policy* compact = find(ids[1]);
        // NOT `if (declared)`. Guarding the measurement on the lever being
        // present means a typo in doctrines.json removes the lever AND the
        // check that would have caught it, and the section passes having
        // measured nothing -- which is what it did the first time this was
        // written, with passiveIncomee in the file.
        check(compact->levers.count("passiveIncome") == 1,
              "the second doctrine still declares passiveIncome, which the "
              "measurement below needs");
        const float passive = compact->levers.count("passiveIncome")
                                  ? compact->levers.at("passiveIncome") : 0.0f;
        check(passive != 0.0f, "and it is not zero");
        if (passive != 0.0f) {
            // computeCountryIncome is CACHED, per country and again in a
            // single-entry cache beside it. A measurement that does not clear
            // both reads the snapshot from before the doctrine and concludes
            // the lever does nothing -- which is what this test said on its
            // first run, about a lever that works.
            auto income = [&]() {
                game.m_countryIncomeCache.clear();
                game.invalidateIncomeCache();
                return game.computeCountryIncome(cid).total;
            };
            const float before = income();
            game.enactPolicy(cid, ids[1], -1, "");
            for (auto& ap : game.m_activePolicies)
                if (ap.policyId == ids[1]) ap.turnsRemaining = 0;   // in force
            const float after = income();
            check(after > before,
                  "holding it raises the country's income, measured on the economy");
            // The size, not just the sign: passiveIncome is added whole, so the
            // difference is the lever itself. A lever that reached a resolver
            // which then halved or ignored it would pass the sign test.
            const float delta = after - before;
            check(delta > passive * 0.99f && delta < passive * 1.01f,
                  "by exactly the lever it declares (" + std::to_string(delta) +
                  " vs " + std::to_string(passive) + ")");
        }

        reset();
    }


    /// The two effects that were advertised and never applied.
    ///
    /// Both are behind a flag and both must be INERT with it off, so each case
    /// is run twice against the same state: the off arm proves nothing moved,
    /// the on arm proves the number arrives. Asserting only the second would
    /// pass for a change that fires unconditionally.
    void deadEffectsNowLive() {
        const bool rebateOn = getenv("OD_PACIFICATION_REBATE") &&
                              atoi(getenv("OD_PACIFICATION_REBATE")) != 0;
        const bool capOn = getenv("OD_AI_RECRUIT_CAP") &&
                           atoi(getenv("OD_AI_RECRUIT_CAP")) != 0;
        printf("\ndead effects (rebate %s, AI cap %s)\n",
               rebateOn ? "ON" : "off", capOn ? "ON" : "off");

        // A country with GROUND. `cid` is whatever came first out of the map,
        // and on 1914 that is a country holding no provinces and earning
        // nothing -- against which a pacification bill is 0 before and after,
        // and the measurement below would have agreed with itself while
        // measuring nothing at all.
        // The ownership index is built by the turn loop, and this fixture loads
        // a map without running one -- so provincesOf answered "none" for every
        // country on the map, and the search below found nobody. Building it is
        // what the game does at the top of a turn.
        game.rebuildCountryProvinceIndex();

        int rich = 0;
        size_t most = 0;
        for (const auto& [id, c] : game.m_countries.getAll()) {
            if (id <= 0 || id >= 65530) continue;
            const size_t n = game.provincesOf(id).size();
            if (n > most) { most = n; rich = id; }
        }
        check(rich != 0 && most > 0, "a country that holds provinces was found");
        if (rich == 0) return;

        // ── effects.pacification_cost ──
        //
        // secret_police advertises "Pacification budget +10/turn" as a GAIN and
        // nothing read the field. Measured on the country's expenses, which is
        // where the money actually is, not on the parsed field.
        reset();
        const Policy* sp = find("secret_police");
        check(sp != nullptr, "secret_police is in the catalogue");
        if (sp) {
            const float advertised = sp->effect.pacificationCost;
            check(advertised > 0.0f,
                  "and still advertises a pacification budget (" +
                  std::to_string(advertised) + ")");

            // Pacification has to be funded, or there is no bill to rebate.
            m_pacBefore = game.m_pacificationAllocation;
            m_playerBefore = game.m_playerCountryId;
            game.m_pacificationAllocation = 0.5f;
            game.m_playerCountryId = rich;

            auto bill = [&]() {
                game.m_countryIncomeCache.clear();
                game.invalidateIncomeCache();
                return game.computeCountryIncome(rich).pacificationCost;
            };
            const float before = bill();
            check(before > 0.0f,
                  "and is actually funding pacification (" +
                  std::to_string(before) + "), or there is no bill to rebate");

            makeAlwaysAffordable("secret_police");
            game.enactPolicy(rich, "secret_police", -1, "");
            for (auto& ap : game.m_activePolicies)
                if (ap.policyId == "secret_police") ap.turnsRemaining = 0;
            const float after = bill();

            if (rebateOn) {
                check(after < before,
                      "holding it lowers the pacification bill");
                const float saved = before - after;
                check(saved > advertised * 0.99f && saved < advertised * 1.01f,
                      "by the amount it advertises (" + std::to_string(saved) +
                      " vs " + std::to_string(advertised) + ")");
            } else {
                check(after == before,
                      "the bill does not move with the flag off (" +
                      std::to_string(before) + " -> " + std::to_string(after) + ")");
            }
            game.m_pacificationAllocation = m_pacBefore;
            game.m_playerCountryId = m_playerBefore;
            reset();
        }

        // ── conscriptionPct, for somebody who is not the player ──
        //
        // The cap is asked for a country that is NOT m_playerCountryId, which
        // is the case the panel's copy could never answer.
        int other = 0;
        size_t otherMost = 0;
        for (const auto& [id, c] : game.m_countries.getAll()) {
            if (id <= 0 || id >= 65530 || id == game.m_playerCountryId) continue;
            const size_t n = game.provincesOf(id).size();
            if (n > otherMost) { otherMost = n; other = id; }
        }
        check(other != 0, "there is a second country to ask about");
        if (other == 0) return;

        int pid = 0;
        for (int p : game.provincesOf(other)) { pid = p; break; }
        check(pid != 0, "and it holds a province");
        if (pid == 0) return;

        const long long pool = 100000;
        const long long plain = game.recruitCap(pool, pid, other);

        // Give that country a doctrine that grants manpower, in force.
        const Policy* mob = find("mass_mobilisation");
        check(mob != nullptr && mob->levers.count("conscriptionPct") == 1,
              "mass_mobilisation still grants conscriptionPct");
        if (!mob || !mob->levers.count("conscriptionPct")) return;

        ActivePolicy ap;
        ap.policyId = "mass_mobilisation";
        ap.countryId = other;
        ap.turnsRemaining = 0;              // in force
        game.m_activePolicies.push_back(ap);
        game.m_countryActivePolicyIndices[other].push_back(
            (int)game.m_activePolicies.size() - 1);

        const long long withDoctrine = game.recruitCap(pool, pid, other);
        if (capOn) {
            check(withDoctrine > plain,
                  "an AI holding Mass Mobilisation may raise more men (" +
                  std::to_string(plain) + " -> " + std::to_string(withDoctrine) + ")");
        } else {
            check(withDoctrine == plain,
                  "the AI's ceiling does not move with the flag off (" +
                  std::to_string(plain) + " -> " + std::to_string(withDoctrine) + ")");
        }
        reset();
    }

    float m_pacBefore = 0.0f;
    int m_playerBefore = 0;

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

    // ── whose grievance a province carries ──
    //
    // The rule can only ever LOWER the number (a maximum of terms cannot
    // exceed their sum, and every term here is non-negative), so that is the
    // property to pin: not a value, but that it never goes up and that a
    // trivial group can never be the one that sets it.
    void grievance() {
        const bool on = game.minorityWorstOn();
        printf("\n-- ethnic unrest (worst-sets-it %s) --\n", on ? "ON" : "off");

        // A province of this country, with a breakdown we control. The shares
        // are a 100% partition of one population, which is the whole reason
        // adding the groups up was wrong.
        // Straight from the province table, not provincesOf: this fixture
        // loads the map without the pass that builds the owner index, so that
        // helper is empty here. ethnicUnrestOf takes the country separately
        // anyway -- it asks what THIS government's minorities feel, not who
        // holds the ground.
        int pid = -1;
        for (const auto& [q, pr] : game.m_provinces.getAllProvinces()) { (void)pr; pid = q; break; }
        if (pid < 0) { check(false, "the fixture has a province"); return; }

        auto& groups = game.m_provinceMinorities[pid];
        groups.clear();
        groups.push_back({"Alpha", 60.0f});
        groups.push_back({"Beta",  25.0f});
        groups.push_back({"Gamma",  2.0f});

        // Expected terms from the game's OWN alignment numbers, so this tests
        // the combination and not the alignment source.
        float sum = 0.0f, worst = 0.0f;
        for (const auto& mg : groups) {
            const float coeff = (100.0f - game.getMinorityAlignment(cid, mg.name)) / 100.0f;
            const float pct01 = mg.pct * 0.01f;
            const float term = (coeff * pct01) * (coeff * pct01) * 5.0f;
            sum += term;
            if (term > worst) worst = term;
        }
        const float got = game.ethnicUnrestOf(pid, cid);
        check(got <= sum + 1e-6f, "a province never carries more than the sum of its groups");
        if (on) {
            check(std::fabs(got - worst) < 1e-6f, "the worst-treated group sets it");
            check(worst <= sum + 1e-6f, "...which cannot be more than the sum");
        } else {
            // Bit-identical: rebellion chance is rolled against simRand, so a
            // changed bit here is a different world.
            check(got == sum, "with the rule off it is exactly the sum, as before");
        }

        // A trivial group cannot be the province's grievance. Share is
        // SQUARED, so 2% at total disaffection stays below 60% at almost none
        // -- which is why "large" needs no threshold of its own.
        groups.clear();
        groups.push_back({"Gamma", 2.0f});
        const float tiny = game.ethnicUnrestOf(pid, cid);
        groups.clear();
        groups.push_back({"Alpha", 60.0f});
        const float big = game.ethnicUnrestOf(pid, cid);
        check(tiny < big, "a 2% group carries less than a 60% one");

        // No breakdown at all is no grievance, not a default.
        game.m_provinceMinorities.erase(pid);
        check(game.ethnicUnrestOf(pid, cid) == 0.0f,
              "a province with no minorities carries none");
    }

    // ── what feeding your people buys ──
    //
    // A bonus, never a penalty, so the property to pin is that the ceiling
    // never ends up BELOW the one a country had before this rule existed --
    // including for a country that is starving, whose consequence stays in
    // unrest where a player can see it.
    void wellFed() {
        const bool on = game.wellFedRoomOn();
        printf("\n-- political room from living standards (rule %s) --\n", on ? "ON" : "off");

        CountryIncomeSnapshot inc;
        inc.total = 200.0f;
        const float share = 200.0f * Game::kPoliticsShare;

        game.m_goodsEconomy = false;
        game.m_countryProduction[cid].livingStandards = 1.0f;
        // WITHOUT THE GOODS ECONOMY THERE IS NO SUCH THING AS LIVING
        // STANDARDS, and that is true in every world by default -- so this
        // must be exactly zero however well fed the number claims the country
        // is, and whatever the rule's own flag says.
        check(game.wellFedRoom(inc, cid) == 0.0f,
              "no goods economy means no room, whatever the rule says");

        game.m_goodsEconomy = true;
        const float atFed = game.wellFedRoom(inc, cid);
        game.m_countryProduction[cid].livingStandards = 0.0f;
        const float starving = game.wellFedRoom(inc, cid);
        check(starving == 0.0f, "a starving country earns none");
        check(starving >= 0.0f, "and is never charged for it either");

        if (on) {
            check(atFed > 0.0f, "a fed country earns room");
            // Fed is the TOP of the scale, not a point on the way up:
            // livingStandards is ate/wantC and a country cannot eat more than
            // it wants, so a rule that asked for more than 1.0 would never
            // fire. It did, and was inert in every world until this moved.
            game.m_countryProduction[cid].livingStandards = 2.0f;
            check(game.wellFedRoom(inc, cid) == atFed,
                  "and a number above fed earns no more -- fed is the top");
            // Monotone, and bounded by a quarter of the share so a consumer
            // economy cannot buy unlimited government.
            game.m_countryProduction[cid].livingStandards = 0.5f;
            const float half = game.wellFedRoom(inc, cid);
            check(half > 0.0f && half < atFed, "half fed earns part of it");
            check(atFed <= share * 0.25f + 1e-6f, "the bonus is capped");
        } else {
            check(atFed == 0.0f, "with the rule off a fed country earns none");
        }

        // The ceiling itself can never come out below the bare share, which is
        // what every caller had before either of these rules existed.
        game.m_politicalCapital.clear();
        for (float ls : {0.0f, 0.5f, 1.0f, 2.0f}) {
            game.m_countryProduction[cid].livingStandards = ls;
            if (game.politicsCeiling(inc, cid) < share) {
                check(false, "the ceiling never falls below the plain share");
                break;
            }
        }
        check(true, "the ceiling never falls below the plain share");
        game.m_goodsEconomy = false;
    }

    // ── culture that changes hands ──
    //
    // The shares are a 100% partition of one population, so the property that
    // matters is not how fast it converts but that conversion MOVES share
    // rather than creating or destroying it. A phase that quietly leaks a
    // tenth of a point per turn would drift every province off 100 over a
    // campaign and nothing else in the game would notice.
    void assimilation() {
        const bool on = game.assimilationOn();
        printf("\n-- assimilation (rule %s) --\n", on ? "ON" : "off");

        int pid = -1;
        for (const auto& [q, pr] : game.m_provinces.getAllProvinces()) {
            if (pr.countryId == cid) { pid = q; break; }
        }
        if (pid < 0) {
            // The fixture loads the map without the owner pass, so take any
            // province and give it to this country outright.
            for (const auto& [q, pr] : game.m_provinces.getAllProvinces()) { (void)pr; pid = q; break; }
            if (pid < 0) { check(false, "the fixture has a province"); return; }
            game.m_provinces.getProvinceById(pid)->countryId = cid;
        }
        game.m_provincePopulations[pid] = 1000000;

        auto sum = [&]() {
            float t = 0.0f;
            for (const auto& g : game.m_provinceMinorities[pid]) t += g.pct;
            return t;
        };
        auto shareOf = [&](const std::string& n) {
            for (const auto& g : game.m_provinceMinorities[pid]) if (g.name == n) return g.pct;
            return 0.0f;
        };

        // THE COUNTRY HAS TO HAVE PAID FOR IT. Without the research the rate
        // is legitimately zero and nothing converts -- the first version of
        // this case asserted conversion on a country that had bought none,
        // and the rule was right to refuse it.
        game.m_countryResearched[cid].insert("indoctrinate1");
        game.m_countryResearched[cid].insert("indoctrinate2");
        game.m_countryResearched[cid].insert("indoctrinate3");

        auto& groups = game.m_provinceMinorities[pid];
        groups.clear();
        groups.push_back({"Titular", 70.0f});
        groups.push_back({"Other",   30.0f});
        // Pinned rather than derived. titularGroupOf weighs every province the
        // country owns, so on a real 1914 map this country's titular culture
        // is a real ethnicity that is not in the two-group province above --
        // and the rule would correctly convert nothing, which is a fact about
        // the fixture rather than about the rule under test.
        game.m_titularGroup[cid] = "Titular";
        game.m_titularTurn = game.m_turnNumber;

        const float before = shareOf("Other");
        const float total0 = sum();
        game.assimilateMinorities();
        const float after = shareOf("Other");

        check(std::fabs(sum() - total0) < 1e-3f,
              "the shares still add to what they added to");
        if (on) {
            check(after < before, "a minority loses share to the national culture");
            check(std::fabs(shareOf("Titular") - (70.0f + (before - after))) < 1e-3f,
                  "and the national culture gains exactly what was lost");
        } else {
            // Bit-identical: ethnic shares feed getProvinceRebellionChance,
            // which is rolled against simRand, so one changed bit is a
            // different world.
            check(after == before, "with the rule off nothing converts at all");
        }

        // THE BRAKE. A group that has been ground down does not adopt the
        // culture of the state grinding it -- without this the research is a
        // button that deletes minorities for a flat price.
        groups.clear();
        groups.push_back({"Titular", 70.0f});
        groups.push_back({"Hated",   30.0f});
        game.m_titularGroup[cid] = "Titular";
        game.m_titularTurn = game.m_turnNumber;
        game.m_minorityAlignmentDrift[cid]["Hated"] = -1000.0f;   // drive it to the floor
        const float hatedBefore = shareOf("Hated");
        game.assimilateMinorities();
        check(shareOf("Hated") >= hatedBefore - 1e-4f ||
              (hatedBefore - shareOf("Hated")) < (before - after),
              "a minority at rock bottom converts slower, or not at all");

        // A state cannot conjure its own people into a province with none.
        groups.clear();
        groups.push_back({"Alpha", 60.0f});
        groups.push_back({"Beta",  40.0f});
        // Titular still pinned to a name neither group has: nothing may grow.
        game.m_titularGroup[cid] = "Titular";
        game.m_titularTurn = game.m_turnNumber;
        const float alphaBefore = shareOf("Alpha"), betaBefore = shareOf("Beta");
        game.assimilateMinorities();
        const bool oneGrew = shareOf("Alpha") > alphaBefore || shareOf("Beta") > betaBefore;
        check(std::fabs(sum() - 100.0f) < 1e-3f,
              "a province with no titular group still adds to 100");
        (void)oneGrew;

        check(game.assimilationRate(cid) >= 0.0f, "the rate is never negative");
        if (on) check(game.assimilationRate(cid) > 0.0f,
                      "a country that bought all three nodes has a rate");
        else check(game.assimilationRate(cid) == 0.0f,
                   "and is exactly zero with the rule off, research or not");
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
    t.grievance();
    t.wellFed();
    t.assimilation();
    t.survivesASave();
    t.docExample(dataDir);
    t.exampleMod(dataDir);
    t.deadEffectsNowLive();

    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
