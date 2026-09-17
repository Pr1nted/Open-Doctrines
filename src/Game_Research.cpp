#include "Game.h"
#include "util/LoadLog.h"
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
#include <cstdlib>
#include <map>

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
    if (node.mutexGroup > 0) {
        for (const auto& n : m_researchNodes)
            if (n.id != node.id && n.mutexGroup == node.mutexGroup && has(n.id)) return false;
        // ── A FORK SIBLING IN PROGRESS IS TAKEN TOO (OD_RESEARCH_MUTEX_FIX=0 turns it off) ──
        //
        // The check above sees only what is RESEARCHED. The player's
        // ResearchNode::isAvailable also refuses a sibling that is IN PROGRESS;
        // this per-country version did not, so an extra research group could
        // start off_tactics while the main slot was still on def_tactics and
        // the country finished holding both. Measured, journal 373: 41 / 71 / 52
        // fork nodes held beside their sibling per 400-turn world, off_tactics
        // 13/13, 25/25, 12/12. ON by default since journal 382, the user's call after
        // journal 376: 128 seeds per arm found no harm on any statistic and a land
        // gain on 1914:FRA (+3.33, p 0.003). OD_RESEARCH_MUTEX_FIX=0 restores the old
        // rule, for reproducing results measured before the flip.
        static const bool inProgressBlocks = [] {
            const char* e = std::getenv("OD_RESEARCH_MUTEX_FIX");
            return !(e && *e == '0');
        }();
        if (inProgressBlocks) {
            auto sibling = [&](int idx) {
                return idx >= 0 && idx < (int)m_researchNodes.size() &&
                       m_researchNodes[idx].id != node.id &&
                       m_researchNodes[idx].mutexGroup == node.mutexGroup;
            };
            auto act = m_countryResearchActive.find(countryId);
            if (act != m_countryResearchActive.end() && sibling(act->second)) return false;
            auto ext = m_countryResearchExtra.find(countryId);
            if (ext != m_countryResearchExtra.end())
                for (const auto& sl : ext->second)
                    if (sibling(sl.activeNode)) return false;
        }
    }
    return true;
}

// Per-country research progression for AI countries — the mirror of the
// player block in processUpgrades: allocation buys research points
// (rp = 1 + sqrt(spend/2)), points sink into the active node, completion
// lands in m_countryResearched where all the effect queries pick it up.
// ── [PROBE] WHICH SIDE OF A RESEARCH FORK, AND CAN ONE COUNTRY HOLD BOTH?
//    (OD_RESEARCH_PROBE, off) ──
//
// Journal 372 / backlog 100. Every mutex pair in the tree ties on cost and both
// AI choosers take the cheapest node with a strict `<`, so the first-declared
// side is always chosen. And isNodeAvailableFor blocks a sibling only once it
// is RESEARCHED, so an extra research group can start the other side while the
// main slot is still on the first. These counters say how often each happens.
// Counters only: nothing here is read by any decision, which the decision
// hash proves rather than asserts.
namespace {
struct ResearchProbeRow { int group = 0; long long mainSlot = 0, extraSlot = 0, siblingHeld = 0; };
std::map<std::string, ResearchProbeRow> s_researchProbe;
long long s_researchProbeDone[2] = {};      // completions: main slot, extra groups
long long s_researchProbeBeside = 0;        // extra group STARTED the main slot's mutex sibling
bool researchProbeOn() {
    static const bool on = std::getenv("OD_RESEARCH_PROBE") &&
                           atoi(std::getenv("OD_RESEARCH_PROBE")) != 0;
    return on;
}
void dumpResearchProbe() {
    static bool done = false;
    if (done) return;
    done = true;
    fprintf(stderr, "[RPROBE] AI research completions: main slot %lld, extra groups %lld; "
                    "extra group started the main slot's mutex sibling %lld times\n",
            s_researchProbeDone[0], s_researchProbeDone[1], s_researchProbeBeside);
    for (const auto& [id, r] : s_researchProbe)
        fprintf(stderr, "[RPROBE] mutex %d  %-20s completed main %lld  extra %lld  "
                        "with its sibling already held %lld\n",
                r.group, id.c_str(), r.mainSlot, r.extraSlot, r.siblingHeld);
}
void researchProbeNote(const Game& g, int cid, const ResearchNode& node, bool extra) {
    if (!researchProbeOn()) return;
    static const bool reg = (atexit(&dumpResearchProbe), true);
    (void)reg;
    ++s_researchProbeDone[extra ? 1 : 0];
    if (node.mutexGroup <= 0) return;
    auto& row = s_researchProbe[node.id];
    row.group = node.mutexGroup;
    ++(extra ? row.extraSlot : row.mainSlot);
    auto it = g.m_countryResearched.find(cid);
    if (it == g.m_countryResearched.end()) return;
    for (const auto& sib : g.m_researchNodes)
        if (sib.id != node.id && sib.mutexGroup == node.mutexGroup && it->second.count(sib.id)) {
            ++row.siblingHeld;
            break;
        }
}
}  // namespace

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

    // ── THE SAME BUDGET RULE THE PLAYER GETS ──
    //
    // Shares sum to 100 across the unlocked groups -- evenly, because the net
    // has no action for setting them and inventing a preference here would be
    // a rule with no author. An idle group's share is not lost: it is left in
    // the pool for the ones that are working, exactly as the player's is.
    // Gated so the AI half can be measured against itself. Without this the
    // control arm is the same build as the treatment, which is an A/B with one
    // arm -- and it reports a perfect null every time.
    static const bool groupsOff = getenv("OD_RGROUPS_OFF") != nullptr;
    const int unlocked = groupsOff ? 1
        : std::clamp(researchGroupsUnlocked(countryId), 1, RESEARCH_GROUPS_MAX);
    auto& extra = m_countryResearchExtra[countryId];

    // Each extra group takes the cheapest thing it can start that no other
    // group here is already on. Cheapest-available is the same fallback the
    // policy net's own research action uses when its focus has nothing left,
    // so this adds throughput without adding a second opinion about WHAT to
    // research -- which is the part the net owns.
    for (int g = 1; g < unlocked; ++g) {
        auto& sl = extra[g - 1];
        if (sl.activeNode >= 0 && sl.activeNode < (int)m_researchNodes.size()) {
            if (!m_countryResearched[countryId].count(m_researchNodes[sl.activeNode].id))
                continue;
            sl.activeNode = -1; sl.invested = 0;
        }
        int best = -1, bestCost = INT32_MAX;
        for (size_t i = 0; i < m_researchNodes.size(); ++i) {
            const ResearchNode& n = m_researchNodes[i];
            if (m_countryResearched[countryId].count(n.id)) continue;
            if (!isNodeAvailableFor(n, countryId)) continue;
            if ((int)i == active) continue;
            bool taken = false;
            for (int k = 1; k < unlocked; ++k)
                if (k != g && extra[k - 1].activeNode == (int)i) taken = true;
            if (taken) continue;
            if (n.cost < bestCost) { bestCost = n.cost; best = (int)i; }
        }
        if (researchProbeOn() && best >= 0 && active >= 0 && active < (int)m_researchNodes.size() &&
            m_researchNodes[best].mutexGroup > 0 &&
            m_researchNodes[best].mutexGroup == m_researchNodes[active].mutexGroup)
            ++s_researchProbeBeside;
        sl.activeNode = best;
        sl.invested = 0;
    }
    // Groups that have something to work on; the divisor, so nothing leaks.
    int working = 1;
    for (int g = 1; g < unlocked; ++g) if (extra[g - 1].activeNode >= 0) ++working;
    const int perGroup = pts / std::max(1, working);

    if (m_ai) m_ai->noteResearchFunded(countryId);
    int toSpend = std::min(perGroup, node.cost - invested);
    if (toSpend > 0) { invested += toSpend; pts -= toSpend; }
    if (invested >= node.cost) {
        if (m_ai) m_ai->noteResearchDone(countryId);
        researchProbeNote(*this, countryId, node, false);
        m_countryResearched[countryId].insert(node.id);
        active = -1;
        invested = 0;
        if (m_config.aiDebug) {
            const Country* c = m_countries.getCountry(countryId);
            printf("[RESEARCH] %s completed %s\n",
                   c ? c->name.c_str() : "?", node.id.c_str());
        }
    }
    for (int g = 1; g < unlocked; ++g) {
        auto& sl = extra[g - 1];
        if (sl.activeNode < 0 || sl.activeNode >= (int)m_researchNodes.size()) continue;
        ResearchNode& n2 = m_researchNodes[sl.activeNode];
        const int spend = std::min({perGroup, pts, n2.cost - sl.invested});
        if (spend <= 0) continue;
        sl.invested += spend;
        pts -= spend;
        if (sl.invested >= n2.cost) {
            if (m_ai) m_ai->noteResearchDone(countryId);
            researchProbeNote(*this, countryId, n2, true);
            m_countryResearched[countryId].insert(n2.id);
            sl.activeNode = -1;
            sl.invested = 0;
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

    // ─── Buildings > Industrial Efficiency ───
    //
    // The one answer to industry upkeep. The rate is total-levels/600 charged
    // against gross industry income, so it grows quadratically as a country
    // industrialises and was, before this branch, completely unavoidable --
    // no doctrine lever and no research node touched it. This is deliberately
    // its own subcategory rather than more nodes on the industry spine: it is a
    // parallel investment a player chooses INSTEAD of the next level, which is
    // the decision the upkeep curve is there to create.
    //
    // The three linear nodes total 45%, and the two exclusive capstones take it
    // to 60% or 55% with a sweetener. Upkeep is never removed: at the 45% cap a
    // huge empire still pays about a fifth of its industrial income, so the
    // brake on the snowball survives, it just stops being a wall.
    add("ind_eff1","Standardised Parts",
        "Interchangeable components and common tooling. Industry upkeep -15%",
        "efficiency","works",{"ind3"},30,80,120).industryUpkeepPct=15;
    add("ind_eff2","Assembly Line",
        "Continuous flow production. Industry upkeep -15%",
        "efficiency","works",{"ind_eff1"},60,80,320).industryUpkeepPct=15;
    add("ind_eff3","Scientific Management",
        "Time-and-motion study across the works. Industry upkeep -15%",
        "efficiency","works",{"ind_eff2"},110,80,520).industryUpkeepPct=15;
    add("ind_eff_grid","National Power Grid",
        "One grid, one tariff. Industry upkeep -15%",
        "efficiency","works",{"ind_eff3"},220,-90,720).industryUpkeepPct=15;
    m_researchNodes.back().mutexGroup=11;
    add("ind_eff_auto","Automation",
        "Machines that mind themselves. Industry upkeep -10%, population modifier +15%",
        "efficiency","works",{"ind_eff3"},220,250,720).industryUpkeepPct=10;
    m_researchNodes.back().popModPct=15;
    m_researchNodes.back().mutexGroup=11;

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
        "army","navy",{},8,750,80).unlockShips=true;
    add("navy2","Advanced Shipbuilding","Ship cost -10%",
        "army","navy",{"navy1"},12,750,180).navyCostPct=10;
    add("navy3","Naval Architecture","Ship cost -15%",
        "army","navy",{"navy2"},20,750,280).navyCostPct=15;
    add("navy4","Efficient Dockyards","Ship cost -20%",
        "army","navy",{"navy3"},25,640,380).navyCostPct=20;
    m_researchNodes.back().mutexGroup=5;
    add("navy5","Naval Logistics","Ship speed +25%",
        "army","navy",{"navy3"},25,860,380).navySpeedPct=25;
    m_researchNodes.back().mutexGroup=5;
    add("navy6","Fleet Modernization","Ship cost -15%",
        "army","navy",{"navy4","navy5"},30,750,480).navyCostPct=15;
    m_researchNodes.back().depsAny=true;
    add("navy7","Radar Technology","Ship defence +15%",
        "army","navy",{"navy6"},30,640,580).navyDefPct=15;
    m_researchNodes.back().mutexGroup=8;
    add("navy8","Naval Aviation","Ship attack +15%",
        "army","navy",{"navy6"},30,860,580).navyAtkPct=15;
    m_researchNodes.back().mutexGroup=8;
    add("navy9","Fleet Logistics","Ship speed +10%",
        "army","navy",{"navy7","navy8"},35,750,680).navySpeedPct=10;
    m_researchNodes.back().depsAny=true;
    add("navy10","Global Navy Doctrine","Ship attack +10%, Ship defence +10%",
        "army","navy",{"navy9"},40,750,780);
    m_researchNodes.back().navyAtkPct=10; m_researchNodes.back().navyDefPct=10;

    // ─── Army > Formations: WHAT the army is made of ──────────────────────
    //
    // A branch about composition rather than another column of percentages.
    // Each node unlocks a kind from TROOP_TYPES, exactly as the artillery
    // branch below unlocks an ammunition from ARTY_COSTS -- same mechanism,
    // same table-driven shape, nothing new invented.
    //
    // Line infantry has no node: it is what every army has always been made of,
    // and gating it would strand every existing save behind a technology it
    // never researched.
    //
    // The three are deliberately NOT a line. Militia hangs off basic training
    // because a country that can drill conscripts can raise a levy; the other
    // two need real logistics, because that is what an expensive soldier
    // actually costs a country. Cheap and early, or good and late.
    // ── ITS OWN TREE, NOT A FOURTH COLUMN OF THE ARMY ONE ──
    //
    // This branch was tried twice inside the army category and was wrong both
    // times, for the same underlying reason. At x=400 it drew straight through
    // the navy column; moved clear to x=1350 it drew short lines but pushed its
    // prerequisites the full width of the tree, so "Militia Levies" hung off a
    // wire that crossed both other branches to reach Basic Training.
    //
    // Neither position was the problem. The army category is three dense
    // columns that already fill the canvas, and a fourth thing whose deps reach
    // back into the first column cannot be placed in it without crossing
    // something. So it is a tree of its own, which is also what it IS: every
    // other army node makes the troops you already have better, and these
    // change what a province is able to raise at all.
    //
    // Its prerequisites still live in Army, and cross-tree deps deliberately
    // draw no line -- see the connection-line pass, which now skips them rather
    // than drawing to a node that is not on this screen.
    add("militia_levy","Militia Levies",
        "Raise militia: half the money and men of line infantry, and better on the defensive. Poor at attacking.",
        "formations","infantry",{"basic_training"},10,80,120);
    m_researchNodes.back().troopType="militia";
    add("assault_doctrine","Assault Infantry",
        "Raise assault infantry: expensive in men, hits hard, fights well on a narrow front",
        // ── THE LADDER IS ITS OWN ──
        //
        // This hung off Combined Arms, in the Army tree, so the Formations tree
        // read as three unconnected nodes with their prerequisites somewhere
        // else entirely -- and the one line it did draw, down to Mechanisation,
        // made it look like a chain that started nowhere. Militia first is also
        // the better rule: a country learns to raise a cheap levy before it
        // learns to raise an expensive assault division, and the tree now says
        // so. Mechanisation still needs Advanced Logistics from the Army tree,
        // which is the point of it -- a mechanised division IS logistics.
        "formations","infantry",{"militia_levy"},30,80,320);
    m_researchNodes.back().troopType="assault";
    add("mechanisation","Mechanisation",
        "Raise mechanised troops: costly in everything and thirsty for fuel, but the best use of a frontage",
        "formations","infantry",{"logistics","assault_doctrine"},55,80,520);
    m_researchNodes.back().troopType="mech";

    // ─── Army > Artillery (linear with final branch) ───
    add("arty1","Mortar","Kills 5% of troops in targeted province",
        "army","artillery",{},12,1250,80);
    m_researchNodes.back().artilleryType="mortar"; m_researchNodes.back().artilleryTroopKillPct=5;
    add("arty2","Light Artillery","Kills 10% of troops in targeted province",
        "army","artillery",{"arty1"},20,1250,180);
    m_researchNodes.back().artilleryType="light"; m_researchNodes.back().artilleryTroopKillPct=10;
    add("arty3","Heavy Artillery","Kills 20% of troops, 5% of population",
        "army","artillery",{"arty2"},30,1250,280);
    m_researchNodes.back().artilleryType="heavy"; m_researchNodes.back().artilleryTroopKillPct=20;
    m_researchNodes.back().artilleryPopKillPct=5;
    add("arty4a","Napalm","Kills 25% of troops, 15% of population",
        "army","artillery",{"arty3"},50,1100,380);
    m_researchNodes.back().artilleryType="napalm"; m_researchNodes.back().artilleryTroopKillPct=25;
    m_researchNodes.back().artilleryPopKillPct=15; m_researchNodes.back().mutexGroup=6;
    add("arty4b","Carpet Bombing","Kills 15% of troops, 10% of population. 50% chance to damage fortifications",
        "army","artillery",{"arty3"},50,1400,380);
    m_researchNodes.back().artilleryType="carpet"; m_researchNodes.back().artilleryTroopKillPct=15;
    m_researchNodes.back().artilleryPopKillPct=10; m_researchNodes.back().artilleryFortDamageChance=50;
    m_researchNodes.back().mutexGroup=6;
    add("arty5","Chemical Artillery","Kills 50% of troops, 30% of population",
        "army","artillery",{"arty4a","arty4b"},80,1250,480);
    m_researchNodes.back().depsAny=true;
    m_researchNodes.back().artilleryType="chemical"; m_researchNodes.back().artilleryTroopKillPct=50;
    m_researchNodes.back().artilleryPopKillPct=30;
    add("arty6a","Nuclear Shelling","Kills 75% of troops. Damages industry -3, fortifications -2",
        "army","artillery",{"arty5"},100,1100,580);
    m_researchNodes.back().artilleryType="nuclear"; m_researchNodes.back().artilleryTroopKillPct=75;
    m_researchNodes.back().artilleryIndustryDamage=3; m_researchNodes.back().artilleryFortDamage=2;
    m_researchNodes.back().mutexGroup=7;
    add("arty6b","Biological Shelling","Kills 80% of troops, 95% of population",
        "army","artillery",{"arty5"},100,1400,580);
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

    LoadLog() << "  Loaded " << m_researchNodes.size() << " research nodes" << std::endl;
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
    LoadLog() << "  Applied per-country starting research" << std::endl;
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

// Temporary instrument: the distribution of income per head, so the group
// unlock thresholds are chosen off the shipped world rather than guessed.
void Game::dumpResearchCapacity() {
    printf("[RCAP] turn %d, world median income per 1M = %.3f\n",
           m_turnNumber, researchMedianPerMillion());
    for (const auto& [cid, c] : m_countries.getAll()) {
        if (cid <= 0 || cid >= REBEL_CID_MIN) continue;
        long long pop = 0;
        int provs = 0;
        for (const auto& [pid, p] : m_provinces.getAllProvinces()) {
            if (p.countryId != cid) continue;
            ++provs;
            auto it = m_provincePopulations.find(pid);
            if (it != m_provincePopulations.end()) pop += it->second;
        }
        if (provs == 0 || pop <= 0) continue;
        auto cs = computeCountryIncome(cid);
        const float med = researchMedianPerMillion();
        const float mine = researchIncomePerMillion(cid);
        printf("[RCAP] %-28s pop %12lld  gross %8.1f  grossX %6.2f  per1m %7.3f  perX %5.2f  GROUPS %d\n",
               c.name.c_str(), pop, cs.total,
               researchMedianGross() > 0 ? cs.total / researchMedianGross() : 0.0f,
               mine, med > 0 ? mine / med : 0.0f, researchGroupsUnlocked(cid));
    }
}

void Game::rebuildResearchCapacity() const {
    if (m_rgroupCacheTurn == m_turnNumber) return;
    m_rgroupCacheTurn = m_turnNumber;
    m_rgroupPerMillion.clear();
    m_rgroupGross.clear();

    // ONE PASS over the provinces, accumulating by owner, rather than one pass
    // per country. This is the whole reason the answer is cached at all.
    std::unordered_map<int, long long> pop;
    for (const auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId <= 0 || p.countryId >= REBEL_CID_MIN) continue;
        auto it = m_provincePopulations.find(pid);
        if (it != m_provincePopulations.end()) pop[p.countryId] += it->second;
    }
    std::vector<float> all, gross;
    all.reserve(pop.size());
    gross.reserve(pop.size());
    for (const auto& [cid, n] : pop) {
        if (n <= 0) continue;
        m_rgroupGross[cid] = computeCountryIncome(cid).total;
        gross.push_back(m_rgroupGross[cid]);
        // Gross, not net. Net is what is LEFT after the war, and measured
        // across this project the AI runs its treasury at zero by design -- so
        // net would drop every country at war to one group the turn it
        // mobilised, which is a rule about warfare wearing the clothes of a
        // rule about industry.
        const float v = computeCountryIncome(cid).total / ((float)n / 1000000.0f);
        m_rgroupPerMillion[cid] = v;
        all.push_back(v);
    }
    if (all.empty()) { m_rgroupMedian = 0.0f; m_rgroupMedianGross = 0.0f; return; }
    std::sort(all.begin(), all.end());
    m_rgroupMedian = all[all.size() / 2];
    std::sort(gross.begin(), gross.end());
    m_rgroupMedianGross = gross[gross.size() / 2];
}

float Game::researchIncomePerMillion(int countryId) const {
    if (countryId <= 0 || countryId == SPC_CID) return 0.0f;
    rebuildResearchCapacity();
    auto it = m_rgroupPerMillion.find(countryId);
    return it == m_rgroupPerMillion.end() ? 0.0f : it->second;
}

float Game::researchMedianPerMillion() const {
    rebuildResearchCapacity();
    return m_rgroupMedian;
}

float Game::researchGross(int countryId) const {
    rebuildResearchCapacity();
    auto it = m_rgroupGross.find(countryId);
    return it == m_rgroupGross.end() ? 0.0f : it->second;
}

float Game::researchMedianGross() const {
    rebuildResearchCapacity();
    return m_rgroupMedianGross;
}

int Game::researchGroupsUnlocked(int countryId) const {
    if (countryId <= 0 || countryId == SPC_CID) return 1;
    // A scenario's word overrides the economy's. Checked first so a forced
    // count is not silently raised by the gate below, and clamped so a script
    // cannot invent a fourth programme the UI has no room for.
    {
        auto it = m_scriptResearchGroups.find(countryId);
        if (it != m_scriptResearchGroups.end() && it->second > 0)
            return std::clamp(it->second, 1, RESEARCH_GROUPS_MAX);
    }
    const float medGross = researchMedianGross();
    const float medHead  = researchMedianPerMillion();
    if (medGross <= 0.0f) return 1;        // nothing to compare against yet

    // THE GATE FIRST. An economy spread too thin over too many people supports
    // one programme however large it is in total; see the constants.
    if (medHead > 0.0f &&
        researchIncomePerMillion(countryId) < medHead * RGROUP_POVERTY_MULT)
        return 1;

    const float mine = researchGross(countryId);
    if (mine >= medGross * RGROUP3_GROSS_MULT) return 3;
    if (mine >= medGross * RGROUP2_GROSS_MULT) return 2;
    return 1;                               // ONE IS ALWAYS AVAILABLE
}

void Game::normaliseResearchShares(int changed) {
    const int unlocked = std::clamp(researchGroupsUnlocked(m_playerCountryId),
                                    1, RESEARCH_GROUPS_MAX);
    if (unlocked <= 1) {                       // one group owns the whole budget
        m_researchGroups[0].sharePct = 100;
        return;
    }
    changed = std::clamp(changed, 0, unlocked - 1);
    int mine = std::clamp(m_researchGroups[changed].sharePct, 0, 100);
    m_researchGroups[changed].sharePct = mine;

    int rest = 0;
    for (int g = 0; g < unlocked; ++g)
        if (g != changed) rest += std::max(0, m_researchGroups[g].sharePct);

    const int budget = 100 - mine;
    int handed = 0, last = -1;
    for (int g = 0; g < unlocked; ++g) {
        if (g == changed) continue;
        // In proportion to what they already hold, so dragging one slider does
        // not silently reorder the other two. With nothing to go on -- every
        // other share at zero -- they split what is left evenly.
        const int v = rest > 0
            ? (int)((long long)budget * std::max(0, m_researchGroups[g].sharePct) / rest)
            : budget / (unlocked - 1);
        m_researchGroups[g].sharePct = v;
        handed += v;
        last = g;
    }
    // Integer division loses up to a point or two; the remainder goes somewhere
    // rather than nowhere, or the sum reads 99 and the rule is a lie.
    if (last >= 0) m_researchGroups[last].sharePct += budget - handed;
    for (int g = unlocked; g < RESEARCH_GROUPS_MAX; ++g)
        m_researchGroups[g].sharePct = 0;
}

int Game::researchGroupPoints(int groupIndex, int totalPoints) const {
    // NORMALISED OVER THE GROUPS THAT ARE ACTUALLY WORKING. Shares that only
    // summed to 100 would leak the budget of an idle group into nothing, so a
    // country with one project running would research at a third speed for no
    // reason it could see. An idle group claims nothing.
    const int unlocked = researchGroupsUnlocked(m_playerCountryId);
    int denom = 0;
    for (int g = 0; g < unlocked && g < RESEARCH_GROUPS_MAX; ++g)
        if (m_researchGroups[g].activeNode >= 0)
            denom += std::max(1, m_researchGroups[g].sharePct);
    if (denom <= 0 || groupIndex >= unlocked) return 0;
    if (m_researchGroups[groupIndex].activeNode < 0) return 0;
    const int mine = std::max(1, m_researchGroups[groupIndex].sharePct);
    return (int)((long long)totalPoints * mine / denom);
}

int Game::researchAutoNext(int groupIndex, int countryId) const {
    if (groupIndex < 0 || groupIndex >= RESEARCH_GROUPS_MAX) return -1;
    const int last = m_researchGroups[groupIndex].lastNode;
    if (last < 0 || last >= (int)m_researchNodes.size()) return -1;
    const ResearchNode& from = m_researchNodes[last];

    // ── ONE CANDIDATE MEANS NO DECISION ──
    //
    // Scoped to the BRANCH the group was already working, not the whole tree:
    // "carry on down this line" is a continuation, "start a different line" is
    // a choice, and the point of the setting is to make the first automatic
    // without ever making the second. Two open nodes on the branch is the
    // decision the player asked to be stopped for; none is the end of it.
    int found = -1;
    for (size_t i = 0; i < m_researchNodes.size(); ++i) {
        const ResearchNode& n = m_researchNodes[i];
        if (n.category != from.category || n.subcategory != from.subcategory) continue;
        if (n.researched || hasResearched(n.id, countryId)) continue;
        if (!n.isAvailable(m_researchNodes)) continue;
        if (found >= 0) return -1;          // a decision, not a continuation
        found = (int)i;
    }
    return found;
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

    // THE BUDGET IS DIVIDED ONCE, BEFORE ANY OF IT IS SPENT. Handing the first
    // group the whole pool and the second whatever survived would make the
    // order of this loop a game rule.
    const int unlocked = researchGroupsUnlocked(countryId);
    int budget[RESEARCH_GROUPS_MAX] = {};
    for (int g = 0; g < unlocked && g < RESEARCH_GROUPS_MAX; ++g)
        budget[g] = researchGroupPoints(g, m_researchPoints);

    for (int g = 0; g < unlocked && g < RESEARCH_GROUPS_MAX; ++g) {
        ResearchGroup& grp = m_researchGroups[g];
        if (grp.activeNode < 0 || grp.activeNode >= (int)m_researchNodes.size()) continue;
        auto& node = m_researchNodes[grp.activeNode];
        if (hasResearched(node.id, countryId) || node.researched) {
            node.inProgress = false;
            grp.activeNode = -1;
            continue;
        }
        int toSpend = std::min({budget[g], m_researchPoints, node.cost - node.invested});
        if (toSpend <= 0) continue;
        node.invested += toSpend;
        m_researchPoints -= toSpend;
        if (node.invested >= node.cost) {
            node.researched = true;
            node.inProgress = false;
            m_countryResearched[countryId].insert(node.id);
            grp.lastNode = grp.activeNode;
            grp.activeNode = -1;
            if (countryId == m_playerCountryId) {
                Audio::get().playSfx("research_complete");
                printf("[RESEARCH] %s completed!\n", node.name.c_str());
                m_researchAlert = true;
            }
            // And walk on, if it was told to and there is only one way to walk.
            if (grp.autoAdvance) {
                const int next = researchAutoNext(g, countryId);
                if (next >= 0) {
                    grp.activeNode = next;
                    m_researchNodes[next].inProgress = true;
                }
            }
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
float Game::getResearchEffect(const std::string& effectField, int countryId) const {
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
        else if (effectField == "industryUpkeepPct") total += n.industryUpkeepPct;
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

// Research plus doctrines. The research sum is taken first and the doctrine
// levers added to it in the same order as before the split, so the float
// result is unchanged -- which the decision hash checks (journal 373).
float Game::getTotalEffect(const std::string& effectField, int countryId) const {
    const int cid = (countryId >= 0) ? countryId : m_playerCountryId;
    float total = getResearchEffect(effectField, countryId);
    // ...AND THE DOCTRINES IN FORCE. See Policy::levers: these were parsed into
    // nothing and summed nowhere, so every doctrine's advertised effects were
    // decoration. Only doctrines that have finished implementing count --
    // turnsRemaining > 0 is still being enacted, < 0 is not in force at all --
    // which is what the implementation delay is for.
    for (const auto& ap : m_activePolicies) {
        if (ap.countryId != cid || ap.turnsRemaining != 0) continue;
        for (const auto& q : m_allPolicies) {
            if (q.id != ap.policyId) continue;
            auto lv = q.levers.find(effectField);
            // Scaled by how long it has been held: see Game::policyTenure for
            // why that is the whole mechanic, and why it is 1.0 with the flag
            // off so this line is bit-identical to before.
            if (lv != q.levers.end()) total += lv->second * policyTenure(ap);
            break;
        }
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
        return T("This country has no political compass.");
    const auto& pc = it->second;

    // economic runs -100 (left) to +100 (right); social -100 (authoritarian)
    // to +100 (libertarian). See PoliticalCompass.
    if (pc.economic < p.minEcon)
        return TextFormat(T("Your economy is too far left for this (%.0f; needs %.0f or higher)."),
                          pc.economic, p.minEcon);
    if (pc.economic > p.maxEcon)
        return TextFormat(T("Your economy is too far right for this (%.0f; needs %.0f or lower)."),
                          pc.economic, p.maxEcon);
    if (pc.social < p.minSoc)
        return TextFormat(T("Your government is too authoritarian for this (%.0f; needs %.0f or higher)."),
                          pc.social, p.minSoc);
    if (pc.social > p.maxSoc)
        return TextFormat(T("Your government is too libertarian for this (%.0f; needs %.0f or lower)."),
                          pc.social, p.maxSoc);

    auto displayName = [&](const std::string& id) {
        for (const auto& q : m_allPolicies)
            if (q.id == id) return q.name;
        return id;
    };

    for (const auto& ap : m_activePolicies) {
        if (ap.countryId != countryId || ap.turnsRemaining < 0) continue;
        if (ap.policyId == p.id)
            return ap.turnsRemaining > 0 ? T("Already being implemented.")
                                         : T("Already in force.");
        if (policiesConflict(p.id, ap.policyId))
            // One sentence with the name in it, not three pieces glued
            // together: the name does not sit in the middle in every language.
            return TextFormat(T("Conflicts with %s, which is active. Repeal it first."),
                              od::i18n::tr(displayName(ap.policyId)));
    }

    auto cs = computeCountryIncome(countryId);
    float available = cs.total - (cs.armyExpenses + cs.navyExpenses + cs.policyCosts + cs.minorityCosts);
    available = std::max(0.0f, available);
    // FULL PRICE, deliberately, even though policyUpkeep charges less while the
    // doctrine is being built. The phased bill is relief during construction,
    // not a licence to sign something you could never sustain -- a country let
    // in on the first turn's discount would enact, ramp, and be repealed by its
    // own austerity three turns later, which is the churn the commitment rules
    // exist to stop.
    if (p.costPerTurn > 0 && available < p.costPerTurn)
        return TextFormat(T("Costs %d/turn and only %.0f is spare."), p.costPerTurn, available);

    return "";
}

bool Game::canCountryEnactPolicy(int countryId, const Policy& p) const {
    return policyBlockReason(countryId, p).empty();
}

namespace {

/// What a research branch is CALLED, as against what the data calls it.
///
/// The tree draws node.subcategory straight from the definition -- lowercase
/// "fortifications", "ports", "efficiency" -- which reads as a leaked field
/// name and is the one shape tools/i18n_extract.py refuses to collect, on the
/// grounds that a bare lowercase word is usually an id. It usually is. So the
/// id stays an id and the heading is a heading.
/// The display name of a tree, for naming the one a prerequisite lives in.
const char* categoryLabel(const std::string& id) {
    if (id == "buildings")  return "Buildings";
    if (id == "efficiency") return "Efficiency";
    if (id == "army")       return "Army";
    if (id == "formations") return "Formations";
    if (id == "population") return "Population";
    if (id == "misc")       return "Misc";
    return id.c_str();
}

const char* subcategoryLabel(const std::string& id) {
    if (id == "army")           return T("Army");
    if (id == "navy")           return T("Navy");
    if (id == "artillery")      return T("Artillery");
    if (id == "conscription")   return T("Conscription");
    if (id == "efficiency")     return T("Efficiency");
    if (id == "fortifications") return T("Fortifications");
    if (id == "indoctrination") return T("Indoctrination");
    if (id == "industry")       return T("Industry");
    if (id == "ports")          return T("Ports");
    if (id == "tourism")        return T("Tourism");
    if (id == "misc")           return T("Miscellaneous");
    return T(id);
}

}  // namespace


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
    DrawText(T("ESC to close"), m_screenW - 140, 55, 14, Color{120, 120, 140, 150});

    // ─── Category tabs ───
    const char* catNames[] = {"Buildings", "Efficiency", "Army", "Formations",
                              "Population", "Misc"};
    const char* catKeys[] = {"buildings", "efficiency", "army", "formations",
                             "population", "misc"};
    int catCount = 6;
    int catTabY = 8;
    int catTabH = 30;
    int catTabStartX = 16;
    // FITTED, NOT FIXED. This was a hard 200 px a tab, which put the last of
    // four at x=616 and would have put the last of six at 1016 -- off the right
    // of any window narrower than that, with no way to reach the tabs it hid.
    // Adding a tree must not be able to push another one off the screen.
    int catSpacing = std::min(200, (m_screenW - catTabStartX * 2) / catCount);
    if (catSpacing < 90) catSpacing = 90;
    for (int c = 0; c < catCount; ++c) {
        int tx = catTabStartX + c * catSpacing;
        Rectangle cr = {(float)tx, (float)catTabY, (float)(catSpacing - 8), (float)catTabH};
        bool active = (c == m_researchTab);
        bool hovered = CheckCollisionPointRec(mouse, cr);
        Color bg = active ? Color{60, 60, 80, 200} : (hovered ? Color{40, 40, 60, 180} : Color{30, 30, 50, 150});
        DrawRectangleRounded(cr, 0.1f, 6, bg);
        if (active) DrawRectangleRoundedLines(cr, 0.1f, 6, hexToColor(m_config.accent()));
        int tw = MeasureText(catNames[c], 16);
        DrawText(catNames[c], tx + (catSpacing - 8 - tw) / 2, catTabY + 6, 16, active ? hexToColor(m_config.accent()) : LIGHTGRAY);
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

    float wheel = odMouseWheel();
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
        DrawText(T("No research trees in this category"), m_screenW / 2 - 100, m_screenH / 2, 14, LIGHTGRAY);
    }

    // ─── Subcategory labels ───
    std::string lastSubcat;
    for (int idx : catIndices) {
        auto& node = m_researchNodes[idx];
        if (node.subcategory != lastSubcat) {
            lastSubcat = node.subcategory;
            int lx = (int)(node.posX * m_researchZoom + m_researchCamX);
            int ly = (int)(node.posY * m_researchZoom + m_researchCamY);
            DrawText(subcategoryLabel(node.subcategory),
                     lx, ly - (int)(26 * m_researchZoom), (int)(14 * m_researchZoom), {180, 180, 200, 200});
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
                // A PREREQUISITE IN ANOTHER TREE GETS NO LINE. Positions are
                // absolute and per-tree, so drawing to a node this tab is not
                // showing puts a wire across the canvas to a coordinate that
                // means nothing here. The dependency still binds -- it is
                // checked globally, and the node stays locked until it is met.
                if (pn.category != node.category) continue;
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
                    if (sn.category != node.category) continue;
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
            border = hexToColor(m_config.accent());
        }

        DrawRectangleRounded(r, 0.15f, 8, bg);
        DrawRectangleRoundedLines(r, 0.15f, 8, border);
        int fs = (int)(12 * m_researchZoom); if (fs < 7) fs = 7; if (fs > 20) fs = 20;
        int textW = MeasureText(T(node.name), fs);
        DrawText(T(node.name), nx + nodeW / 2 - textW / 2, ny + 4, fs, WHITE);
        int infoFs = (int)(8 * m_researchZoom); if (infoFs < 6) infoFs = 6; if (infoFs > 14) infoFs = 14;
        if (node.inProgress) {
            float pct = (float)node.invested / node.cost;
            int barH = (int)(5 * m_researchZoom); if (barH < 3) barH = 3;
            int barY = ny + nodeH - barH - 4;
            DrawRectangle(nx + 4, barY, nodeW - 8, barH, {60, 60, 60, 200});
            DrawRectangle(nx + 4, barY, (int)((nodeW - 8) * pct), barH, {220, 200, 60, 255});
            DrawText(TextFormat(T("%d/%d RP"), node.invested, node.cost), nx + 4, barY - infoFs - 2, infoFs, {200, 200, 200, 200});
        } else if (!node.researched) {
            DrawText(TextFormat(T("%d RP"), node.cost), nx + 4, ny + nodeH - infoFs - 6, infoFs, {160, 160, 160, 200});
        } else {
            DrawText(T("DONE"), nx + 4, ny + nodeH - infoFs - 6, infoFs, {100, 200, 100, 200});
        }
    }

    // ─── Hover tooltip ───
    if (m_researchHoveredNode >= 0) {
        auto& node = m_researchNodes[m_researchHoveredNode];
        // ── WHAT IT STILL NEEDS, BY NAME ──
        //
        // A prerequisite in another tree draws no line, so a locked node in
        // Formations used to say only "Research prerequisites first" with
        // nothing on screen to point at -- the whole reason the line was there
        // in the first place. So the missing ones are named, and the tree they
        // are in is named with them, because that is where the player has to go.
        std::string needs;
        if (!node.researched && !node.inProgress) {
            for (const auto& req : node.deps) {
                for (const auto& pn : m_researchNodes) {
                    if (pn.id != req || pn.researched) continue;
                    if (!needs.empty()) needs += ", ";
                    needs += T(pn.name);
                    if (pn.category != node.category)
                        needs += std::string(" (") + categoryLabel(pn.category) + ")";
                    break;
                }
            }
        }
        int tw0 = MeasureText(T(node.name), 14);
        int dw = MeasureText(T(node.desc), 11);
        int nw = needs.empty() ? 0 : MeasureText(needs.c_str(), 11);
        int tipW = std::max(std::max(tw0, dw), nw) + 20;
        int tipX = (int)mouse.x + 16; if (tipX + tipW > m_screenW) tipX = m_screenW - tipW - 8;
        int tipY = (int)mouse.y + 16;
        int tipH = 48;
        if (!node.researched && !node.inProgress && !node.isAvailable(m_researchNodes)) tipH += 12;
        if (!needs.empty()) tipH += 14;
        DrawRectangle(tipX, tipY, tipW, tipH, {10, 10, 20, 220});
        DrawRectangleLines(tipX, tipY, tipW, tipH, {100, 100, 140, 200});
        DrawText(T(node.name), tipX + 10, tipY + 4, 14, WHITE);
        DrawText(T(node.desc), tipX + 10, tipY + 22, 11, {200, 200, 200, 255});
        if (!node.researched && !node.inProgress && !node.isAvailable(m_researchNodes))
            DrawText(T("LOCKED - Research prerequisites first"), tipX + 10, tipY + 36, 10, {200, 100, 100, 255});
        if (!needs.empty())
            DrawText(TextFormat(T("Needs: %s"), needs.c_str()), tipX + 10, tipY + 48, 11,
                     Color{200, 180, 120, 255});
    }

    // ─── Click to start research ───
    if (!m_researchDragging && m_researchHoveredNode >= 0 && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        auto& node = m_researchNodes[m_researchHoveredNode];
        if (!node.researched && !node.inProgress && node.isAvailable(m_researchNodes)) {
            // Into the SELECTED group, replacing whatever it was working on.
            ResearchGroup& grp = m_researchGroups[std::clamp(m_researchGroupSel, 0,
                                                             RESEARCH_GROUPS_MAX - 1)];
            if (grp.activeNode >= 0) m_researchNodes[grp.activeNode].inProgress = false;
            node.inProgress = true;
            grp.activeNode = m_researchHoveredNode;
            grp.lastNode = m_researchHoveredNode;
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
    // TALLER, BECAUSE IT NOW CARRIES THREE PROGRAMMES. At 50 px the group
    // rows were drawn straight through the economy slider -- they started at
    // screen-centre minus 240, which on a 1600 canvas is x=560, and the slider
    // runs 300 to 600 with its readout past 760.
    // ── AND IF THEY DO NOT FIT BESIDE THE SLIDER, THEY GO BELOW IT ──
    //
    // The cards sit to the right of the economy allocation, which needs the
    // first 800 px. On a phone canvas there is no such room -- 402 px wide,
    // the whole strip is off the right edge -- and the loop that skips a card
    // it cannot fit would have skipped ALL THREE, leaving the primary control
    // of the research system simply unreachable on that device with nothing on
    // screen to say so. Cards this narrow get their own row instead.
    const int cardsGap = 10, cardsMinW = 96;
    const int cardsNeed = RESEARCH_GROUPS_MAX * cardsMinW
                        + cardsGap * (RESEARCH_GROUPS_MAX - 1);
    const bool cardsBesideSlider = (m_screenW - 800 - 16) >= cardsNeed;
    const int cardsX = cardsBesideSlider ? 800 : 16;
    const int barH2 = cardsBesideSlider ? 92 : 92 + 78;
    int barY2 = m_screenH - barH2;
    DrawRectangle(0, barY2, m_screenW, barH2, {10, 10, 15, 220});
    DrawText(TextFormat(T("Research Points: %d"), m_researchPoints), 16, barY2 + 6, 16, hexToColor(m_config.accent()));

    // What each group is working on, one line each.
    {
        const int unlocked = researchGroupsUnlocked(m_playerCountryId);
        // A GROUP UNLOCKING CHANGES THE DIVISOR. Fixing the sum only when a
        // slider moves would leave a country that just earned its second group
        // showing 100/50, and a country that lost one showing 50/50 of a budget
        // one group now owns outright.
        int shareSum = 0;
        for (int g = 0; g < unlocked && g < RESEARCH_GROUPS_MAX; ++g)
            shareSum += m_researchGroups[g].sharePct;
        if (shareSum != 100) normaliseResearchShares(m_researchGroupSel);

        // ── CARDS, NOT ROWS ──
        //
        // A group carries four things -- what it is building, how far along it
        // is, what share of the budget it gets, and whether it walks on alone.
        // Laid out as a 21 px strip those became a name clipped to fit, no
        // progress at all, a slider the width of a thumbnail and a button
        // squeezed against the screen edge. They are the primary control of a
        // whole subsystem and were the smallest thing on the screen.
        //
        // Side by side, because the question a player asks here is a
        // comparison -- which programme gets the budget -- and stacked rows
        // make you read three lines to answer it.
        // CLEAR OF THE ECONOMY SLIDER. That runs 300 to 600, its percentage
        // sits at 608 and the "(+N RP/turn)" readout after it reaches about
        // 780 -- so a card starting at 700 was drawn straight through the one
        // number telling the player how much research their allocation buys.
        const int gap = cardsGap;
        const int availW = m_screenW - cardsX - 16;
        const int cardW = std::clamp((availW - gap * (RESEARCH_GROUPS_MAX - 1))
                                     / RESEARCH_GROUPS_MAX, cardsMinW, 260);
        const int cardH = 80;
        for (int g = 0; g < RESEARCH_GROUPS_MAX; ++g) {
            ResearchGroup& grp = m_researchGroups[g];
            const bool live = (g < unlocked);
            const bool sel  = (m_researchGroupSel == g);
            // On the narrow layout they hang below the slider row rather than
            // beside it; see cardsBesideSlider.
            Rectangle card = {(float)(cardsX + g * (cardW + gap)),
                              (float)(barY2 + (cardsBesideSlider ? 6 : 84)),
                              (float)cardW, (float)cardH};
            if (card.x + card.width > m_screenW - 4) break;

            // ── A LOCKED GROUP SAYS WHY, AND BY HOW MUCH ──
            //
            // "Locked" on its own is a wall. Both halves of the rule are
            // ratios the player can move -- build industry, or hold fewer
            // mouths -- so the card names whichever one is actually stopping
            // them. Telling a big poor empire to grow its economy points it at
            // the one thing that will not help, and that empire is precisely
            // the case the poverty gate exists for.
            if (!live) {
                DrawRectangleRounded(card, 0.12f, 6, Color{17, 18, 24, 200});
                DrawRectangleRoundedLines(card, 0.12f, 6, Color{46, 48, 60, 160});
                DrawText(TextFormat(T("Group %d"), g + 1),
                         (int)card.x + 10, (int)card.y + 8, 13, Color{95, 98, 112, 255});
                DrawText(T("locked"), (int)card.x + 10, (int)card.y + 26, 11,
                         Color{120, 100, 70, 255});
                const float medHead  = researchMedianPerMillion();
                const float medGross = researchMedianGross();
                const float perX  = medHead  > 0.0f
                    ? researchIncomePerMillion(m_playerCountryId) / medHead : 0.0f;
                const float grossX = medGross > 0.0f
                    ? researchGross(m_playerCountryId) / medGross : 0.0f;
                int lfs = 11;
                const std::string why = perX < RGROUP_POVERTY_MULT
                    ? std::string(TextFormat(T("income per head %.2fx, needs %.2fx"),
                                             perX, RGROUP_POVERTY_MULT))
                    : std::string(TextFormat(T("economy %.1fx, needs %.0fx"), grossX,
                                             (g == 1) ? RGROUP2_GROSS_MULT
                                                      : RGROUP3_GROSS_MULT));
                const std::string fit = odText::fitToWidth(why, cardW - 20, lfs, 8);
                DrawText(fit.c_str(), (int)card.x + 10, (int)card.y + 42, lfs,
                         Color{105, 108, 122, 255});
                continue;
            }

            const bool hov = !m_paused && CheckCollisionPointRec(mouse, card);
            DrawRectangleRounded(card, 0.12f, 6,
                sel ? Color{34, 40, 54, 225} : hov ? Color{27, 31, 42, 210}
                                                   : Color{19, 21, 28, 195});
            DrawRectangleRoundedLines(card, 0.12f, 6,
                sel ? hexToColor(m_config.accent()) : Color{60, 64, 80, 170});

            DrawText(TextFormat(T("Group %d"), g + 1),
                     (int)card.x + 10, (int)card.y + 7, 13,
                     sel ? hexToColor(m_config.accent()) : Color{170, 175, 195, 255});

            // ── AND THE FOLLOW SWITCH, WHERE IT FITS ──
            Rectangle ab = {card.x + card.width - 50, card.y + 6, 44, 15};
            const bool ah = !m_paused && CheckCollisionPointRec(mouse, ab);
            DrawRectangleRounded(ab, 0.35f, 4,
                grp.autoAdvance ? Color{50, 84, 52, 225}
                                : ah ? Color{40, 44, 58, 205} : Color{24, 26, 34, 190});
            DrawRectangleRoundedLines(ab, 0.35f, 4,
                grp.autoAdvance ? Color{110, 190, 120, 215} : Color{70, 74, 92, 165});
            const char* al = T("Auto");
            DrawText(al, (int)(ab.x + (ab.width - MeasureText(al, 10)) / 2),
                     (int)(ab.y + 3), 10,
                     grp.autoAdvance ? WHITE : Color{160, 165, 185, 255});
            if (ah) m_uiHint = T("Follow this branch automatically, stopping at any real choice");

            // ── WHAT IT IS BUILDING, AND HOW FAR ALONG ──
            // The progress bar is the thing the strip had no room for at all,
            // and it is the only part of this that changes on its own.
            int nfs = 12;
            std::string what = T("idle");
            float frac = 0.0f;
            if (grp.activeNode >= 0 && grp.activeNode < (int)m_researchNodes.size()) {
                const auto& rn = m_researchNodes[grp.activeNode];
                what = T(rn.name);
                frac = rn.cost > 0 ? (float)rn.invested / rn.cost : 0.0f;
            }
            const std::string nl = odText::fitToWidth(what, cardW - 20, nfs, 9);
            DrawText(nl.c_str(), (int)card.x + 10, (int)card.y + 26, nfs,
                     grp.activeNode >= 0 ? Color{225, 222, 205, 255}
                                         : Color{112, 116, 132, 255});

            const Rectangle pb = {card.x + 10, card.y + 44, (float)cardW - 20, 9};
            DrawRectangle((int)pb.x, (int)pb.y, (int)pb.width, (int)pb.height,
                          Color{30, 32, 42, 220});
            if (frac > 0.0f)
                DrawRectangle((int)pb.x, (int)pb.y,
                              (int)(pb.width * std::clamp(frac, 0.0f, 1.0f)),
                              (int)pb.height, Color{200, 180, 70, 235});
            DrawRectangleLines((int)pb.x, (int)pb.y, (int)pb.width, (int)pb.height,
                               Color{62, 66, 84, 190});
            if (grp.activeNode >= 0 && grp.activeNode < (int)m_researchNodes.size()) {
                const auto& rn = m_researchNodes[grp.activeNode];
                const char* pt = TextFormat("%d/%d", rn.invested, rn.cost);
                // BESIDE THE BAR, NOT ON IT. Dark text sat legibly on the
                // filled part and vanished into the empty part -- so a project
                // just started, which is exactly when a player wants the
                // figure, was the case that could not be read.
                const int px = (int)(pb.x + pb.width - MeasureText(pt, 9) - 3);
                DrawText(pt, px + 1, (int)pb.y + 1, 9, Color{0, 0, 0, 190});
                DrawText(pt, px, (int)pb.y, 9, Color{232, 228, 210, 255});
            }

            // ── ITS CLAIM ON THE TURN'S POINTS ──
            const Rectangle sh = {card.x + 10, card.y + 60, (float)cardW - 58, 14};
            DrawRectangle((int)sh.x, (int)sh.y, (int)sh.width, (int)sh.height,
                          Color{30, 32, 42, 215});
            DrawRectangle((int)sh.x, (int)sh.y,
                          (int)(sh.width * grp.sharePct / 100), (int)sh.height,
                          Color{58, 100, 62, 220});
            DrawRectangleLines((int)sh.x, (int)sh.y, (int)sh.width, (int)sh.height,
                               Color{70, 74, 92, 200});
            DrawText(TextFormat("%d%%", grp.sharePct),
                     (int)(sh.x + sh.width + 6), (int)sh.y + 2, 11,
                     Color{190, 195, 215, 255});
            if (!m_paused && CheckCollisionPointRec(mouse, sh) &&
                IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                grp.sharePct = std::clamp((int)((mouse.x - sh.x) / sh.width * 100), 0, 100);
                normaliseResearchShares(g);   // the three always add to 100
                m_researchGroupSel = g;
            }

            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !m_paused) {
                if (ah) {
                    grp.autoAdvance = !grp.autoAdvance;
                    Audio::get().playSfx("click_soft");
                    // Turned on while idle, it should start now rather than
                    // wait for a project to finish first.
                    if (grp.autoAdvance && grp.activeNode < 0) {
                        const int next = researchAutoNext(g, m_playerCountryId);
                        if (next >= 0) {
                            grp.activeNode = next;
                            m_researchNodes[next].inProgress = true;
                        }
                    }
                } else if (hov && m_researchGroupSel != g) {
                    m_researchGroupSel = g;
                    Audio::get().playSfx("tab_switch");
                }
            }
        }
    }

    auto cs2 = computeCountryIncome(m_playerCountryId);
    // Compute max affordable allocation (accounting for pacification budget)
    float baseExp2 = cs2.armyExpenses + cs2.navyExpenses + cs2.policyCosts + cs2.minorityCosts;
    float pacAmount2 = cs2.total * m_pacificationAllocation;
    float maxAllocFrac = (cs2.total > baseExp2 + pacAmount2) ? (cs2.total - baseExp2 - pacAmount2) / cs2.total : 0;
    if (maxAllocFrac > 1.0f) maxAllocFrac = 1.0f;
    if (m_researchAllocation > maxAllocFrac) m_researchAllocation = maxAllocFrac;
    int sliderX = 300;
    int sliderY = barY2 + 30;
    int sliderW = 300;
    int sliderH = 18;

    DrawText(T("Econ Allocation:"), sliderX - 120, sliderY + 1, 13, LIGHTGRAY);
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
    DrawText(TextFormat(T("(+%d RP/turn)"), rpPerTurn), sliderX + sliderW + 60, sliderY + 1, 11, LIGHTGRAY);

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


// === unlockedTroopTypes ===
//
// See Game::unlockedTroopTypes. Found by walking the research nodes for a
// `troopType`, exactly as an ammunition is found by `artilleryType` -- so
// adding a kind is a table entry and a node, not a fourth place to edit.
std::vector<TroopType> Game::unlockedTroopTypes(int countryId) const {
    // Line infantry always. It is what every army in every existing save is
    // made of, and gating it would strand those campaigns behind a technology
    // they never researched.
    std::vector<TroopType> out{TROOP_LINE};
    for (const auto& n : m_researchNodes) {
        if (n.troopType.empty()) continue;
        if (!hasResearched(n.id, countryId)) continue;
        const TroopType t = troopTypeFromId(n.troopType.c_str());
        if (t == TROOP_LINE) continue;                       // never doubled
        if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    }
    // Sorted by the enum so the panel's order, the AI's order and a replay's
    // order are the same order.
    std::sort(out.begin(), out.end());
    return out;
}

bool Game::troopTypeUnlocked(int countryId, TroopType t) const {
    if (t == TROOP_LINE) return true;
    const auto v = unlockedTroopTypes(countryId);
    return std::find(v.begin(), v.end(), t) != v.end();
}
