// Random events (od-events/1) — the deterministic stochastic-flavour layer.
// Design: docs/design/random-events-plan.md. The one principle: event firing is
// a PURE function of (m_worldSeed, m_turnNumber, deterministic state), drawn from
// a dedicated per-check derived stream, so enabling events perturbs nothing else
// and replay reproduces them with no stored state.
//
// Nothing here may touch randRange() (Game_AITrain.cpp) — its
// std::uniform_*_distribution is implementation-defined across standard
// libraries and would desync multiplayer. The hand-rolled EventRng below is
// identical on every platform.

#include "Game.h"
#include "util/OdEnv.h"
#include "json.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

// splitmix64 — a self-contained, cross-platform integer hash/PRNG step.
inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// One ordered stream for a single (event, scope-key) resolution. Independent
// across (event, key) because the seed mixes all four coordinates; ordered
// within, so a fire-roll then a rand-draw are a fixed sequence.
struct EventRng {
    uint64_t s;
    EventRng(uint32_t seed, uint32_t turn, uint32_t eid, uint32_t key) {
        // Distinct odd multipliers so no two coordinates alias each other.
        uint64_t m = (uint64_t)seed * 0x100000001B3ull;
        m ^= (uint64_t)turn * 0xD1B54A32D192ED03ull;
        m ^= (uint64_t)eid  * 0xA0761D6478BD642Full;
        m ^= (uint64_t)key  * 0xE7037ED1A0B428DBull;
        s = splitmix64(m);
    }
    uint64_t next() { s = splitmix64(s); return s; }
    // Lemire bounded reduction — identical on every platform.
    uint32_t below(uint32_t n) { return n ? (uint32_t)(((next() >> 32) * (uint64_t)n) >> 32) : 0; }
    float unit() { return (float)(next() >> 40) * (1.0f / 16777216.0f); } // [0,1)
};

// FNV-1a over the id string: the per-event seed coordinate. Stable across runs
// and platforms, which is all the derived stream needs.
uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

int resourceIndex(const std::string& name) {
    if (name == "oil") return 0;
    if (name == "gold") return 1;
    if (name == "rubber") return 2;
    if (name == "gemstones" || name == "gem") return 3;
    if (name == "metal") return 4;
    return -1;
}

} // namespace

// ── Loading ──────────────────────────────────────────────────────────────

// Parse one od-events/1 pack into m_eventDefs. Returns false (and leaves
// m_eventDefs untouched beyond what it appended) on a malformed file; a bad pack
// is skipped, never fatal — a mod must not be able to crash the game.
bool Game::eventLoadPack(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    nlohmann::json j;
    try { j = nlohmann::json::parse(f); }
    catch (const std::exception& e) {
        fprintf(stderr, "[EVENTS] skipping %s: parse error: %s\n", path.c_str(), e.what());
        return false;
    }
    if (!j.is_object() || j.value("format", std::string()) != "od-events/1") {
        fprintf(stderr, "[EVENTS] skipping %s: not an od-events/1 pack\n", path.c_str());
        return false;
    }
    const auto it = j.find("events");
    if (it == j.end() || !it->is_array()) return false;

    int loaded = 0, skipped = 0;
    for (const auto& ev : *it) {
        if (!ev.is_object() || !ev.contains("id")) { ++skipped; continue; }
        // Choice events are not resolved yet (the prompt UI + input record come
        // in a later increment). Skip them rather than half-apply.
        if (ev.contains("choices")) { ++skipped; continue; }

        EventDef d;
        d.id = ev.value("id", std::string());
        if (d.id.empty()) { ++skipped; continue; }
        d.idHash = fnv1a(d.id);
        d.chance = ev.value("chance", 0.0f);
        d.weight = ev.value("weight", 1);
        d.cooldown = ev.value("cooldown", 0);
        const std::string sc = ev.value("scope", std::string("province"));
        d.scope = sc == "global"  ? EventScope::Global
                : sc == "country" ? EventScope::Country
                                  : EventScope::Province;
        d.title = ev.value("title", std::string());
        d.text  = ev.value("text",  std::string());

        if (ev.contains("when") && ev["when"].is_object()) {
            const auto& w = ev["when"];
            d.when.minPopulation = w.value("min_population", (long long)0);
            d.when.minIndustry   = w.value("min_industry", 0);
            if (w.contains("has_resource"))
                d.when.resourceReq = resourceIndex(w["has_resource"].get<std::string>());
            // owner: "any" / "player_or_ai" both mean "a real country" here.
            d.when.ownerReal = true;
        }

        auto parseEffects = [&](const nlohmann::json& arr) {
            for (const auto& ef : arr) {
                if (!ef.is_object() || !ef.contains("op")) continue;
                EventEffect e;
                const std::string op = ef["op"].get<std::string>();
                if (op == "treasury") e.op = EventEffect::Treasury;
                else if (op == "population") e.op = EventEffect::Population;
                else continue; // unknown op: ignore, never guess
                // "add" is the only magnitude key; fixed number or "rand:lo..hi".
                if (ef.contains("add")) {
                    const auto& a = ef["add"];
                    if (a.is_string()) {
                        const std::string s = a.get<std::string>();
                        int lo = 0, hi = 0;
                        if (sscanf(s.c_str(), "rand:%d..%d", &lo, &hi) == 2 && hi >= lo) {
                            e.isRand = true; e.lo = lo; e.hi = hi;
                        } else continue; // malformed magnitude: skip this effect
                    } else {
                        e.value = a.get<float>();
                    }
                } else continue;
                d.effects.push_back(e);
            }
        };
        if (ev.contains("effects") && ev["effects"].is_array()) parseEffects(ev["effects"]);
        if (d.effects.empty()) { ++skipped; continue; }

        m_eventDefs.push_back(std::move(d));
        ++loaded;
    }
    fprintf(stderr, "[EVENTS] %s: %d event(s) loaded, %d skipped\n", path.c_str(), loaded, skipped);
    return loaded > 0;
}

void Game::loadEventPacks() {
    m_eventDefs.clear();
    m_eventCooldown.clear();
    // Explicit single-pack override (tests, a modder iterating on one file).
    if (const char* p = OD_ENV("OD_EVENTS_PACK")) { eventLoadPack(p); return; }
    // Otherwise the shipped pack. (Map-archive and mod packs are a later
    // increment; the loader itself already handles any number of packs.)
    eventLoadPack(m_dataDir + "events/sample.json");
}

// ── Scope keys, predicates, firing ─────────────────────────────────────────

// The set of keys an event iterates, in a STABLE sorted order. Unordered
// iteration would desync multiplayer (unordered_map walk order is
// platform-dependent), so every source here is index- or std::map-ordered.
std::vector<uint32_t> Game::eventScopeKeys(const EventDef& e) const {
    std::vector<uint32_t> keys;
    switch (e.scope) {
        case EventScope::Global:
            keys.push_back(0);
            break;
        case EventScope::Country:
            // m_countries is a std::map<int,Country> — already id-sorted.
            for (const auto& [id, c] : m_countries.getAll()) {
                (void)c;
                if (id > 0 && id < SPC_CID) keys.push_back((uint32_t)id);
            }
            break;
        case EventScope::Province:
            // m_provinceCountryLookup is indexed by province id — index order
            // is the stable sort. Only provinces owned by a real country.
            for (int pid = 1; pid < (int)m_provinceCountryLookup.size(); ++pid) {
                const int owner = m_provinceCountryLookup[(size_t)pid];
                if (owner > 0 && owner < SPC_CID) keys.push_back((uint32_t)pid);
            }
            break;
    }
    return keys;
}

bool Game::eventMatches(const EventDef& e, uint32_t key) const {
    const EventWhen& w = e.when;
    if (e.scope == EventScope::Province) {
        const int pid = (int)key;
        const int owner = (pid < (int)m_provinceCountryLookup.size())
                              ? m_provinceCountryLookup[(size_t)pid] : 0;
        if (w.ownerReal && !(owner > 0 && owner < SPC_CID)) return false;
        if (w.minPopulation > 0) {
            auto it = m_provincePopulations.find(pid);
            if (it == m_provincePopulations.end() || it->second < w.minPopulation) return false;
        }
        if (w.minIndustry > 0) {
            auto it = m_provinceIndustry.find(pid);
            if (it == m_provinceIndustry.end() || it->second.level < w.minIndustry) return false;
        }
        if (w.resourceReq >= 0) {
            auto it = m_provinceResources.find(pid);
            if (it == m_provinceResources.end()) return false;
            const ProvinceResources& r = it->second;
            const float amt = w.resourceReq == 0 ? r.oil.amount
                            : w.resourceReq == 1 ? r.gold.amount
                            : w.resourceReq == 2 ? r.rubber.amount
                            : w.resourceReq == 3 ? r.gemstones.amount
                                                 : r.metal.amount;
            if (amt <= 0.0f) return false;
        }
        return true;
    }
    if (e.scope == EventScope::Country) {
        const int cid = (int)key;
        if (w.ownerReal && m_countries.getCountry(cid) == nullptr) return false;
        return true; // country-scope population/industry predicates: later
    }
    return true; // global
}

bool Game::eventFires(const EventDef& e, uint32_t key) const {
    if (e.chance <= 0.0f) return false;
    EventRng rng(m_worldSeed, (uint32_t)m_turnNumber, e.idHash, key);
    return rng.unit() < e.chance; // consumes nothing shared
}

// ── Applying a fired event ──────────────────────────────────────────────────

void Game::eventResolve(const EventDef& e, uint32_t key) {
    // The province (if any) and its owning country, resolving {province}/{owner}.
    int pid = (e.scope == EventScope::Province) ? (int)key : -1;
    int ownerCid = (e.scope == EventScope::Country) ? (int)key
                 : (pid >= 0 && pid < (int)m_provinceCountryLookup.size())
                       ? m_provinceCountryLookup[(size_t)pid]
                       : 0;

    // One fresh derived stream for this resolution; rand effects draw in array
    // order, so the sequence is fixed and pure in the four coordinates.
    EventRng rng(m_worldSeed, (uint32_t)m_turnNumber, e.idHash, key);

    for (const EventEffect& ef : e.effects) {
        double amt = ef.isRand ? (double)(ef.lo + (int)rng.below((uint32_t)(ef.hi - ef.lo + 1)))
                               : (double)ef.value;
        switch (ef.op) {
            case EventEffect::Treasury: {
                if (Country* c = m_countries.getCountry(ownerCid)) c->treasury += amt;
                break;
            }
            case EventEffect::Population: {
                if (pid >= 0) {
                    long long& p = m_provincePopulations[pid];
                    p += (long long)amt;
                    if (p < 0) p = 0;
                    if (p > MAX_PROVINCE_POP) p = MAX_PROVINCE_POP;
                    // Keep the parallel array in sync — the economy and parts of
                    // the AI read m_provincePopArray in preference to the map, so
                    // writing only the map would make the effect half-invisible.
                    if (pid > 0 && (size_t)pid < m_provincePopArray.size())
                        m_provincePopArray[(size_t)pid] = p;
                }
                break;
            }
        }
    }

    // Flavour line. {province} uses the id for now (a province-name lookup is a
    // UI nicety for the toast increment); {owner} uses the country id likewise.
    if (!e.title.empty() || !e.text.empty()) {
        fprintf(stderr, "[EVENT] turn=%d %s (scope-key=%u, owner=%d): %s\n",
                m_turnNumber, e.id.c_str(), key, ownerCid,
                e.title.empty() ? e.text.c_str() : e.title.c_str());
    }
}

// ── The per-turn phase ──────────────────────────────────────────────────────

void Game::processEvents() {
    if (m_eventDefs.empty()) return;

    struct Fired { uint32_t idHash; uint32_t key; int defIdx; };
    std::vector<Fired> fired;
    for (int i = 0; i < (int)m_eventDefs.size(); ++i) {
        const EventDef& e = m_eventDefs[(size_t)i];
        for (uint32_t key : eventScopeKeys(e)) {
            if (e.cooldown > 0) {
                const uint64_t ck = ((uint64_t)e.idHash << 32) | key;
                auto it = m_eventCooldown.find(ck);
                if (it != m_eventCooldown.end() && (m_turnNumber - it->second) < e.cooldown)
                    continue;
            }
            if (eventMatches(e, key) && eventFires(e, key))
                fired.push_back({e.idHash, key, i});
        }
    }
    // Deterministic apply order: two events touching one province must resolve
    // the same way on every machine and every replay, regardless of check order.
    std::sort(fired.begin(), fired.end(), [](const Fired& a, const Fired& b) {
        return a.idHash != b.idHash ? a.idHash < b.idHash : a.key < b.key;
    });
    for (const Fired& f : fired) {
        if (m_eventDefs[(size_t)f.defIdx].cooldown > 0)
            m_eventCooldown[((uint64_t)f.idHash << 32) | f.key] = m_turnNumber;
        eventResolve(m_eventDefs[(size_t)f.defIdx], f.key);
    }
}
