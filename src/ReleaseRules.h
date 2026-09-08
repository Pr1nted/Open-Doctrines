#pragma once

// WHAT A COUNTRY MAY LET GO OF, decided once, for everybody.
//
// Releasing a nation is the counterpart to a rebellion: the same event, chosen
// rather than suffered. The machinery for making the new country already exists
// -- Game::createRebelCountry names it, flags it, averages its compass over the
// released provinces and moves the ground -- so what is missing is only the
// question of WHICH ground may go, and that question is pure.
//
// IT LIVES IN A HEADER OF ITS OWN, AND NOT IN Game, ON PURPOSE. Three callers
// need the same answer: the panel that offers the player a region, the
// bankruptcy cascade that sheds one when a country can no longer govern it, and
// (later) the AI when it can choose to. This codebase has now been bitten eight
// times by a rule with two homes -- the build-cost tables, the income
// projection, army upkeep, and seven copies of the artillery price list -- and
// the fix each time was one table with one reader. This starts that way.
//
// It also means the rule can be tested without a window, a map or a game; see
// tests/release_rules_test.cpp.

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * A minority must be this much of a province for that province to count as
 * theirs.
 *
 * AN OUTRIGHT MAJORITY, not a plurality. Minorities partition a province to
 * 100% and there are about nine and a half of them in the average one, so a
 * plurality can be 15% -- which would make almost every province in the world
 * "dominated" by somebody and turn release into a way of dismembering
 * neighbours' claims rather than settling your own. A majority is a much
 * stronger statement and a far rarer one.
 */
inline constexpr float RELEASE_DOMINANCE_PCT = 50.0f;

/**
 * A region has to be a region.
 *
 * One province is a grievance, not a nation, and allowing it would let a player
 * shed provinces one at a time to dodge unrest without ever facing the
 * territorial cost that is supposed to be the price.
 */
inline constexpr size_t RELEASE_MIN_PROVINCES = 2;

/**
 * And a country may not release itself out of existence.
 *
 * Without this a country one province from elimination could hand the last of
 * itself away, which is not a strategy, it is an exit. Half is generous enough
 * that shedding a genuinely large ethnic region stays possible.
 */
inline constexpr float RELEASE_MAX_SHARE = 0.5f;

/**
 * How reconciled a people may be and still want out.
 *
 * THIS IS WHAT REPLACED THE "TITULAR PEOPLE" RULE, and it is a better answer to
 * the same question. Measured on the shipped 1939 map, a size cap alone let the
 * Soviet Union release the Russians (34 provinces, 94 million) and the United
 * States release White Americans (36 provinces) -- a country shedding its own
 * core. But excluding "the country's own people" cannot be computed here: by
 * population Britain's largest group is Hindustani and France's is Kinh, by
 * province count Britain's is Indigenous Canadian, and there is no capital in
 * the map data to appeal to. Every definition picks a COLONIAL people for a
 * colonial empire, and would have forbidden Britain from releasing India while
 * offering to release Britain.
 *
 * Alignment asks the question that actually matters: do these people want to be
 * governed by you? A nation's own people are highly aligned and never offered;
 * a repressed minority is disaffected and is. It also connects release to the
 * ethnic policy system, which is where it belongs -- repression makes a region
 * releasable and conciliation removes the need, so release becomes the last
 * rung of that ladder rather than a separate lever.
 *
 * Scale is 0-100, higher is more reconciled; the eval reports a world mean near
 * 74 and calls a group disaffected below 40. 55 sits between: clearly unhappy,
 * without requiring the total breakdown that 40 implies.
 */
inline constexpr float RELEASE_MAX_ALIGNMENT = 55.0f;

/** One province, as the rule needs to see it. */
struct ReleaseProvince {
    int id = 0;
    long long population = 0;
    std::string topMinority;      ///< the largest group living there
    float topMinorityPct = 0.0f;  ///< its share, 0-100
    /**
     * How reconciled that group is with the province's owner, 0-100.
     *
     * Per province rather than per people, because the same minority can be
     * content in one province and not in another -- and because it is what
     * getMinorityAlignment already answers, so no caller has to average
     * anything before it asks.
     */
    float alignment = 0.0f;
};

/** A region that could become a country. */
struct ReleaseCandidate {
    std::string minority;
    std::vector<int> provinces;   ///< ascending, so the answer is stable
    long long population = 0;
};

/**
 * Every contiguous run of provinces where one DISAFFECTED minority holds a
 * majority. See RELEASE_MAX_ALIGNMENT for why contentment is the gate.
 *
 * `neighborsOf(pid)` returns something iterable of province ids; only ids in
 * `owned` are ever followed, so a region cannot reach across somebody else's
 * territory to join two enclaves into one nation.
 *
 * DETERMINISTIC BY CONSTRUCTION. The seeds are walked in the order `owned`
 * arrives, every candidate's province list is sorted, and the candidates
 * themselves are sorted by population and then by first province. A rule that
 * offered the player a different region on a re-run of the same save would be a
 * worse bug than one that offered none.
 */
template <typename NeighborFn>
inline std::vector<ReleaseCandidate> findReleasableRegions(
    const std::vector<ReleaseProvince>& owned, NeighborFn neighborsOf) {
    std::vector<ReleaseCandidate> out;
    if (owned.size() < RELEASE_MIN_PROVINCES) return out;

    // Index by id so the flood fill can ask about a neighbour cheaply, and so
    // that following an edge out of the country is a lookup miss rather than a
    // special case.
    std::unordered_map<int, const ReleaseProvince*> byId;
    byId.reserve(owned.size() * 2);
    for (const auto& p : owned) byId[p.id] = &p;

    // ── THERE IS NO "TITULAR PEOPLE" RULE, AND THAT IS DELIBERATE ──
    //
    // The first version excluded the country's own largest group, so that a
    // nation could not release its own heartland. It was measured against the
    // shipped 1939 map and it was WRONG, in the way that matters most.
    //
    // "The country's own people" is not computable from this data. By
    // population, Britain's largest group is Hindustani and France's is Kinh;
    // by province count, Britain's is Indigenous Canadian and the Netherlands'
    // is Javanese either way. There is no capital in countries.json to fall
    // back on. So every definition picks a COLONIAL people for a colonial
    // empire -- and the rule would then have forbidden Britain from releasing
    // India while cheerfully offering to release Britain. Decolonisation is the
    // single most obvious thing a player would want this feature for, and the
    // rule would have banned exactly that.
    //
    // RELEASE_MAX_SHARE and RELEASE_MAX_ALIGNMENT do the work instead: a
    // homogeneous country's only region is the whole country, which is more
    // than half of itself and is refused for size; and a people who are content
    // to be governed by you are not offered at all, whoever they are. See
    // RELEASE_MAX_ALIGNMENT.

    const size_t maxProvinces =
        (size_t)((double)owned.size() * (double)RELEASE_MAX_SHARE);

    // A province belongs to at most one candidate: once it has been claimed by
    // a region it cannot seed or join another, so the regions offered never
    // overlap and a player cannot be shown the same ground twice.
    std::unordered_set<int> taken;

    for (const auto& seed : owned) {
        if (seed.topMinorityPct < RELEASE_DOMINANCE_PCT) continue;
        if (seed.topMinority.empty()) continue;
        if (seed.alignment > RELEASE_MAX_ALIGNMENT) continue;   // content: not a region
        if (taken.count(seed.id)) continue;

        // Flood out from the seed through provinces held by the SAME minority.
        ReleaseCandidate cand;
        cand.minority = seed.topMinority;
        std::vector<int> stack{seed.id};
        std::unordered_set<int> seen{seed.id};
        while (!stack.empty()) {
            const int pid = stack.back();
            stack.pop_back();
            auto it = byId.find(pid);
            if (it == byId.end()) continue;                 // not ours: stop
            const ReleaseProvince& p = *it->second;
            if (p.topMinority != cand.minority) continue;    // theirs, not this nation's
            if (p.topMinorityPct < RELEASE_DOMINANCE_PCT) continue;
            if (p.alignment > RELEASE_MAX_ALIGNMENT) continue;
            if (taken.count(pid)) continue;

            cand.provinces.push_back(pid);
            cand.population += p.population;
            for (int n : neighborsOf(pid))
                if (!seen.count(n)) { seen.insert(n); stack.push_back(n); }
        }

        if (cand.provinces.size() < RELEASE_MIN_PROVINCES) continue;
        // Too large to give away. Refused whole rather than trimmed: a region
        // is a nation, and handing over an arbitrary half of one because the
        // whole would not fit is not a border anybody drew.
        if (maxProvinces > 0 && cand.provinces.size() > maxProvinces) continue;

        std::sort(cand.provinces.begin(), cand.provinces.end());
        for (int pid : cand.provinces) taken.insert(pid);
        out.push_back(std::move(cand));
    }

    // Largest first -- the region a player is most likely to mean -- with the
    // first province id breaking ties so the order never depends on how the
    // provinces happened to be stored.
    std::sort(out.begin(), out.end(), [](const ReleaseCandidate& a, const ReleaseCandidate& b) {
        if (a.population != b.population) return a.population > b.population;
        return a.provinces.front() < b.provinces.front();
    });
    return out;
}
