#include "AISystem.h"
#include "../Game.h"
#include "../GameInternals.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    m_policy[MOD_ECONOMY]  = NeuralNet({FEATURE_COUNT, 512, 320, ECON_ACTIONS}, 101);
    m_policy[MOD_POLITICS] = NeuralNet({FEATURE_COUNT, 512, 320, POL_ACTIONS},  102);
    m_policy[MOD_WAR]      = NeuralNet({FEATURE_COUNT, 512, 320, WAR_ACTIONS},  103);
    m_policy[MOD_NAVY]     = NeuralNet({FEATURE_COUNT, 512, 320, NAVY_ACTIONS}, 104);
    for (int m = 0; m < MOD_COUNT; ++m)
        m_value[m] = NeuralNet({FEATURE_COUNT, 160, 1}, 200 + m);
    m_diplo = NeuralNet({FEATURE_COUNT, 256, 160, DIPLO_ACTIONS}, 300);
    if (loadModel())
        printf("[AI] Model loaded from %s\n", m_modelPath.c_str());
    else
        printf("[AI] Fresh model (no file at %s)\n", m_modelPath.c_str());
}

AISystem::~AISystem() { saveModel(); }

// ─── World cache ─────────────────────────────────────────

void AISystem::beginTurn() {
    m_turn++;
    m_decisionsThisTurn = 0;
    m_stats.clear();
    m_worldArmy = 0;
    m_worldPixels = 0;

    Game& g = *m_g;

    // Provinces / population / industry — one pass each over existing maps.
    for (auto& [pid, prov] : g.m_provinces.getAllProvinces()) {
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

    // Sampled unrest (bounded work: at most 6 provinces)
    float unrestSum = 0; int unrestN = 0;
    for (auto& [pid, prov] : g.m_provinces.getAllProvinces()) {
        if (prov.countryId != cid) continue;
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

    // 60-87: spare. 88-93: request context filled by decideDiplomacy only.
    f[95] = 1.0f; // bias
    // Degenerate late-game state (populations/treasuries overflowing float)
    // leaks inf/NaN into features; one NaN input turns every logit NaN and
    // poisons weight updates. Zero them at the source.
    for (float& v : f)
        if (!std::isfinite(v)) v = 0.0f;
}

// ─── Difficulty / sampling ───────────────────────────────

void AISystem::difficultyParams(float& temperature, float& epsilon) const {
    // Self-play training ALWAYS explores, whatever the difficulty setting says.
    // Training on Insane (argmax, no randomness) would freeze the policy on
    // its current best guess forever — exploration is what learning eats.
    if (m_g->m_aiTraining) { temperature = 1.0f; epsilon = 0.10f; return; }
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
                         int graveAction) {
    const std::vector<float>& logits = net.forward(feats);
    scoreOut = 0;
    if (logits.empty()) return 0;
    std::vector<float> masked(logits);
    int validCount = 0;
    for (size_t i = 0; i < masked.size(); ++i) {
        if (i < valid.size() && !valid[i]) masked[i] = -1e9f;
        else validCount++;
    }
    if (validCount == 0) return 0;

    float temperature, epsilon;
    difficultyParams(temperature, epsilon);

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
            if ((int)i == graveAction && m_g && !m_g->m_aiTraining) continue;
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

    Experience exp;
    buildFeatures(cid, exp.features);
    exp.provinces = st.provinces;
    exp.treasury = c->treasury;
    exp.army = st.army;
    exp.ships = st.boats + st.destroyers + st.carriers;
    exp.netIncome = g.computeCountryIncome(cid).net;
    exp.industrySum = st.industrySum;
    auto resIt2 = g.m_countryResearched.find(cid);
    exp.researched = resIt2 != g.m_countryResearched.end() ? (int)resIt2->second.size() : 0;

    std::vector<bool> valid;
    float score;

    validEconomy(cid, valid);
    int a = pickAction(m_policy[MOD_ECONOMY], exp.features, valid, score);
    m_policy[MOD_ECONOMY].snapshotActs(exp.acts[MOD_ECONOMY]);
    exp.action[MOD_ECONOMY] = a; exp.acted[MOD_ECONOMY] = true;
    logDecision(cid, MOD_ECONOMY, a, score, execEconomy(cid, a));

    validPolitics(cid, valid);
    a = pickAction(m_policy[MOD_POLITICS], exp.features, valid, score);
    m_policy[MOD_POLITICS].snapshotActs(exp.acts[MOD_POLITICS]);
    exp.action[MOD_POLITICS] = a; exp.acted[MOD_POLITICS] = true;
    logDecision(cid, MOD_POLITICS, a, score, execPolitics(cid, a));

    validWar(cid, valid);
    // 4 = declare war; see the graveAction note in pickAction.
    a = pickAction(m_policy[MOD_WAR], exp.features, valid, score, /*graveAction=*/4);
    m_policy[MOD_WAR].snapshotActs(exp.acts[MOD_WAR]);
    exp.action[MOD_WAR] = a; exp.acted[MOD_WAR] = true;
    logDecision(cid, MOD_WAR, a, score, execWar(cid, a));

    validNavy(cid, valid);
    a = pickAction(m_policy[MOD_NAVY], exp.features, valid, score);
    m_policy[MOD_NAVY].snapshotActs(exp.acts[MOD_NAVY]);
    exp.action[MOD_NAVY] = a; exp.acted[MOD_NAVY] = true;
    logDecision(cid, MOD_NAVY, a, score, execNavy(cid, a));

    auto& dq = m_pending[cid];
    dq.push_back(std::move(exp));
    while (dq.size() > (size_t)N_STEP + 4) dq.pop_front(); // safety cap
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
    v[9] = v[10] = v[11] = idle && !g.m_researchNodes.empty(); // pick a node
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
}

void AISystem::validWar(int cid, std::vector<bool>& v) {
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    const CountryStat& st = m_stats[cid];
    v.assign(WAR_ACTIONS, false);
    v[0] = true;
    if (!c) return;
    v[1] = c->treasury >= 1 && st.population > 10000; // recruit
    v[2] = st.army > 0 && !st.frontiers.empty();      // reinforce
    // attack / declare war / artillery need frontier context; cheap checks only
    bool anyWarFrontier = false, anyDeclarable = false;
    auto relIt = g.m_relations.find(c->isoA3);
    std::unordered_set<int> seen;
    for (auto& fr : st.frontiers) {
        if (!seen.insert(fr.enemyCid).second) continue;
        const Country* ec = g.m_countries.getCountry(fr.enemyCid);
        if (!ec) continue;
        bool war = false, friendly = false;
        if (relIt != g.m_relations.end()) {
            auto rr = relIt->second.find(ec->isoA3);
            if (rr != relIt->second.end()) {
                war = rr->second.war;
                // A NAP is breakable-for-war (declareWar clears it); only a
                // real alliance or a guarantee makes a neighbour off-limits.
                // Without this the AI pacts every border shut and can never
                // choose war again — the late-game freeze you saw.
                friendly = rr->second.alliance || rr->second.guarantee;
            }
        }
        if (war) anyWarFrontier = true;
        else if (!friendly) anyDeclarable = true;
    }
    // Overseas foes are declarable too, provided we can actually project power
    // (own a port + a real army). exec picks the land target first, then falls
    // back to the weakest beatable coastal enemy across the water.
    bool navalDeclarable = st.maxPort >= 1 && st.army > 1000 && st.navalTargets > 0;
    v[3] = anyWarFrontier && st.army > 0;
    v[4] = (anyDeclarable || navalDeclarable) && st.army > 0;
    v[5] = anyWarFrontier && c->treasury >= 5; // artillery (ammo checked at exec)
    // Offer ceasefire ONLY when not dominantly winning. A country that
    // outguns all its war enemies presses the attack instead of white-peacing,
    // so wars actually reach a conclusion instead of endless skirmish-then-
    // ceasefire (which left every country alive forever).
    long long warEnemyArmy = 0; bool anyWar = false;
    if (relIt != g.m_relations.end())
        for (auto& [iso, r] : relIt->second)
            if (r.war) {
                anyWar = true;
                int ocid = g.cidForIso(iso);
                if (ocid >= 0) warEnemyArmy += m_stats[ocid].army;
            }
    v[6] = anyWar && st.army < (long long)(warEnemyArmy * 1.2);
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
    v[3] = st.maxPort >= 1 && st.army > 1000 &&
           (st.navalTargets > 0 || st.navalWarTargets > 0);
    v[4] = st.boatsWithCrew > 0;
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
            for (auto& [pid, prov] : g.m_provinces.getAllProvinces()) {
                if (prov.countryId != cid) continue;
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
                for (auto& [pid, prov] : g.m_provinces.getAllProvinces()) {
                    if (prov.countryId != cid || g.m_provincePorts.count(pid)) continue;
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
            for (auto& [pid, prov] : g.m_provinces.getAllProvinces()) {
                if (prov.countryId != cid) continue;
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
            if (pick < 0) return TextFormat("research: nothing left (%s)", want);
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
        case 1: { // enact the policy closest to this country's compass
            const Country* c = g.m_countries.getCountry(cid);
            if (!c) return "policy: no country";
            const Policy* best = nullptr; float bestDist = 1e9f;
            for (auto& p : g.m_allPolicies) {
                if (!g.canCountryEnactPolicy(cid, p)) continue;
                float dx = p.econShift, dy = p.socShift;
                // Prefer policies pushing toward the country's own leaning
                float d = std::fabs(c->compassEconomic / 25.0f - dx) +
                          std::fabs(c->compassSocial / 25.0f - dy);
                if (d < bestDist) { bestDist = d; best = &p; }
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
                if (ea > targetArmy) { targetArmy = ea; target = fr.enemyCid; }
            }
            if (target < 0) return TextFormat("%s: no suitable target", req);
            const Country* ec = g.m_countries.getCountry(target);
            g.m_pendingDiplomaticActions.push_back({c->isoA3, ec->isoA3, req, 1});
            diploCoolDown(cid, target);
            m_trainStats.pactsProposed++;
            return TextFormat("%s -> %s", req, ec->name.c_str());
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
                for (auto& [p2, prov] : g.m_provinces.getAllProvinces())
                    if (prov.countryId == cid) {
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
        case 2: { // reinforce the most threatened frontier province
            // Was: whichever frontier had the smallest garrison, with no regard
            // for what stood opposite it. That topped up quiet provinces facing
            // allies while a province facing a live enemy stack stayed thin —
            // which is why the AI never appeared to defend against an invasion.
            int weakPid = -1; float worst = -1.0e30f;
            for (auto& fr : st.frontiers) {
                float s = threatScore(fr.pid);
                if (s > worst) { worst = s; weakPid = fr.pid; }
            }
            if (weakPid < 0) return "reinforce: no frontier";
            auto nIt = g.m_provinceNeighbors.find(weakPid);
            if (nIt == g.m_provinceNeighbors.end()) return "reinforce: no neighbors";
            int srcPid = -1, srcG = 0;
            for (int nid : nIt->second) {
                if (nid >= (int)g.m_provinceCountryLookup.size() ||
                    g.m_provinceCountryLookup[nid] != cid) continue;
                int gsz = garrisonOf(nid, cid);
                if (gsz > srcG) { srcG = gsz; srcPid = nid; }
            }
            if (srcPid < 0 || srcG < 200) return "reinforce: nothing to move";
            for (auto& mo : g.m_pendingMoveOrders)
                if (mo.fromProvince == srcPid && mo.countryId == cid) return "reinforce: already moving";
            g.m_pendingMoveOrders.push_back({srcPid, weakPid, 50, cid});
            return TextFormat("reinforce prov %d from %d", weakPid, srcPid);
        }
        case 3: { // attack the weakest adjacent at-war enemy province we can beat
            float atkMod = 1.0f; // getTotalEffect is global-player; stay conservative
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
                    float def = defG * (1.0f + fort * 0.1f);
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
            if (bestFrom < 0) return "attack: no winnable target";
            for (auto& mo : g.m_pendingMoveOrders)
                if (mo.fromProvince == bestFrom && mo.countryId == cid) return "attack: order pending";
            g.m_pendingMoveOrders.push_back({bestFrom, bestTo, 75, cid});
            return TextFormat("attack prov %d from %d (margin %.1fx)", bestTo, bestFrom, bestMargin);
        }
        case 4: { // declare war: prefer neighbours holding OUR claimed land,
                  // then the weakest beatable one. Claims are the war goal.
            // Which neighbours hold provinces we claim?
            std::unordered_set<int> claimTargets;
            auto myClaims = g.m_claims.find(c.isoA3);
            if (myClaims != g.m_claims.end())
                for (int pid : myClaims->second) {
                    int owner = pid < (int)g.m_provinceCountryLookup.size()
                                    ? g.m_provinceCountryLookup[pid] : 0;
                    if (owner > 0 && owner != cid && owner < Game::SPC_CID)
                        claimTargets.insert(owner);
                }
            int target = -1; long long targetArmy = -1;
            bool targetClaimed = false;
            std::unordered_set<int> seen;
            for (auto& fr : st.frontiers) {
                if (!seen.insert(fr.enemyCid).second) continue;
                const Country* ec = g.m_countries.getCountry(fr.enemyCid);
                if (!ec) continue;
                bool friendly = false, war = false;
                if (relIt != g.m_relations.end()) {
                    auto rr = relIt->second.find(ec->isoA3);
                    if (rr != relIt->second.end()) {
                        war = rr->second.war;
                        // NAP is breakable (declareWar clears it); allies and
                        // guaranteed states stay off-limits.
                        friendly = rr->second.alliance || rr->second.guarantee;
                    }
                }
                if (war || friendly) continue;
                long long ea = m_stats[fr.enemyCid].army;
                bool claimed = claimTargets.count(fr.enemyCid) > 0;
                // Relaxed superiority gate — a hard 1.3x bar meant matched
                // late-game borders were never attackable, so the model could
                // never even learn aggression at parity. Reconquering CLAIMED
                // land is worth more risk (it removes unrest + satisfies the
                // claim), so the bar drops further there.
                double bar = claimed ? 0.85 : 1.05;
                if (st.army < (long long)(ea * bar) + 200) continue;
                // A claimed neighbour beats any unclaimed one; within the same
                // class, weakest wins.
                if (target < 0 || (claimed && !targetClaimed) ||
                    (claimed == targetClaimed && ea < targetArmy)) {
                    target = fr.enemyCid; targetArmy = ea; targetClaimed = claimed;
                }
            }
            // Naval fallback: no reachable land target, but we have a port and
            // an army — declare war on the weakest beatable OVERSEAS coastal
            // enemy (one that owns a port to land at) so the navy module can
            // embark, sail, and invade it. This is the unlock that lets the AI
            // cross water for territory instead of only fighting land borders.
            if (target < 0 && st.maxPort >= 1 && st.army > 1000) {
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
                    bool friendly = false, war = false;
                    if (relIt != g.m_relations.end()) {
                        auto rr = relIt->second.find(ec2->isoA3);
                        if (rr != relIt->second.end()) {
                            war = rr->second.war;
                            friendly = rr->second.alliance || rr->second.guarantee;
                        }
                    }
                    if (war || friendly) continue;
                    long long ea = m_stats[oc].army;
                    bool claimed = claimTargets.count(oc) > 0;
                    // Amphibious assaults are costlier than a land push (troops
                    // ferry in piecemeal), so demand a clearer edge unless the
                    // land is claimed — reconquest is worth the risk.
                    double bar = claimed ? 1.0 : 1.3;
                    if (st.army < (long long)(ea * bar) + 500) continue;
                    if (navalTarget < 0 || (claimed && !navalClaimed) ||
                        (claimed == navalClaimed && ea < bestArmy)) {
                        navalTarget = oc; bestArmy = ea; navalClaimed = claimed;
                    }
                }
                if (navalTarget >= 0) {
                    const Country* ec2 = g.m_countries.getCountry(navalTarget);
                    g.m_pendingDiplomaticActions.push_back({c.isoA3, ec2->isoA3, "declare_war", 1});
                    m_trainStats.warsDeclared++;
                    return std::string("declare NAVAL war on ") + ec2->name +
                           (navalClaimed ? " (claims)" : " (overseas)");
                }
            }
            if (target < 0) return "war: no suitable target";
            const Country* ec = g.m_countries.getCountry(target);
            g.m_pendingDiplomaticActions.push_back({c.isoA3, ec->isoA3, "declare_war", 1});
            m_trainStats.warsDeclared++;
            return std::string("declare war on ") + ec->name + (targetClaimed ? " (claims)" : "");
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
                for (auto& [pid, prov] : g.m_provinces.getAllProvinces()) {
                    if ((int)out.size() >= maxN) break;
                    if (prov.countryId != owner) continue;
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
            m_trainStats.ceasefiresOffered++;
            return TextFormat("offer ceasefire (%s) to %s", posture, ec->name.c_str());
        }
        default: return "hold";
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
                if (!findEnemyPort(s.lon, s.lat, tp, tLon, tLat)) break;
                double dLon = tLon - s.lon, dLat = tLat - s.lat;
                double dist = std::sqrt(dLon * dLon + dLat * dLat);
                const double STEP = 18.0; // degrees per turn — no cross-map teleports
                if (dist > STEP) { dLon *= STEP / dist; dLat *= STEP / dist; }
                g.m_pendingShipMoveOrders.push_back({(int)i, s.lon + dLon, s.lat + dLat});
                if (++moved >= 3) break; // a few ships per turn is plenty
            }
            if (moved) return std::string(TextFormat("move %d ship(s) toward enemy port", moved));
            // "no target" conflated three very different situations, which made
            // the archipelago stall impossible to read off the dashboard.
            {
                int tp; double tl, ta;
                if (!findEnemyPort(0, 0, tp, tl, ta))
                    return "navy move: no at-war enemy owns a port";
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
            m_trainStats.embarks++;
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
                    m_trainStats.landings++;
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
                    m_trainStats.unloadsHome++;
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
                               const std::string& sourceIso) {
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
    int a = pickAction(m_diplo, feats, valid, score);
    // Record in the country's experience so the diplo net learns too
    auto it = m_pending.find(targetCid);
    if (it != m_pending.end() && !it->second.empty()) {
        it->second.back().action[MOD_COUNT] = a;
        it->second.back().acted[MOD_COUNT] = true;
    }
    logDecision(targetCid, MOD_POLITICS, a, score,
                std::string(a ? "ACCEPT " : "REJECT ") + action + " from " + sourceIso);
    return a == 1;
}

// ─── Learning ────────────────────────────────────────────

void AISystem::endTurn() {
    Game& g = *m_g;
    if (!g.m_config.aiLearning) { m_pending.clear(); return; }


    // Refresh post-turn stats for reward deltas. beginTurn() rebuilds m_stats
    // from post-turn state with the same cheap single passes; undo its turn
    // counter bump (and keep the decision counter — the dashboard reads it
    // after this) since this isn't a new turn.
    int savedDecisions = m_decisionsThisTurn;
    beginTurn();
    m_turn--;
    m_decisionsThisTurn = savedDecisions;

    float rewardSum[MOD_COUNT] = {0, 0, 0, 0};
    int rewardN = 0;

    // Shared update path for one finished (or terminal) experience
    auto applyUpdate = [&](int cid, Experience& exp, const float* rewards) {
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

            // Value baseline: V(s) trained toward the normalised reward
            m_value[m].forward(exp.features);
            float baseline = m_value[m].lastOutput()[0];
            float advantage = std::clamp(norm - baseline, -3.0f, 3.0f);
            m_value[m].valueUpdate(norm, LR_VALUE);

            // Activations were snapshotted at decision time — restoring them
            // replaces a full policy re-forward with a couple of vector moves.
            if (!exp.acts[m].empty()) m_policy[m].restoreActs(std::move(exp.acts[m]));
            else m_policy[m].forward(exp.features);
            m_policy[m].policyGradientUpdate(exp.action[m], advantage, LR_POLICY);

            // Attach the advantage to the matching log entry (if still in the ring)
            for (auto rit = m_log.rbegin(); rit != m_log.rend(); ++rit)
                if (rit->cid == cid && rit->module == m && rit->advantage == 0) {
                    rit->advantage = advantage;
                    break;
                }
        }
        // Diplomacy net learns with the politics reward
        if (exp.acted[MOD_COUNT] && exp.action[MOD_COUNT] >= 0) {
            float dev = rewards[MOD_POLITICS] - m_rMean[MOD_POLITICS];
            float norm = dev / std::sqrt(m_rVar[MOD_POLITICS] + 1e-4f);
            m_diplo.forward(exp.features);
            m_diplo.policyGradientUpdate(exp.action[MOD_COUNT],
                                         std::clamp(norm, -3.0f, 3.0f), LR_POLICY);
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
        for (auto& exp : dq) { exp.age++; exp.rebellions += rebNow; }

        // Research completions counter (per turn, not per window). First sight
        // of a country just records its baseline — otherwise every node the
        // map GRANTED at start would count as "completed in training".
        auto resIt = g.m_countryResearched.find(cid);
        int researchedNow = resIt != g.m_countryResearched.end() ? (int)resIt->second.size() : 0;
        auto lrIt = m_lastResearchCount.find(cid);
        if (lrIt == m_lastResearchCount.end()) {
            m_lastResearchCount[cid] = researchedNow;
        } else {
            if (researchedNow > lrIt->second)
                m_trainStats.researchCompleted += researchedNow - lrIt->second;
            lrIt->second = researchedNow;
        }

        float dNetNow = dead ? 0.0f : g.computeCountryIncome(cid).net;

        while (!dq.empty() && (dead || dq.front().age >= N_STEP)) {
            Experience& exp = dq.front();
            float rewards[MOD_COUNT];
            if (dead) {
                // Terminal: being eliminated is the worst possible outcome —
                // every decision in the final window shares the blame.
                for (int m = 0; m < MOD_COUNT; ++m) rewards[m] = -4.0f;
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
                float global = 2.0f * std::tanh(dProv / 3.0f)
                             + 0.4f * std::tanh(dTre / 100.0f)
                             + 0.8f * std::tanh(dNet / 15.0f)
                             - 2.5f * std::tanh(rebels / 2.0f);
                rewards[MOD_ECONOMY]  = global + 0.6f * std::tanh(dInd / 3.0f)
                                      + 0.6f * std::tanh(dResearch / 2.0f);
                rewards[MOD_POLITICS] = global - 3.0f * std::tanh(rebels / 2.0f)
                                      + (exp.netIncome > 0 ? 0.2f : -0.2f);
                rewards[MOD_WAR]      = global + 2.0f * std::tanh(dProv / 3.0f)
                                      + 0.3f * std::tanh(dArmy / 40000.0f);
                rewards[MOD_NAVY]     = global + 0.5f * std::tanh(dShips / 2.0f);
            }
            // tanh(NaN) is still NaN: overflowed treasuries/incomes must not
            // poison the weight update (a single NaN reward corrupts the net
            // permanently, including the model file saved to disk).
            for (int m = 0; m < MOD_COUNT; ++m)
                if (!std::isfinite(rewards[m])) rewards[m] = 0.0f;
            applyUpdate(cid, exp, rewards);
            dq.pop_front();
        }

        if (dead) {
            m_lastResearchCount.erase(cid);
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }

    // Reward trend feed for the trainer dashboard
    if (rewardN > 0) {
        for (int m = 0; m < MOD_COUNT; ++m) {
            m_rewardHistory[m].push_back(rewardSum[m] / rewardN);
            while (m_rewardHistory[m].size() > 600) m_rewardHistory[m].pop_front();
        }
    }

    if (m_turn % 20 == 0) saveModel();
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

void AISystem::saveModel() {
    // Observation mode: act on the trained model but never write it back, so a
    // normal game can run beside a training session. Both processes save every
    // 20 turns otherwise, and last-writer-wins would let a single-map play
    // session overwrite the trainer's accumulated progress.
    if (m_modelPath.empty() || s_readOnlyModel) return;
    auto slash = m_modelPath.find_last_of('/');
    if (slash != std::string::npos)
        system(("mkdir -p \"" + m_modelPath.substr(0, slash) + "\"").c_str());
    std::vector<uint8_t> out;
    const char magic[4] = {'O', 'D', 'A', 'I'};
    out.insert(out.end(), magic, magic + 4);
    out.push_back(1); // format version
    out.push_back(MOD_COUNT * 2 + 1); // net count
    for (int m = 0; m < MOD_COUNT; ++m) {
        std::vector<uint8_t> b; m_policy[m].serialize(b); appendBlob(out, b);
    }
    for (int m = 0; m < MOD_COUNT; ++m) {
        std::vector<uint8_t> b; m_value[m].serialize(b); appendBlob(out, b);
    }
    { std::vector<uint8_t> b; m_diplo.serialize(b); appendBlob(out, b); }
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

bool AISystem::loadModel() {
    FILE* f = fopen(m_modelPath.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 10) { fclose(f); return false; }
    std::vector<uint8_t> buf((size_t)n);
    size_t rd = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (rd != buf.size()) return false;
    if (memcmp(buf.data(), "ODAI", 4) != 0 || buf[4] != 1) return false;
    int count = buf[5];
    if (count != MOD_COUNT * 2 + 1) return false;
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
    for (int m = 0; m < MOD_COUNT; ++m) if (!readBlob(m_policy[m])) return false;
    for (int m = 0; m < MOD_COUNT; ++m) if (!readBlob(m_value[m])) return false;
    return readBlob(m_diplo);
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
