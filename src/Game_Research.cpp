#include "Game.h"
#include "Audio.h"
#include "GameInternals.h"
#include "ai/AISystem.h"   // noteResearchStall, called from progressCountryResearch
#include "Keybinds.h"
#include "raymath.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdio>

bool ResearchNode::isAvailable(const std::vector<ResearchNode>& nodes) const {
    if (researched || inProgress) return false;
    if (depsAny) {
        bool anyFound = deps.empty();
        for (const auto& req : deps) {
            for (const auto& n : nodes) {
                if (n.id == req && n.researched) { anyFound = true; break; }
            }
            if (anyFound) break;
        }
        if (!anyFound) return false;
    } else {
        for (const auto& req : deps) {
            bool found = false;
            for (const auto& n : nodes) {
                if (n.id == req && n.researched) { found = true; break; }
            }
            if (!found) return false;
        }
    }
    // Check mutual exclusivity: if any node in the same mutexGroup is researched, block
    if (mutexGroup > 0) {
        for (const auto& n : nodes) {
            if (n.id != id && n.mutexGroup == mutexGroup && (n.researched || n.inProgress)) return false;
        }
    }
    return true;
}

// Country-aware ResearchNode::isAvailable: that one reads the player-global
// node flags; this reads m_countryResearched[cid] so every AI country walks
// the tree independently.
bool Game::isNodeAvailableFor(const ResearchNode& node, int countryId) const {
    auto cit = m_countryResearched.find(countryId);
    auto has = [&](const std::string& id) {
        return cit != m_countryResearched.end() && cit->second.count(id) > 0;
    };
    if (has(node.id)) return false;
    if (node.depsAny) {
        bool any = node.deps.empty();
        for (const auto& req : node.deps)
            if (has(req)) { any = true; break; }
        if (!any) return false;
    } else {
        for (const auto& req : node.deps)
            if (!has(req)) return false;
    }
    if (node.mutexGroup > 0)
        for (const auto& n : m_researchNodes)
            if (n.id != node.id && n.mutexGroup == node.mutexGroup && has(n.id)) return false;
    return true;
}

// Per-country research progression for AI countries — the mirror of the
// player block in processUpgrades: allocation buys research points
// (rp = 1 + sqrt(spend/2)), points sink into the active node, completion
// lands in m_countryResearched where all the effect queries pick it up.
void Game::progressCountryResearch(int countryId) {
    if (countryId == m_playerCountryId) return;
    auto raIt = m_countryResearchAllocation.find(countryId);
    if (raIt == m_countryResearchAllocation.end() || raIt->second <= 0.001f) {
        // Unfunded. Worth counting only when a node is actually waiting on the
        // money -- that is the locked-out case, not merely a country that has
        // never started researching.
        auto acIt = m_countryResearchActive.find(countryId);
        if (m_ai && acIt != m_countryResearchActive.end() && acIt->second >= 0)
            m_ai->noteResearchStall(countryId);
        return;
    }

    auto cs = computeCountryIncome(countryId); // O(1) while the turn cache is hot
    int rp = 1 + (int)sqrtf(cs.researchCost * 0.5f);
    int& pts = m_countryResearchPoints[countryId];
    pts = std::min(10000, pts + rp);

    int& active = m_countryResearchActive.count(countryId)
                      ? m_countryResearchActive[countryId]
                      : (m_countryResearchActive[countryId] = -1);
    if (active < 0 || active >= (int)m_researchNodes.size()) return;
    const ResearchNode& node = m_researchNodes[active];
    int& invested = m_countryResearchInvested[countryId];
    if (m_countryResearched[countryId].count(node.id)) { active = -1; invested = 0; return; }

    if (m_ai) m_ai->noteResearchFunded(countryId);
    int toSpend = std::min(pts, node.cost - invested);
    if (toSpend > 0) { invested += toSpend; pts -= toSpend; }
    if (invested >= node.cost) {
        if (m_ai) m_ai->noteResearchDone(countryId);
        m_countryResearched[countryId].insert(node.id);
        active = -1;
        invested = 0;
        if (m_config.aiDebug) {
            const Country* c = m_countries.getCountry(countryId);
            printf("[RESEARCH] %s completed %s\n",
                   c ? c->name.c_str() : "?", node.id.c_str());
        }
    }
}

// A doctrineList() used to live here: thirteen names (Blitzkrieg, Trench
// Warfare, Fortress Doctrine...) that a country could be tagged with. Nothing
// ever called it, and Country::doctrine, which it was meant to populate, was
// written by the map editor's exporter and read by nobody. No combat, economy
// or unrest calculation looked at it, so a country's "doctrine" was a label
// with no consequences. Both are gone.
//
// The doctrines that DO exist are elsewhere and are real: the Politics screen
// (titled "Doctrines" in the UI) enacts entries from m_allPolicies, and the
// research tree has nodes like total_war and fortress_doctrine with actual
// modifiers behind them.

void buildResearchNodes(std::vector<ResearchNode>& out) {
    std::vector<ResearchNode>& m_researchNodes = out; // alias so the body below reads naturally
    m_researchNodes.clear();
    auto add = [&](const std::string& id, const std::string& name, const std::string& desc,
                   const std::string& cat, const std::string& subcat,
                   std::vector<std::string> reqs, int cost, float x, float y) -> ResearchNode& {
        ResearchNode n;
        n.id = id; n.name = name; n.desc = desc;
        n.category = cat; n.subcategory = subcat;
        n.deps = reqs; n.cost = cost;
        n.posX = x; n.posY = y;
        m_researchNodes.push_back(n);
        return m_researchNodes.back();
    };

    // ─── Buildings > Fortifications (linear) ───
    add("fort1",  "Fortification I",   "Unlocks level 1 fortifications (+10% defence)",
        "buildings","fortifications",{},5,50,80).fortLevel=1;
    add("fort2",  "Fortification II",  "Unlocks level 2 fortifications (+20% defence)",
        "buildings","fortifications",{"fort1"},8,50,180).fortLevel=2;
    add("fort3",  "Fortification III", "Unlocks level 3 fortifications (+30% defence)",
        "buildings","fortifications",{"fort2"},15,50,280).fortLevel=3;
    add("fort4",  "Fortification IV",  "Unlocks level 4 fortifications (+40% defence)",
        "buildings","fortifications",{"fort3"},25,50,380).fortLevel=4;
    add("fort5",  "Fortification V",   "Unlocks level 5 fortifications (+50% defence)",
        "buildings","fortifications",{"fort4"},40,50,480).fortLevel=5;
    add("fort6",  "Fortification VI",  "Unlocks level 6 fortifications (+60% defence)",
        "buildings","fortifications",{"fort5"},80,50,580).fortLevel=6;

    // ─── Buildings > Industry (branched) ───
    add("ind1", "Industry I",   "Unlocks industry level 1",
        "buildings","industry",{},4,450,80).industryLevel=1;
    add("ind2", "Industry II",  "Unlocks industry level 2",
        "buildings","industry",{"ind1"},6,450,180).industryLevel=2;
    add("ind3", "Industry III", "Unlocks industry level 3",
        "buildings","industry",{"ind2"},10,450,280).industryLevel=3;
    add("ind4", "Industry IV",  "Unlocks industry level 4",
        "buildings","industry",{"ind3"},15,450,380).industryLevel=4;
    add("ind5", "Industry V",   "Unlocks industry level 5",
        "buildings","industry",{"ind4"},25,450,480).industryLevel=5;
    add("ind_pop", "Population Focus", "Population modifier +50%",
        "buildings","industry",{"ind5"},40,250,580).popModPct=50;
    m_researchNodes.back().mutexGroup=9;
    add("ind_res", "Resource Focus", "Resource modifier +10%",
        "buildings","industry",{"ind5"},40,650,580).resourceModPct=10;
    m_researchNodes.back().mutexGroup=9;
    add("ind6", "Industry VI",  "Unlocks industry level 6",
        "buildings","industry",{"ind_pop","ind_res"},50,450,680).industryLevel=6;
    m_researchNodes.back().depsAny=true;
    add("ind7", "Industry VII", "Unlocks industry level 7",
        "buildings","industry",{"ind6"},50,450,780).industryLevel=7;
    add("ind8", "Industry VIII","Unlocks industry level 8",
        "buildings","industry",{"ind7"},80,450,880).industryLevel=8;
    add("ind9", "Industry IX",  "Unlocks industry level 9",
        "buildings","industry",{"ind8"},120,450,980).industryLevel=9;
    add("ind10","Industry X",   "Unlocks industry level 10",
        "buildings","industry",{"ind9"},150,450,1080).industryLevel=10;
    add("ind_cost","Cost Efficiency","Industry cost -50%",
        "buildings","industry",{"ind10"},400,250,1180).industryCostPct=50;
    m_researchNodes.back().mutexGroup=1;
    add("ind_res2","Resource Exploitation","Resource modifier +25%",
        "buildings","industry",{"ind10"},400,650,1180).resourceModPct=25;
    m_researchNodes.back().mutexGroup=1;

    // ─── Buildings > Ports (linear, 3) ───
    add("port1","Port I",   "Unlocks level 1 ports (basic naval access)",
        "buildings","ports",{},5,780,80).portLevel=1;
    add("port2","Port II",  "Unlocks level 2 ports (improved naval capacity)",
        "buildings","ports",{"port1"},10,780,180).portLevel=2;
    add("port3","Port III", "Unlocks level 3 ports (major naval hub)",
        "buildings","ports",{"port2"},20,780,280).portLevel=3;

    // ─── Army > Army (extensive branching) ───
    add("basic_training","Basic Training","Unlocks army. Conscription cost -10%",
        "army","army",{},4,100,80).conscriptionCostPct=10;
    add("professional_army","Professional Army","Maintenance cost -10%",
        "army","army",{"basic_training"},6,100,180).maintenanceCostPct=10;
    add("def_tactics","Defensive Tactics","Province defence +10%",
        "army","army",{"professional_army"},8,30,280).armyDefPct=10;
    m_researchNodes.back().mutexGroup=2;
    add("off_tactics","Offensive Tactics","Province attack +10%",
        "army","army",{"professional_army"},8,210,280).armyAtkPct=10;
    m_researchNodes.back().mutexGroup=2;
    add("combined_arms","Combined Arms","Attack +5%, Defence +5%",
        "army","army",{"def_tactics","off_tactics"},15,100,380);
    m_researchNodes.back().depsAny=true;
    m_researchNodes.back().armyDefPct=5; m_researchNodes.back().armyAtkPct=5;
    add("logistics","Advanced Logistics","Maintenance cost -15%",
        "army","army",{"combined_arms"},15,100,480).maintenanceCostPct=15;
    add("elite_training","Elite Training","Attack +15%",
        "army","army",{"logistics"},20,100,580).armyAtkPct=15;
    add("modern_warfare","Modern Warfare","Attack +10%, Defence +10%",
        "army","army",{"elite_training"},25,100,680);
    m_researchNodes.back().armyAtkPct=10; m_researchNodes.back().armyDefPct=10;
    add("total_war","Total War Doctrine","Attack +20%",
        "army","army",{"modern_warfare"},30,30,780).armyAtkPct=20;
    m_researchNodes.back().mutexGroup=3;
    add("fortress_doctrine","Fortress Doctrine","Defence +25%",
        "army","army",{"modern_warfare"},30,210,780).armyDefPct=25;
    m_researchNodes.back().mutexGroup=3;
    add("officer_corps","Officer Corps","Maintenance cost -20%",
        "army","army",{"total_war","fortress_doctrine"},25,100,880).maintenanceCostPct=20;
    m_researchNodes.back().depsAny=true;
    add("reserve_system","Reserve System","Conscription cost -25%",
        "army","army",{"officer_corps"},20,100,980).conscriptionCostPct=25;
    add("national_mob","National Mobilization","Conscription capacity +30%",
        "army","army",{"reserve_system"},30,30,1080).conscriptionPct=30;
    m_researchNodes.back().mutexGroup=4;
    add("volunteer_force","Volunteer Force","Maintenance -30%, Conscription cost -50%",
        "army","army",{"reserve_system"},30,210,1080);
    m_researchNodes.back().mutexGroup=4;
    m_researchNodes.back().maintenanceCostPct=30; m_researchNodes.back().conscriptionCostPct=50;
    add("people_army","People's Army","Conscription capacity +50%",
        "army","army",{"national_mob","volunteer_force"},40,100,1180).conscriptionPct=50;
    m_researchNodes.back().depsAny=true;

    // ─── Army > Navy (linear with branches) ───
    add("navy1","Naval Engineering","Unlocks ship building",
        "army","navy",{},8,450,80).unlockShips=true;
    add("navy2","Advanced Shipbuilding","Ship cost -10%",
        "army","navy",{"navy1"},12,450,180).navyCostPct=10;
    add("navy3","Naval Architecture","Ship cost -15%",
        "army","navy",{"navy2"},20,450,280).navyCostPct=15;
    add("navy4","Efficient Dockyards","Ship cost -20%",
        "army","navy",{"navy3"},25,340,380).navyCostPct=20;
    m_researchNodes.back().mutexGroup=5;
    add("navy5","Naval Logistics","Ship speed +25%",
        "army","navy",{"navy3"},25,560,380).navySpeedPct=25;
    m_researchNodes.back().mutexGroup=5;
    add("navy6","Fleet Modernization","Ship cost -15%",
        "army","navy",{"navy4","navy5"},30,450,480).navyCostPct=15;
    m_researchNodes.back().depsAny=true;
    add("navy7","Radar Technology","Ship defence +15%",
        "army","navy",{"navy6"},30,340,580).navyDefPct=15;
    m_researchNodes.back().mutexGroup=8;
    add("navy8","Naval Aviation","Ship attack +15%",
        "army","navy",{"navy6"},30,560,580).navyAtkPct=15;
    m_researchNodes.back().mutexGroup=8;
    add("navy9","Fleet Logistics","Ship speed +10%",
        "army","navy",{"navy7","navy8"},35,450,680).navySpeedPct=10;
    m_researchNodes.back().depsAny=true;
    add("navy10","Global Navy Doctrine","Ship attack +10%, Ship defence +10%",
        "army","navy",{"navy9"},40,450,780);
    m_researchNodes.back().navyAtkPct=10; m_researchNodes.back().navyDefPct=10;

    // ─── Army > Artillery (linear with final branch) ───
    add("arty1","Mortar","Kills 5% of troops in targeted province",
        "army","artillery",{},12,950,80);
    m_researchNodes.back().artilleryType="mortar"; m_researchNodes.back().artilleryTroopKillPct=5;
    add("arty2","Light Artillery","Kills 10% of troops in targeted province",
        "army","artillery",{"arty1"},20,950,180);
    m_researchNodes.back().artilleryType="light"; m_researchNodes.back().artilleryTroopKillPct=10;
    add("arty3","Heavy Artillery","Kills 20% of troops, 5% of population",
        "army","artillery",{"arty2"},30,950,280);
    m_researchNodes.back().artilleryType="heavy"; m_researchNodes.back().artilleryTroopKillPct=20;
    m_researchNodes.back().artilleryPopKillPct=5;
    add("arty4a","Napalm","Kills 25% of troops, 15% of population",
        "army","artillery",{"arty3"},50,800,380);
    m_researchNodes.back().artilleryType="napalm"; m_researchNodes.back().artilleryTroopKillPct=25;
    m_researchNodes.back().artilleryPopKillPct=15; m_researchNodes.back().mutexGroup=6;
    add("arty4b","Carpet Bombing","Kills 15% of troops, 10% of population. 50% chance to damage fortifications",
        "army","artillery",{"arty3"},50,1100,380);
    m_researchNodes.back().artilleryType="carpet"; m_researchNodes.back().artilleryTroopKillPct=15;
    m_researchNodes.back().artilleryPopKillPct=10; m_researchNodes.back().artilleryFortDamageChance=50;
    m_researchNodes.back().mutexGroup=6;
    add("arty5","Chemical Artillery","Kills 50% of troops, 30% of population",
        "army","artillery",{"arty4a","arty4b"},80,950,480);
    m_researchNodes.back().depsAny=true;
    m_researchNodes.back().artilleryType="chemical"; m_researchNodes.back().artilleryTroopKillPct=50;
    m_researchNodes.back().artilleryPopKillPct=30;
    add("arty6a","Nuclear Shelling","Kills 75% of troops. Damages industry -3, fortifications -2",
        "army","artillery",{"arty5"},100,800,580);
    m_researchNodes.back().artilleryType="nuclear"; m_researchNodes.back().artilleryTroopKillPct=75;
    m_researchNodes.back().artilleryIndustryDamage=3; m_researchNodes.back().artilleryFortDamage=2;
    m_researchNodes.back().mutexGroup=7;
    add("arty6b","Biological Shelling","Kills 80% of troops, 95% of population",
        "army","artillery",{"arty5"},100,1100,580);
    m_researchNodes.back().artilleryType="biological"; m_researchNodes.back().artilleryTroopKillPct=80;
    m_researchNodes.back().artilleryPopKillPct=95; m_researchNodes.back().mutexGroup=7;

    // ─── Population > Conscription ───
    add("conscript1","Local Draft","Conscription capacity +10%",
        "population","conscription",{},5,100,80).conscriptionPct=10;
    add("conscript2","Regional Recruitment","Conscription capacity +15%",
        "population","conscription",{"conscript1"},10,100,180).conscriptionPct=15;
    add("conscript3","National Service","Conscription capacity +20%",
        "population","conscription",{"conscript2"},15,100,280).conscriptionPct=20;
    add("conscript4","Universal Conscription","Conscription capacity +30%",
        "population","conscription",{"conscript3"},20,100,380).conscriptionPct=30;
    add("conscript5","Total Mobilization","Conscription capacity +50%",
        "population","conscription",{"conscript4"},30,100,480).conscriptionPct=50;
    add("conscript6","Patriotic Wave","Conscription capacity +75%, Maintenance -10%",
        "population","conscription",{"conscript5"},40,100,580);
    m_researchNodes.back().conscriptionPct=75; m_researchNodes.back().maintenanceCostPct=10;

    // ─── Population > Tourism ───
    add("tourism1","Local Tourism","Population growth +5%, Migration +20%",
        "population","tourism",{},8,400,80).popGrowthPct=5;
    m_researchNodes.back().migrationRate=0.2f;
    add("tourism2","Regional Tourism","Population growth +10%, Migration +50%",
        "population","tourism",{"tourism1"},15,400,180).popGrowthPct=10;
    m_researchNodes.back().migrationRate=0.5f;
    add("tourism3","International Tourism","Population growth +20%, Migration +100%",
        "population","tourism",{"tourism2"},25,400,280).popGrowthPct=20;
    m_researchNodes.back().migrationRate=1.0f;

    // ─── Population > Indoctrination ───
    add("indoctrinate1","Cultural Programs","Minority alignment +5%/turn",
        "population","indoctrination",{},10,700,80).indoctrinationPct=5;
    add("indoctrinate2","Educational Reform","Minority alignment +10%/turn",
        "population","indoctrination",{"indoctrinate1"},20,700,180).indoctrinationPct=10;
    add("indoctrinate3","National Identity","Minority alignment +20%/turn",
        "population","indoctrination",{"indoctrinate2"},30,700,280).indoctrinationPct=20;

    // ─── Misc > Repeatable Research ───
    add("passive_income","Passive Income I","+1 to economy per research level.",
        "misc","misc",{},10,100,80).passiveIncome=1;
    m_researchNodes.back().infinite=true;
    add("passive_income_2","Passive Income II","+1 to economy per research level.",
        "misc","misc",{"passive_income"},15,100,180).passiveIncome=1;
    m_researchNodes.back().infinite=true;
    add("passive_income_3","Passive Income III","+1 to economy per research level.",
        "misc","misc",{"passive_income_2"},20,100,280).passiveIncome=1;
    m_researchNodes.back().infinite=true;
    add("passive_income_4","Passive Income IV","+1 to economy per research level.",
        "misc","misc",{"passive_income_3"},25,100,380).passiveIncome=1;
    m_researchNodes.back().infinite=true;
    add("passive_income_5","Passive Income V","+1 to economy per research level.",
        "misc","misc",{"passive_income_4"},30,100,480).passiveIncome=1;
    m_researchNodes.back().infinite=true;
    add("pop_bonus1","Population Income Bonus I","Population income +10% per research level.",
        "misc","misc",{},10,300,80).popModPct=10;
    m_researchNodes.back().infinite=true;
    add("pop_bonus2","Population Income Bonus II","Population income +10% per research level.",
        "misc","misc",{"pop_bonus1"},75,300,180).popModPct=10;
    m_researchNodes.back().infinite=true;
    add("pop_bonus3","Population Income Bonus III","Population income +10% per research level.",
        "misc","misc",{"pop_bonus2"},100,300,280).popModPct=10;
    m_researchNodes.back().infinite=true;
    add("pop_bonus4","Population Income Bonus IV","Population income +10% per research level.",
        "misc","misc",{"pop_bonus3"},125,300,380).popModPct=10;
    m_researchNodes.back().infinite=true;
    add("pop_bonus5","Population Income Bonus V","Population income +10% per research level.",
        "misc","misc",{"pop_bonus4"},150,300,480).popModPct=10;
    m_researchNodes.back().infinite=true;

    std::cout << "  Loaded " << m_researchNodes.size() << " research nodes" << std::endl;
}

void Game::initResearchTrees() {
    buildResearchNodes(m_researchNodes);

    // ─── Apply per-country starting research based on development level ───
    // Helper: mark a node as researched for a given country
    auto setResearched = [&](int cid, const std::string& id) {
        m_countryResearched[cid].insert(id);
    };
    // Helper: find country ID by ISO code
    auto findCid = [&](const std::string& iso) -> int {
        for (auto& [id, c] : m_countries.getAll())
            if (c.isoA3 == iso) return id;
        return -1;
    };
    // Tier 1: USA, CHN, GBR, FRA, DEU, JPN — fort1-3, ind1-5, port1-3, basic_training, navy1-2, arty1
    std::vector<std::string> tier1Nodes = {"fort1","fort2","fort3","ind1","ind2","ind3","ind4","ind5",
                                           "port1","port2","port3","basic_training","navy1","navy2","arty1"};
    for (auto& iso : {"USA","CHN","GBR","FRA","DEU","JPN"}) {
        int cid = findCid(iso);
        if (cid < 0) continue;
        for (auto& nid : tier1Nodes) setResearched(cid, nid);
    }
    // Tier 2: ITA, BRA, RUS, CAN, MEX, IDN, ESP, TUR, THA, CHE, POL, NLD, SAU, AUS, KOR, SWE, NOR, DNK, BEL, AUT, CZE, FIN, PRT, GRC, IRL
    std::vector<std::string> tier2Nodes = {"fort1","fort2","ind1","ind2","ind3",
                                           "port1","port2","basic_training","navy1","arty1"};
    for (auto& iso : {"ITA","BRA","RUS","CAN","MEX","IDN","ESP","TUR","THA","CHE","POL","NLD","SAU","AUS","KOR","SWE","NOR","DNK","BEL","AUT","CZE","FIN","PRT","GRC","IRL"}) {
        int cid = findCid(iso);
        if (cid < 0) continue;
        for (auto& nid : tier2Nodes) setResearched(cid, nid);
    }
    // Tier 3 (all others): ind1, basic_training
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID || cid == SPC_CID) continue;
        if (m_countryResearched[cid].empty()) {
            setResearched(cid, "ind1");
            setResearched(cid, "basic_training");
        }
    }
    std::cout << "  Applied per-country starting research" << std::endl;
}

bool Game::hasResearched(const std::string& nodeId, int countryId) const {
    int cid = (countryId >= 0) ? countryId : m_playerCountryId;
    // Check per-country state first
    auto cit = m_countryResearched.find(cid);
    if (cit != m_countryResearched.end() && cit->second.count(nodeId))
        return true;
    // Fall back to global node state (for backward compat / uncached)
    for (const auto& n : m_researchNodes)
        if (n.id == nodeId && n.researched) return true;
    return false;
}

void Game::addResearchPoints(int countryId) {
    if (countryId <= 0 || countryId == SPC_CID) return;
    auto cs = computeCountryIncome(countryId);
    float allocAmount = cs.researchCost; // already capped with pacification
    // Logarithmic: base 1 + sqrt(alloc) for diminishing returns
    int rp = 1 + (int)(sqrtf(allocAmount * 0.5f));
    m_researchPoints += rp;
    // Clamp points
    if (m_researchPoints > 10000) m_researchPoints = 10000;

    // Add points to active research node
    if (m_researchActiveNode >= 0 && m_researchActiveNode < (int)m_researchNodes.size()) {
        auto& node = m_researchNodes[m_researchActiveNode];
        if (hasResearched(node.id, countryId)) { m_researchActiveNode = -1; return; }
        if (node.researched) { m_researchActiveNode = -1; return; } // legacy fallback
        // Spend points
        int toSpend = std::min(m_researchPoints, node.cost - node.invested);
        node.invested += toSpend;
        m_researchPoints -= toSpend;
        if (node.invested >= node.cost) {
            node.researched = true;
            node.inProgress = false;
            m_countryResearched[countryId].insert(node.id);
            m_researchActiveNode = -1;
            trackChange();
        }
    }
}

void Game::updateResearch(int countryId) {
    addResearchPoints(countryId);
}

int Game::getResearchedFortLevel(int countryId) const {
    if (countryId < 0) countryId = m_playerCountryId;
    int maxLevel = 0;
    auto cit = m_countryResearched.find(countryId);
    if (cit != m_countryResearched.end()) {
        for (const auto& n : m_researchNodes)
            if (cit->second.count(n.id) && n.fortLevel > maxLevel) maxLevel = n.fortLevel;
    } else {
        for (const auto& n : m_researchNodes)
            if (n.researched && n.fortLevel > maxLevel) maxLevel = n.fortLevel;
    }
    return maxLevel;
}

int Game::getResearchedIndustryLevel(int countryId) const {
    if (countryId < 0) countryId = m_playerCountryId;
    int maxLevel = 0;
    auto cit = m_countryResearched.find(countryId);
    if (cit != m_countryResearched.end()) {
        for (const auto& n : m_researchNodes)
            if (cit->second.count(n.id) && n.industryLevel > maxLevel) maxLevel = n.industryLevel;
    } else {
        for (const auto& n : m_researchNodes)
            if (n.researched && n.industryLevel > maxLevel) maxLevel = n.industryLevel;
    }
    return maxLevel;
}

int Game::getResearchedPortLevel(int countryId) const {
    if (countryId < 0) countryId = m_playerCountryId;
    int maxLevel = 0;
    auto cit = m_countryResearched.find(countryId);
    if (cit != m_countryResearched.end()) {
        for (const auto& n : m_researchNodes)
            if (cit->second.count(n.id) && n.portLevel > maxLevel) maxLevel = n.portLevel;
    } else {
        for (const auto& n : m_researchNodes)
            if (n.researched && n.portLevel > maxLevel) maxLevel = n.portLevel;
    }
    return maxLevel;
}

// Sum of one research modifier across everything `countryId` has researched.
//
// This used to read ResearchNode::researched, a single flag on the shared node
// list — which is the PLAYER's tree, because that is the only tree the research
// screen ever writes to. Every caller therefore applied the player's bonuses to
// whoever happened to be fighting: an AI country that had completed Total War
// Doctrine got the unlock and none of the +20% attack, while the player's
// research quietly buffed every AI army on the map as well as their own.
//
// The per-country set is authoritative when it exists, with the same fallback
// to the shared flag that getResearchedFortLevel and its siblings use, so a map
// or save that predates per-country research still behaves as it did.
float Game::getTotalEffect(const std::string& effectField, int countryId) const {
    const int cid = (countryId >= 0) ? countryId : m_playerCountryId;
    auto cit = m_countryResearched.find(cid);
    const std::unordered_set<std::string>* own =
        (cit != m_countryResearched.end() && !cit->second.empty()) ? &cit->second : nullptr;
    float total = 0;
    for (const auto& n : m_researchNodes) {
        if (own ? !own->count(n.id) : !n.researched) continue;
        if (effectField == "armyDefPct") total += n.armyDefPct;
        else if (effectField == "armyAtkPct") total += n.armyAtkPct;
        else if (effectField == "conscriptionCostPct") total += n.conscriptionCostPct;
        else if (effectField == "maintenanceCostPct") total += n.maintenanceCostPct;
        else if (effectField == "navyCostPct") total += n.navyCostPct;
        else if (effectField == "popModPct") total += n.popModPct;
        else if (effectField == "resourceModPct") total += n.resourceModPct;
        else if (effectField == "industryCostPct") total += n.industryCostPct;
        else if (effectField == "passiveIncome") total += n.passiveIncome;
        else if (effectField == "popGrowthPct") total += n.popGrowthPct;
        else if (effectField == "migrationRate") total += n.migrationRate;
        else if (effectField == "indoctrinationPct") total += n.indoctrinationPct;
        else if (effectField == "conscriptionPct") total += n.conscriptionPct;
        else if (effectField == "navyAtkPct") total += n.navyAtkPct;
        else if (effectField == "navyDefPct") total += n.navyDefPct;
        else if (effectField == "navySpeedPct") total += n.navySpeedPct;
    }
    return total;
}

// Why this country cannot enact this doctrine, in words a player can act on.
// Empty means it can.
//
// This exists because the doctrine screen used to answer the question wrongly.
// It drew "X Conflicts with: ..." under any doctrine that HAD incompatibilities,
// whether or not a single one of them was in force, and said nothing at all
// about the reason it was actually greyed out -- which is usually the treasury
// or the compass. A player read the conflict list as the explanation, went to
// the Active tab, found none of those doctrines there, and reported that the
// game was blocking them for no reason. It was; it just was not that reason.
//
// canCountryEnactPolicy is now this function asking whether it found anything,
// so the button and the explanation cannot disagree about why.
std::string Game::policyBlockReason(int countryId, const Policy& p) const {
    auto it = m_countryCompass.find(countryId);
    if (it == m_countryCompass.end())
        return "This country has no political compass.";
    const auto& pc = it->second;

    // economic runs -100 (left) to +100 (right); social -100 (authoritarian)
    // to +100 (libertarian). See PoliticalCompass.
    if (pc.economic < p.minEcon)
        return TextFormat("Your economy is too far left for this (%.0f; needs %.0f or higher).",
                          pc.economic, p.minEcon);
    if (pc.economic > p.maxEcon)
        return TextFormat("Your economy is too far right for this (%.0f; needs %.0f or lower).",
                          pc.economic, p.maxEcon);
    if (pc.social < p.minSoc)
        return TextFormat("Your government is too authoritarian for this (%.0f; needs %.0f or higher).",
                          pc.social, p.minSoc);
    if (pc.social > p.maxSoc)
        return TextFormat("Your government is too libertarian for this (%.0f; needs %.0f or lower).",
                          pc.social, p.maxSoc);

    auto displayName = [&](const std::string& id) {
        for (const auto& q : m_allPolicies)
            if (q.id == id) return q.name;
        return id;
    };

    for (const auto& ap : m_activePolicies) {
        if (ap.countryId != countryId || ap.turnsRemaining < 0) continue;
        if (ap.policyId == p.id)
            return ap.turnsRemaining > 0 ? "Already being implemented."
                                         : "Already in force.";
        for (const auto& inc : p.incompatibleWith)
            if (ap.policyId == inc)
                return "Conflicts with " + displayName(inc) +
                       ", which is active. Repeal it first.";
    }

    auto cs = computeCountryIncome(countryId);
    float available = cs.total - (cs.armyExpenses + cs.navyExpenses + cs.policyCosts + cs.minorityCosts);
    available = std::max(0.0f, available);
    if (p.costPerTurn > 0 && available < p.costPerTurn)
        return TextFormat("Costs %d/turn and only %.0f is spare.", p.costPerTurn, available);

    return "";
}

bool Game::canCountryEnactPolicy(int countryId, const Policy& p) const {
    return policyBlockReason(countryId, p).empty();
}

void Game::drawResearchTab() {
    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 200});
    Vector2 mouse = getMouse();

    // ─── Close button ───
    Rectangle closeBtn = {(float)(m_screenW - 44), 8, 36, 36};
    DrawRectangleRounded(closeBtn, 0.2f, 6, {60, 60, 70, 180});
    DrawRectangleRoundedLines(closeBtn, 0.2f, 6, {180, 180, 180, 200});
    int xw = MeasureText("X", 20);
    DrawText("X", (int)(closeBtn.x + closeBtn.width/2 - xw/2), 12, 20, {180, 180, 180, 200});
    if (CheckCollisionPointRec(mouse, closeBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        Audio::get().playSfx("back");
        m_inResearch = false;
        m_inPolitics = false;
        m_activeSidebarTab = 0;
        if (m_renderer) m_renderer->setPaused(false);
        return;
    }
    DrawText("ESC to close", m_screenW - 140, 55, 14, Color{120, 120, 140, 150});

    // ─── Category tabs ───
    const char* catNames[] = {"Buildings", "Army", "Population", "Misc"};
    const char* catKeys[] = {"buildings", "army", "population", "misc"};
    int catCount = 4;
    int catTabY = 8;
    int catTabH = 30;
    int catTabStartX = 16;
    int catSpacing = 200;
    for (int c = 0; c < catCount; ++c) {
        int tx = catTabStartX + c * catSpacing;
        Rectangle cr = {(float)tx, (float)catTabY, (float)(catSpacing - 8), (float)catTabH};
        bool active = (c == m_researchTab);
        bool hovered = CheckCollisionPointRec(mouse, cr);
        Color bg = active ? Color{60, 60, 80, 200} : (hovered ? Color{40, 40, 60, 180} : Color{30, 30, 50, 150});
        DrawRectangleRounded(cr, 0.1f, 6, bg);
        if (active) DrawRectangleRoundedLines(cr, 0.1f, 6, hexToColor(m_config.accentColor));
        int tw = MeasureText(catNames[c], 16);
        DrawText(catNames[c], tx + (catSpacing - 8 - tw) / 2, catTabY + 6, 16, active ? hexToColor(m_config.accentColor) : LIGHTGRAY);
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && hovered && m_researchTab != c) {
            m_researchTab = c;
            Audio::get().playSfx("tab_switch");
        }
    }

    // ─── Pan/Zoom ───
    bool overCatTab = mouse.y < catTabY + catTabH;
    if (!overCatTab && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (!m_researchDragging) { m_researchDragging = true; m_researchDragPrevX = (int)mouse.x; m_researchDragPrevY = (int)mouse.y; }
        int dx = (int)mouse.x - m_researchDragPrevX;
        int dy = (int)mouse.y - m_researchDragPrevY;
        m_researchCamX += dx; m_researchCamY += dy;
        m_researchDragPrevX = (int)mouse.x; m_researchDragPrevY = (int)mouse.y;
    } else { m_researchDragging = false; }

    float wheel = GetMouseWheelMove();
    if (wheel != 0 && !overCatTab) {
        float oldZoom = m_researchZoom;
        m_researchZoom *= (wheel > 0) ? 1.2f : 0.833f;
        if (m_researchZoom < 0.5f) m_researchZoom = 0.5f;
        if (m_researchZoom > 3.0f) m_researchZoom = 3.0f;
        float factor = m_researchZoom / oldZoom;
        m_researchCamX = (int)(mouse.x - factor * (mouse.x - m_researchCamX));
        m_researchCamY = (int)(mouse.y - factor * (mouse.y - m_researchCamY));
    }

    // ─── Collect nodes for active category ───
    std::vector<int> catIndices;
    float catMinX = 1e9f, catMaxX = -1e9f, catMinY = 1e9f, catMaxY = -1e9f;
    for (int i = 0; i < (int)m_researchNodes.size(); i++) {
        if (m_researchNodes[i].category == catKeys[m_researchTab]) {
            catIndices.push_back(i);
            auto& n = m_researchNodes[i];
            if (n.posX < catMinX) catMinX = n.posX;
            if (n.posX > catMaxX) catMaxX = n.posX;
            if (n.posY < catMinY) catMinY = n.posY;
            if (n.posY > catMaxY) catMaxY = n.posY;
        }
    }

    // ─── Clamp camera ───
    float pad = 80.0f;
    float scaledNodeW = 160.0f * m_researchZoom;
    float scaledNodeH = 50.0f * m_researchZoom;
    if (catMinX < 1e8f) {
        float viewW = m_screenW / m_researchZoom;
        float viewH = (m_screenH - 80) / m_researchZoom;
        float minCamX = -(catMaxX + scaledNodeW + pad - viewW);
        float maxCamX = -catMinX + pad;
        float minCamY = -(catMaxY + scaledNodeH + pad - viewH);
        float maxCamY = -catMinY + pad;
        if (minCamX > maxCamX) { float avg = (minCamX + maxCamX) / 2; minCamX = avg; maxCamX = avg; }
        if (minCamY > maxCamY) { float avg = (minCamY + maxCamY) / 2; minCamY = avg; maxCamY = avg; }
        m_researchCamX = std::clamp(m_researchCamX, minCamX, maxCamX);
        m_researchCamY = std::clamp(m_researchCamY, minCamY, maxCamY);
    }

    if (catIndices.empty()) {
        DrawText("No research trees in this category", m_screenW / 2 - 100, m_screenH / 2, 14, LIGHTGRAY);
    }

    // ─── Subcategory labels ───
    std::string lastSubcat;
    for (int idx : catIndices) {
        auto& node = m_researchNodes[idx];
        if (node.subcategory != lastSubcat) {
            lastSubcat = node.subcategory;
            int lx = (int)(node.posX * m_researchZoom + m_researchCamX);
            int ly = (int)(node.posY * m_researchZoom + m_researchCamY);
            DrawText(node.subcategory.c_str(), lx, ly - (int)(26 * m_researchZoom), (int)(14 * m_researchZoom), {180, 180, 200, 200});
        }
    }

    const int baseNodeW = 160;
    const int baseNodeH = 50;
    int nodeW = (int)(baseNodeW * m_researchZoom);
    int nodeH = (int)(baseNodeH * m_researchZoom);
    if (nodeW < 40) nodeW = 40;
    if (nodeH < 14) nodeH = 14;

    // ─── Draw connection lines ───
    for (int idx : catIndices) {
        auto& node = m_researchNodes[idx];
        int nx = (int)(node.posX * m_researchZoom + m_researchCamX);
        int ny = (int)(node.posY * m_researchZoom + m_researchCamY);
        for (const auto& req : node.deps) {
            int depMutexGroup = 0;
            for (auto& pn : m_researchNodes) {
                if (pn.id == req) {
                    depMutexGroup = pn.mutexGroup;
                    int px = (int)(pn.posX * m_researchZoom + m_researchCamX);
                    int py = (int)(pn.posY * m_researchZoom + m_researchCamY);
                    Color lineCol = (pn.researched) ? Color{100, 200, 100, 120} : Color{100, 100, 100, 80};
                    DrawLine(px + nodeW / 2, py + nodeH, nx + nodeW / 2, ny, lineCol);
                    break;
                }
            }
            // Draw lines from all mutex siblings of the dependency too
            if (depMutexGroup > 0) {
                for (auto& sn : m_researchNodes) {
                    if (sn.id == req || sn.mutexGroup != depMutexGroup) continue;
                    int sx = (int)(sn.posX * m_researchZoom + m_researchCamX);
                    int sy = (int)(sn.posY * m_researchZoom + m_researchCamY);
                    Color sCol = sn.researched ? Color{100, 200, 100, 80} : Color{80, 80, 80, 60};
                    DrawLine(sx + nodeW / 2, sy + nodeH, nx + nodeW / 2, ny, sCol);
                }
            }
        }
    }

    // ─── Draw nodes ───
    m_researchHoveredNode = -1;
    for (int idx : catIndices) {
        auto& node = m_researchNodes[idx];
        int nx = (int)(node.posX * m_researchZoom + m_researchCamX);
        int ny = (int)(node.posY * m_researchZoom + m_researchCamY);
        Rectangle r = {(float)nx, (float)ny, (float)nodeW, (float)nodeH};

        Color bg, border;
        if (node.researched) { bg = {40, 120, 40, 220}; border = {80, 200, 80, 255}; }
        else if (node.inProgress) { bg = {120, 100, 30, 220}; border = {220, 200, 60, 255}; }
        else if (node.isAvailable(m_researchNodes)) { bg = {40, 40, 60, 220}; border = {120, 120, 180, 255}; }
        else { bg = {30, 30, 35, 180}; border = {60, 60, 70, 150}; }

        bool hovered = CheckCollisionPointRec(mouse, r);
        if (hovered) {
            if (m_lastResearchHover != idx) {
                m_lastResearchHover = idx;
                Audio::get().playSfx("hover");
            }
            m_researchHoveredNode = idx;
            border = hexToColor(m_config.accentColor);
        }

        DrawRectangleRounded(r, 0.15f, 8, bg);
        DrawRectangleRoundedLines(r, 0.15f, 8, border);
        int fs = (int)(12 * m_researchZoom); if (fs < 7) fs = 7; if (fs > 20) fs = 20;
        int textW = MeasureText(node.name.c_str(), fs);
        DrawText(node.name.c_str(), nx + nodeW / 2 - textW / 2, ny + 4, fs, WHITE);
        int infoFs = (int)(8 * m_researchZoom); if (infoFs < 6) infoFs = 6; if (infoFs > 14) infoFs = 14;
        if (node.inProgress) {
            float pct = (float)node.invested / node.cost;
            int barH = (int)(5 * m_researchZoom); if (barH < 3) barH = 3;
            int barY = ny + nodeH - barH - 4;
            DrawRectangle(nx + 4, barY, nodeW - 8, barH, {60, 60, 60, 200});
            DrawRectangle(nx + 4, barY, (int)((nodeW - 8) * pct), barH, {220, 200, 60, 255});
            DrawText(TextFormat("%d/%d RP", node.invested, node.cost), nx + 4, barY - infoFs - 2, infoFs, {200, 200, 200, 200});
        } else if (!node.researched) {
            DrawText(TextFormat("%d RP", node.cost), nx + 4, ny + nodeH - infoFs - 6, infoFs, {160, 160, 160, 200});
        } else {
            DrawText("DONE", nx + 4, ny + nodeH - infoFs - 6, infoFs, {100, 200, 100, 200});
        }
    }

    // ─── Hover tooltip ───
    if (m_researchHoveredNode >= 0) {
        auto& node = m_researchNodes[m_researchHoveredNode];
        int tw0 = MeasureText(node.name.c_str(), 14);
        int dw = MeasureText(node.desc.c_str(), 11);
        int tipW = std::max(tw0, dw) + 20;
        int tipX = (int)mouse.x + 16; if (tipX + tipW > m_screenW) tipX = m_screenW - tipW - 8;
        int tipY = (int)mouse.y + 16;
        int tipH = 48;
        if (!node.researched && !node.inProgress && !node.isAvailable(m_researchNodes)) tipH += 12;
        DrawRectangle(tipX, tipY, tipW, tipH, {10, 10, 20, 220});
        DrawRectangleLines(tipX, tipY, tipW, tipH, {100, 100, 140, 200});
        DrawText(node.name.c_str(), tipX + 10, tipY + 4, 14, WHITE);
        DrawText(node.desc.c_str(), tipX + 10, tipY + 22, 11, {200, 200, 200, 255});
        if (!node.researched && !node.inProgress && !node.isAvailable(m_researchNodes))
            DrawText("LOCKED - Research prerequisites first", tipX + 10, tipY + 36, 10, {200, 100, 100, 255});
    }

    // ─── Click to start research ───
    if (!m_researchDragging && m_researchHoveredNode >= 0 && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        auto& node = m_researchNodes[m_researchHoveredNode];
        if (!node.researched && !node.inProgress && node.isAvailable(m_researchNodes)) {
            if (m_researchActiveNode >= 0) m_researchNodes[m_researchActiveNode].inProgress = false;
            node.inProgress = true;
            m_researchActiveNode = m_researchHoveredNode;
            Audio::get().playSfx("research_start");
        } else if (node.researched || node.inProgress) {
            // Clicking something already done or already running is inspecting
            // it, not being refused -- only a locked node is a refusal.
            Audio::get().playSfx("research_select");
        } else {
            Audio::get().playSfx("deny");
        }
    }

    // ─── Bottom bar: research points + allocation + currently researching ───
    int barY2 = m_screenH - 50;
    DrawRectangle(0, barY2, m_screenW, 50, {10, 10, 15, 220});
    DrawText(TextFormat("Research Points: %d", m_researchPoints), 16, barY2 + 8, 16, hexToColor(m_config.accentColor));

    // Currently researching indicator
    if (m_researchActiveNode >= 0 && m_researchActiveNode < (int)m_researchNodes.size()) {
        auto& rn = m_researchNodes[m_researchActiveNode];
        int rx = m_screenW / 2 - 200;
        DrawText(TextFormat("Researching: %s (%d/%d RP)", rn.name.c_str(), rn.invested, rn.cost),
                 rx, barY2 + 8, 14, {200, 200, 100, 255});
    }

    auto cs2 = computeCountryIncome(m_playerCountryId);
    // Compute max affordable allocation (accounting for pacification budget)
    float baseExp2 = cs2.armyExpenses + cs2.navyExpenses + cs2.policyCosts + cs2.minorityCosts;
    float pacAmount2 = cs2.total * m_pacificationAllocation;
    float maxAllocFrac = (cs2.total > baseExp2 + pacAmount2) ? (cs2.total - baseExp2 - pacAmount2) / cs2.total : 0;
    if (maxAllocFrac > 1.0f) maxAllocFrac = 1.0f;
    if (m_researchAllocation > maxAllocFrac) m_researchAllocation = maxAllocFrac;
    int sliderX = 300;
    int sliderY = barY2 + 14;
    int sliderW = 300;
    int sliderH = 18;

    DrawText("Econ Allocation:", sliderX - 120, sliderY + 1, 13, LIGHTGRAY);
    DrawRectangle(sliderX, sliderY, sliderW, sliderH, {40, 40, 50, 200});
    // Show max affordable boundary
    int maxFillW = (int)(sliderW * maxAllocFrac);
    if (maxFillW > 0) DrawRectangle(sliderX, sliderY, maxFillW, sliderH, {40, 60, 40, 150});
    DrawRectangleLines(sliderX, sliderY, sliderW, sliderH, {80, 80, 100, 200});
    int fillW = (int)(sliderW * m_researchAllocation);
    if (fillW > 0) DrawRectangle(sliderX, sliderY, fillW, sliderH, {60, 120, 60, 200});
    DrawText(TextFormat("%d%%", (int)(m_researchAllocation * 100)), sliderX + sliderW + 8, sliderY + 1, 13, WHITE);
    float allocAmount2 = cs2.total * m_researchAllocation;
    int rpPerTurn = 1 + (int)(sqrtf(allocAmount2 * 0.5f));
    DrawText(TextFormat("(+%d RP/turn)", rpPerTurn), sliderX + sliderW + 60, sliderY + 1, 11, LIGHTGRAY);

    {
        const Rectangle allocBar = {(float)sliderX, (float)sliderY,
                                    (float)sliderW, (float)sliderH};
        float t = m_researchAllocation;
        if (sliderInteract(allocBar, /*steps=*/0, t, m_draggingResearchAlloc)) {
            // Clamped after the shared control has spoken: this slider's
            // ceiling is whatever the budget currently allows, not 1.0.
            m_researchAllocation = std::clamp(t, 0.0f, maxAllocFrac);
        }
    }
}
