#include "Game.h"
#include "Palette.h"
#include "GameInternals.h"
#include "Keybinds.h"
#include "raymath.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdio>
#include "BuildCosts.h"

// === specializationBoostPct / provinceResourceIncome ===
//
// See the note on the declarations: the boost is applied here, on the way out,
// rather than written into resourceIncome -- which stays the province's
// unspecialised base so that re-specialising cannot compound it and so that
// every save written before specialisation paid anything still reads right.
const char* const Game::SPEC_RESOURCES[5] = {"Oil", "Gold", "Metal", "Rubber", "Gemstones"};

float Game::specializationBoostPct(int pid) const {
    auto indIt = m_provinceIndustry.find(pid);
    if (indIt == m_provinceIndustry.end() || indIt->second.specialization.empty()) return 0.0f;
    auto resIt = m_provinceResources.find(pid);
    if (resIt == m_provinceResources.end()) return 0.0f;
    const std::string& s = indIt->second.specialization;
    const ProvinceResources& r = resIt->second;
    if (s == "Oil")       return r.oil.boost;
    if (s == "Gold")      return r.gold.boost;
    if (s == "Metal")     return r.metal.boost;
    if (s == "Rubber")    return r.rubber.boost;
    if (s == "Gemstones") return r.gemstones.boost;
    return 0.0f;
}

float Game::provinceResourceIncome(int pid) const {
    auto indIt = m_provinceIndustry.find(pid);
    if (indIt == m_provinceIndustry.end()) return 0.0f;
    return indIt->second.resourceIncome * (1.0f + specializationBoostPct(pid) / 100.0f);
}

const char* Game::bestSpecializationFor(int pid) const {
    auto resIt = m_provinceResources.find(pid);
    if (resIt == m_provinceResources.end()) return nullptr;
    const ProvinceResources& r = resIt->second;
    const float boosts[5] = {r.oil.boost, r.gold.boost, r.metal.boost,
                             r.rubber.boost, r.gemstones.boost};
    int best = -1;
    for (int i = 0; i < 5; ++i)
        if (boosts[i] > 0.0f && (best < 0 || boosts[i] > boosts[best])) best = i;
    return best < 0 ? nullptr : SPEC_RESOURCES[best];
}

// === projectIncome ===
//
// See the declaration. Today's snapshot, walked forward over the build queues.
//
// EVERYTHING ELSE IS HELD STILL ON PURPOSE. A projection that guessed at
// conquest, population growth or what the neighbours will do would be a
// forecast, and a wrong one; this answers the narrower question a player
// answers by glancing at their build queue -- "of what I have already paid
// for, what has arrived by then, and what is it costing me?" That is the only
// part of the future this game is arithmetic about, and it is the part every
// multi-turn purchase decision actually turns on.
CountryIncomeSnapshot Game::projectIncome(int countryId, int turns) const {
    CountryIncomeSnapshot cs = computeCountryIncome(countryId);
    if (turns <= 0) return cs;
    // What today costs, kept so the deltas below are added once each rather
    // than re-derived from a second lookup of the same snapshot.
    const float navyNow = cs.navyExpenses;

    auto ownedByUs = [&](int pid) {
        const Province* p = m_provinces.getProvinceById(pid);
        return p && p->countryId == countryId;
    };

    for (const PendingUpgrade& pu : m_pendingUpgrades) {
        if (pu.turnsRemaining > turns || !ownedByUs(pu.provinceId)) continue;
        if (pu.type != "industry") continue;      // forts and ports earn nothing
        // processUpgrades sets income = level * 2, so the gain is the
        // difference from whatever the province earns today. Read from the same
        // place rather than assumed, or the two drift the first time the
        // formula changes.
        auto it = m_provinceIndustry.find(pu.provinceId);
        const float now = (it != m_provinceIndustry.end()) ? it->second.income : 0.0f;
        const int   lvl = (it != m_provinceIndustry.end()) ? it->second.level : 0;
        cs.gross += std::max(0.0f, (float)pu.targetLevel * 2.0f - now);
        cs.industryLevels += std::max(0, pu.targetLevel - lvl);
    }

    // A hull under construction is free until it floats and then costs its
    // berth every turn for ever. This is the half of a ship's price that
    // nothing used to look at -- see the note in execEconomy's ship case.
    for (const PendingShipBuild& sb : m_pendingShipBuilds) {
        if (sb.turnsRemaining > turns || !ownedByUs(sb.provinceId)) continue;
        cs.navyExpenses += (sb.type == "carrier") ? 25.0f : 10.0f;
    }

    // The factories' own running cost grows with how many levels are held, so
    // a projection that added the income and not the upkeep would make every
    // build look better than it is. Recomputed from the projected totals
    // through the same function the live snapshot uses.
    const float upkeepNow = cs.industryUpkeep;
    cs.industryUpkeep = industryUpkeep(cs.industryLevels, cs.gross,
                                       getTotalEffect("industryUpkeepPct", countryId));
    cs.expenses += (cs.industryUpkeep - upkeepNow);

    cs.expenses += (cs.navyExpenses - navyNow);
    cs.total = cs.gross + cs.resource + cs.pop;
    cs.net   = cs.total - cs.expenses;
    return cs;
}

CountryIncomeSnapshot Game::computeCountryIncome(int countryId) const {
    auto cacheIt = m_countryIncomeCache.find(countryId);
    if (cacheIt != m_countryIncomeCache.end()) return cacheIt->second;
    if (countryId == m_lastIncomeCountryId) return m_cachedIncome;
    CountryIncomeSnapshot cs;
    const auto& allProvs = m_provinces.getAllProvinces();
    for (int pid : provincesOf(countryId)) {
        auto pIt = allProvs.find(pid);
        if (pIt == allProvs.end() || pIt->second.countryId != countryId) continue;
        auto ind = m_provinceIndustry.find(pid);
        if (ind != m_provinceIndustry.end()) {
            cs.gross += ind->second.income;
            cs.industryLevels += ind->second.level;
            cs.resource += provinceResourceIncome(pid);
            cs.pop += ind->second.popIncome;
        }
    }
    for (auto& [pid, units] : m_provinceArmies) {
        for (auto& u : units) {
            if (u.countryId != countryId) continue;
            cs.armyExpenses += (u.count / 10000.0f) * 0.01f;
        }
    }
    for (auto& ship : m_ships) {
        if (ship.countryId != countryId) continue;
        if (ship.type == "carrier") cs.navyExpenses += 25;
        else if (ship.type == "destroyer") cs.navyExpenses += 10;
        cs.navyExpenses += (ship.crew / 10000.0f) * 0.2f;
    }
    auto it = m_countryActivePolicyIndices.find(countryId);
    if (it != m_countryActivePolicyIndices.end()) {
        for (int apIdx : it->second) {
            if (apIdx >= (int)m_activePolicies.size()) continue;
            auto& ap = m_activePolicies[apIdx];
            if (ap.countryId != countryId) continue;
            if (ap.turnsRemaining < 0) continue;
            const Policy* p = nullptr;
            for (const auto& policy : m_allPolicies) {
                if (policy.id == ap.policyId) { p = &policy; break; }
            }
            if (p) cs.policyCosts += p->costPerTurn;
        }
    }
    std::unordered_set<std::string> processedMinorities;
    for (int pid : provincesOf(countryId)) {
        auto pIt = allProvs.find(pid);
        if (pIt == allProvs.end() || pIt->second.countryId != countryId) continue;
        auto mit = m_provinceMinorities.find(pid);
        if (mit == m_provinceMinorities.end()) continue;
        for (auto& mg : mit->second) {
            if (processedMinorities.count(mg.name)) continue;
            processedMinorities.insert(mg.name);
            for (size_t ci = 0; ci < m_ethnicPolicyCategories.size(); ci++) {
                const int oi = ethnicPolicyOption(countryId, mg.name, ci);
                if (oi >= 0 && oi < (int)m_ethnicPolicyCategories[ci].options.size())
                    cs.minorityCosts += m_ethnicPolicyCategories[ci].options[oi].costPerTurn;
            }
        }
    }
    cs.total = cs.gross + cs.resource + cs.pop;
    // Factories cost money to run, and more of them cost disproportionately
    // more. See industryUpkeep() for why this is a running cost rather than a
    // higher price.
    cs.industryUpkeep = industryUpkeep(cs.industryLevels, cs.gross,
                                       getTotalEffect("industryUpkeepPct", countryId));
    float baseExpenses = cs.armyExpenses + cs.navyExpenses + cs.policyCosts +
                         cs.minorityCosts + cs.industryUpkeep;
    float affordable = std::max(0.0f, cs.total - baseExpenses);
    // Allocations are per-country: only the player pays the research slider
    // Each country pays for ITS OWN allocations: the player's come from the
    // UI sliders, AI countries' from the per-country maps the AI sets — they
    // used to be billed the player's sliders as pure money sinks.
    float rAlloc = m_researchAllocation;
    float pAlloc = m_pacificationAllocation;
    if (countryId != m_playerCountryId) {
        auto raIt = m_countryResearchAllocation.find(countryId);
        rAlloc = (raIt != m_countryResearchAllocation.end()) ? raIt->second : 0.0f;
        auto pacIt = m_countryPacification.find(countryId);
        pAlloc = (pacIt != m_countryPacification.end()) ? pacIt->second : 0.0f;
    }
    cs.researchCost = cs.total * rAlloc;
    cs.pacificationCost = cs.total * pAlloc;
    float totalAlloc = cs.researchCost + cs.pacificationCost;
    if (totalAlloc > affordable && totalAlloc > 0) {
        float scale = affordable / totalAlloc;
        cs.researchCost *= scale;
        cs.pacificationCost *= scale;
    }
    cs.expenses = baseExpenses + cs.researchCost + cs.pacificationCost;
    cs.net = cs.total - cs.expenses;
    m_lastIncomeCountryId = countryId;
    m_cachedIncome = cs;
    return cs;
}

void Game::refreshIncomeCache() {
    m_countryIncomeCache.clear();
    struct IncomeAccum { float gross = 0, res = 0, pop = 0; int levels = 0; };
    std::unordered_map<int, IncomeAccum> incAcc;
    for (auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId <= 0) continue;
        auto ind = m_provinceIndustry.find(pid);
        if (ind == m_provinceIndustry.end()) continue;
        auto& a = incAcc[p.countryId];
        a.gross += ind->second.income;
        a.levels += ind->second.level;
        a.res += provinceResourceIncome(pid);
        a.pop += ind->second.popIncome;
    }
    // Upkeep in one pass each, not one pass per country. Both of these used to
    // sit inside the per-country loop below, so every army stack and every ship
    // on the map was visited once for every country in the game.
    std::unordered_map<int, float> armyUpkeep, navyUpkeep;
    for (auto& [pid, units] : m_provinceArmies)
        for (auto& u : units)
            if (u.countryId > 0) armyUpkeep[u.countryId] += (u.count / 10000.0f) * 0.01f;
    for (auto& ship : m_ships) {
        if (ship.countryId <= 0) continue;
        float& n = navyUpkeep[ship.countryId];
        if (ship.type == "carrier") n += 25;
        else if (ship.type == "destroyer") n += 10;
        n += (ship.crew / 10000.0f) * 0.2f;
    }

    const auto& allProvs = m_provinces.getAllProvinces();
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID) continue;
        auto& a = incAcc[cid];
        CountryIncomeSnapshot cs;
        cs.gross = a.gross;
        cs.resource = a.res;
        cs.pop = a.pop;
        cs.total = a.gross + a.res + a.pop;
        cs.industryLevels = a.levels;
        cs.industryUpkeep = industryUpkeep(a.levels, a.gross,
                                           getTotalEffect("industryUpkeepPct", cid));
        { auto it = armyUpkeep.find(cid); if (it != armyUpkeep.end()) cs.armyExpenses = it->second; }
        { auto it = navyUpkeep.find(cid); if (it != navyUpkeep.end()) cs.navyExpenses = it->second; }
        auto pIt = m_countryActivePolicyIndices.find(cid);
        if (pIt != m_countryActivePolicyIndices.end()) {
            for (int apIdx : pIt->second) {
                if (apIdx >= (int)m_activePolicies.size()) continue;
                auto& ap = m_activePolicies[apIdx];
                if (ap.countryId != cid || ap.turnsRemaining < 0) continue;
                for (const auto& policy : m_allPolicies)
                    if (policy.id == ap.policyId) { cs.policyCosts += policy.costPerTurn; break; }
            }
        }
        std::unordered_set<std::string> pm;
        for (int pid : provincesOf(cid)) {
            auto pIt = allProvs.find(pid);
            if (pIt == allProvs.end() || pIt->second.countryId != cid) continue;
            auto mit = m_provinceMinorities.find(pid);
            if (mit == m_provinceMinorities.end()) continue;
            for (auto& mg : mit->second) {
                if (pm.count(mg.name)) continue;
                pm.insert(mg.name);
                for (size_t ci = 0; ci < m_ethnicPolicyCategories.size(); ci++) {
                    const int oi = ethnicPolicyOption(cid, mg.name, ci);
                    if (oi >= 0 && oi < (int)m_ethnicPolicyCategories[ci].options.size())
                        cs.minorityCosts += m_ethnicPolicyCategories[ci].options[oi].costPerTurn;
                }
            }
        }
        // industryUpkeep included HERE TOO. This is the bulk path every AI
        // country's income comes from and the one above is the player's; a cost
        // added to one and not the other is the two of them playing different
        // games, which is the mistake the header of BuildCosts.h exists to
        // record.
        float baseExpenses = cs.armyExpenses + cs.navyExpenses + cs.policyCosts +
                             cs.minorityCosts + cs.industryUpkeep;
        float affordable = std::max(0.0f, cs.total - baseExpenses);
        float rAlloc = m_researchAllocation;
        float pAlloc = m_pacificationAllocation;
        if (cid != m_playerCountryId) {
            auto raIt = m_countryResearchAllocation.find(cid);
            rAlloc = (raIt != m_countryResearchAllocation.end()) ? raIt->second : 0.0f;
            auto pacIt = m_countryPacification.find(cid);
            pAlloc = (pacIt != m_countryPacification.end()) ? pacIt->second : 0.0f;
        }
        cs.researchCost = cs.total * rAlloc;
        cs.pacificationCost = cs.total * pAlloc;
        float totalAlloc = cs.researchCost + cs.pacificationCost;
        if (totalAlloc > affordable && totalAlloc > 0) {
            float scale = affordable / totalAlloc;
            cs.researchCost *= scale;
            cs.pacificationCost *= scale;
        }
        cs.expenses = baseExpenses + cs.researchCost + cs.pacificationCost;
        cs.net = cs.total - cs.expenses;
        m_countryIncomeCache[cid] = cs;
    }
}

void Game::recordIncomeSnapshot() {
    refreshIncomeCache();
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID) continue;
        auto cs = m_countryIncomeCache[cid];
        m_incomeHistory[cid].push_back(cs);
        while (m_incomeHistory[cid].size() > 12)
            m_incomeHistory[cid].erase(m_incomeHistory[cid].begin());
        if (m_countryBalances.find(cid) == m_countryBalances.end())
            m_countryBalances[cid] = 0;
        m_countryBalances[cid] += cs.net;
        // Treasury is NOT credited here. processEconomy() already applied this
        // turn's net income (Game_TurnLogic.cpp), so doing it again made every
        // country earn double the income actually shown in the UI — which also
        // drove treasuries into the range where float precision breaks down.
        // This function only records history/balances.
    }
}

void Game::updateEconomy() {
    Vector2 mouse = getMouse();
    if (IsKeyPressed(KEY_ESCAPE)) {
        m_inEconomy = false;
        m_activeSidebarTab = 0;
        m_economyTab = 0;
        m_economyShowWorst = false;
        m_economyScroll = 0;
        m_economyExpScroll = 0;
        m_economyGrossScroll = 0;
        return;
    }
    int centerX = m_screenW / 2;
    int tabY = 100;
    int tabSpacing = 200;

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        Rectangle closeBtn = {(float)(m_screenW - 44), 8, 36, 36};
        if (CheckCollisionPointRec(mouse, closeBtn)) {
            m_inEconomy = false;
            m_activeSidebarTab = 0;
            m_economyTab = 0;
            m_economyShowWorst = false;
            m_economyScroll = 0;
            m_economyExpScroll = 0;
            m_economyGrossScroll = 0;
            printf("[DIAG] Economy X button clicked\n");
            return;
        }
        const char* tabs[] = {"Global Economy", "Local Economy"};
        int visibleTabs = 2;
        int tabStartX = centerX - (visibleTabs * tabSpacing) / 2 + tabSpacing / 2;
        for (int t = 0; t < 2; ++t) {
            int tx = tabStartX + t * tabSpacing;
            int tw = MeasureText(tabs[t], 24);
            Rectangle tr = {(float)(tx - tw/2 - 10), (float)(tabY - 5), (float)(tw + 20), 34};
            if (CheckCollisionPointRec(mouse, tr)) {
                m_economyTab = t;
                m_economyScroll = 0;
                m_economyExpScroll = 0;
                m_economyGrossScroll = 0;
                return;
            }
        }
        if (m_economyTab == 0) {
            const char* toggleLabel = m_economyShowWorst ? "Show Top 10" : "Show Bottom 10";
            int tw = MeasureText(toggleLabel, 16);
            int startY = tabY + 70;
            Rectangle toggleBtn = {(float)(centerX - tw/2 - 10), (float)(startY - 30), (float)(tw + 20), 26};
            if (CheckCollisionPointRec(mouse, toggleBtn)) {
                m_economyShowWorst = !m_economyShowWorst;
            }
        }
    }
    int wheel = GetMouseWheelMove();
    if (wheel != 0 && m_economyTab == 0) {
        int scrollSpeed = 20;
        int marginX = 16;
        int colGap = 4;
        int availW = m_screenW - marginX * 2;
        int colW = (availW - colGap * 2) / 3;
        if (colW < 140) { colW = 140; availW = colW * 3 + colGap * 2; }
        int col0 = (m_screenW - availW) / 2;

        int listTop = tabY + 70 + 4 + std::min(140, (m_screenH - (tabY + 70) - 60) / 3) + 4;

        if (mouse.y >= listTop) {
            int colIdx = (int)((mouse.x - col0) / (colW + colGap));
            if (colIdx < 0) colIdx = 0;
            if (colIdx > 2) colIdx = 2;
            int* scrollVar = (colIdx == 0) ? &m_economyGrossScroll :
                             (colIdx == 1) ? &m_economyScroll :
                                             &m_economyExpScroll;
            *scrollVar -= wheel * scrollSpeed;
            if (*scrollVar < 0) *scrollVar = 0;
        }
    }
}

void Game::drawEconomy() {
    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 180});
    int centerX = m_screenW / 2;
    int tabY = 100;
    int tabSpacing = 200;

    const char* tabs[] = {"Global Economy", "Local Economy"};
    int visibleTabs = 2;
    int tabStartX = centerX - (visibleTabs * tabSpacing) / 2 + tabSpacing / 2;
    for (int t = 0; t < 2; ++t) {
        int tx = tabStartX + t * tabSpacing;
        bool active = (t == m_economyTab);
        Color tc = active ? hexToColor(m_config.accent()) : LIGHTGRAY;
        DrawText(tabs[t], tx - MeasureText(tabs[t], 24) / 2, tabY, 24, tc);
        if (active) {
            int tw = MeasureText(tabs[t], 24);
            DrawRectangle(tx - tw / 2, tabY + 28, tw, 3, hexToColor(m_config.accent()));
        }
    }

    Rectangle closeBtn = {(float)(m_screenW - 44), 8, 36, 36};
    Vector2 mouse = getMouse();
    bool closeHover = CheckCollisionPointRec(mouse, closeBtn);
    Color closeCol = closeHover ? RED : Color{180, 180, 180, 200};
    DrawRectangleRounded(closeBtn, 0.2f, 6, {60, 60, 70, 180});
    DrawRectangleRoundedLines(closeBtn, 0.2f, 6, closeCol);
    int xw = MeasureText("X", 20);
    DrawText("X", (int)(closeBtn.x + closeBtn.width/2 - xw/2), 12, 20, closeCol);

    DrawText(T("ESC to close"), m_screenW - 140, 55, 14, Color{120, 120, 140, 150});

    int startY = tabY + 70;
    if (m_economyTab == 0) {
        drawEconomyGlobal(centerX, startY);
    } else {
        drawEconomyLocal(centerX, startY);
    }
}

void Game::drawEconomyGlobal(int centerX, int startY) {
    struct CountryEcon {
        int cid;
        float gross, resource, pop, expenses, net, total;
        Color color;
    };
    std::vector<CountryEcon> list;
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID) continue;
        auto cs = computeCountryIncome(cid);
        list.push_back({cid, cs.gross, cs.resource, cs.pop, cs.expenses, cs.net, cs.total, c.color});
    }

    auto getName = [&](int cid) -> std::string {
        const Country* c = m_countries.getCountry(cid);
        return c ? od::i18n::properName(c->name) : "CID" + std::to_string(cid);
    };

    auto buildData = [&](const std::vector<CountryEcon>& src, std::function<float(const CountryEcon&)> extract) {
        std::vector<CountryEcon> sorted = src;
        std::sort(sorted.begin(), sorted.end(), [&](auto& a, auto& b) { return extract(a) > extract(b); });
        std::vector<CountryEcon> d;
        int n = std::min(10, (int)sorted.size());
        if (m_economyShowWorst) {
            std::sort(sorted.begin(), sorted.end(), [&](auto& a, auto& b) { return extract(a) < extract(b); });
            for (int i = 0; i < n; ++i) d.push_back(sorted[i]);
        } else {
            for (int i = 0; i < n; ++i) d.push_back(sorted[i]);
        }
        return d;
    };

    auto extractGross     = [](const CountryEcon& e) { return e.gross; };
    auto extractNet       = [](const CountryEcon& e) { return e.net; };
    auto extractExpenses  = [](const CountryEcon& e) { return e.expenses; };

    auto dGross  = buildData(list, extractGross);
    auto dNet    = buildData(list, extractNet);
    auto dExp    = buildData(list, extractExpenses);

    const char* toggleLabel = m_economyShowWorst ? "Show Top 10" : "Show Bottom 10";
    int tw = MeasureText(toggleLabel, 16);
    int toggleX = centerX;
    Rectangle toggleBtn = {(float)(toggleX - tw/2 - 10), (float)(startY - 30), (float)(tw + 20), 26};
    Vector2 mouse = getMouse();
    bool toggleHover = CheckCollisionPointRec(mouse, toggleBtn);
    Color toggleCol = toggleHover ? ColorAlpha(hexToColor(m_config.accent()), 200.0f/255.0f) : ColorAlpha(hexToColor(m_config.accent()), 150.0f/255.0f);
    DrawRectangleRounded(toggleBtn, 0.2f, 6, ColorAlpha(hexToColor(m_config.accent()), 40.0f/255.0f));
    DrawRectangleRoundedLines(toggleBtn, 0.2f, 6, toggleCol);
    DrawText(toggleLabel, (int)(toggleX - tw/2), startY - 26, 16, toggleCol);

    int marginX = 16;
    int colGap = 4;
    int availW = m_screenW - marginX * 2;
    int colW = (availW - colGap * 2) / 3;
    if (colW < 140) { colW = 140; availW = colW * 3 + colGap * 2; }
    int col0 = (m_screenW - availW) / 2;

    int graphH = std::clamp((m_screenH - startY - 30) * 35 / 100, 140, 240);
    int listTop = startY + 4 + graphH + 28;
    int listH = std::clamp(m_screenH - listTop - 50, 120, 400);

    auto drawGraph = [&](int x, int y, int w, int h, const std::vector<CountryEcon>& data,
                          const std::string& title, std::function<float(const CountryEcon&)> extract) {
        // Translated here rather than at the three call sites, the same way
        // drawButton does it: this is the one place the heading is drawn.
        DrawText(T(title), x, y, 16, WHITE);
        y += 22;
        if (data.empty()) { DrawText(T("No data"), x + 5, y + 5, 12, LIGHTGRAY); return; }
        int barArea = w - 14;
        int barGap = 3;
        int barCnt = (int)data.size();
        int barW = (barArea - barGap * (barCnt + 1)) / barCnt;
        if (barW < 6) barW = 6;
        if (barW > 26) barW = 26;

        float maxVal = 0;
        for (auto& e : data) { float v = extract(e); if (fabsf(v) > maxVal) maxVal = fabsf(v); }
        if (maxVal <= 0) maxVal = 1;
        float scale = (h - 44) / maxVal;

        BeginScissorMode(x, y, w, h);

        for (auto& e : data) {
            if (extract(e) < 0) {
                DrawLine(x + 6, y + h - 22, x + w - 6, y + h - 22, {80, 80, 100, 150});
                break;
            }
        }

        for (int i = 0; i < barCnt; ++i) {
            float v = extract(data[i]);
            int bx = x + 6 + barGap + i * (barW + barGap);
            int bh = (int)(fabsf(v) * scale);
            if (bh < 1 && v != 0) bh = 1;
            int by = (v >= 0) ? (y + h - 22 - bh) : (y + h - 22);
            Color barCol = (v >= 0) ? data[i].color : odPalette::of(odPalette::Role::Bad);
            DrawRectangle(bx, by, barW, bh, barCol);
        }

        bool showNames = barW >= 14;
        int labelCnt = showNames ? std::min(barCnt, 10) : std::min(barCnt, 5);
        for (int i = 0; i < labelCnt; ++i) {
            int bx = x + 6 + barGap + i * (barW + barGap);
            std::string name = getName(data[i].cid);
            // AS MANY CHARACTERS AS FIT THE BAR, not a fixed five.
            //
            // Five was five bytes, which was five Latin letters and one and
            // two thirds of a Japanese one -- so the label was cut mid-
            // character and drew as "南?". Counting CHARACTERS fixes the
            // mojibake and leaves the other half of the problem: five kanji
            // are about three times as wide as five letters at this size, and
            // the labels ran into each other. Measuring is the only rule that
            // holds for twenty languages at once.
            const int slot = barW + barGap;
            if (odText::measureText(name.c_str(), 8) > slot) {
                int n = odText::charCount(name);
                while (n > 1) {
                    const std::string cut = odText::firstChars(name, --n) + ".";
                    if (odText::measureText(cut.c_str(), 8) <= slot) { name = cut; break; }
                    if (n == 1) name = cut;
                }
            }
            DrawText(name.c_str(), bx, y + h - 20, 8, LIGHTGRAY);
            float v = extract(data[i]);
            if (v != 0) {
                int bh = (int)(fabsf(v) * scale);
                if (bh < 1) bh = 1;
                int vy = (v >= 0) ? (y + h - 22 - bh - 12) : (y + h - 22 + bh + 2);
                float dv = (fabsf(v) < 0.5f) ? 0 : v;
                DrawText(TextFormat("%.0f", dv), bx, vy, 8, WHITE);
            }
        }

        EndScissorMode();
    };

    drawGraph(col0, startY + 4, colW, graphH, dGross, "Gross Income", extractGross);
    drawGraph(col0 + colW + colGap, startY + 4, colW, graphH, dNet, "Net Income", extractNet);
    drawGraph(col0 + (colW + colGap) * 2, startY + 4, colW, graphH, dExp, "Expenses", extractExpenses);

    auto drawScrollableList = [&](int x, int y, int w, int h, const char* title,
                                   const std::vector<CountryEcon>& sortedList,
                                   std::function<const char*(const CountryEcon&)> formatLine,
                                   int& scrollVar) {
        int headerH = 22;
        int entryH = 18;
        int contentH = (int)sortedList.size() * entryH;
        int scrollH = h - headerH - 4;
        int maxScroll = std::max(0, contentH - scrollH);
        if (scrollVar > maxScroll) scrollVar = maxScroll;
        if (scrollVar < 0) scrollVar = 0;

        DrawRectangle(x, y, w, h, {20, 20, 30, 180});
        DrawText(T(title), x + 6, y + 3, 14, WHITE);
        DrawLine(x, y + headerH, x + w, y + headerH, {60, 60, 80, 150});

        int clipY = y + headerH + 2;
        int clipH = h - headerH - 2;
        BeginScissorMode(x, clipY, w, clipH);

        int yOff = clipY - scrollVar;
        for (size_t i = 0; i < sortedList.size(); ++i) {
            auto& e = sortedList[i];
            Color textCol = (e.cid == m_playerCountryId) ? hexToColor(m_config.accent()) : LIGHTGRAY;
            DrawText(formatLine(e), x + 6, yOff + i * entryH, 12, textCol);
        }

        EndScissorMode();

        if (maxScroll > 0) {
            int barX = x + w - 10;
            int barW_sb = 6;
            int barH_sb = std::max(20, (int)(scrollH * scrollH / (float)contentH));
            int barY_sb = clipY + (int)(scrollVar / (float)maxScroll * (scrollH - barH_sb));
            DrawRectangle(barX, clipY, barW_sb, scrollH, {40, 40, 50, 150});
            DrawRectangle(barX, barY_sb, barW_sb, barH_sb, {120, 120, 140, 200});
        }
    };

    auto formatGrossLine = [&](const CountryEcon& e) -> const char* {
        const Country* c = m_countries.getCountry(e.cid);
        std::string name = c ? od::i18n::properName(c->name)
                             : "CID" + std::to_string(e.cid);
        static char buf[128];
        float v = e.gross;
        if (fabsf(v) < 0.5f) v = 0;
        snprintf(buf, sizeof(buf), "%-8s %.0f", name.c_str(), v);
        return buf;
    };

    auto formatNetLine = [&](const CountryEcon& e) -> const char* {
        const Country* c = m_countries.getCountry(e.cid);
        std::string name = c ? od::i18n::properName(c->name)
                             : "CID" + std::to_string(e.cid);
        static char buf[128];
        float v = e.net;
        if (fabsf(v) < 0.5f) v = 0;
        snprintf(buf, sizeof(buf), "%-8s %.0f", name.c_str(), v);
        return buf;
    };

    auto formatExpLine = [&](const CountryEcon& e) -> const char* {
        const Country* c = m_countries.getCountry(e.cid);
        std::string name = c ? od::i18n::properName(c->name)
                             : "CID" + std::to_string(e.cid);
        static char buf[128];
        float v = e.expenses;
        if (fabsf(v) < 0.5f) v = 0;
        snprintf(buf, sizeof(buf), "%-8s %.0f", name.c_str(), v);
        return buf;
    };

    std::sort(list.begin(), list.end(),
              [](auto& a, auto& b) { return a.gross > b.gross; });

    std::vector<CountryEcon> listSortedByNet = list;
    std::sort(listSortedByNet.begin(), listSortedByNet.end(),
              [](auto& a, auto& b) { return a.net > b.net; });

    std::vector<CountryEcon> listSortedByExp = list;
    std::sort(listSortedByExp.begin(), listSortedByExp.end(),
              [](auto& a, auto& b) { return a.expenses > b.expenses; });

    drawScrollableList(col0, listTop, colW, listH, "Top Gross Income",
                       list, formatGrossLine, m_economyGrossScroll);

    drawScrollableList(col0 + colW + colGap, listTop, colW, listH, "Top Net Income",
                       listSortedByNet, formatNetLine, m_economyScroll);

    drawScrollableList(col0 + (colW + colGap) * 2, listTop, colW, listH, "Top Expenses",
                       listSortedByExp, formatExpLine, m_economyExpScroll);
}

void Game::drawEconomyLocal(int centerX, int startY) {
    int cid = m_playerCountryId;
    if (cid <= 0 || cid == SPC_CID) {
        DrawText(T("Select a country to view local economy"), centerX - 200, startY + 40, 18, LIGHTGRAY);
        return;
    }
    const Country* c = m_countries.getCountry(cid);
    if (!c) return;

    auto cs = computeCountryIncome(cid);
    int pieStartY = startY + 30;

    int lx = std::max(20, centerX - 340);
    DrawText(TextFormat(T("%s - Economic Breakdown"),
                        od::i18n::properName(c->name).c_str()), lx, startY, 20, WHITE);
    startY += 30;

    int valX = lx + 260;

    startY = drawBreakdownRow(lx, startY, valX, "Component", "Amount", LIGHTGRAY, false);
    startY = drawBreakdownRow(lx, startY, valX, "Industry Income (base)", TextFormat("%.1f", cs.gross), WHITE, false);
    startY = drawBreakdownRow(lx, startY, valX, "Resource Bonus", TextFormat("%.1f", cs.resource), WHITE, false);
    startY = drawBreakdownRow(lx, startY, valX, "Population Bonus", TextFormat("%.1f", cs.pop), WHITE, false);
    DrawRectangle(lx, startY, 320, 1, {100, 100, 120, 150});
    startY += 6;
    startY = drawBreakdownRow(lx, startY, valX, "Gross Income", TextFormat("%.1f", cs.total), WHITE, false);
    startY += 4;
    // Shown with the percentage, and shown even at zero, because an upkeep the
    // player cannot see is a tax they conclude is a bug. The number they need
    // in order to decide whether the next factory is worth building is the RATE
    // it is charged at, not the total.
    if (cs.industryLevels > 0) {
        const float pct = cs.gross > 0.0f ? (cs.industryUpkeep / cs.gross) * 100.0f : 0.0f;
        startY = drawBreakdownRow(lx, startY, valX,
                                  TextFormat(T("Industry Upkeep (%.0f%% of %d lvl)"), pct, cs.industryLevels),
                                  TextFormat("-%.1f", cs.industryUpkeep),
                                  Color{255, 210, 160, 255}, false);
    }
    startY = drawBreakdownRow(lx, startY, valX, "Army Cost", TextFormat("-%.1f", cs.armyExpenses), Color{255, 180, 180, 255}, false);
    if (cs.navyExpenses > 0) {
        startY = drawBreakdownRow(lx, startY, valX, "Navy Cost", TextFormat("-%.1f", cs.navyExpenses), Color{255, 180, 255, 255}, false);
    }
    if (cs.policyCosts > 0) {
        startY = drawBreakdownRow(lx, startY, valX, "Doctrine Costs", TextFormat("-%.1f", cs.policyCosts), Color{180, 200, 255, 255}, false);
    }
    if (cs.minorityCosts > 0) {
        startY = drawBreakdownRow(lx, startY, valX, "Minority Programmes", TextFormat("-%.1f", cs.minorityCosts), Color{255, 200, 180, 255}, false);
    }
    if (cs.researchCost > 0) {
        startY = drawBreakdownRow(lx, startY, valX, "Research Allocation", TextFormat("-%.1f", cs.researchCost), Color{180, 255, 180, 255}, false);
    }
    if (cs.pacificationCost > 0) {
        startY = drawBreakdownRow(lx, startY, valX, "Pacification Budget", TextFormat("-%.1f", cs.pacificationCost), Color{180, 180, 255, 255}, false);
    }
    DrawRectangle(lx, startY, 320, 1, {100, 100, 120, 150});
    startY += 6;
    { float netv = (fabsf(cs.net) < 0.05f) ? 0 : cs.net;
    startY = drawBreakdownRow(lx, startY, valX, "Net Income", TextFormat("%.1f", netv), netv >= 0 ? hexToColor(m_config.accent()) : odPalette::of(odPalette::Role::Bad), false); }
    startY += 10;

    auto histIt = m_incomeHistory.find(cid);
    if (histIt != m_incomeHistory.end() && histIt->second.size() >= 2) {
        auto& hist = histIt->second;
        int graphW = 350;
        int graphH = 140;
        int gx = lx;
        int gy = startY;

        DrawText(T("Income Over Recent Turns"), gx, gy, 16, WHITE);
        gy += 20;

        float maxV = 0;
        for (auto& s : hist) maxV = std::max(maxV, s.total);
        if (maxV <= 0) maxV = 1;

        DrawRectangleLines(gx, gy, graphW, graphH, {80, 80, 100, 150});

        auto drawLine = [&](const std::vector<float>& vals, Color col) {
            if (vals.size() < 2) return;
            for (size_t i = 1; i < vals.size(); ++i) {
                int x1 = gx + (int)((i - 1) * graphW / (float)(hist.size() - 1));
                int y1 = gy + graphH - (int)(vals[i - 1] / maxV * (graphH - 4)) - 2;
                int x2 = gx + (int)(i * graphW / (float)(hist.size() - 1));
                int y2 = gy + graphH - (int)(vals[i] / maxV * (graphH - 4)) - 2;
                DrawLine(x1, y1, x2, y2, col);
            }
        };

        std::vector<float> grossV, resV, popV, netV, totalV;
        for (auto& s : hist) {
            grossV.push_back(s.gross);
            resV.push_back(s.resource);
            popV.push_back(s.pop);
            netV.push_back(s.net);
            totalV.push_back(s.total);
        }
        drawLine(totalV, hexToColor(m_config.accent()));
        drawLine(grossV, SKYBLUE);
        drawLine(resV, odPalette::of(odPalette::Role::Good));
        drawLine(popV, ORANGE);
        drawLine(netV, odPalette::of(odPalette::Role::Bad));

        int legX = gx + graphW + 20;
        int legY = gy;
        auto drawLegend = [&](const char* label, Color col) {
            DrawRectangle(legX, legY, 10, 10, col);
            DrawText(T(label), legX + 14, legY, 12, LIGHTGRAY);
            legY += 16;
        };
        drawLegend(T("Total"), hexToColor(m_config.accent()));
        drawLegend(T("Industry"), SKYBLUE);
        drawLegend(T("Resource"), odPalette::of(odPalette::Role::Good));
        drawLegend(T("Population"), ORANGE);
        drawLegend(T("Net"), odPalette::of(odPalette::Role::Bad));

        startY = gy + graphH + 10;
    } else {
        DrawText(T("(No historical data yet — play more turns)"), lx, startY, 12, Color{120, 120, 140, 200});
        startY += 22;
    }

    int pieX = centerX + 80;
    int pieY = pieStartY;
    float expTotal = cs.expenses;
    if (expTotal > 0) {
        DrawText(T("Expense Composition"), pieX - 50, pieY, 16, WHITE);
        pieY += 24;

        struct Slice { float val; Color col; const char* label; };
        Slice slices[] = {
            // INDUSTRY UPKEEP BELONGS HERE, and its absence was not a missing
            // label -- it was a missing wedge. `expTotal` is cs.expenses, which
            // INCLUDES the upkeep, so every other slice was divided by a
            // denominator carrying a cost that got no arc: the percentages did
            // not sum to 100 and the pie had a hole exactly the size of the
            // upkeep. Reported from a 67-level France, where the four listed
            // slices came to 79.2% and the 20.8% missing was the -69.7 sitting
            // in the table directly beside the chart. It is also, for most
            // industrial countries, the second largest thing they pay.
            {cs.industryUpkeep, Color{230, 170, 110, 255}, T("Industry upkeep")},
            {cs.armyExpenses, Color{200, 80, 80, 255}, T("Army")},
            {cs.navyExpenses, Color{120, 80, 180, 255}, T("Navy")},
            {cs.policyCosts, Color{80, 120, 200, 255}, T("Doctrines")},
            {cs.minorityCosts, Color{200, 140, 80, 255}, T("Minority")},
            {cs.researchCost, Color{80, 200, 80, 255}, T("Research")},
            {cs.pacificationCost, Color{80, 180, 220, 255}, T("Pacification")},
        };

        int radius = 55;
        int cx = pieX;
        int cy = pieY + radius;
        float startAngle = 0;
        for (auto& sl : slices) {
            if (sl.val <= 0) continue;
            float sweep = (sl.val / expTotal) * 360.0f;
            DrawCircleSector({(float)cx, (float)cy}, (float)radius, startAngle, startAngle + sweep, 40, sl.col);
            startAngle += sweep;
        }
        DrawCircleLines(cx, cy, radius, {100, 100, 120, 150});

        int legX = pieX + radius + 20;
        int legY = pieY + radius - 40;
        for (auto& sl : slices) {
            if (sl.val <= 0) continue;
            DrawRectangle(legX, legY, 10, 10, sl.col);
            float pct = (sl.val / expTotal) * 100.0f;
            DrawText(TextFormat("%s: %.1f%%", sl.label, pct), legX + 14, legY, 12, LIGHTGRAY);
            legY += 16;
        }
    }

    pieY = pieY + (expTotal > 0 ? 100 : 40);
    float incTotal = cs.total;
    if (incTotal > 0) {
        DrawText(T("Income Composition"), pieX - 50, pieY, 16, WHITE);
        pieY += 24;

        struct Slice { float val; Color col; const char* label; };
        Slice slices[] = {
            {cs.gross, SKYBLUE, T("Industry")},
            {cs.resource, odPalette::of(odPalette::Role::Good), T("Resource")},
            {cs.pop, ORANGE, T("Population")},
        };

        int radius = 55;
        int cx = pieX;
        int cy = pieY + radius;
        float startAngle = 0;
        for (auto& sl : slices) {
            if (sl.val <= 0) continue;
            float sweep = (sl.val / incTotal) * 360.0f;
            DrawCircleSector({(float)cx, (float)cy}, (float)radius, startAngle, startAngle + sweep, 40, sl.col);
            startAngle += sweep;
        }
        DrawCircleLines(cx, cy, radius, {100, 100, 120, 150});

        int legX = pieX + radius + 20;
        int legY = pieY + radius - 40;
        for (auto& sl : slices) {
            if (sl.val <= 0) continue;
            DrawRectangle(legX, legY, 10, 10, sl.col);
            float pct = (sl.val / incTotal) * 100.0f;
            DrawText(TextFormat("%s: %.1f%%", sl.label, pct), legX + 14, legY, 12, LIGHTGRAY);
            legY += 16;
        }
    }
}

int Game::drawBreakdownRow(int x, int y, int valX, const char* label, const char* value, Color col, bool highlight) {
    if (highlight) DrawRectangle(x - 4, y, 400, 22, {255, 255, 255, 12});
    // The label is a heading ("Gross Income"); the value is a number the
    // caller has already formatted, so only the label is translated here.
    // Same one-place rule as the button helpers -- see Game_Multiplayer.cpp.
    DrawText(T(label), x, y, 16, col);
    DrawText(value, valX, y, 16, col);
    return y + 22;
}
