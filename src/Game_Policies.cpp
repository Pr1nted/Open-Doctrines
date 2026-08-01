#include "Game.h"
#include "Audio.h"
#include "GameInternals.h"
#include "Keybinds.h"
#include "raymath.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <random>

// ═══════════════════════════════════════════════════════════════════
// ─── Policy System ─────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════

void Game::initPolicies() {
    m_allPolicies.clear();
    
    // THE CATALOGUE IS A RULE SET, NOT MAP CONTENT.
    //
    // It used to be read from the .odmap archive and nowhere else. Every
    // shipped map carries a byte-identical copy of it — same 15,943 bytes,
    // same hash, six times over — which is the shape of a global rule that got
    // filed as per-map data.
    //
    // What that cost was invisible and large: procedurally generated maps carry
    // no policies.json, and generated maps are the ONLY maps the AI trains on.
    // So m_allPolicies was empty for every turn of every self-play run,
    // validPolitics masked out enact/cancel/calming permanently, policy costs
    // were always zero, and the model reached the shipped scenarios — where 15
    // of the 17 are enactable — having never once seen a doctrine exist. The
    // game is named after them.
    //
    // The archive still wins when it has one, because a map that ships its own
    // catalogue is a map deliberately changing the rules, and that is a
    // capability worth keeping. The data directory is the fallback, so any map
    // without one gets the standard set instead of nothing at all.
    std::string json;
    auto it = m_odmJsonData.find("policies.json");
    if (it != m_odmJsonData.end()) {
        json = it->second;
    } else {
        const std::string path = m_dataDir + "policies.json";
        std::ifstream f(path);
        if (!f) f.open("data/policies.json");
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            json = ss.str();
        }
        if (json.empty()) {
            if (m_origCerr) {
                std::ostream origErr(m_origCerr);
                origErr << "policies.json found in neither the .odmap archive nor "
                        << path << " — no doctrines will be available" << std::endl;
            }
            return;
        }
    }

    try {
        auto j = nlohmann::json::parse(json);
        for (auto& p : j["policies"]) {
            Policy policy;
            policy.id = p.value("id", "");
            policy.name = p.value("name", "");
            policy.category = p.value("category", "");
            policy.folder = p.value("folder", "");
            policy.description = p.value("description", "");
            policy.costPerTurn = p.value("cost_per_turn", 0);
            policy.implementationTurns = p.value("implementation_turns", 3);
            policy.propagandaDuration = p.value("propaganda_duration", 0);
            policy.econShift = p.value("compass_shift", nlohmann::json::object()).value("economic", 0.0f);
            policy.socShift = p.value("compass_shift", nlohmann::json::object()).value("social", 0.0f);
            policy.minEcon = p.value("requirements", nlohmann::json::object()).value("min_economic", -100);
            policy.maxEcon = p.value("requirements", nlohmann::json::object()).value("max_economic", 100);
            policy.minSoc = p.value("requirements", nlohmann::json::object()).value("min_social", -100);
            policy.maxSoc = p.value("requirements", nlohmann::json::object()).value("max_social", 100);
            
            auto effects = p.value("effects", nlohmann::json::object());
            policy.effect.minorityGrowthRate = effects.value("minority_growth_rate", 0.0f);
            policy.effect.immigrationBoost = effects.value("immigration_boost", 0.0f);
            policy.effect.pacificationCost = effects.value("pacification_cost", 0.0f);
            policy.effect.unrestReduction = effects.value("unrest_reduction", 0.0f);
            policy.effect.publicOpinionShift = effects.value("public_opinion_shift", 0.0f);
            policy.effect.targetMinority = effects.value("target_minority", "");
            
            if (p.contains("incompatible_with")) {
                for (auto& inc : p["incompatible_with"]) {
                    policy.incompatibleWith.push_back(inc.get<std::string>());
                }
            }
            
            if (p.contains("tradeoffs")) {
                auto tradeoffs = p["tradeoffs"];
                if (tradeoffs.contains("gains")) {
                    for (auto& g : tradeoffs["gains"]) {
                        policy.tradeoffs.gains.push_back(g.get<std::string>());
                    }
                }
                if (tradeoffs.contains("costs")) {
                    for (auto& c : tradeoffs["costs"]) {
                        policy.tradeoffs.costs.push_back(c.get<std::string>());
                    }
                }
            }
            
            m_allPolicies.push_back(policy);
        }
        std::cout << "  Loaded " << m_allPolicies.size() << " policies from JSON" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse policies.json: " << e.what() << std::endl;
    }
}

void Game::initEthnicPolicyCategories() {
    m_ethnicPolicyCategories.clear();

    auto addCat = [&](const std::string& id, const std::string& name, std::vector<EthnicPolicyOption> opts) {
        EthnicPolicyCategory cat;
        cat.id = id;
        cat.displayName = name;
        cat.options = std::move(opts);
        m_ethnicPolicyCategories.push_back(cat);
    };

    addCat("deportation", "Deportation Policy", {
        {"Harsh",   "Force relocation. -2.5% align/turn, -2% pop/turn, shifts right",
            -2.5f, -2.0f, 0.0f, 1.0f, 0.0f, false},
        {"Medium",  "Status quo. No alignment or population changes.",
            0.0f, 0.5f, 0.0f, 0.0f, 0.0f, true},
        {"Light",   "Encourage immigration. +1.5% align/turn, +1.5% pop/turn",
            1.5f, 1.5f, 0.0f, 0.0f, 0.0f, false},
    });
    addCat("economic", "Economic Incentives", {
        {"Big Incentives",  "3 cost/turn. +5% align/turn, provinces shift toward gov compass",
            5.0f, 0.0f, 3.0f, 0.3f, 0.0f, false},
        {"Some Incentives", "1 cost/turn. +2.5% align/turn, slight compass shift",
            2.5f, 0.0f, 1.0f, 0.1f, 0.0f, false},
        {"No Incentives",   "Free. No alignment or compass changes.",
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true},
    });
    addCat("cultural", "Cultural Autonomy", {
        {"Full Autonomy",    "Free. +3% align/turn, +1% pop/turn",
            3.0f, 1.0f, 0.0f, 0.0f, 0.0f, false},
        {"Partial Autonomy", "Free. +1% align/turn, no pop change",
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, true},
        {"Suppression",      "Free. -3% align/turn, -1% pop/turn, shifts auth",
            -3.0f, -1.0f, 0.0f, 0.0f, 0.5f, false},
    });
    addCat("political", "Political Representation", {
        {"Reserved Seats",   "2 cost/turn. +4% align/turn, minority representation",
            4.0f, 0.0f, 2.0f, 0.0f, 0.0f, false},
        {"Standard Rights",  "Free. +1% align/turn, equal legal rights",
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, true},
        {"Disenfranchised",  "Free. -4% align/turn, shifts auth. No political voice",
            -4.0f, 0.0f, 0.0f, 0.0f, 1.0f, false},
    });
    addCat("language", "Language Policy", {
        {"Official Recognition", "0.5 cost/turn. +2% align/turn, minority language official",
            2.0f, 0.0f, 0.5f, 0.0f, 0.0f, false},
        {"Tolerance",            "Free. +1% align/turn, minority language tolerated",
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, true},
        {"Ban",                  "Free. -5% align/turn, -2% pop/turn, shifts right",
            -5.0f, -2.0f, 0.0f, 0.5f, 0.0f, false},
    });
    addCat("integration", "Integration Programs", {
        {"Active Programs", "2 cost/turn. +3% align/turn, active cultural exchange",
            3.0f, 0.0f, 2.0f, 0.0f, 0.0f, false},
        {"Passive Programs","1 cost/turn. +1% align/turn, basic integration",
            1.0f, 0.0f, 1.0f, 0.0f, 0.0f, false},
        {"None",            "Free. No alignment or population effects.",
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true},
    });
}

std::vector<int> Game::defaultEthnicPolicyOptions() const {
    std::vector<int> out(m_ethnicPolicyCategories.size(), 0);
    for (size_t ci = 0; ci < m_ethnicPolicyCategories.size(); ++ci)
        for (size_t oi = 0; oi < m_ethnicPolicyCategories[ci].options.size(); ++oi)
            if (m_ethnicPolicyCategories[ci].options[oi].isDefault) { out[ci] = (int)oi; break; }
    return out;
}

int Game::ethnicPolicyOption(int countryId, const std::string& minorityName, size_t ci) const {
    if (ci >= m_ethnicPolicyCategories.size()) return -1;
    auto cIt = m_ethnicPolicies.find(countryId);
    if (cIt != m_ethnicPolicies.end()) {
        auto mIt = cIt->second.find(minorityName);
        if (mIt != cIt->second.end() && ci < mIt->second.size()) {
            const int oi = mIt->second[ci];
            if (oi >= 0 && oi < (int)m_ethnicPolicyCategories[ci].options.size()) return oi;
        }
    }
    for (size_t oi = 0; oi < m_ethnicPolicyCategories[ci].options.size(); ++oi)
        if (m_ethnicPolicyCategories[ci].options[oi].isDefault) return (int)oi;
    return 0;
}

void Game::setEthnicPolicyOption(int countryId, const std::string& minorityName,
                                 size_t ci, int option) {
    if (ci >= m_ethnicPolicyCategories.size()) return;
    if (option < 0 || option >= (int)m_ethnicPolicyCategories[ci].options.size()) return;
    auto& row = m_ethnicPolicies[countryId][minorityName];
    // A partially filled row is a trap: every reader indexes by category, so a
    // row shorter than the category list silently answers "default" for the
    // tail. Fill it out before writing into it.
    if (row.size() != m_ethnicPolicyCategories.size()) {
        std::vector<int> def = defaultEthnicPolicyOptions();
        for (size_t k = 0; k < row.size() && k < def.size(); ++k) def[k] = row[k];
        row = std::move(def);
    }
    row[ci] = option;
}

void Game::initCountryCompass() {
    // Fill in countries not already loaded from country_compass.json FROM THEIR
    // OWN COMPASS, not {0,0}. Political unrest is measured as a province's
    // distance from its GOVERNMENT's stance; defaulting the government to the
    // political centre made every ideologically-extreme province read as
    // dissent even when it perfectly matched an extreme government — the true
    // cause of universal early fragmentation on generated maps (which set
    // Country compass but ship no country_compass.json).
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID || cid == SPC_CID) continue;
        if (m_countryCompass.find(cid) == m_countryCompass.end()) {
            m_countryCompass[cid] = {c.compassEconomic, c.compassSocial};
        }
    }
}

void Game::applyStartingPolicies() {
    for (auto& [cid, c] : m_countries.getAll()) {
        auto it = m_startingPolicies.find(c.isoA3);
        if (it == m_startingPolicies.end()) continue;

        for (const std::string& pid : it->second) {
            // Find the policy
            auto pit = std::find_if(m_allPolicies.begin(), m_allPolicies.end(),
                [&](const Policy& p) { return p.id == pid; });
            if (pit == m_allPolicies.end()) continue;

            // Check incompatibility with already-active policies
            bool incompatible = false;
            for (int apIdx : m_countryActivePolicyIndices[cid]) {
                if (apIdx >= (int)m_activePolicies.size()) continue;
                const auto& ap = m_activePolicies[apIdx];
                if (ap.turnsRemaining != 0) continue;
                for (const auto& incId : pit->incompatibleWith) {
                    if (ap.policyId == incId) { incompatible = true; break; }
                }
                if (incompatible) break;
            }
            if (incompatible) continue;

            // Apply as already-active policy (turnsRemaining = 0)
            ActivePolicy ap;
            ap.policyId = pid;
            ap.countryId = cid;
            ap.turnsRemaining = 0;  // Already active
            ap.targetProvince = -1;
            ap.targetMinority = "";

            int idx = (int)m_activePolicies.size();
            m_activePolicies.push_back(ap);
            m_countryActivePolicyIndices[cid].push_back(idx);

            // Apply compass shift
            shiftCountryCompass(cid, pit->econShift, pit->socShift);
        }

        // Also apply starting minority policies (ethnic defaults). These are
        // already per-country in the map data (isoA3 -> minority -> options);
        // they used to be flattened into one world-wide table, so whichever
        // country the loader reached first set the policy for everybody and the
        // rest were dropped on the floor.
        auto smpIt = m_startingMinorityPolicies.find(c.isoA3);
        if (smpIt != m_startingMinorityPolicies.end())
            for (auto& [mname, opts] : smpIt->second)
                m_ethnicPolicies[cid][mname] = opts;
    }
}
 
void Game::enactPolicy(int countryId, const std::string& policyId, int targetProvince, const std::string& targetMinority) {
    auto it = std::find_if(m_allPolicies.begin(), m_allPolicies.end(),
        [&](const Policy& p) { return p.id == policyId; });
    if (it == m_allPolicies.end()) return;
    if (!canCountryEnactPolicy(countryId, *it)) return;
 
    ActivePolicy ap;
    ap.policyId = policyId;
    ap.countryId = countryId;
    ap.turnsRemaining = it->implementationTurns;
    ap.targetProvince = targetProvince;
    ap.targetMinority = targetMinority;
 
    int idx = (int)m_activePolicies.size();
    m_activePolicies.push_back(ap);
    m_countryActivePolicyIndices[countryId].push_back(idx);
}
 
void Game::cancelPolicy(int activePolicyIndex) {
    if (activePolicyIndex < 0 || activePolicyIndex >= (int)m_activePolicies.size()) return;
    auto& ap = m_activePolicies[activePolicyIndex];
    if (ap.turnsRemaining > 0) {
        ap.turnsRemaining = -1;  // Mark as cancelled
        m_policiesEnactedThisTurn = std::max(0, m_policiesEnactedThisTurn - 1);
    } else if (ap.turnsRemaining == 0) {
        ap.turnsRemaining = -1;  // Mark as repealed
    }
}
 
void Game::shiftCountryCompass(int countryId, float econDelta, float socDelta) {
    auto it = m_countryCompass.find(countryId);
    if (it != m_countryCompass.end()) {
        it->second.economic = std::clamp(it->second.economic + econDelta, -100.0f, 100.0f);
        it->second.social = std::clamp(it->second.social + socDelta, -100.0f, 100.0f);
    }
}
 
void Game::applyPolicyEffects(int countryId) {
    auto cs = computeCountryIncome(countryId);
    float totalCost = 0;
 
    auto& indices = m_countryActivePolicyIndices[countryId];
    // Process in reverse to allow erasing
    for (int i = (int)indices.size() - 1; i >= 0; --i) {
        int apIdx = indices[i];
        if (apIdx >= (int)m_activePolicies.size()) continue;
        auto& ap = m_activePolicies[apIdx];
        if (ap.countryId != countryId) continue;
 
        const Policy* p = nullptr;
        for (const auto& policy : m_allPolicies) {
            if (policy.id == ap.policyId) { p = &policy; break; }
        }
        if (!p) continue;
 
        if (ap.turnsRemaining > 0) {
            // Still implementing
            ap.turnsRemaining--;
            // Hitting 0 means it just went live — flag the Politics button so
            // the player notices without having to poll the panel every turn.
            if (ap.turnsRemaining == 0 && countryId == m_playerCountryId)
                m_politicsAlert = true;
            totalCost += p->costPerTurn;
            // Apply compass shift during implementation
            shiftCountryCompass(countryId, p->econShift / p->implementationTurns, p->socShift / p->implementationTurns);
        } else if (ap.turnsRemaining == 0) {
            // Active policy - check if finite duration
            totalCost += p->costPerTurn;
            // Continuous compass shift
            shiftCountryCompass(countryId, p->econShift / 50.0f, p->socShift / 50.0f);
            // Apply public opinion shift
            if (p->effect.publicOpinionShift != 0.0f) {
                for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
                    if (prov.countryId != countryId) continue;
                    auto pcIt = m_provinceCompass.find(pid);
                    if (pcIt != m_provinceCompass.end()) {
                        pcIt->second.x += p->effect.publicOpinionShift;
                        pcIt->second.y += p->effect.publicOpinionShift;
                        pcIt->second.x = std::clamp(pcIt->second.x, -100.0f, 100.0f);
                        pcIt->second.y = std::clamp(pcIt->second.y, -100.0f, 100.0f);
                    }
                }
            }
            if (p->propagandaDuration > 0) {
                // Start duration countdown (negative: -duration-1)
                ap.turnsRemaining = -(p->propagandaDuration + 1);
            }
        } else if (ap.turnsRemaining < -1) {
            // Finite-duration propaganda policy still active
            totalCost += p->costPerTurn;
            shiftCountryCompass(countryId, p->econShift / 50.0f, p->socShift / 50.0f);
            // Apply public opinion shift
            if (p->effect.publicOpinionShift != 0.0f) {
                for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
                    if (prov.countryId != countryId) continue;
                    auto pcIt = m_provinceCompass.find(pid);
                    if (pcIt != m_provinceCompass.end()) {
                        pcIt->second.x += p->effect.publicOpinionShift;
                        pcIt->second.y += p->effect.publicOpinionShift;
                        pcIt->second.x = std::clamp(pcIt->second.x, -100.0f, 100.0f);
                        pcIt->second.y = std::clamp(pcIt->second.y, -100.0f, 100.0f);
                    }
                }
            }
            ap.turnsRemaining++; // count toward -1 (auto-cancel)
        }
    }
 
    // Deduct costs from country balance
    if (totalCost > 0 && m_countryBalances.find(countryId) != m_countryBalances.end()) {
        m_countryBalances[countryId] -= totalCost;
    }
}

void Game::applyEthnicPolicyEffects(int countryId) {
    // Per-turn ethnic policy effects on alignment drift, population, and compass
    std::unordered_set<std::string> processed; // track which minorities we've processed
    float totalCost = 0.0f;

    for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
        if (prov.countryId != countryId) continue;
        auto mit = m_provinceMinorities.find(pid);
        if (mit == m_provinceMinorities.end()) continue;
        for (auto& mg : mit->second) {
            if (processed.count(mg.name)) continue;
            processed.insert(mg.name);

            // Accumulate alignment drift, from THIS country's option set.
            float driftThisTurn = 0.0f;
            float growthPctThisTurn = 0.0f; // summed over categories, applied once below
            for (size_t ci = 0; ci < m_ethnicPolicyCategories.size(); ci++) {
                const int oi = ethnicPolicyOption(countryId, mg.name, ci);
                if (oi < 0 || oi >= (int)m_ethnicPolicyCategories[ci].options.size()) continue;
                auto& opt = m_ethnicPolicyCategories[ci].options[oi];
                driftThisTurn += opt.alignmentPerTurn;
                totalCost += opt.costPerTurn;

                // Population growth: only accumulated here. Applying it inside this
                // loop compounded the rate once per category (six multiplications a
                // turn instead of one), which is what let populations blow past 1e15
                // in long runs.
                growthPctThisTurn += opt.popGrowthPerTurn;

                // Compass shift toward government
                if (opt.compassShiftEcon != 0.0f || opt.compassShiftSoc != 0.0f) {
                    auto pcIt = m_provinceCompass.find(pid);
                    auto govIt = m_countryCompass.find(countryId);
                    if (pcIt != m_provinceCompass.end() && govIt != m_countryCompass.end()) {
                        float dx = govIt->second.economic - pcIt->second.x;
                        float dy = govIt->second.social - pcIt->second.y;
                        pcIt->second.x -= dx * opt.compassShiftEcon / 100.0f;
                        pcIt->second.y -= dy * opt.compassShiftSoc / 100.0f;
                    }
                }
            }

            // Population growth, applied once per province per turn from the summed
            // per-category rate.
            {
                auto popIt = m_provincePopulations.find(pid);
                if (popIt != m_provincePopulations.end() && growthPctThisTurn != 0.0f) {
                    long long pop = popIt->second;
                    float rate = std::clamp(growthPctThisTurn, -50.0f, 50.0f) / 100.0f;
                    if (rate > 0.0f) {
                        // Logistic taper toward MAX_PROVINCE_POP. A bare clamp also
                        // bounds the value, but it parks every province at exactly the
                        // ceiling, and a population feature that reads the same for
                        // every province is no more use to the AI than an overflowing
                        // one. Tapering keeps provinces spread out below the ceiling.
                        double headroom = 1.0 - (double)pop / (double)MAX_PROVINCE_POP;
                        rate *= (float)std::max(0.0, headroom);
                    }
                    long long growth = (long long)((double)pop * rate);
                    popIt->second = std::clamp(pop + growth, 0LL, MAX_PROVINCE_POP);
                }
            }

            // Ongoing war debuff: if this province was conquered from an enemy,
            // minorities get a per-turn alignment penalty while the war continues
            auto ctIt = m_provinceConquestTurn.find(pid);
            if (ctIt != m_provinceConquestTurn.end()) {
                auto prevIt = m_conqueredProvincePrevOwner.find(pid);
                if (prevIt != m_conqueredProvincePrevOwner.end() && prevIt->second > 0) {
                    const Country* curC = m_countries.getCountry(countryId);
                    const Country* prevC = m_countries.getCountry(prevIt->second);
                    if (curC && prevC) {
                        auto ar = m_relations.find(curC->isoA3);
                        if (ar != m_relations.end()) {
                            auto dr = ar->second.find(prevC->isoA3);
                            if (dr != ar->second.end() && dr->second.war) {
                                driftThisTurn -= 5.0f;
                                if (processed.size() < 10) // log first few per country
                                    printf("[DIAG] War alignment penalty for %s in conquered province %d (%s vs %s): -5/turn\n",
                                           mg.name.c_str(), pid, curC->name.c_str(), prevC->name.c_str());
                            }
                        }
                    }
                }
            }
            m_minorityAlignmentDrift[countryId][mg.name] += driftThisTurn;
        }
    }

    // Deduct costs from country balance
    if (totalCost > 0 && m_countryBalances.count(countryId)) {
        m_countryBalances[countryId] -= (long long)totalCost;
    }
}

void Game::updatePolicies() {
    // Called each turn
    m_policiesEnactedThisTurn = 0;

    // Apply pending claim changes
    if (m_playerCountryId > 0 && (!m_claimsPendingDrop.empty() || !m_claimsPendingAdd.empty())) {
        const Country* pc2 = m_countries.getCountry(m_playerCountryId);
        if (pc2) {
            for (int pid : m_claimsPendingDrop) revokeClaim(pc2->isoA3, pid);
            // The add half used to touch m_claims only, so a claim staked from
            // the Claims tab never showed up under "Claimed by" and never
            // stirred any unrest in the province it was staked on.
            for (int pid : m_claimsPendingAdd) grantClaim(pc2->isoA3, pid);
            if (m_renderer && m_showClaims && m_playerCountryId > 0) {
                m_lastClaimsCountryId = m_playerCountryId;
                generateClaimsTexture();
            }
        }
        m_claimsPendingDrop.clear();
        m_claimsPendingAdd.clear();
    }

    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID || cid == SPC_CID) continue;
        applyPolicyEffects(cid);
        applyEthnicPolicyEffects(cid);
        // NOTE: artillery is deliberately NOT processed here. processCountryTurn()
        // already calls processArtilleryOrders() for every country earlier in the
        // turn; doing it again here made every bombardment apply its damage twice.
    }
    // Clean up cancelled/completed policies
    std::vector<ActivePolicy> newActive;
    std::unordered_map<int, std::vector<int>> newIndices;
    for (size_t i = 0; i < m_activePolicies.size(); ++i) {
        if (m_activePolicies[i].turnsRemaining != -1) {  // Keep implementing, active, and propaganda
            int newIdx = (int)newActive.size();
            newActive.push_back(m_activePolicies[i]);
            newIndices[m_activePolicies[i].countryId].push_back(newIdx);
        }
    }
    m_activePolicies = std::move(newActive);
    m_countryActivePolicyIndices = std::move(newIndices);
}
 
float Game::getCountryUnrest(int countryId) const {
    // Calculate unrest based on:
    // - Minority percentage vs national identity
    // - Distance from government compass to province compass
    // - Economic conditions
    // - Active policies
    float unrest = 0.0f;
 
    // Economic factor
    auto it = m_countryCompass.find(countryId);
    if (it != m_countryCompass.end()) {
        // High inequality (far right econ) or extreme left econ increases unrest
        float econAbs = fabsf(it->second.economic);
        if (econAbs > 50) unrest += (econAbs - 50) * 0.1f;
    }
 
    // Minority factor
    long long totalPop = 0;
    long long minorityPop = 0;
    for (auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId != countryId) continue;
        long long pop = m_provincePopulations.count(pid) ? m_provincePopulations.at(pid) : 0;
        totalPop += pop;
        auto mit = m_provinceMinorities.find(pid);
        if (mit != m_provinceMinorities.end()) {
            for (auto& mg : mit->second) {
                minorityPop += (long long)(pop * mg.pct / 100.0f);
            }
        }
    }
    if (totalPop > 0) {
        float minorityPct = (float)minorityPop / (float)totalPop * 100.0f;
        unrest += minorityPct * 0.2f;  // Base unrest from minorities
    }
 
    // Province-level unrest (distance from gov compass)
    if (it != m_countryCompass.end()) {
        for (auto& [pid, p] : m_provinces.getAllProvinces()) {
            if (p.countryId != countryId) continue;
            auto pCompassIt = m_provinceCompass.find(pid);
            if (pCompassIt != m_provinceCompass.end()) {
                float dx = pCompassIt->second.x + it->second.economic;
                float dy = pCompassIt->second.y + it->second.social;
                float dist = sqrtf(dx*dx + dy*dy);
                if (dist > 80) unrest += (dist - 80) * 0.05f;
            }
        }
    }
 
    // Active policy effects
    auto idxIt = m_countryActivePolicyIndices.find(countryId);
    if (idxIt != m_countryActivePolicyIndices.end()) {
        for (int apIdx : idxIt->second) {
            if (apIdx >= (int)m_activePolicies.size()) continue;
            const auto& ap = m_activePolicies[apIdx];
            if (ap.turnsRemaining >= 0) {
                for (const auto& p : m_allPolicies) {
                    if (p.id == ap.policyId) {
                        unrest -= p.effect.unrestReduction;
                        break;
                    }
                }
            }
        }
    }
 
    return std::min(100.0f, std::max(0.0f, unrest));
}

float Game::getProvinceRebellionChance(int provinceId, int countryId) const {
    float polUnrest = 0.0f, ethUnrest = 0.0f;

    // Baseline: a centrist, well-governed province is STABLE (near-zero). The
    // old formula floored base at 1.0 for every province, which — multiplied
    // across dozens of provinces every turn — guaranteed universal early-game
    // fragmentation. Now baseline rises only with ideological extremeness.
    float base = 0.5f;
    auto pcIt = m_provinceCompass.find(provinceId);
    if (pcIt != m_provinceCompass.end()) {
        float extremeness = (fabsf(pcIt->second.x) + fabsf(pcIt->second.y)) * 0.5f;
        base = extremeness * 0.04f;
    }

    auto govIt = m_countryCompass.find(countryId);
    auto pcIt2 = m_provinceCompass.find(provinceId);
    if (pcIt2 != m_provinceCompass.end() && govIt != m_countryCompass.end()) {
        // Unrest comes from a province DISAGREEING with its government, i.e.
        // the DISTANCE between the two compasses. This was a '+', which made a
        // province ALIGNED with its government maximally unhappy and one that
        // was its exact opposite perfectly content — backwards, and the main
        // driver of universal early fragmentation.
        float dx = pcIt2->second.x - govIt->second.economic;
        float dy = pcIt2->second.y - govIt->second.social;
        float dist = sqrtf(dx*dx + dy*dy);
        if (dist > 80) polUnrest = std::min(15.0f, (dist - 80) * 0.1f);
    }
    auto mit = m_provinceMinorities.find(provinceId);
    if (mit != m_provinceMinorities.end()) {
        for (auto& mg : mit->second) {
            float align = getMinorityAlignment(countryId, mg.name);
            float coeff = (100.0f - align) / 100.0f;
            float pct01 = mg.pct * 0.01f;
            ethUnrest += (coeff * pct01) * (coeff * pct01) * 5.0f;
        }
    }
    // Claims on this province increase unrest (foreign claims agitate population)
    // Claimant resolved via the ISO index — this function runs for every owned
    // province of every country each turn plus per-frame in three UI panels,
    // and the old linear scan over ALL countries per claimant grew with every
    // rebel state ever created.
    float claimUnrest = 0.0f;
    auto claimIt = m_claimsByProvince.find(provinceId);
    if (claimIt != m_claimsByProvince.end()) {
        const Country* ownerC = m_countries.getCountry(countryId);
        for (auto& claimantIso : claimIt->second) {
            int claimantCid = cidForIso(claimantIso);
            if (claimantCid < 0 || claimantCid == countryId) continue;
            bool atWar = false;
            auto relIt = m_relations.find(claimantIso);
            if (relIt != m_relations.end() && ownerC) {
                auto rt = relIt->second.find(ownerC->isoA3);
                if (rt != relIt->second.end() && rt->second.war) atWar = true;
            }
            claimUnrest += atWar ? 6.0f : 2.0f;
        }
    }
    // War weariness: unrest carried by a country that answered an ally's call
    // to arms. Country-wide rather than per-province — it is a national mood,
    // not a local grievance.
    float total = base + polUnrest + ethUnrest + claimUnrest + warWearinessOf(countryId);

    // An empty treasury is felt everywhere at once. Unlike the weariness term
    // this does not accumulate or decay — it is on while the country cannot pay
    // for itself and off the turn it can, which makes solvency something a
    // government can see the value of immediately rather than several turns
    // later. See BANKRUPTCY_UNREST_PCT.
    if (m_bankruptCountries.count(countryId)) total += BANKRUPTCY_UNREST_PCT;

    // Active policies advertising "unrest reduction" now actually reduce it.
    // (The effect used to be applied only in getCountryUnrest(), which nothing
    // ever called — the tooltip promised a reduction that never happened.)
    auto apIt = m_countryActivePolicyIndices.find(countryId);
    if (apIt != m_countryActivePolicyIndices.end()) {
        for (int idx : apIt->second) {
            if (idx < 0 || idx >= (int)m_activePolicies.size()) continue;
            const ActivePolicy& ap = m_activePolicies[idx];
            if (ap.turnsRemaining != 0) continue; // only fully active policies
            for (const auto& p : m_allPolicies)
                if (p.id == ap.policyId) { total -= p.effect.unrestReduction; break; }
        }
    }

    // Suppression from pacification allocation.
    // SUBTRACTED, not multiplied: the player reads "Suppression: X%" next to
    // "Unrest: Y%" and rightly expects X >= Y to mean no rebellion. The old
    // multiplicative form (total * (1 - pac*50/100)) capped out at halving the
    // chance, so even max funding could never actually prevent a rebellion.
    float pac = 0.0f;
    if (countryId == m_playerCountryId) {
        pac = m_pacificationAllocation;
    } else {
        auto pacIt = m_countryPacification.find(countryId);
        if (pacIt != m_countryPacification.end()) pac = pacIt->second;
    }
    float suppressionPct = pac * 50.0f;
    total -= suppressionPct;

    // Inherent civil order. Every functioning state commands baseline loyalty,
    // so a well-governed, homogeneous, ideologically-aligned province is STABLE
    // at zero pacification — rebellion is driven by real grievance (extreme
    // mismatch, hostile minorities, foreign claims, war) that exceeds this
    // floor. Without it, the tiny per-province baseline unrest summed across
    // dozens of provinces made early fragmentation certain for every country,
    // and pacification was a mandatory tax rather than a tool for hotspots.
    total -= REBELLION_LOYALTY_FLOOR;

    return std::min(95.0f, std::max(0.0f, total));
}

float Game::getProvinceRebellionChance(int provinceId) const {
    return getProvinceRebellionChance(provinceId, m_playerCountryId);
}

// How a minority feels about the government it lives under — which is now a
// question about a specific government, not about the minority in the abstract.
// A group can be loyal in one country and in open revolt across the border,
// which is the point of letting each government set its own policy.
float Game::getMinorityAlignment(int countryId, const std::string& minorityName) const {
    float align = 50.0f;
    auto cIt = m_minorityAlignmentDrift.find(countryId);
    if (cIt != m_minorityAlignmentDrift.end()) {
        auto dit = cIt->second.find(minorityName);
        if (dit != cIt->second.end()) align += dit->second;
    }
    return std::max(0.0f, std::min(100.0f, align));
}

float Game::getMinorityAlignmentTrend(int countryId, const std::string& minorityName) const {
    float trend = 0.0f;
    for (size_t ci = 0; ci < m_ethnicPolicyCategories.size(); ci++) {
        const int oi = ethnicPolicyOption(countryId, minorityName, ci);
        if (oi >= 0 && oi < (int)m_ethnicPolicyCategories[ci].options.size())
            trend += m_ethnicPolicyCategories[ci].options[oi].alignmentPerTurn;
    }
    return trend;
}

// ═══════════════════════════════════════════════════════════════════
// ─── Policies Tab UI ──────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════

void Game::drawPoliciesTab() {
    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 180});
    int centerX = m_screenW / 2;
    int tabY = 80;

    // Title
    const Country* c = m_countries.getCountry(m_playerCountryId);
    if (c) {
        DrawText(TextFormat("%s - Doctrines", c->name.c_str()), centerX - 150, 30, 28, WHITE);
    }

    // Tabs
    const char* tabs[] = {"Available", "Implementing", "Active", "Analysis", "Ethnic"};
    int tabSpacing = 130;
    int nTabs = 5;
    int tabStartX = centerX - (nTabs * tabSpacing) / 2 + tabSpacing / 2;
    Vector2 mouse = getMouse();
 
    for (int t = 0; t < nTabs; ++t) {
        int tx = tabStartX + t * tabSpacing;
        bool active = (t == m_policyTab);
        Color tc = active ? hexToColor(m_config.accentColor) : LIGHTGRAY;
        int tw = MeasureText(tabs[t], 20);
        Rectangle tr = {(float)(tx - tw/2 - 10), (float)(tabY - 5), (float)(tw + 20), 30};
        DrawText(tabs[t], tx - tw/2, tabY, 20, tc);
        if (active) {
            DrawRectangle(tx - tw/2, tabY + 24, tw, 3, hexToColor(m_config.accentColor));
        }
        if (CheckCollisionPointRec(mouse, tr) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (m_policyTab != t) Audio::get().playSfx("tab_switch");
            m_policyTab = t;
            m_policyScroll = 0;
        }
    }
 
    // Close button
    Rectangle closeBtn = {(float)(m_screenW - 44), 8, 36, 36};
    bool closeHover = CheckCollisionPointRec(mouse, closeBtn);
    Color closeCol = closeHover ? RED : Color{180, 180, 180, 200};
    DrawRectangleRounded(closeBtn, 0.2f, 6, {60, 60, 70, 180});
    DrawRectangleRoundedLines(closeBtn, 0.2f, 6, closeCol);
int xw = MeasureText("X", 20);
    DrawText("X", (int)(closeBtn.x + closeBtn.width/2 - xw/2), 12, 20, closeCol);

    // Close button click
    if (closeHover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        Audio::get().playSfx("back");
        m_inPolitics = false;
        m_activeSidebarTab = 0;
        m_policyTab = 0;
        m_policyScroll = 0;
        m_selectedPolicyIdx = -1;
        return;
    }

    // Build active + implementing policy summary
    std::string activeSummary;
    std::string implementingSummary;
    int activeCount = 0, implementingCount = 0;
    for (size_t i = 0; i < m_activePolicies.size(); ++i) {
        const auto& ap = m_activePolicies[i];
        if (ap.countryId != m_playerCountryId) continue;
        const Policy* p = nullptr;
        for (const auto& pol : m_allPolicies) if (pol.id == ap.policyId) { p = &pol; break; }
        if (!p) continue;
        if (ap.turnsRemaining > 0) {
            if (implementingCount > 0) implementingSummary += ", ";
            implementingSummary += p->name;
            implementingCount++;
        } else if (ap.turnsRemaining == 0 || ap.turnsRemaining < -1) {
            if (activeCount > 0) activeSummary += ", ";
            activeSummary += p->name;
            activeCount++;
        }
    }
 
    int startY = tabY + 50;
    int listH = m_screenH - startY - 30;
 
    // Permanent status bar: active/implementing summary + per-turn actions
    {
        std::string statusText;
        if (activeCount > 0)
            statusText += "Active: " + activeSummary;
        if (implementingCount > 0) {
            if (!statusText.empty()) statusText += "  |  ";
            statusText += "Implementing: " + implementingSummary;
        }
        if (!statusText.empty()) statusText += "  |  ";
        int remaining = 3 - m_policiesEnactedThisTurn;
        statusText += TextFormat("Political actions: %d/3 remaining", remaining);
        int fSize = 13;
        int w = MeasureText(statusText.c_str(), fSize);
        int h = 18;
        int sx = std::max(10, centerX - w / 2 - 8);
        int sy = tabY + 28;
        // Dark background for readability
        DrawRectangle(sx, sy, w + 16, h, {0, 0, 0, 160});
        DrawText(statusText.c_str(), sx + 8, sy + 2, fSize,
            remaining > 0 ? WHITE : RED);
    }
 
    if (m_policyTab == 0) {
        // Available policies with folder grouping
        DrawText("Available Doctrines", 30, startY - 25, 20, WHITE);

        // Collect unique folders in display order
        std::vector<std::string> folderOrder = {"Left", "Right", "Authoritarian", "Libertarian", "Miscellaneous"};
        std::unordered_map<std::string, std::vector<int>> folderPolicies;
        for (size_t i = 0; i < m_allPolicies.size(); ++i) {
            std::string f = m_allPolicies[i].folder;
            if (f.empty()) f = "Miscellaneous";
            folderPolicies[f].push_back((int)i);
        }

        // Compute total height: folder headers (28 each) + policy rows (150 each) for open folders
        int folderHeaderH = 28;
        int policyItemH = 150;
        int totalH = 0;
        for (auto& fname : folderOrder) {
            auto it = folderPolicies.find(fname);
            if (it == folderPolicies.end()) continue;
            totalH += folderHeaderH;
            if (m_openFolders.count(fname)) {
                totalH += (int)it->second.size() * policyItemH;
            }
        }
        int maxScroll = std::max(0, totalH - listH);

        // Scrollbar background
        Rectangle scrollArea = {20, (float)startY, (float)(m_screenW - 30), (float)listH};
        DrawRectangleRec(scrollArea, {0, 0, 0, 60});

        BeginScissorMode(20, startY, m_screenW - 40, listH);
        int y = startY - m_policyScroll;
        for (auto& fname : folderOrder) {
            auto fit = folderPolicies.find(fname);
            if (fit == folderPolicies.end()) continue;

            // Folder header
            bool isOpen = m_openFolders.count(fname);
            Rectangle fhRect = {20, (float)y, (float)(m_screenW - 270), (float)folderHeaderH};
            DrawRectangleRec(fhRect, {50, 50, 60, 180});
            DrawText(TextFormat("%s %s", isOpen ? "▼" : "▶", fname.c_str()), 30, y + 4, 18, hexToColor(m_config.accentColor));
            if (CheckCollisionPointRec(mouse, fhRect) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                if (isOpen) { m_openFolders.erase(fname);  Audio::get().playSfx("panel_close"); }
                else       { m_openFolders.insert(fname); Audio::get().playSfx("panel_open");  }
            }
            y += folderHeaderH;

            if (!isOpen) continue;

            for (int pi : fit->second) {
                const auto& p = m_allPolicies[pi];
                if (!c) continue;
                bool canEnact = canCountryEnactPolicy(m_playerCountryId, p);
                Color nameCol = canEnact ? WHITE : Color{100, 100, 120, 200};
                Color bgCol = (m_selectedPolicyIdx == pi) ? Color{80, 80, 100, 180} : Color{40, 40, 50, 180};
                Rectangle row = {20, (float)y, (float)(m_screenW - 270), 140};
                DrawRectangleRounded(row, 0.1f, 6, bgCol);
                DrawRectangleRoundedLines(row, 0.1f, 6, canEnact ? Color{100, 150, 100, 150} : Color{80, 80, 100, 100});

                DrawText(p.name.c_str(), 30, y + 4, 20, nameCol);
                DrawText(p.description.c_str(), 30, y + 28, 13, LIGHTGRAY);

                // Category badge
                Color catCol = GRAY;
                if (p.category == "left") catCol = {100, 200, 100, 255};
                else if (p.category == "right") catCol = {255, 200, 100, 255};
                else if (p.category == "authoritarian") catCol = {200, 100, 100, 255};
                else if (p.category == "libertarian") catCol = {100, 150, 255, 255};
                else catCol = {180, 180, 180, 255};
                DrawText(p.category.c_str(), 30, y + 50, 11, catCol);

                // Duration info for propaganda
                if (p.propagandaDuration > 0) {
                    DrawText(TextFormat("Campaign: %d turns", p.propagandaDuration), 130, y + 50, 11, Color{200, 150, 255, 200});
                }

                // Info: cost, turns, compass shift
                std::string info = TextFormat("Cost: %d/turn | Setup: %d turn(s) | Shift: E%.0f S%.0f",
                    p.costPerTurn, p.implementationTurns, p.econShift, p.socShift);
                DrawText(info.c_str(), 30, y + 66, 12, Color{150, 150, 170, 200});

                // Tradeoffs - Gains (green)
                int tx = 30;
                for (size_t g = 0; g < p.tradeoffs.gains.size(); ++g) {
                    DrawText(TextFormat("+ %s", p.tradeoffs.gains[g].c_str()), tx, y + 84, 12, Color{100, 255, 100, 200});
                    tx += MeasureText(p.tradeoffs.gains[g].c_str(), 12) + 24;
                }

                // Tradeoffs - Costs (red)
                tx = 30;
                for (size_t g = 0; g < p.tradeoffs.costs.size(); ++g) {
                    DrawText(TextFormat("- %s", p.tradeoffs.costs[g].c_str()), tx, y + 100, 12, Color{255, 100, 100, 200});
                    tx += MeasureText(p.tradeoffs.costs[g].c_str(), 12) + 24;
                }

                // Incompatibility warning - red highlight for conflicts with ACTIVE/implementing policies
                if (!p.incompatibleWith.empty()) {
                    std::string inc = "X Conflicts with: ";
                    bool hasConflict = false;
                    for (size_t ic = 0; ic < p.incompatibleWith.size(); ++ic) {
                        // Check if this incompatible policy is active
                        bool isActiveConflict = false;
                        for (const auto& ap : m_activePolicies) {
                            if (ap.countryId != m_playerCountryId) continue;
                            if (ap.turnsRemaining == -1) continue;
                            if (ap.policyId == p.incompatibleWith[ic]) {
                                isActiveConflict = true;
                                break;
                            }
                        }
                        if (ic > 0) inc += ", ";
                        for (const auto& pol : m_allPolicies) {
                            if (pol.id == p.incompatibleWith[ic]) {
                                inc += pol.name;
                                break;
                            }
                        }
                        if (isActiveConflict) hasConflict = true;
                    }
                    DrawText(inc.c_str(), 30, y + 116, 10, hasConflict ? RED : Color{255, 200, 100, 200});
                }

                // Enact button
                bool enactLimitReached = (m_policiesEnactedThisTurn >= 3);
                Rectangle enactBtn = {(float)(m_screenW - 160), (float)(y + 12), 130, 44};
                if (canEnact && !enactLimitReached) {
                    bool hover = CheckCollisionPointRec(mouse, enactBtn);
                    DrawRectangleRounded(enactBtn, 0.2f, 6, hover ? Color{100, 180, 100, 255} : Color{80, 150, 80, 255});
                    DrawText("Enact", (int)(enactBtn.x + enactBtn.width/2 - MeasureText("Enact", 18)/2), y + 22, 18, WHITE);
                    if (hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                        enactPolicy(m_playerCountryId, p.id);
                        m_policiesEnactedThisTurn++;
                        Audio::get().playSfx("confirm");
                    }
                } else {
                    DrawRectangleRounded(enactBtn, 0.2f, 6, Color{80, 80, 90, 180});
                    const char* label = enactLimitReached ? "No actions" : "Locked";
                    DrawText(label, (int)(enactBtn.x + enactBtn.width/2 - MeasureText(label, 18)/2), y + 22, 18, Color{120, 120, 140, 200});
                    if (CheckCollisionPointRec(mouse, enactBtn) &&
                        IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
                        Audio::get().playSfx("deny");
                }

                y += policyItemH;
            }
        }
        EndScissorMode();



        // Scrollbar
        if (maxScroll > 0) {
            float barH = (float)listH * listH / totalH;
            float barY = (float)startY + (float)m_policyScroll / maxScroll * (listH - barH);
            DrawRectangle(m_screenW - 18, (int)barY, 8, (int)barH, {150, 150, 170, 100});
        }
    } else if (m_policyTab == 1) {
        // Implementing policies
        DrawText("Implementing Doctrines", 30, startY - 25, 20, WHITE);
        int y = startY;
        bool any = false;
        for (size_t i = 0; i < m_activePolicies.size(); ++i) {
            const auto& ap = m_activePolicies[i];
            if (ap.countryId != m_playerCountryId || ap.turnsRemaining <= 0) continue;
            any = true;
            const Policy* p = nullptr;
            for (const auto& pol : m_allPolicies) if (pol.id == ap.policyId) { p = &pol; break; }
            if (!p) continue;
 
            Rectangle row = {20, (float)y, (float)(m_screenW - 40), 90};
            DrawRectangleRounded(row, 0.1f, 6, Color{60, 50, 40, 200});
            DrawRectangleRoundedLines(row, 0.1f, 6, Color{150, 120, 80, 150});
 
            // Folder badge
            std::string folder = p->folder.empty() ? "Misc" : p->folder;
            Color fCol = GRAY;
            if (folder == "Left") fCol = {100, 200, 100, 200};
            else if (folder == "Right") fCol = {255, 200, 100, 200};
            else if (folder == "Authoritarian") fCol = {200, 100, 100, 200};
            else if (folder == "Libertarian") fCol = {100, 150, 255, 200};
            DrawText(TextFormat("[%s]", folder.c_str()), m_screenW - 220, y + 4, 12, fCol);

            DrawText(TextFormat("%s (Implementing: %d turns left)", p->name.c_str(), ap.turnsRemaining), 30, y + 4, 18, ORANGE);
            DrawText(p->description.c_str(), 30, y + 26, 13, LIGHTGRAY);
            DrawText(TextFormat("Compass shift per turn: E%.1f S%.1f", p->econShift / p->implementationTurns, p->socShift / p->implementationTurns),
                30, y + 48, 12, Color{150, 150, 170, 200});
 
            // Cancel button
            Rectangle cancelBtn = {(float)(m_screenW - 160), (float)(y + 20), 130, 44};
            bool hover = CheckCollisionPointRec(mouse, cancelBtn);
            DrawRectangleRounded(cancelBtn, 0.2f, 6, hover ? Color{180, 80, 80, 255} : Color{150, 60, 60, 255});
            DrawText("Cancel", (int)(cancelBtn.x + cancelBtn.width/2 - MeasureText("Cancel", 18)/2), y + 30, 18, WHITE);
            if (hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                Audio::get().playSfx("back");
                cancelPolicy((int)i);
            }
 
            y += 100;
        }
        if (!any) {
            DrawText("No policies currently implementing", 30, startY + 20, 16, Color{120, 120, 140, 200});
        }
    } else if (m_policyTab == 2) {
        // Active policies (permanent + propaganda campaigns)
        DrawText("Active Doctrines", 30, startY - 25, 20, WHITE);
        int y = startY;
        bool any = false;
        for (size_t i = 0; i < m_activePolicies.size(); ++i) {
            const auto& ap = m_activePolicies[i];
            if (ap.countryId != m_playerCountryId) continue;
            // Active if: permanent (turnsRemaining == 0) OR propaganda (-1 < turnsRemaining < 0)
            if (ap.turnsRemaining != 0 && (ap.turnsRemaining >= -1 || ap.turnsRemaining > 0)) continue;
            any = true;
            const Policy* p = nullptr;
            for (const auto& pol : m_allPolicies) if (pol.id == ap.policyId) { p = &pol; break; }
            if (!p) continue;
 
            Rectangle row = {20, (float)y, (float)(m_screenW - 40), 90};
            DrawRectangleRounded(row, 0.1f, 6, Color{40, 60, 40, 200});
            DrawRectangleRoundedLines(row, 0.1f, 6, Color{80, 150, 80, 150});
 
            // Folder badge
            std::string folder = p->folder.empty() ? "Misc" : p->folder;
            Color fCol = GRAY;
            if (folder == "Left") fCol = {100, 200, 100, 200};
            else if (folder == "Right") fCol = {255, 200, 100, 200};
            else if (folder == "Authoritarian") fCol = {200, 100, 100, 200};
            else if (folder == "Libertarian") fCol = {100, 150, 255, 200};
            DrawText(TextFormat("[%s]", folder.c_str()), m_screenW - 220, y + 4, 12, fCol);

            // Propaganda duration indicator
            bool isPropaganda = (ap.turnsRemaining < -1);
            std::string nameStr = p->name;
            if (isPropaganda) {
                int remaining = -(ap.turnsRemaining + 1);
                nameStr += TextFormat(" (%d turn(s) left)", remaining);
            }
            DrawText(nameStr.c_str(), 30, y + 4, 18, isPropaganda ? Color{200, 150, 255, 255} : GREEN);
            DrawText(p->description.c_str(), 30, y + 26, 13, LIGHTGRAY);
            DrawText(TextFormat("Cost: %d/turn | Shift/turn: E%.2f S%.2f | Unrest reduction: %.2f%%",
                p->costPerTurn, p->econShift/50.0f, p->socShift/50.0f, p->effect.unrestReduction*100),
                30, y + 48, 12, Color{150, 180, 150, 200});
 
            // Remove button
            Rectangle removeBtn = {(float)(m_screenW - 160), (float)(y + 20), 130, 44};
            bool hover = CheckCollisionPointRec(mouse, removeBtn);
            DrawRectangleRounded(removeBtn, 0.2f, 6, hover ? Color{100, 100, 180, 255} : Color{80, 80, 150, 255});
            DrawText("Repeal", (int)(removeBtn.x + removeBtn.width/2 - MeasureText("Repeal", 18)/2), y + 30, 18, WHITE);
            if (hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                Audio::get().playSfx("toggle_off");
                cancelPolicy((int)i);  // Mark as completed/removed
            }
 
            y += 100;
        }
        if (!any) {
            DrawText("No active policies", 30, startY + 20, 16, Color{120, 120, 140, 200});
        }
    } else if (m_policyTab == 3) {
        drawAnalysisTab();
    } else if (m_policyTab == 4) {
        drawEthnicTab();
    }
}
 
void Game::updatePoliciesTab() {
    Vector2 mouse = getMouse();
    if (IsKeyPressed(KEY_ESCAPE)) {
        m_inPolitics = false;
        m_activeSidebarTab = 0;
        m_policyTab = 0;
        m_policyScroll = 0;
        m_selectedPolicyIdx = -1;
    }
    // Scroll wheel for available policies
    if (m_policyTab == 0) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0) {
            int startY = 130;
            int listH = m_screenH - startY - 30;
            int scrollStep = 150;
            // Compute approx total height: folder headers + all policy rows
            std::vector<std::string> folderOrder = {"Left", "Right", "Authoritarian", "Libertarian", "Miscellaneous"};
            int totalH = 0;
            for (auto& fname : folderOrder) {
                bool hasPolicy = false;
                for (auto& p : m_allPolicies) {
                    std::string f = p.folder.empty() ? "Miscellaneous" : p.folder;
                    if (f == fname) { hasPolicy = true; break; }
                }
                if (hasPolicy) totalH += 28; // folder header
                if (m_openFolders.count(fname)) {
                    for (auto& p : m_allPolicies) {
                        std::string f = p.folder.empty() ? "Miscellaneous" : p.folder;
                        if (f == fname) totalH += 150;
                    }
                }
            }
            int maxScroll = std::max(0, totalH - listH);
            m_policyScroll = std::clamp(m_policyScroll - (int)(wheel * scrollStep), 0, maxScroll);
        }
    }
    // Scroll wheel for analysis tab
    if (m_policyTab == 3) {
        // Check "Go to" button clicks
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            for (auto& [pid, rect] : m_analysisGoToButtons) {
                if (CheckCollisionPointRec(mouse, rect)) {
                    Audio::get().playSfx("select_province", 0.05f);
                    m_renderer->setSelectedProvince(pid);
                    m_renderer->rebuildSelectionGlow();
                    m_flyToLockTimer = 90;
                    m_blockLeftPanTimer = 5; // block MapRenderer click for ~5 frames
                    auto cit = m_provinceCenters.find(pid);
                    if (cit != m_provinceCenters.end()) {
                        m_renderer->flyTo(cit->second.x, cit->second.y, 2.5f, 3.0f);
                        m_inEconomy = false;
                        m_activeSidebarTab = 0;
                    }
                    break;
                }
            }
        }

        float wheel = GetMouseWheelMove();
        if (wheel != 0) {
            int splitY = 80 + (m_screenH - 160) * 50 / 100;
            int gap = 6;
            int hotStartY = 80 + 24;
            int hotAreaH = splitY - hotStartY - gap;
            int hotTotal = m_analysisHotspotCount * 22 + 24; // rows + header
            int hotMax = std::max(0, hotTotal - hotAreaH);

            int minStartY = splitY + gap + 24;
            int minAreaH = m_screenH - 30 - minStartY;
            int minTotal = 8 * 18 + 20; // approximate max
            int minMax = std::max(0, minTotal - minAreaH);

            // Check if mouse is in hotspot or minority area
            if (mouse.y >= hotStartY && mouse.y < splitY) {
                m_analysisHotspotScroll = std::clamp(m_analysisHotspotScroll - (int)(wheel * 20), 0, hotMax);
            } else if (mouse.y >= minStartY && mouse.y < m_screenH - 30) {
                m_analysisMinorityScroll = std::clamp(m_analysisMinorityScroll - (int)(wheel * 20), 0, minMax);
            }
        }
    }
    if (m_policyTab == 4) {
        updateEthnicTab();
    }
}

void Game::drawPoliticalCompass(int x, int y, int size, int countryId, bool showPopAverage) {
    DrawRectangle(x, y, size, size, {20, 20, 30, 220});
    DrawRectangleLines(x, y, size, size, {100, 100, 120, 255});

    // Axes
    int cx = x + size / 2;
    int cy = y + size / 2;
    DrawLine(x, cy, x + size, cy, {80, 80, 90, 200});
    DrawLine(cx, y, cx, y + size, {80, 80, 90, 200});

    // Labels
    DrawText("LEFT", x + 4, cy - 10, 10, {180, 180, 180, 200});
    DrawText("RIGHT", x + size - 40, cy - 10, 10, {180, 180, 180, 200});
    DrawText("AUTH", cx - 18, y + 4, 10, {180, 180, 180, 200});
    DrawText("LIB", cx - 14, y + size - 16, 10, {180, 180, 180, 200});

    // Country position (gold circle)
    auto it = m_countryCompass.find(countryId);
    if (it != m_countryCompass.end()) {
        int px = cx + (int)(it->second.economic * (size / 2 - 10) / 100.0f);
        int py = cy + (int)(it->second.social * (size / 2 - 10) / 100.0f);
        DrawCircle(px, py, 8, {255, 200, 50, 255});
        DrawCircleLines(px, py, 8, {255, 255, 255, 255});
        DrawCircle(px, py, 4, {255, 255, 255, 255});
    }

    // Population average (white +)
    if (showPopAverage) {
        float avgEcon = 0.0f, avgSoc = 0.0f;
        int count = 0;
        for (auto& [pid, p] : m_provinces.getAllProvinces()) {
            if (p.countryId != countryId) continue;
            auto pcIt = m_provinceCompass.find(pid);
            if (pcIt != m_provinceCompass.end()) {
                avgEcon += pcIt->second.x;
                avgSoc += pcIt->second.y;
                count++;
            }
        }
        if (count > 0) {
            avgEcon /= count;
            avgSoc /= count;
            int pax = cx + (int)(-avgEcon * (size / 2 - 10) / 100.0f);
            int pay = cy - (int)(avgSoc * (size / 2 - 10) / 100.0f);
            DrawLine(pax - 6, pay, pax + 6, pay, WHITE);
            DrawLine(pax, pay - 6, pax, pay + 6, WHITE);
            DrawLine(pax - 6, pay, pax + 6, pay, {255, 255, 255, 150});
            DrawLine(pax, pay - 6, pax, pay + 6, {255, 255, 255, 150});
        }
    }
}

void Game::drawAnalysisTab() {
    const Country* c = m_countries.getCountry(m_playerCountryId);
    if (!c) return;

    DrawText(TextFormat("%s — Analysis", c->name.c_str()), 30, 30, 28, WHITE);
    drawPoliticalCompass(m_screenW - 280, 80, 200, m_playerCountryId, true);

    float unrest = 0.0f;
    int provCount = 0;
    for (auto& [pid, pv] : m_provinces.getAllProvinces()) {
        if (pv.countryId == m_playerCountryId) {
            unrest += getProvinceRebellionChance(pid);
            provCount++;
        }
    }
    if (provCount > 0) unrest /= provCount;
    int bx = m_screenW - 270, by = 300;
    int barW = 180, barH = 16;
    DrawRectangle(bx, by, barW, barH, {50, 30, 30, 200});
    Color unrestCol = unrest < 20 ? GREEN : (unrest < 40 ? ORANGE : RED);
    DrawRectangle(bx, by, (int)(std::min(unrest, 100.0f) / 100.0f * barW), barH, unrestCol);
    DrawRectangleLines(bx, by, barW, barH, {100, 100, 120, 255});
    DrawText(TextFormat("Unrest: %.1f%%", unrest), bx, by - 20, 14, unrestCol);

    int leftX = 30;
    int panelW = (m_screenW - 300) - leftX;
    int splitY = 80 + (m_screenH - 160) * 50 / 100;
    int gap = 6;

    // Section bounds
    int hotTitleY = 80;
    int hotStartY = hotTitleY + 24;
    int hotAreaH = splitY - hotStartY - gap;

    int minTitleY = splitY + gap;
    int minStartY = minTitleY + 24;
    int minAreaH = m_screenH - 30 - minStartY;

    // ── Find hotspots ──
    struct Hotspot { int pid; float rebelChance; std::string name; bool isEthnic; bool isPolitical; };
    std::vector<Hotspot> hotspots;
    m_analysisHotspotCount = 0;
    m_analysisGoToButtons.clear();

    for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
        if (prov.countryId != m_playerCountryId) continue;
        float total = getProvinceRebellionChance(pid, m_playerCountryId);
        // Compute factor breakdown for hotspot type classification
        float polUnrest = 0.0f, ethUnrest = 0.0f;
        auto govIt = m_countryCompass.find(m_playerCountryId);
        auto pcIt = m_provinceCompass.find(pid);
        if (pcIt != m_provinceCompass.end() && govIt != m_countryCompass.end()) {
            float dx = pcIt->second.x + govIt->second.economic;
            float dy = pcIt->second.y + govIt->second.social;
            float dist = sqrtf(dx*dx + dy*dy);
            if (dist > 80) polUnrest = std::min(15.0f, (dist - 80) * 0.1f);
        }
        auto mit = m_provinceMinorities.find(pid);
        if (mit != m_provinceMinorities.end()) {
            for (auto& mg : mit->second) {
                float align = getMinorityAlignment(m_playerCountryId, mg.name);
                float coeff = (100.0f - align) / 100.0f;
                float pct01 = mg.pct * 0.01f;
                ethUnrest += (coeff * pct01) * (coeff * pct01) * 5.0f;
            }
        }
        bool hasPol = polUnrest > 0.0f;
        bool hasEth = ethUnrest > 0.0f;
        bool polDominant = polUnrest >= ethUnrest;

        if (total >= 10.0f) {
            hotspots.push_back({pid, total, prov.name, hasEth && (!hasPol || !polDominant), hasPol && (polDominant || !hasEth)});
        }
    }
    std::sort(hotspots.begin(), hotspots.end(), [](auto& a, auto& b) { return a.rebelChance > b.rebelChance; });
    m_analysisHotspotCount = (int)hotspots.size();

    // ── Draw Hotspots ──
    DrawText("Political Hotspots", leftX, hotTitleY, 20, ORANGE);

    int hotRowH = 22, hotHeaderH = hotRowH + 2;
    int hotTotal = (int)hotspots.size() * hotRowH + hotHeaderH;
    int hotMaxScroll = std::max(0, hotTotal - hotAreaH);

    int btnW = 44;
    int cProv = 10, cReb = 140, cType = 200, cGo = panelW - btnW - 4;

    BeginScissorMode(leftX, hotStartY, panelW, hotAreaH);
    {
        int dy = hotStartY - m_analysisHotspotScroll;
        if (hotspots.empty()) {
            DrawText("No significant hotspots detected", leftX + 10, dy, 14, Color{120, 140, 120, 200});
        } else {
            DrawText("Province", leftX + cProv, dy, 11, LIGHTGRAY);
            DrawText("Rebel%", leftX + cReb, dy, 11, LIGHTGRAY);
            DrawText("Type", leftX + cType, dy, 11, LIGHTGRAY);
            dy += hotRowH;

            for (auto& hs : hotspots) {
                std::string typeStr;
                Color typeCol;
                if (hs.isEthnic && hs.isPolitical)     { typeStr = "Eth+Pol"; typeCol = Color{200,100,255,255}; }
                else if (hs.isEthnic)                    { typeStr = "Ethnic";  typeCol = Color{100,200,255,255}; }
                else if (hs.isPolitical)                 { typeStr = "Pol";     typeCol = Color{255,200,100,255}; }
                else                                     { typeStr = "Econ";    typeCol = Color{180,180,180,255}; }

                Color rc = hs.rebelChance > 30 ? RED : (hs.rebelChance > 15 ? ORANGE : Color{220, 220, 100, 255});
                DrawText(hs.name.c_str(), leftX + cProv, dy, 12, WHITE);
                DrawText(TextFormat("%.0f%%", hs.rebelChance), leftX + cReb, dy, 12, rc);
                DrawText(typeStr.c_str(), leftX + cType, dy, 12, typeCol);

                Rectangle goRect = {(float)(leftX + cGo), (float)(dy), (float)btnW, (float)(hotRowH - 2)};
                DrawRectangleRec(goRect, Color{60, 70, 90, 200});
                int goW = MeasureText("Go", 12);
                DrawText("Go", (int)(goRect.x + (btnW - goW) / 2), (int)(goRect.y + 3), 12, WHITE);
                m_analysisGoToButtons.push_back({hs.pid, goRect});

                dy += hotRowH;
            }
        }
    }
    EndScissorMode();

    // ── Minority Analysis ──
    DrawText("Minority Analysis", leftX, minTitleY, 20, Color{100, 200, 255, 255});

    struct MinAn { std::string name; float totalPct; long long pop; Color color; };
    std::unordered_map<std::string, MinAn> minMap;
    for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
        if (prov.countryId != m_playerCountryId) continue;
        long long pop = m_provincePopulations.count(pid) ? m_provincePopulations.at(pid) : 0;
        auto mit = m_provinceMinorities.find(pid);
        if (mit == m_provinceMinorities.end()) continue;
        for (auto& mg : mit->second) {
            auto& ma = minMap[mg.name];
            ma.name = mg.name;
            ma.totalPct += mg.pct;
            ma.pop += (long long)(pop * mg.pct / 100.0f);
            auto colIt = m_minorityColors.find(mg.name);
            ma.color = colIt != m_minorityColors.end() ? colIt->second : GRAY;
        }
    }
    std::vector<MinAn> minorities;
    for (auto& [n, ma] : minMap) minorities.push_back(ma);
    std::sort(minorities.begin(), minorities.end(), [](auto& a, auto& b) { return a.totalPct > b.totalPct; });
    if (minorities.size() > 8) minorities.resize(8);

    // Policy alignment modifiers
    std::unordered_map<std::string, float> alignMod;
    for (auto& ap : m_activePolicies) {
        const Policy* pp = nullptr;
        for (auto& p : m_allPolicies) if (p.id == ap.policyId) { pp = &p; break; }
        if (!pp) continue;
        if (pp->folder == "Authoritarian" || pp->name == "Secret Police" || pp->name == "Censorship")
            for (auto& ma : minorities) alignMod[ma.name] -= 20.0f;
        if (pp->effect.minorityGrowthRate != 0.0f && !pp->effect.targetMinority.empty())
            alignMod[pp->effect.targetMinority] += 30.0f;
        if (pp->name == "National Unity")
            for (auto& ma : minorities) alignMod[ma.name] -= 15.0f;
    }

    int minRowH = 34, minTotal = (int)minorities.size() * minRowH + minRowH + 2;
    int minMaxScroll = std::max(0, minTotal - minAreaH);
    int mcN = 12, mcP = 200, mcA = 340, mcV = 600;

    BeginScissorMode(leftX, minStartY, panelW, minAreaH);
    {
        int dy = minStartY - m_analysisMinorityScroll;
        if (minorities.empty()) {
            DrawText("No minority data available", leftX + 10, dy, 14, Color{120, 120, 140, 200});
        } else {
            DrawText("Minority", leftX + mcN, dy, 11, LIGHTGRAY);
            DrawText("Population", leftX + mcP, dy, 11, LIGHTGRAY);
            DrawText("Alignment", leftX + mcA, dy, 11, LIGHTGRAY);
            DrawText("Verdict", leftX + mcV, dy, 11, LIGHTGRAY);
            dy += minRowH;

            for (auto& ma : minorities) {
                // Line 1: name + population + alignment + verdict
                DrawRectangle(leftX + mcN - 10, dy + 3, 8, 8, ma.color);
                DrawText(ma.name.c_str(), leftX + mcN, dy + 1, 13, WHITE);
                char ps[32];
                if (ma.pop > 1000000) snprintf(ps, sizeof(ps), "%.1fM", ma.pop / 1000000.0f);
                else if (ma.pop > 1000) snprintf(ps, sizeof(ps), "%.1fK", ma.pop / 1000.0f);
                else snprintf(ps, sizeof(ps), "%lld", ma.pop);
                DrawText(ps, leftX + mcP, dy + 1, 13, LIGHTGRAY);

                float align = getMinorityAlignment(m_playerCountryId, ma.name);
                float trend = getMinorityAlignmentTrend(m_playerCountryId, ma.name);
                float drift = align - 50.0f;   // the same number, relative to neutral

                Color ac = align < 30 ? RED : (align < 60 ? ORANGE : GREEN);
                DrawText(TextFormat("%.0f%%", align), leftX + mcA, dy + 1, 13, ac);

                const char* vd;
                Color vc;
                if (align < 25)      { vd = "Hostile";  vc = RED; }
                else if (align < 50) { vd = "Unhappy";  vc = ORANGE; }
                else if (align < 75) { vd = "Neutral";  vc = LIGHTGRAY; }
                else                 { vd = "Loyal";    vc = GREEN; }
                DrawText(vd, leftX + mcV, dy + 1, 13, vc);

                // Line 2: modifiers (indented, smaller font)
                int modX = leftX + mcN + 10;
                if (drift < -0.01f) {
                    DrawText(TextFormat("Drift: %.0f", drift), modX, dy + 18, 10, Color{255, 140, 140, 200});
                    modX += MeasureText(TextFormat("Drift: %.0f", drift), 10) + 16;
                } else if (drift > 0.01f) {
                    DrawText(TextFormat("Drift: +%.0f", drift), modX, dy + 18, 10, Color{140, 255, 140, 200});
                    modX += MeasureText(TextFormat("Drift: +%.0f", drift), 10) + 16;
                }
                if (trend > 0.01f) {
                    DrawText(TextFormat("Trend: +%.1f%%/t", trend), modX, dy + 18, 10, Color{100, 255, 100, 200});
                    modX += MeasureText(TextFormat("Trend: +%.1f%%/t", trend), 10) + 16;
                } else if (trend < -0.01f) {
                    DrawText(TextFormat("Trend: %.1f%%/t", trend), modX, dy + 18, 10, Color{255, 100, 100, 200});
                    modX += MeasureText(TextFormat("Trend: %.1f%%/t", trend), 10) + 16;
                }
                // War-with-kin penalty (dynamic check)
                {
                    const Country* pc_a = m_countries.getCountry(m_playerCountryId);
                    if (pc_a) {
                        for (auto& [cid2, c2] : m_countries.getAll()) {
                            if (cid2 == m_playerCountryId) continue;
                            auto ar = m_relations.find(pc_a->isoA3);
                            if (ar == m_relations.end()) continue;
                            auto dr2 = ar->second.find(c2.isoA3);
                            if (dr2 == ar->second.end() || !dr2->second.war) continue;
                            long long kinPop = 0;
                            for (auto& [pid, pv] : m_provinces.getAllProvinces()) {
                                if (pv.countryId != cid2) continue;
                                long long pop = m_provincePopulations.count(pid) ? m_provincePopulations[pid] : 0;
                                auto kmit = m_provinceMinorities.find(pid);
                                if (kmit == m_provinceMinorities.end()) continue;
                                for (auto& kmg : kmit->second)
                                    if (kmg.name == ma.name)
                                        kinPop += (long long)(pop * kmg.pct / 100.0f);
                            }
                            if (kinPop >= 500000) {
                                DrawText(TextFormat("-30 War with %s", c2.name.c_str()), modX, dy + 18, 10, RED);
                                modX += MeasureText(TextFormat("-30 War with %s", c2.name.c_str()), 10) + 16;
                            }
                        }
                    }
                }

                dy += minRowH;
            }
        }
    }
    EndScissorMode();

    // ─── Pacification Budget Slider (right side) ────
    {
        auto cs = computeCountryIncome(m_playerCountryId);
        int slX = m_screenW - 270;
        int slY = 340;
        int slW = 200;
        int slH = 18;
        float researchAmt = cs.total * m_researchAllocation;
        float maxAffordPac = std::max(0.0f, cs.total - cs.armyExpenses - cs.navyExpenses - cs.policyCosts - cs.minorityCosts - researchAmt);
        float maxAllocFrac = (cs.total > 0) ? maxAffordPac / cs.total : 0;
        if (maxAllocFrac > 1.0f) maxAllocFrac = 1.0f;
        if (m_pacificationAllocation > maxAllocFrac) m_pacificationAllocation = maxAllocFrac;
        DrawText("Pacification Budget:", slX, slY - 20, 13, WHITE);
        DrawRectangle(slX, slY, slW, slH, {40, 40, 50, 200});
        int maxFill = (int)(slW * maxAllocFrac);
        if (maxFill > 0) DrawRectangle(slX, slY, maxFill, slH, {40, 50, 60, 150});
        DrawRectangleLines(slX, slY, slW, slH, {80, 80, 100, 200});
        int fillPac = (int)(slW * m_pacificationAllocation);
        if (fillPac > 0) DrawRectangle(slX, slY, fillPac, slH, {80, 180, 220, 200});
        DrawText(TextFormat("%d%%", (int)(m_pacificationAllocation * 100)), slX + slW + 6, slY + 2, 12, WHITE);
        float pacPct = m_pacificationAllocation * 50.0f;
        DrawText(TextFormat("Suppression: %.1f%%", pacPct), slX, slY + slH + 4, 11, LIGHTGRAY);
        {
            const Rectangle pacBar = {(float)slX, (float)slY, (float)slW, (float)slH};
            float t = m_pacificationAllocation;
            if (sliderInteract(pacBar, /*steps=*/0, t, m_draggingPacification))
                m_pacificationAllocation = std::clamp(t, 0.0f, maxAllocFrac);
        }
    }
}

void Game::drawEthnicTab() {
    int leftX = 30, panelW = m_screenW - 60;

    const Country* c = m_countries.getCountry(m_playerCountryId);
    DrawText(c ? TextFormat("%s — Ethnic Management", c->name.c_str()) : "Ethnic Management",
        leftX, 30, 28, WHITE);

    struct EthEntry { std::string name; float totalPct; long long pop; Color color; };
    std::unordered_map<std::string, EthEntry> ethMap;
    for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
        if (prov.countryId != m_playerCountryId) continue;
        long long pop = m_provincePopulations.count(pid) ? m_provincePopulations.at(pid) : 0;
        auto mit = m_provinceMinorities.find(pid);
        if (mit == m_provinceMinorities.end()) continue;
        for (auto& mg : mit->second) {
            auto& e = ethMap[mg.name];
            e.name = mg.name;
            e.totalPct += mg.pct;
            e.pop += (long long)(pop * mg.pct / 100.0f);
            auto colIt = m_minorityColors.find(mg.name);
            e.color = colIt != m_minorityColors.end() ? colIt->second : GRAY;
        }
    }
    std::vector<EthEntry> entries;
    for (auto& [n, e] : ethMap) entries.push_back(e);
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.totalPct > b.totalPct; });

    int titleY = 120;
    int startY = titleY + 4;
    int areaH = m_screenH - startY - 20;
    int rowH = 22;
    int expandedH = (int)m_ethnicPolicyCategories.size() * 24 + 10;
    int totalContent = 0;
    for (size_t i = 0; i < entries.size(); i++) {
        totalContent += rowH;
        if ((int)i == m_selectedEthnicity) totalContent += expandedH;
    }
    int maxScroll = std::max(0, totalContent - areaH);

    BeginScissorMode(leftX, startY, panelW, areaH);
    {
        int dy = startY - m_ethnicTabScroll;
        for (size_t ei = 0; ei < entries.size(); ei++) {
            auto& e = entries[ei];
            bool isSel = ((int)ei == m_selectedEthnicity);
            float align = getMinorityAlignment(m_playerCountryId, e.name);
            Color ac = align < 30 ? RED : (align < 60 ? ORANGE : GREEN);

            DrawRectangle(leftX, dy, panelW, rowH, isSel ? Color{60, 60, 80, 200} : Color{30, 30, 40, 180});
            DrawRectangle(leftX, dy, 6, rowH, e.color);
            DrawText(TextFormat("%s", e.name.c_str()), leftX + 12, dy + 3, 14, WHITE);
            char ps[32];
            if (e.pop > 1000000) snprintf(ps, sizeof(ps), "Pop: %.1fM", e.pop / 1000000.0f);
            else if (e.pop > 1000) snprintf(ps, sizeof(ps), "Pop: %.1fK", e.pop / 1000.0f);
            else snprintf(ps, sizeof(ps), "Pop: %lld", e.pop);
            DrawText(ps, leftX + 200, dy + 3, 12, LIGHTGRAY);
            DrawText(TextFormat("Alignment: %.0f%%", align), leftX + 380, dy + 3, 12, ac);
            DrawText(isSel ? "▲" : "▼", leftX + panelW - 30, dy + 2, 14, LIGHTGRAY);

            dy += rowH;
            if (!isSel) continue;

            // Expanded: show policy categories with radio buttons
            int xOff = leftX + 20;
            for (size_t ci = 0; ci < m_ethnicPolicyCategories.size(); ci++) {
                auto& cat = m_ethnicPolicyCategories[ci];
                const int optIdx = ethnicPolicyOption(m_playerCountryId, e.name, ci);

                DrawText(cat.displayName.c_str(), xOff, dy, 12, Color{200, 180, 150, 255});
                int rx = xOff + 200;
                for (size_t oi = 0; oi < cat.options.size(); oi++) {
                    bool selected = ((int)oi == optIdx);
                    Color oc = selected ? hexToColor(m_config.accentColor) : Color{150, 150, 150, 200};
                    DrawText(cat.options[oi].name.c_str(), rx, dy, 12, oc);
                    int nameW = MeasureText(cat.options[oi].name.c_str(), 12);
                    int qx = rx + nameW + 2;
                    DrawText("?", qx + 2, dy - 1, 12, {100, 140, 255, 220});
                    Rectangle qr = {(float)qx, (float)dy, 14, 14};
                    if (CheckCollisionPointRec(getMouse(), qr)) {
                        const char* desc = cat.options[oi].desc.c_str();
                        int tw = MeasureText(desc, 11);
                        int tipX = qx + 16, tipY = dy - 2;
                        if (tipX + tw + 12 > m_screenW) tipX = qx - tw - 24;
                        DrawRectangle(tipX, tipY, tw + 12, 22, {20, 20, 30, 240});
                        DrawRectangleLines(tipX, tipY, tw + 12, 22, {100, 100, 130, 200});
                        DrawText(desc, tipX + 6, tipY + 4, 11, {200, 200, 220, 255});
                    }
                    rx = rx + nameW + 30;
                }
                dy += 24;
            }
            dy += 8;
        }
    }
    EndScissorMode();

    if (maxScroll > 0) {
        float barH = (float)areaH * areaH / totalContent;
        float barY = (float)startY + (float)m_ethnicTabScroll / maxScroll * (areaH - barH);
        DrawRectangle(m_screenW - 18, (int)barY, 8, (int)barH, {150, 150, 170, 100});
    }
}

void Game::updateEthnicTab() {
    Vector2 mouse = getMouse();
    int leftX = 30, panelW = m_screenW - 60;
    int titleY = 120, startY = titleY + 4;
    int areaH = m_screenH - startY - 20;
    int rowH = 22;
    int expandedH = (int)m_ethnicPolicyCategories.size() * 24 + 10;

    float wheel = GetMouseWheelMove();
    if (wheel != 0) {
        std::unordered_set<std::string> names;
        for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
            if (prov.countryId != m_playerCountryId) continue;
            auto mit = m_provinceMinorities.find(pid);
            if (mit == m_provinceMinorities.end()) continue;
            for (auto& mg : mit->second) names.insert(mg.name);
        }
        int nEntries = (int)names.size();
        int totalContent = nEntries * rowH;
        if (m_selectedEthnicity >= 0 && m_selectedEthnicity < nEntries) totalContent += expandedH;
        int maxScroll = std::max(0, totalContent - areaH);
        m_ethnicTabScroll = std::clamp(m_ethnicTabScroll - (int)(wheel * 24), 0, maxScroll);
    }

    if (!IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) return;

    struct E { std::string name; float totalPct; };
    std::unordered_map<std::string, float> pctMap;
    for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
        if (prov.countryId != m_playerCountryId) continue;
        auto mit = m_provinceMinorities.find(pid);
        if (mit == m_provinceMinorities.end()) continue;
        for (auto& mg : mit->second) pctMap[mg.name] += mg.pct;
    }
    std::vector<E> entries;
    for (auto& [n, p] : pctMap) entries.push_back({n, p});
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.totalPct > b.totalPct; });

    int dy = startY - m_ethnicTabScroll;
    for (size_t ei = 0; ei < entries.size(); ei++) {
        Rectangle rowRect = {(float)leftX, (float)dy, (float)panelW, (float)rowH};
        if (CheckCollisionPointRec(mouse, rowRect)) {
            if ((int)ei == m_selectedEthnicity) m_selectedEthnicity = -1;
            else m_selectedEthnicity = (int)ei;
            return;
        }
        dy += rowH;
        if ((int)ei == m_selectedEthnicity) {
            int xOff = leftX + 20;
            int rdy = dy;
            for (size_t ci = 0; ci < m_ethnicPolicyCategories.size(); ci++) {
                int rx = xOff + 200;
                for (size_t oi = 0; oi < m_ethnicPolicyCategories[ci].options.size(); oi++) {
                    int nameW = MeasureText(m_ethnicPolicyCategories[ci].options[oi].name.c_str(), 12);
                    Rectangle optRect = {(float)rx, (float)(rdy - 2), (float)(nameW + 18), 18};
                    if (CheckCollisionPointRec(mouse, optRect)) {
                        // The player edits THEIR government's policy, not the
                        // world's. setEthnicPolicyOption fills a defaulted row
                        // on first touch, which is what the open-coded resize
                        // here was doing.
                        setEthnicPolicyOption(m_playerCountryId, entries[ei].name,
                                              ci, (int)oi);
                        return;
                    }
                    rx = rx + nameW + 30;
                }
                rdy += 24;
            }
            dy += expandedH;
        }
    }
}
