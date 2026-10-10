// Fog of war in multiplayer: what a seat is SENT (src/Game_FogNet.cpp).
//
//   FogNetTest <data-dir>
//
// Two worlds in one process: a host holding the truth and a client that only
// ever receives what fogSeatDelta and fogStateJsonFor give its seat. The
// network is not needed to check the one thing that matters -- that nothing
// the seat cannot see crosses -- so the deltas go from one Game to the other
// directly, through the same pack/unpack the wire uses.
//
// Runs against the SERVER build (no GL), like PolicyRulesTest.

#include "../src/Game.h"
#include "../src/SaveManager.h"
#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    printf("  %-68s %s\n", what.c_str(), ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

}  // namespace

struct FogNetWorld {
    static bool load(Game& g, const std::string& dataDir) {
        g.m_headless = true;
        g.m_dataDir = dataDir;
        if (!g.loadFromODM(dataDir + "STDmaps/1914.odmap")) return false;
        g.loadGameDataStep1();
        g.loadGameDataStep2();
        // The two passes a windowed load runs after the data: the per-province
        // lookups and the labels pass, which is what builds adjacency.
        g.buildPopulationLookups();
        g.computeCountryLabels();
        return true;
    }
    static void apply(Game& g, const TurnDelta& d) {
        g.applyTurnDelta(d);
        g.rebuildOwnershipPixels();
    }
    static void addOrder(Game& g, const PendingMoveOrder& mo) { g.m_pendingMoveOrders.push_back(mo); }
};

namespace {

long long garrisonOf(const Game& g, int pid) {
    long long n = 0;
    auto it = g.m_provinceArmies.find(pid);
    if (it != g.m_provinceArmies.end())
        for (const auto& u : it->second) n += u.count;
    return n;
}

// The wire: pack on the host, unpack on the client.
void deliver(Game& client, const TurnDelta& d) {
    TurnDelta back;
    const std::vector<uint8_t> bytes = SaveManager::packTurn(d);
    SaveManager::unpackTurn(bytes.data(), bytes.size(), back);
    FogNetWorld::apply(client, back);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dataDir = argc > 1 ? argv[1] : "data/";
    printf("FogNetTest\n");
    Game host, client;
    if (!FogNetWorld::load(host, dataDir) || !FogNetWorld::load(client, dataDir)) {
        fprintf(stderr, "could not load %sSTDmaps/1914.odmap\n", dataDir.c_str());
        return 2;
    }
    host.m_fogOfWar = true;
    host.fogBeginWorld();
    host.recomputeFog();

    // The seat: the country with the most provinces, so it sees a lot and
    // still has plenty out of sight.
    int seat = 0;
    {
        std::unordered_map<int, int> held;
        for (size_t pid = 1; pid < host.m_provinceCountryLookup.size(); ++pid)
            ++held[host.m_provinceCountryLookup[pid]];
        int best = 0;
        for (auto& [c, n] : held)
            if (host.fogCountry(c) && n > best) { best = n; seat = c; }
    }
    check(seat > 0, "a seat to play");

    // A hidden province with men in it, and one in sight with men in it.
    int hidden = 0, seen = 0;
    for (const auto& [pid, units] : host.m_provinceArmies) {
        if (garrisonOf(host, pid) <= 0) continue;
        if (!host.provinceVisibleTo(seat, pid) && !hidden) hidden = pid;
        if (host.provinceVisibleTo(seat, pid) &&
            host.m_provinceCountryLookup[(size_t)pid] != seat && !seen) seen = pid;
    }
    check(hidden > 0 && seen > 0, "a garrison out of sight and a foreign one in sight");

    printf("\n== the world a joiner is sent ==\n");
    Game::FogMirror& mirror = host.m_fogMirrors[seat];
    TurnDelta real;
    TurnDelta full = host.fogSeatDelta(seat, mirror, true, real);
    check((int)full.provinces.size() == host.fogMaxPid(), "every province is set outright");
    deliver(client, full);
    check(garrisonOf(client, hidden) == 0, "a garrison out of sight is not sent");
    check(garrisonOf(client, seen) == garrisonOf(host, seen), "a garrison in sight is");
    {
        bool shipLeak = false, ownShipsOk = true;
        for (size_t i = 0; i < host.m_ships.size() && i < client.m_ships.size(); ++i) {
            if (!host.shipVisibleTo(seat, i) && client.m_ships[i].countryId != 0) shipLeak = true;
            if (host.m_ships[i].countryId == seat &&
                // The wire carries coordinates as floats, so compare to that.
                (std::fabs(client.m_ships[i].lat - host.m_ships[i].lat) > 1e-3 ||
                 client.m_ships[i].countryId != seat))
                ownShipsOk = false;
        }
        check(!shipLeak, "a hull out of sight arrives owned by nobody");
        check(ownShipsOk, "the seat's own hulls arrive where they are");
    }

    printf("\n== a turn ==\n");
    // Something happens in each: the hidden garrison doubles, the seen one too.
    for (auto& u : host.m_provinceArmies[hidden]) u.count *= 2;
    for (auto& u : host.m_provinceArmies[seen]) u.count *= 2;
    const long long hiddenTruth = garrisonOf(host, hidden);
    TurnDelta turn = host.fogSeatDelta(seat, mirror, false, real);
    bool mentionsHidden = false;
    for (const auto& a : turn.armies) if (a.provinceId == hidden) mentionsHidden = true;
    for (const auto& p : turn.provinces) if (p.provinceId == hidden) mentionsHidden = true;
    check(!mentionsHidden, "a change out of sight is not sent");
    deliver(client, turn);
    check(garrisonOf(client, seen) == garrisonOf(host, seen), "a change in sight is");
    TurnDelta quiet = host.fogSeatDelta(seat, mirror, false, real);
    check(quiet.provinces.empty() && quiet.armies.empty(), "a turn where nothing moved sends nothing");

    printf("\n== coming into sight ==\n");
    // The seat takes the hidden province. It is now theirs, so in sight -- and
    // everything that changed there while it was hidden arrives at once.
    host.m_provinceCountryLookup[(size_t)hidden] = seat;
    if (Province* p = host.m_provinces.getProvinceById(hidden)) p->countryId = seat;
    host.recomputeFog();
    check(host.provinceVisibleTo(seat, hidden), "the taken province is in sight");
    deliver(client, host.fogSeatDelta(seat, mirror, false, real));
    check(garrisonOf(client, hidden) == hiddenTruth,
          "what changed while it was hidden arrives when it is seen");

    printf("\n== an army abroad ==\n");
    {
        // A province far from the seat, with nobody of the seat's in it.
        int far = 0;
        for (size_t pid = 1; pid < host.m_provinceCountryLookup.size(); ++pid)
            if (!host.provinceVisibleTo(seat, (int)pid) && host.m_provinceCountryLookup[pid] != seat &&
                host.m_provinceCountryLookup[pid] > 0) { far = (int)pid; break; }
        check(far > 0, "a province out of the seat's sight");
        ArmyUnit u;
        u.countryId = seat;
        u.count = 5000;
        host.m_provinceArmies[far].push_back(u);
        host.recomputeFog();
        check(host.provinceVisibleTo(seat, far), "a stack in it puts it in sight");
        deliver(client, host.fogSeatDelta(seat, mirror, false, real));
        bool mine = false;
        for (const auto& cu : client.m_provinceArmies[far]) if (cu.countryId == seat && cu.count == 5000) mine = true;
        check(mine, "and the seat is sent its own men there");
    }

    printf("\n== the state a seat is sent ==\n");
    host.m_countryBalances[seat] = 111.0f;
    int stranger = 0;
    for (const auto& [c, cc] : host.m_countries.getAll())
        if (c != seat && host.fogCountry(c) && !host.sharesIntel(seat, c)) { stranger = c; break; }
    host.m_countryBalances[stranger] = 222.0f;
    PendingMoveOrder mo;
    mo.countryId = stranger;
    mo.fromProvince = seen;
    mo.toProvince = seen;
    mo.pct = 50;
    FogNetWorld::addOrder(host, mo);
    const std::string js = host.fogStateJsonFor(seat);
    nlohmann::json j = nlohmann::json::parse(js);
    check(j["balances"].contains(std::to_string(seat)), "its own balance is there");
    check(!j["balances"].contains(std::to_string(stranger)), "a stranger's is not");
    bool strangerOrder = false;
    if (j.contains("pendingMoveOrders"))
        for (const auto& e : j["pendingMoveOrders"])
            if (e.value("countryId", 0) == stranger) strangerOrder = true;
    check(!strangerOrder, "a stranger's orders are not");
    check(!j.contains("simRng") && !j.contains("mail"), "the dice and the post are not");
    check(j.contains("fogSeatMemory"), "its memory is");

    // The client takes that memory and ends up remembering what the host says it does.
    client.m_fogOfWar = true;
    client.fogBeginWorld();
    client.fogTakeSeatMemory(js);
    const auto* hm = host.intel(seat, hidden);
    const auto* cm = client.intel(seat, hidden);
    check(hm && cm && hm->seenTurn == cm->seenTurn && hm->garrison == cm->garrison,
          "the client's memory is the host's, for this seat");

    printf("\n%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
