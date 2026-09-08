#include "Game.h"
#include <cstdlib>
#include "util/LoadLog.h"
#include "Palette.h"
#include "PoliticalIdentity.h"
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

namespace {

// ─── A DOCTRINE'S GAINS AND COSTS, WITH THE NUMBER TAKEN OUT ───────
//
// data/policies.json states each effect as one finished English sentence --
// "Population growth +0.5%/turn", "-5/turn income" -- and the panel drew them
// as read. Three hundred and eight of them, in English, in the middle of the
// screen the player spends the most time on, in every language.
//
// Translating them one by one is the obvious answer and the wrong one: there
// are 170 distinct strings but only the twenty-five FORMS below, because the
// rest of the difference is the number. So the number comes out, the form is
// translated, and the number goes back where that language puts it -- which is
// not always where English puts it, and is the entire reason this is a pattern
// with a placeholder rather than a prefix and a suffix. "-5/turn income" is a
// number FIRST; "Unrest -1.0%" is a number LAST; both are one entry here.
//
// A form the data uses and this table does not list falls back to the raw
// string, so a new effect reads as English rather than as nothing.
// tools/check_policies.py fails the build for exactly that case, which is the
// only thing that keeps this list honest.
const char* kTradeoffForms[] = {
    "%s/turn income",
    "Army attack %s",
    "Army defence %s",
    "Army upkeep %s",
    "Economic %s",
    "Immigration %s",
    "Indoctrination %s",
    "Industry cost %s",
    "Industry upkeep %s",
    "Manpower %s",
    "Migration %s",
    "Minority growth %s/turn",
    "Pacification budget %s/turn",
    "Population growth %s/turn",
    "Population income %s",
    "Pulls provinces toward the government",
    "Recruitment cost %s",
    "Resource income %s",
    "Ship attack %s",
    "Ship cost %s",
    "Ship defence %s",
    "Ship speed %s",
    "Social %s",
    "Treasury %s/turn",
    "Unrest %s",
    "Upkeep %s",
    "War declarations %s/turn",
};

/// What a doctrine's compass position is CALLED, as opposed to what the JSON
/// calls it. The panel drew the id -- lowercase "authoritarian", straight out
/// of the file -- which reads as a leaked field name and, being a bare
/// lowercase word, is the one shape tools/i18n_extract.py refuses to collect,
/// on the grounds that it is usually an id. It usually is. So the id stays an
/// id and the label is a label, and these five already have translations from
/// the compass they name.
const char* categoryLabel(const std::string& id) {
    if (id == "left")          return T("Left");
    if (id == "right")         return T("Right");
    if (id == "authoritarian") return T("Authoritarian");
    if (id == "libertarian")   return T("Libertarian");
    if (id == "miscellaneous") return T("Miscellaneous");
    return T(id);
}

/// The first run of digits in `s`, with its sign and any trailing percent.
/// Returns [begin, end); begin == npos when there is no number.
std::pair<size_t, size_t> numberSpan(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        size_t j = i;
        if (s[j] == '+' || s[j] == '-') ++j;
        if (j >= s.size() || !isdigit((unsigned char)s[j])) continue;
        while (j < s.size() && isdigit((unsigned char)s[j])) ++j;
        if (j < s.size() && s[j] == '.') {
            ++j;
            while (j < s.size() && isdigit((unsigned char)s[j])) ++j;
        }
        if (j < s.size() && s[j] == '%') ++j;
        return {i, j};
    }
    return {std::string::npos, 0};
}

/// One effect line, in the player's language.
std::string tradeoffLine(const std::string& raw) {
    const auto [b, e] = numberSpan(raw);
    if (b == std::string::npos) return T(raw);

    std::string form = raw.substr(0, b) + "%s" + raw.substr(e);
    for (const char* known : kTradeoffForms) {
        if (form != known) continue;
        // TextFormat, not a hand-rolled splice: the translated form decides
        // where the number goes, and it may put it first.
        return TextFormat(T(form), raw.substr(b, e - b).c_str());
    }
    return T(raw);
}

}  // namespace

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
            
            // The continuous effects. See Policy::levers.
            if (p.contains("levers") && p["levers"].is_object())
                for (auto& [k, v] : p["levers"].items())
                    if (v.is_number()) policy.levers[k] = v.get<float>();

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
        LoadLog() << "  Loaded " << m_allPolicies.size() << " policies from JSON" << std::endl;
    } catch (const std::exception& e) {
        LoadLog() << "Failed to parse policies.json: " << e.what() << std::endl;
    }
    // The other list. Kept beside this one because a reader looking for "where
    // do the district laws come from" looks here first.
    loadDistrictLaws();

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
        // Medium's pop growth is 0, not the +0.5 it carried for a long time.
        // Every other option here states its numbers in its own description;
        // this one promises "no population changes", so the 0.5 was something
        // nobody was told about — and being the default, it was the game's only
        // population growth, applied per minority per province. Homogeneous
        // provinces never grew and a three-minority province grew three times
        // as fast as a one-minority neighbour, which is no world-population
        // rule anyone designed. If the world should grow, that wants a rule of
        // its own; the research tree has an unwired popGrowthPct hook for it.
        {"Medium",  "Status quo. No alignment or population changes.",
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true},
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
            m_countryCompass[cid] = makeCompass(c.compassEconomic, c.compassSocial);
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
                if (policiesConflict(pid, ap.policyId)) { incompatible = true; break; }
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
 
// A conflict between two doctrines is a fact about the pair, not about one of
// them, so it is answered here rather than at each of the three places that
// used to ask.
//
// Every one of them read only `incompatible_with` on the doctrine being
// adopted. Twelve of the shipped pairs name each other once -- Land Reform
// names Flat Tax, Flat Tax names nobody -- so the pair's behaviour depended on
// the order the player picked them in: Flat Tax first refused Land Reform,
// Land Reform first let Flat Tax through, and the country ended up holding
// both halves of a contradiction. The map editor already read the pair both
// ways, so a start position the editor refused to build was reachable in play.
//
// Reading it both ways here settles it for the player screen, the AI and mod
// scripts at once, and means a doctrine added later has to name its conflict
// only once -- on whichever side it reads better.
bool Game::policiesConflict(const std::string& a, const std::string& b) const {
    if (a == b) return false;
    for (const auto& p : m_allPolicies) {
        if (p.id != a && p.id != b) continue;
        const std::string& other = (p.id == a) ? b : a;
        for (const auto& inc : p.incompatibleWith)
            if (inc == other) return true;
    }
    return false;
}

std::vector<std::string> Game::conflictingPolicyNames(const Policy& p) const {
    std::vector<std::string> out;
    for (const auto& q : m_allPolicies)
        if (policiesConflict(p.id, q.id)) out.push_back(q.name);
    return out;
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
        // Same bound as every ingestion point, from the same constant, so the
        // range cannot drift apart in two places again.
        it->second = makeCompass(it->second.economic + econDelta,
                                 it->second.social   + socDelta);
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

// Population growth, as a rule of its own rather than a side effect of an
// ethnic policy nobody was told about.
//
// The tourism branch has described this since it was written -- "Population
// growth +5/10/20%" -- but nothing ever read popGrowthPct, so the branch bought
// a migration bonus and a dead stat, and the world's only growth came from the
// default deportation option. Research MODIFIES the rate here, it does not
// provide it: +5% means 1.05x the baseline, the same whole-percent convention
// conscriptionPct and armyAtkPct use at their call sites.
//
// Every province the country owns grows once per turn. The old behaviour ran
// inside the per-minority loop, so ethnically homogeneous provinces never grew
// at all and a three-minority province grew three times as fast as its
// neighbour -- the province, not its minority list, is what has people in it.
// What this province's ethnic policies do to its population, per turn, as a
// percentage.
//
// SHARE-WEIGHTED, NEVER SUMMED. A province's minorities partition it -- they add
// to 100% -- so a rate that applies to one of them applies to that share of the
// people, not to all of them. Summing across nine groups is how the same
// province grew nine times over; weighting by share makes the result a weighted
// average, bounded by the strongest single option rather than by how many
// distinct peoples happen to live there.
float Game::ethnicGrowthPctFor(int countryId, int provinceId) const {
    auto mit = m_provinceMinorities.find(provinceId);
    if (mit == m_provinceMinorities.end()) return 0.0f;
    float weighted = 0.0f;
    for (const auto& mg : mit->second) {
        float perGroup = 0.0f;
        for (size_t ci = 0; ci < m_ethnicPolicyCategories.size(); ++ci) {
            const int oi = ethnicPolicyOption(countryId, mg.name, ci);
            if (oi < 0 || oi >= (int)m_ethnicPolicyCategories[ci].options.size()) continue;
            perGroup += m_ethnicPolicyCategories[ci].options[oi].popGrowthPerTurn;
        }
        weighted += perGroup * (mg.pct * 0.01f);
    }
    return weighted;
}

// WHAT THE LAND WILL HOLD.
//
// The taper used to run toward MAX_PROVINCE_POP, a flat 1e10 for every province
// on every map. The largest province on the shipped 1939 map holds 74.7
// million, so the headroom term read 0.9925 and the taper did NOTHING across
// the entire range any real game occupies -- it only began to bite at 133 times
// the largest province that has ever existed in this game. A ceiling nothing
// can reach is not a ceiling.
//
// Ground is what limits people, so the ceiling is the province's own
// cos(latitude)-weighted area -- the same measure Phase 1's industry capacity
// reads, already computed at load, no new state. Sized so the world tends
// toward roughly four times its starting population over a long game: 2.1
// billion at load on the 1939 map against a ceiling near 8.5 billion, which is
// about what a century does and is a number a player can recognise.
//
// MAX_PROVINCE_POP stays as the absolute backstop, because the save format and
// SaveManager's uint32 packing depend on it.
long long Game::provinceCarryingCapacity(int pid) const {
    const double area = (double)provinceArea(pid);
    if (area <= 0.0) return MAX_PROVINCE_POP;   // unmeasured: do not bound it
    const double cap = area * (double)POP_PER_AREA;
    return (long long)std::min((double)MAX_PROVINCE_POP, std::max(1.0, cap));
}

void Game::growCountryPopulation(int countryId) {
    const float mod = 1.0f + getTotalEffect("popGrowthPct", countryId) / 100.0f;
    const float baseRate = BASE_POP_GROWTH_PCT * mod;

    for (int pid : provincesOf(countryId)) {
        auto popIt = m_provincePopulations.find(pid);
        if (popIt == m_provincePopulations.end()) continue;
        const long long pop = popIt->second;
        if (pop <= 0) continue;

        // Regional law, which is per PROVINCE and so cannot ride on the
        // country-wide modifier above: settlement grants pull people into one
        // district, martial law pushes them out of another, and both are
        // happening in the same country on the same turn.
        const float lawRate = baseRate *
            (1.0f + districtLawsAt(countryId, pid).growthPct / 100.0f);

        // Base growth plus whatever this province's ethnic policies add or take
        // away -- once, for the province, share-weighted. Clamped to the same
        // +/-50% a turn the old path used, so no combination of options can
        // produce a rate that is absurd on its face.
        const float pct = std::clamp(lawRate + ethnicGrowthPctFor(countryId, pid),
                                     -50.0f, 50.0f);
        if (pct == 0.0f) continue;
        float rate = pct / 100.0f;

        // Logistic taper toward what the ground will hold. Only on the way UP:
        // a province being emptied by a deportation policy is not helped along
        // by having room.
        const long long cap = provinceCarryingCapacity(pid);
        if (rate > 0.0f) {
            const double headroom = std::max(0.0, 1.0 - (double)pop / (double)cap);
            rate *= (float)headroom;
        }
        const long long growth = (long long)((double)pop * rate);
        popIt->second = std::clamp(pop + growth, 0LL, MAX_PROVINCE_POP);
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

            // The alignment half of the turn comes from minorityDriftPerTurn,
            // which is also what the Ethnic tab reports -- policy dial plus the
            // conquered-ground penalty. Only the population and compass effects
            // are still walked per category here, because those are per
            // province rather than per country.
            const float driftThisTurn = minorityDriftPerTurn(countryId, mg.name);
            float growthPctThisTurn = 0.0f; // summed over categories, applied once below
            for (size_t ci = 0; ci < m_ethnicPolicyCategories.size(); ci++) {
                const int oi = ethnicPolicyOption(countryId, mg.name, ci);
                if (oi < 0 || oi >= (int)m_ethnicPolicyCategories[ci].options.size()) continue;
                auto& opt = m_ethnicPolicyCategories[ci].options[oi];
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

            // POPULATION GROWTH USED TO BE APPLIED HERE, AND THAT WAS THE
            // WORLD'S RUNAWAY.
            //
            // The comment on the deportation categories above already records
            // this bug being found once: growth "applied per minority per
            // province", so "homogeneous provinces never grew and a
            // three-minority province grew three times as fast as a
            // one-minority neighbour". A rule of its own was written for it --
            // growCountryPopulation -- and this block was left standing, so the
            // fix and the bug shipped together.
            //
            // What that cost: `processed` is scoped to the COUNTRY, so the
            // first province holding each new minority name had growth applied
            // once per name -- a mean of 9.5 times over on the shipped map,
            // against neighbours that got nothing. Ethnic options reach +2.5%
            // a turn between them, and the world compounded at an effective
            // ~3.35% a turn: 2.1 billion people at load, 110 billion by turn
            // 120. Nothing noticed because every other reader of population is
            // logarithmic or fractional.
            //
            // The ethnic policies still move population -- that is a real
            // mechanic and a player lever. It is now share-weighted across a
            // province's minorities and applied ONCE for the province, in
            // growCountryPopulation, where the rest of the world's growth
            // lives. See ethnicGrowthPctFor().
            (void)growthPctThisTurn;

            // The conquered-ground war penalty is part of minorityDriftPerTurn
            // above, so it is not applied a second time here.
            if (m_config.aiDebug && processed.size() < 10 && driftThisTurn != 0.0f)
                printf("[DIAG] %s under country %d: %+.1f/turn\n",
                       mg.name.c_str(), countryId, driftThisTurn);
            addMinorityDrift(countryId, mg.name, driftThisTurn);
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
        growCountryPopulation(cid);
        // NOTE: artillery is deliberately NOT processed here. processCountryTurn()
        // already calls processArtilleryOrders() for every country earlier in the
        // turn; doing it again here made every bombardment apply its damage twice.
    }

    // After the loop above, not inside it: applyPolicyEffects() is what moves
    // the government compass, so every country's position is final for this
    // turn only once all of them have run.
    updatePoliticalIdentities();
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
                        unrest -= policyUnrestPct(p);   // see the declaration
                        break;
                    }
                }
            }
        }
    }
 
    return std::min(100.0f, std::max(0.0f, unrest));
}

float Game::getProvinceRebellionChance(int provinceId, int countryId) const {
    // A province that has just risen cannot rise again yet. Gated here rather
    // than in processRebellions so the number the player reads is the number
    // that governs: this IS the chance the province revolts this turn, and
    // during the cooldown it is genuinely zero. See REBELLION_COOLDOWN_TURNS.
    {
        auto cdIt = m_provinceRebellionCooldown.find(provinceId);
        if (cdIt != m_provinceRebellionCooldown.end() && cdIt->second > 0) return 0.0f;
    }

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

    // An empty treasury is felt everywhere at once. It does not decay like the
    // weariness term -- it is on while the country cannot pay for itself and off
    // the turn it can, so solvency is worth something immediately rather than
    // several turns later -- but it does RAMP over the first three bankrupt
    // turns. See BANKRUPT_UNREST_FULL_STREAK for the case that settled that.
    total += bankruptcyUnrestFor(countryId);

    // ── AND SO IS AN EMPTY SHELF ──
    //
    // The consumer good is the one the population actually eats, and how much
    // of what they wanted they got is the whole point of the production
    // economy: "the higher and cheaper these things are, the better the living
    // standards".
    //
    // SHAPED LIKE THE BANKRUPTCY TERM ABOVE, deliberately, because it is the
    // same kind of fact: a national condition, felt everywhere at once, on
    // while it is true and off the turn it stops being. It does not accumulate
    // and it does not decay, so a government that fixes the shortage sees the
    // unrest go the same turn -- which is what makes it something a player can
    // act on rather than a punishment they serve out.
    //
    // Nothing at all when the country is fed (ratio >= 1) or when the goods
    // economy is off, so this is exactly zero in every world that has not opted
    // in. Scaled by the SHORTFALL rather than being a step, so half-fed is half
    // the trouble of starving.
    if (m_goodsEconomy) {
        const float fed = livingStandards(countryId);
        if (fed < 1.0f) total += (1.0f - std::clamp(fed, 0.0f, 1.0f)) * SHORTAGE_UNREST_PCT;
    }

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
                if (p.id == ap.policyId) { total -= policyUnrestPct(p); break; }
        }
    }

    // ── AND THE REGIONAL LAW IN FORCE HERE ──
    //
    // Added rather than subtracted, because these go both ways: a curfew calms
    // the ground and a grain requisition does the opposite, and both are things
    // a provincial administration actually does. See DistrictLaw.
    total += districtLawsAt(countryId, provinceId).unrestPct;

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
    // ── AND WHERE THAT BUDGET IS ACTUALLY SPENT ──
    //
    // One for an undivided country and one for a district drawing its own share
    // of the ground, so this line is arithmetically the same as it was for
    // every save that predates districts and for every country that never
    // draws one. See District and Game::pacificationFactor.
    float suppressionPct = pac * 50.0f * pacificationFactor(countryId, provinceId);
    total -= suppressionPct;

    // Inherent civil order. Every functioning state commands baseline loyalty,
    // so a well-governed, homogeneous, ideologically-aligned province is STABLE
    // at zero pacification — rebellion is driven by real grievance (extreme
    // mismatch, hostile minorities, foreign claims, war) that exceeds this
    // floor. Without it, the tiny per-province baseline unrest summed across
    // dozens of provinces made early fragmentation certain for every country,
    // and pacification was a mandatory tax rather than a tool for hotspots.
    total -= REBELLION_LOYALTY_FLOOR;

    // OD_UNREST_TRACE=<cid>: the terms of this sum for one country's
    // provinces, once per turn (the resolver's call; the UI panels call this
    // per frame, and those are skipped by the turn check).
    {
        static const int traceCid = std::getenv("OD_UNREST_TRACE") ? atoi(std::getenv("OD_UNREST_TRACE")) : -1;
        static int lastTurn = -1, lastPid = -1;
        if (traceCid == countryId && !(lastTurn == m_turnNumber && lastPid == provinceId)) {
            lastTurn = m_turnNumber; lastPid = provinceId;
            fprintf(stderr, "[UNREST] turn %d cid=%d pid=%d base %.1f pol %.1f eth %.1f claim %.1f wear %.1f"
                    " broke %d pac -%.0f floor -%.0f => %.1f%%\n",
                    m_turnNumber, countryId, provinceId, base, polUnrest, ethUnrest, claimUnrest,
                    warWearinessOf(countryId), (int)m_bankruptCountries.count(countryId),
                    suppressionPct, REBELLION_LOYALTY_FLOOR, std::min(95.0f, std::max(0.0f, total)));
        }
    }

    return std::min(95.0f, std::max(0.0f, total));
}

float Game::getProvinceRebellionChance(int provinceId) const {
    return getProvinceRebellionChance(provinceId, m_playerCountryId);
}

// How a minority feels about the government it lives under — which is now a
// question about a specific government, not about the minority in the abstract.
// A group can be loyal in one country and in open revolt across the border,
// which is the point of letting each government set its own policy.
// === releasableRegions ===
//
// The adapter between the game's state and the pure rule in ReleaseRules.h.
// Everything decided here is a lookup; everything decided THERE is the rule.
//
// A province's "top minority" is the largest group living in it. Minorities
// partition a province to 100% with about nine and a half groups in the average
// one, so the largest is usually well short of a majority -- which is why the
// rule wants a majority and not a plurality, and why most provinces are not
// part of any releasable region at all.
std::vector<ReleaseCandidate> Game::releasableRegions(int countryId) const {
    std::vector<ReleaseProvince> owned;
    for (int pid : provincesOf(countryId)) {
        auto mit = m_provinceMinorities.find(pid);
        if (mit == m_provinceMinorities.end() || mit->second.empty()) continue;
        const MinorityGroup* top = nullptr;
        for (const auto& mg : mit->second)
            if (!top || mg.pct > top->pct) top = &mg;
        if (!top || top->name.empty()) continue;

        ReleaseProvince rp;
        rp.id = pid;
        rp.topMinority = top->name;
        rp.topMinorityPct = top->pct;
        rp.alignment = getMinorityAlignment(countryId, top->name);
        auto pop = m_provincePopulations.find(pid);
        rp.population = (pop != m_provincePopulations.end()) ? pop->second : 0;
        owned.push_back(std::move(rp));
    }
    // provincesOf is the ordered index, so `owned` arrives in a stable order and
    // the rule's own tie-breaks do the rest. See the determinism note there.
    return findReleasableRegions(owned, [this](int pid) {
        static const std::vector<int> none;
        auto it = m_provinceNeighbors.find(pid);
        return (it != m_provinceNeighbors.end()) ? it->second : none;
    });
}

// === drawPolicySearchBox / policyMatchesSearch ===
//
// ONE SEARCH, ON EVERY TAB THAT LISTS DOCTRINES.
//
// The box and the matcher lived inside the Available tab's branch, so a player
// with a dozen doctrines running could search the list they were choosing FROM
// and not either of the lists they already had. Asked for directly: "we should
// be able to search in both active and implementing policies."
//
// Extracted rather than copied. The matcher searches the description and the
// tradeoff lines as well as the name, which is the whole reason it is worth
// having -- a player types "upkeep" or "navy", and few doctrines are named
// after what they do -- and three copies of that would drift.
void Game::drawPolicySearchBox(Vector2 mouse, int startY) {
    const int boxW = 260, boxH = 24;
    Rectangle box = {(float)(m_screenW - 270 - boxW + 20), (float)(startY - 28),
                     (float)boxW, (float)boxH};
    const bool hov = CheckCollisionPointRec(mouse, box);
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_policySearchFocus = hov;
    DrawRectangleRec(box, m_policySearchFocus ? Color{30, 30, 42, 230}
                                              : Color{24, 24, 32, 200});
    DrawRectangleLinesEx(box, 1, m_policySearchFocus ? hexToColor(m_config.accent())
                                                     : Color{90, 90, 110, 180});
    if (m_policySearch.empty() && !m_policySearchFocus) {
        DrawText(T("Search doctrines..."), (int)box.x + 8, (int)box.y + 5, 13,
                 Color{120, 120, 140, 200});
    } else {
        DrawText(m_policySearch.c_str(), (int)box.x + 8, (int)box.y + 5, 13, WHITE);
        if (m_policySearchFocus && ((int)(GetTime() * 2.0) % 2) == 0)
            DrawText("_", (int)box.x + 10 + MeasureText(m_policySearch.c_str(), 13),
                     (int)box.y + 5, 13, WHITE);
    }
    if (!m_policySearch.empty()) {
        Rectangle clr = {box.x + boxW - 20, box.y + 4, 16, 16};
        const bool ch = CheckCollisionPointRec(mouse, clr);
        DrawText("x", (int)clr.x + 5, (int)clr.y, 14,
                 ch ? WHITE : Color{160, 160, 180, 220});
        if (ch && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_policySearch.clear();
            m_policyScroll = 0;
        }
    }
    if (m_policySearchFocus) {
        int ch;
        while ((ch = GetCharPressed()) != 0)
            if (ch >= 32 && ch < 127 && m_policySearch.size() < 40) {
                m_policySearch += (char)ch;
                m_policyScroll = 0;
            }
        if (IsKeyPressed(KEY_BACKSPACE) && !m_policySearch.empty()) {
            m_policySearch.pop_back();
            m_policyScroll = 0;
        }
    }
}

bool Game::policyMatchesSearch(const Policy& pol) const {
    if (m_policySearch.empty()) return true;
    std::string needle = m_policySearch;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    auto has = [&](const std::string& hay) {
        std::string h = hay;
        std::transform(h.begin(), h.end(), h.begin(),
                       [](unsigned char c) { return (char)tolower(c); });
        return h.find(needle) != std::string::npos;
    };
    if (has(pol.name) || has(pol.description) || has(pol.folder)) return true;
    for (const auto& g : pol.tradeoffs.gains) if (has(g)) return true;
    for (const auto& cst : pol.tradeoffs.costs) if (has(cst)) return true;
    return false;
}

float Game::getMinorityAlignment(int countryId, const std::string& minorityName) const {
    float align = 50.0f;
    auto cIt = m_minorityAlignmentDrift.find(countryId);
    if (cIt != m_minorityAlignmentDrift.end()) {
        auto dit = cIt->second.find(minorityName);
        if (dit != cIt->second.end()) align += dit->second;
    }
    return std::max(0.0f, std::min(100.0f, align));
}

// WHY THE BOUND IS ON THE STORED VALUE AND NOT ON THE READER.
//
// Alignment is 50 + drift clamped to 0..100, so any drift past +/-50 is
// invisible. It used to be stored anyway, and that is what made repression
// permanent: the most repressive option set is -14.5/turn, so twenty turns of
// it left -290 in a field the bar could only ever show as 0%. Switching back
// to the default set, worth +3/turn, then bought nothing for the next
// ninety-seven turns. The player reverted the policy, watched a positive
// trend, and saw the bar sit at 0% for the rest of the game -- which is the
// bug as reported: the drift does not go away once it is put.
//
// The events are worse than the policies. declareWar's kin penalty was -30 per
// PROVINCE of the attacker holding that minority, so one declaration against a
// neighbour could bury a widespread group at -1500, and conquest adds -25 for
// every province taken. None of that could be worked off inside a game.
//
// Clamping here means the stored number is always the number on the bar, so
// undoing a policy takes as long as setting it did and no longer.
void Game::addMinorityDrift(int countryId, const std::string& minorityName, float delta) {
    if (delta == 0.0f) return;
    float& d = m_minorityAlignmentDrift[countryId][minorityName];
    d = std::clamp(d + delta, -MINORITY_DRIFT_LIMIT, MINORITY_DRIFT_LIMIT);
}

float Game::getMinorityPolicyRate(int countryId, const std::string& minorityName) const {
    float rate = 0.0f;
    for (size_t ci = 0; ci < m_ethnicPolicyCategories.size(); ci++) {
        const int oi = ethnicPolicyOption(countryId, minorityName, ci);
        if (oi >= 0 && oi < (int)m_ethnicPolicyCategories[ci].options.size())
            rate += m_ethnicPolicyCategories[ci].options[oi].alignmentPerTurn;
    }
    return rate;
}

// The whole per-turn rule, in one place because it has two readers.
//
// applyEthnicPolicyEffects applies it and the Ethnic tab reports it, and when
// those were separate the tab reported the policy dial only -- so a minority in
// ground you had just taken off someone you are still fighting showed
// "Trend: +3.0%/t" while it was in fact drifting -2 a turn. A rule written
// twice is a rule that disagrees with itself.
float Game::minorityDriftPerTurn(int countryId, const std::string& minorityName) const {
    float rate = getMinorityPolicyRate(countryId, minorityName);

    // Holding conquered ground against an enemy you are still at war with.
    // Once per minority, not once per province: the resolver's `processed` set
    // already had that effect, but it landed on whichever province the map
    // happened to iterate first, so the same position could score differently
    // between two runs. "Any such province" is the same magnitude, decided.
    const Country* cur = m_countries.getCountry(countryId);
    if (!cur) return rate;
    auto ar = m_relations.find(cur->isoA3);
    if (ar == m_relations.end()) return rate;

    for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
        if (prov.countryId != countryId) continue;
        if (!m_provinceConquestTurn.count(pid)) continue;
        auto prevIt = m_conqueredProvincePrevOwner.find(pid);
        if (prevIt == m_conqueredProvincePrevOwner.end() || prevIt->second <= 0) continue;
        auto mit = m_provinceMinorities.find(pid);
        if (mit == m_provinceMinorities.end()) continue;
        bool here = false;
        for (auto& mg : mit->second) if (mg.name == minorityName) { here = true; break; }
        if (!here) continue;
        const Country* prev = m_countries.getCountry(prevIt->second);
        if (!prev) continue;
        auto dr = ar->second.find(prev->isoA3);
        if (dr != ar->second.end() && dr->second.war) return rate - 5.0f;
    }
    return rate;
}

float Game::getMinorityAlignmentTrend(int countryId, const std::string& minorityName) const {
    const float rate = minorityDriftPerTurn(countryId, minorityName);
    // What the bar will actually do. A group already pinned at 0 or 100 is not
    // moving, and reporting the dial's number there is the other half of the
    // misleading trend: it read "+3.0%/t" against a percentage that had not
    // changed in fifty turns.
    float drift = 0.0f;
    auto cIt = m_minorityAlignmentDrift.find(countryId);
    if (cIt != m_minorityAlignmentDrift.end()) {
        auto dit = cIt->second.find(minorityName);
        if (dit != cIt->second.end()) drift = dit->second;
    }
    const float next = std::clamp(drift + rate, -MINORITY_DRIFT_LIMIT, MINORITY_DRIFT_LIMIT);
    return next - drift;
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
        DrawText(TextFormat(T("%s - Doctrines"), od::i18n::properName(c->name).c_str()), centerX - 150, 30, 28, WHITE);
    }

    // Tabs
    const char* tabs[] = {"Available", "Implementing", "Active", "Analysis", "Ethnic",
                          "Districts"};
    int nTabs = 6;
    Vector2 mouse = getMouse();

    // MEASURED, NOT A FIXED PITCH. Five tabs at 130 px already crowded the
    // longer languages; a sixth on the same arithmetic would have run them into
    // each other, which is the fault the claims screen was just fixed for.
    int tabW[6] = {0}, tabsTotal = 0;
    const int TAB_GAP = 26;
    for (int t = 0; t < nTabs; ++t) { tabW[t] = MeasureText(tabs[t], 20); tabsTotal += tabW[t]; }
    tabsTotal += TAB_GAP * (nTabs - 1);
    int tabPen = centerX - tabsTotal / 2;

    for (int t = 0; t < nTabs; ++t) {
        int tw = tabW[t];
        int tx = tabPen + tw / 2;
        tabPen += tw + TAB_GAP;
        bool active = (t == m_policyTab);
        Color tc = active ? hexToColor(m_config.accent()) : LIGHTGRAY;
        Rectangle tr = {(float)(tx - tw/2 - 10), (float)(tabY - 5), (float)(tw + 20), 30};
        DrawText(tabs[t], tx - tw/2, tabY, 20, tc);
        if (active) {
            DrawRectangle(tx - tw/2, tabY + 24, tw, 3, hexToColor(m_config.accent()));
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
            implementingSummary += od::i18n::tr(p->name);
            implementingCount++;
        } else if (ap.turnsRemaining == 0 || ap.turnsRemaining < -1) {
            if (activeCount > 0) activeSummary += ", ";
            activeSummary += od::i18n::tr(p->name);
            activeCount++;
        }
    }
 
    int startY = tabY + 50;
    int listH = m_screenH - startY - 30;
 
    // Permanent status bar: active/implementing summary + per-turn actions
    {
        std::string statusText;
        if (activeCount > 0)
            statusText += TextFormat(T("Active: %s"), activeSummary.c_str());
        if (implementingCount > 0) {
            if (!statusText.empty()) statusText += "  |  ";
            statusText += TextFormat(T("Implementing: %s"), implementingSummary.c_str());
        }
        if (!statusText.empty()) statusText += "  |  ";
        int remaining = 3 - m_policiesEnactedThisTurn;
        statusText += TextFormat(T("Political actions: %d/3 remaining"), remaining);
        int fSize = 13;
        int w = MeasureText(statusText.c_str(), fSize);
        int h = 18;
        int sx = std::max(10, centerX - w / 2 - 8);
        int sy = tabY + 28;
        // Dark background for readability
        DrawRectangle(sx, sy, w + 16, h, {0, 0, 0, 160});
        DrawText(statusText.c_str(), sx + 8, sy + 2, fSize,
            remaining > 0 ? WHITE : odPalette::of(odPalette::Role::Bad));
    }
 
    if (m_policyTab == 0) {
        // Available policies with folder grouping
        DrawText(T("Available Doctrines"), 30, startY - 25, 20, WHITE);

        // ── Search ──
        // Drawn here rather than in updatePoliciesTab because the filter it
        // drives has to be applied before the folders are collected below.
        // Shared with the Implementing and Active tabs; see drawPolicySearchBox.
        drawPolicySearchBox(mouse, startY);

        auto matches = [&](const Policy& pol) { return policyMatchesSearch(pol); };

        // Collect unique folders in display order
        std::vector<std::string> folderOrder = {"Left", "Right", "Authoritarian", "Libertarian", "Miscellaneous"};
        std::unordered_map<std::string, std::vector<int>> folderPolicies;
        for (size_t i = 0; i < m_allPolicies.size(); ++i) {
            if (!matches(m_allPolicies[i])) continue;
            std::string f = m_allPolicies[i].folder;
            if (f.empty()) f = "Miscellaneous";
            folderPolicies[f].push_back((int)i);
        }
        // A search opens every folder it found something in: leaving a match
        // hidden behind a collapsed header is the same as not finding it.
        if (!m_policySearch.empty())
            for (auto& [fn, v] : folderPolicies) m_openFolders.insert(fn);

        // Compute total height: folder headers (28 each) + policy rows (150 each) for open folders
        int folderHeaderH = 28;
        // Tall enough for four gain/cost lines plus the reason beneath them.
        // At 150 the columns ran under the conflict line and the two overlapped
        // -- visible in the doctrine screen as green text crossed out by amber.
        int policyItemH = 182;
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

        if (folderPolicies.empty()) {
            DrawText(TextFormat(T("No doctrine matches \"%s\""), m_policySearch.c_str()),
                     30, startY + 20, 16, Color{150, 150, 170, 220});
        }
        BeginScissorMode(20, startY, m_screenW - 40, listH);
        int y = startY - m_policyScroll;
        for (auto& fname : folderOrder) {
            auto fit = folderPolicies.find(fname);
            if (fit == folderPolicies.end()) continue;

            // Folder header
            bool isOpen = m_openFolders.count(fname);
            Rectangle fhRect = {20, (float)y, (float)(m_screenW - 270), (float)folderHeaderH};
            DrawRectangleRec(fhRect, {50, 50, 60, 180});
            // The folder name is the compass word the doctrine sits under, and
            // those five already have translations.
            DrawText(TextFormat("%s %s", isOpen ? "▼" : "▶",
                                od::i18n::tr(fname)), 30, y + 4, 18,
                     hexToColor(m_config.accent()));
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
                Rectangle row = {20, (float)y, (float)(m_screenW - 270), 172};
                DrawRectangleRounded(row, 0.1f, 6, bgCol);
                DrawRectangleRoundedLines(row, 0.1f, 6, canEnact ? Color{100, 150, 100, 150} : Color{80, 80, 100, 100});

                // Name and blurb come from data/policies.json and are drawn
                // as they are read; T() is what puts them through the table.
                DrawText(T(p.name), 30, y + 4, 20, nameCol);
                DrawText(T(p.description), 30, y + 28, 13, LIGHTGRAY);

                // Category badge
                Color catCol = GRAY;
                if (p.category == "left") catCol = {100, 200, 100, 255};
                else if (p.category == "right") catCol = {255, 200, 100, 255};
                else if (p.category == "authoritarian") catCol = {200, 100, 100, 255};
                else if (p.category == "libertarian") catCol = {100, 150, 255, 255};
                else catCol = {180, 180, 180, 255};
                DrawText(categoryLabel(p.category), 30, y + 50, 11, catCol);

                // Duration info for propaganda
                if (p.propagandaDuration > 0) {
                    DrawText(TextFormat(T("Campaign: %d turns"), p.propagandaDuration), 130, y + 50, 11, Color{200, 150, 255, 200});
                }

                // Info: cost, turns, compass shift
                std::string info = TextFormat(T("Cost: %d/turn | Setup: %d turn(s) | Shift: E%.0f S%.0f"),
                    p.costPerTurn, p.implementationTurns, p.econShift, p.socShift);
                DrawText(info.c_str(), 30, y + 66, 12, Color{150, 150, 170, 200});

                // ── WHAT IT GIVES AND WHAT IT TAKES, in two columns ──
                //
                // These were two single lines, each laid out left to right with
                // no width limit: a doctrine with four gains ran off the panel
                // and under the Enact button, and the reader had to parse "+ A
                // + B + C" as a list because nothing separated the items. They
                // are lists, so they are drawn as lists -- gains down the left,
                // costs down the right, one per line, headed, so the two can be
                // compared by eye instead of by reading a sentence.
                //
                // Columns are sized off the row rather than fixed, because the
                // row is m_screenW-270 wide and a hardcoded split puts the
                // costs off-screen on a narrow window.
                {
                    const int colGap = 18;
                    const int colW = ((int)row.width - 40 - colGap) / 2;
                    const int gx = 30, cx2 = 30 + colW + colGap;
                    const int headY = y + 82, listY = y + 96;
                    const int lineH = 13;
                    // At most four each: the fifth line would collide with the
                    // row below, and a doctrine that needs five is telling the
                    // player too much at browse time anyway. The count says
                    // what was left out rather than silently truncating.
                    const size_t MAXL = 4;

                    DrawText(T("GAINS"), gx, headY, 10, Color{120, 220, 120, 220});
                    for (size_t g = 0; g < p.tradeoffs.gains.size() && g < MAXL; ++g)
                        DrawText(TextFormat("+ %s", tradeoffLine(p.tradeoffs.gains[g]).c_str()),
                                 gx, listY + (int)g * lineH, 12, Color{140, 245, 140, 235});
                    if (p.tradeoffs.gains.size() > MAXL)
                        DrawText(TextFormat(T("+%zu more"), p.tradeoffs.gains.size() - MAXL),
                                 gx, listY + (int)MAXL * lineH, 11, Color{120, 200, 120, 180});

                    DrawText(T("COSTS"), cx2, headY, 10, Color{230, 130, 130, 220});
                    for (size_t g = 0; g < p.tradeoffs.costs.size() && g < MAXL; ++g)
                        DrawText(TextFormat("- %s", tradeoffLine(p.tradeoffs.costs[g]).c_str()),
                                 cx2, listY + (int)g * lineH, 12, Color{255, 150, 150, 235});
                    if (p.tradeoffs.costs.size() > MAXL)
                        DrawText(TextFormat(T("+%zu more"), p.tradeoffs.costs.size() - MAXL),
                                 cx2, listY + (int)MAXL * lineH, 11, Color{215, 130, 130, 180});
                }

                // THE REASON, when there is one -- not a list of doctrines that
                // merely could conflict.
                //
                // This line used to read "X Conflicts with: <every
                // incompatibility this doctrine declares>" whenever the doctrine
                // declared any, in force or not, and nothing else was ever
                // written. So a doctrine greyed out for want of income accused
                // three doctrines the player had never enacted. One went looking
                // in the Active tab, found none of them, and reported the game
                // as blocking him for no reason.
                //
                // Blocked: say which of the reasons it actually is. Available:
                // the incompatibilities are still worth knowing, but as
                // information about the future rather than an accusation.
                const std::string why = policyBlockReason(m_playerCountryId, p);
                if (!why.empty()) {
                    DrawText(why.c_str(), 30, y + 152, 11, Color{255, 130, 130, 255});
                } else if (const auto conflicts = conflictingPolicyNames(p); !conflicts.empty()) {
                    // Both directions, so a doctrine that is only named by its
                    // opposite still warns the player it cannot be combined.
                    std::string names;
                    for (size_t ic = 0; ic < conflicts.size(); ++ic) {
                        if (ic > 0) names += ", ";
                        names += od::i18n::tr(conflicts[ic]);
                    }
                    // The list is built first and formatted once: a sentence
                    // glued together from a translated prefix and a raw tail
                    // puts the colon where English puts it in every language.
                    const std::string inc =
                        TextFormat(T("Cannot be combined with: %s"), names.c_str());
                    DrawText(inc.c_str(), 30, y + 152, 11, Color{200, 180, 120, 200});
                }

                // Enact button
                bool enactLimitReached = (m_policiesEnactedThisTurn >= 3);
                Rectangle enactBtn = {(float)(m_screenW - 160), (float)(y + 12), 130, 44};
                if (canEnact && !enactLimitReached) {
                    bool hover = CheckCollisionPointRec(mouse, enactBtn);
                    DrawRectangleRounded(enactBtn, 0.2f, 6, hover ? Color{100, 180, 100, 255} : Color{80, 150, 80, 255});
                    DrawText(T("Enact"), (int)(enactBtn.x + enactBtn.width/2 - MeasureText(T("Enact"), 18)/2), y + 22, 18, WHITE);
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
        DrawText(T("Implementing Doctrines"), 30, startY - 25, 20, WHITE);
        drawPolicySearchBox(mouse, startY);
        int y = startY;
        bool any = false;
        for (size_t i = 0; i < m_activePolicies.size(); ++i) {
            const auto& ap = m_activePolicies[i];
            if (ap.countryId != m_playerCountryId || ap.turnsRemaining <= 0) continue;
            const Policy* p = nullptr;
            for (const auto& pol : m_allPolicies) if (pol.id == ap.policyId) { p = &pol; break; }
            if (!p) continue;
            // Filtered AFTER the doctrine is resolved, because the search reads
            // its name and text -- and `any` is set only by rows that survive,
            // so a search matching nothing says so instead of drawing a blank.
            if (!policyMatchesSearch(*p)) continue;
            any = true;
 
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

            DrawText(TextFormat(T("%s (Implementing: %d turns left)"), p->name.c_str(), ap.turnsRemaining), 30, y + 4, 18, ORANGE);
            DrawText(T(p->description), 30, y + 26, 13, LIGHTGRAY);
            DrawText(TextFormat(T("Compass shift per turn: E%.1f S%.1f"), p->econShift / p->implementationTurns, p->socShift / p->implementationTurns),
                30, y + 48, 12, Color{150, 150, 170, 200});
 
            // Cancel button
            Rectangle cancelBtn = {(float)(m_screenW - 160), (float)(y + 20), 130, 44};
            bool hover = CheckCollisionPointRec(mouse, cancelBtn);
            DrawRectangleRounded(cancelBtn, 0.2f, 6, hover ? Color{180, 80, 80, 255} : Color{150, 60, 60, 255});
            DrawText(T("Cancel"), (int)(cancelBtn.x + cancelBtn.width/2 - MeasureText(T("Cancel"), 18)/2), y + 30, 18, WHITE);
            if (hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                Audio::get().playSfx("back");
                cancelPolicy((int)i);
            }
 
            y += 100;
        }
        if (!any) {
            // "None implementing" and "none MATCHED" are different facts, and a
            // player who has just typed a search needs to be told which.
            DrawText(m_policySearch.empty()
                         ? T("No policies currently implementing")
                         : TextFormat(T("No implementing doctrine matches \"%s\""),
                                      m_policySearch.c_str()),
                     30, startY + 20, 16, Color{120, 120, 140, 200});
        }
    } else if (m_policyTab == 2) {
        // Active policies (permanent + propaganda campaigns)
        DrawText(T("Active Doctrines"), 30, startY - 25, 20, WHITE);
        drawPolicySearchBox(mouse, startY);
        int y = startY;
        bool any = false;
        for (size_t i = 0; i < m_activePolicies.size(); ++i) {
            const auto& ap = m_activePolicies[i];
            if (ap.countryId != m_playerCountryId) continue;
            // Active if: permanent (turnsRemaining == 0) OR propaganda (-1 < turnsRemaining < 0)
            if (ap.turnsRemaining != 0 && (ap.turnsRemaining >= -1 || ap.turnsRemaining > 0)) continue;
            const Policy* p = nullptr;
            for (const auto& pol : m_allPolicies) if (pol.id == ap.policyId) { p = &pol; break; }
            if (!p) continue;
            if (!policyMatchesSearch(*p)) continue;
            any = true;
 
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
                nameStr += TextFormat(T(" (%d turn(s) left)"), remaining);
            }
            DrawText(nameStr.c_str(), 30, y + 4, 18, isPropaganda ? Color{200, 150, 255, 255} : GREEN);
            DrawText(T(p->description), 30, y + 26, 13, LIGHTGRAY);
            DrawText(TextFormat(T("Cost: %d/turn | Shift/turn: E%.2f S%.2f | Unrest reduction: %.2f%%"),
                p->costPerTurn, p->econShift/50.0f, p->socShift/50.0f, p->effect.unrestReduction*100),
                30, y + 48, 12, Color{150, 180, 150, 200});
 
            // Remove button
            Rectangle removeBtn = {(float)(m_screenW - 160), (float)(y + 20), 130, 44};
            bool hover = CheckCollisionPointRec(mouse, removeBtn);
            DrawRectangleRounded(removeBtn, 0.2f, 6, hover ? Color{100, 100, 180, 255} : Color{80, 80, 150, 255});
            DrawText(T("Repeal"), (int)(removeBtn.x + removeBtn.width/2 - MeasureText(T("Repeal"), 18)/2), y + 30, 18, WHITE);
            if (hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                Audio::get().playSfx("toggle_off");
                cancelPolicy((int)i);  // Mark as completed/removed
            }
 
            y += 100;
        }
        if (!any) {
            DrawText(m_policySearch.empty()
                         ? T("No active policies")
                         : TextFormat(T("No active doctrine matches \"%s\""),
                                      m_policySearch.c_str()),
                     30, startY + 20, 16, Color{120, 120, 140, 200});
        }
    } else if (m_policyTab == 3) {
        drawAnalysisTab();
    } else if (m_policyTab == 4) {
        drawEthnicTab();
    } else if (m_policyTab == 5) {
        drawDistrictsTab();
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
                        if (f == fname) totalH += 182;   // must match policyItemH
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
    DrawText(T("LEFT"), x + 4, cy - 10, 10, {180, 180, 180, 200});
    DrawText(T("RIGHT"), x + size - 40, cy - 10, 10, {180, 180, 180, 200});
    DrawText(T("AUTH"), cx - 18, y + 4, 10, {180, 180, 180, 200});
    DrawText(T("LIB"), cx - 14, y + size - 16, 10, {180, 180, 180, 200});

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

    DrawText(TextFormat(T("%s — Analysis"), od::i18n::properName(c->name).c_str()), 30, 30, 28, WHITE);
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
    Color unrestCol = unrest < 20 ? odPalette::of(odPalette::Role::Good) : (unrest < 40 ? odPalette::of(odPalette::Role::Warning) : odPalette::of(odPalette::Role::Bad));
    DrawRectangle(bx, by, (int)(std::min(unrest, 100.0f) / 100.0f * barW), barH, unrestCol);
    DrawRectangleLines(bx, by, barW, barH, {100, 100, 120, 255});
    DrawText(TextFormat(T("Unrest: %.1f%%"), unrest), bx, by - 20, 14, unrestCol);

    // ── AND WHAT THE WARS ARE COSTING AT HOME ──
    //
    // War weariness is a term in every province's rebellion chance and had no
    // display anywhere in the game: warWearinessOf had exactly one caller, the
    // resolver. So a country could be sliding toward revolt because of a war it
    // answered ten turns ago, with the unrest bar rising and nothing on screen
    // naming the cause. The AI has always been able to read it. Shown only when
    // there is some, so a country at peace gains a line that says zero.
    {
        const float wear = warWearinessOf(m_playerCountryId);
        if (wear > 0.05f) {
            const Color wc = wear < 2.0f ? odPalette::of(odPalette::Role::Warning)
                                         : odPalette::of(odPalette::Role::Bad);
            DrawText(TextFormat(T("of which war weariness: %.1f"), wear),
                     bx, by + barH + 4, 12, wc);
        }
    }

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
    DrawText(T("Political Hotspots"), leftX, hotTitleY, 20, ORANGE);

    int hotRowH = 22, hotHeaderH = hotRowH + 2;
    int hotTotal = (int)hotspots.size() * hotRowH + hotHeaderH;
    int hotMaxScroll = std::max(0, hotTotal - hotAreaH);

    int btnW = 44;
    int cProv = 10, cReb = 140, cType = 200, cGo = panelW - btnW - 4;

    BeginScissorMode(leftX, hotStartY, panelW, hotAreaH);
    {
        int dy = hotStartY - m_analysisHotspotScroll;
        if (hotspots.empty()) {
            DrawText(T("No significant hotspots detected"), leftX + 10, dy, 14, Color{120, 140, 120, 200});
        } else {
            DrawText(T("Province"), leftX + cProv, dy, 11, LIGHTGRAY);
            DrawText(T("Rebel%"), leftX + cReb, dy, 11, LIGHTGRAY);
            DrawText(T("Type"), leftX + cType, dy, 11, LIGHTGRAY);
            dy += hotRowH;

            for (auto& hs : hotspots) {
                std::string typeStr;
                Color typeCol;
                if (hs.isEthnic && hs.isPolitical)     { typeStr = T("Eth+Pol"); typeCol = Color{200,100,255,255}; }
                else if (hs.isEthnic)                    { typeStr = "Ethnic";  typeCol = Color{100,200,255,255}; }
                else if (hs.isPolitical)                 { typeStr = T("Pol");     typeCol = Color{255,200,100,255}; }
                else                                     { typeStr = T("Econ");    typeCol = Color{180,180,180,255}; }

                Color rc = hs.rebelChance > 30 ? odPalette::of(odPalette::Role::Bad) : (hs.rebelChance > 15 ? odPalette::of(odPalette::Role::Warning) : Color{220, 220, 100, 255});
                DrawText(hs.name.c_str(), leftX + cProv, dy, 12, WHITE);
                DrawText(TextFormat("%.0f%%", hs.rebelChance), leftX + cReb, dy, 12, rc);
                DrawText(typeStr.c_str(), leftX + cType, dy, 12, typeCol);

                Rectangle goRect = {(float)(leftX + cGo), (float)(dy), (float)btnW, (float)(hotRowH - 2)};
                DrawRectangleRec(goRect, Color{60, 70, 90, 200});
                int goW = MeasureText(T("Go"), 12);
                DrawText(T("Go"), (int)(goRect.x + (btnW - goW) / 2), (int)(goRect.y + 3), 12, WHITE);
                m_analysisGoToButtons.push_back({hs.pid, goRect});

                dy += hotRowH;
            }
        }
    }
    EndScissorMode();

    // ── Minority Analysis ──
    DrawText(T("Minority Analysis"), leftX, minTitleY, 20, Color{100, 200, 255, 255});

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
            DrawText(T("No minority data available"), leftX + 10, dy, 14, Color{120, 120, 140, 200});
        } else {
            DrawText(T("Minority"), leftX + mcN, dy, 11, LIGHTGRAY);
            DrawText(T("Population"), leftX + mcP, dy, 11, LIGHTGRAY);
            DrawText(T("Alignment"), leftX + mcA, dy, 11, LIGHTGRAY);
            DrawText(T("Verdict"), leftX + mcV, dy, 11, LIGHTGRAY);
            dy += minRowH;

            for (auto& ma : minorities) {
                // Line 1: name + population + alignment + verdict
                DrawRectangle(leftX + mcN - 10, dy + 3, 8, 8, ma.color);
                // AN ETHNONYM IS A PROPER NOUN, so it goes through
                // properName like a country's name -- which means the ones
                // with a settled exonym come from <code>.names.json and the
                // long tail is transcribed phonetically rather than left in
                // English. See docs/i18n.md.
                DrawText(od::i18n::properName(ma.name).c_str(),
                         leftX + mcN, dy + 1, 13, WHITE);
                char ps[32];
                if (ma.pop > 1000000) snprintf(ps, sizeof(ps), "%.1fM", ma.pop / 1000000.0f);
                else if (ma.pop > 1000) snprintf(ps, sizeof(ps), "%.1fK", ma.pop / 1000.0f);
                else snprintf(ps, sizeof(ps), "%lld", ma.pop);
                DrawText(ps, leftX + mcP, dy + 1, 13, LIGHTGRAY);

                float align = getMinorityAlignment(m_playerCountryId, ma.name);
                float trend = getMinorityAlignmentTrend(m_playerCountryId, ma.name);
                float drift = align - 50.0f;   // the same number, relative to neutral

                Color ac = align < 30 ? odPalette::of(odPalette::Role::Bad) : (align < 60 ? odPalette::of(odPalette::Role::Warning) : odPalette::of(odPalette::Role::Good));
                DrawText(TextFormat("%.0f%%", align), leftX + mcA, dy + 1, 13, ac);

                const char* vd;
                Color vc;
                if (align < 25)      { vd = "Hostile";  vc = odPalette::of(odPalette::Role::Bad); }
                else if (align < 50) { vd = "Unhappy";  vc = ORANGE; }
                else if (align < 75) { vd = "Neutral";  vc = LIGHTGRAY; }
                else                 { vd = "Loyal";    vc = odPalette::of(odPalette::Role::Good); }
                DrawText(vd, leftX + mcV, dy + 1, 13, vc);

                // Line 2: modifiers (indented, smaller font)
                int modX = leftX + mcN + 10;
                if (drift < -0.01f) {
                    DrawText(TextFormat(T("Drift: %.0f"), drift), modX, dy + 18, 10, Color{255, 140, 140, 200});
                    modX += MeasureText(TextFormat(T("Drift: %.0f"), drift), 10) + 16;
                } else if (drift > 0.01f) {
                    DrawText(TextFormat(T("Drift: +%.0f"), drift), modX, dy + 18, 10, Color{140, 255, 140, 200});
                    modX += MeasureText(TextFormat(T("Drift: +%.0f"), drift), 10) + 16;
                }
                if (trend > 0.01f) {
                    DrawText(TextFormat(T("Trend: +%.1f%%/t"), trend), modX, dy + 18, 10, Color{100, 255, 100, 200});
                    modX += MeasureText(TextFormat(T("Trend: +%.1f%%/t"), trend), 10) + 16;
                } else if (trend < -0.01f) {
                    DrawText(TextFormat(T("Trend: %.1f%%/t"), trend), modX, dy + 18, 10, Color{255, 100, 100, 200});
                    modX += MeasureText(TextFormat(T("Trend: %.1f%%/t"), trend), 10) + 16;
                }
                // The standing war penalty, taken from the rule that applies it
                // rather than recomputed here.
                //
                // This line used to read "-30 War with X" for every war the
                // player was in against a country with kin. Two things were
                // wrong with that. The -30 is a one-off charged to the
                // AGGRESSOR when war is declared, so a country that was
                // attacked saw a penalty it had never paid; and being a single
                // past event, it is already inside the alignment number above,
                // so showing it again read as an ongoing drain that did not
                // exist. What IS ongoing is the conquered-ground penalty, and
                // the honest way to display it is the difference the rule makes.
                {
                    const float standing = minorityDriftPerTurn(m_playerCountryId, ma.name)
                                         - getMinorityPolicyRate(m_playerCountryId, ma.name);
                    if (standing < -0.01f) {
                        const char* s = TextFormat(T("%.0f/t occupied ground"), standing);
                        DrawText(s, modX, dy + 18, 10, odPalette::of(odPalette::Role::Bad));
                        modX += MeasureText(s, 10) + 16;
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
        DrawText(T("Pacification Budget:"), slX, slY - 20, 13, WHITE);
        DrawRectangle(slX, slY, slW, slH, {40, 40, 50, 200});
        int maxFill = (int)(slW * maxAllocFrac);
        if (maxFill > 0) DrawRectangle(slX, slY, maxFill, slH, {40, 50, 60, 150});
        DrawRectangleLines(slX, slY, slW, slH, {80, 80, 100, 200});
        int fillPac = (int)(slW * m_pacificationAllocation);
        if (fillPac > 0) DrawRectangle(slX, slY, fillPac, slH, {80, 180, 220, 200});
        DrawText(TextFormat("%d%%", (int)(m_pacificationAllocation * 100)), slX + slW + 6, slY + 2, 12, WHITE);
        float pacPct = m_pacificationAllocation * 50.0f;
        DrawText(TextFormat(T("Suppression: %.1f%%"), pacPct), slX, slY + slH + 4, 11, LIGHTGRAY);
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
    DrawText(c ? TextFormat(T("%s — Ethnic Management"), od::i18n::properName(c->name).c_str()) : T("Ethnic Management"),
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
            Color ac = align < 30 ? odPalette::of(odPalette::Role::Bad) : (align < 60 ? odPalette::of(odPalette::Role::Warning) : odPalette::of(odPalette::Role::Good));

            DrawRectangle(leftX, dy, panelW, rowH, isSel ? Color{60, 60, 80, 200} : Color{30, 30, 40, 180});
            DrawRectangle(leftX, dy, 6, rowH, e.color);
            DrawText(TextFormat("%s", e.name.c_str()), leftX + 12, dy + 3, 14, WHITE);
            char ps[32];
            if (e.pop > 1000000) snprintf(ps, sizeof(ps), "Pop: %.1fM", e.pop / 1000000.0f);
            else if (e.pop > 1000) snprintf(ps, sizeof(ps), "Pop: %.1fK", e.pop / 1000.0f);
            else snprintf(ps, sizeof(ps), "Pop: %lld", e.pop);
            DrawText(ps, leftX + 200, dy + 3, 12, LIGHTGRAY);
            DrawText(TextFormat(T("Alignment: %.0f%%"), align), leftX + 380, dy + 3, 12, ac);
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
                    Color oc = selected ? hexToColor(m_config.accent()) : Color{150, 150, 150, 200};
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

// ─── Political identity ──────────────────────────────────────────────────
//
// A government that has spent fifty turns enacting collectivisation is not the
// country it started as, and until now nothing said so: the name and the flag
// were fixed at load and never moved again. This reads the compass once a turn
// and restyles the countries that have gone somewhere.
//
// See PoliticalIdentity.h for the model. The two properties worth restating
// here, because this is the function that would break them:
//
//   The ORIGINAL is the input. Every restyle is computed from Country::rootName
//   and rootFlag, never from the current pair, so nothing compounds and a
//   country that swings back arrives at exactly what it was.
//
//   Changes are rate-limited. A doctrine can cross the whole threshold gap in
//   one step, so the radius pair alone is not enough -- identityTurn holds the
//   line, and it is saved, so reloading cannot buy a free change.
// === jitterStartingPolitics ===
//
// See FRESH_WORLD_COMPASS_JITTER. Runs once, on a world that has just been
// made, and never on one being loaded.
//
// DRAWN FROM THE SIM RNG, which chooseWorldSeed has just seeded from
// random_device. That is what makes two new games differ and what keeps the
// SAME seed reproducing the same world exactly -- OD_WORLD_SEED still pins it,
// and the determinism check still passes, because the check replays one seed.
//
// Deterministic in the ORDER it walks, too: m_countryCompass is an unordered
// map and drawing per country in its iteration order would make the result
// depend on the hash layout. The ids are sorted first.
void Game::jitterStartingPolitics() {
    if (!m_freshWorld) return;
    m_freshWorld = false;
    if (FRESH_WORLD_COMPASS_JITTER <= 0.0f) return;

    std::vector<int> cids;
    cids.reserve(m_countryCompass.size());
    for (const auto& [cid, v] : m_countryCompass) cids.push_back(cid);
    std::sort(cids.begin(), cids.end());

    int moved = 0;
    const float J = FRESH_WORLD_COMPASS_JITTER;
    for (int cid : cids) {
        auto it = m_countryCompass.find(cid);
        if (it == m_countryCompass.end()) continue;
        auto jitter = [&]() {
            // simRand() is the turn RNG; see Game_TurnLogic.
            return ((float)(simRandShared() % 2001) / 1000.0f - 1.0f) * J;
        };
        const float e0 = it->second.economic, s0 = it->second.social;
        it->second.economic = std::clamp(e0 + jitter(), -100.0f, 100.0f);
        it->second.social   = std::clamp(s0 + jitter(), -100.0f, 100.0f);
        // Kept on the Country too, or a save writes the map's value back and
        // the world quietly reverts to the one every other game started in.
        if (Country* c = m_countries.getCountry(cid)) {
            c->compassEconomic = it->second.economic;
            c->compassSocial   = it->second.social;
        }
        ++moved;
    }
    // A number that changes when the world changes, so "did the seed reach the
    // starting position" is a question with an answer rather than a count of
    // countries that is the same either way.
    unsigned long long h = 1469598103934665603ull;
    for (int cid : cids) {
        const auto& v = m_countryCompass[cid];
        auto mix = [&](float f) {
            h ^= (unsigned long long)(int)std::lround(f * 100.0f);
            h *= 1099511628211ull;
        };
        mix(v.economic); mix(v.social);
    }
    LoadLog() << "  Fresh world: nudged the politics of " << moved
              << " countr(ies) by up to " << (int)J
              << ", compass hash " << h << std::endl;
}

// ─── Districts ──────────────────────────────────────────────────────────────
//
// See District in GameStructs.h for what one is and why the ratio is the rule.

int Game::districtIndexOf(int countryId, int provinceId) const {
    auto it = m_districts.find(countryId);
    if (it == m_districts.end()) return -1;
    for (size_t i = 0; i < it->second.size(); ++i) {
        const auto& pv = it->second[i].provinces;
        if (std::find(pv.begin(), pv.end(), provinceId) != pv.end()) return (int)i;
    }
    return -1;
}

float Game::pacificationFactor(int countryId, int provinceId) const {
    auto it = m_districts.find(countryId);
    if (it == m_districts.end() || it->second.empty()) return 1.0f;

    const int di = districtIndexOf(countryId, provinceId);
    // A province in no district is policed as if the country were undivided.
    // NOT as if it had no budget: reconcileDistricts is supposed to make this
    // unreachable, and a gap in its coverage must not silently switch off the
    // suppression on newly conquered ground.
    if (di < 0) return 1.0f;

    long long held = 0;
    for (const auto& [pid, p] : m_provinces.getAllProvinces())
        if (p.countryId == countryId) ++held;
    if (held <= 0) return 1.0f;

    const auto& d = it->second[(size_t)di];
    if (d.provinces.empty()) return 0.0f;

    int shareTotal = 0;
    for (const auto& x : it->second) shareTotal += std::max(0, x.sharePct);
    if (shareTotal <= 0) return 0.0f;

    const double budgetShare = (double)std::max(0, d.sharePct) / (double)shareTotal;
    const double groundShare = (double)d.provinces.size() / (double)held;
    if (groundShare <= 0.0) return 0.0f;

    // Capped, because a one-province district with the whole budget would
    // otherwise multiply suppression by the province count and make rebellion
    // arithmetically impossible anywhere the player pointed at.
    return (float)std::min(6.0, budgetShare / groundShare);
}

void Game::ensureDefaultDistrict(int countryId) {
    if (countryId <= 0) return;
    auto& v = m_districts[countryId];
    if (!v.empty()) return;
    const Country* ac = m_countries.getCountry(countryId);
    // ── WHAT THE MAP'S AUTHOR DREW, IF THEY DREW ANYTHING ──
    //
    // A scenario that divides Austria-Hungary along its actual crown lands is
    // worth more than any name the generator can invent, so an authored
    // division wins outright. It is still passed through reconcileDistricts(),
    // which drops ground this country does not own in this session and hands
    // whatever is left over to the nearest district -- so a map authored before
    // a border moved degrades into something sensible instead of a hole.
    if (ac && !ac->isoA3.empty()) {
        auto aIt = m_authoredDistricts.find(ac->isoA3);
        if (aIt != m_authoredDistricts.end() && !aIt->second.empty()) {
            v = aIt->second;
            reconcileDistricts(countryId);
            normaliseDistrictShares(countryId, -1);
            if (!m_districts[countryId].empty()) {
                if (getenv("OD_DNAME_DEBUG")) {
                    printf("[AUTHDIST] cid=%d iso=%s installed %zu authored district(s):",
                           countryId, ac->isoA3.c_str(), m_districts[countryId].size());
                    for (const auto& ad : m_districts[countryId])
                        printf("  \"%s\" %zu prov %d%%", ad.name.c_str(),
                               ad.provinces.size(), ad.sharePct);
                    printf("\n");
                }
                return;
            }
            v.clear();   // nothing of it survived; fall through to the default
        }
    }
    District d;
    d.id = 1;
    const Country* c = m_countries.getCountry(countryId);
    if (c) { d.r = c->color.r; d.g = c->color.g; d.b = c->color.b; }
    d.sharePct = 100;
    for (const auto& [pid, p] : m_provinces.getAllProvinces())
        if (p.countryId == countryId) d.provinces.push_back(pid);
    std::sort(d.provinces.begin(), d.provinces.end());
    // Named for the ground it holds, not "Home": on a world map that is the
    // difference between "Bavaria Region" and a label that means nothing.
    d.name = suggestDistrictName(countryId, d.provinces);
    v.push_back(std::move(d));
}

void Game::reconcileDistricts(int countryId) {
    auto it = m_districts.find(countryId);
    if (it == m_districts.end() || it->second.empty()) return;   // undivided stays undivided
    auto& v = it->second;

    std::unordered_set<int> owned;
    for (const auto& [pid, p] : m_provinces.getAllProvinces())
        if (p.countryId == countryId) owned.insert(pid);

    // Ground that is no longer ours leaves the district that held it.
    std::unordered_set<int> covered;
    for (auto& d : v) {
        d.provinces.erase(std::remove_if(d.provinces.begin(), d.provinces.end(),
            [&](int pid) { return owned.find(pid) == owned.end(); }), d.provinces.end());
        for (int pid : d.provinces) covered.insert(pid);
    }
    // ── AND GROUND THAT IS OURS AND IN NO DISTRICT JOINS THE NEAREST ONE ──
    //
    // It used to join the FIRST, on the reasoning that a district is something
    // the player drew and extending one they did not choose is presumptuous.
    // That was wrong in practice: conquer a province on your eastern border and
    // it landed in whatever district happened to be first in the list, usually
    // on the other side of the country, and the player had to go and find it.
    // The nearest district is the one they would have put it in.
    //
    // Nearest by the province CENTRE, and adjacency is not enough on its own --
    // an island or a province taken across a strait touches nothing, and has to
    // land somewhere rather than nowhere.
    std::vector<int> orphans;
    for (int pid : owned) if (covered.find(pid) == covered.end()) orphans.push_back(pid);
    std::sort(orphans.begin(), orphans.end());     // deterministic order
    for (int pid : orphans) {
        auto oc = m_provinceCenters.find(pid);
        size_t best = 0;
        double bestD = std::numeric_limits<double>::max();
        for (size_t di = 0; di < v.size(); ++di) {
            for (int other : v[di].provinces) {
                // A district it actually touches wins outright, whatever the
                // centres say: a border is a better answer than a distance.
                if (provincesAdjacent(pid, other)) { bestD = -1.0; best = di; break; }
                if (oc == m_provinceCenters.end()) continue;
                auto pc2 = m_provinceCenters.find(other);
                if (pc2 == m_provinceCenters.end()) continue;
                const double dx = oc->second.x - pc2->second.x;
                const double dy = oc->second.y - pc2->second.y;
                const double d2 = dx * dx + dy * dy;
                if (d2 < bestD) { bestD = d2; best = di; }
            }
            if (bestD < 0.0) break;
        }
        v[best].provinces.push_back(pid);
    }
    for (auto& d : v) std::sort(d.provinces.begin(), d.provinces.end());
    // A district with nothing left in it is not a place any more. One always
    // survives: a country has to be divided into something, and "no districts"
    // is a different state (undivided) reached by deleting down to one.
    if (v.size() > 1) {
        v.erase(std::remove_if(v.begin(), v.end(),
                    [](const District& d) { return d.provinces.empty(); }), v.end());
        if (v.empty()) v.resize(1);
    }
    normaliseDistrictShares(countryId, 0);
}

void Game::normaliseDistrictShares(int countryId, int changed) {
    auto it = m_districts.find(countryId);
    if (it == m_districts.end() || it->second.empty()) return;
    auto& v = it->second;
    const int n = (int)v.size();
    if (n == 1) { v[0].sharePct = 100; return; }
    changed = std::clamp(changed, 0, n - 1);
    v[(size_t)changed].sharePct = std::clamp(v[(size_t)changed].sharePct, 0, 100);

    int rest = 0;
    for (int i = 0; i < n; ++i) if (i != changed) rest += std::max(0, v[(size_t)i].sharePct);
    const int budget = 100 - v[(size_t)changed].sharePct;
    int handed = 0, last = -1;
    for (int i = 0; i < n; ++i) {
        if (i == changed) continue;
        const int val = rest > 0
            ? (int)((long long)budget * std::max(0, v[(size_t)i].sharePct) / rest)
            : budget / (n - 1);
        v[(size_t)i].sharePct = val;
        handed += val;
        last = i;
    }
    // The rounding remainder goes somewhere, or the panel reads 99.
    if (last >= 0) v[(size_t)last].sharePct += budget - handed;
}

void Game::splitDistrictSharesEqually(int countryId) {
    auto it = m_districts.find(countryId);
    if (it == m_districts.end() || it->second.empty()) return;
    auto& v = it->second;
    const int n = (int)v.size();
    const int each = 100 / n;
    for (auto& d : v) d.sharePct = each;
    // EQUAL MEANS EQUAL, and 100 does not divide by three. The remainder is
    // handed out one point at a time from the front rather than dropped, so
    // three districts read 34/33/33 and not 33/33/33.
    int left = 100 - each * n;
    for (int i = 0; i < n && left > 0; ++i, --left) v[(size_t)i].sharePct += 1;
}

// ── SPLIT BY SIZE, WHICH IS THE NEUTRAL SETTING ──
//
// Shares proportional to how much ground each district holds. Worth its own
// button because of what the resolver does with them: pacificationFactor is
// budgetShare / groundShare, so making the two equal gives every district a
// factor of exactly 1.0 -- the same suppression everywhere, which is what the
// country had before it was divided at all.
//
// That makes this the ZERO of the mechanic rather than just another
// convenience. "Equally" is a different thing and often the wrong one: on a
// country cut into a 23-province heartland and a 1-province island, equal
// shares hand the island twenty-three times the suppression per province.
void Game::splitDistrictSharesBySize(int countryId) {
    auto it = m_districts.find(countryId);
    if (it == m_districts.end() || it->second.empty()) return;
    auto& v = it->second;

    long long total = 0;
    for (const auto& d : v) total += (long long)d.provinces.size();
    // Nothing to be proportional TO. An undivided-looking country of empty
    // districts falls back to equal shares rather than to zeros, which would
    // leave the whole budget unspent.
    if (total <= 0) { splitDistrictSharesEqually(countryId); return; }

    int given = 0;
    for (auto& d : v) {
        d.sharePct = (int)((long long)d.provinces.size() * 100 / total);
        given += d.sharePct;
    }
    // The remainder goes to the LARGEST districts first, which is where the
    // rounding took it from: integer division truncates every share, and
    // handing the difference back in size order keeps the result monotone --
    // a bigger district never ends up with a smaller share than a smaller one.
    std::vector<size_t> order(v.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (v[a].provinces.size() != v[b].provinces.size())
            return v[a].provinces.size() > v[b].provinces.size();
        return a < b;                       // stable, so the same country twice agrees
    });
    for (size_t k = 0; given < 100 && k < order.size(); ++k, ++given)
        v[order[k]].sharePct += 1;
}

// ─── The Districts tab ──────────────────────────────────────────────────────
//
// The pacification budget lives here now rather than beside the ethnic list,
// because the question it answers changed: it used to be "how hard do I police
// the country" and it is now "how hard, and WHERE". A single number and a map
// of shares belong on one screen.
void Game::drawDistrictsTab() {
    const Country* pc = m_countries.getCountry(m_playerCountryId);
    if (!pc) return;
    ensureDefaultDistrict(m_playerCountryId);
    auto& districts = m_districts[m_playerCountryId];
    if (districts.empty()) return;
    m_districtSel = std::clamp(m_districtSel, 0, (int)districts.size() - 1);

    const Vector2 mouse = getMouse();
    const bool click = !m_paused && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    const int top = 148;

    // ── THE NAMES THE GAME ITSELF GIVES A DISTRICT ──
    //
    // Stored in the save as ENGLISH KEYS and translated on the way out, so a
    // player who switches language finds their districts renamed rather than
    // finding them in the wrong language forever -- and so a name they typed
    // themselves passes through untouched, because T() returns its input when
    // there is nothing to look up.
    //
    // Named here because the extractor reads T() literals out of the source and
    // has no way to know what a std::string held at runtime. Without this the
    // default district was called "Home" in twenty languages.
    //
    // NOT the bare words "Unrest" and "Settled". Those collide: the interface
    // already says "Unrest: %.1f%%" about the quantity, and a key that means
    // the quantity in one place and a REGION in another is the fault the
    // terminology linter exists to catch -- it caught this one, in fifteen
    // languages at once.
    (void)T("Home"); (void)T("Troubled Region"); (void)T("Quiet Region");

    // ── ITS OWN GROUND ──
    //
    // The politics screen is drawn OVER the world, and the world's own HUD --
    // the flag, the country name, the province count -- keeps its left column.
    // The other tabs live to the right of it and never noticed; this one wants
    // the full width for a map, so it puts something opaque down first.
    // Photographed without it: the budget slider and the country panel were
    // printed on top of each other.
    DrawRectangle(16, top - 34, m_screenW - 32, m_screenH - top - 2, Color{10, 11, 16, 246});
    DrawRectangleLines(16, top - 34, m_screenW - 32, m_screenH - top - 2, Color{54, 58, 76, 200});

    // ─── The budget this whole screen divides ───
    {
        auto cs = computeCountryIncome(m_playerCountryId);
        const float researchAmt = cs.total * m_researchAllocation;
        const float affordable = std::max(0.0f, cs.total - cs.armyExpenses - cs.navyExpenses
                                                 - cs.policyCosts - cs.minorityCosts - researchAmt);
        float maxFrac = (cs.total > 0) ? affordable / cs.total : 0.0f;
        maxFrac = std::min(maxFrac, 1.0f);
        if (m_pacificationAllocation > maxFrac) m_pacificationAllocation = maxFrac;

        const int slX = 34, slY = top, slW = 260, slH = 18;
        DrawText(T("Pacification Budget"), slX, slY - 20, 14, hexToColor(m_config.accent()));
        DrawRectangle(slX, slY, slW, slH, {40, 40, 50, 200});
        const int maxFill = (int)(slW * maxFrac);
        if (maxFill > 0) DrawRectangle(slX, slY, maxFill, slH, {40, 50, 60, 150});
        const int fill = (int)(slW * m_pacificationAllocation);
        if (fill > 0) DrawRectangle(slX, slY, fill, slH, {80, 180, 220, 200});
        DrawRectangleLines(slX, slY, slW, slH, {80, 80, 100, 200});
        DrawText(TextFormat("%d%%", (int)(m_pacificationAllocation * 100)),
                 slX + slW + 8, slY + 2, 13, WHITE);
        const Rectangle bar = {(float)slX, (float)slY, (float)slW, (float)slH};
        float t = m_pacificationAllocation;
        if (sliderInteract(bar, 0, t, m_draggingPacification))
            m_pacificationAllocation = std::clamp(t, 0.0f, maxFrac);
        DrawText(T("Every district draws its share of this."), slX, slY + slH + 6, 11,
                 Color{150, 154, 170, 255});
    }

    // ─── The map ───
    const int mapX = 26, mapY = top + 58;
    const int listW = 380;
    const int mapW = std::max(200, m_screenW - listW - 90);
    const int mapH = std::max(160, m_screenH - mapY - 26);
    const int texW = m_provinces.getWidth(), texH = m_provinces.getHeight();

    if (texW > 0 && texH > 0 && m_politicalTex.id > 0) {
        rebuildDistrictOverlay(m_playerCountryId, texW, texH);

        const float srcW = texW / m_districtMapZoom, srcH = texH / m_districtMapZoom;
        m_districtMapSrcX = std::clamp(m_districtMapSrcX, 0.0f, std::max(0.0f, texW - srcW));
        m_districtMapSrcY = std::clamp(m_districtMapSrcY, 0.0f, std::max(0.0f, texH - srcH));
        const Rectangle src = {m_districtMapSrcX, m_districtMapSrcY, srcW, srcH};
        const Rectangle dst = {(float)mapX, (float)mapY, (float)mapW, (float)mapH};
        DrawTexturePro(m_politicalTex, src, dst, {0, 0}, 0, WHITE);
        // The overlay is a quarter of the political map's size, so its source
        // rectangle is halved in each direction. See rebuildDistrictOverlay.
        const Rectangle ovSrc = {src.x / DISTRICT_OVERLAY_DIV, src.y / DISTRICT_OVERLAY_DIV,
                                 src.width / DISTRICT_OVERLAY_DIV, src.height / DISTRICT_OVERLAY_DIV};
        DrawTexturePro(m_districtOverlayTex, ovSrc, dst, {0, 0}, 0, WHITE);
        DrawRectangleLines(mapX, mapY, mapW, mapH, {80, 80, 120, 180});

        const bool over = CheckCollisionPointRec(mouse, dst);
        if (over) {
            const float wheel = GetMouseWheelMove();
            if (wheel != 0.0f) {
                const float before = m_districtMapZoom;
                m_districtMapZoom = std::clamp(m_districtMapZoom * (wheel > 0 ? 1.2f : 1.0f / 1.2f),
                                               1.0f, 12.0f);
                // Zoom toward the cursor, or the map crawls away from what the
                // player is pointing at.
                const float fx = (mouse.x - dst.x) / dst.width, fy = (mouse.y - dst.y) / dst.height;
                m_districtMapSrcX += texW * fx * (1.0f / before - 1.0f / m_districtMapZoom);
                m_districtMapSrcY += texH * fy * (1.0f / before - 1.0f / m_districtMapZoom);
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                m_districtMapDragging = true; m_districtMapDragFrom = mouse;
            }
        }
        if (m_districtMapDragging) {
            if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
                m_districtMapSrcX -= (mouse.x - m_districtMapDragFrom.x) * (srcW / dst.width);
                m_districtMapSrcY -= (mouse.y - m_districtMapDragFrom.y) * (srcH / dst.height);
                m_districtMapDragFrom = mouse;
            } else m_districtMapDragging = false;
        }

        // ── ASSIGNING GROUND, BY PAINTING IT ──
        //
        // Held rather than clicked: redrawing a district is a shape, and asking
        // for one click per province would make a twenty-province district
        // twenty decisions. Right mouse pans, so the left one is free to paint.
        if (over && !m_districtMapDragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            const float fx = (mouse.x - dst.x) / dst.width, fy = (mouse.y - dst.y) / dst.height;
            const int px = (int)std::clamp(src.x + fx * srcW, 0.0f, (float)(texW - 1));
            const int py = (int)std::clamp(src.y + fy * srcH, 0.0f, (float)(texH - 1));
            const Province* prov = m_provinces.getProvince(px, py);
            if (prov && prov->id > 0 && prov->countryId == m_playerCountryId) {
                const int pid = prov->id;
                const int cur = districtIndexOf(m_playerCountryId, pid);
                if (cur != m_districtSel) {
                    if (cur >= 0) {
                        auto& from = districts[(size_t)cur].provinces;
                        from.erase(std::remove(from.begin(), from.end(), pid), from.end());
                    }
                    auto& to = districts[(size_t)m_districtSel];
                    const bool wasEmpty = to.provinces.empty();
                    to.provinces.push_back(pid);
                    std::sort(to.provinces.begin(), to.provinces.end());
                    // A district takes its name from the ground the moment it
                    // HAS any: until then there is nothing to name it after.
                    if (wasEmpty)
                        to.name = uniqueDistrictName(
                            m_playerCountryId,
                            suggestDistrictName(m_playerCountryId, to.provinces),
                            -1, to.provinces);
                    m_districtOverlayDirty = true;
                }
            }
        }
        DrawText(T("Hold left mouse to paint into the selected district | right-drag to pan"),
                 mapX + 6, mapY + mapH - 18, 12, Color{180, 180, 200, 140});
    }

    // ─── The list ───
    const int listX = m_screenW - listW - 30;
    int y = top;
    DrawRectangle(listX, y - 24, listW, m_screenH - y - 16, {15, 15, 25, 220});
    DrawRectangleLines(listX, y - 24, listW, m_screenH - y - 16, {60, 60, 90, 200});

    // NEW and EQUALLY, side by side: the two things a player does to the SET of
    // districts rather than to one of them.
    Rectangle newBtn = {(float)(listX + 10), (float)(y - 16), 150, 26};
    const bool newHov = CheckCollisionPointRec(mouse, newBtn);
    DrawRectangleRounded(newBtn, 0.2f, 6, newHov ? Color{40, 70, 50, 225} : Color{26, 44, 34, 210});
    DrawRectangleRoundedLines(newBtn, 0.2f, 6, Color{80, 160, 100, 200});
    DrawText(T("+ New district"), (int)newBtn.x + 12, (int)newBtn.y + 6, 13, WHITE);
    if (newHov && click) {
        District d;
        int maxId = 0;
        for (const auto& x : districts) maxId = std::max(maxId, x.id);
        d.id = maxId + 1;
        // Unnamed ground has no name to take, so it keeps a number until the
        // first province is painted into it. See the paint handler. EMPTY
        // rather than "District 3": districtDisplayName numbers it when it is
        // drawn, so the number is in the reader's language rather than in
        // whichever one was loaded when the button was clicked.
        d.name.clear();
        // Spread around the wheel so two new districts are never the same
        // colour -- a map where two districts share a colour is a map that
        // cannot be read, which is the one job it has.
        const float hue = (float)((d.id * 67) % 360);
        const Color c = ColorFromHSV(hue, 0.55f, 0.85f);
        d.r = c.r; d.g = c.g; d.b = c.b;
        d.sharePct = 0;
        districts.push_back(std::move(d));
        m_districtSel = (int)districts.size() - 1;
        normaliseDistrictShares(m_playerCountryId, m_districtSel);
        m_districtOverlayDirty = true;
        Audio::get().playSfx("click_soft");
    }
    // Two ways to divide the money, side by side, because they answer different
    // questions: equally treats the districts as equals, by size treats the
    // GROUND as equal. See splitDistrictSharesBySize.
    {
        const int bw = 132;
        Rectangle eqBtn   = {(float)(listX + 172), (float)(y - 16), (float)bw, 26};
        Rectangle sizeBtn = {(float)(listX + 172 + bw + 8), (float)(y - 16), (float)bw, 26};
        auto splitButton = [&](const Rectangle& r, const char* label, const char* tip) {
            const bool hov = CheckCollisionPointRec(mouse, r);
            DrawRectangleRounded(r, 0.2f, 6, hov ? Color{44, 48, 66, 225} : Color{28, 30, 42, 210});
            DrawRectangleRoundedLines(r, 0.2f, 6, Color{90, 96, 130, 200});
            int fs = 13;
            const std::string fit = odText::fitToWidth(label, (int)r.width - 16, fs, 10);
            DrawText(fit.c_str(), (int)r.x + 8, (int)r.y + 6, fs, WHITE);
            if (hov) DrawText(tip, (int)listX, (int)r.y + 30, 11, Color{150, 155, 172, 220});
            return hov && click;
        };
        if (splitButton(eqBtn, T("Split equally"),
                        T("Every district gets the same money, whatever it holds")))
            { splitDistrictSharesEqually(m_playerCountryId); Audio::get().playSfx("click_soft"); }
        if (splitButton(sizeBtn, T("Split by size"),
                        T("Money follows the ground: the same suppression everywhere")))
            { splitDistrictSharesBySize(m_playerCountryId); Audio::get().playSfx("click_soft"); }
    }

    y += 22;
    long long held = 0;
    for (const auto& [pid, p] : m_provinces.getAllProvinces())
        if (p.countryId == m_playerCountryId) ++held;

    for (size_t i = 0; i < districts.size(); ++i) {
        auto& d = districts[i];
        const int rowH = 74;
        Rectangle row = {(float)(listX + 8), (float)y, (float)(listW - 16), (float)(rowH - 6)};
        if (row.y + row.height > m_screenH - 20) break;
        const bool sel = ((int)i == m_districtSel);
        const bool hov = CheckCollisionPointRec(mouse, row);
        DrawRectangleRounded(row, 0.12f, 6,
            sel ? Color{34, 40, 54, 225} : hov ? Color{26, 30, 40, 210} : Color{20, 22, 30, 195});
        DrawRectangleRoundedLines(row, 0.12f, 6,
            sel ? hexToColor(m_config.accent()) : Color{60, 64, 80, 170});

        // The colour, and a click to change it. The swatch IS the legend: the
        // map has no room for one and a district nobody can find on it is a
        // budget line rather than a place.
        Rectangle sw = {row.x + 8, row.y + 8, 22, 22};
        DrawRectangleRounded(sw, 0.25f, 4, Color{d.r, d.g, d.b, 255});
        DrawRectangleRoundedLines(sw, 0.25f, 4, Color{230, 230, 240, 200});
        if (CheckCollisionPointRec(mouse, sw) && click) {
            const Color c = ColorFromHSV((float)(GetRandomValue(0, 35) * 10), 0.55f, 0.85f);
            d.r = c.r; d.g = c.g; d.b = c.b;
            m_districtOverlayDirty = true;
            Audio::get().playSfx("click_soft");
        }

        int nfs = 14;
        const std::string nm = odText::fitToWidth(districtDisplayName(d), (int)row.width - 120, nfs, 10);
        DrawText(nm.c_str(), (int)row.x + 38, (int)row.y + 9, nfs,
                 sel ? WHITE : Color{200, 205, 220, 255});
        DrawText(TextFormat(T("%d province(s)"), (int)d.provinces.size()),
                 (int)row.x + 38, (int)row.y + 28, 11, Color{150, 154, 170, 255});

        // Its claim on the budget, and what that actually buys.
        Rectangle sh = {row.x + 8, row.y + 46, row.width - 90, 14};
        DrawRectangle((int)sh.x, (int)sh.y, (int)sh.width, (int)sh.height, {30, 32, 42, 215});
        DrawRectangle((int)sh.x, (int)sh.y, (int)(sh.width * d.sharePct / 100), (int)sh.height,
                      Color{80, 170, 200, 220});
        DrawRectangleLines((int)sh.x, (int)sh.y, (int)sh.width, (int)sh.height, {70, 74, 92, 200});
        DrawText(TextFormat("%d%%", d.sharePct), (int)(sh.x + sh.width + 6), (int)sh.y + 1, 11,
                 Color{190, 195, 215, 255});
        if (!m_paused && CheckCollisionPointRec(mouse, sh) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            d.sharePct = std::clamp((int)((mouse.x - sh.x) / sh.width * 100), 0, 100);
            normaliseDistrictShares(m_playerCountryId, (int)i);
            m_districtSel = (int)i;
        }
        // What the ratio comes to, said in the units the player already reads
        // beside unrest. A share on its own does not tell anybody whether this
        // district is being policed harder or softer than before.
        if (held > 0 && !d.provinces.empty()) {
            const double ground = (double)d.provinces.size() / (double)held;
            const double f = ground > 0 ? std::min(6.0, (d.sharePct / 100.0) / ground) : 0.0;
            DrawText(TextFormat(T("suppression x%.2f"), f),
                     (int)row.x + 38, (int)row.y + 62 - 12, 10, Color{130, 175, 195, 255});
        }

        // ── AND A WAY TO UNDRAW ONE ──
        //
        // Offered on every district except when there is only one left: a
        // country is always divided into something, and deleting the last one
        // would leave provinces belonging to nothing. Its ground is not lost --
        // reconcileDistricts puts every orphan into the nearest surviving
        // district, which is the same rule a conquest goes through.
        Rectangle del = {row.x + row.width - 24, row.y + 6, 18, 18};
        if (districts.size() > 1) {
            const bool dh = CheckCollisionPointRec(mouse, del);
            DrawRectangleRounded(del, 0.3f, 4, dh ? Color{92, 34, 30, 235} : Color{40, 26, 26, 200});
            DrawRectangleRoundedLines(del, 0.3f, 4, dh ? Color{210, 110, 95, 220} : Color{90, 62, 60, 180});
            const char* x = "x";
            DrawText(x, (int)(del.x + (del.width - MeasureText(x, 12)) / 2), (int)del.y + 3, 12,
                     dh ? WHITE : Color{190, 150, 145, 255});
            if (dh) m_uiHint = T("Remove this district; its provinces join the nearest one");
            if (dh && click) {
                districts.erase(districts.begin() + (long)i);
                m_districtSel = std::clamp(m_districtSel, 0, (int)districts.size() - 1);
                reconcileDistricts(m_playerCountryId);   // the ground finds a home
                normaliseDistrictShares(m_playerCountryId, m_districtSel);
                m_districtOverlayDirty = true;
                Audio::get().playSfx("click_soft");
                break;                                   // the list just changed under us
            }
        }

        if (hov && click && !CheckCollisionPointRec(mouse, sh) && !CheckCollisionPointRec(mouse, sw)
            && !CheckCollisionPointRec(mouse, del)) {
            if (m_districtSel != (int)i) {
                m_districtSel = (int)i;
                m_districtOverlayDirty = true;
                Audio::get().playSfx("tab_switch");
            }
        }
        y += rowH;
    }

    // ─── The regional law in force in the selected district ───
    //
    // NOT the doctrine list under a smaller heading. A doctrine is a statement
    // about a whole country; what a provincial administration decides is a
    // curfew, a tax holiday, whose language the courts use. Their own set, in
    // data/district_laws.json -- and they go both ways, so this list is a menu
    // of trades rather than a menu of improvements.
    auto& sel = districts[(size_t)m_districtSel];
    y += 6;
    DrawText(TextFormat(T("Regional law in %s"), districtDisplayName(sel).c_str()),
             listX + 12, y, 13, hexToColor(m_config.accent()));
    y += 19;
    DrawText(TextFormat(T("%d province(s) to administer"), (int)sel.provinces.size()),
             listX + 12, y, 11, Color{150, 154, 170, 255});
    y += 17;

    for (const auto& law : m_districtLaws) {
        if (y > m_screenH - 52) { DrawText("...", listX + 12, y, 12, Color{120, 124, 140, 255}); break; }
        const bool on = std::find(sel.policies.begin(), sel.policies.end(), law.id)
                        != sel.policies.end();
        Rectangle r = {(float)(listX + 10), (float)y, (float)(listW - 20), 38};
        const bool ph = CheckCollisionPointRec(mouse, r);
        DrawRectangleRounded(r, 0.16f, 5,
            on ? Color{34, 58, 44, 228} : ph ? Color{30, 34, 46, 212} : Color{20, 22, 30, 192});
        DrawRectangleRoundedLines(r, 0.16f, 5,
            on ? Color{90, 180, 110, 215} : Color{60, 64, 80, 170});

        int pfs = 12;
        const std::string nm2 = odText::fitToWidth(T(law.name.c_str()), (int)r.width - 130, pfs, 9);
        DrawText(nm2.c_str(), (int)r.x + 10, (int)r.y + 5, pfs,
                 on ? WHITE : Color{198, 203, 218, 255});

        // WHAT IT COSTS HERE, not what it costs in the abstract: the price is
        // per province, so the same law is a different bill in a different
        // district and that is the thing being decided.
        const char* bill = TextFormat("-%.1f", law.costPerTurn * (float)sel.provinces.size());
        DrawText(bill, (int)(r.x + r.width - MeasureText(bill, 11) - 9), (int)r.y + 6, 11,
                 Color{215, 175, 130, 255});

        // Both directions, in the colour each deserves. A law that raises money
        // and raises resentment has to read as both at once.
        int ex = (int)r.x + 10;
        auto term = [&](float v, const char* suffix, bool goodIsUp) {
            if (std::abs(v) < 0.05f) return;
            const bool good = goodIsUp ? (v > 0) : (v < 0);
            const char* t = TextFormat("%+.0f%%%s", v, suffix);
            DrawText(t, ex, (int)r.y + 22, 10,
                     good ? Color{130, 200, 150, 255} : Color{215, 130, 120, 255});
            ex += MeasureText(t, 10) + 10;
        };
        term(law.unrestPct, T(" unrest"), false);
        term(law.incomePct, T(" income"), true);
        term(law.growthPct, T(" growth"), true);

        if (ph) m_uiHint = T(law.description.c_str());
        if (ph && click) {
            if (on) sel.policies.erase(std::find(sel.policies.begin(), sel.policies.end(), law.id));
            else    sel.policies.push_back(law.id);
            Audio::get().playSfx("click_soft");
        }
        y += 42;
    }
}


const char* Game::districtWordForMap() const {
    // Chosen by the MAP, not by the game or the player's language: a world
    // either has oblasts or it has counties, and mixing them inside one world
    // reads as an accident. Hashed from the map's own name so it is stable
    // across sessions and a generated world gets its own answer rather than
    // always the first entry.
    // NOT "March". The word for a border province is also the name of a month,
    // and the string table already translates it as one -- a district would
    // have been called "Bavaria Березень". Same collision as the bare "Unrest"
    // key, caught the same way: by looking before translating.
    static const char* const kWords[] = {
        "Region", "Province", "District", "County", "Governorate", "Oblast",
        "Prefecture", "Territory", "Circuit", "Borderland",
    };
    constexpr size_t N = sizeof(kWords) / sizeof(kWords[0]);
    //
    // Hashed from the MAP'S OWN CONTENT -- how many provinces it has and what
    // the first one is called -- rather than from a file path or the world
    // seed. A path is not always known by the time this is asked, and the seed
    // changes every new game, which would make the same world use a different
    // vocabulary on Tuesday. Content is the thing that actually identifies a
    // map, and it is available wherever this is called.
    static unsigned long long cachedKey = 0;
    static size_t cachedIdx = 0;
    unsigned long long h = 1469598103934665603ull;
    auto mix = [&h](unsigned long long v) { h ^= v; h *= 1099511628211ull; };
    mix(m_provinces.getAllProvinces().size());
    int lowest = -1;
    for (const auto& [pid, pv] : m_provinces.getAllProvinces())
        if (lowest < 0 || pid < lowest) lowest = pid;
    if (const Province* p0 = (lowest > 0) ? m_provinces.getProvinceById(lowest) : nullptr)
        for (unsigned char c : p0->name) mix(c);
    if (h != cachedKey) { cachedKey = h; cachedIdx = (size_t)(h % N); }
    return kWords[cachedIdx];
}

// ── NAMED THE WAY A COUNTRY IS NAMED ──
//
// The first version built "<People> <Word>" and translated the word at CREATION
// time: "Українці Територія V" -- a plural noun jammed against a noun, in
// whatever language happened to be loaded when the district was drawn, frozen
// there for the life of the save. Two faults in one string. It read wrong in
// English too ("Ukrainians Territory") and it did not follow a language change,
// because by then the name was just text.
//
// A district is a REGION, and the game already knows how to name places: a
// breakaway state derives one from a people ("Ukrainian" -> Ukraine) and
// od::i18n::properName renders it per language through patterns the string
// table already carries -- "Northern %s" is translated in all twenty. So a
// district is named the same way and rendered the same way: the canonical
// English form is stored, and the language decides what it looks like every
// time it is drawn.
//
// NO RNG HERE. The breakaway namer INVENTS a place when derivation fails, and
// that invention draws from simRand() -- calling it from a UI or reflex path
// would pull the deterministic stream out from under the turn. Only the
// deterministic half of the derivation runs here (the irregular table, then the
// countries actually on the map), and where it fails the compass answers
// instead, which needs no invention at all.
std::string Game::districtPlaceName(int countryId, const std::vector<int>& provinces) const {
    // Whoever holds a real majority lends their name: a district drawn around a
    // people is usually drawn around them on purpose.
    std::unordered_map<std::string, double> share;
    for (int pid : provinces) {
        auto mit = m_provinceMinorities.find(pid);
        if (mit == m_provinceMinorities.end()) continue;
        for (const auto& g : mit->second) share[g.name] += g.pct;
    }
    std::string people;
    if (!share.empty()) {
        // Sorted rather than max_element over an unordered_map: iteration order
        // there is unspecified, and a name that changes between two runs of the
        // same save is worse than a dull one.
        std::vector<std::pair<std::string, double>> byShare(share.begin(), share.end());
        std::sort(byShare.begin(), byShare.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });
        if (byShare.front().second / (double)provinces.size() >= DISTRICT_NAME_MAJORITY)
            people = byShare.front().first;
    }

    auto placeOf = [&](std::string root) -> std::string {
        if (root.empty()) return root;
        // The last word is the demonym; everything before it says which group,
        // not which place -- "Mestizo Mexican", "Han Chinese".
        if (auto sp = root.find_last_of(' '); sp != std::string::npos)
            root = root.substr(sp + 1);
        if (!root.empty() && root.back() == 's') root.pop_back();   // Assyrians -> Assyrian
        if (root.empty()) return root;
        if (std::string place = politid::properPlaceName(root); !place.empty()) return place;
        // A demonym whose country is on this map: "Mexican" must not become
        // anything else while Mexico is sitting there. Prefix-matched, because
        // English builds demonyms by mangling the tail and four characters is
        // enough to be the same place.
        std::string rl = root;
        std::transform(rl.begin(), rl.end(), rl.begin(), ::tolower);
        std::string best; size_t bestN = 0;
        for (const auto& [ocid, oc] : m_countries.getAll()) {
            if (ocid <= 0 || ocid >= REBEL_CID_MIN || oc.name.empty()) continue;
            const std::string core = politid::geographicCoreOf(
                oc.rootSaved && !oc.rootName.empty() ? oc.rootName : oc.name);
            if (core.size() < 4) continue;
            std::string cl = core;
            std::transform(cl.begin(), cl.end(), cl.begin(), ::tolower);
            size_t n = 0;
            while (n < cl.size() && n < rl.size() && cl[n] == rl[n]) ++n;
            if (n >= 4 && n > bestN) { bestN = n; best = core; }
        }
        return best;
    };

    // The PLACE a people names, not the people. placeOf falls back to the
    // country whose core matches the demonym, and for "British Empire" that
    // core IS "British" -- a word the names table knows, as "Британці", the
    // Britons. A district called after a nation of people rather than a piece
    // of ground is the bug this naming was meant to fix, so the irregular table
    // is asked FIRST and the demonym is only a last resort.
    const std::string fromPeople = placeOf(people);
    std::string peopleRoot = people;
    if (auto sp = peopleRoot.find_last_of(' '); sp != std::string::npos)
        peopleRoot = peopleRoot.substr(sp + 1);
    if (!peopleRoot.empty() && peopleRoot.back() == 's') peopleRoot.pop_back();
    const std::string peoplePlace = politid::properPlaceName(peopleRoot);
    if (od::i18n::knownName(peoplePlace)) return peoplePlace;

    // Otherwise the country's own place name, which is what a region inside it
    // is a part OF: "Eastern Ukraine" rather than "Eastern Ukrainians".
    const Country* c = m_countries.getCountry(countryId);
    if (!c) return fromPeople;
    const std::string core = politid::geographicCoreOf(
        c->rootSaved && !c->rootName.empty() ? c->rootName : c->name);
    const std::string place = core.empty() ? c->name : core;
    const std::string irregular = politid::properPlaceName(place);

    // ── A NAME THE TABLE KNOWS BEATS A PRETTIER ONE IT DOES NOT ──
    //
    // "British Empire" cores to the adjective "British", which the irregular
    // table turns into "Britain" -- correct English, and NOT a name the string
    // table carries, so it renders as a transliteration: "Брітаін". Letters,
    // not a word. The country's own name IS in the table, as "Британська
    // імперія", so a district called "Eastern British Empire" reads as real
    // words in every language even though the English is clumsier.
    //
    // Preference order: a real place the table knows, then the country's own
    // name (clumsier English, real words in every language), then a demonym,
    // then the best English we can derive. Only the last transliterates, which
    // is the same deal a breakaway state gets and is acceptable for a place
    // nobody has a word for.
    for (const std::string& cand : {irregular, c->name, place, fromPeople})
        if (od::i18n::knownName(cand)) return cand;
    if (!irregular.empty()) return irregular;
    if (!fromPeople.empty()) return fromPeople;
    return place;
}

std::string Game::suggestDistrictName(int countryId, const std::vector<int>& provinces,
                                      bool forceDirection) const {
    const std::string place = districtPlaceName(countryId, provinces);
    if (place.empty() || provinces.empty()) return place;

    // Where it sits, against the middle of the country that holds it. The
    // direction is the disambiguator -- five of them plus the bare place is six
    // names before anything has to repeat, which is why districts no longer
    // carry Roman numerals in the ordinary case.
    double cx = 0, cy = 0; int n = 0;
    for (const auto& [pid, pv] : m_provinces.getAllProvinces()) {
        if (pv.countryId != countryId) continue;
        auto c = m_provinceCenters.find(pid);
        if (c == m_provinceCenters.end()) continue;
        cx += c->second.x; cy += c->second.y; ++n;
    }
    double dx2 = 0, dy2 = 0; int m = 0;
    for (int pid : provinces) {
        auto c = m_provinceCenters.find(pid);
        if (c == m_provinceCenters.end()) continue;
        dx2 += c->second.x; dy2 += c->second.y; ++m;
    }
    if (n <= 0 || m <= 0) return place;

    const double ex = dx2 / m - cx / n, ey = dy2 / m - cy / n;
    // A span-relative bar, so "north" means north OF THIS COUNTRY rather than
    // north of some absolute number of pixels.
    double span = 1.0;
    for (const auto& [pid, pv] : m_provinces.getAllProvinces()) {
        if (pv.countryId != countryId) continue;
        auto c = m_provinceCenters.find(pid);
        if (c == m_provinceCenters.end()) continue;
        span = std::max(span, std::max(std::abs(c->second.x - cx / n),
                                       std::abs(c->second.y - cy / n)));
    }
    const double bar = span * 0.25;

    // ENGLISH, always: this is the canonical form that properName() translates
    // at draw time. Calling T() here is what froze the old names into whichever
    // language built them.
    //
    // `forceDirection` drops the bar. The bar exists so that a district sitting
    // in the middle of its country is called "Britain" rather than "Slightly
    // Eastern Britain" -- but when that name is already taken, a direction that
    // is merely the strongest of four beats a Roman numeral. Measured on the
    // British Empire, whose districts are scattered across every continent: all
    // three came out under the bar, and the list read "Britain", "Britain II",
    // "Britain III".
    const char* dir = nullptr;
    if (std::abs(ex) > std::abs(ey)) {
        if (ex >  bar) dir = "Eastern";
        else if (ex < -bar) dir = "Western";
        else if (forceDirection) dir = (ex >= 0) ? "Eastern" : "Western";
    } else {
        if (ey >  bar) dir = "Southern";     // y grows downward on the raster
        else if (ey < -bar) dir = "Northern";
        else if (forceDirection) dir = (ey >= 0) ? "Southern" : "Northern";
    }
    if (!dir) return place;                  // the middle of the country IS the place
    return std::string(dir) + " " + place;
}

// What to draw for a district.
//
// The stored name is canonical English and goes through properName(), which is
// the same path a country name takes -- so "Eastern Ukraine" is "Східна
// Україна" in Ukrainian and "Ost-Ukraine" in German, and it CHANGES when the
// player changes language, which the old baked strings did not.
//
// A name the player or a mapmaker typed is theirs. It is drawn exactly as
// written: re-rendering it would transliterate somebody's deliberate choice
// into whatever script is loaded.
std::string Game::districtDisplayName(const District& d) const {
    if (d.customName) return d.name;
    // An unnamed district has no ground to be named after yet -- see the paint
    // handler. Numbered at DRAW time so the number follows the language too.
    if (d.name.empty()) return std::string(TextFormat(T("District %d"), d.id));

    // THE NUMERAL COMES OFF FIRST. properName transliterates anything it does
    // not recognise, and " II" is not a name -- it came back as "ii" in
    // Cyrillic, which is neither Roman nor a number. It is put back afterwards,
    // untouched, because a Roman numeral is the same in every language the game
    // draws.
    std::string base = d.name, numeral;
    // i18n-ignore
    for (const char* n : {" II", " III", " IV", " V", " VI", " VII", " VIII", " IX", " X"}) {
        const size_t len = std::strlen(n);
        if (base.size() > len && base.compare(base.size() - len, len, n) == 0) {
            numeral = n;
            base.resize(base.size() - len);
            break;
        }
    }
    return od::i18n::properName(base) + numeral;
}

std::string Game::uniqueDistrictName(int countryId, const std::string& base,
                                     int skipIndex,
                                     const std::vector<int>& provinces) const {
    auto it = m_districts.find(countryId);
    if (it == m_districts.end()) return base;
    auto taken = [&](const std::string& n) {
        for (size_t i = 0; i < it->second.size(); ++i)
            if ((int)i != skipIndex && it->second[i].name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    // EVERY DIRECTION BEFORE A NUMERAL. "Western Britain" says where the
    // district is; "Britain II" says only that somebody got there first. The
    // district's own bearing is tried first, then the rest, so the name that
    // fits best is the one it gets when it can.
    if (!provinces.empty()) {
        const std::string place = districtPlaceName(countryId, provinces);
        if (!place.empty()) {
            std::vector<std::string> tries;
            tries.push_back(suggestDistrictName(countryId, provinces, true));
            // i18n-ignore: canonical English, translated by properName at draw
            for (const char* d : {"Northern", "Southern", "Eastern", "Western", "Central"})
                tries.push_back(std::string(d) + " " + place);
            for (const std::string& t : tries)
                if (t != base && !taken(t)) return t;
        }
    }
    // Roman, because these are place names and "Mongol County 2" reads like a
    // spreadsheet. Bounded so a pathological country cannot spin here.
    //
    // A Roman numeral is a Roman numeral in every language the game draws, and
    // offering " VIII" to twenty translators invites twenty answers to a
    // question that has one.
    // i18n-ignore
    static const char* const kNumerals[] = {" II", " III", " IV", " V", " VI",
                                            " VII", " VIII", " IX", " X"};
    for (const char* n : kNumerals) {
        const std::string cand = base + n;
        if (!taken(cand)) return cand;
    }
    return base;
}

// ── THE DISTRICT MAP, PAINTED FROM THE COUNTRY RATHER THAN FROM THE WORLD ──
//
// This used to clear and repaint a full-map RGBA buffer -- 8192x4096, 134 MB --
// and upload all of it, every time a district changed. Measured: 58 ms per
// repaint, plus 68 ms the first time for ensureProvincePixels(), which builds a
// province-to-pixels index over all 33 million pixels and costs 128 MB to hold.
// So opening the tab stalled for an eighth of a second and every stroke of the
// paint brush cost another 58 ms.
//
// None of that index was needed. m_countryPixels[cid] is built during load and
// already lists every pixel a country owns -- about a million for a large one,
// thirty times less than the map -- and the only pixels this overlay ever
// touches are that country's. So: clear what was painted (the same set), paint
// it again, and upload only the rectangle it occupies.
void Game::rebuildDistrictOverlay(int cid, int texW, int texH) {
    if (texW <= 0 || texH <= 0 || cid <= 0) return;
    const bool sized = m_districtOverlayBuf.size() ==
                       (size_t)std::max(1, texW / DISTRICT_OVERLAY_DIV) *
                       (size_t)std::max(1, texH / DISTRICT_OVERLAY_DIV);
    if (!m_districtOverlayDirty && sized && m_districtOverlayCid == cid) return;
    if (cid >= (int)m_countryPixels.size()) return;

    const auto t0 = std::chrono::steady_clock::now();
    m_districtOverlayDirty = false;

    // ── AND AT HALF RESOLUTION ──
    //
    // The overlay is drawn into a panel a few hundred pixels wide, scaled down
    // from 8192x4096 by a factor of twenty even before the zoom. Holding it at
    // full resolution cost 134 MB to clear, to copy and to upload, and bought
    // detail nothing could see. Half in each direction is a quarter of all
    // three, and the draw compensates by halving the source rectangle.
    const int ovW = std::max(1, texW / DISTRICT_OVERLAY_DIV);
    const int ovH = std::max(1, texH / DISTRICT_OVERLAY_DIV);

    // pid -> colour. A FLAT VECTOR, not a hash: the loop below runs two million
    // times on a large country and province ids are small and dense.
    std::vector<Color> colByPid;
    auto dIt = m_districts.find(cid);
    if (dIt != m_districts.end()) {
        int maxPid = 0;
        for (const District& d : dIt->second)
            for (int pid : d.provinces) maxPid = std::max(maxPid, pid);
        colByPid.assign((size_t)maxPid + 1, Color{0, 0, 0, 0});
        for (size_t di = 0; di < dIt->second.size(); ++di) {
            const District& d = dIt->second[di];
            // OPAQUE, NOT A TINT. Half-lit district colours blended into the
            // political map underneath and came out as neither: a measured zero
            // pixels on screen matched the colour in the list, which is the one
            // thing this map has to make findable. The selected district is
            // brighter still, so "which am I editing" is answerable from the
            // map and not only from the list.
            const unsigned char a =
                ((int)di == m_districtSel && cid == m_playerCountryId) ? 255 : 232;
            const Color col{d.r, d.g, d.b, a};
            for (int pid : d.provinces)
                if (pid >= 0 && pid < (int)colByPid.size()) colByPid[(size_t)pid] = col;
        }
    }

    if (!sized) m_districtOverlayBuf.assign((size_t)ovW * ovH, Color{0, 0, 0, 0});

    const Image& provImg = m_provinces.getImage();
    const Color* prov = (const Color*)provImg.data;
    const std::vector<int>& own = m_countryPixels[cid];
    int x0 = ovW, y0 = ovH, x1 = -1, y1 = -1;
    for (int idx : own) {
        if (idx < 0 || idx >= texW * texH) continue;
        Color c{0, 0, 0, 0};
        if (prov) {
            const int pid = Province::colorToId(prov[idx].r, prov[idx].g, prov[idx].b);
            if (pid >= 0 && pid < (int)colByPid.size()) c = colByPid[(size_t)pid];
        }
        const int x = (idx % texW) / DISTRICT_OVERLAY_DIV;
        const int y = (idx / texW) / DISTRICT_OVERLAY_DIV;
        if (x >= ovW || y >= ovH) continue;
        m_districtOverlayBuf[(size_t)y * ovW + x] = c;   // clears and paints in one pass
        x0 = std::min(x0, x); x1 = std::max(x1, x);
        y0 = std::min(y0, y); y1 = std::max(y1, y);
    }

    // A country whose ground moved since the last build can leave colour behind
    // outside the new bounding box; a full clear is only needed when the country
    // being shown changes, which is rare.
    if (m_districtOverlayCid != cid && m_districtOverlayCid > 0 && sized) {
        if (m_districtOverlayCid < (int)m_countryPixels.size())
            for (int idx : m_countryPixels[m_districtOverlayCid]) {
                if (idx < 0 || idx >= texW * texH) continue;
                const int x = (idx % texW) / DISTRICT_OVERLAY_DIV;
                const int y = (idx / texW) / DISTRICT_OVERLAY_DIV;
                if (x < ovW && y < ovH) m_districtOverlayBuf[(size_t)y * ovW + x] = Color{0, 0, 0, 0};
            }
        x0 = 0; y0 = 0; x1 = ovW - 1; y1 = ovH - 1;   // upload everything once
    }
    m_districtOverlayCid = cid;

    if (m_districtOverlayTex.id == 0 || m_districtOverlayTex.width != ovW ||
        m_districtOverlayTex.height != ovH) {
        if (m_districtOverlayTex.id > 0) UnloadTexture(m_districtOverlayTex);
        Image img{};
        img.data = m_districtOverlayBuf.data();
        img.width = ovW; img.height = ovH; img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        m_districtOverlayTex = LoadTextureFromImage(img);
    } else if (x1 >= x0 && y1 >= y0) {
        // Only the rectangle the country occupies. Uploading the whole texture
        // is 134 MB across the bus for a change that touched Belgium.
        const int rw = x1 - x0 + 1, rh = y1 - y0 + 1;
        std::vector<Color> rect((size_t)rw * rh);
        for (int row = 0; row < rh; ++row)
            memcpy(&rect[(size_t)row * rw],
                   &m_districtOverlayBuf[(size_t)(y0 + row) * ovW + x0],
                   (size_t)rw * sizeof(Color));
        UpdateTextureRec(m_districtOverlayTex,
                         {(float)x0, (float)y0, (float)rw, (float)rh}, rect.data());
    }

    if (getenv("OD_TIME_PIXELS"))
        printf("[TIME] district overlay: %.1f ms, %zu country pixels, rect %dx%d\n",
               std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - t0).count(),
               own.size(), (x1 >= x0) ? x1 - x0 + 1 : 0, (y1 >= y0) ? y1 - y0 + 1 : 0);
}

// ─── Country profile ────────────────────────────────────────────────────────

std::string Game::countryAgeText(int cid) const {
    const Country* c = m_countries.getCountry(cid);
    if (!c) return std::string();
    // -1 means the map already had it. That is not the same as "founded on turn
    // zero" and must not be printed as a date: this game does not know when
    // France began, only that it was there when the world was made.
    if (c->foundedTurn < 0) return std::string(T("Existing since the start of the session"));

    const int months = std::max(0, m_turnNumber - c->foundedTurn);
    // A turn is a month. Said in the largest unit that still carries meaning --
    // "38 months" is a number, "3 years, 2 months" is an age.
    if (months >= 12000) {
        const int mil = months / 12000, yrs = (months % 12000) / 12;
        return std::string(TextFormat(T("%d millennia, %d years"), mil, yrs));
    }
    if (months >= 12) {
        const int yrs = months / 12, rem = months % 12;
        if (rem == 0) return std::string(TextFormat(T("%d year(s)"), yrs));
        return std::string(TextFormat(T("%d year(s), %d month(s)"), yrs, rem));
    }
    return std::string(TextFormat(T("%d month(s)"), months));
}

float Game::disclosureAppeal(int cid) const {
    auto dIt = m_countryDisclosure.find(cid);
    if (dIt == m_countryDisclosure.end()) return 0.0f;
    return disclosureAppealFor(cid, dIt->second);
}

float Game::disclosureAppealFor(int cid, unsigned bits) const {
    if (bits == 0) return 0.0f;

    auto hIt = m_incomeHistory.find(cid);
    if (hIt == m_incomeHistory.end() || hIt->second.empty()) return 0.0f;
    const CountryIncomeSnapshot& now = hIt->second.back();

    // ── THE FIGURES, NOT THE ACT OF PUBLISHING ──
    //
    // Opening the books is worth nothing on its own; what draws people is what
    // the books SAY. A country that publishes a deficit has published a
    // deficit, and gets nothing for it -- which is what makes the decision a
    // decision rather than a free bonus everyone takes on turn one.
    float appeal = 0.0f;
    if ((bits & DISCLOSE_EXPENSES) && now.total > 0.0f) {
        // Spending a smaller share of income on the army and the police reads
        // as a country that is somewhere to live rather than somewhere to be
        // conscripted.
        const float hard = now.armyExpenses + now.navyExpenses + now.pacificationCost;
        const float share = hard / std::max(1.0f, now.total);
        if (share < HARD_SPEND_BAR)
            appeal += (HARD_SPEND_BAR - share) / HARD_SPEND_BAR * DISCLOSE_EXPENSES_MAX;
    }
    if (bits & DISCLOSE_DOCTRINES) {
        // Doctrines that reduce unrest are the ones a prospective migrant reads
        // as "this place is governed", and they are already the ones the game
        // measures per province.
        float calm = 0.0f;
        auto apIt = m_countryActivePolicyIndices.find(cid);
        if (apIt != m_countryActivePolicyIndices.end())
            for (int idx : apIt->second) {
                if (idx < 0 || idx >= (int)m_activePolicies.size()) continue;
                if (m_activePolicies[idx].turnsRemaining != 0) continue;
                for (const auto& p : m_allPolicies)
                    if (p.id == m_activePolicies[idx].policyId) {
                        calm += policyUnrestPct(p) * 0.02f;
                        break;
                    }
            }
        appeal += std::min(calm, DISCLOSE_DOCTRINES_MAX);
    }
    if (bits & DISCLOSE_DISTRICT_LAWS) {
        // ── WHAT THE LAWS SAY ABOUT LIVING THERE ──
        //
        // A curfew and a grain requisition are public knowledge a government
        // would rather not publish; language rights in the courts and a
        // settlement grant are the opposite. Read off the SAME two numbers the
        // resolver uses -- what the law does to unrest and to growth -- so a
        // law that is edited in data/district_laws.json changes what publishing
        // it is worth, with nothing here to keep in step.
        float good = 0.0f;
        auto dIt = m_districts.find(cid);
        if (dIt != m_districts.end() && !dIt->second.empty()) {
            for (const District& d : dIt->second)
                for (const std::string& pid : d.policies)
                    for (const DistrictLaw& dl : m_districtLaws) {
                        if (dl.id != pid) continue;
                        good += std::max(0.0f, -dl.unrestPct) * 0.002f;
                        good += std::max(0.0f, dl.growthPct) * 0.002f;
                        good -= std::max(0.0f, dl.unrestPct) * 0.002f;
                        break;
                    }
            good /= (float)dIt->second.size();   // an average district, not a total
        }
        appeal += std::clamp(good, 0.0f, DISCLOSE_DISTRICTS_MAX);
    }
    if (bits & DISCLOSE_TREASURY) {
        auto tIt = m_treasuryLastTurn.find(cid);
        const double held = (tIt == m_treasuryLastTurn.end()) ? 0.0 : tIt->second;
        // A solvent state, in the units its own income is measured in: ten
        // turns of income in hand is a country that pays its bills.
        if (held > 0.0 && now.total > 0.0f)
            appeal += (float)std::min((double)DISCLOSE_TREASURY_MAX,
                                      held / (double)(now.total * 100.0f));
    }
    // ── AND A CEILING, BECAUSE THIS IS A PULL AND NOT A POLICY ──
    //
    // The first version handed +50% to any country with ten turns of income in
    // the bank -- one tick box, one turn, half again as many migrants, for a
    // condition most solvent countries meet by default. Publishing should tilt
    // where people go, not decide it: the ceiling is a fifth, and reaching it
    // takes all three fields saying something good at once.
    //
    // WHICH IS ALSO WHY EACH FIELD HAS ITS OWN SHARE OF THAT CEILING. With the
    // expenses term free to reach 0.7 on its own, every country the AI reviewed
    // came out pinned at the cap -- 374 decisions, mean appeal 0.197 -- and a
    // pull every country gets in full is one that moves nobody anywhere. The
    // three maxima below sum to exactly the cap, so a country reaches it only
    // by being frugal, calm AND solvent at once, and the ordinary case lands
    // somewhere in between where it can be beaten.
    return std::clamp(appeal, 0.0f, DISCLOSURE_APPEAL_MAX);
}

// === updateAIDistrictLaws ===
//
// GOVERNING THE DISTRICTS IT DREW.
//
// An AI country cut itself into a troubled district and a quiet one and then
// did nothing else with them: the budget split was the whole of its use for the
// mechanic, and since its pacification budget is zero the split moved nothing.
// Regional law is the part of districts that does not need a budget to exist --
// a curfew costs money per province and changes unrest whether or not anybody
// is paying for suppression.
//
// PRICED AS A SHARE OF GROSS INCOME, NOT OUT OF WHAT IS SPARE. That distinction
// is the whole reason the pacification reflex beside this one never fires: an
// AI country's net income is about zero as a matter of course, so any rule
// gated on cash in hand is a prohibition dressed up as a budget. The bill lands
// in cs.policyCosts, which is an expense the resolver already quotes, so the
// country pays for it the way it pays for a doctrine.
void Game::updateAIDistrictLaws(int countryId) {
    if (m_districtLaws.empty()) return;
    auto dIt = m_districts.find(countryId);
    if (dIt == m_districts.end() || dIt->second.empty()) return;
    auto& ds = dIt->second;

    const CountryIncomeSnapshot cs = computeCountryIncome(countryId);
    if (cs.total <= 0.0f) return;
    const float ceiling = cs.total * AI_DLAW_MAX_SHARE;

    // What it is already committed to, so a second law is judged against the
    // first rather than against nothing.
    const float already = districtPolicyCost(countryId);

    for (District& d : ds) {
        if (d.provinces.empty()) continue;
        float risk = 0.0f;
        for (int pid : d.provinces) risk += getProvinceRebellionChance(pid, countryId);
        risk /= (float)d.provinces.size();

        // ── DROP WHAT IT CANNOT AFFORD, OR NO LONGER NEEDS ──
        if (!d.policies.empty()) {
            const bool calm = risk < AI_DLAW_RISK_BAR * 0.5f;
            if (calm || already > ceiling) {
                d.policies.clear();
                if (getenv("OD_DISTRICT_DEBUG"))
                    printf("[AIDLAW] turn %d cid=%d \"%s\" repealed (risk %.2f bill %.1f/%.1f)\n",
                           m_turnNumber, countryId, d.name.c_str(), (double)risk,
                           (double)already, (double)ceiling);
            }
            continue;
        }
        if (risk < AI_DLAW_RISK_BAR) continue;

        // ── THE MOST CALM PER COIN IT CAN AFFORD ──
        //
        // Judged on the resolver's own two numbers, so a law edited in
        // data/district_laws.json is judged differently here without anything
        // being kept in step by hand. A law that raises unrest is never chosen
        // for a district that is already in trouble, whatever it pays.
        // What this district earns, because a law's real price is not only its
        // bill. The first version divided calm by the bill alone and every AI
        // country in the world passed the same law: a tax holiday costs 0.00
        // per province in cash and 10% of the district's industry income, so it
        // looked free and won everywhere, 45 times out of 45. Income forgone is
        // money spent.
        float districtIncome = 0.0f;
        for (int pid : d.provinces) {
            auto iIt = m_provinceIndustry.find(pid);
            districtIncome += provinceIndustryIncome(
                pid, iIt == m_provinceIndustry.end() ? 0 : iIt->second.level);
        }

        const DistrictLaw* best = nullptr;
        float bestValue = 0.0f;
        for (const DistrictLaw& l : m_districtLaws) {
            if (l.unrestPct >= 0.0f) continue;                 // makes it worse
            const float bill = l.costPerTurn * (float)d.provinces.size();
            if (already + bill > ceiling) continue;            // cannot carry it
            const float forgone = std::max(0.0f, -l.incomePct) * 0.01f * districtIncome;
            const float price = bill + forgone;
            // Calm per coin, counting money the law hands back as a discount.
            const float gained = std::max(0.0f, l.incomePct) * 0.01f * districtIncome;
            const float value = (-l.unrestPct) / std::max(0.5f, price - gained);
            if (value > bestValue) { bestValue = value; best = &l; }
        }
        if (!best) continue;
        d.policies.push_back(best->id);
        if (getenv("OD_DISTRICT_DEBUG"))
            printf("[AIDLAW] turn %d cid=%d \"%s\" passes %s (risk %.2f bill %.1f/%.1f)\n",
                   m_turnNumber, countryId, d.name.c_str(), best->id.c_str(), (double)risk,
                   (double)(best->costPerTurn * d.provinces.size()), (double)ceiling);
        return;   // one law per review, so a country does not legislate in bulk
    }
}

// === updateAIDisclosure ===
//
// WHAT A COUNTRY IS WILLING TO SAY ABOUT ITSELF.
//
// The player gets three tick boxes and a sentence explaining that good figures
// draw migrants. An AI country needs the same decision, and the temptation is
// to write it a rule of its own -- "publish if rich", "publish if at peace".
// That is a second copy of a judgement the game already makes, and the two
// would drift apart the first time the appeal formula changed.
//
// So it asks the formula. A field goes out if publishing THAT field, on its
// own, is worth something to this country right now; it stays in if the
// figures behind it say nothing good. The country that spends four fifths of
// its income on soldiers keeps its books shut, and the same country keeps them
// shut for exactly as long as that is true.
void Game::updateAIDisclosure(int countryId) {
    if (countryId == m_playerCountryId) return;
    if (((m_turnNumber + countryId) % AI_DISCLOSURE_REVIEW_TURNS) != 0) return;

    unsigned bits = 0;
    for (unsigned bit : {(unsigned)DISCLOSE_EXPENSES,
                         (unsigned)DISCLOSE_DOCTRINES,
                         (unsigned)DISCLOSE_TREASURY,
                         (unsigned)DISCLOSE_DISTRICT_LAWS})
        if (disclosureAppealFor(countryId, bit) > 0.0f) bits |= bit;

    if (bits == 0) m_countryDisclosure.erase(countryId);
    else           m_countryDisclosure[countryId] = bits;
    if (getenv("OD_DISCLOSE_STATS"))
        fprintf(stderr, "[DISCLOSE] turn %d cid %d bits %u appeal %.3f\n",
                m_turnNumber, countryId, bits, disclosureAppeal(countryId));
}

void Game::releaseProfileFlags() {
    for (Texture2D& t : m_profileFlagTex) if (t.id > 0) UnloadTexture(t);
    m_profileFlagTex.clear();
    m_profileFlagCid = -1;
}

void Game::updateCountryProfile() {
    if (IsKeyPressed(KEY_ESCAPE)) {
        m_inCountryProfile = false;
        m_profileScroll = 0;
        releaseProfileFlags();      // the textures go with the screen
    }
    const float wheel = GetMouseWheelMove();
    // Clamped to what is actually there, in both directions. Unbounded below,
    // the wheel scrolled the whole profile off the top into empty space; and
    // the page grew past one screen the moment it gained a map and a doctrine
    // list, so "there is nothing to scroll" stopped being true.
    if (wheel != 0.0f) {
        const int maxScroll = std::max(0, m_profileContentH - m_screenH + 40);
        m_profileScroll = std::clamp(m_profileScroll - (int)(wheel * 60), 0, maxScroll);
    }
}

void Game::drawCountryProfile() {
    const Country* c = m_countries.getCountry(m_profileCountryId);
    if (!c) { m_inCountryProfile = false; return; }
    const int cid = m_profileCountryId;
    const bool mine = (cid == m_playerCountryId);
    const Vector2 mouse = getMouse();
    const bool click = !m_paused && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);

    DrawRectangle(0, 0, m_screenW, m_screenH, Color{8, 9, 13, 248});

    const int L = 48;
    int y = 36 - m_profileScroll;

    // ── Who, and for how long ──
    auto fit = m_countryFlags.find(cid);
    if (fit != m_countryFlags.end() && fit->second.id > 0) {
        const float fw = 96.0f, fh = fw * 2.0f / 3.0f;
        DrawTexturePro(fit->second, {0, 0, (float)fit->second.width, (float)fit->second.height},
                       {(float)L, (float)y, fw, fh}, {0, 0}, 0, WHITE);
        DrawRectangleLines(L, y, (int)fw, (int)fh, Color{80, 84, 104, 200});
    }
    DrawText(od::i18n::properName(c->name).c_str(), L + 116, y + 4, 30, WHITE);
    DrawText(countryAgeText(cid).c_str(), L + 116, y + 40, 14, Color{160, 166, 186, 255});
    // ── A WAY OUT THAT IS NOT A KEYBOARD SHORTCUT ──
    //
    // "ESC to close" in grey type at the top of a full-screen panel is a note,
    // not a control: it tells a player who is already looking for the exit
    // where to press, and does nothing for one who reaches for the mouse. This
    // screen is opened by clicking, from a panel closed by clicking.
    {
        const int bw = 96, bh = 30;
        const Rectangle close = {(float)(m_screenW - bw - 24), 24.0f, (float)bw, (float)bh};
        const bool hov = CheckCollisionPointRec(mouse, close);
        DrawRectangleRounded(close, 0.2f, 6, hov ? Color{58, 34, 40, 240} : Color{26, 28, 36, 220});
        DrawRectangleRoundedLines(close, 0.2f, 6,
                                  hov ? Color{210, 110, 120, 230} : Color{78, 82, 102, 200});
        const char* lbl = T("Close");
        DrawText(lbl, (int)(close.x + close.width / 2 - MeasureText(lbl, 14) / 2),
                 (int)(close.y + 8), 14, hov ? WHITE : Color{190, 195, 214, 255});
        DrawText(T("ESC"), (int)close.x - 34, (int)close.y + 9, 12, Color{110, 114, 132, 160});
        if (hov && click) {
            m_inCountryProfile = false;
            Audio::get().playSfx("panel_close", 0.45f);
            return;                        // nothing below should draw a dead frame
        }
    }
    y += 84;

    // Everything below reads one history: what this country was, per turn.
    auto hIt = m_incomeHistory.find(cid);
    const bool haveHist = hIt != m_incomeHistory.end() && hIt->second.size() >= 2;

    const int colW = std::min(420, (m_screenW - L * 2 - 40) / 2);
    const int gh = 96;
    auto plot = [&](int gx, int gy, const char* title, const std::vector<float>& v,
                    Color col, const char* nowText) {
        DrawText(T(title), gx, gy, 14, WHITE);
        if (nowText) DrawText(nowText, gx + 220, gy + 1, 13, col);
        gy += 18;
        DrawRectangle(gx, gy, colW, gh, Color{14, 16, 22, 190});
        DrawRectangleLines(gx, gy, colW, gh, Color{62, 66, 86, 170});
        if (v.size() >= 2) {
            float lo = v[0], hi = v[0];
            for (float f : v) { lo = std::min(lo, f); hi = std::max(hi, f); }
            if (hi <= lo) hi = lo + 1.0f;
            for (size_t i = 1; i < v.size(); ++i) {
                const int x1 = gx + (int)((i - 1) * colW / (float)(v.size() - 1));
                const int x2 = gx + (int)(i * colW / (float)(v.size() - 1));
                const int y1 = gy + gh - 3 - (int)((v[i - 1] - lo) / (hi - lo) * (gh - 6));
                const int y2 = gy + gh - 3 - (int)((v[i] - lo) / (hi - lo) * (gh - 6));
                DrawLineEx({(float)x1, (float)y1}, {(float)x2, (float)y2}, 2.0f, col);
            }
        } else {
            DrawText(T("(not enough turns yet)"), gx + 8, gy + gh / 2 - 6, 12,
                     Color{110, 114, 132, 255});
        }
        return gy + gh + 14;
    };

    std::vector<float> popV, inV, outV;
    if (haveHist)
        for (const auto& h : hIt->second) {
            popV.push_back((float)h.population);
            inV.push_back(h.total);
            outV.push_back(h.expenses);
        }

    const int rightX = L + colW + 40;
    int leftY = y, rightY = y;
    const long long popNow = haveHist ? hIt->second.back().population : 0;
    leftY = plot(L, leftY, "Population", popV, Color{130, 190, 230, 255},
                 popNow > 0 ? formatPop(popNow).c_str() : nullptr);
    rightY = plot(rightX, rightY, "Gross Income", inV, SKYBLUE,
                  haveHist ? TextFormat("%.1f", hIt->second.back().total) : nullptr);
    leftY = plot(L, leftY, "Expenses", outV, Color{210, 90, 90, 255},
                 haveHist ? TextFormat("%.1f", hIt->second.back().expenses) : nullptr);

    // ── Where its government stands ──
    {
        DrawText(T("Political Compass"), rightX, rightY, 14, WHITE);
        rightY += 18;
        const int s = gh;
        const int bx = rightX, by = rightY;
        DrawRectangle(bx, by, s, s, Color{14, 16, 22, 190});
        DrawLine(bx, by + s / 2, bx + s, by + s / 2, Color{58, 62, 80, 220});
        DrawLine(bx + s / 2, by, bx + s / 2, by + s, Color{58, 62, 80, 220});
        DrawRectangleLines(bx, by, s, s, Color{62, 66, 86, 170});
        auto cIt = m_countryCompass.find(cid);
        if (cIt != m_countryCompass.end()) {
            const float ex = std::clamp(cIt->second.economic, -100.0f, 100.0f);
            const float sy = std::clamp(cIt->second.social, -100.0f, 100.0f);
            const int px = bx + s / 2 + (int)(ex / 100.0f * (s / 2 - 4));
            const int py = by + s / 2 - (int)(sy / 100.0f * (s / 2 - 4));
            DrawCircle(px, py, 5, hexToColor(m_config.accent()));
            DrawCircleLines(px, py, 5, WHITE);
            DrawText(TextFormat("%+.0f / %+.0f", ex, sy), bx + s + 10, by + s / 2 - 6, 12,
                     Color{170, 176, 196, 255});
        }
        rightY = by + s + 14;
    }

    // ── What it has flown ──
    //
    // Rendered ONCE, when the profile opens on this country. A FlagPattern is
    // an image composite and a texture upload; doing six of those every frame
    // to draw a strip 64 px tall is the kind of thing that is invisible until
    // somebody leaves the panel open.
    if (m_profileFlagCid != cid) {
        releaseProfileFlags();
        m_profileFlagCid = cid;
        for (const auto& era : c->flagHistory)
            m_profileFlagTex.push_back(
                FlagRenderer::render(era.flag, 128, 84, m_dataDir, &m_odmJsonData));
    }
    if (!c->flagHistory.empty()) {
        DrawText(T("Flags"), L, leftY, 14, WHITE);
        leftY += 18;
        int fx = L;
        for (size_t i = 0; i < c->flagHistory.size() && i < m_profileFlagTex.size(); ++i) {
            if (fx + 74 > L + colW) break;
            const Texture2D& t = m_profileFlagTex[i];
            if (t.id > 0)
                DrawTexturePro(t, {0, 0, (float)t.width, (float)t.height},
                               {(float)fx, (float)leftY, 64.0f, 42.0f}, {0, 0}, 0, WHITE);
            DrawRectangleLines(fx, leftY, 64, 42, Color{80, 84, 104, 200});
            DrawText(TextFormat("%d", c->flagHistory[i].turn), fx, leftY + 44, 10,
                     Color{130, 136, 156, 255});
            fx += 74;
        }
        leftY += 62;
    }

    // ── HOW IT IS DIVIDED, WHICH IS NOT A SECRET ──
    //
    // A country's districts are drawn on the map and their borders are visible
    // to anybody looking at it, so the division and the budget split are public
    // here without being asked for. What each district DOES with its share --
    // the laws it runs -- is a separate question, and a publishable one.
    {
        ensureDefaultDistrict(cid);
        auto dIt = m_districts.find(cid);
        if (dIt != m_districts.end() && !dIt->second.empty()) {
            const auto& ds = dIt->second;
            int shareTotal = 0;
            for (const District& d : ds) shareTotal += std::max(0, d.sharePct);
            const bool lawsPublic = mine || discloses(cid, DISCLOSE_DISTRICT_LAWS);

            int dy = std::max(leftY, rightY) + 6;
            DrawText(TextFormat(T("Districts (%d)"), (int)ds.size()), L, dy, 16,
                     hexToColor(m_config.accent()));
            dy += 24;

            // ── THE DIVISION, DRAWN ──
            //
            // A list of names and percentages says how a country is cut up
            // without saying WHERE, which is the half that matters: the same
            // three rows describe a country split east-to-west and one split
            // between a heartland and an island chain. The Districts tab has
            // shown this map since the mechanic existed; a profile that reports
            // the division in words alone was the odd one out.
            //
            // Framed on the country rather than showing the whole world: this
            // is a picture of one country's insides, and at world scale a
            // European power is forty pixels across.
            {
                const int texW = m_provinces.getWidth(), texH = m_provinces.getHeight();
                const int mapW = colW * 2 + 40, mapH = 150;
                const Rectangle dst = {(float)L, (float)dy, (float)mapW, (float)mapH};
                DrawRectangleRec(dst, Color{10, 12, 18, 255});
                rebuildDistrictOverlay(cid, texW, texH);
                if (m_politicalTex.id > 0 && texW > 0 && texH > 0) {
                    // The country's own bounding box, with a margin, so the
                    // frame is filled by the country and not by its ocean.
                    float bx0 = (float)texW, by0 = (float)texH, bx1 = 0, by1 = 0;
                    for (const District& d : ds)
                        for (int pid : d.provinces) {
                            auto pc = m_provinceCenters.find(pid);
                            if (pc == m_provinceCenters.end()) continue;
                            bx0 = std::min(bx0, pc->second.x); bx1 = std::max(bx1, pc->second.x);
                            by0 = std::min(by0, pc->second.y); by1 = std::max(by1, pc->second.y);
                        }
                    if (bx1 > bx0 && by1 > by0) {
                        const float pad = std::max(40.0f, (bx1 - bx0) * 0.12f);
                        bx0 -= pad; bx1 += pad; by0 -= pad; by1 += pad;
                        // Matched to the frame's aspect, or the country comes
                        // out stretched.
                        float sw = bx1 - bx0, sh = by1 - by0;
                        const float want = (float)mapW / (float)mapH;
                        if (sw / sh > want) sh = sw / want; else sw = sh * want;
                        // CLAMPED TO THE TEXTURE. A globe-spanning empire's
                        // bounding box is the whole map, and the aspect fit then
                        // pushes the source rectangle past its edges -- which
                        // does not clip, it TILES: the first shot of this drew
                        // the world three times side by side.
                        sw = std::min(sw, (float)texW);
                        sh = std::min(sh, (float)texH);
                        const float cxm = (bx0 + bx1) * 0.5f, cym = (by0 + by1) * 0.5f;
                        const Rectangle src = {
                            std::clamp(cxm - sw * 0.5f, 0.0f, (float)texW - sw),
                            std::clamp(cym - sh * 0.5f, 0.0f, (float)texH - sh), sw, sh};
                        BeginScissorMode((int)dst.x, (int)dst.y, (int)dst.width, (int)dst.height);
                        DrawTexturePro(m_politicalTex, src, dst, {0, 0}, 0, WHITE);
                        const Rectangle ovSrc = {src.x / DISTRICT_OVERLAY_DIV,
                                                 src.y / DISTRICT_OVERLAY_DIV,
                                                 src.width / DISTRICT_OVERLAY_DIV,
                                                 src.height / DISTRICT_OVERLAY_DIV};
                        DrawTexturePro(m_districtOverlayTex, ovSrc, dst, {0, 0}, 0, WHITE);
                        EndScissorMode();
                    }
                }
                DrawRectangleLinesEx(dst, 1, Color{62, 66, 86, 170});
                dy += mapH + 10;
            }
            const int rowW = colW * 2 + 40;
            // A profile is a summary. Eleven districts is a real number for a
            // large country and the full list crowds out everything below it;
            // the Districts tab is where all of them live.
            size_t shown = 0;
            for (const District& d : ds) {
                if (shown++ >= PROFILE_DISTRICT_ROWS) {
                    DrawText(TextFormat(T("+%d more"), (int)(ds.size() - PROFILE_DISTRICT_ROWS)),
                             L + 12, dy, 11, Color{130, 136, 156, 255});
                    dy += 16;
                    break;
                }
                const float share = shareTotal > 0
                                  ? (float)std::max(0, d.sharePct) / (float)shareTotal : 0.0f;
                DrawRectangle(L, dy, rowW, 24, Color{16, 18, 25, 190});
                // The budget split, as a bar the length of the share. Reading
                // three numbers is work; reading three bars is a glance.
                DrawRectangle(L, dy, (int)(rowW * share), 24, Color{d.r, d.g, d.b, 70});
                DrawRectangle(L, dy, 4, 24, Color{d.r, d.g, d.b, 255});
                DrawText(districtDisplayName(d).c_str(), L + 12, dy + 5, 13, WHITE);
                const char* fig = TextFormat(T("%d%% of the budget  %d province(s)"),
                                             (int)std::lround(share * 100.0f),
                                             (int)d.provinces.size());
                DrawText(fig, L + rowW - MeasureText(fig, 12) - 12, dy + 6, 12,
                         Color{160, 166, 186, 255});
                dy += 26;

                // Its laws, when they are anybody's business.
                if (!lawsPublic) continue;
                if (d.policies.empty()) {
                    DrawText(T("no regional law"), L + 16, dy, 11, Color{104, 108, 126, 255});
                    dy += 16;
                    continue;
                }
                for (const std::string& pid : d.policies) {
                    std::string label = pid;
                    for (const DistrictLaw& dl : m_districtLaws)
                        if (dl.id == pid) { label = T(dl.name.c_str()); break; }
                    DrawText(TextFormat("- %s", label.c_str()), L + 16, dy, 11,
                             Color{150, 175, 195, 255});
                    dy += 16;
                }
            }
            if (!lawsPublic) {
                DrawText(T("This country does not publish how its districts are governed."),
                         L, dy, 11, Color{110, 114, 132, 255});
                dy += 18;
            }
            leftY = rightY = dy + 4;
        }
    }

    // ── And what it chooses to publish ──
    int py2 = std::max(leftY, rightY) + 6;
    DrawText(T("Published figures"), L, py2, 16, hexToColor(m_config.accent()));
    py2 += 22;
    if (mine)
        DrawText(T("What you publish, anyone can read -- and good figures draw people to you."),
                 L, py2, 11, Color{150, 154, 172, 255});
    py2 += 18;

    // ── TWO ARRAYS RATHER THAN ONE TABLE OF PAIRS ──
    //
    // The labels were fields of a struct, drawn through T() and therefore
    // translated at runtime -- but the extractor only reads plain `const char*
    // x[]` tables, so these three strings never reached en.json and T() had
    // nothing to look up. They rendered in English in every language, in the
    // middle of a screen that was otherwise translated. Kept as a bare table
    // so the extractor can see them.
    static const char* const kDiscloseLabels[] = {
        "Where the money goes",
        "Doctrines in force",
        "Treasury at the start of last turn",
        "How its districts are governed",
    };
    struct Row { unsigned bit; const char* label; };
    const Row rows[] = {
        {DISCLOSE_EXPENSES,       kDiscloseLabels[0]},
        {DISCLOSE_DOCTRINES,      kDiscloseLabels[1]},
        {DISCLOSE_TREASURY,       kDiscloseLabels[2]},
        {DISCLOSE_DISTRICT_LAWS,  kDiscloseLabels[3]},
    };
    for (const Row& r : rows) {
        const bool on = discloses(cid, r.bit);
        Rectangle box = {(float)L, (float)py2, (float)(colW * 2 + 40), 26};
        const bool hov = mine && CheckCollisionPointRec(mouse, box);
        DrawRectangleRounded(box, 0.18f, 5,
            on ? Color{28, 46, 36, 220} : hov ? Color{28, 32, 44, 210} : Color{18, 20, 27, 195});
        DrawRectangleRoundedLines(box, 0.18f, 5,
            on ? Color{86, 170, 106, 200} : Color{58, 62, 80, 165});
        DrawText(T(r.label), L + 12, py2 + 6, 13,
                 on ? WHITE : Color{150, 155, 172, 255});

        // The figure itself, when it is published. A profile that says a field
        // is disclosed and does not show it is worse than one that hides it.
        std::string value;
        if (on) {
            auto ih = m_incomeHistory.find(cid);
            const bool hh = ih != m_incomeHistory.end() && !ih->second.empty();
            if (r.bit == DISCLOSE_EXPENSES && hh) {
                // The wedges say the composition; the row says the total.
                value = TextFormat(T("%s per turn"),
                                   formatBalance(ih->second.back().expenses).c_str());
            } else if (r.bit == DISCLOSE_DISTRICT_LAWS) {
                int laws = 0;
                auto dl = m_districts.find(cid);
                if (dl != m_districts.end())
                    for (const District& d : dl->second) laws += (int)d.policies.size();
                value = TextFormat(T("%d law(s) across its districts"), laws);
            } else if (r.bit == DISCLOSE_DOCTRINES) {
                int count = 0;
                auto apIt = m_countryActivePolicyIndices.find(cid);
                if (apIt != m_countryActivePolicyIndices.end())
                    for (int idx : apIt->second)
                        if (idx >= 0 && idx < (int)m_activePolicies.size() &&
                            m_activePolicies[idx].turnsRemaining == 0) ++count;
                value = TextFormat(T("%d in force"), count);
            } else if (r.bit == DISCLOSE_TREASURY) {
                auto tIt = m_treasuryLastTurn.find(cid);
                value = TextFormat("$%s", formatBalance(
                    (float)(tIt == m_treasuryLastTurn.end() ? 0.0 : tIt->second)).c_str());
            }
        } else {
            value = mine ? T("not published") : T("this country does not publish this");
        }
        const int vw = MeasureText(value.c_str(), 12);
        DrawText(value.c_str(), (int)(box.x + box.width - vw - 12), py2 + 7, 12,
                 on ? Color{180, 215, 190, 255} : Color{110, 114, 132, 255});
        if (hov && click) {
            m_countryDisclosure[cid] ^= r.bit;
            Audio::get().playSfx("click_soft");
        }
        py2 += 30;

        // ── AND THE COMPOSITION AS A PIE, WHEN IT IS PUBLISHED ──
        //
        // "army 6 navy 120 doctrines 4 research 247" is four numbers to divide
        // in your head. The same wedges the economy screen draws answer the
        // question the field is actually asking -- what does this country spend
        // its money ON -- at a glance, and from the same table, so the two
        // charts cannot disagree.
        // ── WHICH DOCTRINES, NOT HOW MANY ──
        //
        // "3 in force" is a fact about a list rather than the list. A country
        // that publishes its doctrines is publishing WHAT it has enacted --
        // that is the whole content of the field, and a reader deciding whether
        // this is a place to move to needs the names. Listed the way a
        // district's regional laws are listed, and only when the country
        // publishes them.
        if (r.bit == DISCLOSE_DOCTRINES && on) {
            std::vector<std::string> names;
            auto apIt = m_countryActivePolicyIndices.find(cid);
            if (apIt != m_countryActivePolicyIndices.end())
                for (int idx : apIt->second) {
                    if (idx < 0 || idx >= (int)m_activePolicies.size()) continue;
                    if (m_activePolicies[idx].turnsRemaining != 0) continue;   // still arriving
                    for (const auto& p : m_allPolicies)
                        if (p.id == m_activePolicies[idx].policyId) {
                            names.push_back(T(p.name.c_str()));
                            break;
                        }
                }
            std::sort(names.begin(), names.end());
            // Two columns: a well-governed country can hold a dozen, and a
            // single column of them pushes everything below off the screen.
            const int colw = (colW * 2 + 40) / 2;
            for (size_t i = 0; i < names.size(); ++i) {
                const int cx = L + 16 + (int)(i % 2) * colw;
                const int cy = py2 + (int)(i / 2) * 15;
                DrawText(TextFormat("- %s", names[i].c_str()), cx, cy, 11,
                         Color{150, 175, 195, 255});
            }
            if (!names.empty()) py2 += (int)((names.size() + 1) / 2) * 15 + 6;
        }

        if (r.bit == DISCLOSE_EXPENSES && on) {
            auto ih2 = m_incomeHistory.find(cid);
            if (ih2 != m_incomeHistory.end() && !ih2->second.empty() &&
                ih2->second.back().expenses > 0.0f) {
                const CountryIncomeSnapshot& n = ih2->second.back();
                const std::vector<ExpenseSlice> slices = expenseSlices(n);
                const int radius = 46;
                const int cx = L + 20 + radius, cy = py2 + radius + 4;
                float a0 = 0.0f;
                for (const ExpenseSlice& sl : slices) {
                    if (sl.value <= 0.0f) continue;
                    const float sweep = sl.value / n.expenses * 360.0f;
                    DrawCircleSector({(float)cx, (float)cy}, (float)radius, a0, a0 + sweep, 40, sl.col);
                    a0 += sweep;
                }
                DrawCircleLines(cx, cy, (float)radius, Color{100, 100, 120, 150});
                int lx = cx + radius + 24, ly = py2 + 4;
                for (const ExpenseSlice& sl : slices) {
                    if (sl.value <= 0.0f) continue;
                    DrawRectangle(lx, ly, 10, 10, sl.col);
                    DrawText(TextFormat("%s: %.1f%%", sl.label.c_str(),
                                        sl.value / n.expenses * 100.0f),
                             lx + 14, ly - 1, 12, Color{170, 176, 196, 255});
                    ly += 15;
                }
                py2 = std::max(cy + radius, ly) + 10;
            }
        }
    }

    if (mine) {
        const float appeal = disclosureAppeal(cid);
        DrawText(TextFormat(T("Migrants drawn by what you publish: +%.0f%%"), appeal * 100.0f),
                 L, py2 + 6, 12,
                 appeal > 0.0f ? Color{150, 205, 165, 255} : Color{130, 134, 152, 255});
        py2 += 20;
    }
    // What the wheel is allowed to scroll, measured from the page just drawn
    // rather than guessed at. See the clamp above.
    m_profileContentH = py2 + m_profileScroll;
}

void Game::loadDistrictLaws() {
    m_districtLaws.clear();
    std::string path = m_dataDir + "district_laws.json";
    std::ifstream f(path);
    if (!f) f.open("data/district_laws.json");
    if (!f) { LoadLog() << "  No district_laws.json; districts run no laws" << std::endl; return; }
    try {
        nlohmann::json j; f >> j;
        for (const auto& e : j.value("laws", nlohmann::json::array())) {
            DistrictLaw l;
            l.id          = e.value("id", "");
            l.name        = e.value("name", l.id);
            l.description = e.value("description", "");
            l.costPerTurn = e.value("cost_per_turn", 0.0f);
            l.unrestPct   = e.value("unrest_pct", 0.0f);
            l.incomePct   = e.value("income_pct", 0.0f);
            l.growthPct   = e.value("growth_pct", 0.0f);
            if (!l.id.empty()) m_districtLaws.push_back(std::move(l));
        }
    } catch (const std::exception& ex) {
        LoadLog() << "  district_laws.json: " << ex.what() << std::endl;
    }
    LoadLog() << "  Regional laws: " << m_districtLaws.size() << std::endl;
}

Game::DistrictLawEffect Game::districtLawsAt(int countryId, int provinceId) const {
    DistrictLawEffect e;
    const int di = districtIndexOf(countryId, provinceId);
    if (di < 0) return e;
    auto it = m_districts.find(countryId);
    if (it == m_districts.end()) return e;
    for (const std::string& id : it->second[(size_t)di].policies)
        if (const DistrictLaw* l = districtLawById(id)) {
            e.unrestPct += l->unrestPct;
            e.incomePct += l->incomePct;
            e.growthPct += l->growthPct;
        }
    return e;
}

float Game::districtPolicyCost(int countryId) const {
    auto it = m_districts.find(countryId);
    if (it == m_districts.end() || it->second.empty()) return 0.0f;
    float total = 0.0f;
    for (const auto& d : it->second)
        for (const std::string& id : d.policies)
            if (const DistrictLaw* l = districtLawById(id))
                total += l->costPerTurn * (float)d.provinces.size();
    return total;
}

// === updateAIDistricts ===
//
// An AI country draws its own districts, and does it as a REFLEX rather than as
// a decision the policy net makes.
//
// That division is the same one the research groups use and for the same
// measured reason: the net's actions are a fixed set with a fixed meaning, and
// every model in this repository was fitted against that set. Teaching it to
// draw districts means new actions, new mask bits and a retrain -- and the
// AISystem file already records what re-aiming existing bits cost (265->220,
// 238->189). So the net keeps deciding exactly what it decided before, which is
// how much to spend on pacification, and this decides where that money goes.
//
// TWO DISTRICTS, NOT MANY. The player has a map, a brush and as many districts
// as they care to draw; an AI has an arithmetic rule, and the honest version of
// that rule is "the trouble, and everywhere else". Splitting further would be
// inventing detail the heuristic cannot actually justify.
void Game::updateAIDistricts(int countryId) {
    if (countryId <= 0 || countryId == m_playerCountryId) return;
    if (countryId >= REBEL_CID_MIN) return;

    // Redrawn every few turns, not every turn: the borders move, but a district
    // that is re-cut constantly is not a policy, and this walks the country's
    // provinces to decide. Staggered by country id so the cost does not land on
    // one turn for everybody.
    if (((m_turnNumber + countryId) % AI_DISTRICT_REVIEW_TURNS) != 0) return;

    std::vector<int> owned;
    for (const auto& [pid, p] : m_provinces.getAllProvinces())
        if (p.countryId == countryId) owned.push_back(pid);
    if ((int)owned.size() < AI_DISTRICT_MIN_PROVINCES) {
        // Too small to be worth dividing: one district is the same game as no
        // districts, and leaving it undivided keeps pacificationFactor at 1.
        m_districts.erase(countryId);
        return;
    }
    std::sort(owned.begin(), owned.end());

    // ── AN AUTHOR'S DIVISION IS NOT THE AI'S TO REDRAW ──
    //
    // If the map was drawn with this country already cut into districts, that
    // division is scenario content -- the crown lands, the occupation zones,
    // whatever the author meant by it -- and a reflex that replaces it with
    // "worst fifth / everything else" on turn five throws the scenario away.
    // So the AI keeps the SHAPE it was given and decides only the one thing a
    // government decides anyway: which of its districts gets the money. It
    // still re-cuts freely for a country nobody drew districts for.
    const Country* ac = m_countries.getCountry(countryId);
    if (ac && !ac->isoA3.empty() && m_authoredDistricts.count(ac->isoA3)) {
        ensureDefaultDistrict(countryId);      // installs the authored division once
        reconcileDistricts(countryId);
        auto& av = m_districts[countryId];
        if (av.size() > 1) {
            std::vector<float> risk(av.size(), 0.0f);
            float sum = 0.0f;
            for (size_t i = 0; i < av.size(); ++i) {
                for (int pid : av[i].provinces) risk[i] += getProvinceRebellionChance(pid, countryId);
                sum += risk[i];
            }
            if (sum > 0.001f) {
                // ── THE MONEY FOLLOWS THE TROUBLE, BUT NOT OFF A CLIFF ──
                //
                // Straight proportional shares handed Austria-Hungary's quiet
                // half 6% of the budget, because all of its measured unrest sat
                // in the other half -- a district the author drew, funded at
                // nothing. The existing reflex already refuses that for the
                // districts it cuts itself (its split is clamped to 25/75), and
                // an authored district deserves the same floor: no place is
                // written off, and the rest of the money still goes where the
                // trouble is.
                const int n = (int)av.size();
                const int floorPct = std::max(5, 50 / n);
                std::vector<int> want((size_t)n, floorPct);
                int total = 0;
                for (int i = 0; i < n; ++i) {
                    want[(size_t)i] = std::max(floorPct,
                        (int)std::lround(100.0f * risk[(size_t)i] / sum));
                    total += want[(size_t)i];
                }
                // Back to exactly 100, always off the largest share -- which is
                // the one that can spare it and the one the floor did not set.
                while (total != 100) {
                    size_t pick = 0;
                    for (size_t i = 1; i < want.size(); ++i)
                        if (total > 100 ? want[i] > want[pick] : want[i] < want[pick]) pick = i;
                    if (total > 100 && want[pick] <= floorPct) break;   // nothing left to take
                    want[pick] += (total > 100) ? -1 : 1;
                    total += (total > 100) ? -1 : 1;
                }
                for (int i = 0; i < n; ++i) av[(size_t)i].sharePct = want[(size_t)i];
                normaliseDistrictShares(countryId, 0);
                if (getenv("OD_DNAME_DEBUG")) {
                    printf("[AUTHWEIGH] turn %d cid=%d", m_turnNumber, countryId);
                    for (const auto& d : av)
                        printf("  \"%s\" %d%%", d.name.c_str(), d.sharePct);
                    printf("\n");
                }
            }
        }
        return;
    }

    // The trouble, measured the same way the resolver measures it. Anything
    // else would police one number and be judged on another.
    std::vector<std::pair<float, int>> byRisk;
    byRisk.reserve(owned.size());
    // ── WHOSE UNREST ──
    //
    // The one-argument form answers "how likely is this province to revolt
    // AGAINST THE PLAYER", because that is the only government the panels that
    // call it ever ask about. Used here it scored every AI country's ground as
    // if the player governed it -- and in a headless run, where the player is
    // country 0, as if nobody did. So the reflex cut its districts and aimed
    // its money by a number that had nothing to do with the country it was
    // deciding for. It asks about the right government now.
    for (int pid : owned) byRisk.emplace_back(getProvinceRebellionChance(pid, countryId), pid);
    std::sort(byRisk.begin(), byRisk.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second < b.second;      // ties by id, so it is deterministic
              });

    const size_t hotCount = std::max<size_t>(1, owned.size() / 5);   // the worst fifth
    float hotRisk = 0.0f, calmRisk = 0.0f;
    std::vector<int> hot, calm;
    for (size_t i = 0; i < byRisk.size(); ++i) {
        if (i < hotCount) { hot.push_back(byRisk[i].second); hotRisk += byRisk[i].first; }
        else              { calm.push_back(byRisk[i].second); calmRisk += byRisk[i].first; }
    }
    if (getenv("OD_DISTRICT_DEBUG"))
        printf("[AIRISK] turn %d cid=%d worst=%.3f hot=%.3f calm=%.3f\n",
               m_turnNumber, countryId, (double)byRisk.front().first,
               (double)hotRisk, (double)calmRisk);
    if (hot.empty() || calm.empty()) { m_districts.erase(countryId); return; }
    std::sort(hot.begin(), hot.end());
    std::sort(calm.begin(), calm.end());

    // ── AND THE SHARE FOLLOWS THE TROUBLE, NOT THE GROUND ──
    //
    // Sharing by province count is what the undivided country already does, so
    // it would be a district that changes nothing. Sharing by how much unrest
    // sits in each half is the whole point -- and it is clamped, because a
    // country whose calm half is perfectly calm would otherwise hand the hot
    // fifth everything and let the rest drift.
    const float total = std::max(0.001f, hotRisk + calmRisk);
    int hotShare = (int)std::lround(100.0f * (hotRisk / total));
    hotShare = std::clamp(hotShare, 25, 75);

    auto& v = m_districts[countryId];
    v.clear();
    District a;
    a.id = 1; a.r = 200; a.g = 90; a.b = 70;
    a.provinces = std::move(hot); a.sharePct = hotShare;
    // AFTER the ground is in it. The name is taken from the largest province a
    // district holds, and an empty district holds no largest province -- doing
    // this a line earlier named both of them after nothing.
    a.name = suggestDistrictName(countryId, a.provinces);
    District b;
    b.id = 2; b.r = 90; b.g = 140; b.b = 200;
    b.provinces = std::move(calm); b.sharePct = 100 - hotShare;
    b.name = suggestDistrictName(countryId, b.provinces);
    // Both halves can derive the same place -- one people, two directions that
    // both fell inside the bar. uniqueDistrictName settles it, and it appends a
    // numeral only when the compass could not.
    if (b.name == a.name) b.name = uniqueDistrictName(countryId, b.name, 0, b.provinces);
    if (getenv("OD_DNAME_DEBUG"))
        printf("[DNAME] cid=%d  \"%s\" (%zu prov)  |  \"%s\" (%zu prov)\n",
               countryId, a.name.c_str(), a.provinces.size(),
               b.name.c_str(), b.provinces.size());
    v.push_back(std::move(a));
    v.push_back(std::move(b));

    // ── AND A REASON TO FUND IT AT ALL ──
    //
    // Measured: every AI country runs pacification at 0.000, so a district that
    // redistributes that budget redistributes nothing and the whole mechanic is
    // arithmetically inert for them. The reflex above was correct and did
    // exactly nothing, on every map, byte for byte.
    //
    // Concentration is what might change that: spread over a whole country the
    // spend was mostly wasted on ground that was never going to revolt; aimed
    // at the worst fifth it buys several times the suppression per coin.
    //
    // AND IT STILL BUYS NOTHING TODAY, for a reason worth writing down rather
    // than rediscovering. The bankruptcy handler zeroes m_countryPacification
    // every turn for any country that cannot pay for what it already has, and
    // AI countries run at or near zero treasury as a matter of course -- so
    // `spare` below is 0 for essentially all of them and this sets 0. Measured:
    // turning it on and off is byte-identical on 1939 and 1914.
    //
    // Left in because it is the correct rule and costs nothing, and because the
    // thing standing in its way is the AI's economy rather than this mechanic.
    // Making it bite means giving AI countries spare income to spend, which is
    // a change to how they budget and was measured as expensive the last time
    // it was tried. OD_AI_PACIFY_OFF compares the two.
    if (!getenv("OD_AI_PACIFY_OFF")) {
        const float worst = byRisk.front().first;
        float& pac = m_countryPacification[countryId];
        // ── WHERE THE BAR SITS, AND WHY IT IS A VARIABLE ──
        //
        // 12% was above the whole distribution. Measured over 203 reviews on
        // the shipped scenarios, once the risk was asked about the right
        // government: median 0.00, p90 3.34, worst province anywhere 10.14. So
        // this branch could not fire for any country however rich, and the
        // treasury was never the only thing standing in its way.
        // OD_AI_PACIFY_BAR moves it without a rebuild, so an arm that pacifies
        // and an arm that does not are the same binary.
        static const float bar = [] {
            const char* e = getenv("OD_AI_PACIFY_BAR");
            return e ? (float)atof(e) : AI_PACIFY_RISK_BAR;
        }();
        if (worst > bar) {
            const auto cs = computeCountryIncome(countryId);
            // Only out of what is actually spare, so this cannot bankrupt a
            // country the way a flat floor would.
            const float spare = std::max(0.0f, cs.net) / std::max(1.0f, cs.total);
            pac = std::clamp(std::min(AI_PACIFY_MAX, spare * 0.5f), 0.0f, AI_PACIFY_MAX);
            // ── THE ARM THAT MIGHT MAKE ANY OF THIS BITE ──
            //
            // `spare` is net income, and an AI country's net income is about
            // zero as a matter of course, so the line above sets zero for
            // essentially all of them -- which is why districts are inert for
            // the AI however well they are drawn. This funds a floor out of
            // GROSS income instead, only for a country whose worst province is
            // over the bar, so the money comes from somewhere real rather than
            // from a treasury that does not exist. Off by default until it is
            // measured: it is a change to how AI countries budget, and every
            // caution rule tried before it lifted survival and cost rating.
            if (getenv("OD_AI_PACIFY_GROSS"))
                pac = std::clamp(std::max(pac, AI_PACIFY_FLOOR), 0.0f, AI_PACIFY_MAX);
        } else if (pac > 0.0f && worst < bar * 0.6f) {
            pac = 0.0f;      // the trouble passed; stop paying for it
        }
    }

    if (getenv("OD_DISTRICT_DEBUG")) {
        float pac = 0.0f;
        auto it = m_countryPacification.find(countryId);
        if (it != m_countryPacification.end()) pac = it->second;
        printf("[AIDIST] cid=%d hot=%zu calm=%zu hotShare=%d pacification=%.3f\n",
               countryId, v[0].provinces.size(), v[1].provinces.size(), hotShare, pac);
    }
}

void Game::updatePoliticalIdentities() {
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID || cid == SPC_CID) continue;

        auto compassIt = m_countryCompass.find(cid);
        if (compassIt == m_countryCompass.end()) continue;

        PoliticalIdentity current;
        current.quadrant  = (IdeologyQuadrant)c.identityQuadrant;
        current.intensity = (IdeologyIntensity)c.identityIntensity;

        const PoliticalIdentity next = politid::classify(
            compassIt->second.economic, compassIt->second.social, current);
        if (next == current) continue;
        if (m_turnNumber - c.identityTurn < politid::MIN_DWELL_TURNS) continue;

        // First restyle: remember what this country actually was. Everything
        // after reads from here, including the return journey.
        if (!c.rootSaved) {
            c.rootName         = c.name;
            c.rootFlag         = c.flagActual;
            c.rootFlagCensored = c.flagCensored;
            c.rootSaved        = true;
            // The flag it started under is the first era. Recorded here rather
            // than at world load, because this is the moment we learn the
            // country HAS a history -- most never restyle at all and should
            // carry no history rather than a one-entry one.
            c.flagHistory.push_back({std::max(0, c.foundedTurn), c.rootFlag, c.rootFlagCensored});
        }

        const std::string before = c.name;
        c.name       = politid::applyName(c.rootName, next);
        // Seeded from the ROOT name, so this country picks the same charge out
        // of its identity's vocabulary every time: on the host, on every
        // client, and after a reload. The root name is what is stored, and it
        // does not change when the country renames itself -- seeding from the
        // current name would redraw the emblem at every threshold crossing.
        const uint32_t seed = politid::seedFromName(c.rootName);
        c.flagActual = politid::applyFlag(c.rootFlag, next, seed, false);
        // Built separately, NOT copied from the actual one. A radical
        // nationalist government can now charge its flag with a swastika, so
        // the two genuinely differ: the censored build takes the next charge in
        // the same vocabulary instead, off the censored root, and comes out as
        // a clean radical-nationalist flag rather than a mosaic of a dirty one.
        c.flagCensored = politid::applyFlag(c.rootFlagCensored, next, seed, true);
        c.identityQuadrant  = (int)next.quadrant;
        c.identityIntensity = (int)next.intensity;
        c.identityTurn      = m_turnNumber;

        // The texture is cached per country and would otherwise keep showing
        // the old flag for the rest of the game. Same guard the rebel path
        // uses: FlagRenderer ends in LoadTextureFromImage, and a headless run
        // has no GL context to put it on.
        // !m_aiTraining as well as !m_headless.
        //
        // FlagRenderer::render ends in LoadTextureFromImage, and a training run
        // has no usable GL context to put a texture on -- the call lands on a
        // null function pointer and the process dies at address zero, which is
        // exactly what it did: --train-ai segfaulted on its first turn, every
        // time, because this runs for every country every turn. Self-play never
        // looks at a flag, so there is nothing to draw for.
        if (!m_headless && !m_aiTraining) {
            auto fit = m_countryFlags.find(cid);
            if (fit != m_countryFlags.end() && fit->second.id > 0)
                UnloadTexture(fit->second);
            // m_dataDir, NOT "". The emblem is a symbol, symbols live in
            // <data>/symbols/*.svg, and FlagRenderer only maps a star or a gear
            // or a hammer-and-sickle to its SVG when it has a baseDir to look
            // in. With an empty one, fourteen of the sixteen identities drew
            // NOTHING -- the restyled flag came back byte-identical to the
            // original -- and the two that did (circle, outlined circle) only
            // because they have procedural fallbacks. It looked correct after a
            // save and reload, because rebuildFlags() passes m_dataDir, which
            // is how it survived being verified.
            // Whichever variant the player has asked for, the same choice
            // rebuildFlags() makes at load. Rendering flagActual unconditionally
            // was survivable only while the two were copies of each other; now
            // that a radical nationalist flag can carry a swastika, a player
            // with censoring on would have been shown it anyway -- until they
            // saved and reloaded, which is the same shape of fault as the
            // missing emblem above.
            const FlagPattern& shown = m_config.showActualFlags ? c.flagActual : c.flagCensored;
            m_countryFlags[cid] = FlagRenderer::render(shown, 256, 128, m_dataDir, &m_odmJsonData);
        }
        // Names are drawn from a prebuilt label layer; nothing draws one during
        // self-play either.
        if (!m_aiTraining) m_labelsDirty = true;

        // Worth telling the player about: it is the visible consequence of
        // doctrines they or a neighbour chose, and it is how they find out a
        // rival has gone somewhere.
        // A country whose original name already used the form its new identity
        // wants -- "State of Lithuana" becoming authoritarian -- keeps the same
        // name. The flag still changes, so the identity is real and is
        // recorded, but announcing "X has become X" is noise.
        if (before == c.name) {
            printf("[IDENTITY] %s: flag restyled (%s), name unchanged\n",
                   c.name.c_str(), politid::quadrantName(next.quadrant));
        } else if (cid == m_playerCountryId) {
            addNotification(TextFormat(T("Your government's course has remade the country: %s is now %s"),
                                       before.c_str(), c.name.c_str()),
                            hexToColor(m_config.accent()), 8.0f);
        } else if (m_playerCountryId > 0) {
            // Both names through properName: the sentence was translated and
            // the two countries in it were not.
            addNotification(TextFormat(T("%s has become %s"),
                                       od::i18n::properName(before).c_str(),
                                       od::i18n::properName(c.name).c_str()),
                            Color{170, 180, 210, 255}, 6.0f);
        }
        // And the flag it flies from this turn. Bounded: a country that
        // oscillates across a threshold for two hundred turns is a country with
        // a long history, not a country with a memory leak.
        {
            constexpr size_t kMaxEras = 12;
            c.flagHistory.push_back({m_turnNumber, c.flagActual, c.flagCensored});
            if (c.flagHistory.size() > kMaxEras)
                c.flagHistory.erase(c.flagHistory.begin() + 1);   // keep the first
        }
        printf("[IDENTITY] cid=%d %s -> %s (%s, econ=%.0f soc=%.0f)\n",
               cid, before.c_str(), c.name.c_str(),
               politid::quadrantName(next.quadrant),
               compassIt->second.economic, compassIt->second.social);
    }
}

// === bankruptcyUnrestFor ===
//
// See BANKRUPTCY_UNREST_PCT and BANKRUPT_UNREST_FULL_STREAK in Game.h for the
// rule and the evidence behind the ramp.
float Game::bankruptcyUnrestFor(int countryId) const {
    if (!m_bankruptCountries.count(countryId)) return 0.0f;

    // A country recorded as bankrupt but with no streak entry is bankrupt for
    // the first time this turn -- processEconomy increments the streak in the
    // same pass that fills m_bankruptCountries, so the value here is 1 on the
    // first bankrupt turn -- and an older save that predates the streak counter
    // has no entry at all. Both read as the first turn, which is the lenient
    // end; a save cannot be made harsher by being loaded.
    auto it = m_bankruptStreak.find(countryId);
    const int streak = (it != m_bankruptStreak.end()) ? it->second : 1;

    const int steps = BANKRUPT_UNREST_FULL_STREAK;   // 3: a third, two thirds, all
    const int n = std::max(1, std::min(streak, steps));
    return BANKRUPTCY_UNREST_PCT * (float)n / (float)steps;
}
