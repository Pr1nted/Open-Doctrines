// Monuments: the game's half. The rules are in src/Monuments.h, which has no
// world in it and is tested on its own.
//
// What lives here is everything that needs a map: which province a monument
// stands in, whether it may be built there, what its effect reaches, and what
// the slots cost this country this turn.
//
// ── ONE PLACE ANSWERS, AND FOUR THINGS ASK ──
//
// The panel, a map script, a mod through the ABI and the AI all ask the same
// questions, and the answers must be the same for all of them. So the
// accessors below are the whole interface -- monumentKindAt, monumentLevelAt,
// monumentActiveAt, monumentEffect, monumentEffectAt -- and nothing outside
// this file works an effect out from the catalogue for itself. See the note in
// Monuments.h about what a second copy of a rule costs.
//
// ── THE EFFECT CACHE, AND WHY IT IS NOT COMPUTED ON DEMAND ──
//
// monumentEffectAt is asked per province while drawing, which is thousands of
// calls a frame, and the honest answer needs a walk of the province graph out
// to the monument's radius. So it is built the other way round and once: each
// active monument walks outward to its own radius and adds itself to a map,
// rebuilt when something changes and at the start of a turn. A country with
// eleven monuments and a radius of three walks a few hundred provinces; a
// frame that asked the question directly would walk that per province.

#include "Game.h"

#include "GameInternals.h"

#include <algorithm>
#include <queue>

// ── What is where ───────────────────────────────────────────────────────────

/**
 * Who owns a province, for this file's purposes.
 *
 * m_provinceCountryLookup is the fast path the loader builds and every turn
 * keeps current; the map is the fallback for a province outside it, which is
 * what a save from a bigger map looks like while it is being loaded.
 */
int Game::monumentOwnerOf(int pid) const {
    if (pid > 0 && (size_t)pid < m_provinceCountryLookup.size())
        return m_provinceCountryLookup[pid];
    const Province* p = m_provinces.getProvinceById(pid);
    return p ? p->countryId : 0;
}

int Game::monumentKindAt(int pid) const {
    auto it = m_monuments.find(pid);
    return it == m_monuments.end() ? -1 : (int)it->second.kind;
}

int Game::monumentLevelAt(int pid) const {
    auto it = m_monuments.find(pid);
    return it == m_monuments.end() ? 0 : it->second.level;
}

bool Game::monumentActiveAt(int pid) const {
    auto it = m_monuments.find(pid);
    return it != m_monuments.end() && it->second.active;
}

std::vector<odmon::Holding> Game::monumentsOf(int countryId) const {
    std::vector<odmon::Holding> out;
    for (const auto& [pid, h] : m_monuments) {
        if (monumentOwnerOf(pid) != countryId) continue;
        out.push_back(h);
    }
    // Sorted by province id, so the panel lists them the same way twice and a
    // replay on another machine agrees. chargeOrder sorts it again for money.
    std::sort(out.begin(), out.end(),
              [](const odmon::Holding& a, const odmon::Holding& b) {
                  return a.provinceId < b.provinceId;
              });
    return out;
}

int Game::monumentSlotsUsed(int countryId) const {
    return odmon::activeCount(monumentsOf(countryId));
}

float Game::monumentUpkeep(int countryId) const {
    return odmon::slotUpkeep(monumentSlotsUsed(countryId));
}

float Game::monumentNextSlotCost(int countryId) const {
    return odmon::slotCost(monumentSlotsUsed(countryId) + 1);
}

// ── Whether one may be built ────────────────────────────────────────────────

bool Game::canBuildMonument(int countryId, int pid, int kindIndex,
                            std::string& whyNot) const {
    whyNot.clear();
    if (kindIndex < 0 || kindIndex >= odmon::kKindCount) {
        whyNot = "No such monument.";
        return false;
    }
    const odmon::Kind kind = (odmon::Kind)kindIndex;
    const Province* p = m_provinces.getProvinceById(pid);
    if (!p) { whyNot = "No such province."; return false; }
    if (p->countryId != countryId) {
        whyNot = "That province is not yours.";
        return false;
    }
    // ONE PER PROVINCE. A monument is the thing a province is known for, and a
    // province with three of them is a province with none.
    if (m_monuments.count(pid)) {
        whyNot = "This province already has a monument.";
        return false;
    }
    if (!hasResearched(odmon::unlockNode(kind), countryId)) {
        whyNot = "You have not researched it yet.";
        return false;
    }
    if (odmon::spec(kind).needsPort && !isProvinceCoastal(pid)) {
        whyNot = "It has to stand on the coast.";
        return false;
    }
    const Country* c = m_countries.getCountry(countryId);
    if (c && c->treasury < odmon::levelCost(kind, 0)) {
        whyNot = "You cannot afford it.";
        return false;
    }
    return true;
}

bool Game::buildMonument(int countryId, int pid, int kindIndex) {
    std::string why;
    if (!canBuildMonument(countryId, pid, kindIndex, why)) return false;
    const odmon::Kind kind = (odmon::Kind)kindIndex;

    auto& all = m_countries.getAll();
    auto cit = all.find(countryId);
    if (cit == all.end()) return false;
    cit->second.treasury -= odmon::levelCost(kind, 0);

    odmon::Holding h;
    h.provinceId = pid;
    h.kind = kind;
    h.level = 1;
    // ACTIVE ON ARRIVAL. A monument that had to be switched on after it was
    // built would be switched on by everybody every time, which is a click
    // rather than a decision -- and the slot it takes is charged from the next
    // turn, where the player will see it on the economy screen.
    h.active = true;
    m_monuments[pid] = h;
    rebuildMonumentEffects();
    return true;
}

bool Game::upgradeMonument(int countryId, int pid) {
    auto it = m_monuments.find(pid);
    if (it == m_monuments.end() || monumentOwnerOf(pid) != countryId) return false;
    const odmon::Spec& s = odmon::spec(it->second.kind);
    if (it->second.level >= s.maxLevel) return false;
    const float cost = odmon::levelCost(it->second.kind, it->second.level);

    auto& all = m_countries.getAll();
    auto cit = all.find(countryId);
    if (cit == all.end() || cit->second.treasury < cost) return false;
    cit->second.treasury -= cost;
    it->second.level++;
    rebuildMonumentEffects();
    return true;
}

bool Game::dismantleMonument(int countryId, int pid) {
    auto it = m_monuments.find(pid);
    if (it == m_monuments.end() || monumentOwnerOf(pid) != countryId) return false;
    auto& all = m_countries.getAll();
    auto cit = all.find(countryId);
    if (cit == all.end() || cit->second.treasury < odmon::kDismantleCost) return false;
    cit->second.treasury -= odmon::kDismantleCost;
    m_monuments.erase(it);
    rebuildMonumentEffects();
    return true;
}

bool Game::setMonumentActive(int countryId, int pid, bool active) {
    auto it = m_monuments.find(pid);
    if (it == m_monuments.end() || monumentOwnerOf(pid) != countryId) return false;
    if (it->second.active == active) return true;
    it->second.active = active;
    rebuildMonumentEffects();
    return true;
}

bool Game::moveMonument(int countryId, int fromPid, int toPid) {
    auto it = m_monuments.find(fromPid);
    if (it == m_monuments.end() || monumentOwnerOf(fromPid) != countryId) return false;
    if (!odmon::spec(it->second.kind).movable) return false;
    if (m_monuments.count(toPid)) return false;
    const Province* dst = m_provinces.getProvinceById(toPid);
    // ANY LAND YOU HOLD. "Moving defence corporation could be anywhere where
    // the land is available" -- so the rule is ownership, not adjacency: this
    // is a thing that gets packed up and driven, and the map has no notion of
    // how long that takes.
    if (!dst || dst->countryId != countryId) return false;
    if (odmon::spec(it->second.kind).needsPort && !isProvinceCoastal(toPid)) return false;

    const float cost = odmon::moveCost(it->second.kind, it->second.level);
    auto& all = m_countries.getAll();
    auto cit = all.find(countryId);
    if (cit == all.end() || cit->second.treasury < cost) return false;
    cit->second.treasury -= cost;

    odmon::Holding moved = it->second;
    moved.provinceId = toPid;
    m_monuments.erase(it);
    m_monuments[toPid] = moved;
    rebuildMonumentEffects();
    return true;
}

bool Game::destroyMonumentByOrdnance(int pid) {
    auto it = m_monuments.find(pid);
    if (it == m_monuments.end()) return false;
    // Only the fragile ones, which are exactly the movable ones: the trade
    // they make is that they go where they are needed and do not survive being
    // found. Everything else is rubble that still works.
    if (!odmon::spec(it->second.kind).fragile) return false;
    m_monuments.erase(it);
    rebuildMonumentEffects();
    return true;
}

// ── What the map feels ──────────────────────────────────────────────────────

void Game::rebuildMonumentEffects() {
    m_monumentEffects.clear();
    if (m_monuments.empty()) return;

    // Grouped by country and kind, because the harmonic decay is per country
    // per kind: a country's second university is worth half, and somebody
    // else's university is not its business.
    std::unordered_map<int, std::array<std::vector<odmon::Holding>, odmon::kKindCount>> byKind;
    for (const auto& [pid, h] : m_monuments) {
        if (!h.active) continue;
        const int cid = monumentOwnerOf(pid);
        if (cid <= 0) continue;
        byKind[cid][(int)h.kind].push_back(h);
    }

    for (auto& [cid, kinds] : byKind) {
        // The country's population, once, for the share every effect scales on.
        long long countryPop = 0;
        for (int pid : provincesOf(cid)) {
            if (pid > 0 && (size_t)pid < m_provincePopArray.size())
                countryPop += m_provincePopArray[pid];
        }
        MonumentEffects& out = m_monumentEffects[cid];

        for (int k = 0; k < odmon::kKindCount; ++k) {
            std::vector<odmon::Holding>& list = kinds[k];
            if (list.empty()) continue;
            const odmon::Kind kind = (odmon::Kind)k;

            // RANKED BY WHAT EACH IS WORTH, largest first, so the decay takes
            // the small ones down rather than whichever the map listed first.
            auto worth = [&](const odmon::Holding& h) {
                long long pop = 0;
                if (h.provinceId > 0 && (size_t)h.provinceId < m_provincePopArray.size())
                    pop = m_provincePopArray[h.provinceId];
                const float share = countryPop > 0 ? (float)((double)pop / (double)countryPop) : 0.0f;
                return odmon::scaledEffect(kind, share, h.level);
            };
            std::sort(list.begin(), list.end(),
                      [&](const odmon::Holding& a, const odmon::Holding& b) {
                          const float wa = worth(a), wb = worth(b);
                          if (wa != wb) return wa > wb;
                          return a.provinceId < b.provinceId;   // total, so replays agree
                      });

            for (size_t i = 0; i < list.size(); ++i) {
                const odmon::Holding& h = list[i];
                const int rank = (int)i + 1;
                const float e = worth(h) / (float)rank;
                out.national[k] += e;

                // And where it is felt. A radius of 0 means the province it
                // stands in; the effect still lands there for the kinds whose
                // reach is national, which costs one map entry and saves every
                // caller a special case.
                const int r = odmon::radius(kind, h.level);
                for (int pid : provincesWithin(h.provinceId, r)) out.byProvince[k][pid] += e;
            }
        }
    }
}

std::vector<int> Game::provincesWithin(int pid, int steps) const {
    std::vector<int> out{pid};
    if (steps <= 0) return out;
    // Breadth first over the adjacency the loader already built. Bounded by
    // `steps`, which is at most a handful, so this is a few hundred provinces
    // in the worst case and not a walk of the map.
    std::unordered_map<int, int> seen{{pid, 0}};
    std::queue<int> q;
    q.push(pid);
    while (!q.empty()) {
        const int cur = q.front();
        q.pop();
        const int d = seen[cur];
        if (d >= steps) continue;
        auto nit = m_provinceNeighbors.find(cur);
        if (nit == m_provinceNeighbors.end()) continue;
        for (int nb : nit->second) {
            if (seen.count(nb)) continue;
            seen[nb] = d + 1;
            out.push_back(nb);
            q.push(nb);
        }
    }
    return out;
}

float Game::monumentEffect(int countryId, int kindIndex) const {
    if (kindIndex < 0 || kindIndex >= odmon::kKindCount) return 0.0f;
    auto it = m_monumentEffects.find(countryId);
    return it == m_monumentEffects.end() ? 0.0f : it->second.national[kindIndex];
}

float Game::monumentEffectAt(int countryId, int pid, int kindIndex) const {
    if (kindIndex < 0 || kindIndex >= odmon::kKindCount) return 0.0f;
    auto it = m_monumentEffects.find(countryId);
    if (it == m_monumentEffects.end()) return 0.0f;
    const auto& m = it->second.byProvince[kindIndex];
    auto pit = m.find(pid);
    return pit == m.end() ? 0.0f : pit->second;
}
