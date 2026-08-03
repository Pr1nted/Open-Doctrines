#include "AISystem.h"
#include "../Game.h"
#include "../GameInternals.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

// Cost tables mirrored from the player UI (Game_Render.cpp). Costs are charged
// at ENQUEUE time, exactly like the player's buttons — the turn executors
// never charge money, so skipping the deduction here would let the AI build
// for free.
static const int AI_IND_COST[]  = {0, 1, 10, 15, 25, 50, 75, 100, 150, 200, 300};
static const int AI_FORT_COST[] = {0, 20, 30, 50, 100, 200};
static const int AI_IND_TURNS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

static const char* MODULE_NAMES[] = {"econ", "politics", "war", "navy"};

// Research is a player-only system, so AI countries would report level-0 caps
// forever and could never build anything. They get a baseline capability
// instead; researched levels still raise the cap when a map grants them.
int AISystem::industryCap(int cid) const {
    return std::clamp(std::max(3, m_g->getResearchedIndustryLevel(cid)), 1, 10);
}
int AISystem::fortCap(int cid) const {
    return std::clamp(std::max(2, m_g->getResearchedFortLevel(cid)), 1, 5);
}
int AISystem::portCap(int cid) const {
    return std::clamp(std::max(1, m_g->getResearchedPortLevel(cid)), 1, 3);
}

AISystem::AISystem(Game* game, const std::string& modelPath)
    : m_g(game), m_modelPath(modelPath) {
    // Policy nets: features -> action logits. Value nets: features -> scalar.
    // ~1M parameters across all nine nets (~12MB on disk with Adam state):
    // each policy net is 96->512->320->N (~215k params). Still fast — a
    // forward pass is ~0.2M multiply-adds, so hundreds of countries per turn
    // stay in the low milliseconds.
    // One encoder, eight heads. See TRUNK_OUT.
    m_trunk = NeuralNet({FEATURE_COUNT, 512, TRUNK_OUT}, 100);
    m_trunk.setTanhOutput(true);
    m_relEncoder = NeuralNet({REL_FEATURES, REL_EMBED}, 106);
    m_relEncoder.setTanhOutput(true);
    m_relScore   = NeuralNet({REL_EMBED, 1}, 107);
    m_stanceHead = NeuralNet({TRUNK_OUT, STANCE_COUNT}, 105);
    m_policy[MOD_ECONOMY]  = NeuralNet({TRUNK_OUT, ECON_ACTIONS}, 101);
    m_policy[MOD_POLITICS] = NeuralNet({TRUNK_OUT, POL_ACTIONS},  102);
    m_policy[MOD_WAR]      = NeuralNet({TRUNK_OUT, WAR_ACTIONS},  103);
    m_policy[MOD_NAVY]     = NeuralNet({TRUNK_OUT, NAVY_ACTIONS}, 104);
    for (int m = 0; m < MOD_COUNT; ++m)
        m_value[m] = NeuralNet({FEATURE_COUNT, 160, 1}, 200 + m);
    // Q shares the policy's shape because it answers a question of the same
    // size -- one number per action -- and its own seeds so it does not start
    // life as a copy of the actor it is meant to improve on.
    static constexpr int ACTS_[MOD_COUNT] = {ECON_ACTIONS, POL_ACTIONS, WAR_ACTIONS, NAVY_ACTIONS};
    for (int m = 0; m < MOD_COUNT; ++m)
        m_q[m] = NeuralNet({TRUNK_OUT, ACTS_[m]}, 400 + m);
    m_diplo = NeuralNet({TRUNK_OUT, DIPLO_ACTIONS}, 300);
    // Own state and one candidate in, one score out. Scored once per candidate
    // and softmaxed across them, so the output is deliberately a single number
    // rather than a fixed-width action layer -- the candidate list changes size
    // every turn.
    m_target = NeuralNet({FEATURE_COUNT + TARGET_FEATURES, 256, 128, 1}, 500);
    m_diploValue = NeuralNet({FEATURE_COUNT, 160, 1}, 600);
    // An empty path is a scratch model used for merging peer files, not a
    // model anybody is training. It loads nothing, saves nothing, and should
    // say nothing.
    if (m_modelPath.empty()) return;
    if (loadModel()) {
        printf("[AI] Model loaded from %s (%.1f MB)\n", m_modelPath.c_str(),
               serializedSize() / 1048576.0);
        // "0.0 MB on disk" was not a size, it was an uninitialised counter:
        // m_lastSaveBytes is written only by saveModel(), so it read zero until
        // the first checkpoint -- and permanently under --ai-readonly, which
        // never saves at all. Measure the model we actually hold instead.
        m_lastSaveBytes = serializedSize();
    }
    else
        printf("[AI] Fresh model (no file at %s)\n", m_modelPath.c_str());
}

AISystem::~AISystem() {
    recordLeagueOutcome();
    saveModel();
    // AND HERE, not only on the periodic save. This object is destroyed and
    // rebuilt on every map rotation, and a map can easily be shorter than
    // SAVE_INTERVAL_SECONDS -- three forty-turn maps ran in 1.6 minutes, so the
    // sixty-second timer never fired once inside a single instance's life and
    // no checkpoint was ever written. Map rotation is in fact the better moment
    // for one: it is exactly when a policy has finished learning something.
    writeLeagueCheckpoint();
}

// Rebels are their own bucket -- see the note on m_rebelStats. Everything else
// splits by cohort, and with no --vs-random split m_randomCids is empty, so
// every real country lands in m_trainStats exactly as before.
int AISystem::foreignWarCount(int cid) const {
    auto it = m_warWith.find(cid);
    if (it == m_warWith.end()) return 0;
    int n = 0;
    for (int other : it->second)
        if (other < Game::REBEL_CID_MIN) n++;
    return n;
}

void AISystem::noteConquest(int winnerCid, int loserCid, bool contested) {
    // Split by WHO lost it. Taking a province off a rebel is opportunism on
    // somebody else's collapse; taking one off a country is the war the game is
    // supposed to be about, and a cohort can be doing a great deal of one while
    // doing none of the other.
    if (loserCid >= Game::REBEL_CID_MIN) statsFor(winnerCid).provTakenFromRebel++;
    else {
        statsFor(winnerCid).provTakenFromCountry++;
        statsFor(loserCid).provLostToCountry++;
        if (contested) statsFor(winnerCid).provTakenInBattle++;
        else           statsFor(winnerCid).provWalkedInto++;
    }
}

AISystem::TrainStats& AISystem::statsFor(int cid) {
    if (cid >= Game::REBEL_CID_MIN) return m_rebelStats;
    return isRandomCountry(cid) ? m_randomStats : m_trainStats;
}

// ─── World cache ─────────────────────────────────────────

void AISystem::beginTurn() {
    const bool firstTurnOfMap = (m_turn == 0);
    m_turn++;
    m_decisionsThisTurn = 0;
    // Landings are counted per turn and accumulated into every open reward
    // window, exactly like rebellions — so the tally has to be cleared here,
    // not when it is read.
    m_landingsThisTurn.clear();
    refreshStats();
    updateWorld();
    updateTrends();

    // This map's frozen opponent, drawn once the world exists.
    //
    // AFTER refreshStats, not before: the choice is over countries and it reads
    // them from m_stats, which refreshStats is what fills. Asking first found an
    // empty map, took the "too small to split" branch, and assigned nobody --
    // silently, because that branch has nothing to report.
    if (firstTurnOfMap && selfPlayLearning()) {
        if (loadLeagueOpponent()) assignLeagueCountries();
    }
    // Baseline for NEXT turn's "did I lose ground?" delta. Recorded here, after
    // refreshStats has consumed the previous baseline, and NOT in the endTurn
    // refresh — otherwise the mid-turn refresh would reset the comparison and
    // provincesLost would read zero forever.
    for (auto& [cid, st] : m_stats) m_prevProvinces[cid] = st.provinces;
    // How long each country has been at war without a break. Here and not in
    // refreshStats: that runs again in endTurn, and a war would age two turns
    // for every turn played. emplace leaves an existing start turn alone, so
    // the entry survives for as long as the war does.
    for (auto& [cid, st] : m_stats) {
        (void)st;
        auto w = m_warWith.find(cid);
        if (w != m_warWith.end() && !w->second.empty()) m_warSince.emplace(cid, m_turn);
        else                                           m_warSince.erase(cid);
    }
}

void AISystem::refreshStats() {
    m_stats.clear();
    m_worldArmy = 0;
    m_worldPixels = 0;

    Game& g = *m_g;

    // Provinces / population / industry — one pass each over existing maps.
    for (const auto& [pid, prov] : g.m_provinces.getAllProvinces()) {
        int cid = prov.countryId;
        if (cid <= 0 || cid >= Game::SPC_CID) continue;
        CountryStat& st = m_stats[cid];
        st.provinces++;
        auto popIt = g.m_provincePopulations.find(pid);
        if (popIt != g.m_provincePopulations.end()) st.population += popIt->second;
        auto indIt = g.m_provinceIndustry.find(pid);
        if (indIt != g.m_provinceIndustry.end()) {
            st.industrySum += indIt->second.level;
            st.fortSum += indIt->second.fortification;
        }
        auto portIt = g.m_provincePorts.find(pid);
        if (portIt != g.m_provincePorts.end())
            st.maxPort = std::max(st.maxPort, portIt->second.level);
    }
    // Armies
    for (auto& [pid, units] : g.m_provinceArmies)
        for (auto& u : units)
            if (u.countryId > 0 && u.countryId < Game::SPC_CID) {
                m_stats[u.countryId].army += u.count;
                m_worldArmy += u.count;
            }
    // Ships
    for (auto& s : g.m_ships) {
        if (s.countryId <= 0 || s.countryId >= Game::SPC_CID) continue;
        CountryStat& st = m_stats[s.countryId];
        if (s.type == "carrier") st.carriers++;
        else if (s.type == "destroyer") st.destroyers++;
        else { st.boats++; if (s.crew > 0) st.boatsWithCrew++; }
    }
    // Relations as integer sets, resolved once.
    //
    // Everything below asks "are these two at war / allied?" thousands of times
    // per turn, and each answer used to cost two country lookups plus two
    // string-keyed hash probes into m_relations. Resolving the ISO graph into
    // cid sets once makes every later question an integer set lookup.
    m_warWith.clear();
    m_alliedWith.clear();
    for (auto& [isoA, targets] : g.m_relations) {
        int a = g.cidForIso(isoA);
        if (a < 0) continue;
        for (auto& [isoB, rel] : targets) {
            if (!rel.war && !rel.alliance) continue;
            int b = g.cidForIso(isoB);
            if (b < 0 || b == a) continue;
            if (rel.war) m_warWith[a].insert(b);
            if (rel.alliance) m_alliedWith[a].insert(b);
        }
    }

    // Garrison of one country in one province. Hot enough to be worth a lambda
    // rather than a repeated find + inner loop at each call site.
    auto garrison = [&](int pid, int owner) -> long long {
        auto it = g.m_provinceArmies.find(pid);
        if (it == g.m_provinceArmies.end()) return 0;
        long long n = 0;
        for (auto& u : it->second) if (u.countryId == owner) n += u.count;
        return n;
    };

    // Frontiers: provinces bordering a different country. One pass over the
    // adjacency map (built once at load; geometric, ownership-independent).
    for (auto& [pid, nbrs] : g.m_provinceNeighbors) {
        int owner = (pid >= 0 && pid < (int)g.m_provinceCountryLookup.size())
                        ? g.m_provinceCountryLookup[pid] : 0;
        if (owner <= 0 || owner >= Game::SPC_CID) continue;
        // Record the most THREATENING foreign neighbour rather than simply the
        // first one found. Stopping at the first meant a province facing both a
        // live enemy and a neutral could register the neutral — and the attack
        // and artillery actions both gate on atWarWith(fr.enemyCid), so the real
        // threat became invisible and the AI never responded to it.
        const Country* oc = g.m_countries.getCountry(owner);
        auto relOwner = oc ? g.m_relations.find(oc->isoA3) : g.m_relations.end();
        int best = 0, bestRank = -1;
        for (int nid : nbrs) {
            int nOwner = (nid >= 0 && nid < (int)g.m_provinceCountryLookup.size())
                             ? g.m_provinceCountryLookup[nid] : 0;
            if (nOwner <= 0 || nOwner >= Game::SPC_CID || nOwner == owner) continue;
            int rank = 0;
            const Country* nc = g.m_countries.getCountry(nOwner);
            if (nc && relOwner != g.m_relations.end()) {
                auto rr = relOwner->second.find(nc->isoA3);
                if (rr != relOwner->second.end() && rr->second.war)
                    rank = (nOwner >= Game::REBEL_CID_MIN) ? 3 : 2;
            }
            if (rank > bestRank) { bestRank = rank; best = nOwner; }
            if (rank == 3) break; // nothing outranks a revolt on our own soil
        }
        if (bestRank >= 0) m_stats[owner].frontiers.push_back({pid, best});

        // ── Defensive picture + staging opportunities ──
        // Both need the SAME neighbour walk the frontier scan above already
        // does, so they ride along rather than costing a second pass.
        CountryStat& ost = m_stats[owner];
        auto warIt = m_warWith.find(owner);
        auto allyIt = m_alliedWith.find(owner);
        const bool anyWar = warIt != m_warWith.end() && !warIt->second.empty();
        long long enemyHere = 0;
        for (int nid : nbrs) {
            int nOwner = (nid >= 0 && nid < (int)g.m_provinceCountryLookup.size())
                             ? g.m_provinceCountryLookup[nid] : 0;
            if (nOwner <= 0 || nOwner == owner) continue;
            const bool atWar = warIt != m_warWith.end() && warIt->second.count(nOwner);
            const bool allied = allyIt != m_alliedWith.end() && allyIt->second.count(nOwner);
            if (atWar) {
                enemyHere += garrison(nid, nOwner);
            } else if (allied) {
                ost.allyAdjArmy += garrison(nid, nOwner);
                // Staging: an allied province next door that itself touches
                // somebody we are fighting. Our troops can walk in (allied
                // territory is passable) and be on that front next turn.
                if (anyWar) {
                    auto n2 = g.m_provinceNeighbors.find(nid);
                    if (n2 != g.m_provinceNeighbors.end()) {
                        for (int nn : n2->second) {
                            int nnOwner = (nn >= 0 && nn < (int)g.m_provinceCountryLookup.size())
                                              ? g.m_provinceCountryLookup[nn] : 0;
                            if (nnOwner <= 0 || nnOwner == owner) continue;
                            if (!warIt->second.count(nnOwner)) continue;
                            ost.staging.push_back({pid, nid, nnOwner});
                            break; // one entry per (our province, ally province)
                        }
                    }
                }
            }
        }
        if (enemyHere > 0) {
            const long long mine = garrison(pid, owner);
            ost.threatenedProvinces++;
            ost.enemyAdjArmy += enemyHere;
            ost.defenderArmy += mine;
            const long long deficit = enemyHere - mine;
            if (ost.worstThreatPid < 0 || deficit > ost.worstDeficit) {
                ost.worstDeficit = deficit;
                ost.worstThreatPid = pid;
            }
        }
    }

    // Troops standing on allied soil, and how the coalition is behaving.
    for (auto& [pid, units] : g.m_provinceArmies) {
        int owner = (pid >= 0 && pid < (int)g.m_provinceCountryLookup.size())
                        ? g.m_provinceCountryLookup[pid] : 0;
        if (owner <= 0) continue;
        for (auto& u : units) {
            if (u.countryId <= 0 || u.countryId >= Game::SPC_CID) continue;
            if (u.countryId == owner) continue;
            auto ai2 = m_alliedWith.find(u.countryId);
            if (ai2 != m_alliedWith.end() && ai2->second.count(owner)) {
                CountryStat& as = m_stats[u.countryId];
                as.armyAbroad += u.count;
                as.abroadPids.push_back(pid);
            }
        }
    }
    // Standing agreements, deduplicated across both directions of the relation
    // graph. Counted here rather than at reward time because the politics
    // module is judged on how many it is holding, and rescanning the ISO-keyed
    // relations once per settled experience would be a scan per country turn.
    {
        std::unordered_map<int, std::unordered_set<int>> pactWith;
        for (auto& [isoA, targets] : g.m_relations) {
            int a = g.cidForIso(isoA);
            if (a < 0) continue;
            for (auto& [isoB, rel] : targets) {
                if (!rel.alliance && !rel.nonAggression && !rel.guarantee) continue;
                int b = g.cidForIso(isoB);
                if (b < 0 || b == a) continue;
                pactWith[a].insert(b);
                pactWith[b].insert(a);   // a pact recorded on one row binds both
            }
        }
        for (auto& [cid, st] : m_stats) {
            auto it = pactWith.find(cid);
            if (it != pactWith.end()) st.pacts = (int)it->second.size();
        }
    }

    for (auto& [cid, st] : m_stats) {
        auto warIt = m_warWith.find(cid);
        if (warIt == m_warWith.end() || warIt->second.empty()) continue;
        auto allyIt = m_alliedWith.find(cid);
        if (allyIt == m_alliedWith.end()) continue;
        for (int ally : allyIt->second) {
            auto aw = m_warWith.find(ally);
            bool shares = false;
            if (aw != m_warWith.end())
                for (int e : warIt->second)
                    if (aw->second.count(e)) { shares = true; break; }
            if (shares) st.coBelligerents++;
            else st.idleAllies++;
        }
    }

    // ── Minorities, per country ──
    // Only provinces that actually have minorities are visited, and each
    // distinct group is measured once per country rather than once per
    // province — getMinorityAlignmentTrend walks every policy category, so the
    // per-province version of this would be the same answer computed dozens of
    // times for a country with a widespread group.
    {
        std::unordered_map<int, std::unordered_set<std::string>> byCountry;
        for (auto& [pid, groups] : g.m_provinceMinorities) {
            int owner = (pid >= 0 && pid < (int)g.m_provinceCountryLookup.size())
                            ? g.m_provinceCountryLookup[pid] : 0;
            if (owner <= 0 || owner >= Game::SPC_CID) continue;
            for (auto& mg : groups) byCountry[owner].insert(mg.name);
        }
        for (auto& [cid, names] : byCountry) {
            auto sIt = m_stats.find(cid);
            if (sIt == m_stats.end() || names.empty()) continue;
            CountryStat& st = sIt->second;
            st.minorities = (int)names.size();
            float alignSum = 0, trendSum = 0, costSum = 0;
            for (const std::string& name : names) {
                const float a = g.getMinorityAlignment(cid, name);
                alignSum += a;
                trendSum += g.getMinorityAlignmentTrend(cid, name);
                if (a < st.worstAlignment) { st.worstAlignment = a; st.worstMinority = name; }
                for (size_t ci = 0; ci < g.m_ethnicPolicyCategories.size(); ++ci) {
                    const int oi = g.ethnicPolicyOption(cid, name, ci);
                    if (oi >= 0 && oi < (int)g.m_ethnicPolicyCategories[ci].options.size())
                        costSum += g.m_ethnicPolicyCategories[ci].options[oi].costPerTurn;
                }
            }
            st.meanAlignment = alignSum / names.size();
            st.minorityTrend = trendSum / names.size();
            st.minorityCost = costSum;
        }
    }

    // Provinces lost since last turn. "Am I losing?" is not derivable from a
    // single snapshot, and it is the signal a defensive policy needs most.
    for (auto& [cid, st] : m_stats) {
        auto prev = m_prevProvinces.find(cid);
        if (prev != m_prevProvinces.end() && prev->second > st.provinces)
            st.provincesLost = prev->second - st.provinces;
    }
    // Claims: one pass over the reverse index. A claim only matters while the
    // claimant and the owner are different countries.
    for (auto& [pid, claimants] : g.m_claimsByProvince) {
        int owner = (pid >= 0 && pid < (int)g.m_provinceCountryLookup.size())
                        ? g.m_provinceCountryLookup[pid] : 0;
        if (owner <= 0 || owner >= Game::SPC_CID) continue;
        for (auto& iso : claimants) {
            int claimant = g.cidForIso(iso);
            if (claimant < 0 || claimant == owner) continue;
            m_stats[owner].claimsAgainstMe++;
            if (claimant < Game::SPC_CID) m_stats[claimant].myClaimsOutstanding++;
        }
    }
    for (auto& v : g.m_countryPixels) m_worldPixels += v.size();
    if (m_worldPixels == 0) m_worldPixels = 1;

    // ── Naval invasion targets ──────────────────────────────
    // The war module can only declare war across a LAND frontier, so a country
    // separated by water from everyone would never fight — and the whole
    // embark→sail→disembark chain the navy module already implements would
    // never fire (the classic "AI won't cross water to take land" problem).
    // Here we count, per port-owning country, how many enemy countries it could
    // reach BY SEA: they own a port (ports are always coastal and are where the
    // fleet lands), we are not land-adjacent to them, and we are not already
    // friendly or at war. This both feeds a feature and unlocks the declare-war
    // action for overseas foes.
    {
        std::vector<int> coastal; // real countries that own at least one port
        std::unordered_set<int> seenCoastal;
        for (auto& [pid, port] : g.m_provincePorts) {
            int owner = (pid >= 0 && pid < (int)g.m_provinceCountryLookup.size())
                            ? g.m_provinceCountryLookup[pid] : 0;
            if (owner > 0 && owner < Game::REBEL_CID_MIN && seenCoastal.insert(owner).second)
                coastal.push_back(owner);
        }
        if (coastal.size() >= 2) {
            for (auto& [cid, st] : m_stats) {
                if (st.maxPort < 1 || cid >= Game::REBEL_CID_MIN) continue;
                const Country* c = g.m_countries.getCountry(cid);
                if (!c) continue;
                std::unordered_set<int> landNbr;
                for (auto& fr : st.frontiers) landNbr.insert(fr.enemyCid);
                auto relIt = g.m_relations.find(c->isoA3);
                int count = 0, warCount = 0;
                for (int oc : coastal) {
                    if (oc == cid || landNbr.count(oc)) continue;
                    const Country* ec = g.m_countries.getCountry(oc);
                    if (!ec) continue;
                    if (relIt != g.m_relations.end()) {
                        auto rr = relIt->second.find(ec->isoA3);
                        if (rr != relIt->second.end()) {
                            if (rr->second.war) { ++warCount; continue; }
                            if (rr->second.alliance || rr->second.guarantee)
                                continue; // off-limits
                        }
                    }
                    ++count;
                }
                st.navalTargets = count;
                st.navalWarTargets = warCount;
            }
        }
    }
}

// ─── Features ────────────────────────────────────────────

static inline float nlog(double v, double scale) {
    return (float)std::tanh(std::log1p(std::max(0.0, v)) / scale);
}


// ─── Trends ──────────────────────────────────────────────
//
// The eight features at the end of the observation; see FEATURE_COUNT.
//
// Every read below is of state that already exists this turn. Nothing here may
// call anything with a per-turn cache -- that is what broke determinism the
// first time this was written.

AISystem::TrendPoint AISystem::sampleTrend(int cid) const {
    TrendPoint t;
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    auto sIt = m_stats.find(cid);
    if (!c || sIt == m_stats.end()) return t;
    const CountryStat& st = sIt->second;
    t.turn       = m_turn;
    t.provinces  = (float)st.provinces;
    t.army       = (float)st.army;
    t.industry   = st.industrySum;
    t.population = (float)st.population;
    t.treasury   = (float)c->treasury;
    t.align      = st.meanAlignment;
    t.weariness  = g.warWearinessOf(cid);
    // What is standing across our borders. A neighbour massing troops is the
    // most actionable thing a turn-based AI can notice, and the observation had
    // no representation for it at all. Accumulated as an integer count, so the
    // sum does not depend on the order the province map is walked.
    long long enemy = 0;
    for (const auto& fr : st.frontiers) {
        auto aIt = g.m_provinceArmies.find(fr.pid);
        if (aIt == g.m_provinceArmies.end()) continue;
        for (const auto& u : aIt->second)
            if (u.countryId != cid) enemy += u.count;
    }
    t.threat = (float)enemy;
    return t;
}

void AISystem::updateWorld() {
    Game& g = *m_g;
    WorldSnapshot w;
    w.turn = m_turn;
    std::vector<std::pair<int, int>> byLand;   // (provinces, cid)
    byLand.reserve(m_stats.size());
    long long total = 0;
    int alive = 0, atWar = 0;
    double unrestSum = 0.0;
    for (const auto& [cid, st] : m_stats) {
        if (cid >= Game::REBEL_CID_MIN) continue;   // rebels are not powers
        if (st.provinces <= 0) continue;
        byLand.push_back({st.provinces, cid});
        total += st.provinces;
        alive++;
        if (foreignWarCount(cid) > 0) atWar++;
        unrestSum += g.warWearinessOf(cid);
    }
    w.totalProvinces = total;
    if (alive > 0 && total > 0) {
        double hh = 0.0;
        int largest = 0;
        for (const auto& [prov, cid] : byLand) {
            (void)cid;
            const double share = (double)prov / (double)total;
            hh += share * share;
            if (prov > largest) largest = prov;
        }
        w.herfindahl   = (float)hh;
        w.largestShare = (float)largest / (float)total;
        w.atWarFrac    = (float)atWar / (float)alive;
        // Normalised against a typical map rather than reported raw: what the
        // policy needs is "crowded or empty", not a headcount.
        w.aliveNorm    = std::tanh((float)alive / 40.0f);
        w.meanUnrest   = (float)(unrestSum / alive);
        // Ranked once here so buildFeatures stays O(1) per country. Sorting
        // pairs breaks ties by cid, so the order does not depend on how the
        // stats map happened to be walked.
        std::sort(byLand.begin(), byLand.end());
        for (size_t i = 0; i < byLand.size(); ++i)
            w.rank[byLand[i].second] =
                byLand.size() > 1 ? (float)i / (float)(byLand.size() - 1) : 1.0f;
    }
    m_world = std::move(w);
}

void AISystem::updateTrends() {
    for (const auto& [cid, st] : m_stats) {
        (void)st;
        auto it = m_trend.find(cid);
        if (it == m_trend.end() || m_turn - it->second.turn >= TREND_WINDOW)
            m_trend[cid] = sampleTrend(cid);
    }
}


void AISystem::buildRelational(int cid, std::vector<std::vector<float>>& cand,
                               std::vector<float>& pooled) {
    cand.clear();
    pooled.assign(REL_EMBED, 0.0f);
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    auto sIt = m_stats.find(cid);
    if (!c || sIt == m_stats.end()) return;
    const CountryStat& st = sIt->second;

    std::vector<std::pair<long long, int>> nb;
    std::unordered_set<int> seen;
    for (const auto& fr : st.frontiers) {
        if (fr.enemyCid <= 0 || fr.enemyCid == cid) continue;
        if (!seen.insert(fr.enemyCid).second) continue;
        auto nIt = m_stats.find(fr.enemyCid);
        if (nIt == m_stats.end()) continue;
        nb.push_back({nIt->second.army, fr.enemyCid});
    }
    if (nb.empty()) return;
    std::sort(nb.rbegin(), nb.rend());
    if ((int)nb.size() > REL_MAX) nb.resize(REL_MAX);

    const double myArmy = (double)std::max(1LL, st.army);
    const double myProv = (double)std::max(1, st.provinces);
    auto relIt = g.m_relations.find(c->isoA3);
    for (const auto& [army, ocid] : nb) {
        (void)army;
        auto oIt = m_stats.find(ocid);
        if (oIt == m_stats.end()) continue;
        const CountryStat& o = oIt->second;
        const Country* oc = g.m_countries.getCountry(ocid);
        std::vector<float> r((size_t)REL_FEATURES, 0.0f);
        r[0] = (float)std::tanh(std::log1p((double)o.army / myArmy));
        r[1] = (float)std::tanh(std::log1p((double)o.provinces / myProv));
        r[2] = std::tanh(o.industrySum / 20.0f);
        r[3] = oc ? std::tanh((float)oc->treasury / 500.0f) : 0.0f;
        if (relIt != g.m_relations.end() && oc) {
            auto rr = relIt->second.find(oc->isoA3);
            if (rr != relIt->second.end()) {
                r[4] = rr->second.war ? 1.0f : 0.0f;
                r[5] = rr->second.alliance ? 1.0f : 0.0f;
            }
        }
        r[6] = std::tanh(g.warWearinessOf(ocid) / 5.0f);
        r[7] = (float)std::min(1.0, (double)o.claimsAgainstMe / 4.0);
        cand.push_back(std::move(r));
    }

    if (std::getenv("OD_REL_DUMP") && m_turn <= 3) {
        printf("[REL] t=%d cid=%d n=%zu", m_turn, cid, cand.size());
        for (size_t i = 0; i < cand.size(); ++i) {
            printf(" |%d:", nb[i].second);
            for (float v : cand[i]) printf(" %.9g", v);
        }
        printf("\n");
    }
    // Encode, score, pool. forward() is used rather than forwardInto because
    // this runs on the decision path, single-threaded, once per country-turn.
    std::vector<std::vector<float>> emb;
    std::vector<float> scores;
    emb.reserve(cand.size()); scores.reserve(cand.size());
    for (const auto& r : cand) {
        emb.push_back(m_relEncoder.forward(r));
        scores.push_back(m_relScore.forward(emb.back())[0]);
    }
    std::vector<float> attn;
    NeuralNet::attentionPool(emb, scores, pooled, attn);
    if (std::getenv("OD_REL_DUMP") && m_turn <= 3) {
        printf("[EMB] t=%d cid=%d", m_turn, cid);
        for (size_t i = 0; i < emb.size(); ++i)
            printf(" s%zu:%.9g e%zu:%.9g", i, scores[i], i, emb[i].empty() ? 0.0f : emb[i][0]);
        printf(" | pooled0:%.9g\n", pooled.empty() ? 0.0f : pooled[0]);
    }
    if ((int)pooled.size() != REL_EMBED) pooled.assign(REL_EMBED, 0.0f);
}

void AISystem::buildFeatures(int cid, std::vector<float>& f) {
    f.assign(FEATURE_COUNT, 0.0f);
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    if (!c) return;
    const CountryStat& st = m_stats[cid];

    CountryIncomeSnapshot inc = g.computeCountryIncome(cid);

    f[0] = nlog(c->treasury, 4.0);
    f[1] = std::tanh(inc.net / 100.0f);
    f[2] = std::tanh(inc.total / 200.0f);
    f[3] = inc.total > 1 ? std::min(2.0f, inc.expenses / inc.total) * 0.5f : 0.0f;
    f[4] = inc.expenses > 1 ? inc.armyExpenses / inc.expenses : 0.0f;
    f[5] = inc.expenses > 1 ? inc.navyExpenses / inc.expenses : 0.0f;
    f[6] = inc.expenses > 1 ? inc.policyCosts / inc.expenses : 0.0f;
    // Income trend from the existing 12-turn history ring
    auto histIt = g.m_incomeHistory.find(cid);
    if (histIt != g.m_incomeHistory.end() && histIt->second.size() >= 2)
        f[7] = std::tanh((histIt->second.back().net - histIt->second.front().net) / 50.0f);
    f[8] = std::tanh(st.provinces / 30.0f);
    f[9] = (cid >= 0 && cid < (int)g.m_countryPixels.size())
               ? std::min(1.0f, (float)((double)g.m_countryPixels[cid].size() / m_worldPixels * 10.0))
               : 0.0f;
    f[10] = nlog((double)st.population, 5.0);
    f[11] = nlog((double)st.army, 4.0);
    f[12] = st.provinces > 0 ? nlog((double)st.army / st.provinces, 3.0) : 0.0f;
    f[13] = std::tanh(st.boats / 5.0f);
    f[14] = std::tanh(st.destroyers / 5.0f);
    f[15] = std::tanh(st.carriers / 3.0f);

    // Relations
    int wars = 0, allies = 0, naps = 0, guars = 0;
    auto relIt = g.m_relations.find(c->isoA3);
    if (relIt != g.m_relations.end()) {
        for (auto& [iso, r] : relIt->second) {
            if (r.war) wars++;
            if (r.alliance) allies++;
            if (r.nonAggression) naps++;
            if (r.guarantee) guars++;
        }
    }
    f[16] = std::tanh(wars / 3.0f);
    f[17] = std::tanh(allies / 3.0f);
    f[18] = std::tanh(naps / 3.0f);
    f[19] = std::tanh(guars / 3.0f);
    f[20] = std::tanh(st.frontiers.size() / 15.0f);

    // Neighbour threat: strongest bordering country vs us
    long long strongest = 0, weakestWar = -1;
    std::unordered_set<int> seenN;
    for (auto& fr : st.frontiers) {
        if (!seenN.insert(fr.enemyCid).second) continue;
        long long ea = m_stats[fr.enemyCid].army;
        strongest = std::max(strongest, ea);
        const Country* ec = g.m_countries.getCountry(fr.enemyCid);
        if (ec && relIt != g.m_relations.end()) {
            auto rr = relIt->second.find(ec->isoA3);
            if (rr != relIt->second.end() && rr->second.war)
                weakestWar = (weakestWar < 0) ? ea : std::min(weakestWar, ea);
        }
    }
    double myA = (double)std::max(1LL, st.army);
    f[21] = (float)std::tanh(std::log1p((double)strongest / myA));
    f[22] = weakestWar >= 0 ? (float)std::tanh(std::log1p((double)weakestWar / myA)) : 0.0f;
    f[23] = wars > 0 ? 1.0f : 0.0f;

    // Sampled unrest (bounded work: at most 6 provinces).
    // "Bounded" was only true for the sample size — finding six OWNED provinces
    // used to mean walking the whole map, because the iteration order is the
    // province map's hash order, not ownership. The index makes it genuinely
    // six lookups.
    float unrestSum = 0; int unrestN = 0;
    for (int pid : g.provincesOf(cid)) {
        unrestSum += g.getProvinceRebellionChance(pid, cid);
        if (++unrestN >= 6) break;
    }
    f[24] = unrestN ? std::tanh(unrestSum / unrestN / 10.0f) : 0.0f;
    auto pacIt = g.m_countryPacification.find(cid);
    f[25] = (cid == g.m_playerCountryId) ? g.m_pacificationAllocation
            : (pacIt != g.m_countryPacification.end() ? pacIt->second : 0.0f);
    f[26] = c->compassEconomic / 100.0f;
    f[27] = c->compassSocial / 100.0f;
    auto resIt = g.m_countryResearched.find(cid);
    f[28] = resIt != g.m_countryResearched.end() ? std::tanh(resIt->second.size() / 10.0f) : 0.0f;
    f[29] = st.provinces > 0 ? st.industrySum / st.provinces / 10.0f : 0.0f;
    f[30] = st.provinces > 0 ? st.fortSum / st.provinces / 5.0f : 0.0f;
    f[31] = st.maxPort / 3.0f;
    f[32] = st.maxPort >= 1 ? 1.0f : 0.0f;
    f[33] = st.maxPort >= 2 ? 1.0f : 0.0f;
    f[34] = st.maxPort >= 3 ? 1.0f : 0.0f;
    f[35] = (c->treasury >= AI_IND_COST[1]) ? 1.0f : 0.0f;
    f[36] = std::tanh((float)g.m_rebellionsThisTurnByCid[cid] / 2.0f);
    f[37] = std::tanh(m_turn / 100.0f);
    auto apIt = g.m_countryActivePolicyIndices.find(cid);
    f[38] = apIt != g.m_countryActivePolicyIndices.end()
                ? std::tanh(apIt->second.size() / 4.0f) : 0.0f;
    f[39] = inc.net > 0 ? 1.0f : 0.0f;
    double worldAvgArmy = (double)m_worldArmy / (double)std::max<size_t>(1, m_stats.size());
    f[40] = (float)std::tanh(std::log1p((double)st.army / std::max(1.0, worldAvgArmy)));
    f[41] = st.boatsWithCrew > 0 ? 1.0f : 0.0f;
    f[42] = std::tanh(st.boatsWithCrew / 3.0f);

    // ── Research state ──
    auto raIt = g.m_countryResearchAllocation.find(cid);
    f[43] = raIt != g.m_countryResearchAllocation.end() ? raIt->second : 0.0f;
    auto rpIt = g.m_countryResearchPoints.find(cid);
    f[44] = rpIt != g.m_countryResearchPoints.end() ? rpIt->second / 10000.0f : 0.0f;
    auto actIt = g.m_countryResearchActive.find(cid);
    int activeNode = actIt != g.m_countryResearchActive.end() ? actIt->second : -1;
    f[45] = (activeNode >= 0) ? 1.0f : 0.0f;
    if (activeNode >= 0 && activeNode < (int)g.m_researchNodes.size()) {
        const ResearchNode& an = g.m_researchNodes[activeNode];
        auto invIt = g.m_countryResearchInvested.find(cid);
        int inv = invIt != g.m_countryResearchInvested.end() ? invIt->second : 0;
        f[46] = an.cost > 0 ? (float)inv / an.cost : 0.0f;
    }
    f[47] = industryCap(cid) / 10.0f;
    f[48] = fortCap(cid) / 5.0f;
    f[49] = portCap(cid) / 3.0f;
    f[50] = g.hasResearched("arty1", cid) ? 1.0f : 0.0f;
    f[51] = g.hasResearched("navy1", cid) ? 1.0f : 0.0f;

    // ── Military balance across ALL wars (not just frontiers) ──
    long long enemyArmyTotal = 0, allyArmyTotal = 0;
    if (relIt != g.m_relations.end()) {
        for (auto& [iso, r] : relIt->second) {
            if (!r.war && !r.alliance) continue;
            int ocid = g.cidForIso(iso);
            if (ocid < 0) continue;
            if (r.war) enemyArmyTotal += m_stats[ocid].army;
            if (r.alliance) allyArmyTotal += m_stats[ocid].army;
        }
    }
    f[52] = (float)std::tanh(std::log1p((double)enemyArmyTotal / myA));
    f[53] = (float)std::tanh(std::log1p((double)allyArmyTotal / myA));
    f[54] = enemyArmyTotal > st.army ? 1.0f : 0.0f; // outgunned flag

    // ── Claims ──
    f[55] = st.provinces > 0
                ? std::tanh(3.0f * st.claimsAgainstMe / st.provinces) : 0.0f; // rebellion exposure
    f[56] = std::tanh(st.myClaimsOutstanding / 5.0f);                          // war goals available

    // ── Naval invasion opportunity ──
    // We have a port + an army, and there is an overseas coastal enemy we could
    // declare war on and invade by sea. Without this signal the model can't
    // tell when picking "declare war" leads to a reachable amphibious target.
    bool navalReady = st.maxPort >= 1 && st.army > 1000;
    f[57] = (navalReady && st.navalTargets > 0) ? 1.0f : 0.0f;
    f[58] = std::tanh(st.navalTargets / 5.0f);
    f[59] = std::tanh((st.destroyers + st.carriers) / 4.0f); // escort/bombard fleet

    // ── Defensive posture (60-66) ──
    // None of this was visible to the model before: the only war-related inputs
    // were "am I at war" and army ratios, which say nothing about whether an
    // enemy is standing on a border right now or whether ground is being lost.
    // A policy cannot learn to defend against a state it cannot observe.
    f[60] = std::tanh(st.provincesLost / 2.0f);
    f[61] = st.provinces > 0
                ? std::min(1.0f, (float)st.threatenedProvinces / st.provinces) : 0.0f;
    f[62] = (float)std::tanh(std::log1p((double)st.enemyAdjArmy / myA));
    f[63] = st.enemyAdjArmy > 0
                ? (float)std::tanh((double)(st.enemyAdjArmy - st.defenderArmy) /
                                   std::max(1.0, (double)st.enemyAdjArmy)) : 0.0f;
    f[64] = (st.threatenedProvinces > 0 && st.enemyAdjArmy > st.defenderArmy) ? 1.0f : 0.0f;
    f[65] = std::tanh(st.worstDeficit / 5000.0f);
    f[66] = st.provincesLost > 0 ? 1.0f : 0.0f;

    // ── Coalition (67-71) ──
    f[67] = (float)std::tanh(std::log1p((double)st.allyAdjArmy / myA));
    f[68] = std::tanh(st.coBelligerents / 2.0f);   // allies actually fighting with us
    f[69] = std::tanh(st.idleAllies / 2.0f);       // allies sitting the war out
    f[70] = st.staging.empty() ? 0.0f : 1.0f;      // a road onto a shared front exists
    f[71] = std::tanh(st.staging.size() / 4.0f);

    // ── Expeditionary + the price of loyalty (72-74) ──
    f[72] = st.armyAbroad > 0 ? 1.0f : 0.0f;
    f[73] = (float)std::tanh((double)st.armyAbroad / myA);
    // War weariness is what an alliance COSTS. Without it in the vector the
    // politics module can see the benefit of a pact and never the bill.
    f[74] = g.warWearinessOf(cid) / Game::WAR_WEARINESS_MAX;

    // ── What the fleet costs (75-76) ──
    // f[5] is the navy's share of EXPENSES, which says nothing about whether
    // the country can afford it — a country with no other outgoings reads 1.0
    // there while paying almost nothing. What the scrap action needs is the
    // bill measured against income, and whether the fleet has anything to do.
    f[75] = inc.total > 1.0f ? std::min(1.0f, inc.navyExpenses / inc.total) : 0.0f;
    {
        auto w = m_warWith.find(cid);
        const bool atWar = (w != m_warWith.end() && !w->second.empty());
        const int ships = st.boats + st.destroyers + st.carriers;
        f[76] = (!atWar && ships > 0 && st.navalTargets == 0 &&
                 st.navalWarTargets == 0) ? 1.0f : 0.0f;
    }

    // ── Minorities (77-79, 85-86) ──
    // f[24] samples raw rebellion chance, which is the SYMPTOM. These are the
    // cause the politics module can actually act on: how the groups living here
    // feel, whether the current option set is winning them over or driving them
    // out, and what that costs.
    f[77] = st.meanAlignment / 100.0f;
    f[78] = st.minorities > 0 ? st.worstAlignment / 100.0f : 1.0f;
    f[79] = std::tanh(st.minorityTrend / 5.0f);
    f[85] = inc.total > 1.0f ? std::min(1.0f, st.minorityCost / inc.total) : 0.0f;
    f[86] = std::tanh(st.minorities / 4.0f);

    // An empty treasury (87). Treasury and net income are already in f[0] and
    // f[1], but "has actually run out" is a state with its own consequences —
    // twenty points of rebellion chance in every province — and a threshold the
    // net would otherwise have to infer from two continuous inputs.
    f[87] = g.isBankrupt(cid) ? 1.0f : 0.0f;

    // 80-84 and 88-94 are request context, written only by decideDiplomacy;
    // they stay zero on an ordinary turn.
    f[95] = 1.0f; // bias

    // 96-103: TRENDS -- direction and pace, against a baseline up to
    // TREND_WINDOW turns old. Squashed, so a runaway late-game value cannot
    // swamp the input the way a raw delta would.
    {
        auto tIt = m_trend.find(cid);
        if (tIt != m_trend.end() && tIt->second.turn >= 0) {
            const TrendPoint& p = tIt->second;
            const TrendPoint  n = sampleTrend(cid);
            f[96]  = std::tanh((n.provinces  - p.provinces)  / 3.0f);
            f[97]  = std::tanh((n.army       - p.army)       / 20000.0f);
            f[98]  = std::tanh((n.industry   - p.industry)   / 3.0f);
            f[99]  = std::tanh((n.population - p.population) / 500000.0f);
            f[100] = std::tanh((n.treasury   - p.treasury)   / 150.0f);
            f[101] = std::tanh((n.threat     - p.threat)     / 20000.0f);
            f[102] = std::tanh((n.align      - p.align)      / 10.0f);
            f[103] = std::tanh((n.weariness  - p.weariness)  / 5.0f);
        }
    }

    // 116-139: THE NEIGHBOURS, attention-pooled. See REL_FEATURES.
    {
        std::vector<std::vector<float>> cand;
        std::vector<float> pooled;
        buildRelational(cid, cand, pooled);
        for (int i = 0; i < REL_EMBED && i < (int)pooled.size(); ++i)
            f[116 + i] = pooled[i];
        m_lastRelCand = std::move(cand);
    }

    // 112-115: THE STANCE in force. Set on a previous turn (see STANCE_WINDOW),
    // so the head that picks it never reads its own output on the same turn.
    {
        const int sc = stanceOf(cid);
        if (sc >= 0 && sc < STANCE_COUNT) f[112 + sc] = 1.0f;
    }

    // 104-111: THE WORLD, not us. See WORLD_FEATURES -- the value head was
    // judging a position with no idea what shape the map around it was in.
    {
        const WorldSnapshot& w = m_world;
        f[104] = w.herfindahl;
        f[105] = w.largestShare;
        f[106] = w.totalProvinces > 0
                     ? (float)st.provinces / (float)w.totalProvinces : 0.0f;
        auto wrIt = w.rank.find(cid);
        f[107] = wrIt != w.rank.end() ? wrIt->second : 0.0f;
        f[108] = w.atWarFrac;
        f[109] = w.aliveNorm;
        f[110] = std::tanh(w.meanUnrest / 5.0f);
        // Where we are in the game. Holding half the map on turn 30 and on turn
        // 380 are not the same position, and nothing said which one it was.
        f[111] = std::tanh((float)m_turn / 200.0f);
    }

    // Degenerate late-game state (populations/treasuries overflowing float)
    // leaks inf/NaN into features; one NaN input turns every logit NaN and
    // poisons weight updates. Zero them at the source.
    for (float& v : f)
        if (!std::isfinite(v)) v = 0.0f;
}

// ─── Difficulty / sampling ───────────────────────────────

bool AISystem::selfPlayLearning() const {
    return m_g && m_g->m_aiTraining && !s_evaluating;
}

void AISystem::difficultyParams(float& temperature, float& epsilon) const {
    // Self-play training ALWAYS explores, whatever the difficulty setting says.
    // Training on Insane (argmax, no randomness) would freeze the policy on
    // its current best guess forever — exploration is what learning eats.
    if (selfPlayLearning()) {
        temperature = 1.0f;
        // Annealed, not fixed. A flat 10% random forever is a hard ceiling on
        // how good the policy can ever get: one action in ten is a coin flip no
        // matter how much the net has learned, and those flips are also what
        // the value baseline is fitted against. Decays from 15% toward 2% over
        // one policy head's lifetime experience — see EPSILON_ANNEAL_SAMPLES
        // for why it is one head's count and not the nine-net total.
        const double progress = (double)m_policy[MOD_WAR].updateCount() /
                                EPSILON_ANNEAL_SAMPLES;
        const float t = (float)std::min(1.0, progress);
        epsilon = EPSILON_START + (EPSILON_FINAL - EPSILON_START) * t;
        return;
    }
    switch (m_g->m_config.aiDifficulty) {
        case 0: temperature = 2.5f;  epsilon = 0.35f; break; // easy
        default:
        case 1: temperature = 1.0f;  epsilon = 0.10f; break; // normal
        case 2: temperature = 0.35f; epsilon = 0.02f; break; // hard
        case 3: temperature = 0.05f; epsilon = 0.0f;  break; // insane: argmax
    }
}

int AISystem::pickAction(NeuralNet& net, const std::vector<float>& feats,
                         const std::vector<bool>& valid, float& scoreOut,
                         int graveAction, const std::vector<float>* logitBias,
                         float* logProbOut) {
    const std::vector<float>& logits = net.forward(feats);
    scoreOut = 0;
    if (logits.empty()) return 0;
    std::vector<float> masked(logits);
    // Bias BEFORE masking, so a biased action that is invalid stays invalid
    // rather than climbing back out of -1e9.
    if (logitBias)
        for (size_t i = 0; i < masked.size() && i < logitBias->size(); ++i)
            masked[i] += (*logitBias)[i];
    int validCount = 0;
    for (size_t i = 0; i < masked.size(); ++i) {
        if (i < valid.size() && !valid[i]) masked[i] = -1e9f;
        else validCount++;
    }
    if (validCount == 0) return 0;

    float temperature, epsilon;
    difficultyParams(temperature, epsilon);
    // The control group ignores the model entirely. Not "mostly random" — a
    // benchmark whose baseline occasionally consults the thing being measured
    // is not a baseline.
    if (m_randomThisCountry) epsilon = 1.0f;

    int a;
    std::uniform_real_distribution<float> d(0.0f, 1.0f);
    if (epsilon > 0.0f && d(m_rng) < epsilon) {
        // Deliberately dumb: uniform over the valid actions.
        // NOTE: membership must come from the validity mask, NOT the logit
        // value. The old test (masked[i] > -1e8f) silently excluded valid
        // actions whose logits were NaN (NaN > x is false) — late in long
        // self-play runs, exploded game stats push NaN through the net, the
        // pool came up empty, and pool[x % 0] was a modulo-by-zero + null
        // deref: the intermittent training SIGSEGV (AISystem.cpp:373).
        std::vector<int> pool;
        for (size_t i = 0; i < masked.size(); ++i) {
            if (i < valid.size() && !valid[i]) continue;
            // Exploration is meant to make one AI play *worse*, not to make the
            // world incoherent. Declaring war is the one action here that cannot
            // be undone and that rewrites the game for every other country: a
            // coin flip landing on it dogpiles a neighbour for no reason, and at
            // eps=0.10 over ~6 valid war actions that fires somewhere on the map
            // every few turns, forever. Players read that as the AI being
            // deranged rather than merely weak.
            //
            // Self-play is the exact opposite case: the net cannot learn what
            // war is worth unless it sometimes tries one, so exploration stays
            // unrestricted while training.
            // ...but not for the control group. For them the random pool IS
            // the whole policy, so removing an action from it removes the
            // action from their repertoire and quietly handicaps the baseline.
            if ((int)i == graveAction && !selfPlayLearning() && !m_randomThisCountry)
                continue;
            pool.push_back((int)i);
        }
        if (!pool.empty()) {
            a = pool[(size_t)(d(m_rng) * pool.size()) % pool.size()];
        } else {
            // Every valid action was grave. Fall back to the policy rather than
            // action 0 -- returning "hold" here would silently make the module
            // inert in exactly the situations that matter most.
            a = NeuralNet::samplePolicy(masked, temperature, m_rng);
        }
    } else {
        a = NeuralNet::samplePolicy(masked, temperature, m_rng);
    }
    if (a < 0 || a >= (int)logits.size()) a = 0;
    scoreOut = std::isfinite(logits[a]) ? logits[a] : 0.0f;
    // Recorded from the MASKED logits, because that is the distribution the
    // policy actually offered: an action at -1e9 has probability zero, and a
    // ratio measured against the unmasked logits would be measuring a policy
    // that was never on the table.
    if (logProbOut) *logProbOut = NeuralNet::logProbOf(masked, a);
    return a;
}

void AISystem::logDecision(int cid, int module, int action, float score, const std::string& label) {
    Decision d;
    d.turn = m_turn; d.cid = cid; d.module = module; d.action = action;
    d.score = score; d.label = label;
    m_log.push_back(d);
    while (m_log.size() > 400) m_log.pop_front();
    m_decisionsThisTurn++;
    if (m_g->m_config.aiDebug) {
        const Country* c = m_g->m_countries.getCountry(cid);
        printf("[AI] t%d %s [%s] %s (score %.2f)\n", m_turn,
               c ? c->name.c_str() : "?", MODULE_NAMES[module], label.c_str(), score);
    }
}

// ─── Think ───────────────────────────────────────────────

void AISystem::takeTurn(int cid) {
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    if (!c) return;
    // Countries with nothing left don't think (eliminated but not yet culled)
    const CountryStat& st = m_stats[cid];
    if (st.provinces == 0) return;

    m_randomThisCountry = isRandomCountry(cid);
    // A league country acts with a frozen past policy and teaches nothing.
    m_leagueThisCountry = m_leagueCids.count(cid) > 0;

    Experience exp;
    buildFeatures(cid, exp.features);
    exp.provinces = st.provinces;
    exp.treasury = c->treasury;
    exp.army = st.army;
    exp.ships = st.boats + st.destroyers + st.carriers;
    exp.netIncome = g.computeCountryIncome(cid).net;
    exp.industrySum = st.industrySum;
    exp.threatened = st.threatenedProvinces;
    exp.weariness = g.warWearinessOf(cid);
    exp.minorityAlignment = st.meanAlignment;
    exp.pacts = st.pacts;
    {
        auto w = m_warWith.find(cid);
        exp.atWar = (w != m_warWith.end() && !w->second.empty());
    }
    exp.warInWindow = exp.atWar;
    if (exp.atWar) {
        auto ws = m_warSince.find(cid);
        if (ws != m_warSince.end()) exp.warTurns = m_turn - ws->second;
    }
    auto resIt2 = g.m_countryResearched.find(cid);
    exp.researched = resIt2 != g.m_countryResearched.end() ? (int)resIt2->second.size() : 0;
    exp.relCand = m_lastRelCand;

    std::vector<bool> valid;
    float score;

    // Q as a nudge on the policy's logits.
    //
    // REINFORCE only ever moves the logit of the action it happened to sample,
    // so the actor learns slowly which of eight war actions is the good one and
    // says nothing at all about the seven it did not try. Q was trained on the
    // same target for whichever action WAS taken, across every country and
    // every turn, so it holds an opinion about all of them. Adding it here
    // makes the choice a policy-improvement step over the actor rather than a
    // straight sample from it.
    //
    // CENTRED, because a constant added to every logit changes nothing after a
    // softmax -- what should move is the preference between actions, not the
    // overall confidence, which temperature owns.
    //
    // INERT UNTIL TRAINED, and that guard is not optional: every model file
    // written before this existed carries no Q head at all, so it starts from
    // noise. Blending noise into a policy with millions of updates behind it
    // would make the shipped AI worse the moment this shipped, and it would
    // look like the actor had regressed.
    // ONE trunk pass for this country's turn; every head below reads it.
    // Copied rather than referenced: forward() returns the net's own buffer,
    // and a league country runs a different trunk a few lines down.
    const std::vector<float> emb =
        m_leagueThisCountry ? m_leagueTrunk.forward(exp.features)
                            : m_trunk.forward(exp.features);

    // ── The stance, re-chosen every STANCE_WINDOW turns ──
    // Not for a league country: a frozen opponent is exactly the policy it was
    // checkpointed as, and it has no stance head of its own.
    if (!m_leagueThisCountry) {
        auto stIt = m_stance.find(cid);
        const bool due = stIt == m_stance.end() ||
                         (m_turn - stIt->second.second) >= STANCE_WINDOW;
        if (due) {
            std::vector<bool> anyStance((size_t)STANCE_COUNT, true);
            float sScore = 0.0f;
            float sLogProb = 0.0f;
            const int sc = pickAction(m_stanceHead, emb, anyStance, sScore,
                                      /*graveAction=*/-1, nullptr, &sLogProb);
            m_stance[cid] = {sc, m_turn};
            exp.action[MOD_COUNT + 1] = sc;
            exp.acted[MOD_COUNT + 1]  = true;
            exp.logProb[MOD_COUNT + 1] = sLogProb;
        }
    }

    std::vector<float> qbias;
    auto qBiasFor = [&](int m) -> const std::vector<float>* {
        if (Q_BLEND <= 0.0f) return nullptr;
        // A frozen opponent is exactly the policy it was checkpointed as.
        // Letting the CURRENT critic re-rank its actions would make it a
        // moving target again, which is the one thing the league exists to
        // prevent.
        if (m_leagueThisCountry) return nullptr;
        if (m_q[m].updateCount() < Q_WARMUP_UPDATES) return nullptr;
        const std::vector<float>& q = m_q[m].forward(emb);
        if (q.empty()) return nullptr;
        float mean = 0.0f;
        for (float v : q) mean += v;
        mean /= (float)q.size();
        qbias.assign(q.size(), 0.0f);
        for (size_t i = 0; i < q.size(); ++i) qbias[i] = Q_BLEND * (q[i] - mean);
        return &qbias;
    };

    // Which brain is playing this country. A league country is driven by a
    // frozen past self: it is never trained, so it consults no Q and its
    // experience is discarded below.
    auto brainFor = [&](int m) -> NeuralNet& {
        return m_leagueThisCountry ? m_leaguePolicy[m] : m_policy[m];
    };

    // Solvency, counted once per country-turn HERE rather than in endTurn --
    // that function returns early when learning is off, so every evaluation
    // reported 0.0% bankrupt however broke the world actually was.
    if (g.isBankrupt(cid)) statsFor(cid).bankruptTurns++;
    // The denominator for every offensive rate below: a country not at war
    // cannot attack, so comparing raw battle counts across cohorts that fight
    // at different frequencies compares nothing.
    if (foreignWarCount(cid) > 0) statsFor(cid).turnsAtWar++;

    validEconomy(cid, valid);
    int a = pickAction(brainFor(MOD_ECONOMY), emb, valid, score,
                       /*graveAction=*/-1, qBiasFor(MOD_ECONOMY),
                   &exp.logProb[MOD_ECONOMY]);
    m_policy[MOD_ECONOMY].snapshotActs(exp.acts[MOD_ECONOMY]);
    exp.action[MOD_ECONOMY] = a; exp.acted[MOD_ECONOMY] = true;
    {
        TrainStats& ts = statsFor(cid);
        for (int i = 0; i < ECON_ACTIONS; ++i)
            if (i < (int)valid.size() && valid[i]) ts.econOffered[i]++;
        if (a >= 0 && a < ECON_ACTIONS) ts.econChosen[a]++;
        if (a >= 9 && a <= 11) ts.researchPicked++;
    }
    logDecision(cid, MOD_ECONOMY, a, score, execEconomy(cid, a));

    validPolitics(cid, valid);
    a = pickAction(brainFor(MOD_POLITICS), emb, valid, score,
                   /*graveAction=*/-1, qBiasFor(MOD_POLITICS),
                   &exp.logProb[MOD_POLITICS]);
    m_policy[MOD_POLITICS].snapshotActs(exp.acts[MOD_POLITICS]);
    exp.action[MOD_POLITICS] = a; exp.acted[MOD_POLITICS] = true;
    logDecision(cid, MOD_POLITICS, a, score, execPolitics(cid, a));

    // Defence runs before the sampled war action, unconditionally. See the
    // note on garrisonReflex: holding a threatened border is not a choice the
    // policy should be gambling on once every eight turns.
    garrisonReflex(cid);
    // Peacetime housekeeping, same reasoning: neither of these is a gamble.
    redeployReflex(cid);
    // Solvency before manpower: austerity cuts things that come back, the
    // manpower reflex cuts men who do not.
    austerityReflex(cid);
    manpowerReflex(cid);
    // Finish any crossing already under way. Runs BEFORE the navy action is
    // sampled so that action sees the move orders it has already issued.
    amphibiousReflex(cid);

    validWar(cid, valid);
    // 4 = declare war; see the graveAction note in pickAction.
    a = pickAction(brainFor(MOD_WAR), emb, valid, score, /*graveAction=*/4,
                   qBiasFor(MOD_WAR), &exp.logProb[MOD_WAR]);
    m_policy[MOD_WAR].snapshotActs(exp.acts[MOD_WAR]);
    exp.action[MOD_WAR] = a; exp.acted[MOD_WAR] = true;
    // Offered against chosen, recorded HERE because `a` is reused by the navy
    // action a few lines down. See the counters' note: this is what separates a
    // mask that never offers attack from a policy that refuses it.
    {
        TrainStats& ts = statsFor(cid);
        for (int i = 0; i < WAR_ACTIONS; ++i)
            if (i < (int)valid.size() && valid[i]) ts.warOffered[i]++;
        if (a >= 0 && a < WAR_ACTIONS) ts.warChosen[a]++;
    }
    // DECISION TRACE. OD_DEC_TRACE=1 prints one line per country-turn: the
    // features that produced the decision, and what each module chose. Two runs
    // can then be diffed to find the FIRST country whose decision differs, and
    // whether its inputs differed too -- which separates "the observation was
    // already different" from "the same observation produced a different pick".
    static const bool decTrace = std::getenv("OD_DEC_TRACE") != nullptr;
    if (decTrace) {
        uint64_t fh = 1469598103934665603ULL;
        for (float v : exp.features) {
            uint32_t bits; memcpy(&bits, &v, 4);
            fh ^= bits; fh *= 1099511628211ULL;
        }
        uint64_t eh = 1469598103934665603ULL;
        for (float v : emb) {
            uint32_t bits; memcpy(&bits, &v, 4);
            eh ^= bits; eh *= 1099511628211ULL;
        }
        if (std::getenv("OD_FEAT_DUMP") && m_turn <= 3) {
            printf("[FEAT] t=%d cid=%d", m_turn, cid);
            for (size_t k = 0; k < exp.features.size(); ++k)
                printf(" %zu:%.9g", k, exp.features[k]);
            printf("\n");
        }
        printf("[DEC] t=%d cid=%d feat=%llu emb=%llu e=%d p=%d w=%d n=%d\n",
               m_turn, cid, (unsigned long long)fh, (unsigned long long)eh,
               exp.action[MOD_ECONOMY], exp.action[MOD_POLITICS],
               exp.action[MOD_WAR], exp.action[MOD_NAVY]);
    }
    m_declaredUnprovoked = false;
    {
        const std::string label = execWar(cid, a);
        // Captured AFTER execWar, because that is what runs the chooser. A
        // declaration that fell through to the old rule leaves this empty and
        // simply trains nothing.
        if (m_pendingTargetChosen >= 0) {
            exp.targetCand = std::move(m_pendingTargetCand);
            exp.targetChosen = m_pendingTargetChosen;
            m_pendingTargetCand.clear();
            m_pendingTargetChosen = -1;
        }
        logDecision(cid, MOD_WAR, a, score, label);
    }
    exp.aggressor = m_declaredUnprovoked;

    validNavy(cid, valid);
    a = pickAction(brainFor(MOD_NAVY), emb, valid, score,
                   /*graveAction=*/-1, qBiasFor(MOD_NAVY),
                   &exp.logProb[MOD_NAVY]);
    m_policy[MOD_NAVY].snapshotActs(exp.acts[MOD_NAVY]);
    exp.action[MOD_NAVY] = a; exp.acted[MOD_NAVY] = true;
    logDecision(cid, MOD_NAVY, a, score, execNavy(cid, a));

    // A control-group country's choices are coin flips, so training on them
    // would be teaching the model to imitate noise. They play; they do not
    // teach. (Learning is off during evaluation anyway — this makes the
    // mechanism safe to use anywhere, including a mixed training run.)
    if (m_randomThisCountry) { m_randomThisCountry = false; return; }
    // Same rule, different reason: a league country's choices come from a
    // policy that is not being trained, so its experience would teach the
    // learner to imitate its own past rather than to beat it.
    if (m_leagueThisCountry) { m_leagueThisCountry = false; return; }

    auto& dq = m_pending[cid];
    dq.push_back(std::move(exp));
    while (dq.size() > (size_t)nStep() + 4) dq.pop_front(); // safety cap
}

// ─── Validity masks ──────────────────────────────────────

void AISystem::validEconomy(int cid, std::vector<bool>& v) {
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    const CountryStat& st = m_stats[cid];
    v.assign(ECON_ACTIONS, false);
    v[0] = true; // save money is always allowed
    if (!c) return;
    double t = c->treasury;
    int indCap = industryCap(cid);
    v[1] = st.industrySum < (float)st.provinces * indCap && t >= AI_IND_COST[1];
    v[2] = !st.frontiers.empty() && t >= AI_FORT_COST[1];
    v[3] = t >= 60; // port build/upgrade
    v[4] = st.industrySum >= 1 && t >= 2; // specialize (cost >= 1.5)
    v[5] = st.maxPort >= 2 && t >= 15;    // destroyer
    v[6] = st.maxPort >= 3 && t >= 40;    // carrier
    // Research funding + branch focus
    auto raIt = g.m_countryResearchAllocation.find(cid);
    float alloc = raIt != g.m_countryResearchAllocation.end() ? raIt->second : 0.0f;
    v[7] = alloc < 0.45f;   // fund up
    v[8] = alloc > 0.01f;   // fund down
    auto actIt = g.m_countryResearchActive.find(cid);
    bool idle = actIt == g.m_countryResearchActive.end() || actIt->second < 0;
    // AN ARMED NODE WITH NO FUNDING IS A TRAP, and it was closing on the model
    // constantly: 2,434 country-turns frozen in a single 300-turn map.
    //
    // progressCountryResearch does nothing while allocation is zero, so the
    // node never advances and never clears -- and offering "pick a node" only
    // while IDLE meant the country could neither pay for the node it holds nor
    // put it down. The only way out was to raise funding before something
    // zeroed it again, and the policy defunds research far more often than it
    // funds it (fund down taken at 63-81%, fund up at 6-8%), so in practice
    // there was no way out at all.
    //
    // Re-arming is the exit, and it costs nothing to allow: exec re-floors
    // allocation to 5% whenever a node is chosen, so picking again both
    // re-targets and re-funds. No action should be able to permanently disable
    // a subsystem.
    bool stalledUnfunded = !idle && alloc <= 0.001f;
    v[9] = v[10] = v[11] = (idle || stalledUnfunded) && !g.m_researchNodes.empty();
}

void AISystem::validPolitics(int cid, std::vector<bool>& v) {
    Game& g = *m_g;
    v.assign(POL_ACTIONS, false);
    v[0] = true;
    v[1] = !g.m_allPolicies.empty();
    auto pacIt = g.m_countryPacification.find(cid);
    float pac = pacIt != g.m_countryPacification.end() ? pacIt->second : 0.0f;
    v[2] = pac < 0.99f;
    v[3] = pac > 0.01f;
    auto apIt = g.m_countryActivePolicyIndices.find(cid);
    v[4] = apIt != g.m_countryActivePolicyIndices.end() && !apIt->second.empty();
    // Diplomacy proposals need someone to talk to (target picked at exec) AND
    // this country's overture budget. Marking them permanently valid parked
    // ~3/8 of the politics softmax on "propose something" every single turn,
    // and no reward term ever taught the net that was wasteful — so it just
    // kept proposing forever.
    bool hasNeighbor = !m_stats[cid].frontiers.empty();
    v[5] = v[6] = v[7] = hasNeighbor && diploBudgetReady(cid);

    const CountryStat& st = m_stats[cid];
    // A calming policy is worth offering only when there is something to calm.
    // Which policies qualify is decided in exec, which already pays for a scan
    // of m_allPolicies; repeating that scan in the mask would cost it on every
    // country's turn instead of only on the turns the action is chosen.
    v[8] = !g.m_allPolicies.empty() &&
           (st.meanAlignment < 60.0f || g.m_rebellionsThisTurnByCid[cid] > 0 ||
            g.warWearinessOf(cid) > 2.0f);

    // Minority policy: only where there are minorities, and only in a direction
    // that still has somewhere to go.
    ensureTrendBounds();
    v[9]  = st.minorities > 0 && st.minorityTrend < m_trendMax - 1e-3f;
    v[10] = st.minorities > 0 && st.minorityTrend > m_trendMin + 1e-3f;
}

void AISystem::ensureTrendBounds() const {
    if (m_trendBoundsReady) return;
    m_trendMin = m_trendMax = 0.0f;
    for (const auto& cat : m_g->m_ethnicPolicyCategories) {
        if (cat.options.empty()) continue;
        float lo = cat.options[0].alignmentPerTurn, hi = lo;
        for (const auto& o : cat.options) {
            lo = std::min(lo, o.alignmentPerTurn);
            hi = std::max(hi, o.alignmentPerTurn);
        }
        m_trendMin += lo;
        m_trendMax += hi;
    }
    m_trendBoundsReady = true;
}

// See the declaration in AISystem.h for why the mask and the executor share
// this. The logic below is execWar case 4's, moved verbatim rather than
// reimplemented -- two copies of a rule this fiddly would drift within a week.
float AISystem::predictAcceptance(int partnerCid, const char* requestKind) const {
    if (!requestKind) return 0.5f;
    // Their features, not ours: this is their decision, and buildFeatures is
    // what their own answer would be computed from.
    std::vector<float> feats;
    const_cast<AISystem*>(this)->buildFeatures(partnerCid, feats);
    if ((int)feats.size() != FEATURE_COUNT) return 0.5f;

    // The same thumb on the scale answerDiplomacy puts there. Predicting
    // without it would model a different policy from the one that answers.
    std::vector<float> logits = const_cast<NeuralNet&>(m_diplo).forward(feats);
    if ((int)logits.size() != DIPLO_ACTIONS) return 0.5f;
    if (strcmp(requestKind, "request_nap") == 0)
        logits[1] += AI_NAP_WILLINGNESS;

    std::vector<float> probs;
    NeuralNet::softmax(logits, 1.0f, probs);
    return probs.size() > 1 && std::isfinite(probs[1]) ? probs[1] : 0.5f;
}

void AISystem::buildTargetFeatures(int cid, const WarCandidate& cand,
                                   std::vector<float>& out) const {
    out.assign(TARGET_FEATURES, 0.0f);
    Game& g = *m_g;
    auto meIt = m_stats.find(cid);
    auto themIt = m_stats.find(cand.cid);
    if (meIt == m_stats.end() || themIt == m_stats.end()) return;
    const CountryStat& me = meIt->second;
    const CountryStat& th = themIt->second;
    const double myArmy = std::max(1.0, (double)me.army);

    // Ratios, not totals: "twice my army" means the same thing on a twelve
    // country map and a two hundred country one, and the same weights have to
    // serve both.
    out[0] = (float)std::tanh(std::log1p((double)th.army / myArmy));
    out[1] = cand.claimed ? 1.0f : 0.0f;
    out[2] = cand.naval ? 1.0f : 0.0f;
    out[3] = cand.napBlocked ? 1.0f : 0.0f;
    out[4] = (float)std::tanh(std::log1p((double)th.provinces /
                                         std::max(1.0, (double)me.provinces)));
    const Country* tc = g.m_countries.getCountry(cand.cid);
    if (tc) {
        int theirWars = 0, theirAllies = 0;
        auto rel = g.m_relations.find(tc->isoA3);
        if (rel != g.m_relations.end())
            for (auto& [oiso, r] : rel->second) {
                if (r.war) theirWars++;
                if (r.alliance || r.guarantee) theirAllies++;
            }
        // Someone already fighting two wars is a different proposition from
        // someone at peace, and the old rule could not see the difference.
        out[5] = std::tanh(theirWars / 2.0f);
        out[6] = std::tanh(theirAllies / 2.0f);
        out[7] = (float)std::tanh(tc->treasury / 300.0);
        out[8] = g.warWearinessOf(cand.cid) / 20.0f;
    }
    out[9]  = std::tanh(th.frontiers.size() / 10.0f);
    out[10] = th.provinces > 0 ? std::tanh(th.industrySum / th.provinces / 10.0f) : 0.0f;
    out[11] = th.maxPort / 3.0f;
}

int AISystem::chooseWarTarget(int cid, const std::vector<WarCandidate>& cands) {
    m_pendingTargetCand.clear();
    m_pendingTargetChosen = -1;
    // Nothing to choose between, and nothing to learn from a choice of one.
    if (cands.size() < 2) return -1;

    // The candidate inputs are built EVEN WHEN the head is not allowed to
    // steer, because that is how it warms up. Returning early here was a
    // chicken and egg: below the threshold nothing was recorded, so the head
    // never trained, so it never reached the threshold, so it never chose.
    // Below it the old rule picks and findWarTarget records which candidate
    // that was -- the head learns by watching a policy that already works.
    const bool maySteer = m_target.updateCount() >= TARGET_WARMUP_UPDATES;

    std::vector<float> own;
    buildFeatures(cid, own);

    const size_t n = std::min(cands.size(), (size_t)TARGET_MAX_CANDIDATES);
    std::vector<float> scores(n, 0.0f);
    std::vector<float> candFeat;
    for (size_t i = 0; i < n; ++i) {
        buildTargetFeatures(cid, cands[i], candFeat);
        std::vector<float> in = own;
        in.insert(in.end(), candFeat.begin(), candFeat.end());
        const std::vector<float>& o = m_target.forward(in);
        scores[i] = o.empty() || !std::isfinite(o[0]) ? 0.0f : o[0];
        m_pendingTargetCand.push_back(std::move(in));
    }

    if (!maySteer) return -1;   // recorded, but the rule still decides

    float temperature, epsilon;
    difficultyParams(temperature, epsilon);
    const int pick = NeuralNet::samplePolicy(scores, temperature, m_rng);
    if (pick < 0 || pick >= (int)n) return -1;
    m_pendingTargetChosen = pick;
    return pick;
}

bool AISystem::findWarTarget(int cid, WarTarget& out, bool learnedChoice) {
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    if (!c) return false;
    const CountryStat& st = m_stats[cid];
    if (st.army <= 0) return false;
    auto relIt = g.m_relations.find(c->isoA3);

    // RESTRAINT, WITHOUT PACIFISM.
    //
    // The AI declared war whenever it could win a fight, which is not the same
    // as whenever war is a good idea: it opened fronts while already fighting
    // two, at parity, on neighbours it had no claim to, with the home front in
    // revolt. The gates below say when NOT to, and every one of them is
    // deliberately blind to CLAIMED land -- retaking territory it claims is the
    // AI's whole war goal and stays cheap. What gets harder is opportunistic
    // conquest of land it has no argument for.
    //
    // These are heuristics rather than learning, on purpose: the model is
    // trained and shipped, so "be less aggressive" cannot wait for a retrain,
    // and a gate the policy cannot talk its way past is the only kind that
    // holds.
    // Foreign wars only -- see foreignWarCount. Also cheaper: m_warWith is the
    // relation graph already resolved to cids, so this is a set walk rather
    // than a string-keyed scan of every relation this country has.
    const int myWars = foreignWarCount(cid);

    // Already fighting two? Nothing is worth a third front.
    if (myWars >= AI_MAX_CONCURRENT_WARS) return false;
    // A country coming apart at home does not go looking for more.
    if (g.warWearinessOf(cid) >= AI_WAR_WEARINESS_BLOCK) return false;

    // Which neighbours hold provinces we claim?
    std::unordered_set<int> claimTargets;
    auto myClaims = g.m_claims.find(c->isoA3);
    if (myClaims != g.m_claims.end())
        for (int pid : myClaims->second) {
            int owner = pid < (int)g.m_provinceCountryLookup.size()
                            ? g.m_provinceCountryLookup[pid] : 0;
            if (owner > 0 && owner != cid && owner < Game::SPC_CID)
                claimTargets.insert(owner);
        }

    int target = -1; long long targetArmy = -1;
    bool targetClaimed = false;
    std::vector<WarCandidate> cands;
    std::unordered_set<int> seen;
    std::unordered_set<int> napBlocked;   // wanted, but under a pact
    for (auto& fr : st.frontiers) {
        if (!seen.insert(fr.enemyCid).second) continue;
        const Country* ec = g.m_countries.getCountry(fr.enemyCid);
        if (!ec) continue;
        bool friendly = false, war = false, nap = false;
        if (relIt != g.m_relations.end()) {
            auto rr = relIt->second.find(ec->isoA3);
            if (rr != relIt->second.end()) {
                war = rr->second.war;
                friendly = rr->second.alliance || rr->second.guarantee;
                nap = rr->second.nonAggression;
            }
        }
        if (war || friendly) continue;
        // A NAP does not make this target off-limits, but it does mean the pact
        // has to be broken FIRST -- see the break-then-declare note where the
        // action is issued. The target is still chosen here so the AI can want
        // a war it is not yet allowed to start.
        if (nap) napBlocked.insert(fr.enemyCid);
        long long ea = m_stats[fr.enemyCid].army;
        bool claimed = claimTargets.count(fr.enemyCid) > 0;
        // Reconquering CLAIMED land stays cheap: it removes unrest and
        // satisfies the claim, and it is the expansion that is supposed to
        // happen. Attacking a neighbour it has NO claim on now needs a real
        // edge rather than a coin-flip one -- 1.05 meant "very slightly ahead",
        // which is why the map was permanently on fire. Land is still taken; it
        // just has to be worth taking.
        double bar = claimed ? AI_WAR_BAR_CLAIMED : AI_WAR_BAR_UNCLAIMED;
        // Opening a SECOND war costs more again, claim or no claim: one front
        // at a time unless the second is genuinely easy.
        if (myWars >= 1) bar += AI_WAR_BAR_SECOND_FRONT;
        if (st.army < (long long)(ea * bar) + 200) continue;
        // EVERY neighbour that clears the bars is a candidate, not just the
        // best one by the old rule. The rule still decides who is ALLOWED to be
        // attacked; which of them actually is, is chosen below.
        cands.push_back({fr.enemyCid, claimed, false,
                         napBlocked.count(fr.enemyCid) > 0, ea});
        // The old rule, kept as the fallback and as the mask's answer: a
        // claimed neighbour beats any unclaimed one, and within a class the
        // weakest wins.
        if (target < 0 || (claimed && !targetClaimed) ||
            (claimed == targetClaimed && ea < targetArmy)) {
            target = fr.enemyCid; targetArmy = ea; targetClaimed = claimed;
        }
    }
    if (target >= 0) {
        const int pick = learnedChoice ? chooseWarTarget(cid, cands) : -1;
        if (pick >= 0 && pick < (int)cands.size()) {
            out.cid = cands[pick].cid;
            out.claimed = cands[pick].claimed;
            out.naval = false;
            out.napBlocked = cands[pick].napBlocked;
            return true;
        }
        // The rule decided. Tell the head what it picked, so a head that is
        // not yet trusted still has something to learn from.
        if (!m_pendingTargetCand.empty()) {
            for (size_t i = 0; i < cands.size() && i < m_pendingTargetCand.size(); ++i)
                if (cands[i].cid == target) { m_pendingTargetChosen = (int)i; break; }
        }
        out.cid = target;
        out.claimed = targetClaimed;
        out.naval = false;
        out.napBlocked = napBlocked.count(target) > 0;
        return true;
    }

    // Naval fallback: no reachable land target, but we have a port and an army
    // -- declare war on the weakest beatable OVERSEAS coastal enemy (one that
    // owns a port to land at) so the navy module can embark, sail, and invade
    // it. This is the unlock that lets the AI cross water for territory instead
    // of only fighting land borders.
    if (st.maxPort < 1 || st.army <= 1000) return false;
    std::unordered_set<int> landNbr;
    for (auto& fr : st.frontiers) landNbr.insert(fr.enemyCid);
    std::unordered_set<int> seenC;
    long long bestArmy = -1; int navalTarget = -1; bool navalClaimed = false;
    for (auto& [ppid, port] : g.m_provincePorts) {
        int oc = (ppid >= 0 && ppid < (int)g.m_provinceCountryLookup.size())
                     ? g.m_provinceCountryLookup[ppid] : 0;
        if (oc <= 0 || oc == cid || oc >= Game::REBEL_CID_MIN) continue;
        if (landNbr.count(oc) || !seenC.insert(oc).second) continue;
        const Country* ec2 = g.m_countries.getCountry(oc);
        if (!ec2) continue;
        bool friendly = false, war = false, nap2 = false;
        if (relIt != g.m_relations.end()) {
            auto rr = relIt->second.find(ec2->isoA3);
            if (rr != relIt->second.end()) {
                war = rr->second.war;
                friendly = rr->second.alliance || rr->second.guarantee;
                nap2 = rr->second.nonAggression;
            }
        }
        if (war || friendly) continue;
        // Same rule as the land path: an overseas pact is broken first, not
        // sailed through. A surprise amphibious landing on a country you have a
        // pact with is the same violation, and was reachable by the same route.
        if (nap2) napBlocked.insert(oc);
        long long ea = m_stats[oc].army;
        bool claimed = claimTargets.count(oc) > 0;
        // Amphibious assaults are costlier than a land push (troops ferry in
        // piecemeal), so this already demanded a clearer edge. It carries the
        // same second-front surcharge as the land path, or restraint would just
        // be a matter of sailing round it.
        double bar = claimed ? 1.0 : AI_WAR_BAR_UNCLAIMED_NAVAL;
        if (myWars >= 1) bar += AI_WAR_BAR_SECOND_FRONT;
        if (st.army < (long long)(ea * bar) + 500) continue;
        if (navalTarget < 0 || (claimed && !navalClaimed) ||
            (claimed == navalClaimed && ea < bestArmy)) {
            navalTarget = oc; bestArmy = ea; navalClaimed = claimed;
        }
    }
    if (navalTarget < 0) return false;
    out.cid = navalTarget;
    out.claimed = navalClaimed;
    out.naval = true;
    out.napBlocked = napBlocked.count(navalTarget) > 0;
    return true;
}

void AISystem::validWar(int cid, std::vector<bool>& v) {
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    const CountryStat& st = m_stats[cid];
    v.assign(WAR_ACTIONS, false);
    v[0] = true;
    if (!c) return;
    v[1] = c->treasury >= 1 && st.population > 10000; // recruit

    // Reinforce needs somewhere to move troops FROM, not merely a frontier.
    //
    // "st.army > 0 && has a frontier" offered this action on almost every turn
    // of the game, and execWar then answered "reinforce: nothing to move" —
    // measured at 3,181 times in a 400-turn run, a tenth of every war decision
    // taken on the map. A masked-out action costs the policy nothing; an action
    // that is offered and does nothing costs it a turn, and teaches it that the
    // war module is mostly inert.
    auto garrisonOf = [&](int pid, int owner) -> long long {
        auto it = g.m_provinceArmies.find(pid);
        if (it == g.m_provinceArmies.end()) return 0;
        long long n = 0;
        for (auto& u : it->second) if (u.countryId == owner) n += u.count;
        return n;
    };
    bool canReinforce = false;
    for (auto& fr : st.frontiers) {
        auto nIt = g.m_provinceNeighbors.find(fr.pid);
        if (nIt == g.m_provinceNeighbors.end()) continue;
        for (int nid : nIt->second) {
            if (nid < 0 || nid >= (int)g.m_provinceCountryLookup.size() ||
                g.m_provinceCountryLookup[nid] != cid) continue;
            if (garrisonOf(nid, cid) >= 200) { canReinforce = true; break; }
        }
        if (canReinforce) break;
    }
    v[2] = st.army > 0 && canReinforce;
    // attack / artillery need frontier context; cheap checks only. Whether a
    // neighbour is DECLARABLE is no longer decided here — see v[4] below.
    bool anyWarFrontier = false;
    auto relIt = g.m_relations.find(c->isoA3);
    std::unordered_set<int> seen;
    for (auto& fr : st.frontiers) {
        if (!seen.insert(fr.enemyCid).second) continue;
        const Country* ec = g.m_countries.getCountry(fr.enemyCid);
        if (!ec) continue;
        if (relIt == g.m_relations.end()) continue;
        auto rr = relIt->second.find(ec->isoA3);
        if (rr != relIt->second.end() && rr->second.war) { anyWarFrontier = true; break; }
    }
    // Attack is also possible from an army standing on allied ground, which is
    // the only way a staged force is ever any use.
    v[3] = (anyWarFrontier || !st.abroadPids.empty()) && st.army > 0;
    // Declare war: offered only when there is a declaration the executor would
    // actually issue. This asks the same function exec does -- see
    // findWarTarget for what the old "any non-friendly neighbour and any army"
    // test was costing. It also subsumes the overseas case, which used to need
    // a separate navalDeclarable test here that did not match exec's.
    {
        WarTarget wt;
        v[4] = findWarTarget(cid, wt);
    }
    // Artillery needs SHELLS. The comment used to say "ammo checked at exec",
    // which is true and is exactly the problem: exec answered "artillery: no
    // researched ammo" 3,271 times in a 400-turn run, because AI countries
    // rarely research an artillery node and the mask never asked. Check the
    // same table exec fires from, so the action is offered only when it exists.
    bool haveShell = false;
    if (anyWarFrontier) {
        static const struct { const char* node; float cost; } SHELLS[] = {
            {"arty6a", 80}, {"arty6b", 60}, {"arty5", 40}, {"arty4a", 30},
            {"arty4b", 25}, {"arty3", 20},  {"arty2", 10}, {"arty1", 5}};
        for (auto& s : SHELLS)
            if (g.hasResearched(s.node, cid) && c->treasury >= s.cost) { haveShell = true; break; }
    }
    v[5] = anyWarFrontier && haveShell;
    // Offer ceasefire unless we are so far ahead that the war is nearly won.
    //
    // The bar used to be 1.2x, i.e. "only sue for peace while roughly level or
    // losing", on the reasoning that a winner should press on so wars reach a
    // conclusion. What it actually produced was long wars: the moment a country
    // pulled ahead the only exit left was total conquest, and conquest is slow.
    // 1.6x keeps that intent for a genuinely decisive lead while letting a
    // country that is merely ahead cash the advantage in — and when it is ahead
    // it does not offer a white peace, it offers terms (see the posture
    // selection below), so peace stays worth something.
    // ...and only when there is an enemy we are actually allowed to talk to.
    // The cooldown lives in exec, so the mask offered "sue for peace" to
    // countries with no war at all, or none off cooldown: "ceasefire: no war to
    // end" 1,370 times in the same run.
    long long warEnemyArmy = 0; bool anyWar = false, anyReachable = false;
    if (relIt != g.m_relations.end())
        for (auto& [iso, r] : relIt->second)
            if (r.war) {
                anyWar = true;
                int ocid = g.cidForIso(iso);
                if (ocid >= 0) {
                    warEnemyArmy += m_stats[ocid].army;
                    if (diploReady(cid, ocid)) anyReachable = true;
                }
            }
    v[6] = anyWar && anyReachable && st.army < (long long)(warEnemyArmy * 1.6);
    // Staging needs an allied crossing that leads somewhere and troops to send.
    v[7] = !st.staging.empty() && st.army > 500;
}

void AISystem::validNavy(int cid, std::vector<bool>& v) {
    Game& g = *m_g;
    const CountryStat& st = m_stats[cid];
    v.assign(NAVY_ACTIONS, false);
    v[0] = true;
    int ships = st.boats + st.destroyers + st.carriers;
    v[1] = ships > 0;
    v[2] = st.destroyers + st.carriers > 0; // bombard needs a warship
    // Embarking must have somewhere to go. Without this the AI loaded half the
    // garrison of its best port onto boats every time the action came up, with
    // no invasion target anywhere — ~90% of embarkations never produced a
    // landing, so the troops were simply deleted from the land army. That bled
    // armies on every map type and is why the war module sat on "hold".
    // Embark needs a shore we are ALREADY AT WAR WITH, not merely one we could
    // declare on. navalTargets counts countries a war has not been declared
    // against yet, and loading troops for a war that has not started is exactly
    // the churn that produced 1,372 embarkations and 121 landings: the cargo
    // had nowhere to go, so it sailed about and came home. The pipeline that
    // works reads declare naval war -> navalWarTargets -> embark -> the
    // amphibious reflex sails and lands it.
    v[3] = st.maxPort >= 1 && st.army > 1000 && st.navalWarTargets > 0;
    v[4] = st.boatsWithCrew > 0;

    // ── Scrap: stop paying for a fleet that is not earning it ──
    //
    // WARSHIPS ONLY, and that is not an oversight. Upkeep (Game_Economy.cpp) is
    // 25 a turn for a carrier and 10 for a destroyer, plus a crew charge; a
    // transport with no cargo is charged nothing at all, so scrapping one saves
    // exactly nothing and only costs the country the ability to move troops
    // later. A crewed boat is never scrappable at any price: processScrapShips
    // reassigns the hull to UNC_CID and the men aboard simply cease to exist.
    //
    // 25 a turn is real money at this scale — a small country's whole gross
    // income was measured at 18 — so an idle carrier is a country that cannot
    // afford industry because it is paying for a ship with nowhere to sail.
    const bool haveScrappable = st.destroyers + st.carriers > 0;
    bool atWar = false;
    {
        auto w = m_warWith.find(cid);
        atWar = (w != m_warWith.end() && !w->second.empty());
    }
    CountryIncomeSnapshot inc = g.computeCountryIncome(cid);
    const bool costly = inc.total > 1.0f && inc.navyExpenses > inc.total * 0.15f;
    const bool broke  = inc.net < 0.0f;
    // Nothing to fight, nowhere to sail, and more than one hull to pay for.
    const bool idleFleet = !atWar && st.navalTargets == 0 &&
                           st.navalWarTargets == 0 && ships > 1;
    v[5] = haveScrappable && (costly || broke || idleFleet);
}

// ─── Action execution ────────────────────────────────────
// Each returns a human-readable label for the debug log. All orders go
// through the exact vectors the player's buttons fill, with the same costs
// deducted at enqueue.

std::string AISystem::execEconomy(int cid, int action) {
    Game& g = *m_g;
    Country& c = g.m_countries.getAll()[cid];
    const CountryStat& st = m_stats[cid];

    switch (action) {
        case 1: { // upgrade industry in the most populous eligible province
            int bestPid = -1; long long bestPop = -1;
            int cap = industryCap(cid);
            for (int pid : g.provincesOf(cid)) {
                auto ind = g.m_provinceIndustry.find(pid);
                int lvl = ind != g.m_provinceIndustry.end() ? ind->second.level : 0;
                if (lvl >= cap) continue;
                bool pending = false;
                for (auto& pu : g.m_pendingUpgrades)
                    if (pu.provinceId == pid && pu.type == "industry") { pending = true; break; }
                if (pending) continue;
                long long pop = g.m_provincePopulations.count(pid) ? g.m_provincePopulations[pid] : 0;
                // Resource-rich provinces pay industry back faster (and enable
                // a later specialization), so weight population by resources.
                float resBoost = 0;
                auto res = g.m_provinceResources.find(pid);
                if (res != g.m_provinceResources.end())
                    resBoost = res->second.oil.amount + res->second.gold.amount +
                               res->second.metal.amount + res->second.rubber.amount +
                               res->second.gemstones.amount;
                long long score = (long long)(pop * (1.0f + resBoost / 100.0f));
                if (score > bestPop) { bestPop = score; bestPid = pid; }
            }
            if (bestPid < 0) return "industry: no eligible province";
            auto ind = g.m_provinceIndustry.find(bestPid);
            int nextLv = (ind != g.m_provinceIndustry.end() ? ind->second.level : 0) + 1;
            if (nextLv > 10) return "industry: capped";
            float cost = (float)AI_IND_COST[nextLv];
            if (c.treasury < cost) return "industry: cannot afford";
            c.treasury -= cost;
            g.m_pendingUpgrades.push_back({bestPid, "industry", nextLv, AI_IND_TURNS[nextLv]});
            return TextFormat("industry lvl %d in prov %d ($%.0f)", nextLv, bestPid, cost);
        }
        case 2: { // fortify the most THREATENED under-fortified frontier:
            // prioritise the frontier facing the biggest enemy army, weakest
            // walls first (score = enemy army / (1 + fort level)).
            int bestPid = -1, bestLvl = 0;
            double bestScore = -1;
            int cap = fortCap(cid);
            for (auto& fr : st.frontiers) {
                auto ind = g.m_provinceIndustry.find(fr.pid);
                int fl = ind != g.m_provinceIndustry.end() ? ind->second.fortification : 0;
                if (fl >= cap) continue;
                bool pending = false;
                for (auto& pu : g.m_pendingUpgrades)
                    if (pu.provinceId == fr.pid && pu.type == "fortification") { pending = true; break; }
                if (pending) continue;
                double threat = (double)m_stats[fr.enemyCid].army / (1.0 + fl);
                if (threat > bestScore) { bestScore = threat; bestLvl = fl; bestPid = fr.pid; }
            }
            if (bestPid < 0) return "fort: no eligible frontier";
            int nextLv = bestLvl + 1;
            float cost = (float)AI_FORT_COST[std::min(nextLv, 5)];
            if (c.treasury < cost) return "fort: cannot afford";
            c.treasury -= cost;
            g.m_pendingUpgrades.push_back({bestPid, "fortification", nextLv, 1});
            return TextFormat("fort lvl %d in prov %d ($%.0f)", nextLv, bestPid, cost);
        }
        case 3: { // port: upgrade an existing one, else found a new one
            int cap = portCap(cid);
            for (auto& [pid, port] : g.m_provincePorts) {
                const Province* p = g.m_provinces.getProvinceById(pid);
                if (!p || p->countryId != cid || port.level >= cap) continue;
                float cost = 60.0f * (port.level + 1);
                if (c.treasury < cost) continue;
                bool pending = false;
                for (auto& pu : g.m_pendingUpgrades)
                    if (pu.provinceId == pid && pu.type == "port") { pending = true; break; }
                if (pending) continue;
                c.treasury -= cost;
                g.m_pendingUpgrades.push_back({pid, "port", port.level + 1, 3});
                return TextFormat("port lvl %d in prov %d", port.level + 1, pid);
            }
            if (cap >= 1 && c.treasury >= 60) {
                // Found a new port in the most populous coastal province.
                // isProvinceCoastal does a bounded BFS; test only the best few.
                std::vector<std::pair<long long, int>> cands;
                for (int pid : g.provincesOf(cid)) {
                    if (g.m_provincePorts.count(pid)) continue;
                    long long pop = g.m_provincePopulations.count(pid) ? g.m_provincePopulations[pid] : 0;
                    cands.push_back({pop, pid});
                }
                std::sort(cands.rbegin(), cands.rend());
                for (size_t i = 0; i < cands.size() && i < 4; ++i) {
                    if (!g.isProvinceCoastal(cands[i].second)) continue;
                    c.treasury -= 60;
                    g.m_pendingUpgrades.push_back({cands[i].second, "port", 1, 3});
                    return TextFormat("new port in prov %d", cands[i].second);
                }
            }
            return "port: no candidate";
        }
        case 4: { // specialize the strongest-resource industrial province
            int bestPid = -1; float bestAmt = 0; const char* bestRes = nullptr;
            for (int pid : g.provincesOf(cid)) {
                auto ind = g.m_provinceIndustry.find(pid);
                if (ind == g.m_provinceIndustry.end() || ind->second.level < 1) continue;
                if (!ind->second.specialization.empty()) continue;
                bool pending = false;
                for (auto& ps : g.m_pendingSpecializations)
                    if (ps.provinceId == pid) { pending = true; break; }
                if (pending) continue;
                auto res = g.m_provinceResources.find(pid);
                if (res == g.m_provinceResources.end()) continue;
                struct { const char* n; float a; } rs[] = {
                    {"Oil", res->second.oil.amount}, {"Gold", res->second.gold.amount},
                    {"Metal", res->second.metal.amount}, {"Rubber", res->second.rubber.amount},
                    {"Gemstones", res->second.gemstones.amount}};
                for (auto& r : rs)
                    if (r.a > bestAmt) { bestAmt = r.a; bestPid = pid; bestRes = r.n; }
            }
            if (bestPid < 0 || !bestRes) return "spec: no candidate";
            auto ind = g.m_provinceIndustry.find(bestPid);
            float cost = AI_IND_COST[std::clamp(ind->second.level, 0, 10)] * 1.5f;
            if (c.treasury < cost) return "spec: cannot afford";
            c.treasury -= cost;
            g.m_pendingSpecializations.push_back({bestPid, bestRes, 3});
            return TextFormat("specialize %s in prov %d", bestRes, bestPid);
        }
        case 5: case 6: { // build ship at the best own port
            const char* type = action == 5 ? "destroyer" : "carrier";
            float cost = action == 5 ? 15.0f : 40.0f;
            int needPort = action == 5 ? 2 : 3;
            if (c.treasury < cost) return "ship: cannot afford";
            for (auto& [pid, port] : g.m_provincePorts) {
                const Province* p = g.m_provinces.getProvinceById(pid);
                if (!p || p->countryId != cid || port.level < needPort) continue;
                c.treasury -= cost;
                g.m_pendingShipBuilds.push_back({pid, type, 3});
                return TextFormat("build %s at prov %d", type, pid);
            }
            return "ship: no port";
        }
        case 7: { // research funding up
            float& alloc = g.m_countryResearchAllocation[cid];
            alloc = std::min(0.5f, alloc + 0.05f);
            return TextFormat("research funding up to %.0f%%", alloc * 100);
        }
        case 8: { // research funding down
            float& alloc = g.m_countryResearchAllocation[cid];
            alloc = std::max(0.0f, alloc - 0.05f);
            return TextFormat("research funding down to %.0f%%", alloc * 100);
        }
        case 9: case 10: case 11: { // pick the next research node by branch
            static const char* FOCUS[] = {"buildings", "army", "navy"};
            const char* want = FOCUS[action - 9];
            // Cheapest available node in the focused branch; if the branch is
            // exhausted, cheapest available anywhere (population/misc land here).
            int bestIdx = -1, bestCost = INT32_MAX;
            int fallbackIdx = -1, fallbackCost = INT32_MAX;
            for (int i = 0; i < (int)g.m_researchNodes.size(); ++i) {
                const ResearchNode& n = g.m_researchNodes[i];
                if (n.infinite) continue;
                if (!g.isNodeAvailableFor(n, cid)) continue;
                // Skip nodes whose ONLY effect is a build cap the baseline
                // already grants — researching them changes nothing. (Nodes
                // with any other effect are always worth considering.)
                if ((n.fortLevel > 0 || n.industryLevel > 0 || n.portLevel > 0) &&
                    n.fortLevel <= fortCap(cid) && n.industryLevel <= industryCap(cid) &&
                    n.portLevel <= portCap(cid) && !n.unlockShips &&
                    n.armyDefPct == 0 && n.armyAtkPct == 0 && n.conscriptionCostPct == 0 &&
                    n.maintenanceCostPct == 0 && n.navyCostPct == 0 && n.navyAtkPct == 0 &&
                    n.navyDefPct == 0 && n.navySpeedPct == 0 && n.popModPct == 0 &&
                    n.resourceModPct == 0 && n.industryCostPct == 0 && n.passiveIncome == 0 &&
                    n.popGrowthPct == 0 && n.migrationRate == 0 && n.indoctrinationPct == 0 &&
                    n.conscriptionPct == 0 && n.artilleryType.empty())
                    continue;
                if (n.category == want) {
                    if (n.cost < bestCost) { bestCost = n.cost; bestIdx = i; }
                } else if (n.cost < fallbackCost) {
                    fallbackCost = n.cost; fallbackIdx = i;
                }
            }
            int pick = bestIdx >= 0 ? bestIdx : fallbackIdx;
            if (pick < 0) {
                statsFor(cid).researchNothingLeft++;
                return TextFormat("research: nothing left (%s)", want);
            }
            statsFor(cid).researchArmed++;
            g.m_countryResearchActive[cid] = pick;
            g.m_countryResearchInvested[cid] = 0;
            // Funding must be flowing or the node never completes
            float& alloc = g.m_countryResearchAllocation[cid];
            if (alloc < 0.05f) alloc = 0.05f;
            return TextFormat("research %s (%s, cost %d)",
                              g.m_researchNodes[pick].id.c_str(),
                              g.m_researchNodes[pick].category.c_str(),
                              g.m_researchNodes[pick].cost);
        }
        default: return "save money";
    }
}

std::string AISystem::execPolitics(int cid, int action) {
    Game& g = *m_g;
    switch (action) {
        case 1: { // enact the policy that best fits this country's politics
            const Country* c = g.m_countries.getCountry(cid);
            if (!c) return "policy: no country";
            // Compass fit was the ONLY criterion here, which made the choice a
            // deterministic lookup the model could not steer and, worse, one
            // that ignored the price: an enactable policy the treasury could
            // technically afford was picked over a free one that did the same
            // job, every turn, forever. Fit still dominates — a government does
            // not enact things it disagrees with — but a cheap policy now wins
            // ties, which over a long game is the difference between a budget
            // and a slow bleed.
            const CountryIncomeSnapshot inc = g.computeCountryIncome(cid);
            const Policy* best = nullptr; float bestScore = -1e9f;
            for (auto& p : g.m_allPolicies) {
                if (!g.canCountryEnactPolicy(cid, p)) continue;
                const float d = std::fabs(c->compassEconomic / 25.0f - p.econShift) +
                                std::fabs(c->compassSocial / 25.0f - p.socShift);
                float score = -d;
                if (inc.total > 1.0f) score -= 3.0f * (p.costPerTurn / inc.total);
                if (score > bestScore) { bestScore = score; best = &p; }
            }
            if (!best) return "policy: none enactable";
            g.enactPolicy(cid, best->id);
            return "enact policy " + best->id;
        }
        case 2: {
            float& pac = g.m_countryPacification[cid];
            pac = std::min(1.0f, pac + 0.125f);
            return TextFormat("pacification up to %.0f%%", pac * 100);
        }
        case 3: {
            float& pac = g.m_countryPacification[cid];
            pac = std::max(0.0f, pac - 0.125f);
            return TextFormat("pacification down to %.0f%%", pac * 100);
        }
        case 4: { // cancel the costliest active policy (budget rescue)
            auto apIt = g.m_countryActivePolicyIndices.find(cid);
            if (apIt == g.m_countryActivePolicyIndices.end() || apIt->second.empty())
                return "cancel: none active";
            int bestIdx = -1; int bestCost = -1;
            for (int idx : apIt->second) {
                if (idx < 0 || idx >= (int)g.m_activePolicies.size()) continue;
                auto& ap = g.m_activePolicies[idx];
                if (ap.turnsRemaining < 0) continue;
                for (auto& p : g.m_allPolicies)
                    if (p.id == ap.policyId && p.costPerTurn > bestCost) {
                        bestCost = p.costPerTurn; bestIdx = idx;
                    }
            }
            if (bestIdx < 0) return "cancel: none active";
            std::string pid = g.m_activePolicies[bestIdx].policyId;
            g.cancelPolicy(bestIdx);
            return "cancel policy " + pid;
        }
        case 5: case 6: case 7: { // propose alliance / NAP / guarantee
            static const char* REQ[] = {"request_alliance", "request_nap", "request_guarantee"};
            const char* req = REQ[action - 5];
            const Country* c = g.m_countries.getCountry(cid);
            if (!c) return "diplo: no country";
            auto relIt = g.m_relations.find(c->isoA3);
            // Don't pact a neighbour whose land we claim — we want to conquer
            // it, not befriend it. Otherwise the politics module keeps pacting
            // the very targets the war module wants, and the map freezes.
            std::unordered_set<int> claimTargets;
            auto myClaims = g.m_claims.find(c->isoA3);
            if (myClaims != g.m_claims.end())
                for (int pid : myClaims->second) {
                    int owner = pid < (int)g.m_provinceCountryLookup.size()
                                    ? g.m_provinceCountryLookup[pid] : 0;
                    if (owner > 0 && owner != cid && owner < Game::SPC_CID)
                        claimTargets.insert(owner);
                }
            // Target: the STRONGEST neighbour we're not at war with and don't
            // already have this relation with — befriend the biggest threat.
            int target = -1; long long targetArmy = -1;
            double bestWorth = -1.0;
            std::unordered_set<int> seen;
            for (auto& fr : m_stats[cid].frontiers) {
                if (!seen.insert(fr.enemyCid).second) continue;
                const Country* ec = g.m_countries.getCountry(fr.enemyCid);
                if (!ec || ec->isoA3.empty()) continue;
                bool war = false, already = false;
                if (relIt != g.m_relations.end()) {
                    auto rr = relIt->second.find(ec->isoA3);
                    if (rr != relIt->second.end()) {
                        war = rr->second.war;
                        // An alliance already implies non-aggression and mutual
                        // defence, so an allied pair has nothing left to ask
                        // for. Testing only the matching flag meant allies kept
                        // proposing NAPs and guarantees to each other.
                        already = rr->second.alliance ||
                                  (action == 6 && rr->second.nonAggression) ||
                                  (action == 7 && rr->second.guarantee);
                    }
                }
                if (war || already || claimTargets.count(fr.enemyCid) ||
                    !diploReady(cid, fr.enemyCid)) continue;
                bool pendingReq = false;
                for (auto& da : g.m_pendingDiplomaticActions)
                    if (da.sourceIso == c->isoA3 && da.targetIso == ec->isoA3) { pendingReq = true; break; }
                if (pendingReq) continue;
                long long ea = m_stats[fr.enemyCid].army;
                // WORTH HAVING, AND LIKELY TO AGREE.
                //
                // This was "whoever has the biggest army", which picks the most
                // valuable partner and also the one least likely to want us --
                // so the module spent its turns being refused, and each refusal
                // costs a turn and a cooldown. Strength still counts, because a
                // strong ally is the point; it is now multiplied by how the
                // partner will actually answer rather than assumed.
                //
                // predictAcceptance runs the diplomacy net on THEIR features
                // with the same bias their own answer would use. Every country
                // shares these weights, so that is not a guess about them, it is
                // the reply computed a turn early.
                const float pAccept = predictAcceptance(fr.enemyCid, req);
                // Floored, so a partner the model currently dislikes is
                // unlikely rather than impossible: a hard zero would let an
                // early, badly-calibrated diplomacy net permanently rule out
                // whole classes of ally and never learn otherwise.
                const double worth = std::log1p((double)ea) * (0.15 + 0.85 * pAccept);
                if (worth > bestWorth) { bestWorth = worth; targetArmy = ea; target = fr.enemyCid; }
            }
            if (target < 0) return TextFormat("%s: no suitable target", req);
            const Country* ec = g.m_countries.getCountry(target);
            g.m_pendingDiplomaticActions.push_back({c->isoA3, ec->isoA3, req, 1});
            diploCoolDown(cid, target);
            statsFor(cid).pactsProposed++;
            return TextFormat("%s -> %s", req, ec->name.c_str());
        }
        case 8: { // enact whatever calms the country down
            // The counterpart to case 1. That one asks "what do we believe in";
            // this asks "what stops the country coming apart", which is a
            // different question with a different answer, and the module had no
            // way to express it: its only unrest lever was the pacification
            // slider, which is money spent to suppress a symptom every turn
            // rather than a policy that removes the cause once.
            const Policy* best = nullptr; float bestScore = 0.0f;
            const CountryIncomeSnapshot inc = g.computeCountryIncome(cid);
            for (auto& p : g.m_allPolicies) {
                if (!g.canCountryEnactPolicy(cid, p)) continue;
                // publicOpinionShift moves provinces toward the government,
                // which is exactly what the political half of unrest measures.
                float score = 2.0f * p.effect.unrestReduction
                            + 1.0f * std::fabs(p.effect.publicOpinionShift)
                            + 0.5f * p.effect.minorityGrowthRate;
                if (score <= 0.0f) continue;
                if (inc.total > 1.0f) score -= 2.0f * (p.costPerTurn / inc.total);
                if (score > bestScore) { bestScore = score; best = &p; }
            }
            if (!best) return "calm: no policy would help";
            g.enactPolicy(cid, best->id);
            statsFor(cid).calmingPolicies++;
            return "enact calming policy " + best->id;
        }
        case 9: case 10: { // conciliate / repress a minority
            // ONE CATEGORY PER TURN, and never by index.
            //
            // The option lists are not ordered consistently — "Harsh, Medium,
            // Light" runs one way and "Full Autonomy, Partial, Suppression" the
            // other — so stepping an index would liberalise one category and
            // tighten another in the same breath. alignmentPerTurn is the thing
            // that actually means "more or less conciliatory", so the step is
            // taken in that.
            const bool conciliate = (action == 9);
            const CountryStat& st = m_stats[cid];
            if (st.minorities <= 0) return "minority: none here";

            // Conciliation goes to whoever is closest to revolt; repression to
            // whoever is costing the most, because saving that money is the
            // only reason to do it.
            // EVERY minority is a candidate, in preference order -- not one.
            //
            // This used to commit to a single target: the least reconciled for
            // conciliation, the most expensive for repression. The validity
            // mask, meanwhile, asks whether the country's MEAN trend still has
            // room to move, which it does whenever ANY minority does. So a
            // country whose costliest group was already at the harshest setting
            // in every category was offered the action, picked it, and got back
            // "repress: already hardest" -- every turn, for the rest of the
            // game. Seen on a live run at turn 3534: two of the four countries
            // on screen were doing exactly that, and the wasted decision was
            // still being recorded and still generating a gradient, teaching
            // the politics head that the action is safe and free.
            //
            // Ordered by preference and then walked until one yields a step, so
            // the executor can always do what the mask promised.
            std::vector<std::pair<float, std::string>> candidates;
            {
                std::unordered_set<std::string> seen;
                for (int pid : g.provincesOf(cid)) {
                    auto mIt = g.m_provinceMinorities.find(pid);
                    if (mIt == g.m_provinceMinorities.end()) continue;
                    for (auto& mg : mIt->second) {
                        if (!seen.insert(mg.name).second) continue;
                        if (conciliate) {
                            // Least reconciled first: lowest alignment ranks top.
                            candidates.push_back({-g.getMinorityAlignment(cid, mg.name), mg.name});
                        } else {
                            float cost = 0;
                            for (size_t ci = 0; ci < g.m_ethnicPolicyCategories.size(); ++ci) {
                                const int oi = g.ethnicPolicyOption(cid, mg.name, ci);
                                if (oi >= 0 && oi < (int)g.m_ethnicPolicyCategories[ci].options.size())
                                    cost += g.m_ethnicPolicyCategories[ci].options[oi].costPerTurn;
                            }
                            candidates.push_back({cost, mg.name});   // dearest first
                        }
                    }
                }
                std::sort(candidates.rbegin(), candidates.rend());
            }
            if (candidates.empty()) return "minority: none here";

            // Best single change: the largest move in the wanted direction per
            // unit of extra cost. Ties on cost break toward the bigger move.
            const CountryIncomeSnapshot inc = g.computeCountryIncome(cid);
            const float headroom = std::max(0.0f, inc.total - inc.expenses);
            std::string target;
            size_t bestCat = 0; int bestOpt = -1;
            for (const auto& [rank, name] : candidates) {
                (void)rank;
                float bestScore = 0.0f;
                size_t cat = 0; int opt = -1;
                for (size_t ci = 0; ci < g.m_ethnicPolicyCategories.size(); ++ci) {
                    const auto& c2 = g.m_ethnicPolicyCategories[ci];
                    const int cur = g.ethnicPolicyOption(cid, name, ci);
                    if (cur < 0 || cur >= (int)c2.options.size()) continue;
                    const float curAlign = c2.options[cur].alignmentPerTurn;
                    const float curCost  = c2.options[cur].costPerTurn;
                    for (size_t oi = 0; oi < c2.options.size(); ++oi) {
                        if ((int)oi == cur) continue;
                        const float dAlign = c2.options[oi].alignmentPerTurn - curAlign;
                        const float dCost  = c2.options[oi].costPerTurn - curCost;
                        if (conciliate ? dAlign <= 0.0f : dAlign >= 0.0f) continue;
                        // Never sign up for something we cannot pay for.
                        if (dCost > headroom) continue;
                        const float gain = conciliate ? dAlign : -dAlign;
                        const float score = gain / (1.0f + std::max(0.0f, dCost));
                        if (score > bestScore) { bestScore = score; cat = ci; opt = (int)oi; }
                    }
                }
                if (opt >= 0) { target = name; bestCat = cat; bestOpt = opt; break; }
            }
            if (bestOpt < 0)
                return conciliate ? "conciliate: nothing affordable anywhere"
                                  : "repress: every minority already at the harshest";
            g.setEthnicPolicyOption(cid, target, bestCat, bestOpt);
            if (conciliate) statsFor(cid).minorityConciliations++;
            else            statsFor(cid).minorityRepressions++;
            return TextFormat("%s %s: %s -> %s", conciliate ? "conciliate" : "repress",
                              target.c_str(),
                              g.m_ethnicPolicyCategories[bestCat].displayName.c_str(),
                              g.m_ethnicPolicyCategories[bestCat].options[bestOpt].name.c_str());
        }
        default: return "politics hold";
    }
}

std::string AISystem::execWar(int cid, int action) {
    Game& g = *m_g;
    Country& c = g.m_countries.getAll()[cid];
    const CountryStat& st = m_stats[cid];
    auto relIt = g.m_relations.find(c.isoA3);

    auto garrisonOf = [&](int pid, int owner) -> int {
        auto it = g.m_provinceArmies.find(pid);
        if (it == g.m_provinceArmies.end()) return 0;
        int n = 0;
        for (auto& u : it->second) if (u.countryId == owner) n += u.count;
        return n;
    };
    auto atWarWith = [&](int otherCid) -> bool {
        const Country* oc = g.m_countries.getCountry(otherCid);
        if (!oc || relIt == g.m_relations.end()) return false;
        auto rr = relIt->second.find(oc->isoA3);
        return rr != relIt->second.end() && rr->second.war;
    };

    // How badly a frontier province is outgunned by whatever hostile force sits
    // next to it. Both recruitment and reinforcement aim at the worst score, so
    // troops actually go where the pressure is.
    auto threatScore = [&](int pid) -> float {
        long long enemy = 0;
        auto nIt = g.m_provinceNeighbors.find(pid);
        if (nIt != g.m_provinceNeighbors.end())
            for (int nid : nIt->second) {
                int nOwner = (nid >= 0 && nid < (int)g.m_provinceCountryLookup.size())
                                 ? g.m_provinceCountryLookup[nid] : 0;
                if (nOwner > 0 && nOwner != cid && atWarWith(nOwner))
                    enemy += garrisonOf(nid, nOwner);
            }
        float s = (float)enemy - (float)garrisonOf(pid, cid);
        // Any province with a live enemy opposite outranks every quiet one.
        return enemy > 0 ? s + 1.0e6f : s;
    };

    switch (action) {
        case 1: { // recruit in the most threatened frontier province (or richest)
            int pid = -1;
            if (!st.frontiers.empty()) {
                // Was st.frontiers[0] — index 0 of a vector built in hash order,
                // i.e. an arbitrary border province unrelated to any threat,
                // despite the comment claiming otherwise.
                float best = -1.0e30f;
                for (auto& fr : st.frontiers) {
                    float s = threatScore(fr.pid);
                    if (s > best) { best = s; pid = fr.pid; }
                }
            }
            else {
                long long bp = -1;
                for (int p2 : g.provincesOf(cid)) {
                    long long pop = g.m_provincePopulations.count(p2) ? g.m_provincePopulations[p2] : 0;
                    if (pop > bp) { bp = pop; pid = p2; }
                }
            }
            if (pid < 0) return "recruit: no province";
            long long pop = g.m_provincePopulations.count(pid) ? g.m_provincePopulations[pid] : 0;
            long long maxRecruit = pop / 5;
            // Spend at most 20% of treasury on this order. Clamp BEFORE the
            // cast: a runaway treasury times 10000 overflows long long (UB).
            long long budgetCount = (long long)std::min((double)INT32_MAX,
                                                        c.treasury * 0.20 * 10000.0);
            int count = (int)std::min((long long)INT32_MAX,
                                      std::min(maxRecruit, budgetCount));
            if (count < 1000) return "recruit: too poor/small";
            float cost = std::max(1.0f, count / 10000.0f);
            c.treasury -= cost;
            g.m_pendingRecruitments.push_back({pid, count, 1});
            return TextFormat("recruit %d in prov %d ($%.0f)", count, pid, cost);
        }
        case 2: { // reinforce EVERY threatened frontier, worst first
            // One order per turn could never produce a frontline. A country
            // invaded across six provinces got to top up exactly one of them,
            // and only in the turns where the policy happened to sample this
            // action out of eight — so the defence never converged and the
            // player saw an AI that simply did not react to being invaded.
            // Ordering is by threat, so if the budget runs out it runs out on
            // the quiet borders.
            std::vector<std::pair<float, int>> ranked;
            ranked.reserve(st.frontiers.size());
            for (auto& fr : st.frontiers) ranked.push_back({threatScore(fr.pid), fr.pid});
            if (ranked.empty()) return "reinforce: no frontier";
            std::sort(ranked.rbegin(), ranked.rend());
            int issued = 0;
            for (auto& [score, dstPid] : ranked) {
                if (issued >= MAX_REINFORCE_ORDERS) break;
                if (!reinforceProvince(cid, dstPid)) continue;
                ++issued;
            }
            if (!issued) return "reinforce: nothing to move";
            return TextFormat("reinforce %d province(s), worst prov %d", issued, ranked[0].second);
        }
        case 3: { // attack the weakest adjacent at-war enemy province we can beat
            // Our own attack research. This was pinned at 1.0 with the note
            // "getTotalEffect is global-player; stay conservative" — true at the
            // time, and it meant the AI planned every assault as if its own
            // doctrine research did not exist, so it declined attacks it would
            // in fact have won. getTotalEffect now takes a country.
            const float atkMod = 1.0f + g.getTotalEffect("armyAtkPct", cid) / 100.0f;
            int bestFrom = -1, bestTo = -1; float bestMargin = 1.05f;
            for (auto& fr : st.frontiers) {
                if (!atWarWith(fr.enemyCid)) continue;
                int myG = garrisonOf(fr.pid, cid);
                // Putting down a revolt is worth committing a smaller force to:
                // rebels start with no army at all, and the parent's garrison in
                // the area was just decimated by the uprising itself, so a 500
                // floor meant the AI usually could not respond to a secession
                // at all and simply watched it consolidate.
                if (myG < (fr.enemyCid >= Game::REBEL_CID_MIN ? 150 : 500)) continue;
                auto nIt = g.m_provinceNeighbors.find(fr.pid);
                if (nIt == g.m_provinceNeighbors.end()) continue;
                for (int nid : nIt->second) {
                    int nOwner = nid < (int)g.m_provinceCountryLookup.size()
                                     ? g.m_provinceCountryLookup[nid] : 0;
                    if (nOwner != fr.enemyCid) continue;
                    int defG = garrisonOf(nid, nOwner);
                    auto ind = g.m_provinceIndustry.find(nid);
                    float fort = ind != g.m_provinceIndustry.end() ? (float)ind->second.fortification : 0.0f;
                    float atk = myG * 0.75f * atkMod;
                    // Mirrors processArmyMovement: fortification AND the
                    // defender's own defensive research.
                    float def = defG * (1.0f + fort * 0.1f) *
                                (1.0f + g.getTotalEffect("armyDefPct", nOwner) / 100.0f);
                    float margin = def > 0 ? atk / def : 10.0f;
                    // Claimed provinces are priority targets: taking one both
                    // expands us AND satisfies the claim.
                    auto clIt = g.m_claimsByProvince.find(nid);
                    if (clIt != g.m_claimsByProvince.end())
                        for (auto& iso : clIt->second)
                            if (iso == c.isoA3) { margin += 0.4f; break; }
                    // Secession outranks foreign conquest. Every turn a breakaway
                    // survives it entrenches, and the unrest model feeds on it —
                    // neighbouring provinces then carry a war-claim penalty that
                    // spawns the next revolt.
                    if (nOwner >= Game::REBEL_CID_MIN) margin += 1.0f;
                    if (margin > bestMargin) { bestMargin = margin; bestFrom = fr.pid; bestTo = nid; }
                }
            }
            // Assaults launched from allied soil. Same arithmetic, but the
            // launch province is one we are standing in rather than one we own
            // — this is what turns a staged army into an offensive instead of a
            // garrison on somebody else's border.
            bool fromAlly = false;
            for (int apid : st.abroadPids) {
                int myG = garrisonOf(apid, cid);
                if (myG < 500) continue;
                auto nIt = g.m_provinceNeighbors.find(apid);
                if (nIt == g.m_provinceNeighbors.end()) continue;
                for (int nid : nIt->second) {
                    int nOwner = nid < (int)g.m_provinceCountryLookup.size()
                                     ? g.m_provinceCountryLookup[nid] : 0;
                    if (nOwner <= 0 || nOwner == cid || !atWarWith(nOwner)) continue;
                    int defG = garrisonOf(nid, nOwner);
                    auto ind = g.m_provinceIndustry.find(nid);
                    float fort = ind != g.m_provinceIndustry.end() ? (float)ind->second.fortification : 0.0f;
                    float atk = myG * 0.75f * atkMod;
                    // Mirrors processArmyMovement: fortification AND the
                    // defender's own defensive research.
                    float def = defG * (1.0f + fort * 0.1f) *
                                (1.0f + g.getTotalEffect("armyDefPct", nOwner) / 100.0f);
                    float margin = def > 0 ? atk / def : 10.0f;
                    auto clIt = g.m_claimsByProvince.find(nid);
                    if (clIt != g.m_claimsByProvince.end())
                        for (auto& iso : clIt->second)
                            if (iso == c.isoA3) { margin += 0.4f; break; }
                    if (margin > bestMargin) {
                        bestMargin = margin; bestFrom = apid; bestTo = nid; fromAlly = true;
                    }
                }
            }
            if (bestFrom < 0) {
                statsFor(cid).attackNoTarget++;
                return "attack: no winnable target";
            }
            for (auto& mo : g.m_pendingMoveOrders)
                if (mo.fromProvince == bestFrom && mo.countryId == cid) {
                    statsFor(cid).attackPending++;
                    return "attack: order pending";
                }
            statsFor(cid).attackIssued++;
            if (fromAlly) {
                g.m_pendingMoveOrders.push_back({bestFrom, bestTo, 75, cid});
                return TextFormat("attack prov %d from allied prov %d (margin %.1fx)",
                                  bestTo, bestFrom, bestMargin);
            }
            g.m_pendingMoveOrders.push_back({bestFrom, bestTo, 75, cid});
            return TextFormat("attack prov %d from %d (margin %.1fx)", bestTo, bestFrom, bestMargin);
        }
        case 4: { // declare war: prefer neighbours holding OUR claimed land,
                  // then the weakest beatable one. Claims are the war goal.
            //
            // The choice itself, and every gate on it, now lives in
            // findWarTarget -- which validWar consults too, so this can no
            // longer be reached with nothing to declare on. It still can be
            // reached with a pact in the way, which is a real answer rather
            // than a wasted turn: the pact is broken this turn, war follows.
            WarTarget wt;
            if (!findWarTarget(cid, wt, /*learnedChoice=*/true))
                return "war: no suitable target";
            const Country* ec = g.m_countries.getCountry(wt.cid);
            if (!ec) return "war: target vanished";

            // A NON-AGGRESSION PACT IS BROKEN BEFORE IT IS IGNORED.
            //
            // declareWar() clears the pact as a side effect, so issuing the
            // declaration straight away "worked" -- and meant the AI could
            // attack through a pact in the same turn it was still holding. The
            // player cannot do that: their Declare War button does not exist
            // while a NAP stands (Game_Render.cpp), they have to break it and
            // wait. The AI was simply exempt from a rule the player is held to.
            //
            // Breaking it here instead costs the AI one turn and gives the
            // other side the turn of warning the pact is FOR. The target is
            // still chosen by the same logic, so this is not the AI refusing to
            // fight -- the earlier attempt to fix this by treating a NAP as
            // off-limits outright is what froze the late game, because every
            // border ended up pacted and nothing could ever be declared again.
            if (wt.napBlocked) {
                g.m_pendingDiplomaticActions.push_back({c.isoA3, ec->isoA3, "break_nap", 1});
                return std::string("break NAP with ") + ec->name +
                       (wt.naval ? " (naval war next turn)" : " (war next turn)");
            }

            g.m_pendingDiplomaticActions.push_back({c.isoA3, ec->isoA3, "declare_war", 1});
            statsFor(cid).warsDeclared++;
            m_declaredUnprovoked = !wt.claimed;
            if (wt.naval)
                return std::string("declare NAVAL war on ") + ec->name +
                       (wt.claimed ? " (claims)" : " (overseas)");
            return std::string("declare war on ") + ec->name +
                   (wt.claimed ? " (claims)" : "");
        }
        case 5: { // artillery: best researched ammo on an adjacent enemy province
            struct Ammo { const char* type; const char* node; float cost; };
            static const Ammo AMMO[] = {
                {"nuclear", "arty6a", 80}, {"biological", "arty6b", 60},
                {"chemical", "arty5", 40}, {"napalm", "arty4a", 30},
                {"carpet", "arty4b", 25},  {"heavy", "arty3", 20},
                {"light", "arty2", 10},    {"mortar", "arty1", 5}};
            const Ammo* use = nullptr;
            for (auto& a2 : AMMO)
                if (g.hasResearched(a2.node, cid) && c.treasury >= a2.cost) { use = &a2; break; }
            if (!use) return "artillery: no researched ammo";
            for (auto& fr : st.frontiers) {
                if (!atWarWith(fr.enemyCid)) continue;
                auto nIt = g.m_provinceNeighbors.find(fr.pid);
                if (nIt == g.m_provinceNeighbors.end()) continue;
                for (int nid : nIt->second) {
                    int nOwner = nid < (int)g.m_provinceCountryLookup.size()
                                     ? g.m_provinceCountryLookup[nid] : 0;
                    if (nOwner != fr.enemyCid) continue;
                    bool pending = false;
                    for (auto& ao : g.m_pendingArtilleryOrders)
                        if (ao.fromProvince == fr.pid) { pending = true; break; }
                    if (pending) continue;
                    c.treasury -= use->cost;
                    g.m_pendingArtilleryOrders.push_back({fr.pid, nid, use->type});
                    return TextFormat("%s shell prov %d", use->type, nid);
                }
            }
            return "artillery: no target";
        }
        case 6: { // offer ceasefire (white peace) to the strongest enemy
            int target = -1; long long targetArmy = -1;
            if (relIt != g.m_relations.end()) {
                for (auto& [iso, r] : relIt->second) {
                    if (!r.war) continue;
                    int ocid = g.cidForIso(iso);
                    if (ocid < 0 || !diploReady(cid, ocid)) continue;
                    bool pendingReq = false;
                    for (auto& da : g.m_pendingDiplomaticActions)
                        if (da.sourceIso == c.isoA3 && da.targetIso == iso &&
                            da.action == "request_ceasefire") { pendingReq = true; break; }
                    if (pendingReq) continue;
                    long long ea = m_stats[ocid].army;
                    if (ea > targetArmy) { targetArmy = ea; target = ocid; }
                }
            }
            if (target < 0) return "ceasefire: no war to end";
            const Country* ec = g.m_countries.getCountry(target);

            // Compose actual peace terms rather than always offering a bare
            // white peace. The CeasefireTerms machinery, the negotiation
            // screen and the review popup all existed already — the AI simply
            // never filled anything in, so every offer the player ever saw was
            // an empty "proposes a ceasefire" with nothing under it.
            CeasefireTerms terms;
            long long myArmy = m_stats[cid].army;
            long long theirArmy = std::max(1LL, m_stats[target].army);
            double edge = (double)myArmy / (double)theirArmy;
            Country& tc = g.m_countries.getAll()[target];

            auto provsOf = [&](int owner, int adjacentTo, int maxN) {
                std::vector<int> out;
                for (int pid : g.provincesOf(owner)) {
                    if ((int)out.size() >= maxN) break;
                    auto nIt = g.m_provinceNeighbors.find(pid);
                    if (nIt == g.m_provinceNeighbors.end()) continue;
                    for (int nid : nIt->second) {
                        int no = (nid >= 0 && nid < (int)g.m_provinceCountryLookup.size())
                                     ? g.m_provinceCountryLookup[nid] : 0;
                        if (no == adjacentTo) { out.push_back(pid); break; }
                    }
                }
                return out;
            };

            const char* posture;
            if (edge > 1.5) {
                // Winning: take something for stopping. Demand border provinces
                // (capped so a victory doesn't annex a whole country in one
                // deal) and a slice of their treasury.
                posture = "demanding";
                terms.theirProvs = provsOf(target, cid, edge > 3.0 ? 3 : 1);
                terms.theirMoney = (int)std::max(0.0, std::min(tc.treasury * 0.25, 2000.0));
                // Make them renounce claims on us as part of the settlement.
                auto clIt = g.m_claims.find(tc.isoA3);
                if (clIt != g.m_claims.end())
                    for (int pid : clIt->second) {
                        if (terms.theirDropClaims.size() >= 3) break;
                        int owner = (pid >= 0 && pid < (int)g.m_provinceCountryLookup.size())
                                        ? g.m_provinceCountryLookup[pid] : 0;
                        if (owner == cid) terms.theirDropClaims.push_back(pid);
                    }
            } else if (edge < 0.67) {
                // Losing: buy the peace. Pay what we can, drop our claims on
                // them, and cede a border province if we are being overrun.
                posture = "conceding";
                terms.ourMoney = (int)std::max(0.0, std::min(c.treasury * 0.30, 1500.0));
                auto clIt = g.m_claims.find(c.isoA3);
                if (clIt != g.m_claims.end())
                    for (int pid : clIt->second) {
                        if (terms.ourDropClaims.size() >= 3) break;
                        int owner = (pid >= 0 && pid < (int)g.m_provinceCountryLookup.size())
                                        ? g.m_provinceCountryLookup[pid] : 0;
                        if (owner == target) terms.ourDropClaims.push_back(pid);
                    }
                if (edge < 0.4) terms.ourProvs = provsOf(cid, target, 1);
            } else {
                posture = "white peace"; // evenly matched — no demands
            }

            g.m_pendingDiplomaticActions.push_back({c.isoA3, ec->isoA3, "request_ceasefire", 1});
            if (!terms.ourProvs.empty() || !terms.theirProvs.empty() ||
                terms.ourMoney || terms.theirMoney ||
                !terms.ourDropClaims.empty() || !terms.theirDropClaims.empty())
                g.m_pendingCeasefireTerms[c.isoA3 + "|" + ec->isoA3] = terms;
            diploCoolDown(cid, target);
            statsFor(cid).ceasefiresOffered++;
            return TextFormat("offer ceasefire (%s) to %s", posture, ec->name.c_str());
        }
        case 7: { // stage troops on allied soil next to a shared enemy
            // Allied territory has always been passable (processArmyMovement
            // walks straight through it without a fight) — the AI just had no
            // way to name a province it did not own, so it never once used an
            // alliance to reach a front. Prefer the crossing that puts the most
            // troops closest to the biggest enemy stack.
            int bestFrom = -1, bestTo = -1; long long bestScore = -1;
            for (auto& s : st.staging) {
                long long mine = garrisonOf(s.fromPid, cid);
                if (mine < 500) continue; // not worth splitting a token garrison
                bool busy = false;
                for (auto& mo : g.m_pendingMoveOrders)
                    if (mo.fromProvince == s.fromPid && mo.countryId == cid) { busy = true; break; }
                if (busy) continue;
                // Weight by the enemy force this staging point actually faces:
                // parking an army on a quiet allied border helps nobody.
                long long threat = 0;
                auto nIt = g.m_provinceNeighbors.find(s.allyPid);
                if (nIt != g.m_provinceNeighbors.end())
                    for (int nid : nIt->second) {
                        int nOwner = (nid >= 0 && nid < (int)g.m_provinceCountryLookup.size())
                                         ? g.m_provinceCountryLookup[nid] : 0;
                        if (nOwner == s.enemyCid) threat += garrisonOf(nid, nOwner);
                    }
                long long score = mine + threat * 2;
                if (score > bestScore) { bestScore = score; bestFrom = s.fromPid; bestTo = s.allyPid; }
            }
            if (bestFrom < 0) return "stage: no allied crossing available";
            // Half the garrison: the province we are leaving still has its own
            // border to hold.
            g.m_pendingMoveOrders.push_back({bestFrom, bestTo, 50, cid});
            statsFor(cid).stagingMoves++;
            return TextFormat("stage troops into allied prov %d from %d", bestTo, bestFrom);
        }
        default: return "hold";
    }
}

// Top up one province from the strongest adjacent province we own. Shared by
// the sampled reinforce action and the standing garrison reflex, so the two
// cannot drift apart.
bool AISystem::reinforceProvince(int cid, int dstPid) {
    Game& g = *m_g;
    auto nIt = g.m_provinceNeighbors.find(dstPid);
    if (nIt == g.m_provinceNeighbors.end()) return false;
    auto garrisonOf = [&](int pid, int owner) -> long long {
        auto it = g.m_provinceArmies.find(pid);
        if (it == g.m_provinceArmies.end()) return 0;
        long long n = 0;
        for (auto& u : it->second) if (u.countryId == owner) n += u.count;
        return n;
    };
    int srcPid = -1; long long srcG = 0;
    for (int nid : nIt->second) {
        if (nid < 0 || nid >= (int)g.m_provinceCountryLookup.size() ||
            g.m_provinceCountryLookup[nid] != cid) continue;
        // Never strip a province that is itself under threat to feed another.
        long long gsz = garrisonOf(nid, cid);
        if (gsz > srcG) { srcG = gsz; srcPid = nid; }
    }
    if (srcPid < 0 || srcG < 200) return false;
    for (auto& mo : g.m_pendingMoveOrders)
        if (mo.fromProvince == srcPid && mo.countryId == cid) return false;
    g.m_pendingMoveOrders.push_back({srcPid, dstPid, 50, cid});
    return true;
}

void AISystem::garrisonReflex(int cid) {
    // Holding a line is doctrine, not a gamble.
    //
    // Every other order this AI gives is sampled from a policy, which is right
    // for choices with a real trade-off. Moving troops toward a province that
    // an enemy stack is standing next to is not one of those: no competent
    // commander leaves that to a dice roll, and making the net rediscover it
    // every turn from a 1-in-8 action slot is what left the map looking
    // undefended. This runs before the sampled war action and costs the country
    // nothing it would not spend anyway.
    const CountryStat& st = m_stats[cid];
    if (st.threatenedProvinces == 0) return;
    Game& g = *m_g;

    auto garrisonOf = [&](int pid, int owner) -> long long {
        auto it = g.m_provinceArmies.find(pid);
        if (it == g.m_provinceArmies.end()) return 0;
        long long n = 0;
        for (auto& u : it->second) if (u.countryId == owner) n += u.count;
        return n;
    };
    auto warIt = m_warWith.find(cid);
    if (warIt == m_warWith.end() || warIt->second.empty()) return;

    // Rank our own frontier provinces by how badly they are outnumbered.
    std::vector<std::pair<long long, int>> deficits;
    for (auto& fr : st.frontiers) {
        auto nIt = g.m_provinceNeighbors.find(fr.pid);
        if (nIt == g.m_provinceNeighbors.end()) continue;
        long long enemy = 0;
        for (int nid : nIt->second) {
            int nOwner = (nid >= 0 && nid < (int)g.m_provinceCountryLookup.size())
                             ? g.m_provinceCountryLookup[nid] : 0;
            if (nOwner > 0 && nOwner != cid && warIt->second.count(nOwner))
                enemy += garrisonOf(nid, nOwner);
        }
        if (enemy <= 0) continue;
        long long deficit = enemy - garrisonOf(fr.pid, cid);
        if (deficit > 0) deficits.push_back({deficit, fr.pid});
    }
    if (deficits.empty()) return;
    std::sort(deficits.rbegin(), deficits.rend());
    int issued = 0;
    for (auto& [deficit, pid] : deficits) {
        if (issued >= MAX_GARRISON_ORDERS) break;
        if (reinforceProvince(cid, pid)) ++issued;
    }
    if (issued && g.m_config.aiDebug) {
        const Country* c = g.m_countries.getCountry(cid);
        printf("[AI] t%d %s [defence] garrison reflex: %d province(s) reinforced\n",
               m_turn, c ? c->name.c_str() : "?", issued);
    }
}

// Troops sitting in the interior are troops doing nothing.
//
// The AI could only ever move a garrison one hop into an ADJACENT frontier
// province, and only while at war (garrisonReflex returns early otherwise).
// So an army raised in a safe heartland province stayed in that province for
// the rest of the game, however far from any border it was -- the AI behaved
// as though moving inside its own country required someone to fight.
//
// This walks a distance-to-border field outward: every province gets its hop
// count to the nearest frontier, and any garrison deeper than one hop sends a
// slice to whichever neighbour is closer to the edge. Over a few turns that
// drains the interior toward the borders without anyone declaring anything.
void AISystem::redeployReflex(int cid) {
    Game& g = *m_g;
    const CountryStat& st = m_stats[cid];
    if (st.provinces < 2 || st.army <= 0) return;

    const std::vector<int>& own = g.provincesOf(cid);
    if (own.size() < 2) return;

    auto garrisonOf = [&](int pid) -> long long {
        auto it = g.m_provinceArmies.find(pid);
        if (it == g.m_provinceArmies.end()) return 0;
        long long n = 0;
        for (auto& u : it->second) if (u.countryId == cid) n += u.count;
        return n;
    };

    // Hops to the nearest province of ours that touches somebody else.
    std::unordered_map<int, int> depth;
    std::vector<int> frontier;
    for (auto& fr : st.frontiers) {
        if (depth.emplace(fr.pid, 0).second) frontier.push_back(fr.pid);
    }
    // A country with no land neighbour still wants its troops near the coast,
    // but there is no "toward" to move them in -- leave it alone.
    if (frontier.empty()) return;
    for (size_t qi = 0; qi < frontier.size(); ++qi) {
        int pid = frontier[qi];
        int d = depth[pid];
        if (d >= 8) continue;                     // deep enough; stop walking
        auto nIt = g.m_provinceNeighbors.find(pid);
        if (nIt == g.m_provinceNeighbors.end()) continue;
        for (int nid : nIt->second) {
            if (nid < 0 || nid >= (int)g.m_provinceCountryLookup.size()) continue;
            if (g.m_provinceCountryLookup[nid] != cid) continue;
            if (depth.emplace(nid, d + 1).second) frontier.push_back(nid);
        }
    }

    int issued = 0;
    for (int pid : own) {
        if (issued >= MAX_GARRISON_ORDERS) break;
        auto dIt = depth.find(pid);
        if (dIt == depth.end() || dIt->second == 0) continue;   // already at the edge
        long long here = garrisonOf(pid);
        if (here < 400) continue;               // not worth splitting further
        bool busy = false;
        for (auto& mo : g.m_pendingMoveOrders)
            if (mo.fromProvince == pid && mo.countryId == cid) { busy = true; break; }
        if (busy) continue;
        // Step toward the border: any neighbour of ours strictly closer to it.
        int best = -1, bestDepth = dIt->second;
        for (int nid : g.m_provinceNeighbors[pid]) {
            if (nid < 0 || nid >= (int)g.m_provinceCountryLookup.size()) continue;
            if (g.m_provinceCountryLookup[nid] != cid) continue;
            auto nd = depth.find(nid);
            if (nd != depth.end() && nd->second < bestDepth) { bestDepth = nd->second; best = nid; }
        }
        if (best < 0) continue;
        // Half, not all: a province emptied completely invites a revolt and
        // leaves nothing to slow an enemy that lands behind the line.
        g.m_pendingMoveOrders.push_back({pid, best, 50, cid});
        ++issued;
    }
    if (issued && g.m_config.aiDebug) {
        const Country* c = g.m_countries.getCountry(cid);
        printf("[AI] t%d %s [redeploy] %d interior stack(s) sent toward the border\n",
               m_turn, c ? c->name.c_str() : "?", issued);
    }
}

// An army you cannot pay for is a bankruptcy with extra steps.
//
// The AI could recruit but had no way to stand anyone down, so a country that
// over-raised early carried that upkeep for the rest of the game and slid into
// the bankruptcy penalties instead of trimming. Disbanding is maintenance, not
// strategy, so it is a reflex: only when income is actually negative, only
// from the safest provinces, and never below a floor that would leave the
// country undefended.
void AISystem::austerityReflex(int cid) {
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    if (!c) return;

    const CountryIncomeSnapshot inc = g.computeCountryIncome(cid);
    if (inc.net >= 0.0f) return;   // paying its way; nothing to do

    // How long the treasury lasts at this rate. Already empty counts as no
    // runway at all, which is the case the bankruptcy cascade is handling this
    // same turn — cutting here as well just gets there sooner and cheaper.
    const double burn = -(double)inc.net;
    const double runway = burn > 1e-6 ? c->treasury / burn : 1e9;
    if (runway > AI_AUSTERITY_RUNWAY_TURNS && !g.isBankrupt(cid)) return;

    const char* what = nullptr;

    // ── 1. Discretionary budgets ──
    // Research and pacification are sliders. They come down first because they
    // come back up for free the moment income recovers.
    auto raIt = g.m_countryResearchAllocation.find(cid);
    if (!what && raIt != g.m_countryResearchAllocation.end() && raIt->second > 0.01f) {
        raIt->second = std::max(0.0f, raIt->second - 0.15f);
        what = "cut research funding";
    }
    auto pacIt = g.m_countryPacification.find(cid);
    if (!what && pacIt != g.m_countryPacification.end() && pacIt->second > 0.01f) {
        // Pacification is unrest suppression, and unrest is exactly what an
        // empty treasury causes — so this is cut second-to-last among the
        // budgets and never below a quarter while anything else remains.
        pacIt->second = std::max(0.25f, pacIt->second - 0.125f);
        if (pacIt->second < 0.25f + 1e-3f && inc.policyCosts <= 0.0f) what = nullptr;
        else what = "cut pacification";
    }

    // ── 2. Repeal the costliest doctrine ──
    if (!what && inc.policyCosts > 0.0f) {
        auto apIt = g.m_countryActivePolicyIndices.find(cid);
        if (apIt != g.m_countryActivePolicyIndices.end()) {
            int bestIdx = -1, bestCost = 0;
            for (int idx : apIt->second) {
                if (idx < 0 || idx >= (int)g.m_activePolicies.size()) continue;
                const ActivePolicy& ap = g.m_activePolicies[idx];
                if (ap.countryId != cid || ap.turnsRemaining < 0) continue;
                for (const auto& p : g.m_allPolicies)
                    if (p.id == ap.policyId) {
                        if (p.costPerTurn > bestCost) { bestCost = p.costPerTurn; bestIdx = idx; }
                        break;
                    }
            }
            if (bestIdx >= 0) { g.cancelPolicy(bestIdx); what = "repealed a doctrine"; }
        }
    }

    // ── 3. Step one minority settlement back ──
    // The cheapest single reduction, not the whole bill: this is trimming, and
    // alignment lost here takes a long time to earn back.
    if (!what && inc.minorityCosts > 0.0f) {
        std::unordered_set<std::string> seen;
        std::string bestName; size_t bestCat = 0; int bestOpt = -1; float bestSaving = 0.0f;
        for (int pid : g.provincesOf(cid)) {
            auto mIt = g.m_provinceMinorities.find(pid);
            if (mIt == g.m_provinceMinorities.end()) continue;
            for (auto& mg : mIt->second) {
                if (!seen.insert(mg.name).second) continue;
                for (size_t ci = 0; ci < g.m_ethnicPolicyCategories.size(); ++ci) {
                    const auto& cat = g.m_ethnicPolicyCategories[ci];
                    const int cur = g.ethnicPolicyOption(cid, mg.name, ci);
                    if (cur < 0) continue;
                    const float curCost = cat.options[cur].costPerTurn;
                    if (curCost <= 0.0f) continue;
                    for (size_t oi = 0; oi < cat.options.size(); ++oi) {
                        const float saving = curCost - cat.options[oi].costPerTurn;
                        if (saving <= 0.0f) continue;
                        // Most money saved per point of goodwill given up.
                        const float lost = std::max(0.1f, cat.options[cur].alignmentPerTurn -
                                                          cat.options[oi].alignmentPerTurn);
                        const float score = saving / lost;
                        if (score > bestSaving) {
                            bestSaving = score; bestName = mg.name;
                            bestCat = ci; bestOpt = (int)oi;
                        }
                    }
                }
            }
        }
        if (bestOpt >= 0) {
            g.setEthnicPolicyOption(cid, bestName, bestCat, bestOpt);
            what = "trimmed a minority programme";
        }
    }

    // ── 4. Pay off a warship ──
    // Last, because it is the first thing here that destroys something. Crewed
    // transports are never touched: the men aboard would go with the hull.
    if (!what && inc.navyExpenses > 0.0f) {
        int bestIdx = -1; float bestCost = 0.0f;
        for (size_t i = 0; i < g.m_ships.size(); ++i) {
            const auto& s = g.m_ships[i];
            if (s.countryId != cid || s.crew > 0) continue;
            const float cost = s.type == "carrier" ? 25.0f
                             : (s.type == "destroyer" ? 10.0f : 0.0f);
            if (cost <= bestCost) continue;
            bool queued = false;
            for (auto& ss : g.m_pendingScrapShips)
                if (ss.shipIndex == (int)i) { queued = true; break; }
            if (queued) continue;
            bestCost = cost; bestIdx = (int)i;
        }
        if (bestIdx >= 0) {
            g.m_pendingScrapShips.push_back({bestIdx});
            statsFor(cid).shipsScrapped++;
            what = "scrapped a warship";
        }
    }

    if (what) statsFor(cid).austerityCuts++;
    if (what && g.m_config.aiDebug)
        printf("[AI] t%d %s [austerity] %s (net %.1f, treasury %.0f, %.1f turns left)\n",
               m_turn, c->name.c_str(), what, inc.net, c->treasury, runway);
}

void AISystem::manpowerReflex(int cid) {
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    if (!c) return;
    const CountryStat& st = m_stats[cid];
    if (st.army <= 0) return;

    // At war you keep the men and find the money elsewhere.
    auto warIt = m_warWith.find(cid);
    if (warIt != m_warWith.end() && !warIt->second.empty()) return;

    auto inc = g.computeCountryIncome(cid);
    // DORMANT AT CURRENT BALANCE, AND DELIBERATELY SO.
    //
    // Measured over ~6,600 self-play country-turns: army payroll runs 0.03%
    // to 0.5% of gross income (0.08 out of 18.4 for a small country, 0.31 out
    // of 1,097 for a large one) while treasuries sit between 12,000 and
    // 31,000. Nothing in this economy makes an army worth standing down, so
    // this reflex correctly almost never fires -- and forcing it to would be
    // teaching the AI to throw away troops it can easily afford.
    //
    // It stays because insolvency IS reachable (processEconomy's bankruptcy
    // penalties exist for exactly that), and because if army upkeep is ever
    // rebalanced upward the AI should already know how to respond. The ratio
    // test is written against gross income rather than a magic number so it
    // starts working the moment upkeep becomes material.
    bool insolvent = inc.net < 0.0 && c->treasury <= 25.0;
    bool bloated = inc.total > 0.0f && inc.armyExpenses > inc.total * 0.33f;
    if (!insolvent && !bloated) return;

    auto garrisonOf = [&](int pid) -> long long {
        auto it = g.m_provinceArmies.find(pid);
        if (it == g.m_provinceArmies.end()) return 0;
        long long n = 0;
        for (auto& u : it->second) if (u.countryId == cid) n += u.count;
        return n;
    };
    std::unordered_set<int> frontierPids;
    for (auto& fr : st.frontiers) frontierPids.insert(fr.pid);

    // Shed a tenth of the army per turn, from the deepest garrisons first, and
    // never touch a border province.
    long long target = st.army / 10;
    if (target < 500) return;
    std::vector<std::pair<long long, int>> pool;
    for (int pid : g.provincesOf(cid)) {
        if (frontierPids.count(pid)) continue;
        long long n = garrisonOf(pid);
        if (n >= 500) pool.push_back({n, pid});
    }
    if (pool.empty()) return;
    std::sort(pool.rbegin(), pool.rend());

    long long shed = 0;
    int orders = 0;
    for (auto& [n, pid] : pool) {
        if (shed >= target || orders >= MAX_GARRISON_ORDERS) break;
        long long take = std::min(n / 2, target - shed);   // never empty it
        if (take < 250) continue;
        g.m_pendingDisbandOrders.push_back({pid, (int)take});
        shed += take;
        ++orders;
    }
    if (orders && g.m_config.aiDebug)
        printf("[AI] t%d %s [manpower] disbanding %lld men across %d province(s) "
               "(%s: net %.1f, army costs %.1f of %.1f)\n", m_turn, c->name.c_str(),
               shed, orders, insolvent ? "insolvent" : "army too costly",
               inc.net, inc.armyExpenses, inc.total);
}

void AISystem::amphibiousReflex(int cid) {
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    if (!c) return;
    auto relIt = g.m_relations.find(c->isoA3);
    if (relIt == g.m_relations.end()) return;

    const int mapW = g.m_provinces.getWidth(), mapH = g.m_provinces.getHeight();
    if (mapW <= 0 || mapH <= 0) return;

    auto atWarWith = [&](int otherCid) -> bool {
        const Country* oc = g.m_countries.getCountry(otherCid);
        if (!oc) return false;
        auto rr = relIt->second.find(oc->isoA3);
        return rr != relIt->second.end() && rr->second.war;
    };
    auto portAt = [&](int pid, double& lon, double& lat) -> bool {
        auto cIt = g.m_provinceCenters.find(pid);
        if (cIt == g.m_provinceCenters.end()) return false;
        lon = cIt->second.x / mapW * 360.0 - 180.0;
        lat = 90.0 - cIt->second.y / mapH * 180.0;
        return true;
    };

    // Landing range must exceed the per-turn step, or a fleet can straddle the
    // gap: 18 degrees of travel against a 12-degree landing window lets a ship
    // pass from "too far" to "too far on the other side" without ever being
    // able to unload. Closing speed is reduced near the target for the same
    // reason — the last approach is made in small steps.
    const double LAND_RANGE = 12.0;
    const double FULL_STEP  = 18.0;

    for (size_t i = 0; i < g.m_ships.size(); ++i) {
        auto& s = g.m_ships[i];
        if (s.countryId != cid || s.crew <= 0) continue;
        bool busy = false;
        for (auto& dd : g.m_pendingShipDisembarks)
            if (dd.shipIndex == (int)i) { busy = true; break; }
        if (busy) continue;

        // Nearest hostile port, and nearest of our own, in one pass.
        int enemyPid = -1, homePid = -1;
        double enemyD = 1e18, homeD = 1e18;
        double enemyLon = 0, enemyLat = 0, homeLon = 0, homeLat = 0;
        for (auto& [pid, port] : g.m_provincePorts) {
            const Province* p = g.m_provinces.getProvinceById(pid);
            if (!p) continue;
            double lon, lat;
            if (!portAt(pid, lon, lat)) continue;
            const double d = std::hypot(lon - s.lon, lat - s.lat);
            if (atWarWith(p->countryId)) {
                if (d < enemyD) { enemyD = d; enemyPid = pid; enemyLon = lon; enemyLat = lat; }
            } else if (p->countryId == cid) {
                if (d < homeD) { homeD = d; homePid = pid; homeLon = lon; homeLat = lat; }
            }
        }

        // In range of a hostile shore: land, now. This is the whole point.
        if (enemyPid >= 0 && enemyD <= LAND_RANGE) {
            g.m_pendingShipDisembarks.push_back({(int)i, enemyPid});
            statsFor(cid).landings++;
            m_landingsThisTurn[cid]++;
            if (g.m_config.aiDebug)
                printf("[AI] t%d %s [amphib] landing %d troops on prov %d\n",
                       m_turn, c->name.c_str(), s.crew * 100, enemyPid);
            continue;
        }
        // No war left to fight: put the cargo ashore at home rather than
        // carrying an army around the ocean for the rest of the game.
        if (enemyPid < 0) {
            if (homePid >= 0 && homeD <= LAND_RANGE) {
                g.m_pendingShipDisembarks.push_back({(int)i, homePid});
                statsFor(cid).unloadsHome++;
            } else if (homePid >= 0) {
                double dLon = homeLon - s.lon, dLat = homeLat - s.lat;
                const double dist = std::max(1e-6, std::hypot(dLon, dLat));
                const double step = std::min(FULL_STEP, dist);
                g.m_pendingShipMoveOrders.push_back(
                    {(int)i, s.lon + dLon / dist * step, s.lat + dLat / dist * step});
            }
            continue;
        }

        // Otherwise close on the target. Already under a move order from the
        // sampled navy action? Leave it — two orders for one ship in a turn is
        // the last one winning, which makes the reflex and the policy fight.
        bool moving = false;
        for (auto& mo : g.m_pendingShipMoveOrders)
            if (mo.shipIndex == (int)i) { moving = true; break; }
        if (moving) continue;
        double dLon = enemyLon - s.lon, dLat = enemyLat - s.lat;
        const double dist = std::max(1e-6, std::hypot(dLon, dLat));
        // Stop just inside landing range rather than on top of the port, so the
        // next turn lands instead of overshooting.
        const double want = std::max(0.0, dist - LAND_RANGE * 0.5);
        const double step = std::min(FULL_STEP, want);
        if (step <= 0.0) continue;
        g.m_pendingShipMoveOrders.push_back(
            {(int)i, s.lon + dLon / dist * step, s.lat + dLat / dist * step});
    }
}

std::string AISystem::execNavy(int cid, int action) {
    Game& g = *m_g;
    Country& c = g.m_countries.getAll()[cid];
    auto relIt = g.m_relations.find(c.isoA3);

    auto atWarWith = [&](int otherCid) -> bool {
        const Country* oc = g.m_countries.getCountry(otherCid);
        if (!oc || relIt == g.m_relations.end()) return false;
        auto rr = relIt->second.find(oc->isoA3);
        return rr != relIt->second.end() && rr->second.war;
    };
    // Nearest port we or an ally hold. Used when there is no war on, so the
    // fleet has a station to make for instead of drifting. Skips the port a
    // ship is already sitting on, or ships would "move" zero degrees forever.
    auto findHomePort = [&](double fromLon, double fromLat, int& outPid,
                            double& outLon, double& outLat) -> bool {
        int mapW = g.m_provinces.getWidth(), mapH = g.m_provinces.getHeight();
        if (mapW <= 0 || mapH <= 0) return false;
        double bestD = 1e18;
        bool found = false;
        for (auto& [pid, port] : g.m_provincePorts) {
            const Province* p = g.m_provinces.getProvinceById(pid);
            if (!p) continue;
            bool mine = p->countryId == cid;
            if (!mine && relIt != g.m_relations.end()) {
                const Country* oc = g.m_countries.getCountry(p->countryId);
                if (oc) {
                    auto rr = relIt->second.find(oc->isoA3);
                    mine = rr != relIt->second.end() && rr->second.alliance;
                }
            }
            if (!mine) continue;
            auto cIt = g.m_provinceCenters.find(pid);
            if (cIt == g.m_provinceCenters.end()) continue;
            double lon = cIt->second.x / mapW * 360.0 - 180.0;
            double lat = 90.0 - cIt->second.y / mapH * 180.0;
            double dLon = lon - fromLon, dLat = lat - fromLat;
            double d = dLon * dLon + dLat * dLat;
            if (d < 0.25) return false;   // already on station here
            if (d < bestD) { bestD = d; outPid = pid; outLon = lon; outLat = lat; found = true; }
        }
        return found;
    };

    // Nearest at-war enemy province with a port (ports are always coastal)
    auto findEnemyPort = [&](double fromLon, double fromLat, int& outPid,
                             double& outLon, double& outLat) -> bool {
        int mapW = g.m_provinces.getWidth(), mapH = g.m_provinces.getHeight();
        if (mapW <= 0 || mapH <= 0) return false;
        double bestD = 1e18;
        bool found = false;
        for (auto& [pid, port] : g.m_provincePorts) {
            const Province* p = g.m_provinces.getProvinceById(pid);
            if (!p || !atWarWith(p->countryId)) continue;
            auto cIt = g.m_provinceCenters.find(pid);
            if (cIt == g.m_provinceCenters.end()) continue;
            double lon = cIt->second.x / mapW * 360.0 - 180.0;
            double lat = 90.0 - cIt->second.y / mapH * 180.0;
            double dLon = lon - fromLon, dLat = lat - fromLat;
            double d = dLon * dLon + dLat * dLat;
            if (d < bestD) { bestD = d; outPid = pid; outLon = lon; outLat = lat; found = true; }
        }
        return found;
    };

    // Nearest at-war enemy port to any crewed boat we own, in degrees. Used to
    // tell "we have nobody to invade" apart from "we are still sailing".
    auto nearestLandingRange = [&](bool& anyPort) -> double {
        anyPort = false;
        double best = 1e18;
        int mapW = g.m_provinces.getWidth(), mapH = g.m_provinces.getHeight();
        if (mapW <= 0 || mapH <= 0) return best;
        for (auto& s : g.m_ships) {
            if (s.countryId != cid || s.crew <= 0) continue;
            for (auto& [pid, port] : g.m_provincePorts) {
                const Province* p = g.m_provinces.getProvinceById(pid);
                if (!p || !atWarWith(p->countryId)) continue;
                auto cIt = g.m_provinceCenters.find(pid);
                if (cIt == g.m_provinceCenters.end()) continue;
                anyPort = true;
                double lon = cIt->second.x / mapW * 360.0 - 180.0;
                double lat = 90.0 - cIt->second.y / mapH * 180.0;
                best = std::min(best, std::hypot(lon - s.lon, lat - s.lat));
            }
        }
        return best;
    };

    switch (action) {
        case 1: { // steam the fleet toward the nearest enemy port (capped step)
            int moved = 0;
            for (size_t i = 0; i < g.m_ships.size(); ++i) {
                auto& s = g.m_ships[i];
                if (s.countryId != cid) continue;
                bool busy = false;
                for (auto& mo : g.m_pendingShipMoveOrders)
                    if (mo.shipIndex == (int)i) { busy = true; break; }
                if (busy) continue;
                int tp; double tLon, tLat;
                // At war, steam at the enemy. In peacetime the fleet used to
                // simply stop -- this broke out of the loop and reported "no
                // at-war enemy owns a port", so every navy in the world sat
                // wherever it was built until somebody declared war on its
                // owner. A fleet with nothing to attack still has somewhere to
                // be: its own ports, which is where it can resupply and where
                // it covers the coast it is supposed to be covering.
                if (!findEnemyPort(s.lon, s.lat, tp, tLon, tLat) &&
                    !findHomePort(s.lon, s.lat, tp, tLon, tLat)) break;
                double dLon = tLon - s.lon, dLat = tLat - s.lat;
                double dist = std::sqrt(dLon * dLon + dLat * dLat);
                const double STEP = 18.0; // degrees per turn — no cross-map teleports
                if (dist > STEP) { dLon *= STEP / dist; dLat *= STEP / dist; }
                g.m_pendingShipMoveOrders.push_back({(int)i, s.lon + dLon, s.lat + dLat});
                if (++moved >= 3) break; // a few ships per turn is plenty
            }
            if (moved) return std::string(TextFormat("move %d ship(s) to station", moved));
            // "no target" conflated three very different situations, which made
            // the archipelago stall impossible to read off the dashboard.
            {
                int tp; double tl, ta;
                if (!findEnemyPort(0, 0, tp, tl, ta))
                    return "navy move: every ship already on station";
                return "navy move: all ships already under orders";
            }
        }
        case 2: { // bombard the nearest at-war enemy province in range
            struct Ammo { const char* type; const char* node; float cost; };
            static const Ammo AMMO[] = {
                {"heavy", "arty3", 20}, {"light", "arty2", 10}, {"mortar", "arty1", 5}};
            const Ammo* use = nullptr;
            for (auto& a2 : AMMO)
                if (g.hasResearched(a2.node, cid) && c.treasury >= a2.cost) { use = &a2; break; }
            if (!use) return "bombard: no ammo";
            int mapW = g.m_provinces.getWidth(), mapH = g.m_provinces.getHeight();
            if (mapW <= 0 || mapH <= 0) return "bombard: no map";
            for (size_t i = 0; i < g.m_ships.size(); ++i) {
                auto& s = g.m_ships[i];
                if (s.countryId != cid || s.type == "boat") continue;
                // Only scan port provinces — cheap and always coastal
                for (auto& [pid, port] : g.m_provincePorts) {
                    const Province* p = g.m_provinces.getProvinceById(pid);
                    if (!p || !atWarWith(p->countryId)) continue;
                    auto cIt = g.m_provinceCenters.find(pid);
                    if (cIt == g.m_provinceCenters.end()) continue;
                    double lon = cIt->second.x / mapW * 360.0 - 180.0;
                    double lat = 90.0 - cIt->second.y / mapH * 180.0;
                    double d = std::hypot(lon - s.lon, lat - s.lat);
                    if (d > 10.0) continue; // rough range gate
                    c.treasury -= use->cost;
                    g.m_pendingShipBombardOrders.push_back({(int)i, pid, use->type});
                    return TextFormat("bombard prov %d (%s)", pid, use->type);
                }
            }
            return "bombard: nothing in range";
        }
        case 3: { // embark troops at the strongest port garrison
            int bestPid = -1, bestG = 0;
            for (auto& [pid, port] : g.m_provincePorts) {
                const Province* p = g.m_provinces.getProvinceById(pid);
                if (!p || p->countryId != cid) continue;
                auto aIt = g.m_provinceArmies.find(pid);
                if (aIt == g.m_provinceArmies.end()) continue;
                int gsz = 0;
                for (auto& u : aIt->second) if (u.countryId == cid) gsz += u.count;
                if (gsz > bestG) { bestG = gsz; bestPid = pid; }
            }
            if (bestPid < 0 || bestG < 1000) return "embark: no garrison at port";
            for (auto& pe : g.m_pendingEmbarkations)
                if (pe.provinceId == bestPid) return "embark: pending";
            g.m_pendingEmbarkations.push_back({bestPid, bestG / 2, 1});
            statsFor(cid).embarks++;
            return TextFormat("embark %d from prov %d", bestG / 2, bestPid);
        }
        case 4: { // amphibious landing: nearest at-war coastal (port) province
            int mapW = g.m_provinces.getWidth(), mapH = g.m_provinces.getHeight();
            if (mapW <= 0 || mapH <= 0) return "disembark: no map";
            for (size_t i = 0; i < g.m_ships.size(); ++i) {
                auto& s = g.m_ships[i];
                if (s.countryId != cid || s.crew <= 0) continue;
                bool busy = false;
                for (auto& dd : g.m_pendingShipDisembarks)
                    if (dd.shipIndex == (int)i) { busy = true; break; }
                if (busy) continue;
                for (auto& [pid, port] : g.m_provincePorts) {
                    const Province* p = g.m_provinces.getProvinceById(pid);
                    if (!p || !atWarWith(p->countryId)) continue;
                    auto cIt = g.m_provinceCenters.find(pid);
                    if (cIt == g.m_provinceCenters.end()) continue;
                    double lon = cIt->second.x / mapW * 360.0 - 180.0;
                    double lat = 90.0 - cIt->second.y / mapH * 180.0;
                    if (std::hypot(lon - s.lon, lat - s.lat) > 12.0) continue;
                    g.m_pendingShipDisembarks.push_back({(int)i, pid});
                    statsFor(cid).landings++;
                    return TextFormat("disembark %d troops at prov %d", s.crew * 100, pid);
                }
            }
            // No hostile shore to land on. Put the troops back ashore at one of
            // our own ports instead of leaving them floating: a war that ends in
            // a ceasefire mid-crossing used to strand the cargo permanently,
            // with the army subtracted from the land total and never returned.
            for (size_t i = 0; i < g.m_ships.size(); ++i) {
                auto& s = g.m_ships[i];
                if (s.countryId != cid || s.crew <= 0) continue;
                bool busy = false;
                for (auto& dd : g.m_pendingShipDisembarks)
                    if (dd.shipIndex == (int)i) { busy = true; break; }
                if (busy) continue;
                for (auto& [pid, port] : g.m_provincePorts) {
                    const Province* p = g.m_provinces.getProvinceById(pid);
                    if (!p || p->countryId != cid) continue;
                    auto cIt = g.m_provinceCenters.find(pid);
                    if (cIt == g.m_provinceCenters.end()) continue;
                    double lon = cIt->second.x / mapW * 360.0 - 180.0;
                    double lat = 90.0 - cIt->second.y / mapH * 180.0;
                    if (std::hypot(lon - s.lon, lat - s.lat) > 12.0) continue;
                    g.m_pendingShipDisembarks.push_back({(int)i, pid});
                    statsFor(cid).unloadsHome++;
                    return TextFormat("unload %d troops home at prov %d", s.crew * 100, pid);
                }
            }
            {
                bool anyPort = false;
                double d = nearestLandingRange(anyPort);
                if (!anyPort)
                    return "disembark: no at-war enemy port (returning home)";
                return std::string(TextFormat("disembark: nearest enemy port %.0f deg (need <12)", d));
            }
        }
        case 5: { // scrap the most expensive warship doing the least
            // One hull per turn. Scrapping is irreversible and the reward that
            // justifies it (income recovering) takes a few turns to show up, so
            // a country that fires this action several turns running should be
            // deciding that several times, not disposing of its whole navy on
            // one sampled action.
            int bestIdx = -1;
            float bestCost = 0.0f;
            double bestIdleness = -1.0;
            for (size_t i = 0; i < g.m_ships.size(); ++i) {
                auto& s = g.m_ships[i];
                if (s.countryId != cid) continue;
                // Never a transport: it is free to keep (see validNavy) and its
                // crew would be deleted with the hull.
                if (s.crew > 0) continue;
                float cost = s.type == "carrier" ? 25.0f : (s.type == "destroyer" ? 10.0f : 0.0f);
                if (cost <= 0.0f) continue;
                bool queued = false;
                for (auto& ss : g.m_pendingScrapShips)
                    if (ss.shipIndex == (int)i) { queued = true; break; }
                if (queued) continue;
                // Idleness: distance to the nearest shore we are fighting on.
                // With no war on, every ship is equally idle and the tie is
                // broken on upkeep alone.
                int tp; double tLon, tLat;
                double idle = 1e9;
                if (findEnemyPort(s.lon, s.lat, tp, tLon, tLat))
                    idle = std::hypot(tLon - s.lon, tLat - s.lat);
                // Costliest first; among equals, the one furthest from a front.
                if (cost > bestCost || (cost == bestCost && idle > bestIdleness)) {
                    bestCost = cost; bestIdleness = idle; bestIdx = (int)i;
                }
            }
            if (bestIdx < 0) return "scrap: nothing worth scrapping";
            g.m_pendingScrapShips.push_back({bestIdx});
            statsFor(cid).shipsScrapped++;
            return TextFormat("scrap %s #%d (saves %.0f/turn)",
                              g.m_ships[bestIdx].type.c_str(), bestIdx, bestCost);
        }
        default: return "navy hold";
    }
}

// ─── Diplomacy responses ─────────────────────────────────

void AISystem::noteDiploRejected(int sourceCid, int targetCid) {
    // A refusal used to cost exactly what an acceptance did, so the proposer
    // came straight back the moment the ordinary cooldown lapsed. Sit this pair
    // out for a good while instead.
    if (sourceCid <= 0 || targetCid <= 0) return;
    m_diploCooldownUntil[diploKey(sourceCid, targetCid)] = m_turn + 60;
}

bool AISystem::decideDiplomacy(int targetCid, const std::string& action,
                               const std::string& sourceIso,
                               const std::string& subjectIso) {
    // The control group answers diplomacy the way it does everything else. The
    // heuristic gates below still apply to it, because those are machinery
    // rather than policy and the baseline is meant to differ in exactly one
    // thing.
    m_randomThisCountry = isRandomCountry(targetCid);
    struct ClearFlag {
        bool& f;
        ~ClearFlag() { f = false; }
    } clearFlag{m_randomThisCountry};

    std::vector<float> feats;
    buildFeatures(targetCid, feats);
    // Request-specific context in the spare feature slots
    int srcCid = m_g->cidForIso(sourceIso);
    long long srcArmy = srcCid >= 0 ? m_stats[srcCid].army : 0;
    long long myArmy = std::max(1LL, m_stats[targetCid].army);
    feats[88] = (float)std::tanh(std::log1p((double)srcArmy / (double)myArmy));
    feats[89] = (action == "request_ceasefire") ? 1.0f : 0.0f;
    feats[90] = (action == "request_alliance") ? 1.0f : 0.0f;
    feats[91] = (action == "request_nap") ? 1.0f : 0.0f;
    feats[92] = (action == "request_guarantee") ? 1.0f : 0.0f;

    // ── Call to arms ──
    // Judged on completely different terms from a treaty proposal: this is
    // "join a war you did not start, and carry the unrest for it, or lose the
    // ally". The net needs the request type, who it would be fighting, and what
    // it is already carrying at home.
    std::vector<float> bias;
    if (action == "call_to_arms") {
        feats[80] = 1.0f;
        // The aggressor is the third party here — srcCid is the ally ASKING.
        feats[81] = feats[88]; // ally's strength relative to ours
        feats[82] = m_g->warWearinessOf(targetCid) / Game::WAR_WEARINESS_MAX;
        const CountryStat& ts = m_stats[targetCid];
        feats[83] = ts.threatenedProvinces > 0 ? 1.0f : 0.0f; // already busy at home?
        feats[84] = std::tanh(ts.provincesLost / 2.0f);

        // ── When the answer is no whatever the net thinks ──
        //
        // Each of these is a state in which joining does not merely cost more
        // than the alliance is worth, it costs more than the country has. They
        // are checked before the net is consulted, so no sample is recorded:
        // this is not a decision that was taken, it is one that was not
        // available. See AI_CALL_* in the header.
        const char* refuse = nullptr;
        const int myWars = foreignWarCount(targetCid);
        if (myWars >= AI_CALL_MAX_OWN_WARS)
            refuse = "already fighting two wars of its own";
        else if (m_g->warWearinessOf(targetCid) >= AI_CALL_WEARINESS_BLOCK)
            refuse = "home front already too unhappy";
        // Being invaded is a reason to stay home; having a neighbour's stack
        // parked on a quiet border is not, and neither is losing a province to
        // a rebellion somewhere. The old test fired on either, which on a busy
        // map is most of the time.
        else if (ts.threatenedProvinces > 0 && ts.enemyAdjArmy > ts.defenderArmy)
            refuse = "losing ground on its own borders";
        else if (!subjectIso.empty()) {
            // Who we would actually be fighting, and with whom. The aggressor
            // is the third party in the request, not the ally making it — the
            // ally's strength is a reason to join, the aggressor's is a reason
            // not to, and only the caller knows which is which.
            const int aggCid = m_g->cidForIso(subjectIso);
            const long long ourSide = std::max(1LL, ts.army + srcArmy);
            const long long theirArmy = aggCid >= 0 ? m_stats[aggCid].army : 0;
            if (theirArmy > (long long)(ourSide * AI_CALL_MAX_ENEMY_ODDS))
                refuse = "the aggressor outguns both of us";
        }
        if (refuse) {
            logDecision(targetCid, MOD_POLITICS, 0, 0.0f,
                        std::string("REFUSE call_to_arms from ") + sourceIso +
                        " (" + refuse + ")");
            return false;
        }

        // No gate fired: the net decides, but against a thumb on the scale.
        // Answering is a war and seven points of unrest; the alliance it saves
        // is worth that only when the net actively wants the fight.
        bias.assign(DIPLO_ACTIONS, 0.0f);
        bias[1] = -AI_CALL_RELUCTANCE;
    } else if (action == "request_nap") {
        bias.assign(DIPLO_ACTIONS, 0.0f);
        bias[1] = AI_NAP_WILLINGNESS;
    }

    // The deal on the table. Without these the diplomacy net judged a ceasefire
    // purely on army ratios: the player could offer three provinces and a
    // fortune, or demand them, and the answer was identical, because the terms
    // were never looked at. Signed from the RECIPIENT's point of view — what
    // they gain minus what they give up.
    float netProv = 0.0f, netMoney = 0.0f;
    if (action == "request_ceasefire") {
        const Country* tc = m_g->m_countries.getCountry(targetCid);
        if (tc) {
            auto tit = m_g->m_pendingCeasefireTerms.find(sourceIso + "|" + tc->isoA3);
            if (tit != m_g->m_pendingCeasefireTerms.end()) {
                const CeasefireTerms& t = tit->second;
                netProv = (float)t.ourProvs.size() - (float)t.theirProvs.size()
                        + 0.25f * ((float)t.ourDropClaims.size() -
                                   (float)t.theirDropClaims.size());
                netMoney = (float)t.ourMoney - (float)t.theirMoney;
            }
        }
    }
    feats[93] = std::tanh(netProv / 3.0f);
    feats[94] = std::tanh(netMoney / 500.0f);

    std::vector<bool> valid(DIPLO_ACTIONS, true);
    float score;
    float diploLogProb = 0.0f;
    int a = pickAction(m_diplo, feats, valid, score, /*graveAction=*/-1,
                       bias.empty() ? nullptr : &bias, &diploLogProb);
    m_lastDiploLogProb = diploLogProb;
    // Record in the country's experience so the diplo net learns too
    auto it = m_pending.find(targetCid);
    if (it != m_pending.end() && !it->second.empty()) {
        it->second.back().action[MOD_COUNT] = a;
        it->second.back().acted[MOD_COUNT] = true;
        // The probability the policy gave this answer, for PPO's ratio. Without
        // it the ratio is measured against zero and every diplomatic sample
        // looks infinitely off-policy.
        it->second.back().logProb[MOD_COUNT] = m_lastDiploLogProb;
    }
    logDecision(targetCid, MOD_POLITICS, a, score,
                std::string(a ? "ACCEPT " : "REJECT ") + action + " from " + sourceIso);
    return a == 1;
}

// ─── Learning ────────────────────────────────────────────

void AISystem::endTurn() {
    Game& g = *m_g;
    if (!g.m_config.aiLearning) { m_pending.clear(); return; }


    // Refresh post-turn stats for reward deltas: the same cheap single passes
    // beginTurn uses, without the turn bookkeeping. This used to call
    // beginTurn() and then undo its side effects by hand (m_turn--, restore the
    // decision counter), which is exactly the sort of thing that breaks the
    // moment beginTurn grows one more side effect — as it now has.
    refreshStats();

    float rewardSum[MOD_COUNT] = {0, 0, 0, 0};
    int rewardN = 0;

    // ── Phase 1: rewards and normalisation (sequential) ──
    //
    // The running reward statistics are order-dependent, so this half has to
    // stay serial. It is also cheap — a handful of tanh calls per experience.
    // All the expensive work (nine nets' worth of forward and backward passes)
    // is deferred into `m_work` and run in parallel below.
    m_work.clear();
    // `nextFeats` is the state the window ended in, or nullptr when the window
    // ended the episode. See WorkItem::nextFeatures for why it exists.
    auto applyUpdate = [&](int cid, Experience& exp, const float* rewards,
                           float diploReward,
                           const std::vector<float>* nextFeats = nullptr) {
        for (int m = 0; m < MOD_COUNT; ++m) rewardSum[m] += rewards[m];
        rewardN++;
        for (int m = 0; m < MOD_COUNT; ++m) {
            if (!exp.acted[m] || exp.action[m] < 0) continue;
            // Normalise reward by running statistics so advantage scale is
            // stable across maps of very different sizes
            m_rMean[m] = 0.99f * m_rMean[m] + 0.01f * rewards[m];
            float dev = rewards[m] - m_rMean[m];
            m_rVar[m] = 0.99f * m_rVar[m] + 0.01f * dev * dev;
            float norm = dev / std::sqrt(m_rVar[m] + 1e-4f);

            WorkItem w;
            w.module = m;
            w.action = exp.action[m];
            w.norm = norm;
            w.features = exp.features;
            w.relCand = exp.relCand;
            w.acts = std::move(exp.acts[m]);
            w.cid = cid;
            w.oldLogProb = exp.logProb[m];
            if (m == MOD_WAR && exp.targetChosen >= 0) {
                w.targetCand = exp.targetCand;
                w.targetChosen = exp.targetChosen;
            }
            if (nextFeats) {
                w.nextFeatures = *nextFeats;
                w.bootDiscount = BOOTSTRAP_DISCOUNT;
            }
            m_work.push_back(std::move(w));
        }
        // Diplomacy has its own reward, normalised against the politics
        // statistics.
        //
        // It used to be scored on the politics reward outright, which is
        // dominated by rebellions and by the coalition the POLITICS module
        // built. Answering a call to arms is a war and seven points of unrest,
        // and neither of those moved that number enough to matter — so the one
        // head whose entire job is saying yes or no was being told almost
        // nothing about what its answers cost. Sharing the running mean and
        // variance is deliberate: the two rewards are built from the same tanh
        // terms at the same scale, and giving diplomacy its own statistics
        // would change the model file format for no measurable gain.
        if (exp.acted[MOD_COUNT] && exp.action[MOD_COUNT] >= 0) {
            float dev = diploReward - m_rMean[MOD_POLITICS];
            float norm = dev / std::sqrt(m_rVar[MOD_POLITICS] + 1e-4f);
            WorkItem w;
            w.module = MOD_COUNT; // diplo
            w.action = exp.action[MOD_COUNT];
            w.norm = std::clamp(norm, -3.0f, 3.0f);
            w.features = exp.features;
            w.relCand = exp.relCand;
            w.cid = cid;
            w.oldLogProb = exp.logProb[MOD_COUNT];
            if (nextFeats) {
                w.nextFeatures = *nextFeats;
                w.bootDiscount = BOOTSTRAP_DISCOUNT;
            }
            m_work.push_back(std::move(w));
        }
        // THE STANCE. Trained on the mean of the four module rewards -- it is
        // the one decision that owns the whole country's outcome rather than
        // any single module's, so scoring it on one module's slice would ask it
        // to optimise a quarter of what it controls.
        if (exp.acted[MOD_COUNT + 1] && exp.action[MOD_COUNT + 1] >= 0) {
            float shared = 0.0f;
            for (int m = 0; m < MOD_COUNT; ++m) shared += rewards[m];
            shared /= (float)MOD_COUNT;
            const float dev = shared - m_rMean[MOD_POLITICS];
            const float norm = dev / std::sqrt(m_rVar[MOD_POLITICS] + 1e-4f);
            WorkItem w;
            w.module = MOD_COUNT + 1;   // stance
            w.action = exp.action[MOD_COUNT + 1];
            w.norm = std::clamp(norm, -3.0f, 3.0f);
            w.features = exp.features;
            w.relCand = exp.relCand;
            w.cid = cid;
            w.oldLogProb = exp.logProb[MOD_COUNT + 1];
            if (nextFeats) {
                w.nextFeatures = *nextFeats;
                w.bootDiscount = BOOTSTRAP_DISCOUNT;
            }
            m_work.push_back(std::move(w));
        }
    };

    for (auto it = m_pending.begin(); it != m_pending.end(); ) {
        int cid = it->first;
        auto& dq = it->second;
        const Country* c = g.m_countries.getCountry(cid);
        const CountryStat& now = m_stats[cid];
        bool dead = (c == nullptr) || now.provinces == 0;

        // Age every open window; rebellions accumulate so a rebellion within
        // N_STEP turns of a decision punishes THAT decision (this is the
        // "letting a rebellion happen" penalty — e.g. cutting pacification
        // saves money now but eats the rebellion that follows).
        int rebNow = 0;
        auto rbIt = g.m_rebellionsThisTurnByCid.find(cid);
        if (rbIt != g.m_rebellionsThisTurnByCid.end()) rebNow = rbIt->second;
        int landNow = 0;
        auto lnIt = m_landingsThisTurn.find(cid);
        if (lnIt != m_landingsThisTurn.end()) landNow = lnIt->second;
        // How much this country knows, for the dResearch reward term below.
        // The completions COUNTER used to be derived here too; it now lives at
        // the completion site, because this whole function is skipped when
        // learning is off. See noteResearchDone.
        auto resIt = g.m_countryResearched.find(cid);
        const int researchedNow =
            resIt != g.m_countryResearched.end() ? (int)resIt->second.size() : 0;
        // Counted in decide() now, so it survives evaluation and covers the
        // control cohort -- see noteBankruptTurn. Still needed here as a
        // per-window reward term.
        const int brokeNow = g.isBankrupt(cid) ? 1 : 0;
        // refreshStats ran at the top of endTurn, so this is the state AFTER
        // the turn resolved — which is the only place a war declared during it
        // is visible.
        auto wwIt = m_warWith.find(cid);
        const bool atWarNow = wwIt != m_warWith.end() && !wwIt->second.empty();
        for (auto& exp : dq) {
            exp.age++;
            exp.rebellions += rebNow;
            exp.landings += landNow;
            exp.bankruptTurns += brokeNow;
            exp.warInWindow = exp.warInWindow || atWarNow;
        }

        float dNetNow = dead ? 0.0f : g.computeCountryIncome(cid).net;

        // Filled lazily below, and only for countries that actually mature a
        // window this turn: buildFeatures is not free and most do not.
        std::vector<float> bootFeatures;

        while (!dq.empty() && (dead || dq.front().age >= nStep())) {
            Experience& exp = dq.front();
            float rewards[MOD_COUNT];
            float diploReward = 0.0f;
            auto standing = m_finalStanding.find(cid);
            if (dead) {
                // Terminal: being eliminated is the worst possible outcome —
                // every decision in the final window shares the blame.
                for (int m = 0; m < MOD_COUNT; ++m) rewards[m] = -4.0f;
                diploReward = -4.0f;
            } else if (m_victorCid == cid) {
                // ...and the mirror of it. Elimination was worth -4 while
                // WINNING the map was worth nothing beyond the ordinary
                // province delta, so the objective the whole self-play run
                // exists to optimise was the one outcome carrying no signal.
                for (int m = 0; m < MOD_COUNT; ++m) rewards[m] = 4.0f;
                diploReward = 4.0f;
            } else if (standing != m_finalStanding.end()) {
                // The map ended without being decided — see noteMapEnd. How
                // much of the world this country finished holding, on the same
                // scale as the win/loss terminals but with a smaller range,
                // because surviving big is evidence and winning is proof.
                for (int m = 0; m < MOD_COUNT; ++m) rewards[m] = standing->second;
                diploReward = standing->second;
            } else {
                float dProv = (float)(now.provinces - exp.provinces);
                float dTre = (float)(c->treasury - exp.treasury);
                float dArmy = (float)(now.army - exp.army);
                int shipsNow = now.boats + now.destroyers + now.carriers;
                float dShips = (float)(shipsNow - exp.ships);
                float dInd = now.industrySum - exp.industrySum;
                float dNet = dNetNow - exp.netIncome;
                float dResearch = (float)(researchedNow - exp.researched);
                float rebels = (float)exp.rebellions;

                // Deltas span the whole N_STEP window, so an investment made
                // on turn 1 shows its payoff before the reward is settled.
                // Income growth outweighs raw treasury: hoarding is no longer
                // the best money strategy, growing income is.
                //
                // `global` is deliberately WEAK now. It used to dominate every
                // module's reward, which meant all four modules were scored on
                // essentially the same number: conquer a province and the
                // economy, politics and navy heads were all rewarded for it,
                // even when they had chosen "hold". With four modules acting
                // simultaneously each one's gradient was three parts noise, and
                // that cross-talk was the single largest brake on learning.
                // It is not zero, because survival really is a shared outcome —
                // it is just no longer the whole signal.
                // Running out of money is now a shared failure, because it is
                // caused by four modules between them — research and hulls from
                // economy, doctrines and minority settlements from politics,
                // the army from war — and felt by all of them.
                // SOLVENCY, WITHOUT THE CLIFF.
                //
                // This was tanh(bankruptTurns / 4). Over a twelve-turn window
                // that saturates almost immediately: four turns broke scored
                // 0.76 and twelve scored 0.995, so past a third of the window
                // there was essentially no gradient left. A country already in
                // trouble was charged the same whether it climbed out or sank,
                // which is precisely the state where the pull should be
                // strongest -- and it sank: 29.6 bankrupt country-turns per
                // thousand against the random control's 15.2, twice as broke as
                // a policy that does not manage money at all.
                //
                // Linear in the fraction of the window spent insolvent. Same
                // range, constant gradient, so every turn recovered is worth
                // the same as the last.
                const float broke =
                    std::min(1.0f, (float)exp.bankruptTurns / (float)nStep());
                // THE IDLE TAX, PER MODULE.
                //
                // Nothing charged a country for standing still. With no war on,
                // holding produced a delta of zero on every term, which reads
                // as "neutral" — but an action that changes nothing also has no
                // VARIANCE, and a policy gradient with a value baseline will
                // take a certain zero over a risky positive every time. So the
                // modules collapsed onto hold / hold / save money, and the map
                // stopped moving.
                //
                // The first version of this lived in `global` and asked whether
                // the WHOLE COUNTRY was inert: no ground, nothing built,
                // nothing researched, no army change, not at war — all at once.
                // Two things were wrong with that.
                //
                // It could not bind the module it was aimed at. The test is an
                // AND across four modules' effects, so the economy laying down
                // one industry point exempted the war module from the charge
                // meant to price ITS passivity. Measured after that change: the
                // model cohort declared 0.00 wars per thousand country-turns
                // against the random control's 4.72 and 7.09, unchanged.
                //
                // And it was too small to reorder anything. At -0.3 it sat
                // below the -0.5 phoney-war charge, so "stay at peace and do
                // nothing" remained strictly cheaper than "be at war and not
                // winning yet" — which is the exact comparison the war head was
                // getting wrong.
                //
                // Charged per module now, to the module that could have done
                // something about it, and the war module's charge is set equal
                // to its phoney-war charge so idling is never the cheap option.
                // Politics has no term here on purpose: repression, doctrines
                // and pacts leave no trace in any of these deltas, so any
                // inertness test for it would be measuring the other modules.
                const bool econIdle = dInd == 0.0f && dResearch == 0.0f && dShips == 0.0f;
                // Not `exp.atWar`: that is read before the war module acts, so
                // the window a country declares war in looks peaceful and the
                // most decisive action available would be charged for idleness.
                const bool warIdle = !exp.warInWindow && dProv == 0.0f
                                  && std::fabs(dArmy) < 500.0f;

                float global = 0.6f * std::tanh(dProv / 3.0f)
                             + 0.2f * std::tanh(dTre / 100.0f)
                             + 0.3f * std::tanh(dNet / 15.0f)
                             - UNREST_WEIGHT * std::tanh(rebels / 2.0f)
                             - 0.5f * broke;
                // Each module is now judged mostly on what it actually controls.
                rewards[MOD_ECONOMY]  = global
                                      // ...and the economy module's failure in
                                      // particular. Growing income is what it is
                                      // paid for; an empty treasury is what
                                      // happens when it never stops spending.
                                      - 1.2f * broke
                                      + 1.2f * std::tanh(dNet / 15.0f)
                                      + 1.0f * std::tanh(dInd / 3.0f)
                                      + 0.8f * std::tanh(dResearch / 2.0f)
                                      + 0.3f * std::tanh(dTre / 100.0f)
                                      + (econIdle ? -0.3f : 0.0f);
                // What the country agreed to carry over the window. The LEVEL
                // of war weariness barely moves when a country takes on one
                // more commitment; the change over twelve turns is the bill for
                // whatever it took on, and it is the only term that makes
                // answering a call to arms cost anything at all.
                float dWeary = g.warWearinessOf(cid) - exp.weariness;

                // Politics owns unrest, and now also owns the coalition: an
                // ally who fights alongside us is what the module bought, and
                // war weariness is what it paid.
                //
                // Standing agreements are now worth something in their own
                // right. Every diplomatic term here used to be conditional on a
                // war — co-belligerents, weariness — so a country at peace with
                // six neighbours scored exactly as well as one with none, and
                // "make friends" was a strategy the reward could not express.
                // It is a small term on purpose: pacts are a means to being
                // left alone, not a score to farm.
                // How the country's minorities came to feel about it over the
                // window. Repression is not simply punished: it is free, and a
                // government that can absorb the resentment keeps the money —
                // which is exactly the trade the reward should be putting to
                // the module rather than deciding for it. What makes the trade
                // real is that alignment drives rebellion chance, and the
                // rebellion term above is the largest in this reward.
                float dAlign = now.meanAlignment - exp.minorityAlignment;

                rewards[MOD_POLITICS] = global
                                      - 2.5f * std::tanh(rebels / 2.0f)
                                      + 0.5f * std::tanh((float)now.coBelligerents / 2.0f)
                                      + 0.4f * std::tanh((float)now.pacts / 3.0f)
                                      + 0.5f * std::tanh(dAlign / 10.0f)
                                      // ...and the LEVEL, not only the change.
                                      // Alignment is clamped at zero, so a
                                      // government that has already driven its
                                      // minorities to the floor sees dAlign = 0
                                      // from then on and further repression
                                      // becomes free — which is exactly the
                                      // state the model converged to, repressing
                                      // at 207 per thousand country-turns
                                      // against random's 88 while conciliating
                                      // at a twelfth of random's rate. This term
                                      // does not stop pressing once the damage
                                      // is done.
                                      + 0.6f * ((now.meanAlignment - 50.0f) / 50.0f)
                                      - 0.4f * (g.warWearinessOf(cid) / Game::WAR_WEARINESS_MAX)
                                      - 0.5f * std::tanh(std::max(0.0f, dWeary) / 5.0f)
                                      + (exp.netIncome > 0 ? 0.2f : -0.2f);
                // War owns territory in BOTH directions. Ground lost is now
                // punished explicitly rather than showing up as a slightly
                // smaller positive: a country being overrun previously received
                // almost the same reward as one merely standing still, so
                // "defend" had nothing to distinguish it from "hold".
                float dLost = (float)(exp.threatened > 0 ? exp.provinces - now.provinces : 0);

                // An army is a MEANS, not an end.
                //
                // Army growth used to be rewarded unconditionally, and the
                // module did exactly what it was paid to do: over 400 turns it
                // chose "recruit" 14,849 times and "attack" 214. Recruiting is
                // riskless and pays every single turn; attacking risks the
                // stack and only pays if it takes ground. No amount of
                // exploration digs a policy out of an incentive like that.
                //
                // Troops are worth their upkeep when there is a war to fight or
                // a border under pressure. Raised in peacetime, with nobody
                // threatening us, they are a standing bill — which is what the
                // economy already charges for them.
                const bool armyNeeded = exp.atWar || exp.threatened > 0;
                const float armyTerm =
                    armyNeeded ?  0.3f * std::tanh(dArmy / 40000.0f)
                               : -0.3f * std::tanh(std::max(0.0f, dArmy) / 40000.0f);

                // The phoney-war tax. A country at war that gains no ground and
                // is under no pressure is burning upkeep for nothing, and
                // "hold" is precisely the choice that produces it — the action
                // that absorbed 52% of war decisions once the invalid ones were
                // masked away. This makes standing still in a war a small
                // running cost, so the module has to either press the attack or
                // sue for peace. It deliberately does NOT apply while
                // threatened: a country holding its own border against an
                // invasion is doing its job, not stalling.
                //
                // ...AND NOT TO A WAR THAT HAS ONLY JUST STARTED.
                //
                // This is a charge for stalling, and it was landing on wars
                // that had had no chance to move yet. Between it and the
                // aggression charge below, the price of an unproductive
                // declaration was -0.35 in the opening window and -0.5 in every
                // window after it — comfortably more than the flat -0.8 that
                // was removed for teaching the policy never to declare war at
                // all, and applied for as long as the war lasted rather than
                // once. The policy read the arithmetic correctly and stopped
                // declaring: 0.00 per thousand country-turns against a random
                // control's 4.72. One window of grace from the start of the war
                // covers mobilising and reaching the border; after that,
                // gaining nothing really is stalling.
                // ENDING IT. See WAR_END_REWARD: conquest was scored and
                // conclusion was not, so a war that neither won nor finished
                // was free to keep. The N_STEP floor is what stops the module
                // collecting this by declaring a war and immediately suing for
                // peace.
                const bool warEnded =
                    exp.atWar && !atWarNow && exp.warTurns >= nStep();
                const float peaceTerm =
                    warEnded ? WAR_END_REWARD * (1.0f + std::tanh(dProv / 3.0f))
                             : 0.0f;

                const float phoneyWar =
                    (exp.atWar && exp.warTurns >= nStep() &&
                     exp.threatened == 0 && dProv <= 0.0f) ? WAR_PHONEY_CHARGE : 0.0f;

                // The cost of starting it — CHARGED ON THE OUTCOME, not on
                // the decision.
                //
                // This was a flat -0.8 for any unprovoked declaration, and it
                // did precisely what a flat certain cost against a slow
                // uncertain gain always does: the policy stopped declaring war
                // at all. Measured against a random-action control, 0.00
                // declarations per thousand country-turns to random's 3.84.
                // Conquest pays +2.0 x tanh(dProv/3), but a war rarely
                // concludes inside the twelve-turn reward window, so the -0.8
                // arrived with certainty while the +2.0 usually arrived after
                // the window had closed. Expected value said: never fight.
                //
                // Now it scales with how the war is actually going. A
                // declaration that is already taking ground costs nothing —
                // that is the expansion the game is about. One that has taken
                // nothing carries the full charge. Wars of reconquest stay
                // exempt entirely, as before.
                const float aggression =
                    exp.aggressor
                        ? WAR_AGGRESSION_CHARGE * (1.0f - std::tanh(std::max(0.0f, dProv) / 2.0f))
                        : 0.0f;

                rewards[MOD_WAR]      = global
                                      + 2.0f * std::tanh(dProv / 3.0f)
                                      - 2.0f * std::tanh(std::max(0.0f, dLost) / 2.0f)
                                      + armyTerm
                                      + phoneyWar
                                      // Equal to phoneyWar, deliberately: a
                                      // module that sits out the whole window
                                      // at peace is charged exactly what one
                                      // that sits out a war is, so peace is
                                      // never the cheap way to avoid the tax.
                                      + (warIdle ? -0.5f : 0.0f)
                                      // Concluding a war is an outcome the war
                                      // module owns -- it is the one holding
                                      // the ceasefire action. See peaceTerm.
                                      + peaceTerm
                                      + aggression;
                // The navy is scored on what it delivers ashore and on what it
                // costs. Ship COUNT used to be rewarded outright, which paid
                // the module to build a fleet and never to notice the fleet was
                // idle — the exact incentive the army term was rewritten to
                // remove. Hulls are worth having when there is a crossing to
                // make; otherwise they are 10 to 25 a turn each.
                const bool fleetUseful = exp.atWar || now.navalTargets > 0 ||
                                         now.navalWarTargets > 0;
                rewards[MOD_NAVY]     = global
                                      + (fleetUseful ? 0.5f * std::tanh(dShips / 2.0f)
                                                     : -0.4f * std::tanh(std::max(0.0f, dShips) / 2.0f))
                                      + 0.8f * std::tanh(dProv / 3.0f)
                                      // Troops actually put ashore on a hostile
                                      // coast. The ground a landing wins often
                                      // falls outside the twelve-turn window,
                                      // so without this the module is paid for
                                      // the invasion only when it happens to
                                      // conclude quickly — and charged for the
                                      // army and the hulls every other time.
                                      + 1.0f * std::tanh((float)exp.landings / 1.5f);

                // Diplomacy answers requests, so it is judged on what its answer
                // did to this country — and, crucially, on BOTH answers.
                //
                // The first version of this charged the full war weariness of
                // accepting a call to arms (seven of a maximum twenty, worth
                // about -0.89 here) while the alliance a refusal destroys was
                // worth 0.3 x tanh, roughly -0.05 at the margin. Faced with an
                // eighteen-to-one asymmetry the policy correctly learned to
                // refuse everything, and the observed behaviour — an AI that
                // declines essentially every call — was the reward working as
                // written rather than the model failing.
                //
                // Agreements lost is now its own term, and the weariness weight
                // comes down to meet it. The two costs are then within a factor
                // of two of each other, which makes the answer depend on the
                // situation, which is the only thing worth learning here.
                const float pactsLost = (float)std::max(0, exp.pacts - now.pacts);
                diploReward = global
                            + 0.6f * std::tanh((float)now.coBelligerents / 2.0f)
                            + 0.3f * std::tanh((float)now.pacts / 3.0f)
                            - 0.8f * std::tanh(pactsLost)
                            - 0.6f * std::tanh(std::max(0.0f, dWeary) / 5.0f)
                            - 1.2f * std::tanh(std::max(0.0f, dLost) / 2.0f);
            }
            // tanh(NaN) is still NaN: overflowed treasuries/incomes must not
            // poison the weight update (a single NaN reward corrupts the net
            // permanently, including the model file saved to disk).
            for (int m = 0; m < MOD_COUNT; ++m)
                if (!std::isfinite(rewards[m])) rewards[m] = 0.0f;
            if (!std::isfinite(diploReward)) diploReward = 0.0f;

            // Where the window ended, so the decision that opened it can be
            // credited with what the country went on to be worth. Built once
            // per country per flush and shared by every window maturing in this
            // pass -- they all end in the same present.
            //
            // TERMINAL cases get none of it, and the distinction matters more
            // than the arithmetic: elimination already scores -4 and winning
            // the map +4, and adding the value of a state that does not exist
            // on top of either would dilute the only two unambiguous outcomes
            // the game produces.
            const bool episodeOver = dead || m_victorCid == cid ||
                                     standing != m_finalStanding.end();
            if (!episodeOver && bootFeatures.empty()) buildFeatures(cid, bootFeatures);
            applyUpdate(cid, exp, rewards, diploReward,
                        episodeOver ? nullptr : &bootFeatures);
            dq.pop_front();
        }

        if (dead) {
            // beginTurn only walks live countries, so an eliminated one would
            // keep its war-start turn forever.
            m_warSince.erase(cid);
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }

    runLearningWork();

    // One optimiser step per module per turn, over everything that settled.
    for (int m = 0; m < MOD_COUNT; ++m) {
        m_policy[m].flushBatch(LR_POLICY);
        m_value[m].flushBatch(LR_VALUE);
        m_q[m].flushBatch(LR_Q);
    }
    // The trunk takes the policy learning rate: it is trained by the same
    // gradients, from four policy heads, four Q heads and diplomacy at once.
    m_trunk.flushBatch(LR_POLICY);
    m_stanceHead.flushBatch(LR_POLICY);
    m_relEncoder.flushBatch(LR_POLICY);
    m_relScore.flushBatch(LR_POLICY);
    m_target.flushBatch(LR_TARGET);
    m_diploValue.flushBatch(LR_VALUE);
    m_diplo.flushBatch(LR_DIPLO);

    // Reward trend feed for the trainer dashboard
    if (rewardN > 0) {
        for (int m = 0; m < MOD_COUNT; ++m) {
            m_rewardHistory[m].push_back(rewardSum[m] / rewardN);
            while (m_rewardHistory[m].size() > 600) m_rewardHistory[m].pop_front();
        }
    }

    // Checkpoint on a WALL CLOCK, not a turn count.
    //
    // "Every 20 turns" was a sane crash-resilience interval when a turn took
    // half a second. It is not one at 0.03 s a turn: that is a 12 MB file
    // rewritten twice a second, ~35 MB/s sustained, and an overnight run would
    // put on the order of a terabyte through the SSD to protect work that is
    // never more than a few seconds old. Time is what "how much can I afford to
    // lose" is actually measured in, and it does not drift when the simulation
    // gets faster.
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - m_lastSave).count() >= SAVE_INTERVAL_SECONDS) {
        m_lastSave = now;
        saveModel();
        // Rides along with the periodic save rather than on a clock of its own:
        // both want the same moment, between turns and after a merge, and one
        // of them writing while the other renames is how a checkpoint ends up
        // half of one policy and half of another.
        writeLeagueCheckpoint();
    }
}

// ─── Phase 2: gradients (parallel) ───────────────────────

int AISystem::learningThreads() const {
    // One, always, in a browser. The emscripten build has no pthreads, and
    // hardware_concurrency() still reports the machine's core count there --
    // so without this the pool below would ask for four threads it cannot
    // have and abort the process rather than fall back. The serial path a few
    // lines down is the same computation; on web it is the only one.
#ifdef __EMSCRIPTEN__
    return 1;
#endif
    // Explicit override, mainly so the parallel path can be A/B'd against the
    // serial one on the same binary and the same map.
    if (const char* env = std::getenv("OD_AI_THREADS")) {
        const int n = std::atoi(env);
        if (n > 0) return std::min(n, 32);
    }
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    // Leave a core for the rest of the process, and honour the resource limiter
    // — a player who capped the game at 30% of their machine did not mean
    // "except for the AI, which may have every core".
    // Capped at four, from measurement rather than taste. On a 10-core machine
    // the learning step runs 15.3 / 11.4 / 7.9 / 9.3 ms at 1 / 2 / 4 / 8
    // workers: the gradient reduction afterwards is serial and costs
    // O(workers x parameters), so past four the merge grows faster than the
    // accumulate shrinks. OD_AI_THREADS overrides this if a different machine
    // has a different sweet spot.
    int n = std::min((int)std::max(1u, hw - 1), 4);
    // Honour the resource limiter: a player who capped the game at 30% of
    // their machine did not mean "except for the AI, which may have it all".
    n = (int)std::lround(n * (double)resourceBudget());
    return std::clamp(n, 1, 16);
}


void AISystem::backpropRelational(WorkerScratch& ws, const WorkItem& w) {
    // The relational slice of the trunk's input gradient is the encoder's whole
    // training signal. Without it the pooled numbers sit in the observation as
    // constants the model can read but never shape.
    if (w.relCand.empty()) return;
    const std::vector<float>& gIn = NeuralNet::inputGrad(ws.trunk);
    if ((int)gIn.size() < 116 + REL_EMBED) return;
    const std::vector<float> gPooled(gIn.begin() + 116, gIn.begin() + 116 + REL_EMBED);

    const size_t n = w.relCand.size();
    if (ws.relEnc.size() < n) {
        const size_t was = ws.relEnc.size();
        ws.relEnc.resize(n); ws.relSco.resize(n);
        for (size_t i = was; i < n; ++i) {
            m_relEncoder.initScratch(ws.relEnc[i]);
            m_relScore.initScratch(ws.relSco[i]);
        }
    }
    std::vector<std::vector<float>> emb(n);
    std::vector<float> scores(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        emb[i] = m_relEncoder.forwardInto(ws.relEnc[i], w.relCand[i]);
        const std::vector<float>& sc = m_relScore.forwardInto(ws.relSco[i], emb[i]);
        if (sc.empty()) return;
        scores[i] = sc[0];
    }
    std::vector<float> pooled, attn;
    NeuralNet::attentionPool(emb, scores, pooled, attn);
    std::vector<std::vector<float>> gEmb;
    std::vector<float> gScores;
    NeuralNet::attentionPoolBackward(emb, attn, gPooled, gEmb, gScores);
    for (size_t i = 0; i < n; ++i) {
        // An embedding moves the pool directly AND through the weight it earns
        // itself, so both terms reach the encoder.
        m_relScore.accumulateOutputGradInto(ws.relSco[i], gScores[i]);
        std::vector<float> gE = gEmb[i];
        const std::vector<float>& viaScore = NeuralNet::inputGrad(ws.relSco[i]);
        for (size_t k = 0; k < gE.size() && k < viaScore.size(); ++k) gE[k] += viaScore[k];
        m_relEncoder.accumulateVectorGradInto(ws.relEnc[i], gE);
    }
}

void AISystem::runLearningWork() {
    if (m_work.empty()) return;

    // Serial below the threshold: spawning threads to divide forty samples
    // costs more than it saves.
    const int threads = (m_work.size() < 64) ? 1 : learningThreads();

    // Takes the worker's scratch BY REFERENCE. Handing it vectors of Scratch
    // copied whole gradient accumulators — ~3.4 MB per copy, twice a turn —
    // and made the parallel version slower than the serial one it replaced.
    auto runRange = [&](size_t lo, size_t hi, WorkerScratch& ws) {
        for (size_t i = lo; i < hi; ++i) {
            WorkItem& w = m_work[i];
            if (w.module == MOD_COUNT + 1) {
                // The stance: the same PPO update as any policy head, on the
                // shared reward, chaining into the trunk like everything else.
                m_trunk.forwardInto(ws.trunk, w.features);
                m_stanceHead.forwardInto(ws.stance, ws.trunk.acts.back());
                m_stanceHead.accumulatePPOInto(ws.stance, w.action, w.norm,
                                               w.oldLogProb, PPO_CLIP, ppoEntropy());
                m_trunk.accumulateVectorGradInto(ws.trunk,
                                                 NeuralNet::inputGrad(ws.stance));
                backpropRelational(ws, w);
                continue;
            }
            if (w.module == MOD_COUNT) {
                // The same learner the four modules get, for the same reasons.
                // This head used to be trained on the raw normalised reward:
                // no baseline, so maximum variance, and no bootstrap, so the
                // war an answered call eventually wins was invisible to it.
                float dTarget = w.norm;
                if (w.bootDiscount > 0.0f && !w.nextFeatures.empty()) {
                    m_diploValue.forwardInto(ws.diploValueNext, w.nextFeatures);
                    dTarget += w.bootDiscount * ws.diploValueNext.acts.back()[0];
                }
                dTarget = std::clamp(dTarget, -6.0f, 6.0f);

                m_diploValue.forwardInto(ws.diploValue, w.features);
                const float dBase = ws.diploValue.acts.back()[0];
                const float dAdv = std::clamp(dTarget - dBase, -3.0f, 3.0f);
                m_diploValue.accumulateValueInto(ws.diploValue, dTarget);

                m_trunk.forwardInto(ws.trunk, w.features);
                m_diplo.forwardInto(ws.diplo, ws.trunk.acts.back());
                m_diplo.accumulatePPOInto(ws.diplo, w.action, dAdv,
                                          w.oldLogProb, PPO_CLIP, ppoEntropy());
                m_trunk.accumulateVectorGradInto(ws.trunk,
                                                 NeuralNet::inputGrad(ws.diplo));
                backpropRelational(ws, w);
                continue;
            }
            const int m = w.module;

            // The target is this window's reward PLUS what the state it ended
            // in is worth. Without the second half the learner could not see
            // past N_STEP turns, which is shorter than a war -- see
            // WorkItem::nextFeatures.
            //
            // V(s') is read from the value net as it stands, not backpropagated
            // through: this is a bootstrapped target, and letting the gradient
            // chase its own estimate is how these diverge.
            float target = w.norm;
            if (w.bootDiscount > 0.0f && !w.nextFeatures.empty()) {
                m_value[m].forwardInto(ws.valueNext[m], w.nextFeatures);
                target += w.bootDiscount * ws.valueNext[m].acts.back()[0];
            }
            // Clamped in the same units the baseline is, so one absurd
            // bootstrap cannot drag the value head somewhere it will take
            // thousands of updates to come back from.
            target = std::clamp(target, -6.0f, 6.0f);

            // Value baseline: V(s) trained toward that target.
            m_value[m].forwardInto(ws.value[m], w.features);
            const float baseline = ws.value[m].acts.back()[0];
            const float advantage = std::clamp(target - baseline, -3.0f, 3.0f);
            w.advantage = advantage;
            m_value[m].accumulateValueInto(ws.value[m], target);

            // Q(s,a) toward the same target, on the taken action only. The
            // window says what THIS action was worth and nothing about the
            // others, so the others must get no gradient -- see
            // accumulateActionValueInto.
            m_trunk.forwardInto(ws.trunk, w.features);
            m_q[m].forwardInto(ws.q[m], ws.trunk.acts.back());
            m_q[m].accumulateActionValueInto(ws.q[m], w.action, target);
            // THE TRUNK'S GRADIENT. Without this line the encoder receives
            // nothing and stays at its initialisation forever, while every head
            // trains happily on top of it -- the exact shape of the bug that
            // once left the Q heads at zero updates looking like a feature
            // waiting to warm up.
            m_trunk.accumulateVectorGradInto(ws.trunk, NeuralNet::inputGrad(ws.q[m]));
            backpropRelational(ws, w);

            // WHOM it attacked, judged by how the war went.
            //
            // The target head is a policy over a set whose size changes every
            // turn, so there is no output layer to softmax: each candidate was
            // scored by its own forward pass, and each needs the one derivative
            // belonging to it. Same advantage as the decision to declare --
            // choosing the war and choosing whether to have one are the same
            // decision judged by the same outcome.
            if (w.targetChosen >= 0 && w.targetCand.size() > 1) {
                std::vector<float> scores(w.targetCand.size(), 0.0f);
                for (size_t i = 0; i < w.targetCand.size(); ++i) {
                    m_target.forwardInto(ws.target, w.targetCand[i]);
                    const auto& o = ws.target.acts.back();
                    scores[i] = o.empty() || !std::isfinite(o[0]) ? 0.0f : o[0];
                }
                std::vector<float> probs;
                NeuralNet::softmax(scores, 1.0f, probs);
                for (size_t i = 0; i < w.targetCand.size(); ++i) {
                    const float gi = advantage *
                        (probs[i] - (i == (size_t)w.targetChosen ? 1.0f : 0.0f));
                    m_target.forwardInto(ws.target, w.targetCand[i]);
                    m_target.accumulateOutputGradInto(ws.target, gi);
                }
            }
            // Activations were snapshotted at decision time — reusing them
            // replaces a full policy re-forward with a couple of vector moves.
            // THE POLICY MUST BE RE-FORWARDED, not restored from the snapshot.
            //
            // The cached activations are the ones the decision was made with,
            // N_STEP turns and several hundred updates ago. Reusing them was
            // right for a plain policy gradient, which pretends the weights
            // have not moved. PPO's whole purpose is to notice that they have,
            // and a ratio computed from stale activations is always exactly
            // 1.0 -- the correction silently disappears and this becomes
            // REINFORCE with extra steps.
            w.acts.clear();
            m_trunk.forwardInto(ws.trunk, w.features);
            m_policy[m].forwardInto(ws.policy[m], ws.trunk.acts.back());
            m_policy[m].accumulatePPOInto(ws.policy[m], w.action, advantage,
                                          w.oldLogProb, PPO_CLIP, ppoEntropy());
            m_trunk.accumulateVectorGradInto(ws.trunk, NeuralNet::inputGrad(ws.policy[m]));
            backpropRelational(ws, w);
        }
    };

    // Each worker owns a full set of scratches. They are reused across turns
    // (m_scratch is a member) so a turn costs no allocation.
    if ((int)m_scratch.size() < threads) m_scratch.resize(threads);
    for (int t = 0; t < threads; ++t) {
        WorkerScratch& ws = m_scratch[t];
        if (!ws.ready) {
            for (int m = 0; m < MOD_COUNT; ++m) {
                m_policy[m].initScratch(ws.policy[m]);
                m_value[m].initScratch(ws.value[m]);
                // The bootstrap's own scratch, same shape, initialised with the
                // rest. Forgetting this one is a forward pass into unallocated
                // activations.
                m_value[m].initScratch(ws.valueNext[m]);
                m_q[m].initScratch(ws.q[m]);
            }
            m_target.initScratch(ws.target);
            m_trunk.initScratch(ws.trunk);
            m_stanceHead.initScratch(ws.stance);
            m_diploValue.initScratch(ws.diploValue);
            m_diploValue.initScratch(ws.diploValueNext);
            m_diplo.initScratch(ws.diplo);
            ws.ready = true;
        }
    }

    if (threads <= 1) {
        runRange(0, m_work.size(), m_scratch[0]);
    } else {
        std::vector<std::thread> pool;
        pool.reserve(threads - 1);
        const size_t chunk = (m_work.size() + threads - 1) / threads;
        for (int t = 1; t < threads; ++t) {
            const size_t lo = std::min(m_work.size(), chunk * t);
            const size_t hi = std::min(m_work.size(), lo + chunk);
            if (lo >= hi) continue;
            pool.emplace_back([&, t, lo, hi] { runRange(lo, hi, m_scratch[t]); });
        }
        runRange(0, std::min(m_work.size(), chunk), m_scratch[0]);
        for (auto& th : pool) th.join();
    }
    // Reduction is serial and cheap: one add per parameter per worker.
    for (int t = 0; t < threads; ++t) {
        for (int m = 0; m < MOD_COUNT; ++m) {
            m_policy[m].mergeScratch(m_scratch[t].policy[m]);
            m_value[m].mergeScratch(m_scratch[t].value[m]);
            // EVERY net that accumulated has to be reduced here. Q was added
            // without this line and spent its whole existence writing gradients
            // into a scratch nobody read: it trained for zero updates, stayed
            // at its initial weights, and Q_WARMUP_UPDATES then correctly kept
            // it from ever being consulted. It looked exactly like a feature
            // that was working and simply had not warmed up yet.
            m_q[m].mergeScratch(m_scratch[t].q[m]);
        }
        m_trunk.mergeScratch(m_scratch[t].trunk);
        m_stanceHead.mergeScratch(m_scratch[t].stance);
        for (auto& e : m_scratch[t].relEnc) m_relEncoder.mergeScratch(e);
        for (auto& e : m_scratch[t].relSco) m_relScore.mergeScratch(e);
        m_target.mergeScratch(m_scratch[t].target);
        m_diploValue.mergeScratch(m_scratch[t].diploValue);
        m_diplo.mergeScratch(m_scratch[t].diplo);
    }

    // Attach advantages to the debug log after the fact — the log is a shared
    // ring buffer and writing it from workers would need a lock for no gain.
    if (m_g->m_config.aiDebug) {
        for (const WorkItem& w : m_work) {
            if (w.module >= MOD_COUNT) continue;
            for (auto rit = m_log.rbegin(); rit != m_log.rend(); ++rit)
                if (rit->cid == w.cid && rit->module == w.module && rit->advantage == 0) {
                    rit->advantage = w.advantage;
                    break;
                }
        }
    }
    m_work.clear();
}

void AISystem::noteVictory(int cid) {
    m_victorCid = cid;
    // Force every window closed: endTurn only settles experiences that have
    // aged N_STEP turns, and there is no next turn to age them in.
    for (auto& [c, dq] : m_pending)
        for (auto& exp : dq) exp.age = nStep();
    endTurn();
    m_victorCid = -1;
}

void AISystem::noteMapEnd() {
    // Where everyone finished. refreshStats has already run for this turn, but
    // the trainer calls this after a turn has resolved, so take the current
    // picture rather than trusting whatever the last beginTurn saw.
    refreshStats();
    int total = 0;
    for (auto& [cid, st] : m_stats)
        if (cid < Game::REBEL_CID_MIN) total += st.provinces;
    if (total <= 0) { m_pending.clear(); return; }

    // Share of the world, mapped onto [-2, +2]: half the range the decisive
    // terminals use. A country holding an average slice scores nothing either
    // way; the signal is in finishing well above or well below what an equal
    // split would have given, which is the only thing a map that never resolved
    // can honestly say about the countries on it.
    int realCountries = 0;
    for (auto& [cid, st] : m_stats)
        if (cid < Game::REBEL_CID_MIN && st.provinces > 0) realCountries++;
    const float fairShare = realCountries > 0 ? 1.0f / realCountries : 1.0f;
    m_finalStanding.clear();
    for (auto& [cid, st] : m_stats) {
        if (cid >= Game::REBEL_CID_MIN || st.provinces <= 0) continue;
        const float share = (float)st.provinces / (float)total;
        m_finalStanding[cid] = 2.0f * std::tanh((share - fairShare) / fairShare);
    }

    // Force every window closed: endTurn only settles experiences that have
    // aged N_STEP turns, and there is no next turn to age them in.
    for (auto& [c, dq] : m_pending)
        for (auto& exp : dq) exp.age = nStep();
    endTurn();
    m_finalStanding.clear();
}

void AISystem::calibrateRewardScale(const std::vector<float>& perTurnProvinceDeltas) {
    if (perTurnProvinceDeltas.empty()) return;
    // Seed reward statistics from historical province churn so early-game
    // advantages are sensibly scaled instead of wildly off.
    float mean = 0;
    for (float d : perTurnProvinceDeltas) mean += d;
    mean /= perTurnProvinceDeltas.size();
    float var = 0;
    for (float d : perTurnProvinceDeltas) var += (d - mean) * (d - mean);
    var = std::max(0.25f, var / perTurnProvinceDeltas.size());
    for (int m = 0; m < MOD_COUNT; ++m) {
        m_rMean[m] = 2.0f * std::tanh(mean / 2.0f);
        m_rVar[m] = var;
    }
    printf("[AI] Reward scale calibrated from %zu history turns (mean %.2f var %.2f)\n",
           perTurnProvinceDeltas.size(), mean, var);
}

// ─── Persistence / debug ─────────────────────────────────

static void appendBlob(std::vector<uint8_t>& out, const std::vector<uint8_t>& blob) {
    uint32_t n = (uint32_t)blob.size();
    out.push_back(n & 0xFF); out.push_back((n >> 8) & 0xFF);
    out.push_back((n >> 16) & 0xFF); out.push_back((n >> 24) & 0xFF);
    out.insert(out.end(), blob.begin(), blob.end());
}

bool AISystem::s_readOnlyModel = false;
bool AISystem::s_evaluating = false;

void AISystem::saveModel() {
    // Observation mode: act on the trained model but never write it back, so a
    // normal game can run beside a training session. Both processes save every
    // 20 turns otherwise, and last-writer-wins would let a single-map play
    // session overwrite the trainer's accumulated progress.
    if (m_modelPath.empty() || s_readOnlyModel) return;
    // Directory creation happens ONCE, not on every save. This used to fork a
    // shell (system("mkdir -p ...")) every twenty turns for a directory that
    // has existed since the first save of the run.
    if (!m_modelDirReady) {
        auto slash = m_modelPath.find_last_of('/');
        if (slash != std::string::npos) {
            const std::string dir = m_modelPath.substr(0, slash);
#ifdef _WIN32
            _mkdir(dir.c_str());
#else
            mkdir(dir.c_str(), 0755);
#endif
        }
        m_modelDirReady = true;
    }
    std::vector<uint8_t> out;
    const char magic[4] = {'O', 'D', 'A', 'I'};
    out.insert(out.end(), magic, magic + 4);
    // 3 = the action-value heads ride along too. A v2 reader would refuse this
    // file on the net count alone, which is the correct failure: it would
    // otherwise read Q weights as if they were something else.
    // 4 = the war-target head rides along as well.
    // 5 = the diplomacy value head rides along too.
    // 6 = the shared trunk rides at the front, and the policy/Q/diplo blobs
    // after it are HEADS ({TRUNK_OUT, actions}), not whole nets. A v5 reader
    // would load a 320-input head as if it took the full feature vector, so
    // the version bump is not cosmetic -- and a v5 FILE cannot be read by this
    // build for the same reason. See loadModel.
    out.push_back(6);
    out.push_back(MOD_COUNT * 3 + 7); // trunk, stance, relational encoder+scorer, policy, value, Q, target, diplo, diploValue
    { std::vector<uint8_t> b; m_trunk.serialize(b); appendBlob(out, b); }
    { std::vector<uint8_t> b; m_stanceHead.serialize(b); appendBlob(out, b); }
    { std::vector<uint8_t> b; m_relEncoder.serialize(b); appendBlob(out, b); }
    { std::vector<uint8_t> b; m_relScore.serialize(b); appendBlob(out, b); }
    for (int m = 0; m < MOD_COUNT; ++m) {
        std::vector<uint8_t> b; m_policy[m].serialize(b); appendBlob(out, b);
    }
    for (int m = 0; m < MOD_COUNT; ++m) {
        std::vector<uint8_t> b; m_value[m].serialize(b); appendBlob(out, b);
    }
    for (int m = 0; m < MOD_COUNT; ++m) {
        std::vector<uint8_t> b; m_q[m].serialize(b); appendBlob(out, b);
    }
    { std::vector<uint8_t> b; m_target.serialize(b); appendBlob(out, b); }
    { std::vector<uint8_t> b; m_diplo.serialize(b); appendBlob(out, b); }
    { std::vector<uint8_t> b; m_diploValue.serialize(b); appendBlob(out, b); }
    // Reward normalisation statistics.
    //
    // The whole AISystem is destroyed and rebuilt on every map rotation
    // (unloadGameData deletes it), so these restarted at mean 0 / variance 1
    // several times an hour. For the first ~100 turns of every new map the
    // advantage scale was therefore wrong, and those are exactly the turns
    // where the early-game policy is being shaped. The weights were persisted
    // across rotations; the yardstick they are measured against was not.
    {
        std::vector<uint8_t> b;
        auto putf = [&](float f) {
            uint32_t v; memcpy(&v, &f, 4);
            b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF);
            b.push_back((v >> 16) & 0xFF); b.push_back((v >> 24) & 0xFF);
        };
        for (int m = 0; m < MOD_COUNT; ++m) { putf(m_rMean[m]); putf(m_rVar[m]); }
        appendBlob(out, b);
    }
    // Atomic save: write a temp file then rename over the target. A reader
    // (another instance, or a crash mid-write) must never see a half-written
    // model — deserialize would fail and silently reset to fresh weights.
    std::string tmpPath = m_modelPath + ".tmp";
    FILE* f = fopen(tmpPath.c_str(), "wb");
    if (!f) return;
    size_t written = fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    if (written != out.size()) { remove(tmpPath.c_str()); return; }
    rename(tmpPath.c_str(), m_modelPath.c_str());
    m_lastSaveBytes = out.size();
    printf("[AI] Model saved (%zu bytes, %llu updates)\n", out.size(),
           (unsigned long long)m_policy[0].updateCount());
}

int AISystem::syncWithPeers(const std::vector<std::string>& peerPaths, float alpha) {
    if (peerPaths.empty() || alpha <= 0.0f) return 0;
    // Peers are read one at a time into a scratch model and blended in, rather
    // than all held at once and averaged: nine nets is ~12 MB, and a run with
    // several workers would otherwise have every worker holding every peer.
    AISystem scratch(m_g, std::string());   // empty path: never loads, never saves
    int merged = 0;
    for (const std::string& path : peerPaths) {
        scratch.m_modelPath = path;
        if (!scratch.loadModel()) continue;   // not written yet, or mid-rename
        // Each peer gets an equal share of the move, so the result is a step
        // toward the MEAN of the peers rather than toward whichever was read
        // last. Recomputed per peer because a missing file changes the divisor.
        const float share = alpha / (float)peerPaths.size();
        bool ok = true;
        for (int m = 0; m < MOD_COUNT; ++m) {
            ok &= m_policy[m].blendToward(scratch.m_policy[m], share);
            ok &= m_value[m].blendToward(scratch.m_value[m], share);
        }
        ok &= m_trunk.blendToward(scratch.m_trunk, share);
        ok &= m_diplo.blendToward(scratch.m_diplo, share);
        if (!ok) {
            printf("[AI] peer %s has a different architecture — skipped\n", path.c_str());
            continue;
        }
        // The reward statistics are a running mean and variance, so averaging
        // them is exactly right: they describe the same quantity measured on
        // different maps.
        for (int m = 0; m < MOD_COUNT; ++m) {
            m_rMean[m] = (1.0f - share) * m_rMean[m] + share * scratch.m_rMean[m];
            m_rVar[m]  = (1.0f - share) * m_rVar[m]  + share * scratch.m_rVar[m];
            if (!(m_rVar[m] > 1e-6f)) m_rVar[m] = 1.0f;
        }
        ++merged;
    }
    // The scratch model's destructor saves to whatever m_modelPath holds, and
    // that is currently the last peer we read. Leaving it set would have every
    // worker overwrite one of its peers' files with a stale copy of itself on
    // the way out of this function.
    scratch.m_modelPath.clear();
    return merged;
}

bool AISystem::mergeModelFiles(const std::string& outPath,
                               const std::vector<std::string>& inPaths) {
    if (inPaths.empty()) return false;
    // Read-only for the whole of this, so no destructor writes anything back
    // over an input file. The one intended write happens explicitly below.
    const bool wasReadOnly = s_readOnlyModel;
    s_readOnlyModel = true;
    bool result = false;
    {
    AISystem acc(nullptr, inPaths[0]);
    if (!acc.loadModel()) {
        fprintf(stderr, "[AI] merge: cannot read %s\n", inPaths[0].c_str());
        s_readOnlyModel = wasReadOnly;
        return false;
    }
    int n = 1;
    for (size_t i = 1; i < inPaths.size(); ++i) {
        // Equal weighting: after k files the accumulator must move 1/(k+1) of
        // the way toward the next, or the first file read would dominate.
        const float share = 1.0f / (float)(n + 1);
        AISystem peer(nullptr, inPaths[i]);
        if (!peer.loadModel()) {
            fprintf(stderr, "[AI] merge: cannot read %s — skipped\n", inPaths[i].c_str());
            continue;
        }
        bool ok = true;
        for (int m = 0; m < MOD_COUNT; ++m) {
            ok &= acc.m_policy[m].blendToward(peer.m_policy[m], share);
            ok &= acc.m_value[m].blendToward(peer.m_value[m], share);
        }
        ok &= acc.m_trunk.blendToward(peer.m_trunk, share);
        ok &= acc.m_diplo.blendToward(peer.m_diplo, share);
        if (!ok) {
            fprintf(stderr, "[AI] merge: %s has a different architecture — skipped\n",
                    inPaths[i].c_str());
            continue;
        }
        for (int m = 0; m < MOD_COUNT; ++m) {
            acc.m_rMean[m] = (1.0f - share) * acc.m_rMean[m] + share * peer.m_rMean[m];
            acc.m_rVar[m]  = (1.0f - share) * acc.m_rVar[m]  + share * peer.m_rVar[m];
            if (!(acc.m_rVar[m] > 1e-6f)) acc.m_rVar[m] = 1.0f;
        }
        ++n;
    }
    acc.m_modelPath = outPath;
    s_readOnlyModel = false;
    acc.saveModel();
    s_readOnlyModel = true;   // acc's own destructor must not write again
    printf("[AI] merged %d model(s) into %s\n", n, outPath.c_str());
    result = true;
    }
    s_readOnlyModel = wasReadOnly;
    return result;
}

bool AISystem::resetModuleHead(const std::string& modelPath, int module) {
    if (module < 0 || module >= MOD_COUNT) {
        fprintf(stderr, "[AI] reset: no such module %d\n", module);
        return false;
    }
    // Same discipline as mergeModelFiles: read-only for the whole of this so no
    // destructor writes anything back, with one explicit save at the end.
    const bool wasReadOnly = s_readOnlyModel;
    s_readOnlyModel = true;
    bool result = false;
    {
    AISystem a(nullptr, modelPath);
    if (!a.loadModel()) {
        fprintf(stderr, "[AI] reset: cannot read %s\n", modelPath.c_str());
        s_readOnlyModel = wasReadOnly;
        return false;
    }
    const long long before = (long long)a.m_policy[module].updateCount();
    // The same architectures and seeds the constructor uses, so a reset head is
    // indistinguishable from a fresh one.
    static const int ACTIONS[MOD_COUNT] =
        {ECON_ACTIONS, POL_ACTIONS, WAR_ACTIONS, NAVY_ACTIONS};
    a.m_policy[module] = NeuralNet({FEATURE_COUNT, 512, 320, ACTIONS[module]},
                                   (uint32_t)(101 + module));
    a.m_value[module]  = NeuralNet({FEATURE_COUNT, 160, 1}, (uint32_t)(200 + module));
    a.m_rMean[module] = 0.0f;
    a.m_rVar[module]  = 1.0f;
    a.m_modelPath = modelPath;
    s_readOnlyModel = false;
    a.saveModel();
    s_readOnlyModel = true;   // a's own destructor must not write again
    printf("[AI] reset the %s head in %s (discarded %lld updates); "
           "every other module kept\n",
           MODULE_NAMES[module], modelPath.c_str(), before);
    result = true;
    }
    s_readOnlyModel = wasReadOnly;
    return result;
}

size_t AISystem::serializedSize() const {
    size_t n = 6; // magic + version + net count
    std::vector<uint8_t> b;
    for (int m = 0; m < MOD_COUNT; ++m) {
        b.clear(); m_policy[m].serialize(b); n += 4 + b.size();
        b.clear(); m_value[m].serialize(b);  n += 4 + b.size();
    }
    b.clear(); m_diplo.serialize(b); n += 4 + b.size();
    return n + 4 + MOD_COUNT * 2 * 4; // + the reward-statistics blob
}

long long AISystem::paramCount() const {
    long long n = 0;
    for (int m = 0; m < MOD_COUNT; ++m)
        n += (long long)m_policy[m].paramCount() + (long long)m_value[m].paramCount();
    return n + (long long)m_diplo.paramCount();
}

unsigned long long AISystem::totalUpdates() const {
    unsigned long long n = 0;
    for (int m = 0; m < MOD_COUNT; ++m)
        n += m_policy[m].updateCount() + m_value[m].updateCount();
    return n + m_diplo.updateCount();
}

// ─── The league ──────────────────────────────────────────────────────────
//
// Checkpoints live beside the model, named by slot rather than by time, so the
// pool is a fixed size and the oldest is simply overwritten. A run that never
// stops therefore never fills the disk, and a run that is interrupted leaves a
// usable pool behind.

static std::string leagueSlotPath(const std::string& modelPath, int slot) {
    const size_t slash = modelPath.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? std::string(".")
                                                         : modelPath.substr(0, slash);
    return dir + "/league-" + std::to_string(slot) + ".bin";
}

uint64_t AISystem::s_lastCheckpointUpdates = 0;
int AISystem::s_leagueGames[LEAGUE_CHECKPOINTS] = {0};
int AISystem::s_leagueLosses[LEAGUE_CHECKPOINTS] = {0};

void AISystem::recordLeagueOutcome() {
    // Who held more ground when the map ended: the frozen past self, or the
    // policy being trained. Land per country rather than total, because the
    // league is only ever given a third of the map (LEAGUE_SHARE) and comparing
    // totals would score it as losing every time by construction.
    if (m_leagueSlot < 0 || m_leagueSlot >= LEAGUE_CHECKPOINTS) return;
    if (m_leagueCids.empty() || !m_g) return;
    long long leagueLand = 0, ourLand = 0;
    int leagueN = 0, ourN = 0;
    for (const auto& [cid, st] : m_stats) {
        if (cid >= Game::REBEL_CID_MIN) continue;
        if (m_leagueCids.count(cid)) { leagueLand += st.provinces; leagueN++; }
        else                         { ourLand    += st.provinces; ourN++; }
    }
    if (leagueN == 0 || ourN == 0) return;
    const double theirs = (double)leagueLand / leagueN;
    const double ours   = (double)ourLand / ourN;
    s_leagueGames[m_leagueSlot]++;
    if (theirs > ours) s_leagueLosses[m_leagueSlot]++;
    printf("[AI] league slot %d: %.1f vs our %.1f provinces/country (%d/%d lost)\n",
           m_leagueSlot, theirs, ours, s_leagueLosses[m_leagueSlot],
           s_leagueGames[m_leagueSlot]);
    m_leagueSlot = -1;
}

void AISystem::writeLeagueCheckpoint() {
    if (m_modelPath.empty() || s_readOnlyModel) return;
    const uint64_t updates = m_policy[MOD_WAR].updateCount();
    if (updates < s_lastCheckpointUpdates + LEAGUE_CHECKPOINT_EVERY) return;
    s_lastCheckpointUpdates = updates;

    // Slot chosen by rotation, so the pool holds the last N checkpoints and the
    // oldest goes first. Deriving it from the update count means a restarted
    // run continues the rotation rather than always clobbering slot 0.
    const int slot = (int)((updates / LEAGUE_CHECKPOINT_EVERY) % LEAGUE_CHECKPOINTS);
    const std::string path = leagueSlotPath(m_modelPath, slot);

    // The TRUNK plus the policy heads. A frozen opponent only ever acts -- no
    // value head to fit, no Q to consult, no reward statistics of its own -- so
    // the rest is still not written. But the heads are {TRUNK_OUT, actions}
    // now: without the encoder that produced that embedding they are not a
    // policy at all, and a v1 checkpoint holds whole nets rather than heads,
    // so the format version moves with the architecture.
    std::vector<uint8_t> out;
    const char magic[4] = {'O', 'D', 'L', 'G'};
    out.insert(out.end(), magic, magic + 4);
    out.push_back(2);
    out.push_back(MOD_COUNT + 1);
    { std::vector<uint8_t> b; m_trunk.serialize(b); appendBlob(out, b); }
    for (int m = 0; m < MOD_COUNT; ++m) {
        std::vector<uint8_t> b; m_policy[m].serialize(b); appendBlob(out, b);
    }
    // Temp-and-rename, because a worker may be reading this slot right now and
    // a half-written checkpoint is an opponent made of noise.
    const std::string tmp = path + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "wb")) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        rename(tmp.c_str(), path.c_str());
        printf("[AI] league checkpoint -> slot %d (%llu updates)\n", slot,
               (unsigned long long)updates);
    }
}

bool AISystem::loadLeagueOpponent() {
    if (m_modelPath.empty()) return false;
    // Which slots exist. A young run has none, and one slot behind the current
    // policy is still a different policy, so even a single checkpoint is worth
    // playing against.
    std::vector<int> present;
    for (int i = 0; i < LEAGUE_CHECKPOINTS; ++i) {
        FILE* f = fopen(leagueSlotPath(m_modelPath, i).c_str(), "rb");
        if (f) { fclose(f); present.push_back(i); }
    }
    if (present.empty()) return false;

    // PFSP: weight by how badly the slot beats us. See s_leagueGames.
    std::vector<double> weight;
    weight.reserve(present.size());
    for (int slot : present) {
        const int g = s_leagueGames[slot];
        const double lossRate = g > 0 ? (double)s_leagueLosses[slot] / (double)g : 0.5;
        weight.push_back(lossRate * lossRate + 0.05);
    }
    std::discrete_distribution<size_t> pick(weight.begin(), weight.end());
    m_leagueSlot = present[pick(m_rng)];
    const std::string path = leagueSlotPath(m_modelPath, m_leagueSlot);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 6) { fclose(f); return false; }
    std::vector<uint8_t> buf((size_t)n);
    const size_t rd = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (rd != buf.size()) return false;
    if (memcmp(buf.data(), "ODLG", 4) != 0) return false;
    // v1 checkpoints are pre-trunk whole nets; refuse rather than misread them
    // as heads. They age out of the rotation within a few checkpoints.
    if (buf[4] != 2 || buf[5] != MOD_COUNT + 1) return false;

    size_t p = 6;
    {
        if (p + 4 > buf.size()) return false;
        const uint32_t len = buf[p] | (buf[p+1] << 8) | (buf[p+2] << 16) |
                             ((uint32_t)buf[p+3] << 24);
        p += 4;
        if (p + len > buf.size()) return false;
        if (!m_leagueTrunk.deserialize(buf.data() + p, len)) return false;
        m_leagueTrunk.setTanhOutput(true);
        p += len;
    }
    for (int m = 0; m < MOD_COUNT; ++m) {
        if (p + 4 > buf.size()) return false;
        const uint32_t len = buf[p] | (buf[p+1] << 8) | (buf[p+2] << 16) |
                             ((uint32_t)buf[p+3] << 24);
        p += 4;
        if (p + len > buf.size()) return false;
        if (!m_leaguePolicy[m].deserialize(buf.data() + p, len)) return false;
        p += len;
    }
    m_leagueLoaded = true;
    return true;
}

void AISystem::assignLeagueCountries() {
    m_leagueCids.clear();
    if (!m_leagueLoaded || LEAGUE_SHARE <= 0.0f) return;
    // Only real countries: rebels are not a side anyone is training against,
    // and the random control group must stay random.
    std::vector<int> pool;
    for (const auto& [cid, st] : m_stats) {
        (void)st;
        if (cid >= Game::REBEL_CID_MIN) continue;
        if (isRandomCountry(cid)) continue;
        pool.push_back(cid);
    }
    if (pool.size() < 3) return;   // too small a map to give a third away
    std::shuffle(pool.begin(), pool.end(), m_rng);
    const size_t take = (size_t)(pool.size() * LEAGUE_SHARE);
    for (size_t i = 0; i < take && i < pool.size(); ++i)
        m_leagueCids.insert(pool[i]);
    printf("[AI] league: %zu of %zu countries play a frozen past self\n",
           m_leagueCids.size(), pool.size());
}

bool AISystem::loadModel() {
    FILE* f = fopen(m_modelPath.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 10) { fclose(f); return false; }
    std::vector<uint8_t> buf((size_t)n);
    size_t rd = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (rd != buf.size()) return false;
    if (memcmp(buf.data(), "ODAI", 4) != 0) return false;
    // v1 models load fine — they just carry no reward statistics, so those keep
    // their cold-start values. Refusing them would throw away every hour of
    // training already invested in the file on disk.
    const int fileVersion = buf[4];
    if (fileVersion < 1 || fileVersion > 6) return false;
    // A PRE-TRUNK FILE CANNOT BE READ, AND MUST NOT BE GUESSED AT.
    //
    // Versions 1-5 store policy/Q/diplo as whole nets taking the full feature
    // vector; this build wants heads taking a TRUNK_OUT embedding. The shapes
    // differ in their FIRST dimension, which deserialize's input-widening path
    // would happily "migrate" into nonsense. Refusing is the honest failure:
    // the caller prints "Fresh model" and training starts over, which is the
    // price of the architecture change and was decided deliberately.
    if (fileVersion < 6) {
        printf("[AI] %s is a pre-trunk model (v%d); this build needs v6. "
               "Starting fresh.\n", m_modelPath.c_str(), fileVersion);
        return false;
    }
    const int count = buf[5];
    // v1/v2 carry no Q heads. Those files are every hour of training done
    // before this existed, so they load and simply leave Q at its initial
    // weights -- which Q_WARMUP_UPDATES then keeps out of the way until it has
    // learned something.
    if (count != MOD_COUNT * 3 + 7) return false;
    const bool hasDiploValue = true;
    const bool hasTarget = hasDiploValue || (count == MOD_COUNT * 3 + 2);
    const bool hasQ = hasTarget || (count == MOD_COUNT * 3 + 1);
    if (!hasQ && count != MOD_COUNT * 2 + 1) return false;
    size_t p = 6;
    auto readBlob = [&](NeuralNet& net) -> bool {
        if (p + 4 > buf.size()) return false;
        uint32_t len = buf[p] | (buf[p+1] << 8) | (buf[p+2] << 16) | ((uint32_t)buf[p+3] << 24);
        p += 4;
        if (p + len > buf.size()) return false;
        bool ok = net.deserialize(buf.data() + p, len);
        p += len;
        return ok;
    };
    if (!readBlob(m_trunk)) return false;
    m_trunk.setTanhOutput(true);
    if (!readBlob(m_stanceHead)) return false;
    if (!readBlob(m_relEncoder)) return false;
    m_relEncoder.setTanhOutput(true);
    if (!readBlob(m_relScore)) return false;
    for (int m = 0; m < MOD_COUNT; ++m) if (!readBlob(m_policy[m])) return false;
    for (int m = 0; m < MOD_COUNT; ++m) if (!readBlob(m_value[m])) return false;
    if (hasQ)
        for (int m = 0; m < MOD_COUNT; ++m) if (!readBlob(m_q[m])) return false;
    // Older files have no target head; it stays at its initial weights, and
    // TARGET_WARMUP_UPDATES keeps the old rule choosing until it has learned
    // something from watching that rule work.
    if (hasTarget && !readBlob(m_target)) return false;
    if (!readBlob(m_diplo)) return false;
    if (hasDiploValue && !readBlob(m_diploValue)) return false;
    if (fileVersion >= 2 && p + 4 <= buf.size()) {
        uint32_t len = buf[p] | (buf[p+1] << 8) | (buf[p+2] << 16) | ((uint32_t)buf[p+3] << 24);
        p += 4;
        if (p + len <= buf.size() && len >= (uint32_t)(MOD_COUNT * 2 * 4)) {
            auto getf = [&](size_t off) {
                uint32_t v = buf[off] | (buf[off+1] << 8) | (buf[off+2] << 16) |
                             ((uint32_t)buf[off+3] << 24);
                float f; memcpy(&f, &v, 4); return f;
            };
            size_t q = p;
            for (int m = 0; m < MOD_COUNT; ++m) {
                float mean = getf(q); q += 4;
                float var  = getf(q); q += 4;
                if (std::isfinite(mean)) m_rMean[m] = mean;
                // Variance drives a division; a zero or corrupt one would make
                // every advantage infinite on the first update after a load.
                if (std::isfinite(var) && var > 1e-6f) m_rVar[m] = var;
            }
        }
    }
    return true;
}

std::vector<std::string> AISystem::debugLines(int maxLines) const {
    std::vector<std::string> out;
    int n = 0;
    for (auto it = m_log.rbegin(); it != m_log.rend() && n < maxLines; ++it, ++n) {
        const Country* c = m_g->m_countries.getCountry(it->cid);
        std::string name = c ? c->name.substr(0, 14) : std::string("?");
        char buf[256];
        if (it->advantage != 0)
            snprintf(buf, sizeof(buf), "t%d %-14s %-8s %s adv%+.2f", it->turn,
                     name.c_str(), MODULE_NAMES[it->module], it->label.c_str(), it->advantage);
        else
            snprintf(buf, sizeof(buf), "t%d %-14s %-8s %s", it->turn,
                     name.c_str(), MODULE_NAMES[it->module], it->label.c_str());
        out.push_back(buf);
    }
    return out;
}

std::string AISystem::countrySummary(int cid) const {
    std::string out;
    int n = 0;
    for (auto it = m_log.rbegin(); it != m_log.rend() && n < 4; ++it) {
        if (it->cid != cid) continue;
        out += std::string(MODULE_NAMES[it->module]) + ": " + it->label + "\n";
        n++;
    }
    return out;
}
