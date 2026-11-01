// Fog of war: the game half. The rules are src/Fog.h (pure, unit-tested in
// tests/fog_test.cpp); this file builds the fog's view of the live world, runs
// it once a turn, and is the one place the rest of the game asks "can this
// country see that?". Design: docs/design/fog-of-war-plan.md.
//
// NOTHING HERE RUNS IN A WORLD WITHOUT FOG. Every entry point returns at once
// when m_fogOfWar is false, and every accessor answers "yes, visible / the
// truth", so a non-fog world behaves byte for byte as it did before this file.

#include "Game.h"
#include "map/RasterScale.h"

#include <cstring>   // memcpy, for the grant blob
#include "GameInternals.h"
#include "SaveManager.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace {

// Pixels of sight. Phase 5's research scales these per country.
constexpr int   kLandRings  = 1;
constexpr float kCoastSight = 90.0f;
// Sightings kept for ghosts: ships seen this many turns ago still show, faded.
constexpr int   kShipMemoryTurns = 3;
constexpr size_t kShipMemoryCap  = 256;

}  // namespace

bool Game::fogCountry(int cid) const {
    return cid > 0 && cid != UNC_CID && cid != BLC_CID && cid != SPC_CID;
}

int Game::fogMaxPid() const {
    int maxPid = 0;
    for (const auto& [pid, p] : m_provinces.getAllProvinces()) maxPid = std::max(maxPid, pid);
    return maxPid;
}

float Game::shipRadarPx(const NavyShip& s) const {
    // Base radar by hull: smaller than the steaming range (shipMaxRangePx), so
    // a fleet can sail into waters it has not seen. Research scales it (Phase 5).
    // In full-resolution map pixels, as loaded (see map/RasterScale.h).
    if (s.type == "boat")      return odmap::loadedPx(120.0f);
    if (s.type == "destroyer") return odmap::loadedPx(260.0f);
    if (s.type == "carrier")   return odmap::loadedPx(400.0f);
    return odmap::loadedPx(200.0f);
}

bool Game::sharesIntel(int a, int b) const {
    if (a == b) return true;
    if (!fogCountry(a) || !fogCountry(b)) return false;
    if (alliedCids(a, b)) return true;
    const Country* ca = m_countries.getCountry(a);
    const Country* cb = m_countries.getCountry(b);
    if (!ca || !cb) return false;
    return hasRelation(ca->isoA3, cb->isoA3, &CountryRelation::guarantee) ||
           hasRelation(cb->isoA3, ca->isoA3, &CountryRelation::guarantee);
}

// The atlas the world started from. Every country knows it, so a province it
// has never seen still has an owner -- the one on the starting map.
void Game::fogBeginWorld() {
    m_fogMemory.clear();
    m_fogSeenShips.clear();
    m_fogSight.clear();
    m_fogDisplayOwners.clear();
    m_fogCoastal.clear();
    m_fogStartOwners.clear();
    if (!m_fogOfWar) return;
    const int maxPid = fogMaxPid();
    m_fogStartOwners.assign((size_t)maxPid + 1, 0);
    for (int pid = 1; pid <= maxPid && (size_t)pid < m_provinceCountryLookup.size(); ++pid)
        m_fogStartOwners[(size_t)pid] = std::max(0, m_provinceCountryLookup[(size_t)pid]);
}

void Game::recomputeFog() {
    if (!m_fogOfWar) return;
    const int maxPid = fogMaxPid();
    if (maxPid <= 0) return;
    // A world that came in by a door that never called fogBeginWorld (the
    // trainer, the server) takes its atlas from the first turn it is fogged.
    if (m_fogStartOwners.empty()) {
        m_fogStartOwners.assign((size_t)maxPid + 1, 0);
        for (int pid = 1; pid <= maxPid && (size_t)pid < m_provinceCountryLookup.size(); ++pid)
            m_fogStartOwners[(size_t)pid] = std::max(0, m_provinceCountryLookup[(size_t)pid]);
    }
    if (m_fogStartOwners.size() < (size_t)maxPid + 1) m_fogStartOwners.resize((size_t)maxPid + 1, 0);

    // --- the world, as plain arrays ---
    odfog::World w;
    w.mapW = m_landSea.getWidth();
    w.mapH = m_landSea.getHeight();
    w.owner.assign((size_t)maxPid + 1, 0);
    w.neighbors.assign((size_t)maxPid + 1, {});
    w.cx.assign((size_t)maxPid + 1, 0.0f);
    w.cy.assign((size_t)maxPid + 1, 0.0f);
    for (int pid = 1; pid <= maxPid; ++pid) {
        if ((size_t)pid < m_provinceCountryLookup.size())
            w.owner[(size_t)pid] = std::max(0, m_provinceCountryLookup[(size_t)pid]);
        auto nb = m_provinceNeighbors.find(pid);
        if (nb != m_provinceNeighbors.end()) w.neighbors[(size_t)pid] = nb->second;
        auto c = m_provinceCenters.find(pid);
        if (c != m_provinceCenters.end()) {
            w.cx[(size_t)pid] = c->second.x;
            w.cy[(size_t)pid] = c->second.y;
        }
    }
    // Coastal is a property of the map, not of who owns it: work it out once
    // per world. isProvinceCoastal answers from an index that may not exist yet
    // on the very first call, so an empty cache is filled lazily here.
    if (m_fogCoastal.size() != (size_t)maxPid + 1) {
        m_fogCoastal.assign((size_t)maxPid + 1, 0);
        for (int pid = 1; pid <= maxPid; ++pid)
            if (m_provinces.getAllProvinces().count(pid))
                m_fogCoastal[(size_t)pid] = isProvinceCoastal(pid) ? 1 : 0;
    }
    w.coastal = m_fogCoastal;
    w.ships.reserve(m_ships.size());
    for (const NavyShip& s : m_ships) {
        int px = 0, py = 0;
        m_landSea.lonLatToPixel((float)s.lon, (float)s.lat, px, py);
        w.ships.push_back({s.countryId, (float)px, (float)py, shipRadarPx(s),
                           odfog::shipTypeCode(s.type)});
    }
    odfog::Index idx;
    idx.build(w);

    // --- every live country's own sight, then sharing ---
    std::vector<int> cids;
    {
        std::vector<uint8_t> alive(65536, 0);
        for (int pid = 1; pid <= maxPid; ++pid) {
            const int o = w.owner[(size_t)pid];
            if (o > 0 && o < 65536) alive[(size_t)o] = 1;
        }
        for (const auto& s : w.ships)
            if (s.owner > 0 && s.owner < 65536) alive[(size_t)s.owner] = 1;
        for (int c = 1; c < 65536; ++c)
            if (alive[(size_t)c] && fogCountry(c)) cids.push_back(c);   // sorted: deterministic
    }
    // Each country's reach, widened by its intelligence research (fog-only
    // nodes, Game_Research.cpp).
    std::unordered_map<int, odfog::Sight> own;
    own.reserve(cids.size());
    for (int c : cids) {
        odfog::SightParams params;
        params.landRings = kLandRings + std::clamp((int)getResearchEffect("landSightRings", c), 0, 3);
        params.coastSight = kCoastSight * (1.0f + std::max(0.0f, getResearchEffect("coastSightPct", c)) / 100.0f);
        params.radarScale = 1.0f + std::max(0.0f, getResearchEffect("radarRangePct", c)) / 100.0f;
        own[c] = odfog::ownSight(w, idx, c, params);
    }
    // An army sees where it stands and the provinces around it. Without this a
    // stack marching into enemy land walked out of its own country's sight --
    // and in multiplayer its owner would not even be sent where it was.
    for (const auto& [pid, units] : m_provinceArmies) {
        if (pid <= 0 || pid > maxPid) continue;
        for (const ArmyUnit& u : units) {
            if (u.count <= 0) continue;
            auto it = own.find(u.countryId);
            if (it == own.end()) continue;
            it->second.provinces.set((size_t)pid);
            for (int nb : w.neighbors[(size_t)pid])
                if (nb > 0 && nb <= maxPid) it->second.provinces.set((size_t)nb);
        }
    }

    // Sight a mod or a map script granted (gearbox:fog.write). Folded in HERE,
    // into the country's own sight and before sharing, so a granted watchpost
    // reaches an ally one hop exactly as a coast watch does. The spy grants
    // below are deliberately the other way round.
    for (const auto& [gcid, g] : m_fogGrants) {
        auto it = own.find(gcid);
        if (it == own.end()) continue;          // dead or fogless country
        for (int pid : g.provinces)
            if (pid > 0 && pid <= maxPid) it->second.provinces.set((size_t)pid);
        for (const odfog::SightCircle& c : g.circles) {
            it->second.sea.push_back(c);        // the renderer draws it too
            idx.provincesIn(c, [&](int pid) { it->second.provinces.set((size_t)pid); });
            idx.shipsIn(c, [&](int si) { it->second.ships.set((size_t)si); });
        }
    }

    std::unordered_map<int, odfog::Sight> sight;
    sight.reserve(cids.size());
    for (int c : cids) {
        odfog::Sight s = own[c];
        for (int p : cids)
            if (p != c && sharesIntel(c, p)) odfog::shareInto(s, own[p]);   // one hop: own sights only
        // A spy network shows the whole of its target's land -- to the spy
        // only; what it learns is not passed on to allies.
        for (const auto& [key, g] : m_intelGrants) {
            if ((int)(key >> 32) != c || g.networkUntil < m_turnNumber) continue;
            const int target = (int)(uint32_t)key;
            for (int pid = 1; pid <= maxPid; ++pid)
                if (w.owner[(size_t)pid] == target) s.provinces.set((size_t)pid);
        }
        sight[c] = std::move(s);
    }

    // --- memory ---
    std::vector<odfog::ProvinceTruth> truth((size_t)maxPid + 1);
    for (int pid = 1; pid <= maxPid; ++pid) {
        odfog::ProvinceTruth& t = truth[(size_t)pid];
        auto a = m_provinceArmies.find(pid);
        if (a != m_provinceArmies.end()) {
            long long n = 0;
            for (const ArmyUnit& u : a->second) n += std::max(0, u.count);
            t.garrison = (uint32_t)std::min<long long>(n, 0xFFFFFFFFLL);
        }
        auto pop = m_provincePopulations.find(pid);
        if (pop != m_provincePopulations.end())
            t.population = (uint32_t)std::clamp<long long>(pop->second, 0, 0xFFFFFFFFLL);
        auto ind = m_provinceIndustry.find(pid);
        if (ind != m_provinceIndustry.end()) {
            t.industry = (uint8_t)std::clamp(ind->second.level, 0, 255);
            t.fort = (uint8_t)std::clamp(ind->second.fortification, 0, 255);
        }
        auto port = m_provincePorts.find(pid);
        if (port != m_provincePorts.end()) t.port = (uint8_t)std::clamp(port->second.level, 0, 255);
    }
    const uint16_t turn = (uint16_t)std::clamp(m_turnNumber, 0, 0xFFFE);
    for (int c : cids) {
        auto& mem = m_fogMemory[c];
        if (mem.empty()) mem = odfog::seedMemory(m_fogStartOwners);
        odfog::remember(mem, sight[c].provinces, w, truth, turn);

        // Ship sightings: foreign ships seen this turn join the ghost list;
        // anything older than kShipMemoryTurns falls off.
        auto& seen = m_fogSeenShips[c];
        seen.erase(std::remove_if(seen.begin(), seen.end(), [&](const odfog::SeenShip& s) {
                       return (int)turn - (int)s.turn >= kShipMemoryTurns;
                   }), seen.end());
        for (size_t i = 0; i < w.ships.size(); ++i) {
            const auto& sh = w.ships[i];
            if (sh.owner == c || !sight[c].ships.test(i)) continue;
            if (seen.size() >= kShipMemoryCap) break;
            seen.push_back({sh.owner, sh.x, sh.y, sh.type, turn});
        }
    }
    m_fogSight = std::move(sight);
    m_fogTurn = turn;
    rebuildFogDisplayOwners();
}

void Game::rebuildFogDisplayOwners() {
    // The border shading is built from these owners, so when what the player
    // KNOWS changes -- a province comes into sight, or one they remember was
    // taken -- the shading has to be rebuilt even if no real border moved.
    const std::vector<int> before = std::move(m_fogDisplayOwners);
    m_fogDisplayOwners.clear();
    struct Dirty { Game* g; const std::vector<int>& b; ~Dirty() {
        if (g->m_fogDisplayOwners != b) { g->m_gradientDirty = true; g->m_labelsDirty = true; } } }
        dirty{this, before};
    if (m_renderer) m_renderer->setDisplayOwners(&m_fogDisplayOwners);   // empty = the truth
    if (!m_fogOfWar || !fogCountry(m_playerCountryId)) return;   // spectator: the truth
    auto mem = m_fogMemory.find(m_playerCountryId);
    auto vis = m_fogSight.find(m_playerCountryId);
    m_fogDisplayOwners = m_provinceCountryLookup;
    const size_t n = std::min(m_fogDisplayOwners.size(), m_fogStartOwners.size());
    for (size_t pid = 1; pid < n; ++pid) {
        if (vis != m_fogSight.end() && vis->second.provinces.test(pid)) continue;   // seen: truth
        if (mem != m_fogMemory.end() && pid < mem->second.size())
            m_fogDisplayOwners[pid] = mem->second[pid].owner;
        else
            m_fogDisplayOwners[pid] = m_fogStartOwners[pid];
    }
}

// The sea the player can see: their (and their partners') coast-watch and radar
// circles, stamped into a mask an eighth of the map's size per side. 255 seen,
// 0 not. Wraps east-west like the map.
void Game::buildSeaFogMask(std::vector<uint8_t>& mask, int& mw, int& mh) const {
    const int fw = std::max(1, m_provinces.getWidth()), fh = std::max(1, m_provinces.getHeight());
    mw = std::max(1, fw / 8);
    mh = std::max(1, fh / 8);
    mask.assign((size_t)mw * mh, 0);
    auto it = m_fogSight.find(m_playerCountryId);
    if (it == m_fogSight.end()) return;
    const float sx = (float)mw / (float)fw, sy = (float)mh / (float)fh;
    for (const odfog::SightCircle& c : it->second.sea) {
        const float cx = c.x * sx, cy = c.y * sy, r = c.r * sx;
        const int y0 = std::max(0, (int)std::floor(cy - r)), y1 = std::min(mh - 1, (int)std::ceil(cy + r));
        const int x0 = (int)std::floor(cx - r), x1 = (int)std::ceil(cx + r);
        for (int y = y0; y <= y1; ++y) {
            const float dy = ((float)y + 0.5f) - cy;
            for (int x = x0; x <= x1; ++x) {
                const float dx = ((float)x + 0.5f) - cx;
                if (dx * dx + dy * dy > r * r) continue;
                int wx = x % mw; if (wx < 0) wx += mw;
                mask[(size_t)y * mw + wx] = 255;
            }
        }
    }
}

// The fog view belongs to the player's country. When that changes -- the
// country-select screen, a save restoring its seat, the screenshot tour --
// what the map shows has to change with it.
void Game::fogPlayerChanged() {
    if (!m_fogOfWar) return;
    rebuildFogDisplayOwners();
    if (m_renderer) generatePoliticalTexture();
}

// --- accessors ---------------------------------------------------------------

bool Game::provinceVisibleTo(int cid, int pid) const {
    if (!m_fogOfWar || !fogCountry(cid)) return true;
    auto it = m_fogSight.find(cid);
    return it != m_fogSight.end() && it->second.provinces.test((size_t)pid);
}

bool Game::shipVisibleTo(int cid, size_t shipIndex) const {
    if (!m_fogOfWar || !fogCountry(cid)) return true;
    if (shipIndex < m_ships.size() && m_ships[shipIndex].countryId == cid) return true;
    auto it = m_fogSight.find(cid);
    return it != m_fogSight.end() && it->second.ships.test(shipIndex);
}

int Game::knownOwner(int cid, int pid) const {
    const int truth = (pid >= 0 && (size_t)pid < m_provinceCountryLookup.size())
                          ? m_provinceCountryLookup[(size_t)pid] : 0;
    if (provinceVisibleTo(cid, pid)) return truth;
    auto it = m_fogMemory.find(cid);
    if (it != m_fogMemory.end() && (size_t)pid < it->second.size()) return it->second[(size_t)pid].owner;
    return ((size_t)pid < m_fogStartOwners.size()) ? m_fogStartOwners[(size_t)pid] : truth;
}

const odfog::ProvinceIntel* Game::intel(int cid, int pid) const {
    auto it = m_fogMemory.find(cid);
    if (it == m_fogMemory.end() || pid < 0 || (size_t)pid >= it->second.size()) return nullptr;
    return &it->second[(size_t)pid];
}

const std::vector<int>& Game::displayOwners() const {
    return m_fogDisplayOwners.empty() ? m_provinceCountryLookup : m_fogDisplayOwners;
}

// --- persistence -------------------------------------------------------------

// --- sight grants (gearbox:fog.write) ---------------------------------------

// A circle has to be big enough to contain something and small enough not to be
// the whole map: a radius of 0 would be a grant that does nothing and silently
// look like a bug, and one of 100000 would be "switch the fog off" written as
// if it were a watchpost.
static constexpr float kGrantMinRadius = 8.0f;
static constexpr float kGrantMaxRadius = 4096.0f;

bool Game::fogAddSight(int cid, float x, float y, float radius) {
    if (cid <= 0 || cid >= 65536) return false;
    if (!(radius == radius) || !(x == x) || !(y == y)) return false;   // NaN
    const float r = std::clamp(radius, kGrantMinRadius, kGrantMaxRadius);
    m_fogGrants[cid].circles.push_back({x, y, r});
    m_fogTurn = -1;                     // the fog is now stale; next read rebuilds
    return true;
}

bool Game::fogRevealProvince(int cid, int pid) {
    if (cid <= 0 || cid >= 65536 || pid <= 0) return false;
    auto& v = m_fogGrants[cid].provinces;
    if (std::find(v.begin(), v.end(), pid) != v.end()) return true;    // already granted
    v.push_back(pid);
    m_fogTurn = -1;
    return true;
}

int Game::fogClearGrants(int cid) {
    auto it = m_fogGrants.find(cid);
    if (it == m_fogGrants.end()) return 0;
    const int n = (int)(it->second.provinces.size() + it->second.circles.size());
    m_fogGrants.erase(it);
    if (n) m_fogTurn = -1;
    return n;
}

int Game::fogGrantCount(int cid) const {
    auto it = m_fogGrants.find(cid);
    if (it == m_fogGrants.end()) return 0;
    return (int)(it->second.provinces.size() + it->second.circles.size());
}

// Its own save entry rather than a field inside the memory blob: the memory
// format is versioned and shared with the multiplayer path, and widening it to
// carry grants would make every old save a decode failure for a feature that
// did not exist when it was written.
std::string Game::fogGrantsBlob() const {
    if (!m_fogOfWar || m_fogGrants.empty()) return {};
    std::vector<int> cids;
    for (const auto& [c, g] : m_fogGrants) cids.push_back(c);
    std::sort(cids.begin(), cids.end());        // same world, same bytes
    std::string out = "ODFG1";
    auto put32 = [&out](uint32_t v) {
        out.push_back((char)(v & 0xFF));        out.push_back((char)((v >> 8) & 0xFF));
        out.push_back((char)((v >> 16) & 0xFF)); out.push_back((char)((v >> 24) & 0xFF));
    };
    auto putf = [&put32](float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(u); };
    put32((uint32_t)cids.size());
    for (int c : cids) {
        const FogGrants& g = m_fogGrants.at(c);
        put32((uint32_t)c);
        put32((uint32_t)g.provinces.size());
        for (int p : g.provinces) put32((uint32_t)p);
        put32((uint32_t)g.circles.size());
        for (const auto& k : g.circles) { putf(k.x); putf(k.y); putf(k.r); }
    }
    return out;
}

void Game::loadFogGrants(const std::string& blob) {
    m_fogGrants.clear();
    if (!m_fogOfWar || blob.size() < 9 || blob.compare(0, 5, "ODFG1") != 0) return;
    size_t at = 5;
    auto get32 = [&](uint32_t& v) {
        if (at + 4 > blob.size()) return false;
        v = (uint8_t)blob[at] | ((uint32_t)(uint8_t)blob[at+1] << 8)
          | ((uint32_t)(uint8_t)blob[at+2] << 16) | ((uint32_t)(uint8_t)blob[at+3] << 24);
        at += 4; return true;
    };
    uint32_t n = 0;
    if (!get32(n) || n > 65536) return;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t cid = 0, np = 0, nc = 0;
        if (!get32(cid) || !get32(np) || np > 1u << 20) { m_fogGrants.clear(); return; }
        FogGrants g;
        g.provinces.reserve(np);
        for (uint32_t k = 0; k < np; ++k) {
            uint32_t p = 0;
            if (!get32(p)) { m_fogGrants.clear(); return; }
            g.provinces.push_back((int)p);
        }
        if (!get32(nc) || nc > 1u << 20) { m_fogGrants.clear(); return; }
        g.circles.reserve(nc);
        for (uint32_t k = 0; k < nc; ++k) {
            uint32_t a = 0, b = 0, c = 0;
            if (!get32(a) || !get32(b) || !get32(c)) { m_fogGrants.clear(); return; }
            float x, y, r;
            std::memcpy(&x, &a, 4); std::memcpy(&y, &b, 4); std::memcpy(&r, &c, 4);
            g.circles.push_back({x, y, r});
        }
        if (cid > 0 && cid < 65536) m_fogGrants[(int)cid] = std::move(g);
    }
    m_fogTurn = -1;
}

std::string Game::fogMemoryBlob() const {
    if (!m_fogOfWar) return {};
    std::vector<int> cids;
    for (const auto& [c, m] : m_fogMemory) cids.push_back(c);
    std::sort(cids.begin(), cids.end());          // same world, same bytes
    std::vector<odfog::CountryMemory> all;
    all.reserve(cids.size());
    for (int c : cids) {
        odfog::CountryMemory cm;
        cm.cid = c;
        cm.provinces = m_fogMemory.at(c);
        auto s = m_fogSeenShips.find(c);
        if (s != m_fogSeenShips.end()) cm.ships = s->second;
        all.push_back(std::move(cm));
    }
    return odfog::encode(all, m_fogStartOwners);
}

void Game::loadFogMemory(const std::string& blob) {
    if (!m_fogOfWar || blob.empty()) return;
    std::vector<odfog::CountryMemory> all;
    if (!odfog::decode(blob, m_fogStartOwners, all)) {
        // A damaged memory is not a damaged world: every country falls back to
        // the starting atlas and what it can see from here on.
        std::cout << "[FOG] fog/memory.bin is damaged; memories start again from the atlas" << std::endl;
        return;
    }
    for (auto& cm : all) {
        m_fogMemory[cm.cid] = std::move(cm.provinces);
        if (!cm.ships.empty()) m_fogSeenShips[cm.cid] = std::move(cm.ships);
    }
}

void Game::fogAfterLoad(const std::string& savePath) {
    if (!m_fogOfWar) return;
    loadFogMemory(SaveManager::readEntry(savePath, "fog/memory.bin"));
    loadFogGrants(SaveManager::readEntry(savePath, "fog/grants.bin"));
    // OD_FOG_DUMP=<path>: write the memory exactly as loaded, before this
    // turn's recompute adds to it. Comparing it with the save's own
    // fog/memory.bin proves the round trip is byte-exact.
    if (const char* dump = std::getenv("OD_FOG_DUMP"); dump && *dump) {
        if (FILE* f = std::fopen(dump, "wb")) {
            const std::string b = fogMemoryBlob();
            std::fwrite(b.data(), 1, b.size(), f);
            std::fclose(f);
        }
    }
    recomputeFog();
}

std::string Game::fogDebugLine() const {
    if (!m_fogOfWar) return {};
    auto it = m_fogSight.find(m_playerCountryId);
    const size_t vis = it != m_fogSight.end() ? it->second.provinces.count() : 0;
    const size_t ships = it != m_fogSight.end() ? it->second.ships.count() : 0;
    size_t seen = 0;
    if (auto m = m_fogMemory.find(m_playerCountryId); m != m_fogMemory.end())
        for (const auto& p : m->second) if (p.seenTurn != odfog::kNeverSeen) ++seen;
    return "Fog: " + std::to_string(vis) + " provinces visible, " + std::to_string(seen) +
           " ever seen, " + std::to_string(ships) + " ships in sight";
}

bool Game::fogKnowsCountry(int cid) const {
    if (!m_fogOfWar || !fogCountry(m_playerCountryId)) return true;
    return sharesIntel(m_playerCountryId, cid) || fogDossier(m_playerCountryId, cid);
}

// The province panel for a province out of the player's sight: what was there
// the last time anyone the player shares with looked, and how long ago.
void Game::drawFoggedProvinceInfo(int pid, int x, int y, int w) {
    (void)w;
    const odfog::ProvinceIntel* in = intel(m_playerCountryId, pid);
    const Color dim{150, 156, 176, 255};
    const Province* prov = m_provinces.getProvinceById(pid);
    if (prov && !prov->name.empty()) {
        DrawText(prov->name.c_str(), x, y, 18, WHITE);
        y += 26;
    }
    if (!in || in->seenTurn == odfog::kNeverSeen) {
        DrawText(T("Unexplored"), x, y, 20, Color{200, 190, 150, 255});
        DrawText(T("Nobody you share intelligence with has seen this province."),
                 x, y + 26, 12, dim);
        DrawText(T("Its owner is the one on the starting map."), x, y + 42, 12, dim);
        return;
    }
    const int ago = std::max(0, m_turnNumber - (int)in->seenTurn);
    DrawText(T("Out of sight"), x, y, 20, Color{200, 190, 150, 255});
    DrawText(ago == 0 ? T("Last seen this turn") : TextFormat(T("Last seen turn %d (%d turns ago)"),
                                                              (int)in->seenTurn, ago),
             x, y + 26, 13, dim);
    y += 54;
    DrawText(T("As last seen"), x, y, 16, WHITE);
    y += 24;
    DrawText(TextFormat(T("Garrison: %s"), formatTroops((long long)in->garrison).c_str()), x + 8, y, 15, LIGHTGRAY);
    DrawText(TextFormat(T("Population: %s"), formatPop((long long)in->population).c_str()), x + 8, y + 20, 15, LIGHTGRAY);
    DrawText(TextFormat(T("Industry level: %d"), (int)in->industry), x + 8, y + 40, 15, LIGHTGRAY);
    DrawText(TextFormat(T("Fortification: %d"), (int)in->fort), x + 8, y + 60, 15, LIGHTGRAY);
    if (in->port > 0)
        DrawText(TextFormat(T("Port level: %d"), (int)in->port), x + 8, y + 80, 15, LIGHTGRAY);
}

bool Game::fogPointVisible(float x, float y) const { return fogPointVisibleTo(m_playerCountryId, x, y); }

bool Game::fogPointVisibleTo(int cid, float x, float y) const {
    if (!m_fogOfWar || !fogCountry(cid)) return true;
    auto it = m_fogSight.find(cid);
    if (it == m_fogSight.end()) return false;
    const int ix = (int)x, iy = (int)y;
    if (const Province* p = m_provinces.getProvince(ix, iy))
        if (p->id > 0) return it->second.provinces.test((size_t)p->id);
    for (const odfog::SightCircle& c : it->second.sea)
        if (odfog::inCircle(c, x, y, m_landSea.getWidth())) return true;
    return false;
}

bool Game::fogMarkSeen(const TurnOrderMark& m) const { return fogMarkSeenBy(m_playerCountryId, m); }

bool Game::fogMarkSeenBy(int cid, const TurnOrderMark& m) const {
    if (!m_fogOfWar || !fogCountry(cid) || m.countryId == cid) return true;
    auto seesPid = [&](int pid) { return pid > 0 && provinceVisibleTo(cid, pid); };
    auto seesLonLat = [&](double lon, double lat) {
        int px = 0, py = 0;
        m_landSea.lonLatToPixel((float)lon, (float)lat, px, py);
        return fogPointVisibleTo(cid, (float)px, (float)py);
    };
    switch (m.kind) {
        case TurnOrderMark::Kind::ShipVoyage:
            return seesLonLat(m.fromLon, m.fromLat);
        case TurnOrderMark::Kind::NavalBombard:
            return seesLonLat(m.fromLon, m.fromLat) || seesPid(m.toProvince);
        default:
            return seesPid(m.fromProvince) || seesPid(m.toProvince);
    }
}

bool Game::fogAiReadsTruth(int observer, int other) const {
    if (!m_fogOfWar || observer == other) return true;
    static const bool truthForAi = OD_ENV("OD_FOG_AI_TRUTH") != nullptr;
    if (truthForAi) return true;
    return !fogCountry(observer) || sharesIntel(observer, other) || fogDossier(observer, other);
}

const odfog::Estimate* Game::fogEstimate(int observer, int other) const {
    if (fogAiReadsTruth(observer, other)) return nullptr;
    if (m_fogEstimateTurn != m_fogTurn) {
        m_fogEstimateCache.clear();
        m_fogEstimateTurn = m_fogTurn;
    }
    const uint64_t key = ((uint64_t)(uint32_t)observer << 32) | (uint32_t)other;
    auto hit = m_fogEstimateCache.find(key);
    if (hit != m_fogEstimateCache.end()) return &hit->second;
    static const std::vector<odfog::ProvinceIntel> kNoMemory;
    static const std::vector<odfog::SeenShip> kNoShips;
    auto mem = m_fogMemory.find(observer);
    auto sh = m_fogSeenShips.find(observer);
    const odfog::Estimate e = odfog::estimate(
        mem != m_fogMemory.end() ? mem->second : kNoMemory,
        sh != m_fogSeenShips.end() ? sh->second : kNoShips, other,
        (uint16_t)std::clamp(m_turnNumber, 0, 0xFFFE));
    return &m_fogEstimateCache.emplace(key, e).first->second;
}
