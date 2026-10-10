#include "Fog.h"

#include <cstring>

namespace odfog {

size_t Bits::count() const {
    size_t n = 0;
    for (uint64_t x : w) {
        while (x) { x &= x - 1; ++n; }
    }
    return n;
}

bool inCircle(const SightCircle& c, float x, float y, int mapW) {
    float dx = std::fabs(x - c.x);
    if (mapW > 0 && dx > mapW * 0.5f) dx = (float)mapW - dx;   // the short way round
    const float dy = y - c.y;
    return dx * dx + dy * dy <= c.r * c.r;
}

void Index::build(const World& w, float cell) {
    m_w = &w;
    if (cell <= 1.0f) cell = 128.0f;
    const float mw = (float)std::max(1, w.mapW), mh = (float)std::max(1, w.mapH);
    m_cols = std::max(1, (int)std::ceil(mw / cell));
    m_rows = std::max(1, (int)std::ceil(mh / cell));
    m_cellW = mw / (float)m_cols;
    m_cellH = mh / (float)m_rows;
    m_prov.assign((size_t)m_cols * m_rows, {});
    m_ship.assign((size_t)m_cols * m_rows, {});
    auto cellOf = [&](float x, float y) -> size_t {
        int gx = (int)std::floor(x / m_cellW), gy = (int)std::floor(y / m_cellH);
        gx %= m_cols; if (gx < 0) gx += m_cols;
        gy = std::clamp(gy, 0, m_rows - 1);
        return (size_t)gy * m_cols + gx;
    };
    for (size_t pid = 1; pid < w.owner.size() && pid < w.cx.size() && pid < w.cy.size(); ++pid) {
        if (w.cx[pid] == 0.0f && w.cy[pid] == 0.0f) continue;   // no such province
        m_prov[cellOf(w.cx[pid], w.cy[pid])].push_back((int)pid);
    }
    for (size_t i = 0; i < w.ships.size(); ++i)
        m_ship[cellOf(w.ships[i].x, w.ships[i].y)].push_back((int)i);
}

Sight ownSight(const World& w, const Index& idx, int cid, const SightParams& p) {
    Sight s;
    if (cid <= 0) return s;

    // 1. Own land, then rings across the adjacency graph. Breadth-first with a
    //    depth per frontier, so a province reached at ring 1 is not walked again
    //    at ring 2.
    std::vector<int> frontier;
    for (size_t pid = 1; pid < w.owner.size(); ++pid)
        if (w.owner[pid] == cid) { s.provinces.set(pid); frontier.push_back((int)pid); }
    for (int ring = 0; ring < p.landRings && !frontier.empty(); ++ring) {
        std::vector<int> next;
        for (int pid : frontier) {
            if ((size_t)pid >= w.neighbors.size()) continue;
            for (int nb : w.neighbors[(size_t)pid]) {
                if (nb <= 0 || s.provinces.test((size_t)nb)) continue;
                s.provinces.set((size_t)nb);
                next.push_back(nb);
            }
        }
        frontier.swap(next);
    }

    // 2. Sight circles: coast watch from own coastal provinces, radar from own
    //    ships. A province whose centre falls inside one is visible -- radar
    //    spots coastal land as well as sea.
    for (size_t pid = 1; pid < w.owner.size(); ++pid) {
        if (w.owner[pid] != cid) continue;
        if (pid >= w.coastal.size() || !w.coastal[pid]) continue;
        if (pid >= w.cx.size() || p.coastSight <= 0.0f) continue;
        s.sea.push_back({w.cx[pid], w.cy[pid], p.coastSight});
    }
    for (size_t i = 0; i < w.ships.size(); ++i) {
        const auto& sh = w.ships[i];
        if (sh.owner != cid) continue;
        s.ships.set(i);                                   // your own fleet, always
        const float r = sh.radar * p.radarScale;
        if (r > 0.0f) s.sea.push_back({sh.x, sh.y, r});
    }
    for (const auto& c : s.sea) {
        idx.provincesIn(c, [&](int pid) { s.provinces.set((size_t)pid); });
        idx.shipsIn(c, [&](int si) { s.ships.set((size_t)si); });
    }
    return s;
}

void shareInto(Sight& into, const Sight& partnerOwn) {
    into.provinces.orWith(partnerOwn.provinces);
    into.ships.orWith(partnerOwn.ships);
    into.sea.insert(into.sea.end(), partnerOwn.sea.begin(), partnerOwn.sea.end());
}

std::vector<ProvinceIntel> seedMemory(const std::vector<int>& startOwners) {
    std::vector<ProvinceIntel> m(startOwners.size());
    for (size_t pid = 0; pid < startOwners.size(); ++pid)
        m[pid].owner = (uint16_t)std::clamp(startOwners[pid], 0, 0xFFFF);
    return m;
}

void remember(std::vector<ProvinceIntel>& memory, const Bits& visible, const World& w,
              const std::vector<ProvinceTruth>& truth, uint16_t turn) {
    if (memory.size() < w.owner.size()) memory.resize(w.owner.size());
    for (size_t pid = 1; pid < w.owner.size(); ++pid) {
        if (!visible.test(pid)) continue;
        ProvinceIntel& m = memory[pid];
        m.owner = (uint16_t)std::clamp(w.owner[pid], 0, 0xFFFF);
        m.seenTurn = turn;
        if (pid < truth.size()) {
            const ProvinceTruth& t = truth[pid];
            m.garrison = t.garrison;
            m.population = t.population;
            m.industry = t.industry;
            m.fort = t.fort;
            m.port = t.port;
        }
    }
}

uint8_t shipTypeCode(const std::string& type) {
    if (type == "boat") return 1;
    if (type == "destroyer") return 2;
    if (type == "carrier") return 3;
    return 0;
}

// --- persistence ---------------------------------------------------------
//
//   "FOGM"  u8 version(1)  u32 countryCount
//   per country:  u16 cid  u32 provinceEntries  u32 shipEntries
//     province entry: u16 pid u16 owner u16 seenTurn u32 garrison u32 population
//                     u8 industry u8 fort u8 port u8 flags            (18 bytes)
//     ship entry:     i32 owner f32 x f32 y u8 type u16 turn          (15 bytes)
//
// Little-endian, written byte by byte so the format does not depend on the
// host or on struct padding.

namespace {

constexpr uint8_t kVersion = 1;
constexpr uint64_t kProvinceEntry = 2 + 2 + 2 + 4 + 4 + 4;   // 18
constexpr uint64_t kShipEntry = 4 + 4 + 4 + 1 + 2;            // 15

struct Writer {
    std::string s;
    void u8(uint8_t v) { s.push_back((char)v); }
    void u16(uint16_t v) { u8(v & 0xFF); u8(v >> 8); }
    void u32(uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
    void f32(float f) { uint32_t v; std::memcpy(&v, &f, 4); u32(v); }
};

struct Reader {
    const std::string& s;
    size_t at = 0;
    bool ok = true;
    explicit Reader(const std::string& b) : s(b) {}
    uint8_t u8() {
        if (at >= s.size()) { ok = false; return 0; }
        return (uint8_t)s[at++];
    }
    uint16_t u16() { uint16_t lo = u8(); return (uint16_t)(lo | (uint16_t)u8() << 8); }
    uint32_t u32() { uint32_t lo = u16(); return lo | (uint32_t)u16() << 16; }
    float f32() { uint32_t v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
    size_t left() const { return at <= s.size() ? s.size() - at : 0; }
};

bool differsFromBaseline(const ProvinceIntel& m, int startOwner) {
    return m.seenTurn != kNeverSeen || m.owner != (uint16_t)std::clamp(startOwner, 0, 0xFFFF);
}

}  // namespace

std::string encode(const std::vector<CountryMemory>& all, const std::vector<int>& startOwners) {
    Writer w;
    w.s = "FOGM";
    w.u8(kVersion);
    w.u32((uint32_t)all.size());
    for (const CountryMemory& c : all) {
        w.u16((uint16_t)std::clamp(c.cid, 0, 0xFFFF));
        std::vector<uint16_t> pids;
        for (size_t pid = 0; pid < c.provinces.size() && pid <= 0xFFFF; ++pid) {
            const int base = pid < startOwners.size() ? startOwners[pid] : 0;
            if (differsFromBaseline(c.provinces[pid], base)) pids.push_back((uint16_t)pid);
        }
        w.u32((uint32_t)pids.size());
        w.u32((uint32_t)c.ships.size());
        for (uint16_t pid : pids) {
            const ProvinceIntel& m = c.provinces[pid];
            w.u16(pid); w.u16(m.owner); w.u16(m.seenTurn);
            w.u32(m.garrison); w.u32(m.population);
            w.u8(m.industry); w.u8(m.fort); w.u8(m.port); w.u8(m.flags);
        }
        for (const SeenShip& sh : c.ships) {
            w.u32((uint32_t)sh.owner); w.f32(sh.x); w.f32(sh.y);
            w.u8(sh.type); w.u16(sh.turn);
        }
    }
    return w.s;
}

bool decode(const std::string& blob, const std::vector<int>& startOwners,
            std::vector<CountryMemory>& out) {
    out.clear();
    if (blob.size() < 9 || blob.compare(0, 4, "FOGM") != 0) return false;
    Reader r(blob);
    r.at = 4;
    if (r.u8() != kVersion) return false;
    const uint32_t countries = r.u32();
    // Every country costs at least 10 bytes; a count the blob cannot hold is
    // corruption, not a reason to reserve gigabytes.
    if (!r.ok || countries > r.left() / 10 + 1) return false;
    std::vector<CountryMemory> got;
    got.reserve(countries);
    for (uint32_t i = 0; i < countries; ++i) {
        CountryMemory c;
        c.cid = r.u16();
        const uint32_t np = r.u32(), ns = r.u32();
        if (!r.ok || (uint64_t)np * kProvinceEntry + (uint64_t)ns * kShipEntry > r.left()) return false;
        c.provinces = seedMemory(startOwners);
        for (uint32_t k = 0; k < np; ++k) {
            const uint16_t pid = r.u16();
            ProvinceIntel m;
            m.owner = r.u16(); m.seenTurn = r.u16();
            m.garrison = r.u32(); m.population = r.u32();
            m.industry = r.u8(); m.fort = r.u8(); m.port = r.u8(); m.flags = r.u8();
            if (pid >= c.provinces.size()) c.provinces.resize((size_t)pid + 1);
            c.provinces[pid] = m;
        }
        c.ships.resize(ns);
        for (uint32_t k = 0; k < ns; ++k) {
            SeenShip& sh = c.ships[k];
            sh.owner = (int)r.u32(); sh.x = r.f32(); sh.y = r.f32();
            sh.type = r.u8(); sh.turn = r.u16();
        }
        if (!r.ok) return false;
        got.push_back(std::move(c));
    }
    if (!r.ok || r.at != blob.size()) return false;
    out = std::move(got);
    return true;
}

namespace {
template <class T>
T median(std::vector<T> v) {
    if (v.empty()) return T{};
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + (long)mid, v.end());
    return v[mid];
}
}  // namespace

Estimate estimate(const std::vector<ProvinceIntel>& memory, const std::vector<SeenShip>& ships,
                  int other, uint16_t now) {
    Estimate e;
    // The priors: what a seen province holds, over everything this observer has
    // seen. Its own provinces are seen every turn and so dominate, which is the
    // right bias -- "a place I know nothing about is probably like places I know".
    std::vector<uint32_t> garrisons, pops;
    std::vector<uint8_t> industry, forts;
    for (size_t pid = 1; pid < memory.size(); ++pid) {
        const ProvinceIntel& m = memory[pid];
        if (m.seenTurn == kNeverSeen) continue;
        garrisons.push_back(m.garrison);
        pops.push_back(m.population);
        industry.push_back(m.industry);
        forts.push_back(m.fort);
    }
    const uint32_t gPrior = median(garrisons), pPrior = median(pops);
    const uint8_t iPrior = median(industry), fPrior = median(forts);

    double stale = 0;
    int seen = 0;
    for (size_t pid = 1; pid < memory.size(); ++pid) {
        const ProvinceIntel& m = memory[pid];
        if ((int)m.owner != other) continue;
        ++e.provinces;
        if (m.seenTurn == kNeverSeen) {
            ++e.neverSeen;
            e.army += gPrior;
            e.population += pPrior;
            e.industrySum += iPrior;
            e.fortSum += fPrior;
            continue;
        }
        ++seen;
        stale += (double)std::max(0, (int)now - (int)m.seenTurn);
        e.army += m.garrison;
        e.population += m.population;
        e.industrySum += m.industry;
        e.fortSum += m.fort;
        e.maxPort = std::max(e.maxPort, (int)m.port);
    }
    e.meanStaleness = seen > 0 ? (float)(stale / seen) : 0.0f;
    for (const SeenShip& s : ships) {
        if (s.owner != other) continue;
        if (s.type == shipTypeCode("boat")) ++e.boats;
        else if (s.type == shipTypeCode("destroyer")) ++e.destroyers;
        else if (s.type == shipTypeCode("carrier")) ++e.carriers;
    }
    return e;
}

}  // namespace odfog
