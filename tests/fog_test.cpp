// Fog of war: the visibility and memory rules in src/Fog.h.
//
// Pure arithmetic over small hand-built worlds -- no window, map or save. The
// game half (building the world from live state, the accessors, saving) is
// src/Game_Fog.cpp. Build target: FogTest. Non-zero exit means a case failed.

#include "Fog.h"

#include <cstdio>
#include <string>

namespace {

int g_checks = 0, g_failed = 0;

void ok(bool cond, const std::string& what, const std::string& detail = {}) {
    ++g_checks;
    printf("  %-66s %s", what.c_str(), cond ? "ok" : "FAILED");
    if (!cond && !detail.empty()) printf("  --  %s", detail.c_str());
    printf("\n");
    if (!cond) ++g_failed;
}

void section(const char* name) { printf("\n== %s ==\n", name); }

// A row of provinces 1..n along y = 500, `gap` px apart, each adjacent to the
// next. Province 0 is the "no province" slot every real array has.
odfog::World chain(int n, float gap = 200.0f, int mapW = 4000) {
    odfog::World w;
    w.mapW = mapW;
    w.mapH = 1000;
    w.owner.assign((size_t)n + 1, 0);
    w.neighbors.assign((size_t)n + 1, {});
    w.cx.assign((size_t)n + 1, 0.0f);
    w.cy.assign((size_t)n + 1, 0.0f);
    w.coastal.assign((size_t)n + 1, 0);
    for (int p = 1; p <= n; ++p) {
        w.cx[(size_t)p] = 100.0f + gap * (float)(p - 1);
        w.cy[(size_t)p] = 500.0f;
        if (p > 1) w.neighbors[(size_t)p].push_back(p - 1);
        if (p < n) w.neighbors[(size_t)p].push_back(p + 1);
    }
    return w;
}

odfog::Sight sightOf(const odfog::World& w, int cid, odfog::SightParams p = {}) {
    odfog::Index idx;
    idx.build(w);
    return odfog::ownSight(w, idx, cid, p);
}

void testRings() {
    section("own land and neighbour rings");
    auto w = chain(6);
    w.owner[1] = 7;
    odfog::SightParams p;
    p.coastSight = 0;
    p.landRings = 0;
    auto s = sightOf(w, 7, p);
    ok(s.provinces.test(1) && !s.provinces.test(2), "ring 0: only your own province");
    p.landRings = 1;
    s = sightOf(w, 7, p);
    ok(s.provinces.test(2) && !s.provinces.test(3), "ring 1: your neighbour, not theirs");
    p.landRings = 3;
    s = sightOf(w, 7, p);
    ok(s.provinces.test(4) && !s.provinces.test(5), "ring 3: three steps out, no further");
    ok(s.provinces.count() == 4, "ring 3 sees exactly four provinces",
       std::to_string(s.provinces.count()));
    ok(sightOf(w, 0, p).provinces.count() == 0, "country 0 sees nothing");
}

void testWrap() {
    section("the map wraps east-west");
    odfog::SightCircle c{3990.0f, 500.0f, 30.0f};
    ok(odfog::inCircle(c, 10.0f, 500.0f, 4000), "a circle near the east edge reaches the west edge");
    ok(!odfog::inCircle(c, 60.0f, 500.0f, 4000), "but no further than its radius");
    ok(!odfog::inCircle(c, 10.0f, 500.0f, 0), "an unknown map width does not wrap");

    // The index must find it too, across the column boundary.
    auto w = chain(2, 3880.0f);          // provinces at x=100 and x=3980
    w.owner[2] = 3;
    w.coastal[2] = 1;
    odfog::SightParams p;
    p.landRings = 0;
    p.coastSight = 150.0f;               // 3980 -> 4000 -> 100 is 120 px the short way
    auto s = sightOf(w, 3, p);
    ok(s.provinces.test(1), "coast watch sees across the antimeridian");

    // The grid's last column must end AT the east edge: with 128 px cells on a
    // 4000 px map it used to end at 4096, and a circle just inside the edge
    // never looked in column 0.
    auto e = chain(2, 3997.0f);          // provinces at x=100 and x=4097 -> moved below
    e.cx[1] = 2.0f;
    e.cx[2] = 3999.0f;
    e.owner[2] = 4;
    e.coastal[2] = 1;
    p.coastSight = 5.0f;
    ok(sightOf(e, 4, p).provinces.test(1), "a 5 px circle at x=3999 reaches x=2");
}

void testCirclesAndShips() {
    section("coast watch and radar");
    auto w = chain(5, 200.0f);           // x = 100, 300, 500, 700, 900
    w.owner[1] = 1;                      // a coastal country at the west end
    w.coastal[1] = 1;
    w.owner[5] = 2;
    odfog::SightParams p;
    p.landRings = 0;
    p.coastSight = 250.0f;
    auto s = sightOf(w, 1, p);
    ok(s.provinces.test(2) && !s.provinces.test(3), "coast watch reaches 250 px and stops");
    ok(s.sea.size() == 1, "one sight circle per own coastal province");

    p.coastSight = 0.0f;
    w.ships.push_back({1, 700.0f, 520.0f, 120.0f, 2});   // ours, near province 4
    w.ships.push_back({2, 760.0f, 500.0f, 0.0f, 1});     // theirs, inside our radar
    w.ships.push_back({2, 3000.0f, 500.0f, 0.0f, 1});    // theirs, far away
    s = sightOf(w, 1, p);
    ok(s.provinces.test(4), "radar spots coastal land");
    ok(s.ships.test(0), "your own ship is always visible");
    ok(s.ships.test(1), "an enemy ship inside your radar is visible");
    ok(!s.ships.test(2), "an enemy ship far away is not");

    p.radarScale = 0.0f;
    s = sightOf(w, 1, p);
    ok(s.ships.test(0) && !s.ships.test(1), "no radar: you see your fleet and nothing else");

    auto t = sightOf(w, 2, p);
    ok(t.ships.test(1) && t.ships.test(2) && !t.ships.test(0),
       "the other side sees its own two ships, not ours");
}

void testSharing() {
    section("sharing is one hop");
    auto w = chain(9);
    w.owner[1] = 1;   // A
    w.owner[5] = 2;   // B
    w.owner[9] = 3;   // C
    odfog::SightParams p;
    p.coastSight = 0;
    p.landRings = 1;
    const auto a = sightOf(w, 1, p), b = sightOf(w, 2, p), c = sightOf(w, 3, p);

    // A is allied with B; B is allied with C. A gets B's own sight, not C's.
    odfog::Sight aShared = a;
    odfog::shareInto(aShared, b);
    ok(aShared.provinces.test(5) && aShared.provinces.test(4) && aShared.provinces.test(6),
       "A sees what its ally B sees");
    ok(!aShared.provinces.test(9) && !aShared.provinces.test(8),
       "A does not see what B's ally C sees");

    // If B's sight were already shared with C, passing THAT would leak C to A:
    // callers must pass own sights. This is the rule the test pins.
    odfog::Sight bShared = b;
    odfog::shareInto(bShared, c);
    odfog::Sight leak = a;
    odfog::shareInto(leak, bShared);
    ok(leak.provinces.test(9), "(passing a shared sight would leak -- callers pass own sights)");
}

void testMemory() {
    section("memory");
    auto w = chain(4);
    std::vector<int> start = {0, 1, 1, 2, 2};
    auto mem = odfog::seedMemory(start);
    ok(mem[3].owner == 2 && mem[3].seenTurn == odfog::kNeverSeen,
       "unseen provinces keep the starting map's owner");

    w.owner = {0, 1, 1, 1, 2};   // province 3 has changed hands since the start
    std::vector<odfog::ProvinceTruth> truth(5);
    truth[3] = {5000, 120000, 4, 2, 1};
    odfog::Bits vis;
    vis.set(3);
    odfog::remember(mem, vis, w, truth, 12);
    ok(mem[3].owner == 1 && mem[3].seenTurn == 12 && mem[3].garrison == 5000 &&
       mem[3].population == 120000 && mem[3].industry == 4 && mem[3].fort == 2 && mem[3].port == 1,
       "a sighting records the truth and the turn");
    ok(mem[4].owner == 2 && mem[4].seenTurn == odfog::kNeverSeen,
       "provinces not seen are left alone");

    w.owner[3] = 2;
    odfog::remember(mem, odfog::Bits{}, w, truth, 13);
    ok(mem[3].owner == 1 && mem[3].seenTurn == 12, "out of sight, the memory goes stale, not true");
}

void testCodec() {
    section("persistence");
    std::vector<int> start = {0, 1, 1, 2, 2, 3};
    std::vector<odfog::CountryMemory> all(2);
    all[0].cid = 1;
    all[0].provinces = odfog::seedMemory(start);
    all[1].cid = 2;
    all[1].provinces = odfog::seedMemory(start);

    const std::string fresh = odfog::encode(all, start);
    ok(fresh.size() <= 4 + 1 + 4 + 2 * 10, "a fresh world encodes to a few bytes",
       std::to_string(fresh.size()));

    all[0].provinces[4] = {1, 30, 900, 45000, 3, 1, 2, 0};
    all[1].provinces[2].owner = 2;     // differs from baseline without a sighting
    all[1].ships.push_back({1, 123.5f, 456.25f, 3, 29});
    const std::string blob = odfog::encode(all, start);

    std::vector<odfog::CountryMemory> back;
    ok(odfog::decode(blob, start, back), "the blob decodes");
    ok(back.size() == 2 && back[0].cid == 1 && back[1].cid == 2, "both countries come back");
    if (back.size() != 2) return;   // the rest would index an empty result
    const auto& p4 = back[0].provinces[4];
    ok(p4.owner == 1 && p4.seenTurn == 30 && p4.garrison == 900 && p4.population == 45000 &&
       p4.industry == 3 && p4.fort == 1 && p4.port == 2,
       "a sighting round-trips exactly");
    ok(back[0].provinces[3].owner == 2 && back[0].provinces[3].seenTurn == odfog::kNeverSeen,
       "unwritten provinces come back as the baseline");
    ok(back[1].provinces[2].owner == 2, "a changed owner with no sighting round-trips");
    ok(back[1].ships.size() == 1 && back[1].ships[0].x == 123.5f && back[1].ships[0].turn == 29 &&
       back[1].ships[0].type == 3,
       "a ship sighting round-trips");
    ok(odfog::encode(back, start) == blob, "re-encoding is byte-identical");

    std::vector<odfog::CountryMemory> junk;
    ok(!odfog::decode("", start, junk) && junk.empty(), "an empty blob is refused");
    ok(!odfog::decode("NOPE" + blob.substr(4), start, junk), "wrong magic is refused");
    ok(!odfog::decode(blob.substr(0, blob.size() - 3), start, junk), "a truncated blob is refused");
    ok(!odfog::decode(blob + "x", start, junk), "trailing bytes are refused");
    std::string huge = blob;
    huge[5] = huge[6] = huge[7] = huge[8] = (char)0xFF;    // 4 billion countries
    ok(!odfog::decode(huge, start, junk) && junk.empty(), "an impossible count is refused, not allocated");
}

void testEstimate() {
    section("estimate (the AI's view of a stranger)");
    using odfog::ProvinceIntel;
    std::vector<ProvinceIntel> mem(7);
    // 1-3: the observer's own, seen this turn, 100 men each.
    for (int p = 1; p <= 3; ++p) {
        mem[(size_t)p].owner = 1; mem[(size_t)p].seenTurn = 10;
        mem[(size_t)p].garrison = 100; mem[(size_t)p].population = 1000; mem[(size_t)p].industry = 2;
    }
    // 4: theirs, seen four turns ago with 500 men.  5-6: theirs, never seen.
    mem[4].owner = 2; mem[4].seenTurn = 6; mem[4].garrison = 500; mem[4].population = 3000;
    mem[4].industry = 4; mem[4].port = 2;
    mem[5].owner = 2; mem[6].owner = 2;
    std::vector<odfog::SeenShip> ships = {{2, 0, 0, odfog::shipTypeCode("destroyer"), 9},
                                          {3, 0, 0, odfog::shipTypeCode("carrier"), 9}};
    const odfog::Estimate e = odfog::estimate(mem, ships, 2, 10);
    ok(e.provinces == 3, "counts the provinces it believes are theirs");
    ok(e.neverSeen == 2, "two of them stand on the prior");
    ok(e.army == 500 + 2 * 100, "never seen reads as the median seen garrison, not zero",
       std::to_string(e.army));
    ok(e.army > 500, "an unseen province is never assumed empty");
    ok(e.meanStaleness == 4.0f, "staleness is over the seen provinces only");
    ok(e.maxPort == 2, "a port is what was seen");
    ok(e.destroyers == 1 && e.carriers == 0, "only their own ships are counted");
    const odfog::Estimate none = odfog::estimate(mem, ships, 9, 10);
    ok(none.provinces == 0 && none.army == 0, "a country with no known land estimates to nothing");
}

}  // namespace

int main() {
    printf("FogTest\n");
    testRings();
    testWrap();
    testCirclesAndShips();
    testSharing();
    testMemory();
    testCodec();
    testEstimate();
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
