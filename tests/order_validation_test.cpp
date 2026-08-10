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

#include <cstdio>
#include <cstring>
#include <string>
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

    int anyCountry() const {
        for (const auto& [cid, c] : game.m_countries.getAll())
            if (cid > 0 && cid < 65530) return cid;
        return 0;
    }

    void apply(int cid, const std::string& json) {
        game.mpApplyOrders(cid, bytes(json));
    }

    int industryLevel(int pid) const {
        auto it = game.m_provinceIndustry.find(pid);
        return it == game.m_provinceIndustry.end() ? 0 : it->second.level;
    }
    double treasury(int cid) const {
        auto it = game.m_countries.getAll().find(cid);
        return it == game.m_countries.getAll().end() ? 0.0 : it->second.treasury;
    }

    void run() {
        const int cid = anyCountry();
        check(cid != 0, "found a country to act as");
        const int mine = ownedProvince(cid);
        check(mine != 0, "found a province it owns");
        if (!cid || !mine) return;

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
