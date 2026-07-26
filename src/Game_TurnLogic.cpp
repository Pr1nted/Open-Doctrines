#include "Game.h"
#include "GameInternals.h"
#include "mods/ModManager.h"
#include "Keybinds.h"
#include "SaveManager.h"
#include "ai/AISystem.h"
#include "raymath.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>
#include <fstream>
#include <sstream>
#include <random>
#include <cstdio>
#include <deque>
#include <unordered_set>

// === isProvinceCoastal ===
bool Game::isProvinceCoastal(int pid) const {
    auto it = m_provincePixels.find(pid);
    if (it == m_provincePixels.end()) return false;
    int w = m_landSea.getWidth();
    int h = m_landSea.getHeight();
    // Find first adjacent water pixel
    int waterIdx = -1;
    for (int idx : it->second) {
        int px = idx % w;
        int py = idx / w;
        if (px > 0 && !m_landSea.isLand(px - 1, py)) { waterIdx = py * w + (px - 1); break; }
        if (px < w - 1 && !m_landSea.isLand(px + 1, py)) { waterIdx = py * w + (px + 1); break; }
        if (py > 0 && !m_landSea.isLand(px, py - 1)) { waterIdx = (py - 1) * w + px; break; }
        if (py < h - 1 && !m_landSea.isLand(px, py + 1)) { waterIdx = (py + 1) * w + px; break; }
    }
    if (waterIdx < 0) return false;
    // BFS to count water body size — stop early if it exceeds threshold
    static const int MIN_WATER_BODY = 75;
    int dx[4] = {1, -1, 0, 0};
    int dy[4] = {0, 0, 1, -1};
    std::vector<int> stack;
    std::unordered_set<int> visited;
    stack.push_back(waterIdx);
    visited.insert(waterIdx);
    int count = 0;
    while (!stack.empty() && count < MIN_WATER_BODY) {
        int idx = stack.back();
        stack.pop_back();
        count++;
        int cx = idx % w;
        int cy = idx / w;
        for (int d = 0; d < 4; ++d) {
            int nx = cx + dx[d];
            int ny = cy + dy[d];
            if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
                int nIdx = ny * w + nx;
                if (visited.find(nIdx) == visited.end() && !m_landSea.isLand(nx, ny)) {
                    visited.insert(nIdx);
                    stack.push_back(nIdx);
                }
            }
        }
    }
    return count >= MIN_WATER_BODY;
}

// === processArtilleryOrders ===
void Game::processArtilleryOrders(int countryId) {
    // Look up artillery effects from researched nodes
    struct ArtyEffect { float troopKillPct; float popKillPct; float fortDmg; int indDmg; float fortChance; };
    auto getEffect = [&](const std::string& type) -> ArtyEffect {
        for (auto& n : m_researchNodes) {
            if (n.artilleryType == type) {
                return {n.artilleryTroopKillPct, n.artilleryPopKillPct, n.artilleryFortDamage, n.artilleryIndustryDamage, n.artilleryFortDamageChance};
            }
        }
        return {0,0,0,0,0};
    };
    for (size_t i = 0; i < m_pendingArtilleryOrders.size(); ) {
        auto& ao = m_pendingArtilleryOrders[i];
        Province* srcP = m_provinces.getProvinceById(ao.fromProvince);
        if (!srcP || srcP->countryId != countryId) { ++i; continue; }
        Province* tgtP = m_provinces.getProvinceById(ao.targetProvince);
        if (!tgtP) { ++i; continue; }
        ArtyEffect eff = getEffect(ao.ammoType);

        // Kill troops in target province
        auto aIt = m_provinceArmies.find(ao.targetProvince);
        if (aIt != m_provinceArmies.end() && eff.troopKillPct > 0) {
            for (auto& u : aIt->second) {
                int killed = (int)(u.count * eff.troopKillPct / 100.0f);
                u.count = std::max(0, u.count - killed);
            }
        }

        // Kill population
        if (eff.popKillPct > 0) {
            auto pIt = m_provincePopulations.find(ao.targetProvince);
            if (pIt != m_provincePopulations.end()) {
                long long killed = (long long)(pIt->second * eff.popKillPct / 100.0f);
                pIt->second = std::max(0LL, pIt->second - killed);
            }
        }

        // Damage fortifications
        if (eff.fortDmg > 0 || eff.fortChance > 0) {
            auto indIt = m_provinceIndustry.find(ao.targetProvince);
            if (indIt != m_provinceIndustry.end()) {
                float dmg = eff.fortDmg;
                if (eff.fortChance > 0 && (rand() % 100) < eff.fortChance) dmg += 1;
                indIt->second.fortification = std::max(0, indIt->second.fortification - (int)dmg);
            }
        }

        // Damage industry
        if (eff.indDmg > 0) {
            auto indIt = m_provinceIndustry.find(ao.targetProvince);
            if (indIt != m_provinceIndustry.end()) {
                indIt->second.level = std::max(0, indIt->second.level - eff.indDmg);
                indIt->second.income *= 0.5f; // halve income from damaged industry
            }
        }

        // Remove processed order
        m_pendingArtilleryOrders.erase(m_pendingArtilleryOrders.begin() + i);
    }
}

// === processTurn ===
void Game::processTurn() {
    // Lambda to draw a loading screen frame (no-op if loading screen not active)
    auto drawFrame = [&](float pct, const char* status) {
        if (!m_showLoadingScreen) return;
        setLoadingProgress(pct, status);
        BeginDrawing();
        ClearBackground(BLACK);
        drawLoadingScreen();
    };
    if (m_config.aiDebug) printf("[TURN] Processing turn %d...\n", m_turnNumber + 1);

    // GameProcess hook. A mod that traps here is disabled, not fatal.
    ModManager::get().preTurn(m_turnNumber + 1);
    auto t0 = std::chrono::steady_clock::now();
    // Snapshot current state for turn delta
    drawFrame(0.01f, "Snapshoting state...");
    auto prevOwnership = m_provinceCountryLookup;
    auto prevPopulations = m_provincePopulations;
    auto prevIndustry = m_provinceIndustry;
    auto prevShips = m_ships;
    auto prevArmies = m_provinceArmies;
    int turnNum = m_turnNumber + 1;
    auto t1 = std::chrono::steady_clock::now();
    // Pre-compute all country incomes in a single province pass (avoids 356 redundant scans)
    refreshIncomeCache();
    m_rebellionsThisTurnByCid.clear();
    // Country AI: created lazily on the first processed turn so map load stays
    // instant; the model persists across saves in the game data directory.
    if (!m_ai) m_ai = new AISystem(this, m_dataDir + "ai/model.bin");
    m_ai->beginTurn();
    drawFrame(0.02f, "Processing countries...");
    // Process per-country actions in batches with loading frames
    // Pre-count active countries for progress tracking
    int totalCountries = 0;
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid != UNC_CID && cid != BLC_CID && cid != SPC_CID) totalCountries++;
    }
    int processedCountries = 0;
    int batchCounter = 0;
    const int BATCH_SIZE = 20;
    // Snapshot the cid list: processRebellions inserts new rebel countries
    // into m_countries mid-loop, and an unordered_map rehash mid-iteration is
    // undefined behavior (rare, but a guaranteed eventual crash on long runs).
    // Fresh rebels take their first turn next turn, which is also the sane rule.
    std::vector<int> turnCids;
    turnCids.reserve(m_countries.getAll().size());
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID || cid == SPC_CID) continue;
        turnCids.push_back(cid);
    }
    for (int cid : turnCids) {
        if (!m_countries.getCountry(cid)) continue; // eliminated mid-turn
        processCountryTurn(cid);
        processedCountries++;
        batchCounter++;
        if (batchCounter >= BATCH_SIZE || processedCountries >= totalCountries) {
            batchCounter = 0;
            float pct = 0.02f + 0.45f * (totalCountries > 0 ? (float)processedCountries / totalCountries : 0.0f);
            char buf[80];
            snprintf(buf, sizeof(buf), "Processing countries... %d/%d", processedCountries, totalCountries);
            drawFrame(pct, buf);
        }
    }
    auto t2 = std::chrono::steady_clock::now();
    drawFrame(0.50f, "Processing upgrades...");
    processUpgrades();
    auto t3 = std::chrono::steady_clock::now();
    drawFrame(0.52f, "Processing navy and diplomacy...");
    // Every country's engage orders execute, not just the player's — AI navies
    // could previously queue engagements that never resolved.
    for (auto& [navyCid, navyC] : m_countries.getAll()) {
        if (navyCid == UNC_CID || navyCid == BLC_CID || navyCid == SPC_CID) continue;
        processNavyCombat(navyCid);
    }
    processDiplomaticRequests();
    auto t4 = std::chrono::steady_clock::now();
    drawFrame(0.55f, "Processing population...");
    processPopulation();
    auto t5 = std::chrono::steady_clock::now();
    drawFrame(0.58f, "Updating policies...");
    updatePolicies();
    auto t6 = std::chrono::steady_clock::now();
    drawFrame(0.60f, "Eliminating defeated countries...");
    eliminateDefeatedCountries();
    auto t7 = std::chrono::steady_clock::now();
    drawFrame(0.62f, "Generating political map...");
    // Self-play training never looks at the map: skip the full-raster texture
    // and label passes, they dominate turn time on big maps.
    if (!m_aiTraining) generatePoliticalTexture();
    // One label rebuild per turn no matter how many rebellions/ceasefires
    // marked them dirty — computeCountryLabels is a full-map raster scan.
    if (m_labelsDirty) {
        m_labelsDirty = false;
        if (!m_aiTraining) {
            computeCountryLabels();
            if (m_renderer) m_renderer->setCountryLabels(&m_countryLabels);
        }
    }
    auto t8 = std::chrono::steady_clock::now();
    drawFrame(0.65f, "Syncing population data...");
    // Sync population array and regenerate population texture for player country
    for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
        if ((size_t)pid < m_provincePopArray.size()) {
            auto it = m_provincePopulations.find(pid);
            m_provincePopArray[pid] = (it != m_provincePopulations.end()) ? it->second : 0;
        }
    }
    if (m_playerCountryId > 0 && m_playerCountryId != SPC_CID)
        generatePopulationTexture(m_playerCountryId, -1);
    auto t9 = std::chrono::steady_clock::now();
    if (m_config.aiDebug)
    printf("[TURN] timing: snapshot=%lldms countryTurn=%lldms upgrades=%lldms navy+diplo=%lldms pop=%lldms policies=%lldms elim=%lldms politTexture=%lldms popSync=%lldms\n",
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count(),
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t2-t1).count(),
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t3-t2).count(),
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t4-t3).count(),
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t5-t4).count(),
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t6-t5).count(),
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t7-t6).count(),
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t8-t7).count(),
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t9-t8).count());
    // Build TurnDelta from diff
    drawFrame(0.68f, "Building turn delta...");
    TurnDelta delta;
    delta.turnNumber = turnNum;
    delta.researchAllocation = m_researchAllocation;
    delta.pacificationAllocation = m_pacificationAllocation;
    delta.researchActiveNode = m_researchActiveNode;
    delta.researchPoints = m_researchPoints;
    // Province changes: ownership, population, industry, fortification
    for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
        ProvinceDelta pd;
        pd.provinceId = pid;
        bool changed = false;
        if ((size_t)pid < prevOwnership.size() && prevOwnership[pid] != prov.countryId) {
            pd.ownerChanged = true; pd.newOwner = prov.countryId; changed = true;
            // Track conquest for refugee surge (skip UNC→player and player→UNC transitions)
            int prevOwner = prevOwnership[pid];
            if (prevOwner > 0 && prevOwner != UNC_CID && prevOwner != BLC_CID &&
                prov.countryId > 0 && prov.countryId != UNC_CID && prov.countryId != BLC_CID) {
                m_provinceConquestTurn[pid] = m_turnNumber;
                m_conqueredProvincePrevOwner[pid] = prevOwner;
            }
        }
        auto prevPopIt = prevPopulations.find(pid);
        auto curPopIt = m_provincePopulations.find(pid);
        long long prevPop = prevPopIt != prevPopulations.end() ? prevPopIt->second : 0;
        long long curPop = curPopIt != m_provincePopulations.end() ? curPopIt->second : 0;
        if (prevPop != curPop) {
            pd.populationChanged = true; pd.newPopulation = curPop; changed = true;
        }
        auto prevIndIt = prevIndustry.find(pid);
        auto curIndIt = m_provinceIndustry.find(pid);
        bool hadInd = prevIndIt != prevIndustry.end();
        bool hasInd = curIndIt != m_provinceIndustry.end();
        if (hadInd || hasInd) {
            int prevLvl = hadInd ? prevIndIt->second.level : 0;
            int curLvl = hasInd ? curIndIt->second.level : 0;
            int prevFort = hadInd ? prevIndIt->second.fortification : 0;
            int curFort = hasInd ? curIndIt->second.fortification : 0;
            float prevInc = hadInd ? prevIndIt->second.income : 0;
            float curInc = hasInd ? curIndIt->second.income : 0;
            float prevResInc = hadInd ? prevIndIt->second.resourceIncome : 0;
            float curResInc = hasInd ? curIndIt->second.resourceIncome : 0;
            float prevPopInc = hadInd ? prevIndIt->second.popIncome : 0;
            float curPopInc = hasInd ? curIndIt->second.popIncome : 0;
            float prevPopMod = hadInd ? prevIndIt->second.popModifier : 1.0f;
            float curPopMod = hasInd ? curIndIt->second.popModifier : 1.0f;
            if (prevLvl != curLvl) { pd.industryLevelChanged = true; pd.newIndustryLevel = curLvl; changed = true; }
            if (prevFort != curFort) { pd.fortificationChanged = true; pd.newFortification = curFort; changed = true; }
            if (prevInc != curInc) { pd.incomeChanged = true; pd.newIncome = curInc; changed = true; }
            if (prevResInc != curResInc) { pd.resourceIncomeChanged = true; pd.newResourceIncome = curResInc; changed = true; }
            if (prevPopInc != curPopInc) { pd.popIncomeChanged = true; pd.newPopIncome = curPopInc; changed = true; }
            if (prevPopMod != curPopMod) { pd.popModifierChanged = true; pd.newPopModifier = curPopMod; changed = true; }
        }
        if (changed) delta.provinces.push_back(pd);
    }
    // Ship changes: position, health, crew
    for (size_t i = 0; i < m_ships.size() && i < prevShips.size(); ++i) {
        ShipDelta sd;
        sd.shipIndex = i;
        bool changed = false;
        if (m_ships[i].countryId != prevShips[i].countryId) { sd.countryIdChanged = true; sd.newCountryId = m_ships[i].countryId; changed = true; }
        if (m_ships[i].lat != prevShips[i].lat) { sd.latChanged = true; sd.newLat = m_ships[i].lat; changed = true; }
        if (m_ships[i].lon != prevShips[i].lon) { sd.lonChanged = true; sd.newLon = m_ships[i].lon; changed = true; }
        if (m_ships[i].health != prevShips[i].health) { sd.healthChanged = true; sd.newHealth = m_ships[i].health; changed = true; }
        if (m_ships[i].crew != prevShips[i].crew) { sd.crewChanged = true; sd.newCrew = m_ships[i].crew; changed = true; }
        if (changed) delta.ships.push_back(sd);
    }
    // Army changes: record current state of all armies
    for (auto& [pid, units] : m_provinceArmies) {
        ArmyDelta ad;
        ad.provinceId = pid;
        for (auto& u : units) ad.units.push_back({u.countryId, u.count});
        delta.armies.push_back(ad);
    }
    // Also record provinces where armies were removed
    for (auto& [pid, units] : prevArmies) {
        if (m_provinceArmies.find(pid) == m_provinceArmies.end()) {
            ArmyDelta ad;
            ad.provinceId = pid;
            delta.armies.push_back(ad); // Empty units = cleared
        }
    }
    // Persist turn delta to .odsv
    drawFrame(0.85f, "Saving turn data...");
    if (!m_currentSavePath.empty()) {
        // One archive rewrite, not two: appendTurn also writes the state
        // snapshot (pending orders, claims, research, ...) and rebel flags.
        std::vector<std::pair<std::string, std::string>> rebelFiles;
        for (auto& [cid2, svg] : m_rebelFlagSvgs)
             rebelFiles.push_back({"rebellion/" + std::to_string(cid2) + ".svg", svg});
         // Persist the rebel countries themselves, not just their flags —
         // otherwise their provinces reload as ownerless limbo.
         { std::string rj = buildRebelsJson(); if (!rj.empty()) rebelFiles.push_back({"rebels.json", rj}); }
        std::string stateJson = saveStateJson();
        SaveManager::appendTurn(m_currentSavePath, delta, &stateJson, &rebelFiles);
        m_turnCount++;
    }
    drawFrame(0.90f, "Cleaning up...");
    recordIncomeSnapshot();
    // AI learning step: rewards from the turn's state deltas. Runs while the
    // income cache recordIncomeSnapshot just refreshed is still hot, so the
    // per-country post-turn income reads are O(1) instead of full map scans.
    if (m_ai) m_ai->endTurn();
    m_countryIncomeCache.clear();
    // Cleanup sunk ships AFTER delta building so ship indices stay stable during comparison
    cleanupSunkShips();
    // Advance date by one month
    drawFrame(0.95f, "Advancing date...");
    {
        static const char* MONTHS[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
        int mi = -1, yr = 2000;
        char mb[32]={}; int scanned = sscanf(m_mapDate.c_str(), "%s %d", mb, &yr);
        if (scanned >= 2) {
            for (int i = 0; i < 12; ++i) if (strcmp(mb, MONTHS[i]) == 0) { mi = i; break; }
        }
        // Preserve the era suffix (BC dates count years down toward 1 BC/AD)
        bool isBC = m_mapDate.find("BC") != std::string::npos;
        const char* era = isBC ? "BC" : "AD";
        if (mi >= 0 && mi < 11) {
            m_mapDate = std::string(MONTHS[mi+1]) + " " + std::to_string(yr) + " " + era;
        } else if (isBC && yr > 1) {
            m_mapDate = std::string("January ") + std::to_string(yr - 1) + " BC";
        } else {
            m_mapDate = std::string("January ") + std::to_string(yr + 1) + " AD";
        }
    }
    m_turnNumber++;
    // Resume any map scripts suspended on waitUntil now that turn/date advanced
    if (m_scriptEngine) {
        m_scriptEngine->tick();
        if (!m_scriptEngine->getErrors().empty()) m_scriptErrorTimer = 3.0f;
    }
    m_turnState = TURN_NORMAL;

    // GameProcess post-turn hook. This also services a reload that was asked
    // for mid-turn: reloads happen between turns, never inside one.
    ModManager::get().postTurn(m_turnNumber);

    drawFrame(1.0f, "Done!");
    if (m_config.aiDebug) printf("[TURN] Turn %d processed.\n", turnNum);
}

// === processCountryTurn ===
void Game::processCountryTurn(int countryId) {
    if (countryId <= 0 || countryId == SPC_CID || countryId == UNC_CID || countryId == BLC_CID) return;
    auto pt0 = std::chrono::steady_clock::now();
    processEconomy(countryId);
    // AI countries think AFTER their economy resolves (fresh treasury) and
    // BEFORE order execution, so orders enqueued here fire this same turn.
    // Research progresses AFTER the AI's snapshot: a node completed here is
    // then visible as a delta in the learning step — progressing before the
    // snapshot made every completion invisible (and unrewarded, and uncounted).
    if (m_ai && countryId != m_playerCountryId) m_ai->takeTurn(countryId);
    if (countryId != m_playerCountryId) progressCountryResearch(countryId);
    auto pt1 = std::chrono::steady_clock::now();
    processArtilleryOrders(countryId);
    auto pt2 = std::chrono::steady_clock::now();
    processShipBombardOrders(countryId);
    auto pt3 = std::chrono::steady_clock::now();
    processShipDisembarks(countryId);
    auto pt4 = std::chrono::steady_clock::now();
    processArmyMovement(countryId);
    auto pt5 = std::chrono::steady_clock::now();
    processNavyMovement(countryId);
    auto pt6 = std::chrono::steady_clock::now();
    processDisbandOrders(countryId);
    auto pt7 = std::chrono::steady_clock::now();
    processScrapShips(countryId);
    auto pt8 = std::chrono::steady_clock::now();
    processRecruitments(countryId);
    auto pt9 = std::chrono::steady_clock::now();
    processEmbarkations(countryId);
    processRebellions(countryId);
    auto pt10 = std::chrono::steady_clock::now();
    long long te = (long long)std::chrono::duration_cast<std::chrono::microseconds>(pt1-pt0).count();
    if (te > 1000) printf("[PROCESS] cid=%d economy=%lldus\n", countryId, te);
    te = (long long)std::chrono::duration_cast<std::chrono::microseconds>(pt2-pt1).count();
    if (te > 1000) printf("[PROCESS] cid=%d artillery=%lldus\n", countryId, te);
    te = (long long)std::chrono::duration_cast<std::chrono::microseconds>(pt3-pt2).count();
    if (te > 1000) printf("[PROCESS] cid=%d bombard=%lldus\n", countryId, te);
    te = (long long)std::chrono::duration_cast<std::chrono::microseconds>(pt4-pt3).count();
    if (te > 1000) printf("[PROCESS] cid=%d disembark=%lldus\n", countryId, te);
    te = (long long)std::chrono::duration_cast<std::chrono::microseconds>(pt5-pt4).count();
    if (te > 1000) printf("[PROCESS] cid=%d armyMove=%lldus\n", countryId, te);
    te = (long long)std::chrono::duration_cast<std::chrono::microseconds>(pt6-pt5).count();
    if (te > 1000) printf("[PROCESS] cid=%d navyMove=%lldus\n", countryId, te);
    te = (long long)std::chrono::duration_cast<std::chrono::microseconds>(pt7-pt6).count();
    if (te > 1000) printf("[PROCESS] cid=%d disband=%lldus\n", countryId, te);
    te = (long long)std::chrono::duration_cast<std::chrono::microseconds>(pt8-pt7).count();
    if (te > 1000) printf("[PROCESS] cid=%d scrap=%lldus\n", countryId, te);
    te = (long long)std::chrono::duration_cast<std::chrono::microseconds>(pt9-pt8).count();
    if (te > 1000) printf("[PROCESS] cid=%d recruit=%lldus\n", countryId, te);
    te = (long long)std::chrono::duration_cast<std::chrono::microseconds>(pt10-pt9).count();
    if (te > 1000) printf("[PROCESS] cid=%d embark=%lldus\n", countryId, te);
}

// Rebel countries only ever existed in memory: createRebelCountry() inserts
// them into m_countries at runtime and the save wrote nothing but their flag
// SVG. On reload every province they owned referenced a missing country id,
// which is why captured territory ended up in limbo without even a UNC/BLC
// tag. Emitting them in countries.json shape lets CountryMap::loadFromJson
// merge them straight back in.
std::string Game::buildRebelsJson() const {
    nlohmann::json root = nlohmann::json::object();
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid < REBEL_CID_MIN || cid >= SPC_CID) continue; // skip map countries + UNC/BLC/SPC
        nlohmann::json e;
        e["id"] = cid;
        e["name"] = c.name;
        e["iso_a3"] = c.isoA3;
        char hex[8];
        snprintf(hex, sizeof(hex), "#%02x%02x%02x", c.color.r, c.color.g, c.color.b);
        e["color"] = std::string(hex);
        e["treasury"] = c.treasury;
        e["compass_economic"] = c.compassEconomic;
        e["compass_social"] = c.compassSocial;
        root[std::to_string(cid)] = e;
    }
    if (root.empty()) return std::string();
    return root.dump();
}

void Game::restoreRebels(const std::string& savePath) {
    if (savePath.empty()) return;
    std::string rebels = SaveManager::readEntry(savePath, "rebels.json");
    if (rebels.empty()) return;
    // Merges by id without clearing, so map countries are left alone
    m_countries.loadFromJson(rebels);
    rebuildIsoIndex();

    // Re-attach their flags and make sure new rebels don't reuse a live id
    try {
        auto j = nlohmann::json::parse(rebels);
        for (auto& [cidStr, e] : j.items()) {
            int cid = atoi(cidStr.c_str());
            if (cid >= m_nextRebelCid) m_nextRebelCid = cid + 1;
            std::string svg = SaveManager::readEntry(savePath, "rebellion/" + cidStr + ".svg");
            if (!svg.empty()) m_rebelFlagSvgs[cid] = svg;
        }
        std::cout << "  Restored " << j.size() << " rebel countries" << std::endl;
    } catch (...) { std::cerr << "  Failed to parse rebels.json" << std::endl; }
}

void Game::rebuildIsoIndex() {
    m_isoToCid.clear();
    for (auto& [cid, c] : m_countries.getAll())
        if (!c.isoA3.empty()) m_isoToCid[c.isoA3] = cid;
}

int Game::cidForIso(const std::string& iso) const {
    auto it = m_isoToCid.find(iso);
    return it != m_isoToCid.end() ? it->second : -1;
}

void Game::synthesizeMissingRebels() {
    // Collect rebel cids that provinces point at but no country exists for.
    std::unordered_set<int> missing;
    for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
        int cid = prov.countryId;
        if (cid >= REBEL_CID_MIN && cid < SPC_CID && !m_countries.getCountry(cid))
            missing.insert(cid);
    }
    if (missing.empty()) return;

    for (int cid : missing) {
        Country c;
        c.id = cid;
        c.name = "Rebel State " + std::to_string(cid - REBEL_CID_MIN + 1);
        // Deterministic ISO + colour from the cid so reloads stay stable.
        char iso[4];
        snprintf(iso, sizeof(iso), "R%02d", (cid - REBEL_CID_MIN) % 100);
        c.isoA3 = iso;
        unsigned h = (unsigned)(cid * 2654435761u);
        c.color = {(uint8_t)(90 + (h & 0x7F)), (uint8_t)(70 + ((h >> 8) & 0x7F)),
                   (uint8_t)(90 + ((h >> 16) & 0x7F)), 255};
        c.treasury = 0.0;
        m_countries.getAll()[cid] = c;
        if (cid >= m_nextRebelCid) m_nextRebelCid = cid + 1;
    }
    rebuildIsoIndex();
    std::cout << "  Synthesized " << missing.size()
              << " placeholder rebel state(s) missing from the save" << std::endl;
}

// === allocateRebelCid ===
int Game::allocateRebelCid() {
    // Rebel cids must stay inside [REBEL_CID_MIN, SPC_CID). Long self-play
    // runs create thousands of rebels; the old unbounded ++ walked the counter
    // straight into the SPC/UNC/BLC sentinel ids (65533-65535) — overwriting
    // those pseudo-countries in m_countries — and then past 65535, where every
    // 16-bit owner field (turn-history codec) silently truncates. Wrap within
    // the band and reuse cids freed by eliminated rebels instead.
    const int SPAN = SPC_CID - REBEL_CID_MIN;
    if (m_nextRebelCid < REBEL_CID_MIN || m_nextRebelCid >= SPC_CID)
        m_nextRebelCid = REBEL_CID_MIN;
    for (int tries = 0; tries < SPAN; ++tries) {
        int cid = m_nextRebelCid;
        m_nextRebelCid = REBEL_CID_MIN + (cid + 1 - REBEL_CID_MIN) % SPAN;
        if (m_countries.getCountry(cid) == nullptr) return cid;
    }
    // Every cid in the band is occupied (pathological): recycle an eliminated
    // rebel rather than ever touching the sentinel range.
    for (int cid = REBEL_CID_MIN; cid < SPC_CID; ++cid)
        if (m_eliminatedCids.count(cid)) return cid;
    return REBEL_CID_MIN;
}

// === createRebelCountry ===
void Game::createRebelCountry(int rebelCid, int parentCid, const std::vector<int>& provinceIds) {
    std::string regionName;
    long long totalPop = 0;
    float avgEcon = 0, avgSoc = 0;
    // Collect minority info across rebel provinces
    std::unordered_map<std::string, float> minorityTotals;
    for (int pid : provinceIds) {
        auto popIt = m_provincePopulations.find(pid);
        long long pop = (popIt != m_provincePopulations.end()) ? popIt->second : 0;
        totalPop += pop;
        if (regionName.empty()) {
            const Province* p = m_provinces.getProvinceById(pid);
            if (p) regionName = p->name;
        }
        auto compIt = m_provinceCompass.find(pid);
        if (compIt != m_provinceCompass.end()) {
            avgEcon += compIt->second.x;
            avgSoc += compIt->second.y;
        }
        // Sum minority percentages across faction provinces
        auto mit = m_provinceMinorities.find(pid);
        if (mit != m_provinceMinorities.end()) {
            for (auto& mg : mit->second)
                minorityTotals[mg.name] += mg.pct;
        }
    }
    if (!provinceIds.empty()) {
        avgEcon /= provinceIds.size();
        avgSoc /= provinceIds.size();
    }

    // Find the dominant minority or use region name
    std::string ethnicName;
    float maxMinorityPct = 0;
    for (auto& [name, pct] : minorityTotals) {
        if (pct > maxMinorityPct) { maxMinorityPct = pct; ethnicName = name; }
    }

    // ── Second-largest minority naming ──
    // If the second-largest minority averages ≥20% across rebel provinces,
    // name the rebel country after that minority instead of the dominant one.
    bool secondMinorityNaming = false;
    if (!ethnicName.empty()) {
        // Sort minorities by rebel total descending
        std::vector<std::pair<std::string, float>> sorted(minorityTotals.begin(), minorityTotals.end());
        std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
        std::string secondEthnic;
        float secondPct = 0;
        for (auto& [name, pct] : sorted) {
            if (name != ethnicName) { secondEthnic = name; secondPct = pct; break; }
        }
        // Name after the second-largest minority if it averages ≥20% across rebel provinces
        if (!secondEthnic.empty() && secondPct > provinceIds.size() * 20.0f) {
            ethnicName = secondEthnic;
            secondMinorityNaming = true;
            if (m_config.aiDebug)
                printf("[REBELLION] Named after second-largest minority '%s' (%.1f%% avg across %zu provinces)\n",
                   secondEthnic.c_str(), secondPct / provinceIds.size(), provinceIds.size());
        }
    }

    // ── Procedural name generation ──
    bool hasEthnicBasis = !ethnicName.empty() && (secondMinorityNaming || maxMinorityPct > provinceIds.size() * 40.0f);
    std::string countryName;

    // Helper: check if a proposed name is too similar to an existing or parent country
    std::string parentName_lower = m_countries.getAll()[parentCid].name;
    std::transform(parentName_lower.begin(), parentName_lower.end(), parentName_lower.begin(), ::tolower);
    auto nameConflicts = [&](const std::string& proposed) -> bool {
        std::string low1 = proposed;
        std::transform(low1.begin(), low1.end(), low1.begin(), ::tolower);
        // Check against parent too (prevents "Mongolia" from a Mongolia rebel)
        if (low1 == parentName_lower) return true;
        for (auto& [cid, c] : m_countries.getAll()) {
            if (cid == rebelCid) continue;
            std::string low2 = c.name;
            std::transform(low2.begin(), low2.end(), low2.begin(), ::tolower);
            if (low1 == low2) return true;
            if (low1.size() >= 4 && low2.size() >= 4 &&
                (low1.find(low2) != std::string::npos || low2.find(low1) != std::string::npos))
                return true;
        }
        return false;
    };

    // Helper: derive a plausible territory name from an ethnicity
    static const std::vector<std::string> SUFFIXES = {"ia", "a", "stan", "istan", "land", ""};
    static const char* DIR_WORDS[] = {"south ", "north ", "east ", "west ", "central "};
    auto stripDirection = [&](const std::string& s) -> std::string {
        std::string low = s;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        for (const char* dw : DIR_WORDS) {
            size_t dwl = strlen(dw);
            if (low.size() > dwl && low.substr(0, dwl) == dw)
                return s.substr(dwl);
        }
        return s;
    };
    auto deriveTerritory = [&](const std::string& ethnicity) -> std::string {
        std::string root = ethnicity;
        if (root.empty()) return ethnicity;
        // Strip trailing 's' (e.g., "Assyrians" → "Assyrian")
        if (root.back() == 's') root.pop_back();
        if (root.empty()) return ethnicity;

        bool endsInI = root.back() == 'i';
        bool endsInN = root.back() == 'n';
        bool endsInE = root.back() == 'e';
        bool endsInAn = root.size() >= 3 && root.substr(root.size()-2) == "an";
        bool endsInIn = root.size() >= 3 && root.substr(root.size()-2) == "in";
        bool endsInIan = root.size() >= 4 && root.substr(root.size()-3) == "ian";
        bool endsInIsh = root.size() >= 4 && root.substr(root.size()-3) == "ish";
        bool endsInEse = root.size() >= 4 && root.substr(root.size()-3) == "ese";

        // Helper: add suffix avoiding doubled vowels
        auto addSuffix = [&](const std::string& base, const std::string& sfx) -> std::string {
            if (base.empty()) return sfx;
            if (sfx.empty()) return base;
            char lastB = tolower(base.back());
            char firstS = tolower(sfx.front());
            // If last letter of base equals first letter of suffix, drop one
            if (lastB == firstS) return base + sfx.substr(1);
            return base + sfx;
        };

        // Try suffixes in shuffled order until a non-conflicting name emerges
        std::vector<int> suffixOrder = {0,1,2,3,4,5};
        for (size_t si = 0; si < suffixOrder.size(); ++si) {
            size_t j = si + rand() % (suffixOrder.size() - si);
            std::swap(suffixOrder[si], suffixOrder[j]);
        }

        // For endsInIan (Austrian → Austria, Russian → Russia): strip "ian" + "ia"
        if (endsInIan) {
            std::string t = root.substr(0, root.size()-3) + "ia";
            if (!nameConflicts(t)) return t;
            // Also try just dropping the 'n' (Russian → Russia)
            t = root.substr(0, root.size()-1);
            if (!nameConflicts(t)) return t;
            // Try + "a" as fallback
            t = root.substr(0, root.size()-3) + "a";
            if (!nameConflicts(t)) return t;
        }
        // For endsInAn (Assyrian → Assyria): strip "an" + "ia" (but only if not also endsInIan which we already tried)
        if (endsInAn && !endsInIan) {
            std::string t = root.substr(0, root.size()-2) + "ia";
            if (!nameConflicts(t)) return t;
            t = root.substr(0, root.size()-2) + "a";
            if (!nameConflicts(t)) return t;
        }
        // For endsInIn (Montenegrin → Montenegro): strip "in" + "o"
        if (endsInIn) {
            std::string t = root.substr(0, root.size()-2) + "o";
            if (!nameConflicts(t)) return t;
        }
        // For endsInI (Punjabi → Punjab): drop 'i' + "ab"
        if (endsInI) {
            std::string base = root.substr(0, root.size()-1);
            std::string t = base + "ab";
            if (!nameConflicts(t)) return t;
        }
        // For endsInish (English → England): strip "ish" + "land"
        if (endsInIsh) {
            std::string t = root.substr(0, root.size()-3) + "land";
            if (!nameConflicts(t)) return t;
        }
        // For endsInEse (Japanese → Japan): strip "ese" + leave as root base
        if (endsInEse) {
            std::string base = root.substr(0, root.size()-3);
            std::string t = base + "a";
            if (!nameConflicts(t)) return t;
            t = base;
            if (!nameConflicts(t)) return t;
        }
        // For endsInE (Basque → basque): try root + "ia", root itself
        if (endsInE) {
            std::string t1 = addSuffix(root, "ia");
            if (!nameConflicts(t1)) return t1;
            if (!nameConflicts(root)) return root;
        }
        // For endsInN (non-an, e.g., "German"): try dropping 'n' + suffixes
        if (endsInN && !endsInAn && !endsInIan) {
            std::string base = root.substr(0, root.size()-1);
            for (int sii : suffixOrder) {
                std::string t = addSuffix(base, SUFFIXES[sii]);
                if (t.empty()) continue;
                if (!nameConflicts(t)) return t;
            }
        }

        // Generic suffix trial with vowel deduplication
        for (int sii : suffixOrder) {
            const std::string& sfx = SUFFIXES[sii];
            std::string t;
            if (endsInI && sfx.empty()) {
                t = root.substr(0, root.size()-1);
            } else {
                t = addSuffix(root, sfx);
            }
            if (t.empty()) continue;
            if (!nameConflicts(t)) return t;
        }
        // Last resort: directional modifier on root
        std::string dirRoot = stripDirection(root);
        if (dirRoot != root && !nameConflicts(dirRoot)) return dirRoot;
        return root;
    };

    auto makeDirectionalName = [&](const std::string& base, const std::string& parentName) -> std::string {
        static const char* dirs[] = {"South ", "North ", "East ", "West ", "Central "};
        std::string place = base.empty() ? parentName : base;
        if (place.size() > 4 && place.substr(0,4) == "The ") place = place.substr(4);
        // Strip existing direction to avoid "South South X" doubling
        std::string lowPlace = place;
        std::transform(lowPlace.begin(), lowPlace.end(), lowPlace.begin(), ::tolower);
        for (const char* dw : DIR_WORDS) {
            size_t dwl = strlen(dw);
            if (lowPlace.size() > dwl && lowPlace.substr(0, dwl) == dw) {
                place = place.substr(dwl);
                break;
            }
        }
        int di = rand() % 5;
        return std::string(dirs[di]) + place;
    };

    if (hasEthnicBasis) {
        // Step 1: See if the ethnicity name itself is too close to an existing country
        // (e.g., "Japanese" → Japan exists, "Georgian" → Georgia exists)
        std::string parentName = m_countries.getAll()[parentCid].name;
        std::string territory = deriveTerritory(ethnicName);

        // Step 2: if territory conflicts with existing countries, try directional
        if (nameConflicts(territory)) {
            territory = makeDirectionalName(territory, parentName);
        }

        // Step 3: wrap with ideology prefix/suffix (50% plain, 25% prefix, 25% suffix)
        int nr = rand() % 100;
        if (nr < 50) {
            countryName = territory;
        } else if (nr < 75) {
            if (avgEcon < -20) countryName = "People's Republic of " + territory;
            else if (avgEcon > 30) countryName = "Free State of " + territory;
            else if (avgSoc < -20) countryName = "State of " + territory;
            else countryName = "Republic of " + territory;
        } else {
            static const char* genSuffixes[] = {" Liberation Front", " National Council", " Independence Movement", " State"};
            int si = rand() % 4;
            countryName = territory + genSuffixes[si];
        }

        // Final conflict check: if still conflicting, fall back to directional
        if (nameConflicts(countryName)) {
            countryName = makeDirectionalName(territory, parentName);
        }
    } else {
        // Region-based naming — try directional first if parent country has many provinces
        std::string parentName = m_countries.getAll()[parentCid].name;
        std::string nameBase = regionName;
        std::string baseName;

        // Use region name, but if it's too similar to parent, make directional
        if (nameBase.empty()) nameBase = parentName;

        if (rand() % 100 < 40 && !nameConflicts("South " + nameBase)) {
            // 40% chance of directional name with ideology prefix
            std::string dirStr = makeDirectionalName(nameBase, parentName);
            if (rand() % 100 < 50) {
                countryName = dirStr;
            } else {
                if (avgEcon < -20) countryName = "People's Republic of " + dirStr;
                else if (avgEcon > 30) countryName = "Free State of " + dirStr;
                else countryName = "Republic of " + dirStr;
            }
        } else {
            // Original ideology template approach
            static const char* leftPre[] = {
                "People's Liberation Army of ", "Socialist Movement of ",
                "People's Republic of ", "Revolutionary Council of ",
                "Popular Front for the Liberation of "
            };
            static const char* farLeftPre[] = {
                "People's Liberation Army of ", "Communist Party of ",
                "People's Republic of ", "Red Army of "
            };
            static const char* rightPre[] = {
                "Free ", "Liberation Front of ", "Independence Movement of ",
                "Democratic Alliance of "
            };
            static const char* farRightPre[] = {
                "Free State of ", "National Front of ", "Liberation Army of "
            };
            static const char* authPre[] = {
                "National Salvation Council of ", "State of ",
                "Authority for the Liberation of "
            };
            static const char* libPre[] = {
                "Democratic Movement of ", "Free People of ",
                "Liberal Alliance of "
            };
            static const char* neutralPre[] = {
                "Liberation Army of ", "Freedom Fighters of ",
                "Separatist Movement of ", "Resistance Council of "
            };
            static const char* leftOneWord[] = {
                "The Commune", "The Collective", "Soviet", "The Union"
            };
            static const char* farLeftOneWord[] = {
                "The Commune", "The Collective", "Soviet", "Red Star", "The Proletariat"
            };
            static const char* rightOneWord[] = {
                "The Alliance", "The Federation", "The Directorate"
            };
            static const char* farRightOneWord[] = {
                "The Dominion", "The Imperium", "The Authority"
            };
            static const char* authOneWord[] = {
                "The Junta", "The Council", "The Order"
            };
            static const char* libOneWord[] = {
                "The Republic", "The Commonwealth", "The Concord"
            };
            static const char* neutralOneWord[] = {
                "Libertalia", "The Resistance", "The Front", "The Movement"
            };

            const char** multiWordSet = neutralPre;
            const char** oneWordSet = neutralOneWord;
            int multiWordCount = 4;
            int oneWordCount = 4;
            if (avgEcon < -40) {
                multiWordSet = farLeftPre; multiWordCount = 4;
                oneWordSet = farLeftOneWord; oneWordCount = 5;
            } else if (avgEcon < -20) {
                multiWordSet = leftPre; multiWordCount = 5;
                oneWordSet = leftOneWord; oneWordCount = 4;
            } else if (avgEcon > 40) {
                multiWordSet = farRightPre; multiWordCount = 3;
                oneWordSet = farRightOneWord; oneWordCount = 3; // farRightOneWord has 3 entries
            } else if (avgEcon > 20) {
                multiWordSet = rightPre; multiWordCount = 4;
                oneWordSet = rightOneWord; oneWordCount = 3;
            } else if (avgSoc < -30) {
                multiWordSet = authPre; multiWordCount = 3;
                oneWordSet = authOneWord; oneWordCount = 3;
            } else if (avgSoc > 30) {
                multiWordSet = libPre; multiWordCount = 3;
                oneWordSet = libOneWord; oneWordCount = 3; // libOneWord has 3 entries
            }
            if (rand() % 100 < 60) {
                int ti = rand() % oneWordCount;
                countryName = oneWordSet[ti];
            } else {
                int ti = rand() % multiWordCount;
                countryName = std::string(multiWordSet[ti]) + nameBase;
            }
        }
        // Final conflict check for region-based path
        if (nameConflicts(countryName)) {
            std::string parentName = m_countries.getAll()[parentCid].name;
            countryName = makeDirectionalName(regionName.empty() ? parentName : regionName, parentName);
        }
    }

    // Safety net: if name still conflicts, use a generic pattern
    if (nameConflicts(countryName)) {
        std::string parentName = m_countries.getAll()[parentCid].name;
        static const char* fallbackDirs[] = {"South ", "North ", "East ", "West ", "Central "};
        countryName = std::string(fallbackDirs[rand() % 5]) + parentName + " Breakaway";
    }

    int rebelNum = rebelCid - 60000;
    std::string isoA3 = (rebelNum < 10) ? "R0" + std::to_string(rebelNum) : "R" + std::to_string(rebelNum);

    // ── Rich flag generation (HSL-based harmonious palettes) ──
    Color c1, c2;
    {
        // Pick a primary hue based on ideology
        int hue;
        int sat = 70 + rand() % 25;     // 70-95%
        int lig = 40 + rand() % 20;     // 40-60%

        if (avgEcon < -30) {
            // Far-left: reds (0±20)
            hue = (rand() % 40 - 20 + 360) % 360;
        } else if (avgEcon > 30) {
            // Far-right: deep blues (220±20)
            hue = 220 + rand() % 40 - 20;
        } else if (avgSoc < -30) {
            // Authoritarian: dark greens (120±25)
            hue = 120 + rand() % 50 - 25;
            sat = 60 + rand() % 25;     // more muted greens
            lig = 30 + rand() % 20;     // darker
        } else if (avgSoc > 30) {
            // Libertarian: golds/yellows (45±20)
            hue = 45 + rand() % 40 - 20;
            lig = 45 + rand() % 15;     // 45-60%
        } else if (avgEcon < -20) {
            // Center-left: warm reds/oranges (10±15)
            hue = 10 + rand() % 30 - 15;
        } else if (avgEcon > 20) {
            // Center-right: moderate blues (210±15)
            hue = 210 + rand() % 30 - 15;
        } else {
            // Centrist: purples or teals (random, muted)
            int ci = rand() % 3;
            if (ci == 0) { hue = 270 + rand() % 40 - 20; }     // purple
            else if (ci == 1) { hue = 180 + rand() % 40 - 20; } // teal
            else { hue = rand() % 360; }                         // any
            sat = 40 + rand() % 30;     // more muted
            lig = 35 + rand() % 25;     // wider range
        }

        // Convert HSL to RGB
        auto hslToRgb = [](int h, int s, int l) -> Color {
            float H = h / 360.0f;
            float S = s / 100.0f;
            float L = l / 100.0f;
            float C = (1.0f - fabsf(2.0f * L - 1.0f)) * S;
            float X = C * (1.0f - fabsf(fmodf(H * 6.0f, 2.0f) - 1.0f));
            float m = L - C / 2.0f;
            float r, g, b;
            int hi = (int)(H * 6.0f) % 6;
            switch (hi) {
                case 0: r=C; g=X; b=0; break;
                case 1: r=X; g=C; b=0; break;
                case 2: r=0; g=C; b=X; break;
                case 3: r=0; g=X; b=C; break;
                case 4: r=X; g=0; b=C; break;
                default: r=C; g=0; b=X; break;
            }
            return {(uint8_t)((r + m) * 255), (uint8_t)((g + m) * 255), (uint8_t)((b + m) * 255), 255};
        };

        c1 = hslToRgb(hue, sat, lig);

        // Secondary color: choose a harmonious counterpart
        // Options: complementary (hue+180), split-complementary, analogous, or neutral
        int c2choice = rand() % 100;
        if (c2choice < 30) {
            // Complementary: hue + 150..210
            int cHue = (hue + 150 + rand() % 60) % 360;
            c2 = hslToRgb(cHue, std::min(100, sat + 10), std::min(100, lig + 15));
        } else if (c2choice < 55) {
            // Analogous: hue ±20..40
            int offset = (rand() % 2 == 0) ? 20 + rand() % 20 : -(20 + rand() % 20);
            int cHue = (hue + offset + 360) % 360;
            c2 = hslToRgb(cHue, std::max(30, sat - 20), std::min(100, lig + 10));
        } else if (c2choice < 75) {
            // Triadic: hue + 110..130
            int cHue = (hue + 110 + rand() % 20) % 360;
            c2 = hslToRgb(cHue, sat, lig);
        } else if (c2choice < 90) {
            // White/off-white
            c2 = (rand() % 2 == 0) ? WHITE : Color{255, 255, 230, 255};
        } else {
            // Dark neutral (black, dark gray)
            int gv = 20 + rand() % 40;
            c2 = Color{(uint8_t)gv, (uint8_t)gv, (uint8_t)gv, 255};
        }
    }

    // ── Parent country color influence ──
    {
        auto& parentCountry = m_countries.getAll()[parentCid];
        auto& parentFlagColors = parentCountry.flagActual.colors;
        if (parentFlagColors.size() >= 1 && rand() % 100 < 60) {
            Color pc = parentFlagColors[0];
            float blend = 0.2f + (float)(rand() % 30) / 100.0f;
            c1 = {
                (uint8_t)(c1.r * (1.0f - blend) + pc.r * blend),
                (uint8_t)(c1.g * (1.0f - blend) + pc.g * blend),
                (uint8_t)(c1.b * (1.0f - blend) + pc.b * blend),
                255
            };
        }
        if (parentFlagColors.size() >= 2 && rand() % 100 < 40) {
            Color pc = parentFlagColors[1];
            float blend = 0.2f + (float)(rand() % 20) / 100.0f;
            c2 = {
                (uint8_t)(c2.r * (1.0f - blend) + pc.r * blend),
                (uint8_t)(c2.g * (1.0f - blend) + pc.g * blend),
                (uint8_t)(c2.b * (1.0f - blend) + pc.b * blend),
                255
            };
        }
    }

    FlagPattern flag;
    flag.censored = false;

    // Extreme radical check (only for hate symbol eligibility)
    bool extremeRadical = (avgEcon < -60 || avgEcon > 60 || avgSoc < -60 || avgSoc > 60);

    // Helper: pick a symbol color that contrasts with the background colors
    auto contrastColor = [&]() -> Color {
        int ar = 0, ag = 0, ab = 0, an = 0;
        auto addCol = [&](Color col) { ar += col.r; ag += col.g; ab += col.b; an++; };
        addCol(c1); addCol(c2);
        if (an == 0) return WHITE;
        ar /= an; ag /= an; ab /= an;
        float bright = (ar * 0.299f + ag * 0.587f + ab * 0.114f) / 255.0f;
        if (bright > 0.55f) {
            int ci = rand() % 3;
            if (ci == 0) return Color{20, 20, 20, 255};
            else if (ci == 1) return Color{180, 30, 30, 255};
            else return Color{20, 40, 120, 255};
        } else {
            int ci = rand() % 3;
            if (ci == 0) return WHITE;
            else if (ci == 1) return Color{255, 230, 100, 255};
            else return Color{220, 220, 220, 255};
        }
    };

    struct FlagPatternChoice { FlagType ft; float weight; };
    auto pickPattern = [&](const std::vector<FlagPatternChoice>& choices) -> FlagType {
        float total = 0;
        for (auto& c : choices) total += c.weight;
        float roll = (float)rand() / (float)RAND_MAX * total;
        float accum = 0;
        for (auto& c : choices) {
            accum += c.weight;
            if (roll <= accum) return c.ft;
        }
        return choices.back().ft;
    };

    std::vector<Color> flagColors;
    FlagSymbol symbol;
    symbol.type = SymbolType::NONE;
    symbol.x = 0.5f;
    symbol.y = 0.5f;

    // Helper: pick SVG symbol path by name
    auto pickSVG = [&](const char* name) {
        symbol.text = std::string("symbols/") + name;
        symbol.type = SymbolType::SVG_FILE;
    };
    Color symColor = contrastColor();

    // Helper: vary symbol placement based on flag pattern
    auto placeSymbol = [&](FlagType ft) {
        if (ft == FlagType::CANTON) {
            symbol.x = 0.20f;
            symbol.y = 0.20f;
        } else if (ft == FlagType::TRIANGLE || ft == FlagType::TRIANGLE_DOUBLE) {
            symbol.x = 0.35f;
            symbol.y = 0.5f;
        } else if (ft == FlagType::PALE) {
            symbol.x = 0.5f;
            symbol.y = 0.5f;
        } else if (ft == FlagType::FESS) {
            symbol.x = 0.5f;
            symbol.y = 0.5f;
        } else if (ft == FlagType::CROSS_NORDIC || ft == FlagType::CROSS_GREEK) {
            symbol.x = 0.5f;
            symbol.y = 0.5f;
        } else if (ft == FlagType::SUNBURST) {
            symbol.x = 0.5f;
            symbol.y = 0.5f;
        } else {
            symbol.x = 0.5f;
            symbol.y = 0.5f;
        }
    };

    // ── FAR LEFT (avgEcon < -30) ──
    if (avgEcon < -30) {
        FlagType ft = pickPattern({
            {FlagType::HSTRIPES_3, 4.0f},
            {FlagType::HSTRIPES_2, 2.0f},
            {FlagType::VSTRIPES_3, 1.0f},
            {FlagType::TRIANGLE, 1.5f},
            {FlagType::SOLID, 1.0f},
        });
        flag.type = ft;
        if (ft == FlagType::HSTRIPES_3) flagColors = {c1, c2, c1};
        else if (ft == FlagType::HSTRIPES_2) flagColors = {c1, c2};
        else if (ft == FlagType::VSTRIPES_3) flagColors = {c1, c2, c1};
        else if (ft == FlagType::TRIANGLE) flagColors = {c2, c1};
        else flagColors = {c1};

        placeSymbol(ft);
        int symRoll = rand() % 100;
        if (extremeRadical && symRoll < 3) {  // <--- MUCH rarer: 3% instead of 50%
            pickSVG("hammer_sickle.svg");
            symbol.size = 0.35f;
        } else if (symRoll < 30) {
            pickSVG("star5.svg");
            symbol.size = 0.35f;
        } else if (symRoll < 55) {
            pickSVG("gear.svg");
            symbol.size = 0.32f;
        } else if (symRoll < 75) {
            pickSVG("rose.svg");
            symbol.size = 0.32f;
        } else if (symRoll < 88) {
            pickSVG("torch.svg");
            symbol.size = 0.38f;
        } else if (symRoll < 95) {
            pickSVG("crossed_swords.svg");
            symbol.size = 0.30f;
        } else if (symRoll < 98) {
            pickSVG("torch.svg");
            symbol.size = 0.28f;
        } else {
            pickSVG("rose.svg");
            symbol.size = 0.30f;
        }
// ── FAR RIGHT (avgEcon > 30) ──
    } else if (avgEcon > 30) {
        FlagType ft = pickPattern({
            {FlagType::HSTRIPES_3, 3.0f},
            {FlagType::FESS, 2.0f},
            {FlagType::CANTON, 1.5f},
            {FlagType::QUARTERED, 1.0f},
            {FlagType::SOLID, 1.0f},
            {FlagType::SALTIR, 1.0f},
        });
        flag.type = ft;
        if (ft == FlagType::HSTRIPES_3) flagColors = {c1, c2, c1};
        else if (ft == FlagType::FESS) flagColors = {c1, c2, c1};
        else if (ft == FlagType::CANTON) flagColors = {c2, c1};
        else if (ft == FlagType::QUARTERED) flagColors = {c1, c2, c2, c1};
        else if (ft == FlagType::SALTIR) flagColors = {c1, c2, c2, c1};
        else flagColors = {c1};

        placeSymbol(ft);
        int symRoll = rand() % 100;
        if (extremeRadical && symRoll < 3) {  // <--- MUCH rarer: 3% instead of 30%
            pickSVG("swastika.svg");
            symbol.size = 0.35f;
        } else if (symRoll < 30) {
            pickSVG("cross_latin.svg");
            symbol.size = 0.38f;
        } else if (symRoll < 55) {
            pickSVG(rand() % 2 == 0 ? "eagle.svg" : "star5.svg");
            symbol.size = 0.35f;
        } else if (symRoll < 75) {
            pickSVG("fasces.svg");
            symbol.size = 0.32f;
        } else if (symRoll < 88) {
                        pickSVG("cross_saltir.svg");
            symbol.size = 0.30f;
        } else if (symRoll < 96) {
            pickSVG("cross_pattee.svg");
            symbol.size = 0.32f;
        } else {
            pickSVG("torch.svg");
            symbol.size = 0.28f;
        }
// ── LIBERTARIAN (avgSoc > 25) ──
    } else if (avgSoc > 25) {
        flag.type = pickPattern({
            {FlagType::HSTRIPES_3, 3.0f},
            {FlagType::VSTRIPES_3, 2.0f},
            {FlagType::PALE, 2.0f},
            {FlagType::QUARTERED, 1.0f},
            {FlagType::SOLID, 1.0f},
            {FlagType::SUNBURST, 1.5f},
        });
        if (flag.type == FlagType::HSTRIPES_3) flagColors = {c2, c1, c2};
        else if (flag.type == FlagType::VSTRIPES_3) flagColors = {c2, c1, c2};
        else if (flag.type == FlagType::PALE) flagColors = {c2, c1, c2};
        else if (flag.type == FlagType::QUARTERED) flagColors = {c1, c2, c2, c1};
        else if (flag.type == FlagType::SUNBURST) flagColors = {c1, c2};
        else flagColors = {c1};

        placeSymbol(flag.type);
        int symRoll = rand() % 100;
        if (symRoll < 30) {
            pickSVG(rand() % 2 == 0 ? "sun.svg" : "sun_wavy.svg");
            symbol.size = 0.38f;
        } else if (symRoll < 55) {
            pickSVG("torch.svg");
            symbol.size = 0.38f;
        } else if (symRoll < 70) {
            pickSVG("crescent.svg");
            symbol.size = 0.32f;
        } else if (symRoll < 82) {
            pickSVG(rand() % 2 == 0 ? "star5.svg" : "diamond.svg");
            symbol.size = 0.32f;
        } else if (symRoll < 92) {
                        pickSVG("anchor.svg");
            symbol.size = 0.30f;
        } else if (symRoll < 94) {
            pickSVG("gear.svg");
            symbol.size = 0.28f;
        } else {
            pickSVG("star_4.svg");
            symbol.size = 0.28f;
        }
// ── CENTRIST (other) ──
    } else {
        FlagType ft = pickPattern({
            {FlagType::HSTRIPES_3, 3.0f},
            {FlagType::FESS, 1.5f},
            {FlagType::PALE, 1.5f},
            {FlagType::DIAGONAL_L, 1.0f},
            {FlagType::TRIANGLE, 1.0f},
            {FlagType::SOLID, 2.0f},
            {FlagType::SALTIR, 1.0f},
        });
        flag.type = ft;
        if (ft == FlagType::HSTRIPES_3 || ft == FlagType::FESS) flagColors = {c1, c2, c1};
        else if (ft == FlagType::PALE) flagColors = {c2, c1, c2};
        else if (ft == FlagType::DIAGONAL_L) flagColors = {c1, c2};
        else if (ft == FlagType::TRIANGLE) flagColors = {c2, c1};
        else if (ft == FlagType::SALTIR) flagColors = {c1, c2, c2, c1};
        else flagColors = {c1};

        placeSymbol(ft);
        int symRoll = rand() % 100;
        if (symRoll < 25) {
            pickSVG("star5.svg");
            symbol.size = 0.32f;
        } else if (symRoll < 45) {
            pickSVG("cross_latin.svg");
            symbol.size = 0.32f;
        } else if (symRoll < 60) {
            pickSVG("crescent.svg");
            symbol.size = 0.32f;
        } else if (symRoll < 75) {
            pickSVG(rand() % 2 == 0 ? "diamond.svg" : "circle_stars.svg");
            symbol.size = 0.28f;
        } else if (symRoll < 88) {
            pickSVG("tree.svg");
            symbol.size = 0.30f;
        } else if (symRoll < 94) {
            pickSVG("gear.svg");
            symbol.size = 0.28f;
        } else {
            pickSVG("star_4.svg");
            symbol.size = 0.28f;
        }
    }
// Apply symbol color
    if (symbol.type == SymbolType::SVG_FILE) {
        symbol.colors = {symColor};
    }

    // Safety: if extreme radical and no symbol was selected, force a generic symbol (not hate)
    if (symbol.type == SymbolType::NONE) {
        pickSVG("star5.svg");
        symbol.colors = {contrastColor()};
        symbol.size = 0.35f;
    }

    flag.colors = flagColors;
    if (symbol.type != SymbolType::NONE) flag.symbols = {symbol};

    // Build censored version
    FlagPattern flagCensored = flag;
    bool hasHate = false;
    for (auto& sym : flagCensored.symbols) {
        if (sym.type == SymbolType::SVG_FILE &&
            (sym.text.find("swastika") != std::string::npos ||
             sym.text.find("hammer_sickle") != std::string::npos)) {
            hasHate = true;
            sym.type = SymbolType::CENSOR_BAR;
            sym.colors = {BLANK};
        }
    }
    flagCensored.censored = hasHate;

    Country rebel;
    rebel.id = rebelCid;
    rebel.name = countryName;
    rebel.isoA3 = isoA3;
    rebel.color = c1;
    rebel.flagActual = flag;
    rebel.flagCensored = flagCensored;
    rebel.treasury = 0;

    // Generate SVG for this rebel flag and store for save persistence
    std::string rebelSvg = flagPatternToSvg(flag, 200, 133, &m_odmJsonData);
    m_rebelFlagSvgs[rebelCid] = rebelSvg;

    m_countries.getAll()[rebelCid] = rebel;
    if (!rebel.isoA3.empty()) m_isoToCid[rebel.isoA3] = rebelCid;
    m_countryCompass[rebelCid] = {avgEcon, avgSoc};

    if (rebelCid >= (int)m_countryPixels.size())
        m_countryPixels.resize(rebelCid + 1);
    if (rebelCid >= (int)m_countryRelationColors.size())
        m_countryRelationColors.resize(rebelCid + 1, Color{80, 80, 80, 255});

    Texture2D tex = FlagRenderer::render(rebel.flagActual, 256, 128, "", &m_odmJsonData);
    m_countryFlags[rebelCid] = tex;

    if (m_config.aiDebug)
        printf("[REBELLION] Created '%s' (CID=%d, ISO=%s, %zu provinces, %lld pop, econ=%.1f soc=%.1f)\n",
           countryName.c_str(), rebelCid, isoA3.c_str(), provinceIds.size(), totalPop, avgEcon, avgSoc);

    // Declare war (bidirectional)
    auto& parentIso = m_countries.getAll()[parentCid].isoA3;
    m_relations[isoA3][parentIso].war = true;
    m_relations[parentIso][isoA3].war = true;
    if (m_config.aiDebug)
        printf("[REBELLION] War: %s vs %s\n", isoA3.c_str(), parentIso.c_str());

    // Notify player if this affects them
    if (parentCid == m_playerCountryId) {
        std::string parentName = m_countries.getAll()[parentCid].name;
        pushPopup(PopupType::REBELLION,
            "Breakaway State!",
            countryName + " has declared independence!\nWar: " + parentName + " vs " + countryName,
            rebelCid);
    }

    // Transfer province ownership + populate m_countryPixels for population view
    std::unordered_set<int> movedPixels; // everything leaving the parent
    for (int pid : provinceIds) {
        Province* pp = m_provinces.getProvinceById(pid);
        if (pp) {
            pp->countryId = rebelCid;
            if ((size_t)pid < m_provinceCountryLookup.size())
                m_provinceCountryLookup[pid] = rebelCid;
        }
        auto ppIt = m_provincePixels.find(pid);
        if (ppIt != m_provincePixels.end()) {
            for (int idx : ppIt->second) {
                if (idx >= 0 && idx < (int)m_pixelCountryArray.size())
                    m_pixelCountryArray[idx] = rebelCid;
                // Populate rebel's m_countryPixels
                if (rebelCid >= 0 && rebelCid < (int)m_countryPixels.size())
                    m_countryPixels[rebelCid].push_back(idx);
                movedPixels.insert(idx);
            }
        }
    }
    // Remove the moved pixels from the parent in ONE pass. The old per-pixel
    // std::find + erase over the parent's whole pixel vector was
    // O(rebelPixels x parentPixels) — for a large country that's billions of
    // element visits per rebellion, and the single biggest reason turns with
    // several rebellions took so long.
    if (!movedPixels.empty() && parentCid >= 0 && parentCid < (int)m_countryPixels.size()) {
        auto& parentPixels = m_countryPixels[parentCid];
        parentPixels.erase(std::remove_if(parentPixels.begin(), parentPixels.end(),
                                          [&](int idx) { return movedPixels.count(idx) != 0; }),
                           parentPixels.end());
    }

    // Labels are rebuilt ONCE per turn (m_labelsDirty, consumed in
    // processTurn) instead of per rebellion — computeCountryLabels does a full
    // 8192x4096 raster scan, so ten rebellions used to mean ten full scans.
    m_labelsDirty = true;
}

// === processRebellions ===
void Game::processRebellions(int countryId) {
    if (countryId <= 0 || countryId == SPC_CID || countryId == UNC_CID || countryId == BLC_CID) return;

    // Breakaway states must not immediately re-fracture. They own low-alignment
    // provinces that the parent auto-claims and declares war on, which keeps
    // rebellion chance high — so rebel countries processing rebellions produced
    // a runaway (dozens of new one-province rebel states every turn, each of
    // which then rebelled again). A newly formed rebel is simply skipped here.
    if (countryId >= REBEL_CID_MIN) return;

    int totalProvCount = 0;
    std::vector<int> rebellingProvs;
    for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
        if (prov.countryId != countryId) continue;
        totalProvCount++;
        float chance = getProvinceRebellionChance(pid, countryId);
        float roll = (float)rand() / (float)RAND_MAX * 100.0f;
        if (roll < chance) rebellingProvs.push_back(pid);
    }
    // Single-province countries can't have rebellions
    if (totalProvCount <= 1) {
        if (m_config.aiDebug)
            printf("[REBELLION] cid=%d only %d province(s), skipping\n", countryId, totalProvCount);
        return;
    }
    if (rebellingProvs.empty()) return;

    if (m_config.aiDebug)
        printf("[REBELLION] cid=%d has %zu rebelling provinces (of %d total)\n", countryId, rebellingProvs.size(), totalProvCount);

    // Precompute BFS distances between rebelling provinces (capped at 6 steps)
    auto graphDist = [&](int a, int b, int maxD) -> int {
        if (a == b) return 0;
        std::queue<int> q;
        std::unordered_map<int,int> dmap;
        q.push(a); dmap[a] = 0;
        while (!q.empty()) {
            int cur = q.front(); q.pop();
            int d = dmap[cur];
            if (d >= maxD) continue;
            auto nit = m_provinceNeighbors.find(cur);
            if (nit == m_provinceNeighbors.end()) continue;
            for (int n : nit->second) {
                if (dmap.find(n) != dmap.end()) continue;
                if (n == b) return d + 1;
                dmap[n] = d + 1;
                q.push(n);
            }
        }
        return maxD + 1;
    };

    // Group by ideology (±20) AND geographic proximity (≤4 BFS steps)
    std::vector<std::vector<int>> factions;
    std::vector<bool> assigned(rebellingProvs.size(), false);
    for (size_t i = 0; i < rebellingProvs.size(); i++) {
        if (assigned[i]) continue;
        std::vector<int> faction = {rebellingProvs[i]};
        assigned[i] = true;
        Vector2 baseComp = {0, 0};
        auto cit = m_provinceCompass.find(rebellingProvs[i]);
        if (cit != m_provinceCompass.end()) baseComp = cit->second;
        for (size_t j = i + 1; j < rebellingProvs.size(); j++) {
            if (assigned[j]) continue;
            auto cjt = m_provinceCompass.find(rebellingProvs[j]);
            Vector2 comp = (cjt != m_provinceCompass.end()) ? cjt->second : Vector2{0,0};
            float ideoDist = sqrtf((comp.x - baseComp.x) * (comp.x - baseComp.x) +
                                   (comp.y - baseComp.y) * (comp.y - baseComp.y));
            if (ideoDist <= 20.0f) {
                int gd = graphDist(rebellingProvs[i], rebellingProvs[j], 5);
                if (gd <= 4) {
                    faction.push_back(rebellingProvs[j]);
                    assigned[j] = true;
                }
            }
        }
        factions.push_back(faction);
    }

    if (m_config.aiDebug)
        printf("[REBELLION] cid=%d => %zu faction(s)\n", countryId, factions.size());

    for (auto& faction : factions) {
        // Damage parent armies in rebelling provinces
        for (int pid : faction) {
            auto ait = m_provinceArmies.find(pid);
            if (ait == m_provinceArmies.end()) continue;
            auto& units = ait->second;
            for (auto it = units.begin(); it != units.end(); ) {
                if (it->countryId == countryId) {
                    float killPct = 0.3f + (float)rand() / (float)RAND_MAX * 0.4f;
                    int killed = (int)(it->count * killPct);
                    it->count -= killed;
                    if (m_config.aiDebug)
                        printf("[REBELLION] cid=%d lost %d/%d troops in prov %d\n",
                           countryId, killed, killed + it->count, pid);
                    if (it->count <= 0) { it = units.erase(it); continue; }
                }
                ++it;
            }
        }

        int rebelCid = allocateRebelCid();
        createRebelCountry(rebelCid, countryId, faction);
        m_rebellionsThisTurnByCid[countryId]++;

        // The survivors of the uprising are now standing inside a country that
        // did not exist a moment ago and that their owner is automatically at
        // war with. Nothing in the engine retreats, captures or attrits troops
        // in hostile territory, so left alone they sat there forever — inert,
        // still drawing upkeep, and blocking the rebel from holding its own
        // ground. Pull them back to an adjacent province the parent still
        // holds; if the province is fully enclosed by the revolt, they are
        // overrun and disband. (Same intent as the ceasefire cession cleanup.)
        for (int pid : faction) {
            auto ait = m_provinceArmies.find(pid);
            if (ait == m_provinceArmies.end()) continue;
            auto& units = ait->second;
            int retreating = 0;
            for (auto it = units.begin(); it != units.end(); ) {
                if (it->countryId == countryId) {
                    retreating += it->count;
                    it = units.erase(it);
                } else ++it;
            }
            if (units.empty()) m_provinceArmies.erase(pid);
            if (retreating <= 0) continue;

            int dest = -1;
            auto nIt = m_provinceNeighbors.find(pid);
            if (nIt != m_provinceNeighbors.end())
                for (int nid : nIt->second)
                    if (nid >= 0 && nid < (int)m_provinceCountryLookup.size() &&
                        m_provinceCountryLookup[nid] == countryId) { dest = nid; break; }

            if (dest >= 0) {
                auto& dstUnits = m_provinceArmies[dest];
                bool merged = false;
                for (auto& u : dstUnits)
                    if (u.countryId == countryId) { u.count += retreating; merged = true; break; }
                if (!merged) dstUnits.push_back({countryId, retreating});
            }
            if (m_config.aiDebug)
                printf("[REBELLION] cid=%d %s %d troops from prov %d\n", countryId,
                       dest >= 0 ? "retreated" : "lost (surrounded)", retreating, pid);
        }

        // Reduce population (1% casualties from uprising)
        for (int pid : faction) {
            auto popIt = m_provincePopulations.find(pid);
            if (popIt != m_provincePopulations.end() && popIt->second > 0) {
                long long lost = std::max(1LL, (long long)(popIt->second * 0.01f));
                popIt->second -= lost;
                if (popIt->second < 0) popIt->second = 0;
            }
        }

        // Parent country automatically claims all rebel provinces
        std::string parentIso = m_countries.getAll()[countryId].isoA3;
        if (!parentIso.empty()) {
            for (int pid : faction) {
                auto& cl = m_claims[parentIso];
                if (std::find(cl.begin(), cl.end(), pid) == cl.end()) {
                    cl.push_back(pid);
                    // Keep the reverse index in sync — it used to be skipped,
                    // so rebellion claims produced no unrest until a save
                    // reload rebuilt m_claimsByProvince from m_claims.
                    auto& rev = m_claimsByProvince[pid];
                    if (std::find(rev.begin(), rev.end(), parentIso) == rev.end())
                        rev.push_back(parentIso);
                    printf("[CLAIM] %s -> province %d (rebellion claim)\n", parentIso.c_str(), pid);
                }
            }
        }

        // Parent country automatically wages war on the rebel
        std::string rebelIso = m_countries.getAll()[rebelCid].isoA3;
        if (!parentIso.empty() && !rebelIso.empty()) {
            m_relations[parentIso][rebelIso].war = true;
            m_relations[rebelIso][parentIso].war = true;
            printf("[WAR] %s vs %s (rebellion war)\n", parentIso.c_str(), rebelIso.c_str());
        }
    }
}

// === processEconomy ===
void Game::processEconomy(int countryId) {
    auto cs = computeCountryIncome(countryId);
    auto& treasury = m_countries.getAll()[countryId].treasury;
    float net = cs.total - cs.expenses;
    treasury += net;
    if (treasury < 0) treasury = 0;
}

// === processShipBombardOrders ===
void Game::processShipBombardOrders(int countryId) {
    if (m_pendingShipBombardOrders.empty()) return; // common case: nothing to do
    if (m_config.aiDebug)
        printf("[SHIPBOMBARD] entered for cid=%d, pending orders: %zu\n", countryId, m_pendingShipBombardOrders.size());
    struct ArtyEffect { float troopKillPct; float popKillPct; float fortDmg; int indDmg; float fortChance; };
    auto getEffect = [&](const std::string& type) -> ArtyEffect {
        for (auto& n : m_researchNodes)
            if (n.artilleryType == type)
                return {n.artilleryTroopKillPct, n.artilleryPopKillPct, n.artilleryFortDamage, n.artilleryIndustryDamage, n.artilleryFortDamageChance};
        return {0,0,0,0,0};
    };
    struct ArtyNodeLookup { const char* type; const char* nodeId; };
    static const ArtyNodeLookup ARTY_NODES[] = {
        {"mortar","arty1"},{"light","arty2"},{"heavy","arty3"},
        {"napalm","arty4a"},{"carpet","arty4b"},{"chemical","arty5"},
        {"nuclear","arty6a"},{"biological","arty6b"},{nullptr,nullptr}
    };
    auto ammoToNode = [&](const std::string& t) -> std::string {
        for (int ai = 0; ARTY_NODES[ai].type; ++ai)
            if (t == ARTY_NODES[ai].type) return ARTY_NODES[ai].nodeId;
        return "";
    };
    for (size_t i = 0; i < m_pendingShipBombardOrders.size(); ) {
        auto& bo = m_pendingShipBombardOrders[i];
        if (bo.shipIndex < 0 || bo.shipIndex >= (int)m_ships.size()) { printf("  [SHIPBOMBARD] skip: bad shipIndex %d/%zu\n", bo.shipIndex, m_ships.size()); ++i; continue; }
        auto& ship = m_ships[bo.shipIndex];
        if (ship.countryId != countryId) { ++i; continue; }
        // Verify the ammo type is researched
        std::string reqNode = ammoToNode(bo.ammoType);
        if (!reqNode.empty() && !hasResearched(reqNode, countryId)) { printf("  [SHIPBOMBARD] skip: %s not researched for cid %d\n", bo.ammoType.c_str(), countryId); ++i; continue; }
        // Carriers can use any researched ammo type
        Province* tgt = m_provinces.getProvinceById(bo.targetProvince);
        if (!tgt) { printf("  [SHIPBOMBARD] skip: no target province %d\n", bo.targetProvince); ++i; continue; }
        ArtyEffect eff = getEffect(bo.ammoType);
        printf("  [SHIPBOMBARD] cid=%d ship=%d type=%s ammo=%s tgt=%d eff=(t=%.0f,p=%.0f,f=%.0f,i=%d,fc=%.0f)\n",
            countryId, bo.shipIndex, ship.type.c_str(), bo.ammoType.c_str(), bo.targetProvince,
            eff.troopKillPct, eff.popKillPct, eff.fortDmg, eff.indDmg, eff.fortChance);
        // Kill troops
        auto aIt = m_provinceArmies.find(bo.targetProvince);
        if (aIt != m_provinceArmies.end() && eff.troopKillPct > 0) {
            for (auto& u : aIt->second) {
                int killed = (int)(u.count * eff.troopKillPct / 100.0f);
                printf("    troop kill: cid=%d count=%d killed=%d\n", u.countryId, u.count, killed);
                u.count = std::max(0, u.count - killed);
            }
        }
        // Kill population
        if (eff.popKillPct > 0) {
            auto pIt = m_provincePopulations.find(bo.targetProvince);
            if (pIt != m_provincePopulations.end()) {
                long long killed = (long long)(pIt->second * eff.popKillPct / 100.0f);
                printf("    pop kill: prev=%lld killed=%lld\n", pIt->second, killed);
                pIt->second = std::max(0LL, pIt->second - killed);
            }
        }
        // Damage fortifications
        if (eff.fortDmg > 0 || eff.fortChance > 0) {
            auto indIt = m_provinceIndustry.find(bo.targetProvince);
            if (indIt != m_provinceIndustry.end()) {
                float dmg = eff.fortDmg;
                if (eff.fortChance > 0 && (rand() % 100) < eff.fortChance) dmg += 1;
                int prev = indIt->second.fortification;
                indIt->second.fortification = std::max(0, indIt->second.fortification - (int)dmg);
                printf("    fort dmg: prev=%d dmg=%.0f new=%d\n", prev, dmg, indIt->second.fortification);
            }
        }
        // Damage industry
        if (eff.indDmg > 0) {
            auto indIt = m_provinceIndustry.find(bo.targetProvince);
            if (indIt != m_provinceIndustry.end()) {
                int prevLvl = indIt->second.level;
                float prevInc = indIt->second.income;
                indIt->second.level = std::max(0, indIt->second.level - eff.indDmg);
                indIt->second.income *= 0.5f;
                printf("    ind dmg: prev_lvl=%d prev_inc=%.1f new_lvl=%d new_inc=%.1f\n", prevLvl, prevInc, indIt->second.level, indIt->second.income);
            }
        }
        printf("  [SHIPBOMBARD] done\n");
        m_pendingShipBombardOrders.erase(m_pendingShipBombardOrders.begin() + i);
    }
}

// === processShipDisembarks ===
void Game::processShipDisembarks(int countryId) {
    auto areAllied = [&](int a, int b) -> bool {
        if (a <= 0 || b <= 0 || a == b) return false;
        const Country* ac = m_countries.getCountry(a);
        const Country* bc = m_countries.getCountry(b);
        if (!ac || !bc) return false;
        auto ar = m_relations.find(ac->isoA3);
        if (ar == m_relations.end()) return false;
        auto dr = ar->second.find(bc->isoA3);
        return dr != ar->second.end() && dr->second.alliance;
    };
    // Transfer conquered pixels to the new owner's countryPixels
    auto transferCountryPixels = [&](int pid, int newOwner, int oldOwner) {
        if (m_countryPixels.empty()) return;
        auto ppIt = m_provincePixels.find(pid);
        if (ppIt == m_provincePixels.end()) return;
        const auto& provincePixels = ppIt->second;
        for (int idx : provincePixels) {
            if (idx >= 0 && idx < (int)m_pixelCountryArray.size())
                m_pixelCountryArray[idx] = newOwner;
        }
        // m_countryPixels only feeds texture generation (political, relations,
        // population, claims overlays) — never game logic. Maintaining it costs a
        // full scan of the owning country's pixel list on every province
        // capture, which profiled at ~39% of self-play runtime. Headless
        // training draws its minimap from m_provinceCountryLookup, so the list
        // is pure overhead there.
        if (m_aiTraining) return;
        // Move pixels from old owner to new owner in m_countryPixels (O(N+M), not O(N*M))
        if (oldOwner >= 0 && (size_t)oldOwner < m_countryPixels.size() &&
            newOwner >= 0 && (size_t)newOwner < m_countryPixels.size()) {
            auto& oldPx = m_countryPixels[oldOwner];
            auto& newPx = m_countryPixels[newOwner];
            std::unordered_set<int> pxSet(provincePixels.begin(), provincePixels.end());
            std::vector<int> transferred;
            transferred.reserve(provincePixels.size());
            auto newEnd = std::remove_if(oldPx.begin(), oldPx.end(), [&](int idx) {
                if (pxSet.count(idx)) {
                    transferred.push_back(idx);
                    return true;
                }
                return false;
            });
            oldPx.erase(newEnd, oldPx.end());
            newPx.insert(newPx.end(), transferred.begin(), transferred.end());
        }
    };

    for (size_t i = 0; i < m_pendingShipDisembarks.size(); ) {
        auto& do_ = m_pendingShipDisembarks[i];
        if (do_.shipIndex < 0 || do_.shipIndex >= (int)m_ships.size()) { ++i; continue; }
        auto& ship = m_ships[do_.shipIndex];
        if (ship.countryId != countryId) { ++i; continue; }
        int pid = do_.targetProvince;
        Province* dst = m_provinces.getProvinceById(pid);
        if (!dst) { ++i; continue; }
        int crew = ship.crew * 100; // Scale to army internal units (100x)
        ship.crew = 0;
        if (crew <= 0) { m_pendingShipDisembarks.erase(m_pendingShipDisembarks.begin() + i); continue; }
        // Apply fortification defense at destination
        auto indIt = m_provinceIndustry.find(pid);
        float fortDef = 0;
        if (indIt != m_provinceIndustry.end()) fortDef = indIt->second.fortification * 10.0f;
        auto& dstArmies = m_provinceArmies[pid];
        auto eIt = std::find_if(dstArmies.begin(), dstArmies.end(),
            [&](auto& u) { return u.countryId != countryId && u.countryId > 0 && !areAllied(countryId, u.countryId); });
        if (eIt != dstArmies.end()) {
            // Combat: attackers (crew) vs defenders (enemy troops + fort)
            float atkMod = 1.0f + getTotalEffect("armyAtkPct") / 100.0f;
            int atkPower = (int)(crew * atkMod);
            int defPower = (int)(eIt->count * (1.0f + fortDef / 100.0f));
            if (atkPower > defPower) {
                int remaining = atkPower - defPower;
                eIt->count = 0;
                // Take over province
                int prevOwner = dst->countryId;
                dst->countryId = countryId;
                if (dst->id > 0 && (size_t)dst->id < m_provinceCountryLookup.size())
                    m_provinceCountryLookup[dst->id] = countryId;
                transferCountryPixels(dst->id, countryId, prevOwner);
                // Conquered province: add negative alignment drift for its minorities
                {
                    auto minIt = m_provinceMinorities.find(pid);
                    if (minIt != m_provinceMinorities.end())
                        for (auto& mg : minIt->second)
                            m_minorityAlignmentDrift[mg.name] -= 25.0f;
                }
                // Auto-claim: previous owner claims this province (skip UNC/BLC)
                if (prevOwner > 0 && prevOwner != UNC_CID && prevOwner != BLC_CID && prevOwner != countryId) {
                    const Country* prevC = m_countries.getCountry(prevOwner);
                    if (prevC && dst->id > 0) {
                        auto& claimList = m_claims[prevC->isoA3];
                        if (std::find(claimList.begin(), claimList.end(), dst->id) == claimList.end()) {
                            claimList.push_back(dst->id);
                            m_claimsByProvince[dst->id].push_back(prevC->isoA3);
                        }
                    }
                }
                // Remove claim if conqueror claimed this province
                {
                    const Country* conqueror = m_countries.getCountry(countryId);
                    if (conqueror) {
                        auto& cl = m_claims[conqueror->isoA3];
                        auto cp = std::find(cl.begin(), cl.end(), dst->id);
                        if (cp != cl.end()) {
                            cl.erase(cp);
                            auto& bp = m_claimsByProvince[dst->id];
                            bp.erase(std::remove(bp.begin(), bp.end(), conqueror->isoA3), bp.end());
                            if (bp.empty()) m_claimsByProvince.erase(dst->id);
                        }
                    }
                }
                // Place remaining troops (survivors keep same ratio)
                int survivingTroops = (atkMod > 0) ? (int)(remaining / atkMod) : crew;
                auto myIt = std::find_if(dstArmies.begin(), dstArmies.end(),
                    [&](auto& u) { return u.countryId == countryId; });
                if (myIt != dstArmies.end()) myIt->count += survivingTroops;
                else { ArmyUnit nu; nu.countryId = countryId; nu.count = survivingTroops; dstArmies.push_back(nu); }
            } else {
                // Attacker loses — some attackers survive ordeal
                int atkSurvivors = (int)(crew * 0.1f * (1.0f - fortDef / 200.0f));
                if (atkSurvivors < 1) atkSurvivors = 1;
                eIt->count -= (int)(crew * atkMod * (1.0f - fortDef / 200.0f));
                if (eIt->count < 0) eIt->count = 0;
                auto myIt = std::find_if(dstArmies.begin(), dstArmies.end(),
                    [&](auto& u) { return u.countryId == countryId; });
                if (myIt != dstArmies.end()) myIt->count += atkSurvivors;
                else { ArmyUnit nu; nu.countryId = countryId; nu.count = atkSurvivors; dstArmies.push_back(nu); }
            }
            dstArmies.erase(std::remove_if(dstArmies.begin(), dstArmies.end(),
                [](auto& u) { return u.count <= 0; }), dstArmies.end());
            if (dstArmies.empty()) m_provinceArmies.erase(pid);
        } else {
            // No enemy (only allies or empty): disembark and take over empty/enemy province
            int prevOwner = dst->countryId;
            if (prevOwner > 0 && prevOwner != BLC_CID && prevOwner != countryId) {
                // Take over the province (includes unclaimed — colonize it)
                dst->countryId = countryId;
                if (dst->id > 0 && (size_t)dst->id < m_provinceCountryLookup.size())
                    m_provinceCountryLookup[dst->id] = countryId;
                transferCountryPixels(dst->id, countryId, prevOwner);
                // Conquered province: add negative alignment drift for its minorities
                {
                    auto minIt = m_provinceMinorities.find(pid);
                    if (minIt != m_provinceMinorities.end())
                        for (auto& mg : minIt->second)
                            m_minorityAlignmentDrift[mg.name] -= 25.0f;
                }
                // Auto-claim: previous owner claims this province (skip UNC/BLC)
                if (prevOwner > 0 && prevOwner != UNC_CID && prevOwner != BLC_CID && prevOwner != countryId) {
                    const Country* prevC = m_countries.getCountry(prevOwner);
                    if (prevC && dst->id > 0) {
                        auto& claimList = m_claims[prevC->isoA3];
                        if (std::find(claimList.begin(), claimList.end(), dst->id) == claimList.end()) {
                            claimList.push_back(dst->id);
                            m_claimsByProvince[dst->id].push_back(prevC->isoA3);
                        }
                    }
                }
                // Remove claim if conqueror claimed this province
                {
                    const Country* conqueror = m_countries.getCountry(countryId);
                    if (conqueror) {
                        auto& cl = m_claims[conqueror->isoA3];
                        auto cp = std::find(cl.begin(), cl.end(), dst->id);
                        if (cp != cl.end()) {
                            cl.erase(cp);
                            auto& bp = m_claimsByProvince[dst->id];
                            bp.erase(std::remove(bp.begin(), bp.end(), conqueror->isoA3), bp.end());
                            if (bp.empty()) m_claimsByProvince.erase(dst->id);
                        }
                    }
                }
            }
            auto myIt = std::find_if(dstArmies.begin(), dstArmies.end(),
                [&](auto& u) { return u.countryId == countryId; });
            if (myIt != dstArmies.end()) myIt->count += crew;
            else { ArmyUnit nu; nu.countryId = countryId; nu.count = crew; dstArmies.push_back(nu); }
        }
        // Remove disembark order; delete the boat immediately
        int shipIdx = do_.shipIndex;
        m_pendingShipDisembarks.erase(m_pendingShipDisembarks.begin() + i);
        if (shipIdx >= 0 && shipIdx < (int)m_ships.size()) {
            m_ships.erase(m_ships.begin() + shipIdx);
            // Shift pending order indices to account for removal
            auto shiftIdx = [&](int& idx) { if (idx > shipIdx) idx--; };
            for (auto& mo : m_pendingShipMoveOrders) shiftIdx(mo.shipIndex);
            // Engage orders reference TWO ships; forgetting targetIndex here
            // skewed every queued engagement one ship over after a disembark.
            for (auto& eo : m_pendingShipEngageOrders) { shiftIdx(eo.shipIndex); shiftIdx(eo.targetIndex); }
            for (auto& bo : m_pendingShipBombardOrders) shiftIdx(bo.shipIndex);
            for (auto& d : m_pendingShipDisembarks) shiftIdx(d.shipIndex);
            for (auto& ss : m_pendingScrapShips) shiftIdx(ss.shipIndex);
        }
    }
}

// === processRecruitments ===
void Game::processRecruitments(int countryId) {
    for (size_t i = 0; i < m_pendingRecruitments.size(); ) {
        auto& r = m_pendingRecruitments[i];
        auto pit = m_provinces.getProvinceById(r.provinceId);
        if (!pit || pit->countryId != countryId) { ++i; continue; }
        r.turnsRemaining--;
        if (r.turnsRemaining <= 0) {
            auto& armies = m_provinceArmies[r.provinceId];
            auto it = std::find_if(armies.begin(), armies.end(), [&](auto& u) { return u.countryId == countryId; });
            if (it != armies.end()) it->count += r.count;
            else { ArmyUnit nu; nu.countryId = countryId; nu.count = r.count; armies.push_back(nu); }
            m_pendingRecruitments.erase(m_pendingRecruitments.begin() + i);
        } else ++i;
    }
}

// === processDisbandOrders ===
void Game::processDisbandOrders(int countryId) {
    for (size_t i = 0; i < m_pendingDisbandOrders.size(); ) {
        auto& d = m_pendingDisbandOrders[i];
        auto pit = m_provinces.getProvinceById(d.provinceId);
        if (!pit || pit->countryId != countryId) { ++i; continue; }
        auto aIt = m_provinceArmies.find(d.provinceId);
        if (aIt != m_provinceArmies.end()) {
            for (auto& u : aIt->second) {
                if (u.countryId == countryId) {
                    if (d.count <= 0 || d.count >= u.count) u.count = 0;
                    else u.count -= d.count;
                }
            }
            aIt->second.erase(std::remove_if(aIt->second.begin(), aIt->second.end(),
                [](auto& u) { return u.count <= 0; }), aIt->second.end());
            if (aIt->second.empty()) m_provinceArmies.erase(d.provinceId);
        }
        m_pendingDisbandOrders.erase(m_pendingDisbandOrders.begin() + i);
    }
}

// === processEmbarkations ===
void Game::processEmbarkations(int countryId) {
    for (size_t i = 0; i < m_pendingEmbarkations.size(); ) {
        auto& e = m_pendingEmbarkations[i];
        auto pit = m_provinces.getProvinceById(e.provinceId);
        if (!pit || pit->countryId != countryId) { ++i; continue; }
        e.turnsRemaining--;
        if (e.turnsRemaining <= 0) {
            int totalRemoved = 0;
            auto aIt = m_provinceArmies.find(e.provinceId);
            if (aIt != m_provinceArmies.end()) {
                for (auto& u : aIt->second) {
                    if (u.countryId == countryId) {
                        int toRemove = std::min(std::max(0, e.count - totalRemoved), u.count);
                        u.count -= toRemove;
                        totalRemoved += toRemove;
                    }
                }
                aIt->second.erase(std::remove_if(aIt->second.begin(), aIt->second.end(),
                    [](auto& u) { return u.count <= 0; }), aIt->second.end());
                if (aIt->second.empty()) m_provinceArmies.erase(e.provinceId);
            }
            // Assign troops to a boat at this province
            if (totalRemoved > 0) {
                auto cit = m_provinceCenters.find(e.provinceId);
                if (cit != m_provinceCenters.end()) {
                    float cx = cit->second.x, cy = cit->second.y;
                    float bestDist = 50.0f * 50.0f;
                    bool foundBoat = false;
                    for (auto& ship : m_ships) {
                        if (ship.countryId != countryId || ship.type != "boat") continue;
                        int spx, spy;
                        m_landSea.lonLatToPixel((float)ship.lon, (float)ship.lat, spx, spy);
                        float dx = spx - cx, dy = spy - cy;
                        float d2 = dx*dx + dy*dy;
                        if (d2 < bestDist) {
                            ship.crew += totalRemoved / 100;
                            totalRemoved = 0;
                            foundBoat = true;
                            break;
                        }
                    }
                    // No boat nearby — spawn one at the port province's water access
                    if (!foundBoat) {
                        int w = m_landSea.getWidth(), h = m_landSea.getHeight();
                        auto ppIt = m_provincePixels.find(e.provinceId);
                        if (ppIt != m_provincePixels.end()) {
                            int dx[4] = {1,-1,0,0}, dy[4] = {0,0,1,-1};
                            int boatPx = -1;
                            for (int idx : ppIt->second) {
                                if (boatPx >= 0) break;
                                int px = idx % w, py = idx / w;
                                for (int d = 0; d < 4; ++d) {
                                    int nx = px + dx[d], ny = py + dy[d];
                                    if (nx >= 0 && nx < w && ny >= 0 && ny < h && !m_landSea.isLand(nx, ny)) {
                                        // Check water body is large enough
                                        std::unordered_set<int> bv;
                                        std::vector<int> bs = {ny * w + nx};
                                        bv.insert(ny * w + nx);
                                        int bc = 0;
                                        while (!bs.empty() && bc < 200) {
                                            int bi = bs.back(); bs.pop_back(); bc++;
                                            int bcy = bi / w, bcx = bi % w;
                                            for (int bd = 0; bd < 4; ++bd) {
                                                int bnx = bcx + dx[bd], bny = bcy + dy[bd];
                                                if (bnx < 0) bnx = w - 1; else if (bnx >= w) bnx = 0;
                                                if (bny < 0 || bny >= h) continue;
                                                int bni = bny * w + bnx;
                                                if (!bv.count(bni) && !m_landSea.isLand(bnx, bny)) {
                                                    bv.insert(bni); bs.push_back(bni);
                                                }
                                            }
                                        }
                                        if (bc >= 200) {
                                            boatPx = ny * w + nx; break;
                                        }
                                    }
                                }
                            }
                            if (boatPx >= 0) {
                                float lon, lat;
                                m_landSea.pixelToLonLat(boatPx % w, boatPx / w, lon, lat);
                                NavyShip ns;
                                ns.lon = lon; ns.lat = lat;
                                ns.type = "boat"; ns.countryId = countryId;
                                ns.health = 100; ns.crew = totalRemoved / 100;
                                m_ships.push_back(ns);
                                if (m_config.aiDebug)
                                    printf("[EMBARK] Spawned boat for %lld troops at province %d\n",
                                       (long long)totalRemoved, e.provinceId);
                            }
                        }
                    }
                }
            }
            m_pendingEmbarkations.erase(m_pendingEmbarkations.begin() + i);
        } else ++i;
    }
}

// === processScrapShips ===
void Game::processScrapShips(int countryId) {
    for (size_t i = 0; i < m_pendingScrapShips.size(); ) {
        auto& s = m_pendingScrapShips[i];
        if (s.shipIndex >= 0 && s.shipIndex < (int)m_ships.size() && m_ships[s.shipIndex].countryId == countryId) {
            m_ships[s.shipIndex].countryId = UNC_CID; // Mark as sunk/scrapped
        }
        m_pendingScrapShips.erase(m_pendingScrapShips.begin() + i);
    }
}

// === processArmyMovement ===
void Game::processArmyMovement(int countryId) {
    // Helper to check if two countries are allied
    auto areAllied = [&](int a, int b) -> bool {
        if (a <= 0 || b <= 0 || a == b) return false;
        const Country* ac = m_countries.getCountry(a);
        const Country* bc = m_countries.getCountry(b);
        if (!ac || !bc) return false;
        auto ar = m_relations.find(ac->isoA3);
        if (ar == m_relations.end()) return false;
        auto dr = ar->second.find(bc->isoA3);
        return dr != ar->second.end() && dr->second.alliance;
    };
    auto transferCountryPixels = [&](int pid, int newOwner, int oldOwner) {
        if (m_countryPixels.empty()) return;
        auto ppIt = m_provincePixels.find(pid);
        if (ppIt == m_provincePixels.end()) return;
        const auto& provincePixels = ppIt->second;
        for (int idx : provincePixels) {
            if (idx >= 0 && idx < (int)m_pixelCountryArray.size())
                m_pixelCountryArray[idx] = newOwner;
        }
        // Rendering-only bookkeeping — see processArmyMovement's copy.
        if (m_aiTraining) return;
        if (oldOwner >= 0 && (size_t)oldOwner < m_countryPixels.size() &&
            newOwner >= 0 && (size_t)newOwner < m_countryPixels.size()) {
            auto& oldPx = m_countryPixels[oldOwner];
            auto& newPx = m_countryPixels[newOwner];
            std::unordered_set<int> pxSet(provincePixels.begin(), provincePixels.end());
            std::vector<int> transferred;
            transferred.reserve(provincePixels.size());
            auto newEnd = std::remove_if(oldPx.begin(), oldPx.end(), [&](int idx) {
                if (pxSet.count(idx)) {
                    transferred.push_back(idx);
                    return true;
                }
                return false;
            });
            oldPx.erase(newEnd, oldPx.end());
            newPx.insert(newPx.end(), transferred.begin(), transferred.end());
        }
    };
    // Process move orders: move pct of garrison from source to target
    for (size_t i = 0; i < m_pendingMoveOrders.size(); ) {
        auto& mo = m_pendingMoveOrders[i];
        Province* src = m_provinces.getProvinceById(mo.fromProvince);
        Province* dst = m_provinces.getProvinceById(mo.toProvince);
        if (!src || !dst || mo.countryId != countryId) { ++i; continue; }
        auto sIt = m_provinceArmies.find(mo.fromProvince);
        if (sIt == m_provinceArmies.end()) { ++i; continue; }
        auto& srcArmies = sIt->second;
        auto uIt = std::find_if(srcArmies.begin(), srcArmies.end(),
            [&](auto& u) { return u.countryId == countryId; });
        if (uIt == srcArmies.end()) { ++i; continue; }
        // Calculate soldiers to move
        float pct = mo.pct / 100.0f;
        int toMove = (int)(uIt->count * pct);
        if (toMove <= 0) { ++i; continue; }
        uIt->count -= toMove;
        if (uIt->count <= 0) {
            srcArmies.erase(uIt);
        }
        if (srcArmies.empty()) m_provinceArmies.erase(mo.fromProvince);
        // Apply fortification defense at destination
        auto indIt = m_provinceIndustry.find(mo.toProvince);
        float fortDef = 0;
        if (indIt != m_provinceIndustry.end()) fortDef = indIt->second.fortification * 10.0f;
        // Check destination for troops from other (non-allied) countries
        auto& dstArmies = m_provinceArmies[mo.toProvince];
        auto eIt = std::find_if(dstArmies.begin(), dstArmies.end(),
            [&](auto& u) { return u.countryId != countryId && u.countryId > 0 && !areAllied(countryId, u.countryId); });
        if (eIt != dstArmies.end()) {
            // Combat: attacker vs defender
            float atkMod = 1.0f + getTotalEffect("armyAtkPct") / 100.0f;
            float defMod = 1.0f;
            auto eCit = m_countryCompass.find(eIt->countryId);
            if (eCit != m_countryCompass.end()) {
                defMod = 1.0f;
            }
            int atkPower = (int)(toMove * atkMod);
            int defPower = (int)(eIt->count * (1.0f + fortDef / 100.0f) * defMod);
                int prevOwner = dst->countryId;
                if (atkPower > defPower) {
                    int remaining = atkPower - defPower;
                    eIt->count = 0;
                    // Auto-claim: previous owner claims this province (skip UNC/BLC)
                    if (prevOwner > 0 && prevOwner != UNC_CID && prevOwner != BLC_CID && prevOwner != countryId) {
                        const Country* prevC = m_countries.getCountry(prevOwner);
                        if (prevC && dst->id > 0) {
                            auto& claimList = m_claims[prevC->isoA3];
                            if (std::find(claimList.begin(), claimList.end(), dst->id) == claimList.end()) {
                                claimList.push_back(dst->id);
                                m_claimsByProvince[dst->id].push_back(prevC->isoA3);
                            }
                        }
                    }
                    dst->countryId = countryId;
                // Update pixel lookup arrays + countryPixels
                if (dst->id > 0 && (size_t)dst->id < m_provinceCountryLookup.size())
                    m_provinceCountryLookup[dst->id] = countryId;
                transferCountryPixels(dst->id, countryId, prevOwner);
                // Conquered province: add negative alignment drift for its minorities
                {
                    auto minIt = m_provinceMinorities.find(mo.toProvince);
                    if (minIt != m_provinceMinorities.end())
                        for (auto& mg : minIt->second)
                            m_minorityAlignmentDrift[mg.name] -= 25.0f;
                }
                // Remove claim if conqueror claimed this province
                {
                    const Country* conqueror = m_countries.getCountry(countryId);
                    if (conqueror) {
                        auto& cl = m_claims[conqueror->isoA3];
                        auto cp = std::find(cl.begin(), cl.end(), dst->id);
                        if (cp != cl.end()) {
                            cl.erase(cp);
                            auto& bp = m_claimsByProvince[dst->id];
                            bp.erase(std::remove(bp.begin(), bp.end(), conqueror->isoA3), bp.end());
                            if (bp.empty()) m_claimsByProvince.erase(dst->id);
                        }
                    }
                }
                // Place remaining troops (survivors keep same ratio)
                int survivingTroops = (atkMod > 0) ? (int)(remaining / atkMod) : toMove;
                auto myIt = std::find_if(dstArmies.begin(), dstArmies.end(),
                    [&](auto& u) { return u.countryId == countryId; });
                if (myIt != dstArmies.end()) myIt->count += survivingTroops;
                else { ArmyUnit nu; nu.countryId = countryId; nu.count = survivingTroops; dstArmies.push_back(nu); }
            } else {
                // Attacker loses — all attacking troops are killed
                eIt->count -= (int)(toMove * atkMod * (1.0f - fortDef / 200.0f));
                if (eIt->count < 0) eIt->count = 0;
            }
            dstArmies.erase(std::remove_if(dstArmies.begin(), dstArmies.end(),
                [](auto& u) { return u.count <= 0; }), dstArmies.end());
            if (dstArmies.empty()) m_provinceArmies.erase(mo.toProvince);
        } else {
            // No enemy troops — check if destination is empty enemy territory
            if (dst->countryId != countryId && dst->countryId > 0 && !areAllied(countryId, dst->countryId)) {
                int prevOwner = dst->countryId;
                if (prevOwner > 0 && prevOwner != UNC_CID && prevOwner != BLC_CID && prevOwner != countryId) {
                    const Country* prevC = m_countries.getCountry(prevOwner);
                    if (prevC && dst->id > 0) {
                        auto& claimList = m_claims[prevC->isoA3];
                        if (std::find(claimList.begin(), claimList.end(), dst->id) == claimList.end()) {
                            claimList.push_back(dst->id);
                            m_claimsByProvince[dst->id].push_back(prevC->isoA3);
                        }
                    }
                }
                dst->countryId = countryId;
                if (dst->id > 0 && (size_t)dst->id < m_provinceCountryLookup.size())
                    m_provinceCountryLookup[dst->id] = countryId;
                transferCountryPixels(dst->id, countryId, prevOwner);
                auto minIt = m_provinceMinorities.find(mo.toProvince);
                if (minIt != m_provinceMinorities.end())
                    for (auto& mg : minIt->second)
                        m_minorityAlignmentDrift[mg.name] -= 25.0f;
                const Country* conqueror = m_countries.getCountry(countryId);
                if (conqueror) {
                    auto& cl = m_claims[conqueror->isoA3];
                    auto cp = std::find(cl.begin(), cl.end(), dst->id);
                    if (cp != cl.end()) {
                        cl.erase(cp);
                        auto& bp = m_claimsByProvince[dst->id];
                        bp.erase(std::remove(bp.begin(), bp.end(), conqueror->isoA3), bp.end());
                        if (bp.empty()) m_claimsByProvince.erase(dst->id);
                    }
                }
            }
            // Move troops in
            auto myIt = std::find_if(dstArmies.begin(), dstArmies.end(),
                [&](auto& u) { return u.countryId == countryId; });
            if (myIt != dstArmies.end()) myIt->count += toMove;
            else { ArmyUnit nu; nu.countryId = countryId; nu.count = toMove; dstArmies.push_back(nu); }
        }
        m_pendingMoveOrders.erase(m_pendingMoveOrders.begin() + i);
    }
}

// === processNavyMovement ===
void Game::processNavyMovement(int countryId) {
    // Move ships towards their destinations
    for (size_t i = 0; i < m_pendingShipMoveOrders.size(); ) {
        auto& mo = m_pendingShipMoveOrders[i];
        if (mo.shipIndex < 0 || mo.shipIndex >= (int)m_ships.size()) { ++i; continue; }
        auto& ship = m_ships[mo.shipIndex];
        if (ship.countryId != countryId) { ++i; continue; }
        // Simple movement: teleport to destination for now (real: interpolate)
        ship.lon = mo.destLon;
        ship.lat = mo.destLat;
        m_pendingShipMoveOrders.erase(m_pendingShipMoveOrders.begin() + i);
    }
}

// === processNavyCombat ===
void Game::processNavyCombat(int countryId) {
    // Process ship engage orders — health-based damage
    for (size_t i = 0; i < m_pendingShipEngageOrders.size(); ) {
        auto& eo = m_pendingShipEngageOrders[i];
        if (eo.shipIndex < 0 || eo.shipIndex >= (int)m_ships.size() ||
            eo.targetIndex < 0 || eo.targetIndex >= (int)m_ships.size()) { ++i; continue; }
        auto& src = m_ships[eo.shipIndex];
        auto& tgt = m_ships[eo.targetIndex];
        if (src.countryId != countryId || tgt.countryId <= 0 || tgt.countryId == UNC_CID) { ++i; continue; }

        float atkMod = 1.0f + getTotalEffect("navyAtkPct") / 100.0f;
        float dx = (float)(src.lon - tgt.lon), dy = (float)(src.lat - tgt.lat);
        float dist = sqrtf(dx*dx + dy*dy);
        // Closer = more damage: 1.0 at point blank, 0.1 at 15+ units
        float distFactor = std::max(0.1f, 1.0f - dist / 15.0f);

        // Carrier does most damage, boat least
        float baseDmg = 0;
        if (src.type == "carrier") baseDmg = 35;
        else if (src.type == "destroyer") baseDmg = 25;
        else if (src.type == "frigate") baseDmg = 20;
        else if (src.type == "boat") baseDmg = 5;

        int damage = (int)(baseDmg * atkMod * distFactor);
        if (damage < 1) damage = 1;
        tgt.health -= damage;

        printf("[NAVY] Ship %d (%s) dealt %d dmg to ship %d (%s): health %d->%d\n",
               eo.shipIndex, src.type.c_str(), damage, eo.targetIndex, tgt.type.c_str(),
               tgt.health + damage, tgt.health);

        if (tgt.health <= 0) {
            tgt.countryId = UNC_CID;
            printf("[NAVY] Ship %d (%s) SUNK ship %d (%s)!\n",
                   eo.shipIndex, src.type.c_str(), eo.targetIndex, tgt.type.c_str());
        }
        m_pendingShipEngageOrders.erase(m_pendingShipEngageOrders.begin() + i);
    }
}

// === cleanupSunkShips ===
void Game::cleanupSunkShips() {
    // Remove all ships with countryId == UNC_CID (sunk/scrapped) and shift pending order indices
    for (int i = (int)m_ships.size() - 1; i >= 0; i--) {
        if (m_ships[i].countryId == UNC_CID) {
            int removedIdx = i;
            m_ships.erase(m_ships.begin() + i);
            // Shift all pending order indices that reference ships after the removed one
            auto shiftIdx = [&](int& idx) { if (idx > removedIdx) idx--; };
            for (auto& eo : m_pendingShipEngageOrders) { shiftIdx(eo.shipIndex); shiftIdx(eo.targetIndex); }
            for (auto& mo : m_pendingShipMoveOrders) shiftIdx(mo.shipIndex);
            for (auto& bo : m_pendingShipBombardOrders) shiftIdx(bo.shipIndex);
            for (auto& d : m_pendingShipDisembarks) shiftIdx(d.shipIndex);
            for (auto& ss : m_pendingScrapShips) shiftIdx(ss.shipIndex);
            for (int& idx : m_selectedShipIndices) shiftIdx(idx);
        }
    }
}

// === eliminateDefeatedCountries ===
void Game::eliminateDefeatedCountries() {
    // Count provinces per country
    std::unordered_map<int, int> provCount;
    for (auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId > 0 && p.countryId != UNC_CID && p.countryId != BLC_CID)
            provCount[p.countryId]++;
    }
    // Rebels whose last province was retaken are removed outright (below).
    // Map countries are only marked: they keep their entry so an amphibious
    // landing can revive them and saves/UI can still name them.
    std::vector<int> deadRebels;
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID || cid == SPC_CID) continue;
        if (provCount[cid] > 0) {
            // Holds land again (e.g. revived by an amphibious landing) — allow
            // a future re-elimination to fire cleanly.
            m_eliminatedCids.erase(cid);
            continue;
        }
        // Already torn down on a previous turn — nothing left to disband.
        if (!m_eliminatedCids.insert(cid).second) continue;
        // Fully conquered — delete navy, armies, treasury, policies
        for (auto& ship : m_ships)
            if (ship.countryId == cid) ship.countryId = UNC_CID;
        for (auto& [pid, units] : m_provinceArmies)
            units.erase(std::remove_if(units.begin(), units.end(),
                [cid](auto& u) { return u.countryId == cid; }), units.end());
        c.treasury = 0;
        m_countryBalances[cid] = 0;
        if (cid >= REBEL_CID_MIN) deadRebels.push_back(cid);
        if (m_config.aiDebug)
            printf("[ELIMINATE] %s (%s) fully conquered — navy dissolved, armies disbanded\n",
                   c.name.c_str(), c.isoA3.c_str());
    }
    // Fully retire dead rebel states. A dissolved rebel can never return (no
    // navy, no armies, no provinces), yet it used to linger in m_countries
    // forever: thousands piled up over a long self-play run, every one of them
    // still "at war" with its parent — which kept spamming ceasefire requests
    // at corpses each turn — and allocateRebelCid could never reuse their ids,
    // marching the counter into the 65533-65535 sentinel range.
    for (int cid : deadRebels) {
        const Country* rc = m_countries.getCountry(cid);
        if (!rc) continue;
        std::string iso = rc->isoA3;
        // Drop every relation row/column touching the dead rebel so nobody
        // keeps negotiating with (or declaring war on) a ghost.
        m_relations.erase(iso);
        for (auto& [otherIso, rels] : m_relations) rels.erase(iso);
        m_pendingDiplomaticActions.erase(
            std::remove_if(m_pendingDiplomaticActions.begin(), m_pendingDiplomaticActions.end(),
                [&](const PendingDiplomaticAction& da) {
                    return da.sourceIso == iso || da.targetIso == iso;
                }), m_pendingDiplomaticActions.end());
        m_claims.erase(iso);
        for (auto& [pid, isos] : m_claimsByProvince)
            isos.erase(std::remove(isos.begin(), isos.end(), iso), isos.end());
        m_rebelFlagSvgs.erase(cid);
        m_countryBalances.erase(cid);
        m_countryPacification.erase(cid);
        m_rebellionsThisTurnByCid.erase(cid);
        if (cid < (int)m_countryPixels.size()) {
            m_countryPixels[cid].clear();
            m_countryPixels[cid].shrink_to_fit();
        }
        m_eliminatedCids.erase(cid); // cid is free for reuse now
        m_countries.getAll().erase(cid);
    }
    if (!deadRebels.empty()) rebuildIsoIndex();
}

// === declareWar ===
// The one place wars start. Guarantee semantics: a guarantee on the DEFENDER
// obliges the guarantor to enter the war against the attacker. Guarantees are
// stored one-directionally in places, so both directions are checked.
void Game::applyWarKinPenalty(const std::string& attackerIso, const std::string& defenderIso) {
    int attackerCid = cidForIso(attackerIso);
    int defenderCid = cidForIso(defenderIso);
    if (attackerCid < 0 || defenderCid < 0) return;
    // Count minority population in defender
    std::unordered_map<std::string, long long> defenderMinPop;
    for (auto& [pid, pv] : m_provinces.getAllProvinces()) {
        if (pv.countryId != defenderCid) continue;
        long long pop = m_provincePopulations.count(pid) ? m_provincePopulations[pid] : 0;
        auto mit = m_provinceMinorities.find(pid);
        if (mit == m_provinceMinorities.end()) continue;
        for (auto& mg : mit->second)
            defenderMinPop[mg.name] += (long long)(pop * mg.pct / 100.0f);
    }
    // Apply penalty to attacker's minorities that have kin in defender
    for (auto& [pid, pv] : m_provinces.getAllProvinces()) {
        if (pv.countryId != attackerCid) continue;
        auto mit = m_provinceMinorities.find(pid);
        if (mit == m_provinceMinorities.end()) continue;
        for (auto& mg : mit->second) {
            auto dpIt = defenderMinPop.find(mg.name);
            if (dpIt != defenderMinPop.end() && dpIt->second >= 500000)
                m_minorityAlignmentDrift[mg.name] -= 30.0f;
        }
    }
}

void Game::declareWar(const std::string& attackerIso, const std::string& defenderIso,
                      bool chainGuarantees) {
    if (attackerIso.empty() || defenderIso.empty() || attackerIso == defenderIso) return;
    CountryRelation& fwd = m_relations[attackerIso][defenderIso];
    if (fwd.war) return; // already at war — nothing to do, no double penalties
    fwd.war = true;
    fwd.alliance = false;
    fwd.nonAggression = false;
    CountryRelation& rev = m_relations[defenderIso][attackerIso];
    rev.war = true;
    rev.alliance = false;
    rev.nonAggression = false;

    applyWarKinPenalty(attackerIso, defenderIso);
    applyWarKinPenalty(defenderIso, attackerIso);

    // Player notification when dragged in (guarantee chains can reach the
    // player without any request ever targeting them)
    std::string playerIso;
    if (const Country* pc = m_countries.getCountry(m_playerCountryId)) playerIso = pc->isoA3;
    if (!playerIso.empty() && defenderIso == playerIso) {
        int otherCid = cidForIso(attackerIso);
        const Country* oc = m_countries.getCountry(otherCid);
        std::string otherName = oc ? oc->name : attackerIso;
        pushPopup(PopupType::WAR_DECLARED, "War Declared!",
                  otherName + " has declared war on you!", otherCid);
    }

    printf("[WAR] %s declares war on %s\n", attackerIso.c_str(), defenderIso.c_str());

    if (!chainGuarantees) return;
    // Every guarantor of the defender joins against the attacker. Collect
    // first, declare after — declaring mutates m_relations while iterating.
    std::vector<std::string> guarantors;
    for (auto& [isoA, targets] : m_relations) {
        if (isoA == attackerIso || isoA == defenderIso) continue;
        bool guards = false;
        auto it = targets.find(defenderIso);
        if (it != targets.end() && it->second.guarantee) guards = true;
        if (!guards) {
            auto dIt = m_relations.find(defenderIso);
            if (dIt != m_relations.end()) {
                auto rIt = dIt->second.find(isoA);
                if (rIt != dIt->second.end() && rIt->second.guarantee) guards = true;
            }
        }
        if (guards && cidForIso(isoA) >= 0) guarantors.push_back(isoA);
    }
    for (auto& g : guarantors) {
        printf("[WAR] %s honours its guarantee of %s and joins against %s\n",
               g.c_str(), defenderIso.c_str(), attackerIso.c_str());
        declareWar(g, attackerIso, false); // one level only
        if (!playerIso.empty() && g == playerIso)
            addNotification("You honour your guarantee of " + defenderIso +
                            " and are now at war with " + attackerIso, RED, 8.0f);
    }
}

// === processDiplomaticRequests ===
void Game::processDiplomaticRequests() {
    // Get player ISO
    std::string playerIso;
    {
        const Country* pc = m_countries.getCountry(m_playerCountryId);
        if (pc) playerIso = pc->isoA3;
    }

    for (size_t i = 0; i < m_pendingDiplomaticActions.size(); ) {
        auto& da = m_pendingDiplomaticActions[i];
        // Skip if this action involves the player and is a request — handled via popup instead
        bool isRequestToPlayer = !playerIso.empty() && da.targetIso == playerIso
            && da.sourceIso != playerIso
            && (da.action == "request_alliance" || da.action == "request_guarantee"
             || da.action == "request_nap" || da.action == "declare_war"
             || da.action == "request_ceasefire");
        if (isRequestToPlayer) {
            // Find requesting country ID
            int reqCid = -1;
            for (auto& [cid, c] : m_countries.getAll()) {
                if (c.isoA3 == da.sourceIso) { reqCid = cid; break; }
            }
            std::string title, msg;
            const Country* srcC = m_countries.getCountry(reqCid);
            std::string srcName = srcC ? srcC->name : da.sourceIso;

            if (da.action == "request_ceasefire") {
                // Special flow: show terms in the popup.
                // Three-turn flow:
                //   Turn 1 (this): pop the popup up to the player and let them Accept/Reject.
                //                 The action is ERASED here (removed from `m_pendingDiplomaticActions`).
                //                 The terms stay in `m_pendingCeasefireTerms` so cancel
                //                 of the popup isn't required.
                //   Turn 2 (after Accept): we re-add a `apply_ceasefire` action with turnsRemaining=1
                //                 and store the accepted terms in `m_acceptedCeasefireTerms`.
                //   Turn 3 (apply_ceasefire fires): we apply war=false, transfer provinces, etc.
                std::string key = da.sourceIso + "|" + da.targetIso;
                auto tit = m_pendingCeasefireTerms.find(key);
                CeasefireTerms terms;
                if (tit != m_pendingCeasefireTerms.end()) terms = tit->second;

                // Build a brief summary
                std::string summary;
                summary += srcName + " proposes a ceasefire.\n";
                if (terms.ourMoney > 0)  summary += TextFormat("  Offers %d gold\n", terms.ourMoney);
                if (terms.theirMoney > 0) summary += TextFormat("  Demands %d gold\n", terms.theirMoney);
                if (!terms.ourProvs.empty()) summary += TextFormat("  Cedes %zu province(s)\n", terms.ourProvs.size());
                if (!terms.theirProvs.empty()) summary += TextFormat("  Demands %zu province(s)\n", terms.theirProvs.size());
                if (!terms.ourDropClaims.empty()) summary += TextFormat("  Drops %zu own claim(s)\n", terms.ourDropClaims.size());
                if (!terms.theirDropClaims.empty()) summary += TextFormat("  Demands you drop %zu claim(s)\n", terms.theirDropClaims.size());
                if (summary.back() == '\n') summary.pop_back();
                pushPopup(PopupType::CEASEFIRE_REQUEST, "Ceasefire Offer", summary,
                          reqCid, da.action, da.sourceIso, da.targetIso);
                // Attach the terms to the popup entry so draw/update can show + apply them.
                if (!m_popupQueue.empty()) m_popupQueue.back().terms = terms;
                m_pendingDiplomaticActions.erase(m_pendingDiplomaticActions.begin() + i);
                printf("[CEASEFIRE] Incoming offer from %s → player (pending review)\n", da.sourceIso.c_str());
                continue;
            }

            // Turn it into a popup immediately (removed from queue)
            PopupType pt = (da.action == "declare_war") ? PopupType::WAR_DECLARED : PopupType::DIPLOMATIC_REQUEST;
            std::string msg2;
            if (da.action == "declare_war") {
                title = "War Declared!";
                msg2 = srcName + " has declared war on " + (playerIso.empty() ? da.targetIso : "you") + "!";
            } else if (da.action == "request_alliance") {
                title = "Alliance Request";
                msg2 = srcName + " proposes an alliance.";
            } else if (da.action == "request_guarantee") {
                title = "Guarantee Request";
                msg2 = srcName + " requests a mutual guarantee.";
            } else if (da.action == "request_nap") {
                title = "Non-Aggression Proposal";
                msg2 = srcName + " proposes a non-aggression pact.";
            }
            pushPopup(pt, title, msg2, reqCid, da.action, da.sourceIso, da.targetIso);
            // War is not a request — it happens whether or not the player has
            // dismissed the popup yet. This used to be dropped entirely here.
            if (da.action == "declare_war")
                declareWar(da.sourceIso, da.targetIso, true);
            m_pendingDiplomaticActions.erase(m_pendingDiplomaticActions.begin() + i);
            printf("[DIPLO] Incoming request from %s → player: %s (pushed to popup queue)\n",
                   da.sourceIso.c_str(), da.action.c_str());
            continue;
        }

        da.turnsRemaining--;
        if (da.turnsRemaining <= 0) {
            // Requests aimed at an AI country go through its diplomacy net
            // instead of being auto-accepted.
            if (m_ai && (da.action == "request_alliance" || da.action == "request_guarantee" ||
                         da.action == "request_nap")) {
                int aiTgtCid = cidForIso(da.targetIso);
                if (aiTgtCid >= 0 && aiTgtCid != m_playerCountryId &&
                    !m_ai->decideDiplomacy(aiTgtCid, da.action, da.sourceIso)) {
                    // Remember the refusal so the proposer backs off properly.
                    m_ai->noteDiploRejected(cidForIso(da.sourceIso), aiTgtCid);
                    if (m_config.aiDebug)
                        printf("[DIPLO] %s rejected %s from %s\n", da.targetIso.c_str(),
                               da.action.c_str(), da.sourceIso.c_str());
                    if (!playerIso.empty() && da.sourceIso == playerIso)
                        addNotification(da.targetIso + " rejected your request", ORANGE, 6.0f);
                    m_pendingDiplomaticActions.erase(m_pendingDiplomaticActions.begin() + i);
                    continue;
                }
            }
            // Apply the diplomatic action
            auto& rels = m_relations[da.sourceIso];
            auto& rt = rels[da.targetIso];
            if (da.action == "request_alliance") {
                rt.alliance = true;
                m_relations[da.targetIso][da.sourceIso].alliance = true;
            } else if (da.action == "break_alliance") {
                rt.alliance = false;
                m_relations[da.targetIso][da.sourceIso].alliance = false;
                // Find country IDs, protect player's troops from conversion
                int srcCid = -1, tgtCid = -1;
                for (auto& [cid, c] : m_countries.getAll()) {
                    if (c.isoA3 == da.sourceIso) srcCid = cid;
                    if (c.isoA3 == da.targetIso) tgtCid = cid;
                }
                // Absorb foreign troops on each side's soil (player's troops never convert)
                auto absorbForeign = [&](int localCid, int foreignCid) {
                    if (localCid < 0 || foreignCid < 0) return;
                    for (auto& [pid, units] : m_provinceArmies) {
                        Province* pp = m_provinces.getProvinceById(pid);
                        if (!pp || pp->countryId != localCid) continue;
                        for (auto& u : units) {
                            if (u.countryId == foreignCid && foreignCid != m_playerCountryId)
                                u.countryId = localCid;
                        }
                    }
                };
                absorbForeign(srcCid, tgtCid);
                absorbForeign(tgtCid, srcCid);
            } else if (da.action == "request_guarantee") {
                // Mirrored — guarantee was the only relation written one-way,
                // which made guarantor lookups direction-dependent.
                rt.guarantee = true;
                m_relations[da.targetIso][da.sourceIso].guarantee = true;
            } else if (da.action == "break_guarantee") {
                rt.guarantee = false;
                m_relations[da.targetIso][da.sourceIso].guarantee = false;
            } else if (da.action == "request_nap") {
                rt.nonAggression = true;
                m_relations[da.targetIso][da.sourceIso].nonAggression = true;
            } else if (da.action == "break_nap") {
                rt.nonAggression = false;
                m_relations[da.targetIso][da.sourceIso].nonAggression = false;
            } else if (da.action == "request_ceasefire") {
                // AI decides whether to accept the ceasefire offer.
                // Simple heuristic: AI always accepts if the player is stronger
                // or if terms are favorable (no demands from AI).
                std::string key = da.sourceIso + "|" + da.targetIso;
                auto tit = m_pendingCeasefireTerms.find(key);
                // The target country's diplomacy net decides on the ceasefire
                int cfTgtCid = cidForIso(da.targetIso);
                bool aiAccepts = true;
                if (m_ai && cfTgtCid >= 0 && cfTgtCid != m_playerCountryId)
                    aiAccepts = m_ai->decideDiplomacy(cfTgtCid, "request_ceasefire", da.sourceIso);

                // Check for mutual ceasefire: if target also sent a ceasefire
                // request to source, pick one randomly and cancel the other.
                std::string revKey = da.targetIso + "|" + da.sourceIso;
                auto revIt = m_pendingCeasefireTerms.find(revKey);
                bool mutualCeasefire = false;
                if (revIt != m_pendingCeasefireTerms.end()) {
                    // Check if there's also a pending request_ceasefire from target to source
                    for (auto& rda : m_pendingDiplomaticActions) {
                        if (rda.sourceIso == da.targetIso && rda.targetIso == da.sourceIso &&
                            rda.action == "request_ceasefire") {
                            mutualCeasefire = true;
                            break;
                        }
                    }
                }

                if (aiAccepts) {
                    rt.war = false;
                    m_relations[da.targetIso][da.sourceIso].war = false;
                    // Apply terms — if sender already deducted money, pass alreadyDeducted=true
                    if (tit != m_pendingCeasefireTerms.end()) {
                        applyCeasefireTerms(da.sourceIso, da.targetIso, tit->second, da.sourceIso == playerIso);
                        m_pendingCeasefireTerms.erase(tit);
                    }
                    // If mutual ceasefire, cancel the reverse request and pick
                    // this one (the first to be processed wins).
                    if (mutualCeasefire) {
                        for (auto& rda : m_pendingDiplomaticActions) {
                            if (rda.sourceIso == da.targetIso && rda.targetIso == da.sourceIso &&
                                rda.action == "request_ceasefire") {
                                rda.action = "cancel"; // mark for removal
                                break;
                            }
                        }
                        auto revTit = m_pendingCeasefireTerms.find(revKey);
                        if (revTit != m_pendingCeasefireTerms.end())
                            m_pendingCeasefireTerms.erase(revTit);
                        printf("[CEASEFIRE] Mutual ceasefire: picked %s->%s, cancelled reverse\n",
                               da.sourceIso.c_str(), da.targetIso.c_str());
                    }
                    // Notify player if they're involved
                    if (da.sourceIso == playerIso || da.targetIso == playerIso) {
                        const Country* otherC = m_countries.getCountryByCode(
                            (da.sourceIso == playerIso) ? da.targetIso : da.sourceIso);
                        std::string otherName = otherC ? otherC->name : (da.sourceIso == playerIso ? da.targetIso : da.sourceIso);
                        pushPopup(PopupType::WAR_DECLARED, "Ceasefire Accepted!",
                            otherName + " has accepted your ceasefire offer. The war is over.",
                            0, "ceasefire_accepted", da.sourceIso, da.targetIso);
                    }
                } else {
                    // AI rejects — refund offered money + notify player
                    if (tit != m_pendingCeasefireTerms.end()) {
                        int refund = tit->second.ourMoney;
                        // Only the player pre-pays at proposal time (hence the
                        // alreadyDeducted flag passed to applyCeasefireTerms).
                        // An AI sender is charged on acceptance instead, so
                        // refunding one here would mint treasury from nothing
                        // on every rejected offer.
                        if (refund > 0 && da.sourceIso == playerIso) {
                            int srcCid = -1;
                            for (auto& [cid, c] : m_countries.getAll())
                                if (c.isoA3 == da.sourceIso) { srcCid = cid; break; }
                            if (srcCid >= 0)
                                m_countries.getAll()[srcCid].treasury += refund;
                        }
                        m_pendingCeasefireTerms.erase(tit);
                    }
                    if (da.sourceIso == playerIso) {
                        const Country* otherC = m_countries.getCountryByCode(da.targetIso);
                        std::string otherName = otherC ? otherC->name : da.targetIso;
                        pushPopup(PopupType::WAR_DECLARED, "Ceasefire Rejected",
                            otherName + " has rejected your ceasefire offer. The war continues.",
                            0, "ceasefire_rejected", da.sourceIso, da.targetIso);
                    }
                }
                printf("[CEASEFIRE] War ended: %s vs %s (aiAccepts=%d mutual=%d)\n",
                       da.sourceIso.c_str(), da.targetIso.c_str(), aiAccepts, mutualCeasefire);
            } else if (da.action == "cancel") {
                // Dummy action — refund offered money (cancelled by mutual ceasefire override)
                std::string key2 = da.sourceIso + "|" + da.targetIso;
                auto tit2 = m_pendingCeasefireTerms.find(key2);
                if (tit2 != m_pendingCeasefireTerms.end()) {
                    int refund = tit2->second.ourMoney;
                    // Player-only, for the same reason as the rejection path.
                    if (refund > 0 && da.sourceIso == playerIso) {
                        int srcCid = -1;
                        for (auto& [cid, c] : m_countries.getAll())
                            if (c.isoA3 == da.sourceIso) { srcCid = cid; break; }
                        if (srcCid >= 0)
                            m_countries.getAll()[srcCid].treasury += refund;
                    }
                    m_pendingCeasefireTerms.erase(tit2);
                }
                printf("[CEASEFIRE] Cancelled mutual ceasefire (overridden, money refunded)\n");
            } else if (da.action == "apply_ceasefire") {
                // Player approved a ceasefire offer; now apply the actual
                // terms one turn later, then end the war.
                rt.war = false;
                m_relations[da.targetIso][da.sourceIso].war = false;
                std::string key = da.sourceIso + "|" + da.targetIso;
                auto tit = m_acceptedCeasefireTerms.find(key);
                if (tit != m_acceptedCeasefireTerms.end()) {
                    applyCeasefireTerms(da.sourceIso, da.targetIso, tit->second, false);
                    m_acceptedCeasefireTerms.erase(tit);
                }
                printf("[CEASEFIRE] Accepted offer applied: %s vs %s\n", da.sourceIso.c_str(), da.targetIso.c_str());
            } else if (da.action == "declare_war") {
                // Guarantee chains + kin penalties + notifications all live in
                // the helper so every declaration behaves identically.
                declareWar(da.sourceIso, da.targetIso, true);
            }
            if (m_config.aiDebug)
                printf("[DIPLO] %s → %s: %s applied\n", da.sourceIso.c_str(), da.targetIso.c_str(), da.action.c_str());
            m_pendingDiplomaticActions.erase(m_pendingDiplomaticActions.begin() + i);
        } else ++i;
    }
}

// === withdrawArmiesAfterPeace ===
// When a war ends, neither side's troops may remain standing on the other's
// soil. Nothing in the engine retreats, captures or attrits troops in foreign
// territory, so before this existed they simply stayed: still drawing upkeep,
// still blocking the owner, and — since the war was over — untouchable. The
// player-visible symptom was "we made peace and they still have an army in my
// country".
//
// Each intruding stack walks home to the nearest province its owner actually
// holds. A single-hop check is not enough: a deep incursion is several
// provinces from its own border, which is exactly when this is most visible.
// A stack that cannot reach home within kMaxHops is interned and disbands —
// the alternative is teleporting it across the map, which is worse.
int Game::withdrawArmiesAfterPeace(int cidA, int cidB) {
    if (cidA <= 0 || cidB <= 0 || cidA == cidB) return 0;
    constexpr int kMaxHops = 12;

    auto ownerOf = [&](int pid) -> int {
        if (pid < 0 || (size_t)pid >= m_provinceCountryLookup.size()) return 0;
        return m_provinceCountryLookup[pid];
    };

    // Nearest province owned by `cid`, breadth-first from `start`.
    auto findHome = [&](int start, int cid) -> int {
        std::unordered_set<int> seen{start};
        std::deque<std::pair<int, int>> q;
        q.push_back({start, 0});
        while (!q.empty()) {
            auto [p, d] = q.front();
            q.pop_front();
            if (d >= kMaxHops) continue;
            auto nIt = m_provinceNeighbors.find(p);
            if (nIt == m_provinceNeighbors.end()) continue;
            for (int n : nIt->second) {
                if (!seen.insert(n).second) continue;
                if (ownerOf(n) == cid) return n;
                q.push_back({n, d + 1});
            }
        }
        return -1;
    };

    // Collect first: the move below mutates m_provinceArmies.
    std::vector<std::pair<int, int>> work;   // (province, intruding country)
    for (const auto& [pid, units] : m_provinceArmies) {
        int owner = ownerOf(pid);
        if (owner != cidA && owner != cidB) continue;
        int intruder = (owner == cidA) ? cidB : cidA;
        for (const auto& u : units)
            if (u.countryId == intruder && u.count > 0) { work.push_back({pid, intruder}); break; }
    }

    int cleared = 0;
    for (const auto& [pid, intruder] : work) {
        auto ait = m_provinceArmies.find(pid);
        if (ait == m_provinceArmies.end()) continue;

        int moving = 0;
        auto& units = ait->second;
        for (auto it = units.begin(); it != units.end(); ) {
            if (it->countryId == intruder) { moving += it->count; it = units.erase(it); }
            else ++it;
        }
        if (units.empty()) m_provinceArmies.erase(pid);
        if (moving <= 0) continue;
        cleared++;

        int dest = findHome(pid, intruder);
        if (dest >= 0) {
            auto& dst = m_provinceArmies[dest];
            bool merged = false;
            for (auto& u : dst)
                if (u.countryId == intruder) { u.count += moving; merged = true; break; }
            if (!merged) dst.push_back({intruder, moving});
        }
        if (m_config.aiDebug)
            printf("[PEACE] cid=%d %s %d troops from prov %d\n", intruder,
                   dest >= 0 ? "withdrew" : "interned (no route home)", moving, pid);
    }
    return cleared;
}

// === applyCeasefireTerms ===
// Apply a ceasefire's obligations: transfer provinces, drop claims, move money.
//   sourceIso = the offer's sender (the country offering its provinces)
//   targetIso = the offer's recipient (the country being asked to do things)
//   terms.ourProvs   → provinces the sender cedes to the recipient
//   terms.theirProvs → provinces the recipient cedes to the sender
//   terms.ourDropClaims     → claims the sender drops (its own claims on these pids)
//   terms.theirDropClaims   → claims the recipient drops (its claims on these pids)
//   terms.ourMoney   → money sender pays to recipient
//   terms.theirMoney → money recipient pays to sender
// Moves one province between owners, keeping every structure that records
// ownership in step: the Province itself, m_provinceCountryLookup, the
// per-pixel country array and both countries' pixel lists. Getting any one
// of those wrong corrupts a save in a way that only shows up much later.
//
// Extracted from the ceasefire path so that the GameState.Write capability
// and the game's own territory transfers cannot drift apart. Both call this.
void Game::transferProvinceOwnership(int pid, int fromCid, int toCid) {
        Province* pp = m_provinces.getProvinceById(pid);
        if (!pp) return;
        if (pp->countryId != fromCid) return;
        pp->countryId = toCid;
        if ((size_t)pid < m_provinceCountryLookup.size())
            m_provinceCountryLookup[pid] = toCid;
        // Update per-pixel country array + move countryPixels
        auto ppIt = m_provincePixels.find(pid);
        if (ppIt != m_provincePixels.end()) {
            const auto& px = ppIt->second;
            for (int idx : px)
                if (idx >= 0 && idx < (int)m_pixelCountryArray.size())
                    m_pixelCountryArray[idx] = toCid;
            // One pass over the old owner's pixel list, not a full scan of it
            // per transferred pixel. The original nested erase was O(province
            // pixels x country pixels) — invisible while this was dead code
            // (the AI never sent terms, so nothing was ever ceded), but a
            // 10-20x per-turn slowdown the moment ceasefires actually moved
            // territory. Same shape as transferCountryPixels in
            // processShipDisembarks, which already did it this way.
            if (!m_aiTraining &&   // rendering-only, see processArmyMovement
                fromCid >= 0 && fromCid < (int)m_countryPixels.size() &&
                toCid >= 0 && toCid < (int)m_countryPixels.size()) {
                std::unordered_set<int> pxSet(px.begin(), px.end());
                auto& fp = m_countryPixels[fromCid];
                std::vector<int> moved;
                moved.reserve(px.size());
                auto newEnd = std::remove_if(fp.begin(), fp.end(), [&](int idx) {
                    if (pxSet.count(idx)) { moved.push_back(idx); return true; }
                    return false;
                });
                fp.erase(newEnd, fp.end());
                auto& tp = m_countryPixels[toCid];
                tp.insert(tp.end(), moved.begin(), moved.end());
            }
        }
}

void Game::applyCeasefireTerms(const std::string& sourceIso, const std::string& targetIso, const CeasefireTerms& terms, bool alreadyDeducted) {
    int srcCid = -1, tgtCid = -1;
    for (auto& [cid, c] : m_countries.getAll()) {
        if (c.isoA3 == sourceIso) srcCid = cid;
        if (c.isoA3 == targetIso) tgtCid = cid;
    }
    if (srcCid < 0 || tgtCid < 0) {
        printf("[CEASEFIRE] apply: bad ISOs %s/%s — skipping\n", sourceIso.c_str(), targetIso.c_str());
        return;
    }
    Country& srcC = m_countries.getAll()[srcCid];
    Country& tgtC = m_countries.getAll()[tgtCid];

    auto transferProvince = [&](int pid, int fromCid, int toCid) {
        transferProvinceOwnership(pid, fromCid, toCid);
        if (m_config.aiDebug)
            printf("[CEASEFIRE] province %d: %s -> %s\n", pid, sourceIso.c_str(), targetIso.c_str());
    };
    // Sender cedes ourProvs to recipient
    for (int pid : terms.ourProvs) {
        transferProvince(pid, srcCid, tgtCid);
        // Disband any troops belonging to the sender that remain in the ceded province
        auto aIt = m_provinceArmies.find(pid);
        if (aIt != m_provinceArmies.end()) {
            aIt->second.erase(std::remove_if(aIt->second.begin(), aIt->second.end(),
                [srcCid](auto& u) { return u.countryId == srcCid; }), aIt->second.end());
        }
    }
    // Recipient cedes theirProvs to sender
    for (int pid : terms.theirProvs) {
        transferProvince(pid, tgtCid, srcCid);
        auto aIt = m_provinceArmies.find(pid);
        if (aIt != m_provinceArmies.end()) {
            aIt->second.erase(std::remove_if(aIt->second.begin(), aIt->second.end(),
                [tgtCid](auto& u) { return u.countryId == tgtCid; }), aIt->second.end());
        }
    }

    auto dropClaim = [&](const std::string& claimantIso, int pid) {
        auto it = m_claims.find(claimantIso);
        if (it == m_claims.end()) return;
        it->second.erase(std::remove(it->second.begin(), it->second.end(), pid), it->second.end());
        auto bpIt = m_claimsByProvince.find(pid);
        if (bpIt != m_claimsByProvince.end()) {
            bpIt->second.erase(std::remove(bpIt->second.begin(), bpIt->second.end(), claimantIso), bpIt->second.end());
            if (bpIt->second.empty()) m_claimsByProvince.erase(pid);
        }
        printf("[CEASEFIRE] %s dropped claim on province %d\n", claimantIso.c_str(), pid);
    };
    for (int pid : terms.ourDropClaims) dropClaim(sourceIso, pid);
    for (int pid : terms.theirDropClaims) dropClaim(targetIso, pid);

    // Money transfer
    // If alreadyDeducted (player→AI offer: money already taken from player when
    // sending), only credit the target — don't double-deduct from source.
    if (terms.ourMoney > 0) {
        double amt = (double)terms.ourMoney;
        if (!alreadyDeducted) {
            amt = std::min(amt, srcC.treasury);
            srcC.treasury -= amt;
        }
        tgtC.treasury += amt;
        printf("[CEASEFIRE] %s pays %g to %s\n", sourceIso.c_str(), amt, targetIso.c_str());
    }
    if (terms.theirMoney > 0) {
        double amt = std::min((double)terms.theirMoney, tgtC.treasury);
        tgtC.treasury -= amt;
        srcC.treasury += amt;
        printf("[CEASEFIRE] %s pays %g to %s\n", targetIso.c_str(), amt, sourceIso.c_str());
    }

    // Labels refresh once per turn via m_labelsDirty (full-map scan otherwise
    // repeats for every ceasefire processed in the same turn)
    m_labelsDirty = true;
    if (m_renderer) m_renderer->setShowClaims(false);
    if (m_showClaims && m_playerCountryId > 0) {
        m_lastClaimsCountryId = m_playerCountryId;
        generateClaimsTexture();
    }
    // Re-render the political map texture so new borders show immediately.
    // Skipped during self-play: this rebuilds the whole 8192x4096 political
    // buffer and re-uploads it to the GPU, and it runs once per ceasefire that
    // moves territory. Profiling a training run put it at 79% of total runtime
    // once the AI actually started ceding provinces. processTurn already does
    // exactly one regeneration per turn, on the same !m_aiTraining condition,
    // and it runs after processDiplomaticRequests — so interactive play still
    // sees the new borders on the same turn.
    if (!m_aiTraining) generatePoliticalTexture();

    // The war is over, so nobody's troops may still be standing on the other's
    // soil. Done last, after every province transfer above, so ownership is
    // final when we decide what counts as foreign territory.
    withdrawArmiesAfterPeace(srcCid, tgtCid);
}

// === processUpgrades ===
void Game::processUpgrades() {
    // Process pending building upgrades
    for (auto it = m_pendingUpgrades.begin(); it != m_pendingUpgrades.end(); ) {
        it->turnsRemaining--;
        if (it->turnsRemaining <= 0) {
            auto indIt = m_provinceIndustry.find(it->provinceId);
            if (indIt != m_provinceIndustry.end()) {
                if (it->type == "industry") {
                    indIt->second.level = it->targetLevel;
                    indIt->second.income = it->targetLevel * 2.0f; // Simplified income
                } else if (it->type == "fortification") {
                    indIt->second.fortification = it->targetLevel;
                }
            }
            auto ptIt = m_provincePorts.find(it->provinceId);
            if (it->type == "port" && ptIt != m_provincePorts.end()) {
                ptIt->second.level = it->targetLevel;
            }
            it = m_pendingUpgrades.erase(it);
        } else ++it;
    }
    // Process pending specializations
    for (auto it = m_pendingSpecializations.begin(); it != m_pendingSpecializations.end(); ) {
        it->turnsRemaining--;
        if (it->turnsRemaining <= 0) {
            auto indIt = m_provinceIndustry.find(it->provinceId);
            if (indIt != m_provinceIndustry.end()) {
                indIt->second.specialization = it->specialization;
            }
            it = m_pendingSpecializations.erase(it);
        } else ++it;
    }
    // Process ship builds
    for (auto it = m_pendingShipBuilds.begin(); it != m_pendingShipBuilds.end(); ) {
        it->turnsRemaining--;
        if (it->turnsRemaining <= 0) {
            int w = m_landSea.getWidth(), h = m_landSea.getHeight();
            bool placed = false;
            auto ppIt = m_provincePixels.find(it->provinceId);
            if (ppIt != m_provincePixels.end()) {
                int dx[4] = {1,-1,0,0}, dy[4] = {0,0,1,-1};
                for (int idx : ppIt->second) {
                    if (placed) break;
                    int px = idx % w, py = idx / w;
                    for (int d = 0; d < 4; ++d) {
                        int nx = px + dx[d], ny = py + dy[d];
                        if (nx >= 0 && nx < w && ny >= 0 && ny < h && !m_landSea.isLand(nx, ny)) {
                            // Check water body size (≥200px = real ocean, not tiny lake)
                            std::unordered_set<int> bVis;
                            std::vector<int> bStk = {ny * w + nx};
                            bVis.insert(ny * w + nx);
                            int bCnt = 0;
                            while (!bStk.empty() && bCnt < 200) {
                                int bIdx = bStk.back(); bStk.pop_back();
                                bCnt++;
                                int bcy = bIdx / w, bcx = bIdx % w;
                                for (int bd = 0; bd < 4; ++bd) {
                                    int bnx = bcx + dx[bd], bny = bcy + dy[bd];
                                    if (bnx < 0) bnx = w - 1; else if (bnx >= w) bnx = 0;
                                    if (bny < 0 || bny >= h) continue;
                                    int bnIdx = bny * w + bnx;
                                    if (!bVis.count(bnIdx) && !m_landSea.isLand(bnx, bny)) {
                                        bVis.insert(bnIdx); bStk.push_back(bnIdx);
                                    }
                                }
                            }
                            if (bCnt >= 200) {
                                float lon, lat; m_landSea.pixelToLonLat(nx, ny, lon, lat);
                                auto pit = m_provinces.getProvinceById(it->provinceId);
                                NavyShip ns; ns.lon = lon; ns.lat = lat;
                                ns.type = it->type; ns.countryId = pit ? pit->countryId : UNC_CID;
                                ns.health = 100; // newly built ships at full health
                                m_ships.push_back(ns);
                                placed = true; break;
                            }
                        }
                    }
                }
            }
            it = m_pendingShipBuilds.erase(it);
        } else ++it;
    }
    // Process research accumulation (simplified: add points)
    if (m_playerCountryId > 0) {
        auto cs = computeCountryIncome(m_playerCountryId);
        float allocAmount = cs.researchCost;
        int rp = 1 + (int)(sqrtf(allocAmount * 0.5f));
        m_researchPoints += rp;
        if (m_researchPoints > 10000) m_researchPoints = 10000;
        // Apply to active research
        if (m_researchActiveNode >= 0 && m_researchActiveNode < (int)m_researchNodes.size()) {
            auto& node = m_researchNodes[m_researchActiveNode];
            if (!hasResearched(node.id, m_playerCountryId) && !node.researched) {
                int toSpend = std::min(m_researchPoints, node.cost - node.invested);
                node.invested += toSpend;
                m_researchPoints -= toSpend;
                if (node.invested >= node.cost) {
                    m_countryResearched[m_playerCountryId].insert(node.id);
                    node.researched = true;
                    printf("[RESEARCH] %s completed!\n", node.name.c_str());
                    m_researchActiveNode = -1;
                    m_researchAlert = true; // highlight the sidebar button until it's opened
                }
            } else {
                m_researchActiveNode = -1;
            }
        }
    }
}

// === processPopulation ===
void Game::processPopulation() {
    // Phase 1: compute unrest per province (border proximity + claims)
    std::unordered_map<int, float> provinceUnrest;
    for (auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId <= 0 || p.countryId == UNC_CID || p.countryId == BLC_CID) continue;
        float unrest = 0;
        auto nIt = m_provinceNeighbors.find(pid);
        if (nIt != m_provinceNeighbors.end()) {
            for (int nid : nIt->second) {
                Province* np = m_provinces.getProvinceById(nid);
                if (np && np->countryId != p.countryId && np->countryId > 0)
                    unrest += 2.0f;
            }
        }
        auto bpIt = m_claimsByProvince.find(pid);
        if (bpIt != m_claimsByProvince.end()) {
            for (auto& iso : bpIt->second) {
                for (auto& [cid2, c2] : m_countries.getAll()) {
                    if (c2.isoA3 == iso && cid2 != p.countryId) {
                        unrest += 5.0f;
                        break;
                    }
                }
            }
        }
        provinceUnrest[pid] = unrest;
    }

    // Phase 2: ethnic migration — minorities move toward economically better provinces
    float baseMigrationRate = 0.005f; // 0.5% base migration per turn
    float migrationBonus = getTotalEffect("migrationRate"); // research modifier (e.g., +0.01 = +1%)
    float migrationRate = baseMigrationRate * (1.0f + migrationBonus);

    // Compute refugee surge multiplier for recently conquered provinces
    std::unordered_map<int, float> refugeeSurge;
    std::vector<int> expiredConquests;
    for (auto& [pid, conquerTurn] : m_provinceConquestTurn) {
        int turnsSinceConquest = m_turnNumber - conquerTurn;
        if (turnsSinceConquest == 0) refugeeSurge[pid] = 15.0f;
        else if (turnsSinceConquest == 1) refugeeSurge[pid] = 8.0f;
        else if (turnsSinceConquest == 2) refugeeSurge[pid] = 3.0f;
        else if (turnsSinceConquest == 3) refugeeSurge[pid] = 1.5f;
        if (turnsSinceConquest > 3) expiredConquests.push_back(pid);
    }
    for (int pid : expiredConquests) m_provinceConquestTurn.erase(pid);

    // Compute economic attractiveness per province (income-based)
    std::unordered_map<int, float> provinceAttractiveness;
    float maxIncome = 1.0f;
    for (auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId <= 0) continue;
        auto indIt = m_provinceIndustry.find(pid);
        float income = 0;
        if (indIt != m_provinceIndustry.end())
            income = indIt->second.income + indIt->second.resourceIncome + indIt->second.popIncome;
        if (income < 0.1f) income = 0.1f;
        provinceAttractiveness[pid] = income;
        if (income > maxIncome) maxIncome = income;
    }
    // Normalize attractiveness to 0..1 range
    for (auto& [pid, attr] : provinceAttractiveness)
        attr /= maxIncome;

    // Per-country migration: for each country, move minority pops between provinces
    std::unordered_map<int, std::vector<int>> countryProvinces;
    for (auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId > 0 && p.countryId != UNC_CID && p.countryId != BLC_CID)
            countryProvinces[p.countryId].push_back(pid);
    }

    for (auto& [cid, pids] : countryProvinces) {
        if (cid == SPC_CID) continue;
        // Collect all minority groups present in this country
        struct MigrantGroup { std::string name; long long totalPop; int sourcePid; float attr; };
        std::vector<MigrantGroup> migrants;

        for (int srcPid : pids) {
            auto mit = m_provinceMinorities.find(srcPid);
            if (mit == m_provinceMinorities.end()) continue;
            long long srcPop = m_provincePopulations.count(srcPid) ? m_provincePopulations[srcPid] : 0;
            if (srcPop < 10000) continue; // need at least 10k pop for migration
            for (auto& mg : mit->second) {
                if (mg.pct < 5.0f) continue; // need at least 5% enclave
                long long minorityPop = (long long)(srcPop * mg.pct / 100.0f);
                if (minorityPop < 1000) continue; // need at least 1k minority individuals
                float unrest = provinceUnrest.count(srcPid) ? provinceUnrest[srcPid] : 0;
                float align = getMinorityAlignment(mg.name);
                float alignPush = (100.0f - align) / 50.0f; // 0 at 100% align, 2.0 at 0% align
                // Higher unrest + lower alignment = more emigration
                float surge = refugeeSurge.count(srcPid) ? refugeeSurge[srcPid] : 1.0f;
                if (surge > 1.0f)
                    printf("[DIAG] Refugee surge %.1fx in province %d for %s (align=%.0f%%)\n",
                           surge, srcPid, mg.name.c_str(), align);
                float emiRate = migrationRate * (1.0f + unrest / 50.0f + alignPush) * surge;
                long long emigrants = std::max(1LL, (long long)(minorityPop * emiRate));
                migrants.push_back({mg.name, emigrants, srcPid, provinceAttractiveness[srcPid]});
            }
        }

        if (migrants.empty()) continue;

        // Cache nearby provinces (within 2 graph steps, same country) per source
        std::unordered_map<int, std::vector<int>> nearbyWcCache;
        for (auto& mg : migrants) {
            if (nearbyWcCache.count(mg.sourcePid)) continue;
            std::unordered_map<int, int> bDist;
            std::vector<int> bQueue = {mg.sourcePid};
            bDist[mg.sourcePid] = 0;
            int bFront = 0;
            while (bFront < (int)bQueue.size()) {
                int cur = bQueue[bFront++];
                int d = bDist[cur];
                if (d >= 2) continue;
                auto nit = m_provinceNeighbors.find(cur);
                if (nit == m_provinceNeighbors.end()) continue;
                for (int nb : nit->second) {
                    if (bDist.count(nb)) continue;
                    Province* np = m_provinces.getProvinceById(nb);
                    if (!np || np->countryId != cid) continue;
                    bDist[nb] = d + 1;
                    bQueue.push_back(nb);
                }
            }
            for (auto& [pid, d] : bDist)
                if (pid != mg.sourcePid)
                    nearbyWcCache[mg.sourcePid].push_back(pid);
        }

        for (auto& mg : migrants) {
            const auto& nearbyPids = nearbyWcCache[mg.sourcePid];
            int bestDst = -1;
            float bestScore = -1e9;
            for (int dstPid : nearbyPids) {
                long long dstPop = m_provincePopulations.count(dstPid) ? m_provincePopulations[dstPid] : 0;
                if (dstPop < 1000) continue;
                float dstAttr = provinceAttractiveness.count(dstPid) ? provinceAttractiveness[dstPid] : 0;
                float chainBonus = 0;
                auto dMit = m_provinceMinorities.find(dstPid);
                if (dMit != m_provinceMinorities.end()) {
                    for (auto& dmg : dMit->second) {
                        if (dmg.name == mg.name) { chainBonus = 0.3f; break; }
                    }
                }
                float dstUnrest = provinceUnrest.count(dstPid) ? provinceUnrest[dstPid] : 0;
                // Unrest pull: minorities move toward contested zones to claim territory
                float unrestPull = dstUnrest * 0.05f;
                float score = (dstAttr - mg.attr) * 2.0f + chainBonus - dstUnrest * 0.01f + unrestPull;
                score += (rand() % 100) * 0.01f;
                if (score > bestScore) { bestScore = score; bestDst = dstPid; }
            }
            if (bestDst < 0 || bestScore <= 0) continue;

            // Execute migration: move people from source to destination
            long long srcPop = m_provincePopulations.count(mg.sourcePid) ? m_provincePopulations[mg.sourcePid] : 0;
            long long dstPop = m_provincePopulations.count(bestDst) ? m_provincePopulations[bestDst] : 0;
            float wcSurge = refugeeSurge.count(mg.sourcePid) ? refugeeSurge[mg.sourcePid] : 1.0f;
            long long moveCap = (wcSurge > 1.0f) ? srcPop / 2 : srcPop / 10; // 50% cap for refugees, 10% normally
            long long moveCount = std::min(mg.totalPop, moveCap);
            if (moveCount < 1) continue;

            if (m_config.aiDebug)
                printf("[MIGRATION] %lld %s within-country: province %d → %d\n",
                   moveCount, mg.name.c_str(), mg.sourcePid, bestDst);

            // Remove from source: reduce population
            m_provincePopulations[mg.sourcePid] = std::max(0LL, srcPop - moveCount);
            // Add to destination
            m_provincePopulations[bestDst] = dstPop + moveCount;

            // Recalculate ALL minority percentages at destination
            {
                long long newDstPop = dstPop + moveCount;
                auto dstMit = m_provinceMinorities.find(bestDst);
                bool found = false;
                if (dstMit != m_provinceMinorities.end()) {
                    for (auto& dmg : dstMit->second) {
                        long long absPop = (long long)(dstPop * dmg.pct / 100.0f);
                        if (dmg.name == mg.name) { absPop += moveCount; found = true; }
                        dmg.pct = (newDstPop > 0) ? (float)absPop / newDstPop * 100.0f : 0;
                    }
                }
                if (!found)
                    m_provinceMinorities[bestDst].push_back({mg.name, (float)moveCount / newDstPop * 100.0f});
            }
            // Recalculate ALL minority percentages at source
            {
                long long newSrcPop = m_provincePopulations[mg.sourcePid];
                auto sMit = m_provinceMinorities.find(mg.sourcePid);
                if (sMit != m_provinceMinorities.end()) {
                    for (auto& smg : sMit->second) {
                        long long absPop = (long long)(srcPop * smg.pct / 100.0f);
                        if (smg.name == mg.name) absPop -= moveCount;
                        smg.pct = (newSrcPop > 0) ? (float)absPop / newSrcPop * 100.0f : 0;
                    }
                    sMit->second.erase(std::remove_if(sMit->second.begin(), sMit->second.end(),
                        [](auto& g) { return g.pct <= 0; }), sMit->second.end());
                    if (sMit->second.empty()) m_provinceMinorities.erase(mg.sourcePid);
                }
            }
        }
    }

    // Phase 2b: Cross-border ethnic migration
    // Minorities migrate between countries at a reduced rate,
    // driven by unrest at source and economic opportunity at destination
    float crossBorderRate = baseMigrationRate * 0.2f * (1.0f + migrationBonus);
    float crossBorderRefugeeRate = baseMigrationRate * 1.5f * (1.0f + migrationBonus); // high rate for refugees fleeing conquered provinces
    for (auto& [cid, pids] : countryProvinces) {
        if (cid == SPC_CID) continue;

        // Compute per-country immigration boost from policies (once per country, not per province)
        std::unordered_map<int, float> immigBoostByCountry;
        for (auto& ap : m_activePolicies) {
            if (ap.countryId > 0 && ap.countryId == cid) continue;
            for (auto& p : m_allPolicies) {
                if (p.id == ap.policyId && p.effect.immigrationBoost > 0) {
                    immigBoostByCountry[ap.countryId] += p.effect.immigrationBoost;
                    break;
                }
            }
        }

        for (int srcPid : pids) {
            auto mit = m_provinceMinorities.find(srcPid);
            if (mit == m_provinceMinorities.end()) continue;
            long long srcPop = m_provincePopulations.count(srcPid) ? m_provincePopulations[srcPid] : 0;
            if (srcPop < 10000) continue;
            float srcUnrest = provinceUnrest.count(srcPid) ? provinceUnrest[srcPid] : 0;

            // Iterate a snapshot, not mit->second itself: the "recalculate at source"
            // block below erases from m_provinceMinorities[srcPid] (== mit->second) and
            // may erase the whole entry when it empties. Mutating/shrinking the vector
            // while a range-for holds a reference into it invalidates the loop — the
            // next read runs past the new size() (container-overflow) and occasionally
            // dereferences a garbage MinorityGroup string, the intermittent crash. The
            // within-country phase above is safe precisely because it decides over a
            // pre-collected list; do the same here.
            std::vector<MinorityGroup> srcGroups = mit->second;
            for (auto& mg : srcGroups) {
                if (mg.pct < 5.0f) continue;
                long long minorityPop = (long long)(srcPop * mg.pct / 100.0f);
                if (minorityPop < 1000) continue;

                // Low alignment lowers the unrest threshold for cross-border migration (refugee effect)
                float cbAlign = getMinorityAlignment(mg.name);
                float cbMinUnrest = (cbAlign < 30.0f) ? 1.0f : 5.0f;
                if (srcUnrest < cbMinUnrest) continue;

                int bestDst = -1;
                int bestDstCid = -1;
                float bestScore = -1e9;

                // Only consider destinations within 2 graph steps of source (local cross-border movement)
                std::unordered_map<int, int> cbDist;
                std::vector<int> cbQueue = {srcPid};
                cbDist[srcPid] = 0;
                int cbFront = 0;
                while (cbFront < (int)cbQueue.size()) {
                    int cur = cbQueue[cbFront++];
                    int d = cbDist[cur];
                    if (d >= 2) continue;
                    auto nit = m_provinceNeighbors.find(cur);
                    if (nit == m_provinceNeighbors.end()) continue;
                    for (int nb : nit->second) {
                        if (cbDist.count(nb)) continue;
                        cbDist[nb] = d + 1;
                        cbQueue.push_back(nb);
                    }
                }

                const Country* srcC = m_countries.getCountry(cid);
                if (!srcC) continue;

                for (auto& [dstPid, stepDist] : cbDist) {
                    if (dstPid == srcPid) continue;
                    Province* dp = m_provinces.getProvinceById(dstPid);
                    if (!dp) continue;
                    int cid2 = dp->countryId;
                    if (cid2 == cid || cid2 == SPC_CID || cid2 <= 0) continue;
                    const Country* dstC = m_countries.getCountry(cid2);
                    if (!dstC) continue;
                    auto ar = m_relations.find(srcC->isoA3);
                    bool atWar = false;
                    if (ar != m_relations.end()) {
                        auto dr = ar->second.find(dstC->isoA3);
                        if (dr != ar->second.end() && dr->second.war) atWar = true;
                    }
                    if (atWar) continue;

                    long long dstPop = m_provincePopulations.count(dstPid) ? m_provincePopulations[dstPid] : 0;
                    if (dstPop < 1000) continue;
                    float dstAttr = provinceAttractiveness.count(dstPid) ? provinceAttractiveness[dstPid] : 0;
                    float dstUnrest = provinceUnrest.count(dstPid) ? provinceUnrest[dstPid] : 0;

                    float chainBonus = 0;
                    auto dMit = m_provinceMinorities.find(dstPid);
                    if (dMit != m_provinceMinorities.end()) {
                        for (auto& dmg : dMit->second)
                            if (dmg.name == mg.name) { chainBonus = 0.5f; break; }
                    }

                    // 1-step neighbors get a bonus over 2-step
                    float stepBonus = (stepDist == 1) ? 1.5f : 0.5f;
                    float immigBoost = immigBoostByCountry.count(cid2) ? immigBoostByCountry[cid2] : 0;

                    // Unrest pull for cross-border: irredentist migration toward contested zones
                    float unrestPull = dstUnrest * 0.08f;
                    float score = (dstAttr - provinceAttractiveness[srcPid]) * 3.0f
                                  + chainBonus + stepBonus + immigBoost + unrestPull
                                  - dstUnrest * 0.02f
                                  + (rand() % 100) * 0.01f;

                    if (score > bestScore) { bestScore = score; bestDst = dstPid; bestDstCid = cid2; }
                }

                if (bestDst < 0 || bestScore <= 0) continue;

                long long dstPop = m_provincePopulations.count(bestDst) ? m_provincePopulations[bestDst] : 0;
                // Refugee surge: fleeing conquered/war-torn provinces at much higher rate
                float cbSurge = refugeeSurge.count(srcPid) ? refugeeSurge[srcPid] : 1.0f;
                if (cbSurge > 1.0f && cbAlign < 40.0f)
                    printf("[DIAG] Cross-border refugee surge %.1fx in province %d for %s\n",
                           cbSurge, srcPid, mg.name.c_str());
                float cbRate = (cbSurge > 1.0f && cbAlign < 40.0f) ? crossBorderRefugeeRate : crossBorderRate;
                long long moveCount = std::max(1LL, (long long)(minorityPop * cbRate));
                long long cbMoveCap = cbSurge > 1.0f ? srcPop / 2 : srcPop / 10;
                moveCount = std::min(moveCount, cbMoveCap);
                if (moveCount < 1) continue;

                // Execute cross-border move
                m_provincePopulations[srcPid] = std::max(0LL, srcPop - moveCount);
                m_provincePopulations[bestDst] = dstPop + moveCount;

                if (m_config.aiDebug)
                    printf("[MIGRATION] %lld %s cross-border: province %d (%s) → province %d (%s)\n",
                       moveCount, mg.name.c_str(), srcPid,
                       m_countries.getCountry(cid)->name.c_str(),
                       bestDst, m_countries.getCountry(bestDstCid)->name.c_str());

                // Recalculate ALL minority percentages at destination
                {
                    long long newDstPop = dstPop + moveCount;
                    auto dstMit = m_provinceMinorities.find(bestDst);
                    bool found = false;
                    if (dstMit != m_provinceMinorities.end()) {
                        for (auto& dmg : dstMit->second) {
                            long long absPop = (long long)(dstPop * dmg.pct / 100.0f);
                            if (dmg.name == mg.name) { absPop += moveCount; found = true; }
                            dmg.pct = (newDstPop > 0) ? (float)absPop / newDstPop * 100.0f : 0;
                        }
                    }
                    if (!found)
                        m_provinceMinorities[bestDst].push_back({mg.name, (float)moveCount / newDstPop * 100.0f});
                }
                // Recalculate ALL minority percentages at source
                {
                    long long newSrcPop = m_provincePopulations[srcPid];
                    auto sMit = m_provinceMinorities.find(srcPid);
                    if (sMit != m_provinceMinorities.end()) {
                        for (auto& smg : sMit->second) {
                            long long absPop = (long long)(srcPop * smg.pct / 100.0f);
                            if (smg.name == mg.name) absPop -= moveCount;
                            smg.pct = (newSrcPop > 0) ? (float)absPop / newSrcPop * 100.0f : 0;
                        }
                        sMit->second.erase(std::remove_if(sMit->second.begin(), sMit->second.end(),
                            [](auto& g) { return g.pct <= 0; }), sMit->second.end());
                        if (sMit->second.empty()) m_provinceMinorities.erase(srcPid);
                    }
                }
            }
        }
    }

    // Phase 3: compute stored unrest (for display)
    for (auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId <= 0 || p.countryId == UNC_CID || p.countryId == BLC_CID) continue;
        float unrest = provinceUnrest[pid];
        // Minority-based unrest
        float minorityUnrest = 0;
        auto mit = m_provinceMinorities.find(pid);
        if (mit != m_provinceMinorities.end()) {
            for (auto& mg : mit->second) {
                float align = getMinorityAlignment(mg.name);
                if (align < 40.0f)
                    minorityUnrest += mg.pct * 0.3f * (1.0f - align / 40.0f);
            }
        }
        unrest += minorityUnrest;
    }
}

