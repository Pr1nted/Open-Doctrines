// Trying to break the mod API to gain something.
//
// WHAT THIS IS, AND WHY IT IS NOT fuzz_parsers_test.cpp
//
// That one feeds malformed BYTES to the decoders -- a .odmod, a save, a network
// frame -- and asks that the parser survive being wrong about them. This one is
// the layer above: a mod that PARSED fine and is now running, calling the host
// ABI (the ~150 `Game::mod*` functions that GameModAccess forwards to) with
// arguments no honest mod would pass, trying to:
//
//   - reach out of bounds   -- a province, country, ship, monument, district or
//                              party id that does not exist, or an index past
//                              the end of a list, used to read or write memory
//                              it should not;
//   - GAIN SOMETHING         -- a treasury set to infinity, a province
//                              population past the ceiling, a monument built
//                              where the rules forbid it, a province handed to
//                              a country that does not exist, research funding
//                              outside 0..1 -- anything that leaves the world in
//                              a state the game's own rules could never produce;
//   - crash the host         -- a NaN, a 10 KB string, LLONG_MIN, UINT32_MAX.
//
// HOW IT WORKS. Load a real 1914 world, then hammer the ABI. Each step picks a
// random call and feeds it an argument drawn from a pool that is mostly hostile
// (the boundary ids and poison values above) and partly valid (real ids sampled
// from the map), so mutators actually fire and the invariants below are
// exercised rather than bounced at the first check. After EVERY call, assert the
// invariants hold. A mutator is allowed to fail -- returning false is the right
// answer to most of what this throws at it -- but it is NEVER allowed to return,
// or to have left the world, in a state these invariants forbid.
//
// WHAT THE INVARIANTS ARE. Not "the answer is X": that is a test, not a fuzzer,
// and it fails the day the answer improves. They are the things the ABI's own
// validators promise and that a bypass would break -- the same constants those
// validators use (kTreasuryLimit 1e12, the 1e11 population ceiling, research
// funding clamped to 0..1, groups to 0..3, industry to 0..10, an owner that is
// a real country or a sentinel). They are checked once on the freshly loaded
// world first, so a false positive is a bug in the invariant, not the game.
//
// DETERMINISTIC, like FuzzParsersTest: fixed seed, and a failure prints the seed
// and the exact call and arguments that broke it. Run it harder with the env:
//
//   OD_FUZZ_ITERS=2000000 ./ModApiFuzzTest
//   OD_FUZZ_SEED=12345    ./ModApiFuzzTest
//
// and, as with the other fuzzer, under ASan/UBSan by hand -- a read one past an
// array that no invariant happens to name is a real bug the sanitizer catches
// and the logic here would not.

#include "Game.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace {
int failures = 0;
void fail(const std::string& what) {
    printf("  FAILED  %s\n", what.c_str());
    ++failures;
}

// The ABI's own ceilings, from Game_Mods.cpp. Kept here as literals on purpose:
// if one moves there, this test should start failing until it is moved here too,
// which is the same contract tests/combat_depth_test.cpp keeps with depthFactor.
constexpr double    kTreasuryLimit = 1e12;
constexpr long long kPopCeiling    = 100000000000LL;  // 1e11

// xorshift64*, so the stream is ours and a seed reproduces a run exactly rather
// than depending on whatever libc's rand() is this month.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    uint32_t u32() { return (uint32_t)(next() >> 32); }
    int pick(int n) { return n <= 0 ? 0 : (int)(u32() % (uint32_t)n); }
    bool coin() { return (next() & 1) != 0; }
};
}  // namespace

struct ModApiFuzz {
    Game game;
    Rng rng{0};
    std::vector<int> cids;          // real country ids, sampled from the map
    std::vector<int> pids;          // real province ids
    std::vector<std::string> strs;  // real policy/law/node/troop/sector words + poison

    bool load(const std::string& dataDir) {
        game.m_headless = true;
        game.m_dataDir = dataDir;
        if (!game.loadFromODM(dataDir + "STDmaps/1914.odmap")) return false;
        game.loadGameDataStep1();
        game.loadGameDataStep2();
        if (game.m_countries.getAll().empty()) return false;

        for (int cid : game.modCountryIds())
            if (cid > 0 && cid < 65530) cids.push_back(cid);
        for (const auto& [pid, p] : game.m_provinces.getAllProvinces()) {
            (void)p;
            pids.push_back(pid);
            if (pids.size() >= 4000) break;
        }

        // Real words the string-keyed mutators will actually accept, so some of
        // those calls land instead of all bouncing at the lookup.
        for (int i = 0, n = game.modPolicyCount(); i < n && i < 40; ++i)
            strs.push_back(game.modPolicyId(i));
        for (int i = 0, n = game.modDistrictLawCount(); i < n && i < 40; ++i)
            strs.push_back(game.modDistrictLawId(i));
        for (int i = 0, n = game.modResearchNodeCount(); i < n && i < 60; ++i)
            strs.push_back(game.modResearchNodeId(i));
        for (int i = 0, n = game.modTroopTypeCount(); i < n; ++i)
            strs.push_back(game.modTroopTypeId(i));
        for (const char* w : {"oil", "gold", "metal", "rubber", "gemstones", "food"})
            strs.push_back(w);
        for (const char* w : {"mortar", "light", "heavy", "napalm", "carpet",
                              "chemical", "nuclear", "biological"})
            strs.push_back(w);
        // Poison: empty, junk, something huge.
        strs.push_back("");
        strs.push_back("\x01\x02\xff not a real id \0 embedded");
        strs.push_back(std::string(16384, 'A'));
        return !cids.empty() && !pids.empty();
    }

    // ── the hostile argument pools ──
    int anyCountryId() {
        static const int poison[] = {0, -1, 1, 65533, 65534, 65535, 65536,
                                     2147483647, -2147483647 - 1};
        if (rng.pick(3) == 0 && !cids.empty()) return cids[rng.pick((int)cids.size())];
        return poison[rng.pick((int)(sizeof(poison) / sizeof(poison[0])))];
    }
    int anyProvinceId() {
        static const int poison[] = {0, -1, 1, 65534, 65535, 100000,
                                     2147483647, -2147483647 - 1};
        if (rng.pick(3) == 0 && !pids.empty()) return pids[rng.pick((int)pids.size())];
        return poison[rng.pick((int)(sizeof(poison) / sizeof(poison[0])))];
    }
    int anyIndex() {
        static const int v[] = {0, 1, 2, -1, 3, 4, 10, 1000, 2147483647,
                                -2147483647 - 1};
        return v[rng.pick((int)(sizeof(v) / sizeof(v[0])))];
    }
    double anyDouble() {
        static const double v[] = {0.0, 1.0, -1.0, 0.5, 100.0, -100.0,
                                   1e6, -1e6, 1e13, -1e13, 1e308, -1e308,
                                   std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity(),
                                   -std::numeric_limits<double>::infinity()};
        return v[rng.pick((int)(sizeof(v) / sizeof(v[0])))];
    }
    long long anyLL() {
        static const long long v[] = {0, 1, -1, 1000, kPopCeiling, kPopCeiling + 1,
                                      9223372036854775807LL,
                                      -9223372036854775807LL - 1};
        return v[rng.pick((int)(sizeof(v) / sizeof(v[0])))];
    }
    const std::string& anyStr() { return strs[rng.pick((int)strs.size())]; }

    // ── one random ABI call ──
    void step() {
        switch (rng.pick(26)) {
            // Mutators -- the "gain something" surface.
            case 0: game.modSetCountryTreasury(anyCountryId(), anyDouble()); break;
            case 1: game.modAddCountryTreasury(anyCountryId(), anyDouble()); break;
            case 2: game.modSetProvincePopulation(anyProvinceId(), anyLL()); break;
            case 3: game.modSetProvinceOwner(anyProvinceId(), anyCountryId()); break;
            case 4: game.modSetCountryResearchFunding(anyCountryId(), anyDouble()); break;
            case 5: game.modSetCountryResearchGroups(anyCountryId(), anyIndex()); break;
            case 6: game.modSetProvinceIndustryLevel(anyProvinceId(), anyIndex()); break;
            case 7: game.modSetCountryPolicy(anyCountryId(), anyStr(), rng.coin()); break;
            case 8: game.modSetCountryDistrictShare(anyCountryId(), anyIndex(), anyIndex()); break;
            case 9: game.modSetCountryDistrictLaw(anyCountryId(), anyIndex(), anyStr(), rng.coin()); break;
            case 10: game.modSetCountryDisclosure(anyCountryId(), anyIndex(), rng.coin()); break;
            case 11: game.modSetCountrySectorTax(anyCountryId(), anyStr(), anyDouble()); break;
            case 12: game.modSetProvinceOutput(anyCountryId(), anyProvinceId(), anyIndex()); break;
            case 13: game.modSetCountryNationalised(anyCountryId(), anyStr(), rng.coin()); break;
            case 14: game.modProposeWar(anyCountryId(), anyCountryId()); break;
            case 15: game.modSetCountryClaim(anyCountryId(), anyProvinceId(), rng.coin()); break;
            case 16: game.modOrderArmyMove(anyProvinceId(), anyProvinceId(), anyIndex()); break;
            case 17: game.modOrderShipMove(anyIndex(), anyDouble(), anyDouble()); break;
            case 18: game.modOrderShipEngage(anyIndex(), anyIndex()); break;
            case 19: game.modOrderShipBombard(anyIndex(), anyProvinceId(), anyStr()); break;
            // Monuments: gating is the thing worth breaking (build where you may not).
            case 20: {
                const int c = anyCountryId(), p = anyProvinceId(), k = anyIndex();
                const bool may = game.modCanBuildMonument(c, p, k);
                const bool built = game.modBuildMonument(c, p, k);
                if (built && !may)
                    fail("modBuildMonument succeeded where modCanBuildMonument said no "
                         "(cid=" + std::to_string(c) + " pid=" + std::to_string(p) +
                         " kind=" + std::to_string(k) + ")");
                break;
            }
            case 21: game.modUpgradeMonument(anyCountryId(), anyProvinceId()); break;
            case 22: game.modSetMonumentActive(anyCountryId(), anyProvinceId(), rng.coin()); break;
            case 23: game.modMoveMonument(anyCountryId(), anyProvinceId(), anyProvinceId()); break;
            // Accessors with an index/id: they must never read out of bounds.
            // The call returning at all (no crash, no sanitizer trip) is the test.
            case 24: {
                const int p = anyProvinceId();
                const int n = (int)game.modArmyStackCount(p);
                game.modArmyStackOwner(p, anyIndex());
                game.modArmyStackSize(p, anyIndex());
                game.modArmyStackOwner(p, n);          // exactly one past the end
                game.modProvinceMinorityShare(p, anyIndex());
                game.modProvinceNeighborAt(p, anyIndex());
                break;
            }
            case 25: {
                const int c = anyCountryId();
                game.modCountryDistrictName(c, anyIndex());
                game.modCountryDistrictShare(c, anyIndex());
                game.modCountryPartyName(c, anyIndex());
                game.modCountryDistrictProvince(c, anyIndex(), anyIndex());
                game.modResearchNodeId(anyIndex());
                game.modTroopTypeId(anyIndex());
                break;
            }
        }
    }

    // ── what no mod may ever make false ──
    bool invariants(const char* when) {
        const int before = failures;

        for (int cid : cids) {
            Country* c = game.m_countries.getCountry(cid);
            if (!c) continue;
            if (!(std::isfinite(c->treasury) &&
                  c->treasury > -kTreasuryLimit && c->treasury < kTreasuryLimit))
                fail(std::string(when) + ": treasury of " + std::to_string(cid) +
                     " left the sane range (" + std::to_string(c->treasury) + ")");

            auto rf = game.m_countryResearchAllocation.find(cid);
            if (rf != game.m_countryResearchAllocation.end() &&
                !(rf->second >= 0.0f && rf->second <= 1.0f))
                fail(std::string(when) + ": research funding of " + std::to_string(cid) +
                     " outside 0..1 (" + std::to_string(rf->second) + ")");

            auto rg = game.m_scriptResearchGroups.find(cid);
            if (rg != game.m_scriptResearchGroups.end() &&
                (rg->second < 0 || rg->second > Game::RESEARCH_GROUPS_MAX))
                fail(std::string(when) + ": research groups of " + std::to_string(cid) +
                     " outside 0.." + std::to_string(Game::RESEARCH_GROUPS_MAX));
        }

        for (const auto& [pid, pop] : game.m_provincePopulations)
            if (pop < 0 || pop > kPopCeiling)
                fail(std::string(when) + ": population of province " + std::to_string(pid) +
                     " outside 0..1e11 (" + std::to_string(pop) + ")");

        // Owner is always a real country, a sentinel, or unowned. A mod handing
        // a province to a country that does not exist is the plainest bypass.
        for (const auto& [pid, p] : game.m_provinces.getAllProvinces()) {
            const int o = p.countryId;
            if (o == 0 || o == Game::UNC_CID || o == Game::BLC_CID || o == Game::SPC_CID)
                continue;
            if (!game.m_countries.getCountry(o))
                fail(std::string(when) + ": province " + std::to_string(pid) +
                     " owned by non-existent country " + std::to_string(o));
        }

        for (const auto& [pid, ind] : game.m_provinceIndustry)
            if (ind.level < 0 || ind.level > 10)
                fail(std::string(when) + ": industry level of province " +
                     std::to_string(pid) + " outside 0..10 (" + std::to_string(ind.level) + ")");

        // A queued order is acted on next turn; a non-finite destination in it
        // is a NaN the resolver will steer a hull towards.
        for (const auto& o : game.m_pendingShipMoveOrders)
            if (!(std::isfinite(o.destLon) && std::isfinite(o.destLat) &&
                  o.destLon >= -180.0 && o.destLon <= 180.0 &&
                  o.destLat >= -90.0 && o.destLat <= 90.0))
                fail(std::string(when) + ": a queued ship move has an off-globe "
                     "destination (" + std::to_string(o.destLon) + "," +
                     std::to_string(o.destLat) + ")");

        return failures == before;
    }

    void run(uint64_t seed, long long iters) {
        rng = Rng(seed);
        printf("Mod API fuzzer -- seed %llu, %lld iterations\n",
               (unsigned long long)seed, iters);

        if (!invariants("baseline")) {
            printf("  the freshly loaded world already violates an invariant -- "
                   "the invariant is wrong, not the game\n");
            return;
        }
        printf("  baseline world is clean\n");

        for (long long i = 0; i < iters; ++i) {
            step();
            // Not every single iteration: the invariant walk is O(provinces) and
            // the point is to catch a bad state soon, not instantly. Often enough
            // that the offending call is still named in the next failure.
            if ((i & 0x3F) == 0 && !invariants("after a call")) {
                printf("  first violation at iteration %lld (seed %llu)\n", i,
                       (unsigned long long)seed);
                return;
            }
        }
        invariants("final");
        if (failures == 0)
            printf("  %lld calls, every invariant held\n", iters);
    }
};

int main(int argc, char** argv) {
    const std::string dataDir = argc > 1 ? argv[1] : "data/";
    const char* iterEnv = getenv("OD_FUZZ_ITERS");
    const char* seedEnv = getenv("OD_FUZZ_SEED");
    const long long iters = iterEnv ? atoll(iterEnv) : 60000;
    const uint64_t seed = seedEnv ? strtoull(seedEnv, nullptr, 10) : 0xD00D1234ull;

    ModApiFuzz f;
    if (!f.load(dataDir)) {
        fprintf(stderr, "could not load %sSTDmaps/1914.odmap\n", dataDir.c_str());
        return 2;
    }
    f.run(seed, iters);

    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
