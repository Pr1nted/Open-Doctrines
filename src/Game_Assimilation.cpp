// Culture that changes hands.
//
// ── THE HOLE THIS FILLS ──
//
// The game had no assimilation at all. Nothing in src/ matched "assimilat",
// and a province's ethnic breakdown only ever moved because PEOPLE moved --
// migration in processPopulation, which relocates bodies and renormalises the
// shares they leave behind. Nobody ever changed what they were.
//
// Meanwhile three research nodes sold exactly that and delivered nothing:
//
//   indoctrinate1  Cultural Programs    10 research
//   indoctrinate2  Educational Reform   20 research
//   indoctrinate3  National Identity    30 research
//
// Each set Node::indoctrinationPct, getResearchEffect would have summed it,
// and getResearchEffect("indoctrinationPct") was never called by anything. The
// field was written in four places and read in none.
//
// ── WHAT CONVERTS TOWARD WHAT ──
//
// Toward the country's TITULAR group -- its largest culture across all the
// land it holds -- and not toward whatever is locally dominant. A state runs
// schools in its own language; it does not teach the local majority's. So a
// Tajik-majority province inside a Pashtun state drifts Pashtun, which is the
// case that makes the mechanic mean anything. A province already wholly
// titular has nothing to convert and costs nothing to skip.
//
// ── ALIGNMENT IS THE BRAKE ──
//
// Scaled by how the group feels about the government. A minority repressed to
// zero alignment converts at zero: people do not adopt the culture of a state
// that is grinding them down, and without this the mechanic would be a button
// that deletes minorities for a flat research cost. Paired with the ethnic
// policy dial it becomes a real choice -- conciliate AND indoctrinate, and the
// problem goes away in fifty turns; repress and indoctrinate, and you have
// bought nothing.
//
// OFF BY DEFAULT, behind OD_INDOCTRINATION. It moves ethnic shares, which feed
// getProvinceRebellionChance, which is rolled against simRand -- so with the
// rule off this must not touch a single share.

#include "Game.h"

#include "GameInternals.h"

#include <cstdlib>

namespace {

/// A share this small is not a culture, it is rounding. Groups below it are
/// dropped into the titular share rather than left as an immortal 0.01%.
constexpr float kVanishBelow = 0.05f;

/**
 * Turns to convert a given fraction, as a per-turn factor.
 *
 * indoctrinationPct is 5, 10 and 20 on the three nodes, so a country holding
 * all three has 35. At this scale that is 3.5% of each group's share per turn
 * before alignment is applied -- a fifth of a minority in about fifty turns at
 * good alignment, which is slow enough to be a campaign-long project and fast
 * enough to be worth 60 research.
 */
constexpr float kPerPointPerTurn = 0.001f;

}  // namespace

bool Game::assimilationOn() const {
    static const bool on = std::getenv("OD_INDOCTRINATION") &&
                           atoi(std::getenv("OD_INDOCTRINATION")) != 0;
    return on;
}

float Game::assimilationRate(int countryId) const {
    if (!assimilationOn()) return 0.0f;
    // ONE call, not research plus doctrines separately: getTotalEffect is
    // already both ("getTotalEffect is this plus the doctrines in force",
    // Game.h). Adding getResearchEffect to it counts the research twice, which
    // is what the first draft of this line did.
    //
    // It also means a map may add its own assimilation doctrine carrying
    // indoctrinationPct and it works here with no code change -- the same way
    // a map may replace the whole doctrine catalogue.
    return std::max(0.0f, getTotalEffect("indoctrinationPct", countryId));
}

const std::string& Game::titularGroupOf(int countryId) const {
    static const std::string kNone;
    if (m_titularTurn != m_turnNumber) {
        // Once per turn for the whole world, not once per province: this walks
        // every province of every country and doing it per province was the
        // shape that made processRebellions 59 ms of a 133 ms turn.
        m_titularTurn = m_turnNumber;
        m_titularGroup.clear();
        std::unordered_map<int, std::unordered_map<std::string, double>> weight;
        for (const auto& [pid, prov] : m_provinces.getAllProvinces()) {
            if (prov.countryId <= 0) continue;
            auto mit = m_provinceMinorities.find(pid);
            if (mit == m_provinceMinorities.end()) continue;
            auto popIt = m_provincePopulations.find(pid);
            // Weighted by people, not by provinces. A culture holding twenty
            // empty provinces is not a country's titular nation.
            const double pop = popIt == m_provincePopulations.end()
                                   ? 0.0 : (double)popIt->second;
            if (pop <= 0.0) continue;
            for (const MinorityGroup& g : mit->second)
                weight[prov.countryId][g.name] += pop * (double)g.pct;
        }
        for (const auto& [cid, groups] : weight) {
            const std::string* best = nullptr;
            double bestW = -1.0;
            for (const auto& [name, w] : groups) {
                // Strictly greater, and ties broken by name, so the titular
                // group of a country split exactly down the middle is the same
                // one on every machine rather than whatever the hash yielded.
                if (w > bestW || (w == bestW && best && name < *best)) {
                    bestW = w; best = &name;
                }
            }
            if (best) m_titularGroup[cid] = *best;
        }
    }
    auto it = m_titularGroup.find(countryId);
    return it == m_titularGroup.end() ? kNone : it->second;
}

void Game::assimilateMinorities() {
    if (!assimilationOn()) return;

    for (auto& [pid, groups] : m_provinceMinorities) {
        const Province* prov = m_provinces.getProvinceById(pid);
        if (!prov || prov->countryId <= 0) continue;
        const int cid = prov->countryId;

        const float rate = assimilationRate(cid);
        if (rate <= 0.0f) continue;

        const std::string& titular = titularGroupOf(cid);
        if (titular.empty()) continue;

        // The titular group has to already be present to grow: a state does
        // not conjure its own people into a province that has none of them.
        // That is also what keeps this from being colonisation, which is a
        // different mechanic and would need to move population, not shares.
        float* into = nullptr;
        for (MinorityGroup& g : groups)
            if (g.name == titular) { into = &g.pct; break; }
        if (!into) continue;

        float gained = 0.0f;
        for (MinorityGroup& g : groups) {
            if (g.name == titular) continue;
            if (g.pct <= 0.0f) continue;

            const float align = getMinorityAlignment(cid, g.name);
            // The brake. At zero alignment nothing moves at all.
            const float willing = std::clamp(align / 100.0f, 0.0f, 1.0f);
            float moved = g.pct * rate * kPerPointPerTurn * willing;
            if (moved <= 0.0f) continue;

            // A remnant too small to be a culture goes whole rather than
            // halving forever toward an immortal 0.0001%.
            if (g.pct - moved < kVanishBelow) moved = g.pct;
            g.pct -= moved;
            gained += moved;
        }
        if (gained <= 0.0f) continue;
        *into += gained;

        // NOT renormalised. The shares are a partition and this moves share
        // between members of it, so the total is unchanged by construction --
        // renormalising here would paper over an arithmetic bug rather than
        // reveal it. Groups that reached zero are dropped, which is the only
        // structural change.
        groups.erase(std::remove_if(groups.begin(), groups.end(),
                                    [](const MinorityGroup& g) { return g.pct <= 0.0f; }),
                     groups.end());
    }
}
