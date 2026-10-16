// What the host does with orders no honest client would send.
//
//   OrderValidationTest <data-dir>
//
// WHY THIS TEST EXISTS
//
// mpApplyOrders is the boundary between a running world and bytes chosen by
// somebody else's machine. Every other check in the multiplayer stack is about
// WHO is talking -- join tickets, the nonce challenge, the pairwise pseudonym.
// This is the only place that decides what they are allowed to SAY, and until
// recently it decided almost nothing: ownership was checked properly and every
// number was taken at face value.
//
// The case that made this test necessary, verbatim from a modified client:
//
//     {"pendingUpgrades":[{"provinceId":<one you own>,"type":"industry",
//                          "targetLevel":999,"turnsRemaining":1}]}
//
// processUpgrades() assigns targetLevel straight into the province, and the
// price was only ever deducted in Game_Render.cpp -- on the machine that
// clicked the button. So that payload bought a level-999 factory, next turn,
// for nothing, on any server in the world.
//
// The fix was to make the host quote and pay for the build itself. This test is
// what stops that regressing, because the symptom would not look like a bug: it
// would look like somebody being very good at the game.
//
// It runs against the SERVER build (no GL), which is what lets a test load a
// real map and drive real turn state with no window.

#include "../src/Game.h"
#include "../src/net/NetProtocol.h"
#include "../src/net/TurnRunner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    printf("  %-64s %s\n", what.c_str(), ok ? "ok" : "FAILED");
    if (!ok) failures++;
}
std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }
}  // namespace

/** Friend of Game -- see the declaration there for why. */
struct OrderValidationTest {
    Game game;

    bool load(const std::string& dataDir) {
        game.m_headless = true;
        game.m_dataDir = dataDir;
        // Loading a map is enough: this exercises order intake, not a session.
        // loadFromODM is the same call the real load path makes; the renderer
        // half (loadMapPack) is what a server never runs.
        if (!game.loadFromODM(dataDir + "STDmaps/1914.odmap")) return false;
        game.loadGameDataStep1();
        game.loadGameDataStep2();
        return !game.m_countries.getAll().empty();
    }

    /** A province the given country owns, or 0. */
    int ownedProvince(int countryId) const {
        for (const auto& [pid, p] : game.m_provinces.getAllProvinces())
            if (p.countryId == countryId) return pid;
        return 0;
    }

    /// A province this country does NOT own, for the ownership cases.
    int foreignProvince(int countryId) const {
        for (const auto& [pid, p] : game.m_provinces.getAllProvinces())
            if (p.countryId > 0 && p.countryId != countryId && p.countryId < 65530) return pid;
        return 0;
    }

    int anyCountry() const {
        for (const auto& [cid, c] : game.m_countries.getAll())
            if (cid > 0 && cid < 65530) return cid;
        return 0;
    }

    void apply(int cid, const std::string& json) {
        game.mpApplyOrders(cid, bytes(json));
    }

    /// Two distinct provinces owned by one country. Geography is not the point
    /// here -- the placement bug is in resolveAssault, not the map -- so the
    /// caller links them as neighbours itself. Returns {countryId, from, to}.
    std::tuple<int,int,int> sameOwnerPair() const {
        for (const auto& [cid, c] : game.m_countries.getAll()) {
            if (cid <= 0 || cid >= 65530) continue;
            int first = 0;
            for (const auto& [pid, p] : game.m_provinces.getAllProvinces()) {
                if (p.countryId != cid) continue;
                if (!first) { first = pid; continue; }
                return {cid, first, pid};
            }
        }
        return {0, 0, 0};
    }

    long long troopsOfTypeAt(int pid, int cid, TroopType type) const {
        long long n = 0;
        auto it = game.m_provinceArmies.find(pid);
        if (it != game.m_provinceArmies.end())
            for (const ArmyUnit& u : it->second)
                if (u.countryId == cid && u.type == type) n += u.count;
        return n;
    }

    // ── A MARCHING ARMY KEEPS ITS KIND ──
    //
    // Reported from play (Pugz, itch.io): "troops to get transmuted into line
    // infantry whenever they move provinces". resolveAssault worked out WHO
    // survived by kind into survivorsOut, then placed only the headcount through
    // addTroopsTo, whose type defaults to TROOP_LINE -- so a mechanised column
    // that walked into the next province arrived as line infantry.
    void troopsKeepTheirKindOnMove() {
        // This fixture loads the map without the LOAD_COMPUTE_LABELS phase, so
        // m_provinceNeighbors is empty -- and processArmyMovement's adjacency
        // gate reads it. Build it the way the real load does.
        auto [cid, from, to] = sameOwnerPair();
        check(from != 0 && to != 0, "found two provinces one country owns");
        if (!from || !to) return;
        // Make the move legal: the adjacency gate in processArmyMovement reads
        // m_provinceNeighbors, which this map-only fixture never filled.
        game.m_provinceNeighbors[from] = {to};
        game.m_provinceNeighbors[to]   = {from};

        // A clean field: only our mechanised in `from`, nothing in `to`.
        game.m_provinceArmies.erase(from);
        game.m_provinceArmies.erase(to);
        game.m_pendingMoveOrders.clear();
        game.addTroopsTo(from, cid, 50000, TROOP_MECHANISED);
        check(troopsOfTypeAt(from, cid, TROOP_MECHANISED) == 50000,
              "50k mechanised are standing in the source");

        PendingMoveOrder mo;
        mo.fromProvince = from; mo.toProvince = to; mo.pct = 100;
        mo.countryId = cid; mo.troopType = -1;   // the whole garrison
        game.m_pendingMoveOrders.push_back(mo);
        game.processArmyMovement(cid);

        check(troopsOfTypeAt(to, cid, TROOP_MECHANISED) == 50000,
              "they arrive next door still mechanised");
        check(troopsOfTypeAt(to, cid, TROOP_LINE) == 0,
              "and NOT transmuted into line infantry");
        check(game.m_provinceArmies.find(from) == game.m_provinceArmies.end() ||
              troopsOfTypeAt(from, cid, TROOP_MECHANISED) == 0,
              "and the source is empty");
    }

    int industryLevel(int pid) const {
        auto it = game.m_provinceIndustry.find(pid);
        return it == game.m_provinceIndustry.end() ? 0 : it->second.level;
    }
    double treasury(int cid) const {
        auto it = game.m_countries.getAll().find(cid);
        return it == game.m_countries.getAll().end() ? 0.0 : it->second.treasury;
    }

    /**
     * The two settings a hosted game carries between one game and the next.
     *
     * Neither is about hostile input, but both live on this side of Game's
     * private wall and both were wrong in the same way: something the host
     * chose never reached the thing that acts on it.
     */
    void hostedGameSettings() {
        printf("\n== what a hosted game is configured with ==\n");

        game.m_mpAbsent = 0;
        check(game.mpTurnRunnerConfig().absentIsIdle == 0,
              "'the AI plays them' reaches the turn clock");
        game.m_mpAbsent = 1;
        check(game.mpTurnRunnerConfig().absentIsIdle == 1,
              "and so does 'their country sits idle'");

        // The clock itself must not outlive the game that started it: a second
        // game in one sitting inherited the first one's turn number and its
        // long-expired deadline, and resolved a turn nobody had played.
        game.mpHostTurnUpdate();          // no host, so this must not create one
        check(game.m_mpTurns == nullptr,
              "no turn clock is made for a game that is not being hosted");
        game.m_mpTurns = new TurnRunner();
        game.m_mpDeadlineMs[7] = 1234;
        game.mpLeave();
        check(game.m_mpTurns == nullptr, "leaving a game takes its turn clock with it");
        check(game.m_mpDeadlineMs.empty(), "and the per-player deadlines it held");
    }

    /**
     * The world a joiner is sent, and the guard for when they were not.
     */
    void worldForAJoiner() {
        printf("\n== the world somebody arriving is given ==\n");

        game.m_turnNumber = 0;
        game.m_mpSnapshotCache.clear();
        game.m_mpSnapshotTurn = -1;

        const std::vector<uint8_t> first = game.mpSnapshotForJoiner();
        check(!first.empty(), "a snapshot is built for somebody arriving");
        check(game.m_mpSnapshotTurn == game.m_turnNumber, "and remembered for the turn it is of");

        // Several people arriving between two turns are sent the same bytes
        // rather than each paying for a rebuild.
        const uint8_t* was = game.m_mpSnapshotCache.data();
        const std::vector<uint8_t> again = game.mpSnapshotForJoiner();
        check(again == first && game.m_mpSnapshotCache.data() == was,
              "a second arrival in the same turn is sent the same world, not a new build");

        game.m_turnNumber = 1;
        game.mpSnapshotForJoiner();
        check(game.m_mpSnapshotTurn == 1, "and the next turn rebuilds it");
        game.m_turnNumber = 0;

        // The guard: a delta with no world to apply it to is refused rather
        // than written into provinces that are not there.
        const auto owners = game.m_provinceCountryLookup;
        game.m_provinceCountryLookup.clear();
        const int turnWas = game.m_turnNumber;
        game.mpApplyDelta(7, std::vector<uint8_t>{1, 2, 3});
        check(game.m_turnNumber == turnWas,
              "a turn that arrives before the world is not applied to nothing");
        game.m_provinceCountryLookup = owners;
    }

    /** Orders the host never acknowledged. */
    void ordersNobodyAcknowledged() {
        printf("\n== orders the host has not taken ==\n");
        using A = Game::OrdersAck;
        check(Game::mpOrdersAckStep(true, false, 0) == A::Settled,
              "orders the host has are settled at once");
        check(Game::mpOrdersAckStep(true, true, 60000) == A::Settled,
              "and stay settled however long it took");
        check(Game::mpOrdersAckStep(false, false, 1000) == A::Waiting,
              "a moment's wait is only a wait");
        check(Game::mpOrdersAckStep(false, false, 6000) == A::Resend,
              "six seconds without a word means send them again");
        check(Game::mpOrdersAckStep(false, true, 6000) == A::GiveUp,
              "and twice unanswered means telling the player, not waiting on");
    }

    /** Which turns a client may apply, and what it does with the rest. */
    void turnsArrivingFromTheHost() {
        printf("\n== a turn that arrives from the host ==\n");
        using D = Game::DeltaStep;
        check(Game::mpDeltaStep(false, true, 5, 4) == D::Apply,
              "the turn after this one is applied");
        check(Game::mpDeltaStep(false, true, 4, 4) == D::Ignore,
              "the turn we already have is ignored, not applied twice");
        check(Game::mpDeltaStep(false, true, 2, 4) == D::Ignore,
              "and so is an older one");
        check(Game::mpDeltaStep(false, true, 7, 4) == D::Resync,
              "a gap asks for the world rather than guessing at it");
        check(Game::mpDeltaStep(true, true, 5, 4) == D::Queue,
              "a turn arriving mid-load is held, not applied to the old world");
        check(Game::mpDeltaStep(false, false, 5, 4) == D::Resync,
              "and with no world at all there is nothing to apply it to");
    }

    /// A country with no treaty, war or claim against `cid` -- the safe target
    /// for an overture that has to be legal before anything else is tested.
    int strangerTo(int cid) const {
        const Country* me = game.m_countries.getCountry(cid);
        for (const auto& [other, c] : game.m_countries.getAll()) {
            if (other <= 0 || other >= 65530 || other == cid) continue;
            if (!ownedProvince(other)) continue;
            if (game.diploActionAllowed(cid, other, "request_nap", nullptr) &&
                !game.hasRelation(me->isoA3, c.isoA3, &CountryRelation::war))
                return other;
        }
        return 0;
    }
    std::string isoOf(int cid) const {
        const Country* c = game.m_countries.getCountry(cid);
        return c ? c->isoA3 : std::string();
    }
    int nodeIndex(const std::string& id) const {
        for (int i = 0; i < (int)game.m_researchNodes.size(); ++i)
            if (game.m_researchNodes[i].id == id) return i;
        return -1;
    }
    int pendingFrom(const std::string& iso, const std::string& action) const {
        int n = 0;
        for (const auto& da : game.m_pendingDiplomaticActions)
            if (da.sourceIso == iso && da.action == action) ++n;
        return n;
    }

    /**
     * What a remote player decides about their country -- research, budgets,
     * doctrines, diplomacy, disclosure, districts, claims, answers -- arrives
     * in the same payload as their orders now (Game_MpChoices.cpp), and every
     * one of those keys is somebody else's bytes. Each section gets the lie a
     * modified client would tell first, and an honest case after it, so that
     * a refusal that refused everything cannot pass for a fix.
     */
    void choicesOverTheWire(int cid) {
        printf("\n== the choices that are not orders ==\n");
        // The ISO lookup is built by a load phase this map-only fixture skips,
        // and every overture names its target by ISO.
        game.rebuildIsoIndex();
        const std::string me = isoOf(cid);
        const int other = strangerTo(cid);
        check(other != 0, "found a country with no treaty with ours");
        if (!other) return;
        const std::string them = isoOf(other);

        // ── AN OLD CLIENT CHANGES NOTHING ──
        //
        // The rule the format rests on: a key that is absent leaves the host's
        // state alone. A client from before these keys existed sends none of
        // them, and its country must not be reset to anything.
        {
            game.m_countryResearchAllocation[cid] = 0.17f;
            game.m_countryPacification[cid] = 0.03f;
            game.m_countryDisclosure[cid] = 1u;
            const int act = game.m_countryResearchActive.count(cid) ? game.m_countryResearchActive[cid] : -1;
            const size_t policies = game.m_activePolicies.size();
            const size_t pending = game.m_pendingDiplomaticActions.size();
            const auto districts = game.m_districts[cid].size();
            const auto claims = game.m_claims[me].size();
            apply(cid, "{\"pendingUpgrades\":[],\"marker\":\"old client\"}");
            apply(cid, "{}");
            check(game.m_countryResearchAllocation[cid] == 0.17f &&
                  game.m_countryPacification[cid] == 0.03f,
                  "an old payload leaves the budgets where they were");
            check((game.m_countryResearchActive.count(cid) ? game.m_countryResearchActive[cid] : -1) == act,
                  "and the research project");
            check(game.m_countryDisclosure[cid] == 1u, "and what the country publishes");
            check(game.m_activePolicies.size() == policies &&
                  game.m_pendingDiplomaticActions.size() == pending &&
                  game.m_districts[cid].size() == districts && game.m_claims[me].size() == claims,
                  "and its doctrines, diplomacy, districts and claims");
        }

        // ── BUDGETS ──
        {
            game.m_countryResearchAllocation[cid] = 0.10f;
            apply(cid, "{\"budgets\":{\"research\":1e400}}");
            check(std::isfinite(game.m_countryResearchAllocation[cid]) &&
                  game.m_countryResearchAllocation[cid] == 0.10f,
                  "an infinite research budget is refused, not stored");
            apply(cid, "{\"budgets\":{\"research\":-0.5,\"pacification\":7}}");
            check(game.m_countryResearchAllocation[cid] == 0.10f,
                  "a negative one is refused");
            check(game.m_countryPacification[cid] <= 1.0f, "and so is 700% pacification");
            apply(cid, "{\"budgets\":{\"research\":1.0,\"pacification\":0}}");
            const CountryIncomeSnapshot cs = game.computeCountryIncome(cid);
            check(game.m_countryResearchAllocation[cid] <= Game::researchAllocationCap(cs, 0.0f) + 1e-6f,
                  "100% research is held to the cap the slider draws");
            apply(cid, "{\"budgets\":{\"research\":0.0,\"pacification\":0.0}}");
            check(game.m_countryResearchAllocation[cid] == 0.0f && game.m_countryPacification[cid] == 0.0f,
                  "and an honest budget is taken as sent");
        }

        // ── RESEARCH ──
        {
            game.m_scriptResearchGroups[cid] = 1;           // one group, whatever the economy says
            auto& done = game.m_countryResearched[cid];
            // Something it has already finished, and something whose
            // prerequisites it has not.
            std::string finished, locked, open;
            for (const ResearchNode& n : game.m_researchNodes) {
                if (finished.empty() && done.count(n.id)) finished = n.id;
                if (locked.empty() && !done.count(n.id) && !game.isNodeAvailableFor(n, cid) &&
                    !n.deps.empty())
                    locked = n.id;
                if (open.empty() && !done.count(n.id) && game.isNodeAvailableFor(n, cid)) open = n.id;
            }
            if (finished.empty()) { done.insert("ind1"); finished = "ind1"; }
            check(!locked.empty() && !open.empty(), "found a locked node and an open one");
            game.m_countryResearchActive[cid] = -1;
            apply(cid, "{\"research\":[{\"node\":\"" + finished + "\",\"share\":100}]}");
            check(game.m_countryResearchActive[cid] == -1, "a node already researched is refused");
            apply(cid, "{\"research\":[{\"node\":\"" + locked + "\"}]}");
            check(game.m_countryResearchActive[cid] == -1,
                  "a node whose prerequisites are not met is refused");
            apply(cid, "{\"research\":[{\"node\":\"no_such_node\"}]}");
            check(game.m_countryResearchActive[cid] == -1, "a node that does not exist is refused");
            // A second group the country cannot run, and a fourth that no
            // country can.
            apply(cid, "{\"research\":[{\"node\":\"\"},{\"node\":\"" + open +
                       "\"},{\"node\":\"\"},{\"node\":\"" + open + "\"}]}");
            const auto& ex = game.m_countryResearchExtra[cid];
            check(ex[0].activeNode == -1 && ex[1].activeNode == -1,
                  "a group the country has not unlocked is refused (and a 4th is not read)");
            apply(cid, "{\"research\":[{\"node\":\"" + open + "\",\"share\":100,\"auto\":true}]}");
            check(game.m_countryResearchActive[cid] == nodeIndex(open),
                  "an open node in an unlocked group IS accepted");
            // Two groups, one node.
            game.m_scriptResearchGroups[cid] = 2;
            apply(cid, "{\"research\":[{\"node\":\"" + open + "\"},{\"node\":\"" + open + "\"}]}");
            check(game.m_countryResearchActive[cid] == nodeIndex(open) &&
                  game.m_countryResearchExtra[cid][0].activeNode != nodeIndex(open),
                  "the same node in two groups is refused for the second");
            apply(cid, "{\"research\":[{\"node\":\"" + open + "\",\"share\":300}]}");
            const auto& plan = game.m_countryResearchPlan[cid];
            check(plan.share[0] + plan.share[1] == 100, "shares are refused past 100 and add up to 100");
            game.m_scriptResearchGroups.erase(cid);
        }

        // ── DOCTRINES ──
        {
            auto has = [&](const std::string& id) { return game.modCountryHasPolicy(cid, id); };
            std::string blocked;
            std::vector<std::string> open;
            for (const Policy& p : game.m_allPolicies) {
                if (has(p.id)) continue;
                if (!game.canCountryEnactPolicy(cid, p)) { if (blocked.empty()) blocked = p.id; continue; }
                bool clash = false;
                for (const std::string& o : open) if (game.policiesConflict(o, p.id)) clash = true;
                if (!clash && open.size() < 4) open.push_back(p.id);
            }
            if (!blocked.empty()) {
                apply(cid, "{\"policies\":[{\"op\":\"enact\",\"id\":\"" + blocked + "\"}]}");
                check(!has(blocked), "a doctrine the country does not qualify for is refused");
            }
            apply(cid, "{\"policies\":[{\"op\":\"enact\",\"id\":\"martian_law\"}]}");
            check(!has("martian_law"), "a doctrine that does not exist is refused");
            // Plenty of money, so price is not what stops the fourth.
            game.m_countries.getAll()[cid].treasury = 1e7;
            if (open.size() == 4) {
                std::string ops;
                for (const std::string& id : open)
                    ops += std::string(ops.empty() ? "" : ",") + "{\"op\":\"enact\",\"id\":\"" + id + "\"}";
                apply(cid, "{\"policies\":[" + ops + "]}");
                int n = 0;
                for (const std::string& id : open) n += has(id) ? 1 : 0;
                check(n >= 1 && n <= 3, "four enactments in one turn: at most three are taken");
                check(has(open[0]), "and an honest one is enacted");
                apply(cid, "{\"policies\":[{\"op\":\"cancel\",\"id\":\"" + open[0] + "\"}]}");
                // Repealed means marked: the row leaves the list at the turn's
                // clean-up, which this fixture never runs.
                bool live = false;
                for (const ActivePolicy& ap : game.m_activePolicies)
                    if (ap.countryId == cid && ap.policyId == open[0] && ap.turnsRemaining != -1) live = true;
                check(!live, "and can be repealed");
            } else {
                printf("  -- fewer than four doctrines open to this country; the cap is not tested\n");
            }
        }

        // ── DIPLOMACY ──
        {
            game.m_pendingDiplomaticActions.clear();
            apply(cid, "{\"diplomacy\":[{\"action\":\"apply_ceasefire\",\"target\":\"" + them + "\"},"
                       "{\"action\":\"apply_trade\",\"target\":\"" + them + "\"},"
                       "{\"action\":\"cancel\",\"target\":\"" + them + "\"}]}");
            check(game.m_pendingDiplomaticActions.empty(),
                  "the bookkeeping halves of a deal are never taken from a client");
            apply(cid, "{\"diplomacy\":[{\"action\":\"request_ceasefire\",\"target\":\"" + them + "\"}]}");
            check(game.m_pendingDiplomaticActions.empty(), "nor a ceasefire with a country it is not at war with");
            const int notMine = foreignProvince(cid);
            apply(cid, "{\"diplomacy\":[{\"action\":\"propose_trade\",\"target\":\"" + them +
                       "\",\"terms\":{\"ourProvs\":[" + std::to_string(notMine) + "]}}]}");
            check(game.m_pendingDiplomaticActions.empty(), "nor a trade ceding a province that is not ours");
            apply(cid, "{\"diplomacy\":[{\"action\":\"declare_war\",\"target\":\"" + them +
                       "\",\"goal\":999}]}");
            check(game.m_pendingDiplomaticActions.empty(), "nor a war goal that does not exist");
            apply(cid, "{\"diplomacy\":[{\"action\":\"request_nap\",\"target\":\"" + them +
                       "\",\"source\":\"" + them + "\"}]}");
            check(pendingFrom(me, "request_nap") == 1 && pendingFrom(them, "request_nap") == 0,
                  "an honest request is queued, from OUR country whatever the client says");
            game.m_pendingDiplomaticActions.clear();
        }

        // ── DISCLOSURE, CLAIMS ──
        {
            apply(cid, "{\"disclosure\":4294967295}");
            check((game.m_countryDisclosure[cid] & ~0xFu) == 0u && game.m_countryDisclosure[cid] == 0xFu,
                  "disclosure keeps only the bits this world offers");
            apply(cid, "{\"claims\":{\"add\":[" + std::to_string(mine(cid)) + "]}}");
            auto c = game.m_claims.find(me);
            check(c == game.m_claims.end() ||
                  std::find(c->second.begin(), c->second.end(), mine(cid)) == c->second.end(),
                  "a claim on our own ground is refused");
            const int theirs = ownedProvince(other);
            apply(cid, "{\"claims\":{\"add\":[" + std::to_string(theirs) + "]}}");
            c = game.m_claims.find(me);
            check(c != game.m_claims.end() &&
                  std::find(c->second.begin(), c->second.end(), theirs) != c->second.end(),
                  "and a claim on somebody else's is staked");
            apply(cid, "{\"claims\":{\"drop\":[" + std::to_string(theirs) + "]}}");
            c = game.m_claims.find(me);
            check(c == game.m_claims.end() ||
                  std::find(c->second.begin(), c->second.end(), theirs) == c->second.end(),
                  "and dropped again");
        }

        // ── DISTRICTS: A PARTITION OR NOTHING ──
        {
            std::vector<int> owned;
            for (const auto& [pid, p] : game.m_provinces.getAllProvinces())
                if (p.countryId == cid) owned.push_back(pid);
            std::sort(owned.begin(), owned.end());
            game.m_districts.erase(cid);
            game.ensureDefaultDistrict(cid);
            const auto before = game.m_districts[cid].size();
            auto list = [](const std::vector<int>& v) {
                std::string s;
                for (int p : v) s += (s.empty() ? "" : ",") + std::to_string(p);
                return s;
            };
            if (owned.size() >= 2) {
                apply(cid, "{\"districts\":[{\"id\":1,\"name\":\"Half\",\"provinces\":[" +
                           std::to_string(owned[0]) + "]}]}");
                check(game.m_districts[cid].size() == before &&
                      game.m_districts[cid][0].provinces.size() == owned.size(),
                      "districts that leave provinces out are refused whole");
                apply(cid, "{\"districts\":[{\"id\":1,\"name\":\"All\",\"provinces\":[" + list(owned) +
                           "," + std::to_string(foreignProvince(cid)) + "]}]}");
                check(game.m_districts[cid][0].name != "All", "and so are districts holding foreign ground");
                apply(cid, "{\"districts\":[{\"id\":1,\"name\":\"A\",\"provinces\":[" + list(owned) +
                           "]},{\"id\":1,\"name\":\"B\",\"provinces\":[]}]}");
                check(game.m_districts[cid][0].name != "A", "and two districts with one id");
                std::vector<int> rest(owned.begin() + 1, owned.end());
                apply(cid, "{\"districts\":[{\"id\":1,\"name\":\"East\",\"share\":60,\"provinces\":[" +
                           std::to_string(owned[0]) + "]},{\"id\":2,\"name\":\"West\",\"share\":40,"
                           "\"provinces\":[" + list(rest) + "]}]}");
                check(game.m_districts[cid].size() == 2 && game.m_districts[cid][0].name == "East",
                      "and an honest partition is installed");
            }
        }

        // ── ANSWERS, AND SILENCE ──
        {
            Game::MpInboxItem item;
            item.seq = 4242;
            item.action = "request_nap";
            item.sourceIso = them;
            item.targetIso = me;
            game.m_mpInbox[cid] = {item};
            apply(cid, "{\"answers\":[{\"seq\":999999,\"accept\":true}]}");
            check(game.m_mpInbox[cid].size() == 1 &&
                  !game.hasRelation(me, them, &CountryRelation::nonAggression),
                  "an answer to something never asked is ignored");
            apply(cid, "{\"answers\":[{\"seq\":4242,\"accept\":true}]}");
            check(game.m_mpInbox[cid].empty() && game.hasRelation(me, them, &CountryRelation::nonAggression),
                  "an answer to what was asked settles it");
            game.m_relations[me][them].nonAggression = false;
            game.m_relations[them][me].nonAggression = false;

            // A call to arms nobody answers lapses, and the alliance stands.
            game.m_relations[me][them].alliance = true;
            game.m_relations[them][me].alliance = true;
            item.seq = 4243;
            item.action = "call_to_arms";
            item.subjectIso = isoOf(anyCountry());
            game.m_mpInbox[cid] = {item};
            game.mpExpireInbox({{cid, false}});
            check(game.m_mpInbox.empty(), "an unanswered request lapses after a turn");
            check(game.hasRelation(me, them, &CountryRelation::alliance),
                  "and an unanswered call to arms keeps the alliance");
            game.m_relations[me][them].alliance = false;
            game.m_relations[them][me].alliance = false;
        }
    }
    int mine(int cid) const { return ownedProvince(cid); }

    void run() {
        hostedGameSettings();
        turnsArrivingFromTheHost();
        worldForAJoiner();
        ordersNobodyAcknowledged();
        troopsKeepTheirKindOnMove();
        const int cid = anyCountry();
        check(cid != 0, "found a country to act as");
        const int mine = ownedProvince(cid);
        check(mine != 0, "found a province it owns");
        if (!cid || !mine) return;
        choicesOverTheWire(cid);

        // ── MONUMENTS OVER THE WIRE ──
        //
        // A client sends the whole set it holds rather than a change, so the
        // host has to rebuild its copy from a message and must not believe any
        // of it. Four lies, one per rule: somebody else's province, a kind
        // that does not exist, a level past the catalogue, and a port monument
        // inland. The research gate is not tested here because this fixture's
        // country has researched nothing, which is why the honest case below
        // asserts only that a refusal is silent rather than fatal.
        {
            const int notMine = foreignProvince(cid);
            game.m_monuments.clear();
            apply(cid, "{\"monuments\":[{\"province\":" + std::to_string(notMine) +
                       ",\"kind\":\"university\",\"level\":1,\"active\":true}]}");
            check(game.m_monuments.empty(),
                  "a monument in somebody else's province is refused");

            game.m_monuments.clear();
            apply(cid, "{\"monuments\":[{\"province\":" + std::to_string(mine) +
                       ",\"kind\":\"death_star\",\"level\":1,\"active\":true}]}");
            check(game.m_monuments.empty(), "a kind this build has never heard of is refused");

            // The level, which is the shape the industry exploit above took.
            game.m_monuments.clear();
            // Straight into the set the gate reads; there is no turn to spend here.
            game.m_countryResearched[cid].insert(odmon::unlockNode(odmon::Kind::University));
            apply(cid, "{\"monuments\":[{\"province\":" + std::to_string(mine) +
                       ",\"kind\":\"university\",\"level\":999,\"active\":true}]}");
            bool capped = true;
            for (const auto& [pid, h] : game.m_monuments)
                if (h.level > odmon::spec(h.kind).maxLevel) capped = false;
            check(capped, "level 999 is clamped to what the catalogue has a price for");

            // And an ABSENCE is a decision: a monument the client took down
            // must leave the host's copy, or it keeps charging for the slot.
            check(!game.m_monuments.empty(), "the clamped one was accepted");
            apply(cid, "{\"monuments\":[]}");
            // Nothing else in this fixture holds one, so the whole map being
            // empty IS this country's being empty -- and indexing
            // m_provinceCountryLookup to say it more precisely was reading
            // past the end of an array this harness never fills, which made
            // the check pass whatever the host did.
            check(game.m_monuments.empty(),
                  "an empty list takes this country's monuments down");
        }

        // ── the exploit ──
        {
            game.m_pendingUpgrades.clear();
            apply(cid, "{\"pendingUpgrades\":[{\"provinceId\":" + std::to_string(mine) +
                       ",\"type\":\"industry\",\"targetLevel\":999,\"turnsRemaining\":1}]}");
            bool ok = true;
            for (const PendingUpgrade& u : game.m_pendingUpgrades)
                if (u.provinceId == mine && u.targetLevel > 20) ok = false;
            check(ok, "targetLevel 999 does not become a queued level-999 build");

            bool instant = false;
            for (const PendingUpgrade& u : game.m_pendingUpgrades)
                if (u.provinceId == mine && u.turnsRemaining < 1) instant = true;
            check(!instant, "a build cannot be queued as already finished");
        }

        // A LEGITIMATE build must still work, and must cost money.
        //
        // Checked before anything else about builds, because every "the cheat
        // did not work" assertion above passes trivially if no build is ever
        // queued -- a refusal that refused everything would look exactly like a
        // fix. This is what stops that reading.
        {
            game.m_pendingUpgrades.clear();
            const int levelBefore = industryLevel(mine);
            const double before = treasury(cid);
            apply(cid, "{\"pendingUpgrades\":[{\"provinceId\":" + std::to_string(mine) +
                       ",\"type\":\"industry\"}]}");
            const bool queued = !game.m_pendingUpgrades.empty();
            check(queued, "an ordinary industry upgrade is still accepted");
            if (queued) {
                const PendingUpgrade& u = game.m_pendingUpgrades.front();
                check(u.targetLevel == levelBefore + 1,
                      "the server set the level itself: exactly one step up");
                check(u.turnsRemaining >= 1, "the server set a real build time");
                check(treasury(cid) < before,
                      "the submitting country's treasury paid for it");
            }
        }

        // ── ownership, which was already right and must stay right ──
        {
            int theirs = 0, otherCid = 0;
            for (const auto& [pid, p] : game.m_provinces.getAllProvinces())
                if (p.countryId != cid && p.countryId > 0) { theirs = pid; otherCid = p.countryId; break; }
            if (theirs) {
                game.m_pendingUpgrades.clear();
                apply(cid, "{\"pendingUpgrades\":[{\"provinceId\":" + std::to_string(theirs) +
                           ",\"type\":\"industry\"}]}");
                bool touched = false;
                for (const PendingUpgrade& u : game.m_pendingUpgrades)
                    if (u.provinceId == theirs) touched = true;
                check(!touched, "cannot queue a build in a province owned by somebody else");
                (void)otherCid;
            }
        }

        // ── recruitment: unbounded and free, until it was not ──
        {
            game.m_pendingRecruitments.clear();
            const double before = treasury(cid);
            apply(cid, "{\"pendingRecruitments\":[{\"provinceId\":" + std::to_string(mine) +
                       ",\"count\":2000000000,\"turnsRemaining\":0}]}");
            bool absurd = false, instant = false;
            for (const PendingRecruitment& r : game.m_pendingRecruitments) {
                if (r.count > 100000000) absurd = true;
                if (r.turnsRemaining < 1) instant = true;
            }
            check(!absurd, "a two-billion-man recruitment is refused");
            check(!instant, "recruitment cannot arrive the turn it is ordered");
            check(game.m_pendingRecruitments.empty() || treasury(cid) < before,
                  "recruitment was paid for");
        }

        // ── specialisation, ship builds, embarkation ──
        //
        // All three were free over the network, and two of them also carried a
        // std::string field that the reader treated as a number -- so the type
        // was silently replaced with one control character and nothing was ever
        // built. The type surviving intact is as much the point here as the
        // price being taken.
        {
            game.m_pendingShipBuilds.clear();
            const double before = treasury(cid);
            // A landlocked province with no port: the gate, not the price, is
            // what should refuse this.
            apply(cid, "{\"pendingShipBuilds\":[{\"provinceId\":" + std::to_string(mine) +
                       ",\"type\":\"carrier\",\"turnsRemaining\":0}]}");
            bool freeHull = false, instant = false, badType = false;
            for (const PendingShipBuild& b : game.m_pendingShipBuilds) {
                if (b.turnsRemaining < 1) instant = true;
                if (b.type != "carrier" && b.type != "destroyer") badType = true;
            }
            if (!game.m_pendingShipBuilds.empty() && treasury(cid) >= before) freeHull = true;
            check(!instant, "a hull cannot be laid down and finished the same turn");
            check(!badType, "the hull type survives as text, not as one stray byte");
            check(!freeHull, "a queued hull was paid for");
        }
        {
            game.m_pendingShipBuilds.clear();
            apply(cid, "{\"pendingShipBuilds\":[{\"provinceId\":" + std::to_string(mine) +
                       ",\"type\":\"dreadnought\"}]}");
            check(game.m_pendingShipBuilds.empty(),
                  "a hull type this game does not build is refused");
        }
        {
            // POSITIVE CONTROL. Every refusal above passes trivially if hulls
            // are never buildable at all -- and `mine` is whatever province came
            // first, which is usually landlocked. This finds a real shipyard and
            // proves the accepting half works, so the refusals mean something.
            int yard = 0, yardCid = 0;
            for (const auto& [pid, port] : game.m_provincePorts) {
                if (port.level < 2) continue;
                const Province* pr = game.m_provinces.getProvinceById(pid);
                if (!pr || pr->countryId <= 0) continue;
                auto cit = game.m_countries.getAll().find(pr->countryId);
                if (cit == game.m_countries.getAll().end() || cit->second.treasury < 20.0) continue;
                yard = pid; yardCid = pr->countryId;
                break;
            }
            check(yard != 0, "found a port able to build a destroyer");
            if (yard) {
                game.m_pendingShipBuilds.clear();
                const double before = treasury(yardCid);
                apply(yardCid, "{\"pendingShipBuilds\":[{\"provinceId\":" +
                               std::to_string(yard) + ",\"type\":\"destroyer\"}]}");
                check(game.m_pendingShipBuilds.size() == 1,
                      "a destroyer at a real port IS accepted");
                if (game.m_pendingShipBuilds.size() == 1) {
                    const PendingShipBuild& b = game.m_pendingShipBuilds.front();
                    check(b.type == "destroyer", "and it is still a destroyer");
                    check(b.turnsRemaining == 3, "and takes the three turns it should");
                    check(treasury(yardCid) <= before - 15.0 + 0.001,
                          "and cost the $15 the panel charges");
                }
            }
        }
        {
            game.m_pendingSpecializations.clear();
            const double before = treasury(cid);
            apply(cid, "{\"pendingSpecializations\":[{\"provinceId\":" + std::to_string(mine) +
                       ",\"specialization\":\"oil\",\"turnsRemaining\":0}]}");
            bool instant = false, empty = false;
            for (const PendingSpecialization& s : game.m_pendingSpecializations) {
                if (s.turnsRemaining < 1) instant = true;
                if (s.specialization.empty() || s.specialization.size() < 2) empty = true;
            }
            check(!instant, "specialisation cannot complete the turn it is ordered");
            check(!empty, "the resource survives as text");
            check(game.m_pendingSpecializations.empty() || treasury(cid) < before,
                  "specialisation was paid for");
        }
        {
            // Embarking is free, so the check is the count: it must be bounded
            // by troops that are actually standing in the province.
            game.m_pendingEmbarkations.clear();
            apply(cid, "{\"pendingEmbarkations\":[{\"provinceId\":" + std::to_string(mine) +
                       ",\"count\":2000000000,\"turnsRemaining\":0}]}");
            int garrison = 0;
            auto ait = game.m_provinceArmies.find(mine);
            if (ait != game.m_provinceArmies.end())
                for (const ArmyUnit& u : ait->second)
                    if (u.countryId == cid) garrison += u.count;
            bool over = false, instant = false;
            for (const PendingEmbark& em : game.m_pendingEmbarkations) {
                if (em.count > garrison) over = true;
                if (em.turnsRemaining < 1) instant = true;
            }
            check(!over, "cannot embark more troops than are standing there");
            check(!instant, "embarkation still takes its turn");
        }

        // ── hostile shapes ──
        {
            game.m_pendingShipMoveOrders.clear();
            apply(cid, "{\"pendingShipMoveOrders\":[{\"shipIndex\":0,"
                       "\"destLon\":1e400,\"destLat\":1e400}]}");
            bool bad = false;
            for (const PendingShipMoveOrder& m : game.m_pendingShipMoveOrders)
                if (!(m.destLon >= -180.0f && m.destLon <= 180.0f)) bad = true;
            check(!bad, "an infinite destination is refused rather than stored");
        }
        {
            // Deep nesting is a stack overflow in the parser, reachable by
            // anyone holding a seat. It must be refused before parsing.
            std::string deep = "{\"pendingUpgrades\":";
            for (int i = 0; i < 5000; ++i) deep += "[";
            for (int i = 0; i < 5000; ++i) deep += "]";
            deep += "}";
            apply(cid, deep);
            check(true, "5000-deep nesting returns without crashing");
        }
        {
            apply(cid, "not json at all");
            apply(cid, "[]");
            apply(cid, "{\"pendingUpgrades\":\"a string, not an array\"}");
            apply(cid, "{\"pendingUpgrades\":[null,3,\"x\"]}");
            check(true, "malformed and wrong-typed payloads are survivable");
        }
        {
            game.m_pendingMoveOrders.clear();
            apply(cid, "{\"pendingMoveOrders\":[{\"fromProvince\":" + std::to_string(mine) +
                       ",\"toProvince\":" + std::to_string(mine) +
                       ",\"pct\":100000,\"countryId\":65535}]}");
            bool bad = false;
            for (const PendingMoveOrder& m : game.m_pendingMoveOrders) {
                if (m.pct < 0 || m.pct > 100) bad = true;
                if (m.countryId != cid) bad = true;   // attribution is never taken from the wire
            }
            check(!bad, "percentage is bounded and attribution is the server's");
        }

        // ── THE RULE-TABLE SEAL ──
        //
        // Carried with every submission and compared against the host's own.
        // It must never change what is APPLIED -- the host resolves from its
        // own tables regardless -- so these check only what gets recorded, and
        // that the recording is quiet in every case that is not evidence.
        {
            game.m_tableSealMismatches.clear();
            const unsigned long long ours = game.tableSeal();

            // TWO BUILDS, AND BOTH GET ASSERTED.
            //
            // A tree without guard/odseal.cpp links the committed prebuilt,
            // which predates od_t4, so there is no word to compare and the
            // check is inert by design. That must not read as a pass: the
            // reason is printed, and what gets asserted instead is the
            // inertness itself -- a host with no seal of its own must record
            // nothing rather than flag everybody.
            if (ours == 0ull) {
                printf("  -- no rule-table seal in this build (odseal predates od_t4);\n"
                       "     asserting that the check is inert rather than skipping it\n");
                apply(cid, "{\"seal\":12345}");
                check(game.m_tableSealMismatches.empty(),
                      "a host with no seal of its own records nothing");
                game.m_tableSealMismatches.clear();
                return;
            }

            // An honest client: the same install, so the same word.
            apply(cid, "{\"seal\":" + std::to_string(ours) + "}");
            check(game.m_tableSealMismatches.empty(),
                  "a client holding the same tables is not recorded");

            // A client older than the field says nothing, which is not
            // evidence of anything and must stay silent.
            apply(cid, "{}");
            check(game.m_tableSealMismatches.empty(),
                  "a client that sends no seal is not recorded");

            // And one playing from tables that are not the host's.
            apply(cid, "{\"seal\":" + std::to_string(ours ^ 0x5EEDull) + "}");
            check(game.m_tableSealMismatches.size() == 1,
                  "a client holding different tables is recorded once");
            if (game.m_tableSealMismatches.size() == 1) {
                const auto& m = game.m_tableSealMismatches.front();
                check(m.countryId == cid && m.ours == ours && m.theirs == (ours ^ 0x5EEDull),
                      "the record names the country and both words");
            }

            // Said once per country per turn. A player submitting four times
            // before the turn resolves is ordinary, and four identical lines
            // would bury the one that mattered.
            apply(cid, "{\"seal\":" + std::to_string(ours ^ 0x5EEDull) + "}");
            check(game.m_tableSealMismatches.size() == 1,
                  "the same turn is not recorded twice");
            game.m_tableSealMismatches.clear();
        }
    }
};

int main(int argc, char** argv) {
    const std::string dataDir = argc > 1 ? argv[1] : "data/";
    printf("Order validation (hostile input to mpApplyOrders)\n");

    OrderValidationTest t;
    if (!t.load(dataDir)) {
        fprintf(stderr, "could not load %sSTDmaps/1914.odmap\n", dataDir.c_str());
        return 2;
    }
    t.run();

    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
