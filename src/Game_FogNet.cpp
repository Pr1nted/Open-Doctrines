// Fog of war in multiplayer: the host sends each player only what that
// player's country can see (Phase 8 of docs/design/fog-of-war-plan.md).
//
// Without this, every client holds the whole world -- the fog is drawn over a
// map whose truth is sitting in the client's memory, one debugger away. So in
// a fog world nothing is broadcast. Each seat gets:
//
//   - its own STATE (fogStateJsonFor): every country-keyed and order-shaped
//     part of state.json cut down to that country, its intelligence partners
//     and what has been published; its fog memory carried along, because a
//     joiner cannot rebuild it from turns it was never sent.
//   - its own TURN (fogSeatDelta): the host keeps a MIRROR per seat of what it
//     has been told, and a turn is the difference between that mirror and the
//     truth -- for the provinces and ships the seat can see. A province out of
//     sight is not sent at all, so it stays as the seat last knew it; one that
//     comes back into sight is sent whole, however many turns it changed in.
//
// A joiner, or a seat asking for the world again, is sent a FULL turn: every
// province and hull set outright, so the mirror is exactly what was sent.
//
// Ships keep their index (orders name a hull by it), so a hull out of sight is
// sent as a placeholder owned by nobody, parked in the Antarctic interior. The
// count of hulls is the one thing that leaks.
//
// Spectators get nothing in a fog world: a spectator seat would be a second
// window onto the truth for anyone willing to open two clients.

#include "Game.h"
#include "GameInternals.h"
#include "SaveManager.h"
#include "json.hpp"
#include "net/Host.h"
#include "net/WorldSync.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

constexpr double kHiddenLat = -85.0, kHiddenLon = 0.0;

std::string b64(const std::string& raw) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((raw.size() + 2) / 3 * 4);
    for (size_t i = 0; i < raw.size(); i += 3) {
        uint32_t v = (uint8_t)raw[i] << 16;
        if (i + 1 < raw.size()) v |= (uint8_t)raw[i + 1] << 8;
        if (i + 2 < raw.size()) v |= (uint8_t)raw[i + 2];
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += i + 1 < raw.size() ? T[(v >> 6) & 63] : '=';
        out += i + 2 < raw.size() ? T[v & 63] : '=';
    }
    return out;
}

std::string unb64(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        const int v = val(c);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((acc >> bits) & 0xFF);
        }
    }
    return out;
}

}  // namespace

// --- the truth, in the mirror's terms ----------------------------------------

Game::FogProv Game::fogProvTruth(int pid) const {
    FogProv p;
    if ((size_t)pid < m_provinceCountryLookup.size()) p.owner = m_provinceCountryLookup[(size_t)pid];
    auto pop = m_provincePopulations.find(pid);
    if (pop != m_provincePopulations.end()) p.pop = pop->second;
    auto ind = m_provinceIndustry.find(pid);
    if (ind != m_provinceIndustry.end()) {
        p.ind = ind->second.level;
        p.fort = ind->second.fortification;
        p.income = ind->second.income;
        p.resInc = ind->second.resourceIncome;
        p.popInc = ind->second.popIncome;
        p.popMod = ind->second.popModifier;
        p.output = ind->second.output;
    }
    return p;
}

std::vector<Game::FogUnit> Game::fogArmyTruth(int pid) const {
    std::vector<FogUnit> out;
    auto it = m_provinceArmies.find(pid);
    if (it == m_provinceArmies.end()) return out;
    for (const ArmyUnit& u : it->second)
        if (u.count > 0) out.push_back({u.countryId, u.count, (uint8_t)u.type});
    return out;
}

// --- one seat's turn ---------------------------------------------------------

TurnDelta Game::fogSeatDelta(int cid, FogMirror& m, bool full, const TurnDelta& real) {
    TurnDelta d;
    d.turnNumber = real.turnNumber;
    // The scalar trailer is the host player's research state, broadcast to
    // everyone before fog existed; carried as it was. No client applies it any
    // more (mpApplyDelta passes playerScalars = false), so it says nothing
    // about anybody's research to a seat that could not otherwise see it.
    d.researchAllocation = real.researchAllocation;
    d.pacificationAllocation = real.pacificationAllocation;
    d.researchActiveNode = real.researchActiveNode;
    for (int g = 0; g < 3; ++g) d.researchGroups[g] = real.researchGroups[g];
    d.researchPoints = real.researchPoints;

    const int maxPid = fogMaxPid();
    if ((int)m.prov.size() < maxPid + 1) m.prov.resize((size_t)maxPid + 1);
    for (int pid = 1; pid <= maxPid; ++pid) {
        const bool sees = provinceVisibleTo(cid, pid);
        if (!sees && !full) continue;          // out of sight: as last told
        FogProv want;
        if (sees) {
            want = fogProvTruth(pid);
        } else {
            // A joiner, for a province it cannot see: what this seat
            // remembers of it, which is what its own panel will show anyway.
            const odfog::ProvinceIntel* in = intel(cid, pid);
            want.owner = knownOwner(cid, pid);
            if (in && in->seenTurn != odfog::kNeverSeen) {
                want.pop = in->population;
                want.ind = in->industry;
                want.fort = in->fort;
            }
        }
        FogProv& have = m.prov[(size_t)pid];
        ProvinceDelta pd;
        pd.provinceId = pid;
        bool any = false;
        auto put = [&](bool differs, bool& flag) { if (full || differs) { flag = true; any = true; } };
        put(want.owner != have.owner, pd.ownerChanged);           pd.newOwner = want.owner;
        put(want.pop != have.pop, pd.populationChanged);          pd.newPopulation = want.pop;
        put(want.ind != have.ind, pd.industryLevelChanged);       pd.newIndustryLevel = want.ind;
        put(want.fort != have.fort, pd.fortificationChanged);     pd.newFortification = want.fort;
        put(want.income != have.income, pd.incomeChanged);        pd.newIncome = want.income;
        put(want.resInc != have.resInc, pd.resourceIncomeChanged); pd.newResourceIncome = want.resInc;
        put(want.popInc != have.popInc, pd.popIncomeChanged);     pd.newPopIncome = want.popInc;
        put(want.popMod != have.popMod, pd.popModifierChanged);   pd.newPopModifier = want.popMod;
        put(want.output != have.output, pd.outputChanged);        pd.newOutput = want.output;
        if (any) d.provinces.push_back(pd);
        have = want;

        // Armies: the province's whole list, as the delta always carries them.
        const std::vector<FogUnit> units = sees ? fogArmyTruth(pid) : std::vector<FogUnit>{};
        auto& told = m.armies[pid];
        auto same = [&] {
            if (told.size() != units.size()) return false;
            for (size_t i = 0; i < units.size(); ++i)
                if (told[i].countryId != units[i].countryId || told[i].count != units[i].count ||
                    told[i].type != units[i].type) return false;
            return true;
        };
        if (full || !same()) {
            ArmyDelta ad;
            ad.provinceId = pid;
            for (const FogUnit& u : units) ad.units.push_back({u.countryId, u.count, u.type});
            d.armies.push_back(ad);
            told = units;
        }
    }

    if (m.ships.size() < m_ships.size()) m.ships.resize(m_ships.size());
    for (size_t i = 0; i < m_ships.size(); ++i) {
        FogShip want;
        if (shipVisibleTo(cid, i)) {
            const NavyShip& s = m_ships[i];
            want = {s.lat, s.lon, s.health, s.crew, s.countryId};
        } else {
            want = {kHiddenLat, kHiddenLon, 100, 0, 0};
        }
        FogShip& have = m.ships[i];
        ShipDelta sd;
        sd.shipIndex = (int)i;
        bool any = false;
        auto put = [&](bool differs, bool& flag) { if (full || differs) { flag = true; any = true; } };
        put(want.lat != have.lat, sd.latChanged);            sd.newLat = want.lat;
        put(want.lon != have.lon, sd.lonChanged);            sd.newLon = want.lon;
        put(want.health != have.health, sd.healthChanged);   sd.newHealth = want.health;
        put(want.crew != have.crew, sd.crewChanged);         sd.newCrew = want.crew;
        put(want.cid != have.cid, sd.countryIdChanged);      sd.newCountryId = want.cid;
        if (any) d.ships.push_back(sd);
        have = want;
    }
    return d;
}

// --- one seat's state ----------------------------------------------------------

std::string Game::fogStateJsonFor(int cid) {
    nlohmann::json j;
    try { j = nlohmann::json::parse(saveStateJson()); } catch (...) { return {}; }
    j.erase("mail");        // every country's letters; each player is sent their own
    j.erase("simRng");      // the dice to come
    j.erase("withdraws");
    j.erase("claimsPendingAdd");    // the host player's own pending claims
    j.erase("claimsPendingDrop");
    j.erase("mpInbox");             // every seat's unanswered requests; its own go separately

    auto allowed = [&](int other) {
        return other == cid || sharesIntel(cid, other) || fogDossier(cid, other);
    };
    auto ownerOf = [&](int pid) {
        return (pid > 0 && (size_t)pid < m_provinceCountryLookup.size()) ? m_provinceCountryLookup[(size_t)pid] : 0;
    };
    auto cidKeyed = [&](const char* key, const std::function<bool(int)>& keep) {
        if (!j.contains(key) || !j[key].is_object()) return;
        nlohmann::json out = nlohmann::json::object();
        for (auto& [k, v] : j[key].items())
            if (keep(std::atoi(k.c_str()))) out[k] = v;
        j[key] = out;
    };
    for (const char* k : {"balances", "countryResearchActive", "countryResearchAlloc",
                          "countryResearchInvested", "countryResearchPoints", "warWeariness",
                          "stockpiles", "pacification", "politicalCapital", "alignmentDriftByCountry",
                          "ethnicPolicies", "districts", "countryResearchPlan",
                          "countryResearchExtra"})
        cidKeyed(k, allowed);
    cidKeyed("treasuryLastTurn", [&](int c) { return allowed(c) || discloses(c, DISCLOSE_TREASURY); });
    // Province-keyed facts: only where the seat can see.
    for (const char* k : {"provinceSpecializations", "rebellionCooldown", "provinceConquestTurn",
                          "conqueredProvincePrevOwner", "provinceOccupationAnger"})
        cidKeyed(k, [&](int pid) { return provinceVisibleTo(cid, pid); });

    // Arrays of per-country entries.
    auto filterArray = [&](const char* key, const std::function<bool(const nlohmann::json&)>& keep) {
        if (!j.contains(key) || !j[key].is_array()) return;
        nlohmann::json out = nlohmann::json::array();
        for (const auto& e : j[key]) if (keep(e)) out.push_back(e);
        j[key] = out;
    };
    auto field = [](const nlohmann::json& e, const char* k) {
        return (e.is_object() && e.contains(k) && e[k].is_number_integer()) ? e[k].get<int>() : 0;
    };
    filterArray("researched", [&](const nlohmann::json& e) { return allowed(field(e, "countryId")); });
    filterArray("activePolicies", [&](const nlohmann::json& e) {
        const int c = field(e, "countryId");
        return allowed(c) || discloses(c, DISCLOSE_DOCTRINES);
    });
    filterArray("nationalised", [&](const nlohmann::json& e) { return allowed(field(e, "countryId")); });
    filterArray("specTax", [&](const nlohmann::json& e) { return allowed(field(e, "countryId")); });
    filterArray("battles", [&](const nlohmann::json& e) {
        return field(e, "c") == cid || ownerOf(field(e, "p")) == cid;
    });
    if (j.contains("countryFields") && j["countryFields"].is_array())
        for (auto& f : j["countryFields"])
            if (f.is_object() && f.contains("v") && f["v"].is_array()) {
                nlohmann::json out = nlohmann::json::array();
                for (const auto& e : f["v"]) if (allowed(field(e, "c"))) out.push_back(e);
                f["v"] = out;
            }
    // Orders are private even between allies: the seat's own, nobody else's.
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& key = it.key();
        if (key.rfind("pending", 0) != 0 || !it.value().is_array()) continue;
        nlohmann::json out = nlohmann::json::array();
        for (const auto& e : it.value()) {
            if (!e.is_object()) continue;
            bool mine = false;
            if (e.contains("countryId")) mine = field(e, "countryId") == cid;
            else if (e.contains("provinceId")) mine = ownerOf(field(e, "provinceId")) == cid;
            else if (e.contains("shipIndex")) {
                const int si = field(e, "shipIndex");
                mine = si >= 0 && si < (int)m_ships.size() && m_ships[(size_t)si].countryId == cid;
            } else if (e.contains("sourceIso")) {
                const Country* c = m_countries.getCountry(cid);
                mine = c && e["sourceIso"].is_string() && e["sourceIso"].get<std::string>() == c->isoA3;
            }
            if (mine) out.push_back(e);
        }
        it.value() = out;
    }
    // Espionage: this seat's own operations and grants only.
    filterArray("spyOps", [&](const nlohmann::json& e) { return field(e, "spy") == cid; });
    filterArray("intelGrants", [&](const nlohmann::json& e) { return field(e, "spy") == cid; });
    cidKeyed("counterIntel", [&](int c) { return c == cid; });
    filterArray("spyCaught", [&](const nlohmann::json& e) {
        return e.is_array() && e.size() == 3 && (e[0] == cid || e[1] == cid);
    });

    // What this seat has seen, which turns sent to it from here could never
    // rebuild: a joiner has no history to replay.
    {
        std::vector<odfog::CountryMemory> one(1);
        one[0].cid = cid;
        auto mem = m_fogMemory.find(cid);
        if (mem != m_fogMemory.end()) one[0].provinces = mem->second;
        auto sh = m_fogSeenShips.find(cid);
        if (sh != m_fogSeenShips.end()) one[0].ships = sh->second;
        j["fogSeatMemory"] = b64(odfog::encode(one, m_fogStartOwners));
        j["fogSeatCid"] = cid;
    }
    return j.dump();
}

// The client's half of the above: take the memory the host sent for our seat.
void Game::fogTakeSeatMemory(const std::string& stateJson) {
    if (!m_fogOfWar || stateJson.find("fogSeatMemory") == std::string::npos) return;
    try {
        const auto j = nlohmann::json::parse(stateJson);
        if (!j.contains("fogSeatMemory") || !j["fogSeatMemory"].is_string()) return;
        std::vector<odfog::CountryMemory> all;
        if (!odfog::decode(unb64(j["fogSeatMemory"].get<std::string>()), m_fogStartOwners, all)) return;
        for (auto& cm : all) {
            m_fogMemory[cm.cid] = std::move(cm.provinces);
            m_fogSeenShips[cm.cid] = std::move(cm.ships);
        }
    } catch (...) {}
}

// --- the host's sends ------------------------------------------------------------

std::vector<uint8_t> Game::fogSnapshotFor(int cid, const TurnDelta& real) {
    NetWorldSnapshot snap;
    snap.mapName = m_mpMapId;
    snap.turnNumber = (uint32_t)m_turnNumber;
    snap.stateJson = fogStateJsonFor(cid);
    FogMirror& m = m_fogMirrors[cid];
    m = FogMirror{};
    TurnDelta full = fogSeatDelta(cid, m, true, real);
    full.turnNumber = m_turnNumber;
    std::vector<uint8_t> packed = SaveManager::packTurn(full);
    if (!packed.empty()) snap.turns.push_back(NetTurnDelta{(uint32_t)m_turnNumber, std::move(packed)});
    return snap.encode();
}

bool Game::fogSendSnapshot(uint16_t peerId, int cid) {
    if (!m_netHost) return false;
    if (cid <= 0 || !fogCountry(cid)) {
        mpNote(T("A spectator joined a fog-of-war game; spectators are not sent the world."));
        return false;
    }
    TurnDelta real;
    real.turnNumber = m_turnNumber;
    real.researchAllocation = m_researchAllocation;
    real.pacificationAllocation = m_pacificationAllocation;
    real.researchActiveNode = m_researchGroups[0].activeNode;
    for (int g = 0; g < 3; ++g) {
        real.researchGroups[g].activeNode = m_researchGroups[g].activeNode;
        real.researchGroups[g].lastNode = m_researchGroups[g].lastNode;
        real.researchGroups[g].sharePct = m_researchGroups[g].sharePct;
        real.researchGroups[g].autoAdvance = m_researchGroups[g].autoAdvance;
    }
    real.researchPoints = m_researchPoints;
    return m_netHost->sendSnapshot(peerId, (uint32_t)m_turnNumber, fogSnapshotFor(cid, real));
}

void Game::fogSendTurn(uint32_t turn, const TurnDelta& real) {
    if (!m_netHost) return;
    m_netHost->closeTurn();
    for (const NetPeer& p : m_netHost->lobby().roster()) {
        if (!p.connected || p.peerId == 0 || p.peerId == m_netHost->lobby().hostPeerId()) continue;
        const int cid = p.countryId;
        if (cid <= 0 || !fogCountry(cid)) continue;          // spectators: see fogSendSnapshot
        auto it = m_fogMirrors.find(cid);
        if (it == m_fogMirrors.end()) {                     // never sent a world: send one now
            fogSendSnapshot(p.peerId, cid);
            continue;
        }
        TurnDelta mine = fogSeatDelta(cid, it->second, false, real);
        mine.turnNumber = (int)turn;
        m_netHost->sendDeltaTo(p.peerId, turn, SaveManager::packTurn(mine));
        std::vector<uint8_t> orders = mpSerializeTurnOrders(cid);
        if (!orders.empty()) m_netHost->sendTurnOrdersTo(p.peerId, turn, orders);
    }
}
