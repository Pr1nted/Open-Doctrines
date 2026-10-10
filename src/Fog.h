// Fog of war: what one country can see, and what it remembers.
//
// PURE, AND DELIBERATELY SO. No raylib, no Game: the world comes in as plain
// arrays, so every rule here runs in tests/fog_test.cpp without a window, a map
// or a save. The game half -- building that world from the live state, the
// accessors the UI and the AI read through, saving -- is src/Game_Fog.cpp.
// The whole design is docs/design/fog-of-war-plan.md.
//
// What a country sees this turn:
//   1. its own provinces, and every province within `landRings` steps of them
//      across the province adjacency graph;
//   2. every province whose centre, and every ship that, lies inside one of its
//      sight circles: one per own coastal province (coast watch) and one per own
//      ship (radar). The map wraps east-west, so circles do too;
//   3. its partners' OWN sight (allies, and guarantees in either direction) --
//      one hop, never transitive, so intel does not leak along a chain.
//
// What it remembers: the last values it saw for every province, stamped with
// the turn it saw them. A province never seen keeps the owner it had on the
// starting map, because the atlas is public; nothing else about it is known.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace odfog {

constexpr uint16_t kNeverSeen = 0xFFFF;

// A set of indices -- province ids or ship slots. Grows on set().
struct Bits {
    std::vector<uint64_t> w;

    void clear() { w.clear(); }
    void set(size_t i) {
        const size_t k = i >> 6;
        if (k >= w.size()) w.resize(k + 1, 0);
        w[k] |= (uint64_t)1 << (i & 63);
    }
    bool test(size_t i) const {
        const size_t k = i >> 6;
        return k < w.size() && ((w[k] >> (i & 63)) & 1u);
    }
    void orWith(const Bits& o) {
        if (o.w.size() > w.size()) w.resize(o.w.size(), 0);
        for (size_t k = 0; k < o.w.size(); ++k) w[k] |= o.w[k];
    }
    size_t count() const;
};

struct SightCircle { float x = 0, y = 0, r = 0; };

// What one country knows about one province.
struct ProvinceIntel {
    uint16_t owner = 0;
    uint16_t seenTurn = kNeverSeen;   // turn of the last sighting
    uint32_t garrison = 0;            // troops present, all flags
    uint32_t population = 0;
    uint8_t industry = 0, fort = 0, port = 0, flags = 0;
};

// A ship as it was last seen. Ships have no stable identity, so a sighting is
// a picture, not a reference: Phase 2 draws these as fading ghosts.
struct SeenShip {
    int owner = 0;
    float x = 0, y = 0;
    uint8_t type = 0;      // see shipTypeCode()
    uint16_t turn = 0;
};

// The world as the fog needs it, built by the game once per recompute.
// Province arrays are indexed by province id; ids with no province are owner 0.
struct World {
    int mapW = 0, mapH = 0;
    std::vector<int> owner;                    // pid -> country id (0 = none)
    std::vector<std::vector<int>> neighbors;   // pid -> adjacent pids
    std::vector<float> cx, cy;                 // pid -> centre, map pixels
    std::vector<uint8_t> coastal;              // pid -> 1 if coastal
    struct Ship { int owner = 0; float x = 0, y = 0, radar = 0; uint8_t type = 0; };
    std::vector<Ship> ships;
};

// Per-country sight parameters. Phase 5's research scales these.
struct SightParams {
    int landRings = 1;
    float coastSight = 90.0f;   // px
    float radarScale = 1.0f;    // multiplies each ship's radar
};

struct Sight {
    Bits provinces;                    // by province id
    Bits ships;                        // by ship index
    std::vector<SightCircle> sea;      // coasts and radar, for the renderer and the rules
};

// Province and ship buckets, built once per world and shared by every country.
class Index {
public:
    void build(const World& w, float cell = 128.0f);
    // Calls f(pid) for every province whose centre is inside the circle.
    template <class F> void provincesIn(const SightCircle& c, F f) const;
    // Calls f(shipIndex) for every ship inside the circle.
    template <class F> void shipsIn(const SightCircle& c, F f) const;

private:
    template <class F>
    void forCells(const SightCircle& c, const std::vector<std::vector<int>>& cells, F f) const;
    const World* m_w = nullptr;
    // Cells divide the map EXACTLY, so the last column ends at the east edge and
    // a circle wrapping across it lands in column 0, not in a gap past the map.
    float m_cellW = 128.0f, m_cellH = 128.0f;
    int m_cols = 0, m_rows = 0;
    std::vector<std::vector<int>> m_prov, m_ship;
};

// Distance test on a map that wraps east-west.
bool inCircle(const SightCircle& c, float x, float y, int mapW);

// A country's OWN sight: its land, its rings, its coasts and its ships.
Sight ownSight(const World& w, const Index& idx, int cid, const SightParams& p);

// Add a partner's own sight. Callers pass base sights, never already-shared
// ones, which is what keeps sharing one hop.
void shareInto(Sight& into, const Sight& partnerOwn);

// The truth about one province, as the game hands it over for remembering.
struct ProvinceTruth {
    uint32_t garrison = 0, population = 0;
    uint8_t industry = 0, fort = 0, port = 0;
};

// Starting memory: every province's owner from the starting map, never seen.
std::vector<ProvinceIntel> seedMemory(const std::vector<int>& startOwners);

// Write the truth for every visible province into `memory`.
void remember(std::vector<ProvinceIntel>& memory, const Bits& visible, const World& w,
              const std::vector<ProvinceTruth>& truth, uint16_t turn);

// A small, stable code per ship type ("boat" 1, "destroyer" 2, "carrier" 3, else 0).
uint8_t shipTypeCode(const std::string& type);

// --- persistence ---------------------------------------------------------
//
// One blob for every country's memory and ship sightings. Province entries are
// written only where they differ from the seeded baseline, so a fresh world is
// a few bytes and a long one is roughly "what each country has ever seen".
struct CountryMemory {
    int cid = 0;
    std::vector<ProvinceIntel> provinces;
    std::vector<SeenShip> ships;
};
std::string encode(const std::vector<CountryMemory>& all, const std::vector<int>& startOwners);
// False on any malformed input; `out` is then left empty. Never throws.
bool decode(const std::string& blob, const std::vector<int>& startOwners,
            std::vector<CountryMemory>& out);

// What one country can reckon about another from its memory alone: the input
// the AI reads in place of the truth under fog (docs/design/fog-of-war-plan.md,
// "AI observation").
//
// NEVER ZERO FOR "UNKNOWN". A province never seen does not read as empty -- an
// AI that saw zeros would find undefended neighbours everywhere. It reads as
// the median of what the observer HAS seen anywhere (the prior), and the share
// it stands in for is reported so a model can learn to discount it.
struct Estimate {
    int provinces = 0;              // known to be theirs
    long long population = 0;
    long long army = 0;             // garrisons as last seen, priors where never seen
    float industrySum = 0, fortSum = 0;
    int maxPort = 0;
    int boats = 0, destroyers = 0, carriers = 0;   // ships seen recently
    int neverSeen = 0;              // of `provinces`, how many stand on the prior
    float meanStaleness = 0;        // turns since sighting, over the seen ones
};
Estimate estimate(const std::vector<ProvinceIntel>& memory, const std::vector<SeenShip>& ships,
                  int other, uint16_t now);

// ---------------------------------------------------------------------------

template <class F>
void Index::forCells(const SightCircle& c, const std::vector<std::vector<int>>& cells, F f) const {
    if (!m_w || m_cols <= 0 || m_rows <= 0) return;
    const int y0 = std::max(0, (int)std::floor((c.y - c.r) / m_cellH));
    const int y1 = std::min(m_rows - 1, (int)std::floor((c.y + c.r) / m_cellH));
    // Columns may run off either edge; they wrap.
    const int x0 = (int)std::floor((c.x - c.r) / m_cellW);
    const int x1 = (int)std::floor((c.x + c.r) / m_cellW);
    const int span = std::min(x1 - x0 + 1, m_cols);
    for (int gy = y0; gy <= y1; ++gy)
        for (int k = 0; k < span; ++k) {
            int gx = (x0 + k) % m_cols;
            if (gx < 0) gx += m_cols;
            for (int i : cells[(size_t)gy * m_cols + gx]) f(i);
        }
}

template <class F>
void Index::provincesIn(const SightCircle& c, F f) const {
    forCells(c, m_prov, [&](int pid) {
        if (inCircle(c, m_w->cx[pid], m_w->cy[pid], m_w->mapW)) f(pid);
    });
}

template <class F>
void Index::shipsIn(const SightCircle& c, F f) const {
    forCells(c, m_ship, [&](int si) {
        const auto& s = m_w->ships[(size_t)si];
        if (inCircle(c, s.x, s.y, m_w->mapW)) f(si);
    });
}

}  // namespace odfog
