#include "Game.h"
#include "GameInternals.h"
#include "Keybinds.h"
#include "raymath.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdio>

CountryIncomeSnapshot Game::computeCountryIncome(int countryId) const {
    auto cacheIt = m_countryIncomeCache.find(countryId);
    if (cacheIt != m_countryIncomeCache.end()) return cacheIt->second;
    if (countryId == m_lastIncomeCountryId) return m_cachedIncome;
    CountryIncomeSnapshot cs;
    for (auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId == countryId) {
            auto ind = m_provinceIndustry.find(pid);
            if (ind != m_provinceIndustry.end()) {
                cs.gross += ind->second.income;
                cs.resource += ind->second.resourceIncome;
                cs.pop += ind->second.popIncome;
            }
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
    for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
        if (prov.countryId != countryId) continue;
        auto mit = m_provinceMinorities.find(pid);
        if (mit == m_provinceMinorities.end()) continue;
        for (auto& mg : mit->second) {
            if (processedMinorities.count(mg.name)) continue;
            processedMinorities.insert(mg.name);
            auto epIt = m_ethnicPolicies.find(mg.name);
            bool hasEntry = (epIt != m_ethnicPolicies.end());
            for (size_t ci = 0; ci < m_ethnicPolicyCategories.size(); ci++) {
                int oi = -1;
                if (hasEntry && ci < epIt->second.size()) {
                    oi = epIt->second[ci];
                } else {
                    for (size_t oi2 = 0; oi2 < m_ethnicPolicyCategories[ci].options.size(); oi2++) {
                        if (m_ethnicPolicyCategories[ci].options[oi2].isDefault) { oi = (int)oi2; break; }
                    }
                }
                if (oi >= 0 && oi < (int)m_ethnicPolicyCategories[ci].options.size())
                    cs.minorityCosts += m_ethnicPolicyCategories[ci].options[oi].costPerTurn;
            }
        }
    }
    cs.total = cs.gross + cs.resource + cs.pop;
    float baseExpenses = cs.armyExpenses + cs.navyExpenses + cs.policyCosts + cs.minorityCosts;
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
    struct IncomeAccum { float gross = 0, res = 0, pop = 0; };
    std::unordered_map<int, IncomeAccum> incAcc;
    for (auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId <= 0) continue;
        auto ind = m_provinceIndustry.find(pid);
        if (ind == m_provinceIndustry.end()) continue;
        auto& a = incAcc[p.countryId];
        a.gross += ind->second.income;
        a.res += ind->second.resourceIncome;
        a.pop += ind->second.popIncome;
    }
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID) continue;
        auto& a = incAcc[cid];
        CountryIncomeSnapshot cs;
        cs.gross = a.gross;
        cs.resource = a.res;
        cs.pop = a.pop;
        cs.total = a.gross + a.res + a.pop;
        for (auto& [pid, units] : m_provinceArmies)
            for (auto& u : units)
                if (u.countryId == cid)
                    cs.armyExpenses += (u.count / 10000.0f) * 0.01f;
        for (auto& ship : m_ships) {
            if (ship.countryId != cid) continue;
            if (ship.type == "carrier") cs.navyExpenses += 25;
            else if (ship.type == "destroyer") cs.navyExpenses += 10;
            cs.navyExpenses += (ship.crew / 10000.0f) * 0.2f;
        }
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
        for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
            if (prov.countryId != cid) continue;
            auto mit = m_provinceMinorities.find(pid);
            if (mit == m_provinceMinorities.end()) continue;
            for (auto& mg : mit->second) {
                if (pm.count(mg.name)) continue;
                pm.insert(mg.name);
                auto epIt = m_ethnicPolicies.find(mg.name);
                bool hasEntry = (epIt != m_ethnicPolicies.end());
                for (size_t ci = 0; ci < m_ethnicPolicyCategories.size(); ci++) {
                    int oi = -1;
                    if (hasEntry && ci < epIt->second.size()) oi = epIt->second[ci];
                    else {
                        for (size_t oi2 = 0; oi2 < m_ethnicPolicyCategories[ci].options.size(); oi2++)
                            if (m_ethnicPolicyCategories[ci].options[oi2].isDefault) { oi = (int)oi2; break; }
                    }
                    if (oi >= 0 && oi < (int)m_ethnicPolicyCategories[ci].options.size())
                        cs.minorityCosts += m_ethnicPolicyCategories[ci].options[oi].costPerTurn;
                }
            }
        }
        float baseExpenses = cs.armyExpenses + cs.navyExpenses + cs.policyCosts + cs.minorityCosts;
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
        Color tc = active ? hexToColor(m_config.accentColor) : LIGHTGRAY;
        DrawText(tabs[t], tx - MeasureText(tabs[t], 24) / 2, tabY, 24, tc);
        if (active) {
            int tw = MeasureText(tabs[t], 24);
            DrawRectangle(tx - tw / 2, tabY + 28, tw, 3, hexToColor(m_config.accentColor));
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

    DrawText("ESC to close", m_screenW - 140, 55, 14, Color{120, 120, 140, 150});

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
        return c ? c->name : "CID" + std::to_string(cid);
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
    Color toggleCol = toggleHover ? ColorAlpha(hexToColor(m_config.accentColor), 200.0f/255.0f) : ColorAlpha(hexToColor(m_config.accentColor), 150.0f/255.0f);
    DrawRectangleRounded(toggleBtn, 0.2f, 6, ColorAlpha(hexToColor(m_config.accentColor), 40.0f/255.0f));
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
        DrawText(title.c_str(), x, y, 16, WHITE);
        y += 22;
        if (data.empty()) { DrawText("No data", x + 5, y + 5, 12, LIGHTGRAY); return; }
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
            Color barCol = (v >= 0) ? data[i].color : RED;
            DrawRectangle(bx, by, barW, bh, barCol);
        }

        bool showNames = barW >= 14;
        int labelCnt = showNames ? std::min(barCnt, 10) : std::min(barCnt, 5);
        for (int i = 0; i < labelCnt; ++i) {
            int bx = x + 6 + barGap + i * (barW + barGap);
            std::string name = getName(data[i].cid);
            if (name.length() > 5) name = name.substr(0, 5) + ".";
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
        DrawText(title, x + 6, y + 3, 14, WHITE);
        DrawLine(x, y + headerH, x + w, y + headerH, {60, 60, 80, 150});

        int clipY = y + headerH + 2;
        int clipH = h - headerH - 2;
        BeginScissorMode(x, clipY, w, clipH);

        int yOff = clipY - scrollVar;
        for (size_t i = 0; i < sortedList.size(); ++i) {
            auto& e = sortedList[i];
            Color textCol = (e.cid == m_playerCountryId) ? hexToColor(m_config.accentColor) : LIGHTGRAY;
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
        std::string name = c ? c->name : "CID" + std::to_string(e.cid);
        static char buf[128];
        float v = e.gross;
        if (fabsf(v) < 0.5f) v = 0;
        snprintf(buf, sizeof(buf), "%-8s %.0f", name.c_str(), v);
        return buf;
    };

    auto formatNetLine = [&](const CountryEcon& e) -> const char* {
        const Country* c = m_countries.getCountry(e.cid);
        std::string name = c ? c->name : "CID" + std::to_string(e.cid);
        static char buf[128];
        float v = e.net;
        if (fabsf(v) < 0.5f) v = 0;
        snprintf(buf, sizeof(buf), "%-8s %.0f", name.c_str(), v);
        return buf;
    };

    auto formatExpLine = [&](const CountryEcon& e) -> const char* {
        const Country* c = m_countries.getCountry(e.cid);
        std::string name = c ? c->name : "CID" + std::to_string(e.cid);
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
        DrawText("Select a country to view local economy", centerX - 200, startY + 40, 18, LIGHTGRAY);
        return;
    }
    const Country* c = m_countries.getCountry(cid);
    if (!c) return;

    auto cs = computeCountryIncome(cid);
    int pieStartY = startY + 30;

    int lx = std::max(20, centerX - 340);
    DrawText(TextFormat("%s - Economic Breakdown", c->name.c_str()), lx, startY, 20, WHITE);
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
    startY = drawBreakdownRow(lx, startY, valX, "Net Income", TextFormat("%.1f", netv), netv >= 0 ? hexToColor(m_config.accentColor) : RED, false); }
    startY += 10;

    auto histIt = m_incomeHistory.find(cid);
    if (histIt != m_incomeHistory.end() && histIt->second.size() >= 2) {
        auto& hist = histIt->second;
        int graphW = 350;
        int graphH = 140;
        int gx = lx;
        int gy = startY;

        DrawText("Income Over Recent Turns", gx, gy, 16, WHITE);
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
        drawLine(totalV, hexToColor(m_config.accentColor));
        drawLine(grossV, SKYBLUE);
        drawLine(resV, GREEN);
        drawLine(popV, ORANGE);
        drawLine(netV, RED);

        int legX = gx + graphW + 20;
        int legY = gy;
        auto drawLegend = [&](const char* label, Color col) {
            DrawRectangle(legX, legY, 10, 10, col);
            DrawText(label, legX + 14, legY, 12, LIGHTGRAY);
            legY += 16;
        };
        drawLegend("Total", hexToColor(m_config.accentColor));
        drawLegend("Industry", SKYBLUE);
        drawLegend("Resource", GREEN);
        drawLegend("Population", ORANGE);
        drawLegend("Net", RED);

        startY = gy + graphH + 10;
    } else {
        DrawText("(No historical data yet — play more turns)", lx, startY, 12, Color{120, 120, 140, 200});
        startY += 22;
    }

    int pieX = centerX + 80;
    int pieY = pieStartY;
    float expTotal = cs.expenses;
    if (expTotal > 0) {
        DrawText("Expense Composition", pieX - 50, pieY, 16, WHITE);
        pieY += 24;

        struct Slice { float val; Color col; const char* label; };
        Slice slices[] = {
            {cs.armyExpenses, Color{200, 80, 80, 255}, "Army"},
            {cs.navyExpenses, Color{120, 80, 180, 255}, "Navy"},
            {cs.policyCosts, Color{80, 120, 200, 255}, "Doctrines"},
            {cs.minorityCosts, Color{200, 140, 80, 255}, "Minority"},
            {cs.researchCost, Color{80, 200, 80, 255}, "Research"},
            {cs.pacificationCost, Color{80, 180, 220, 255}, "Pacification"},
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
        DrawText("Income Composition", pieX - 50, pieY, 16, WHITE);
        pieY += 24;

        struct Slice { float val; Color col; const char* label; };
        Slice slices[] = {
            {cs.gross, SKYBLUE, "Industry"},
            {cs.resource, GREEN, "Resource"},
            {cs.pop, ORANGE, "Population"},
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
    DrawText(label, x, y, 16, col);
    DrawText(value, valX, y, 16, col);
    return y + 22;
}
