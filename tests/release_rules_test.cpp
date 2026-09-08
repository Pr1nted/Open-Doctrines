// Which ground a country may let go of.
//
// findReleasableRegions decides what the release panel offers, what the
// bankruptcy cascade sheds when a country can no longer govern itself, and
// eventually what the AI may choose. It is pure -- provinces in, regions out --
// so it can be checked exhaustively here without a map or a window, which is
// the whole reason it lives in a header rather than inside Game.
//
// The properties worth pinning are the ones whose failure would be quiet:
//
//   CONTIGUITY. A region that is allowed to jump a gap would hand a player two
//   unconnected enclaves as one "nation", and the map would show a country in
//   two pieces with somebody else's land between them.
//   NO OVERLAP. If two candidates could share a province, releasing one would
//   silently invalidate the other and the second release would move ground that
//   had already left.
//   A MAJORITY, NOT A PLURALITY. Provinces hold about nine and a half minority
//   groups each, so a plurality can be 15%; a rule that accepted one would make
//   almost every province on the map releasable by somebody.
//   DETERMINISM. The same save must offer the same regions in the same order.
//
// No window, no map, no game.

#include "ReleaseRules.h"

#include <cstdio>
#include <map>
#include <string>

static int g_checks = 0, g_failed = 0;

static void ok(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    ++g_failed;
    printf("  FAIL  %s\n", what.c_str());
}

static void section(const char* name) { printf("\n== %s ==\n", name); }

// A tiny world: provinces in a line, 1-2-3-4-5-6, unless stated otherwise.
static std::map<int, std::vector<int>> lineWorld(int n) {
    std::map<int, std::vector<int>> adj;
    for (int i = 1; i <= n; ++i) {
        if (i > 1) adj[i].push_back(i - 1);
        if (i < n) adj[i].push_back(i + 1);
    }
    return adj;
}

// Disaffected by default (alignment 20): most tests are about geography and
// size, and a content people would be filtered before the geometry ran.
static ReleaseProvince prov(int id, const char* who, float pct, long long pop = 1000,
                            float align = 20.0f) {
    ReleaseProvince p;
    p.id = id; p.topMinority = who; p.topMinorityPct = pct; p.population = pop;
    p.alignment = align;
    return p;
}

int main() {
    section("a region is found when a minority holds a majority run");
    {
        auto adj = lineWorld(6);
        std::vector<ReleaseProvince> owned = {
            prov(1, "Ruritanian", 80), prov(2, "Ruritanian", 70),
            prov(3, "Home", 90),       prov(4, "Home", 95),
            prov(5, "Home", 91),       prov(6, "Home", 88),
        };
        auto r = findReleasableRegions(owned, [&](int p) { return adj[p]; });
        ok(r.size() == 1, "exactly one releasable region");
        if (r.size() == 1) {
            ok(r[0].minority == "Ruritanian", "named for the people who live there");
            ok(r[0].provinces == std::vector<int>({1, 2}), "and it is provinces 1 and 2");
            ok(r[0].population == 2000, "with their population");
        }
    }

    section("a single province is a grievance, not a nation");
    {
        auto adj = lineWorld(4);
        std::vector<ReleaseProvince> owned = {
            prov(1, "Ruritanian", 80), prov(2, "Home", 90),
            prov(3, "Home", 90),       prov(4, "Home", 90),
        };
        auto r = findReleasableRegions(owned, [&](int p) { return adj[p]; });
        ok(r.empty(), "one majority province alone is not releasable");
    }

    section("a plurality is not enough");
    {
        auto adj = lineWorld(4);
        // 40% is the largest group in the province and still not a majority --
        // which is the ordinary case on these maps, not an edge one.
        std::vector<ReleaseProvince> owned = {
            prov(1, "Ruritanian", 40), prov(2, "Ruritanian", 45),
            prov(3, "Home", 90),       prov(4, "Home", 90),
        };
        auto r = findReleasableRegions(owned, [&](int p) { return adj[p]; });
        bool anyRuritanian = false;
        for (const auto& c : r) if (c.minority == "Ruritanian") anyRuritanian = true;
        ok(!anyRuritanian, "a plurality does not make a province theirs");
    }

    section("regions do not jump gaps");
    {
        // 1 and 2 are Ruritanian; 4 and 5 are Ruritanian; 3 is not, and sits
        // between them. That is two enclaves, and it must not be one nation.
        // Home is deliberately the LARGEST people here (four provinces at 3,000
        // against the Ruritanians' four at 1,000), or the titular rule would
        // correctly refuse to release the country's own majority and this would
        // be testing that instead of the gap.
        auto adj = lineWorld(10);
        std::vector<ReleaseProvince> owned = {
            prov(1, "Ruritanian", 80, 1000), prov(2, "Ruritanian", 80, 1000),
            prov(3, "Home", 90, 3000),
            prov(4, "Ruritanian", 80, 1000), prov(5, "Ruritanian", 80, 1000),
            prov(6, "Home", 90, 3000), prov(7, "Home", 90, 3000),
            prov(8, "Home", 90, 3000), prov(9, "Home", 90, 3000),
            prov(10, "Home", 90, 3000),
        };
        auto r = findReleasableRegions(owned, [&](int p) { return adj[p]; });
        std::vector<std::vector<int>> rur;
        for (const auto& c : r) if (c.minority == "Ruritanian") rur.push_back(c.provinces);
        ok(rur.size() == 2, "two separate enclaves, not one country in two pieces");
        bool disjoint = true;
        if (rur.size() == 2)
            for (int a : rur[0]) for (int b : rur[1]) if (a == b) disjoint = false;
        ok(disjoint, "and they share no ground");
        for (const auto& g : rur)
            ok(g.size() == 2, "each enclave is exactly the run that touches itself");
    }

    section("a region cannot reach through land we do not own");
    {
        // The line is 1-2-3-4, but we do not own 2. Ruritanians in 1 and in
        // 3-4 are therefore two groups, not one, even though the map connects
        // them -- somebody else's province is in the way.
        // Eight provinces so the "may not release itself out of existence" cap
        // is not what refuses this -- the point under test is the gap, and a
        // three-province country would have been refused for its size.
        auto adj = lineWorld(8);
        std::vector<ReleaseProvince> owned = {
            prov(1, "Ruritanian", 80),
            prov(3, "Ruritanian", 80), prov(4, "Ruritanian", 80),
            prov(5, "Home", 90), prov(6, "Home", 90),
            prov(7, "Home", 90), prov(8, "Home", 90),
        };
        auto r = findReleasableRegions(owned, [&](int p) { return adj[p]; });
        ok(r.size() == 1, "only the run we actually hold is a region");
        if (r.size() == 1)
            ok(r[0].provinces == std::vector<int>({3, 4}),
               "and it is the part on our side of the gap");
    }

    section("a country may not release itself out of existence");
    {
        auto adj = lineWorld(4);
        // All four provinces are Ruritanian: releasing them is releasing the
        // country, which is an exit rather than a strategy.
        std::vector<ReleaseProvince> owned = {
            prov(1, "Ruritanian", 80), prov(2, "Ruritanian", 80),
            prov(3, "Ruritanian", 80), prov(4, "Ruritanian", 80),
        };
        auto r = findReleasableRegions(owned, [&](int p) { return adj[p]; });
        ok(r.empty(), "the whole country is not a releasable region");
    }

    section("content peoples are not offered");
    {
        auto adj = lineWorld(6);
        // A perfectly ordinary majority region whose people are reconciled.
        // This is the rule that stops a country shedding its own core, and it
        // is why there is no titular-people rule; see ReleaseRules.h.
        std::vector<ReleaseProvince> owned = {
            prov(1, "Ruritanian", 80, 1000, 90.0f), prov(2, "Ruritanian", 80, 1000, 90.0f),
            prov(3, "Home", 90, 1000, 95.0f), prov(4, "Home", 90, 1000, 95.0f),
            prov(5, "Home", 90, 1000, 95.0f), prov(6, "Home", 90, 1000, 95.0f),
        };
        ok(findReleasableRegions(owned, [&](int p) { return adj[p]; }).empty(),
           "nobody who is content with your rule is offered");
    }
    {
        auto adj = lineWorld(6);
        // The same world with the Ruritanians repressed instead: now they are.
        std::vector<ReleaseProvince> owned = {
            prov(1, "Ruritanian", 80, 1000, 15.0f), prov(2, "Ruritanian", 80, 1000, 15.0f),
            prov(3, "Home", 90, 1000, 95.0f), prov(4, "Home", 90, 1000, 95.0f),
            prov(5, "Home", 90, 1000, 95.0f), prov(6, "Home", 90, 1000, 95.0f),
        };
        auto r = findReleasableRegions(owned, [&](int p) { return adj[p]; });
        ok(r.size() == 1 && r[0].minority == "Ruritanian",
           "and the disaffected region is");
    }

    section("the size cap also protects the core");
    {
        auto adj = lineWorld(6);
        // Every province is the same people, so the only "region" is the whole
        // country -- more than half of itself, and refused for that reason.
        // There is deliberately no titular-people rule; see ReleaseRules.h.
        std::vector<ReleaseProvince> owned = {
            prov(1, "Home", 90), prov(2, "Home", 90), prov(3, "Home", 90),
            prov(4, "Home", 90), prov(5, "Home", 90), prov(6, "Home", 90),
        };
        ok(findReleasableRegions(owned, [&](int p) { return adj[p]; }).empty(),
           "a homogeneous country has no releasable region");
    }
    {
        auto adj = lineWorld(6);
        // 4-5-6 is four provinces of six and exceeds the half cap; 1-2 is two
        // and does not. So the smaller people are the region and the larger are
        // the country -- by size, not by anybody's name.
        std::vector<ReleaseProvince> owned = {
            prov(1, "Ruritanian", 80, 1000), prov(2, "Ruritanian", 80, 1000),
            prov(3, "Home", 90, 1000),
            prov(4, "Home", 90, 1000), prov(5, "Home", 90, 1000), prov(6, "Home", 90, 1000),
        };
        auto r = findReleasableRegions(owned, [&](int p) { return adj[p]; });
        ok(r.size() == 1 && r[0].minority == "Ruritanian",
           "the block over half the country is refused, the smaller one offered");
    }

    section("candidates never overlap");
    {
        auto adj = lineWorld(8);
        std::vector<ReleaseProvince> owned = {
            prov(1, "Ruritanian", 80), prov(2, "Ruritanian", 80),
            prov(3, "Home", 90),
            prov(4, "Vespuccian", 70), prov(5, "Vespuccian", 70),
            prov(6, "Home", 90), prov(7, "Home", 90), prov(8, "Home", 90),
        };
        auto r = findReleasableRegions(owned, [&](int p) { return adj[p]; });
        ok(r.size() >= 2, "at least the two distinct peoples are offered");
        std::vector<int> all;
        for (const auto& c : r) for (int p : c.provinces) all.push_back(p);
        std::sort(all.begin(), all.end());
        ok(std::adjacent_find(all.begin(), all.end()) == all.end(),
           "no province appears in two candidates");
    }

    section("deterministic, and ordered largest first");
    {
        // Three releasable peoples of clearly different sizes, so the ordering
        // is what is under test rather than the size cap.
        auto adj = lineWorld(8);
        std::vector<ReleaseProvince> owned = {
            prov(1, "Ruritanian", 80, 500), prov(2, "Ruritanian", 80, 500),
            prov(3, "Home", 90, 100),
            prov(4, "Vespuccian", 70, 4000), prov(5, "Vespuccian", 70, 4000),
            prov(6, "Home", 90, 100), prov(7, "Home", 90, 100), prov(8, "Home", 90, 100),
        };
        auto r = findReleasableRegions(owned, [&](int p) { return adj[p]; });
        ok(!r.empty() && r[0].minority == "Vespuccian",
           "the larger population is offered first");

        // Same world, provinces presented in a different order: a save that is
        // loaded rather than played must offer the same answer.
        std::vector<ReleaseProvince> shuffled = {
            owned[4], owned[0], owned[7], owned[3],
            owned[1], owned[6], owned[2], owned[5],
        };
        auto r2 = findReleasableRegions(shuffled, [&](int p) { return adj[p]; });
        bool same = r.size() == r2.size();
        for (size_t i = 0; same && i < r.size(); ++i)
            same = r[i].minority == r2[i].minority && r[i].provinces == r2[i].provinces;
        ok(same, "province order does not change the answer");
    }

    section("nothing to release is not an error");
    {
        auto adj = lineWorld(3);
        std::vector<ReleaseProvince> owned = {
            prov(1, "Home", 95), prov(2, "Home", 95), prov(3, "Home", 95),
        };
        ok(findReleasableRegions(owned, [&](int p) { return adj[p]; }).empty(),
           "a homogeneous country offers nothing");
        std::vector<ReleaseProvince> none;
        ok(findReleasableRegions(none, [&](int p) { return adj[p]; }).empty(),
           "and neither does a country with no provinces");
    }

    section("a chosen subset is one piece, or it is not a nation");
    {
        // Game::releaseSubsetOk, replicated -- the game's copy needs a whole
        // province map to reach. What it guards is a settlement: the player is
        // handed the largest run a people holds and may trim it, and trimming
        // the MIDDLE out of a line leaves two countries with somebody else's
        // ground between them. That reads as one release and is two.
        auto adj = lineWorld(6);           // 1-2-3-4-5-6 in a row
        auto contiguous = [&](std::vector<int> provs) {
            if (provs.size() < RELEASE_MIN_PROVINCES) return false;
            std::vector<int> seen{provs.front()}, stack{provs.front()};
            while (!stack.empty()) {
                const int cur = stack.back(); stack.pop_back();
                for (int pid : provs) {
                    if (std::find(seen.begin(), seen.end(), pid) != seen.end()) continue;
                    const auto& n = adj[cur];
                    if (std::find(n.begin(), n.end(), pid) == n.end()) continue;
                    seen.push_back(pid); stack.push_back(pid);
                }
            }
            return seen.size() == provs.size();
        };

        ok(contiguous({1, 2, 3}), "a run of three is a nation");
        ok(contiguous({4, 5}),    "and so is the smallest allowed run");
        ok(!contiguous({1}),      "one province is not enough");
        ok(!contiguous({1, 3}),   "two provinces with a gap are two countries");
        ok(!contiguous({1, 2, 5, 6}),
           "and so are two healthy halves with the middle kept");
        ok(contiguous({2, 3, 4, 5}), "trimming the ENDS is fine");
        ok(!contiguous({1, 2, 4, 5}), "trimming the middle is not");

        // THE ORDER OF THE LIST MUST NOT DECIDE. The walk starts at the first
        // entry, so a broken set that happens to be listed with its connected
        // half first would pass a lazier check.
        ok(!contiguous({5, 6, 1, 2}),
           "a split set is refused whichever end is listed first");
        ok(contiguous({3, 1, 2}), "and a whole one is accepted unsorted");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
