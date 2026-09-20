#include <cstdlib>
#include "Game.h"
#include "MinorityShares.h"
#include "util/LoadLog.h"
#include "ai/MoneyLedger.h"
#include "PoliticalIdentity.h"
#include "Audio.h"
#include "GameInternals.h"
#include "mods/ModManager.h"
#include "Keybinds.h"
#include "SaveManager.h"
#include "ai/AISystem.h"
#include "raymath.h"
#include <bit>          // std::popcount -- MSVC has no __builtin_popcount
#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>
#include <fstream>
#include <sstream>
#include <random>
#include <cstdio>
#include <deque>
#include <set>
#include <unordered_set>

// ─── The simulation's own randomness ─────────────────────
//
// This file used bare rand(), which is a PROCESS-GLOBAL, non-thread-safe
// generator -- and Audio::randUnit() draws from the same one, from a detached
// background pump thread. Two threads pulling on one stream desynchronise it by
// however the scheduler interleaves them, so identical inputs produced
// different games: about one run in four, always from the same turn, with three
// or more distinct outcomes, and it DISAPPEARED under tracing because the extra
// printf changed the timing. A Heisenbug, and the reason three-run determinism
// checks kept passing.
//
// Seeding rand() per map came first and could not fix this: a second thread on
// the same stream ruins it however it was seeded. The fix is to stop sharing.
//
// File-static rather than a Game member on purpose -- it is turn-resolution
// state, not public API, and Game.h is not the place to put it.
static std::mt19937 g_simRng{1337};
static int simRand() { return (int)(g_simRng() >> 1); }   // non-negative, like rand()
void seedSimRng(unsigned int seed) { g_simRng.seed(seed); }
int simRandShared() { return simRand(); }

// ── SAVING THE STREAM, NOT JUST THE SEED ──
//
// A world's seed says where its RNG started; it does not say where the RNG has
// GOT TO, and by turn fifty those are very different things. Restoring only the
// seed on load would rewind the stream to turn zero, so a reloaded game would
// diverge from the one that was saved -- which is the same class of complaint
// as a save that does not reproduce, and would undo the point of seeding at all.
//
// mt19937 streams its whole state through the standard operators, so the honest
// thing is to write that down. It is a few hundred bytes of text in state.json.
std::string simRngState() {
    std::ostringstream os;
    os << g_simRng;
    return os.str();
}
void setSimRngState(const std::string& st) {
    if (st.empty()) return;
    // Into a SPARE generator first, and only adopted if it parsed. Reading a
    // truncated or corrupt state straight into g_simRng half-consumes it and
    // leaves the world's randomness in whatever state the failure stopped at,
    // which is neither the saved stream nor a clean one. A save that cannot be
    // read here keeps the seed it was already given.
    std::mt19937 parsed;
    std::istringstream is(st);
    if (is >> parsed) g_simRng = parsed;
}

// === isProvinceCoastal ===
bool Game::isProvinceCoastal(int pid) const {
    auto cached = m_coastalCache.find(pid);
    if (cached != m_coastalCache.end()) return cached->second;

    auto it = m_provincePixels.find(pid);
    if (it == m_provincePixels.end()) return false;   // not cached: no province yet

    // Every return below this point goes through here, so the walk happens once
    // per province per map rather than once per frame.
    struct Memo {
        std::unordered_map<int, bool>& cache;
        int pid;
        bool value = false;
        ~Memo() { cache[pid] = value; }
    } memo{m_coastalCache, pid};
    auto answer = [&](bool v) { memo.value = v; return v; };
    int w = m_landSea.getWidth();
    int h = m_landSea.getHeight();

    // EVERY water body this province touches, not just the first one found.
    //
    // This used to take the first adjacent water pixel in pixel order and
    // measure only that body. A province touching both a lagoon and the open
    // sea was therefore judged by whichever the raster order happened to reach
    // first: hit the lagoon, and the province was declared land-locked with the
    // Mediterranean along its other edge. Which one came first was an accident
    // of pixel layout, so the result looked arbitrary and was.
    //
    // Measured on the 1939 map, that mistake covered 127 provinces -- 9 of them
    // French, 22 British, 21 American -- each one a coastline where the player
    // could see the sea and the game refused a port. It was reported as exactly
    // that.
    //
    // So: gather the water pixels around the province, and ask whether ANY of
    // the bodies they belong to is big enough to be a sea.
    static const int MIN_WATER_BODY = 75;
    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};

    std::vector<int> seeds;
    for (int idx : it->second) {
        int px = idx % w;
        int py = idx / w;
        for (int d = 0; d < 4; ++d) {
            int nx = px + dx[d];
            int ny = py + dy[d];
            if (nx >= 0 && nx < w && ny >= 0 && ny < h && !m_landSea.isLand(nx, ny))
                seeds.push_back(ny * w + nx);
        }
    }
    if (seeds.empty()) return answer(false);

    // Pixels already accounted for by a body that turned out to be TOO SMALL.
    // Seeds inside one are skipped, so each pond is measured once however many
    // of the province's pixels touch it. A body that reaches the threshold ends
    // the search immediately, so nothing is gained by recording those.
    std::unordered_set<int> knownSmall;

    for (int seed : seeds) {
        if (knownSmall.count(seed)) continue;

        std::vector<int> stack{seed};
        std::unordered_set<int> visited{seed};
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
        if (count >= MIN_WATER_BODY) return answer(true);   // open water: coastal
        knownSmall.insert(visited.begin(), visited.end());
    }
    return answer(false);
}

// === portAnchor ===
Vector2 Game::portAnchor(int pid) const {
    auto cached = m_portAnchorCache.find(pid);
    if (cached != m_portAnchorCache.end()) return cached->second;

    Vector2 centre{0, 0};
    auto cit = m_provinceCenters.find(pid);
    if (cit != m_provinceCenters.end()) centre = cit->second;

    auto it = m_provincePixels.find(pid);
    if (it == m_provincePixels.end()) {
        m_portAnchorCache[pid] = centre;
        return centre;
    }

    const int w = m_landSea.getWidth();
    const int h = m_landSea.getHeight();

    // Three things decide the anchor, in this order.
    //
    // DRY LAND first. A province's pixels are not all land: an archipelago
    // province covers the water between its islands too, and those pixels are
    // both closer to the centroid and more surrounded by sea than any real
    // coast is. Ranking on openness alone therefore moored Norway, Greece and
    // Japan in a strait offshore of themselves -- the same floating anchor
    // this function exists to stop, arrived at by a different route. A harbour
    // is on land. Only a province with no land at all in the raster, which at
    // this resolution means an island smaller than five kilometres across, may
    // have its anchor in the water.
    //
    // OPENNESS second, because a harbour on a puddle is not a harbour. Counting
    // the water in a 7x7 box separates a pixel on the coast of a sea from a
    // pixel beside a pond, and from a pixel in a notch where the ship icon
    // would be drawn over rock. It is deliberately a cheap local count and not
    // another flood fill: isProvinceCoastal already owns the expensive question
    // of whether this province has a sea at all.
    //
    // DISTANCE last, so that among equally good candidates the anchor goes to
    // the one nearest the middle of the province -- where a player looking at
    // that province expects to find it, rather than out at whichever cape
    // happens to face the most water.
    const int RING = 3;              // 7x7
    const int OPEN_ENOUGH = 8;       // of 48 neighbours
    long long bestScore = -1;
    float bestDist = 0.0f;
    Vector2 best = centre;

    for (int idx : it->second) {
        const int px = idx % w;
        const int py = idx / w;

        bool touchesWater = false;
        const int dx[4] = {1, -1, 0, 0};
        const int dy[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4 && !touchesWater; ++d) {
            const int nx = px + dx[d];
            const int ny = py + dy[d];
            if (nx >= 0 && nx < w && ny >= 0 && ny < h && !m_landSea.isLand(nx, ny))
                touchesWater = true;
        }
        if (!touchesWater) continue;

        int open = 0;
        for (int oy = -RING; oy <= RING; ++oy) {
            const int ny = py + oy;
            if (ny < 0 || ny >= h) continue;
            for (int ox = -RING; ox <= RING; ++ox) {
                if (ox == 0 && oy == 0) continue;
                const int nx = px + ox;
                if (nx < 0 || nx >= w) continue;
                if (!m_landSea.isLand(nx, ny)) open++;
            }
        }

        const float ddx = (float)px - centre.x;
        const float ddy = (float)py - centre.y;
        const float dist = ddx * ddx + ddy * ddy;
        // Land outranks any amount of openness, so it is a whole digit above
        // it rather than a tie-break within it.
        const long long score = (m_landSea.isLand(px, py) ? 100 : 0)
                              + ((open >= OPEN_ENOUGH) ? OPEN_ENOUGH : open);
        if (score > bestScore || (score == bestScore && dist < bestDist)) {
            bestScore = score;
            bestDist = dist;
            best = {(float)px, (float)py};
        }
    }

    // No pixel of this province touches water at all: land-locked, and the
    // centre is the only honest answer. The map data should not be putting a
    // port here, and tools/check_map_naval.py says so, but drawing the anchor
    // somewhere is better than drawing it at (0,0).
    m_portAnchorCache[pid] = best;
    return best;
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
    // The wheel's type ids are short; the recordings are named for the weapon.
    auto artySfx = [](const std::string& type) -> const char* {
        if (type == "mortar")     return "mortar";
        if (type == "light")      return "light_artillery";
        if (type == "heavy")      return "heavy_artillery";
        if (type == "napalm")     return "napalm";
        if (type == "carpet")     return "carpet_bombing";
        if (type == "chemical")   return "chemical_artillery";
        if (type == "nuclear")    return "nuclear_shelling";
        if (type == "biological") return "biological_shelling";
        return nullptr;
    };
    // Capped, and only for strikes the player is an end of. Every country on a
    // 185-country map resolves its artillery in this same pass, and hearing all
    // of it would be a wall of noise that says nothing about your own turn.
    int artyHeard = 0;

    for (size_t i = 0; i < m_pendingArtilleryOrders.size(); ) {
        auto& ao = m_pendingArtilleryOrders[i];
        Province* srcP = m_provinces.getProvinceById(ao.fromProvince);
        if (!srcP || srcP->countryId != countryId) { ++i; continue; }
        Province* tgtP = m_provinces.getProvinceById(ao.targetProvince);
        if (!tgtP) { ++i; continue; }
        ArtyEffect eff = getEffect(ao.ammoType);

        if (artyHeard < 3 && (countryId == m_playerCountryId ||
                              tgtP->countryId == m_playerCountryId)) {
            if (const char* sfx = artySfx(ao.ammoType)) {
                // Jittered: a battery firing four times is four reports, not one
                // report played four times.
                Audio::get().playSfx(sfx, 0.06f);
                ++artyHeard;
            }
        }

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
                if (eff.fortChance > 0 && (simRand() % 100) < eff.fortChance) dmg += 1;
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
// ─── Province ownership index ────────────────────────────────────────────

void Game::rebuildCountryProvinceIndex() {
    // Clear the vectors rather than the map: the country set barely changes
    // between turns, so this keeps the per-country capacity and stops every
    // turn from re-allocating a few hundred small vectors.
    for (auto& [cid, list] : m_countryProvinces) list.clear();
    for (auto& [pid, prov] : m_provinces.getAllProvinces())
        if (prov.countryId > 0) m_countryProvinces[prov.countryId].push_back(pid);
}

void Game::reindexProvinceOwner(int pid, int oldOwner, int newOwner) {
    if (oldOwner == newOwner) return;
    // The gradient that shades each country inward from its coastline and
    // borders is a distance field, and a distance field computed for the OLD
    // borders is wrong the moment a province changes hands. It was only ever
    // rebuilt on load, sync and replay -- never in play -- so after a conquest
    // the shading still faded toward a border that no longer existed, and the
    // 1px dark line in generatePoliticalTexture() was added to paper over it.
    // Mark it here, where every owner change in the game already passes.
    m_gradientDirty = true;
    if (oldOwner > 0) {
        auto it = m_countryProvinces.find(oldOwner);
        if (it != m_countryProvinces.end()) {
            auto& v = it->second;
            auto p = std::find(v.begin(), v.end(), pid);
            // Swap-and-pop: order carries no meaning here, and erase() from
            // the middle of a big country's list would be O(its provinces)
            // on every single conquest.
            if (p != v.end()) { *p = v.back(); v.pop_back(); }
        }
    }
    if (newOwner > 0) m_countryProvinces[newOwner].push_back(pid);
}

const std::vector<int>& Game::provincesOf(int cid) const {
    static const std::vector<int> none;
    auto it = m_countryProvinces.find(cid);
    return it == m_countryProvinces.end() ? none : it->second;
}

// Counted so a change to the sea graph can be judged on whether it STRANDS
// HULLS, not on whether the routes it does produce look better. A stricter
// graph that quietly makes a fifth of all voyages unroutable is a worse game
// than a loose one that sends them over Crimea.
long long g_navRouteCalls = 0, g_navRouteFails = 0;

void Game::processTurn() {
    // ── OD_SEED_KINDS=1: PUT THE OTHER KINDS INTO A WORLD THAT CANNOT RAISE
    //    THEM YET ──
    //
    // A player can pick a troop kind; the AI cannot, because its recruit action
    // has no kind on it (that is the AI session's half). So a normal eval raises
    // 100% line infantry and every combat column added in stage 3 goes
    // unexercised -- which is indistinguishable from a system that does not
    // work.
    //
    // This assigns kinds round-robin to the starting garrisons, once, so the
    // frontage, attack/defence and fuel columns actually run. It is a
    // DEVELOPMENT AID, not a game rule: off unless asked for, read once on the
    // first turn, and it makes no decision any player or AI would make.
    // Measured with it on: survival 75.5% against 73.6% all-line, 18,077
    // assaults, 33 battles, supply biting at 19.8% -- so the columns fire and
    // the game is stable with mixed armies.
    if (m_turnNumber <= 1 && std::getenv("OD_SEED_KINDS")) {
        int k = 0;
        for (auto& [pid, units] : m_provinceArmies)
            for (auto& u : units)
                u.type = (TroopType)(1 + (k++ % (int)(TROOP_TYPE_COUNT - 1)));
    }

    // A new turn: forget every supply route. captureProvince invalidates the
    // two countries whose ground moved, which covers the common case within a
    // turn; this covers everything else that quietly changes a route -- a port
    // finished, an industry level raised, an alliance signed or broken -- none
    // of which is worth its own invalidation call. One cleared map per turn is
    // nothing; a stale one is a stack fighting on last month's supply line.
    m_supplyCache.clear();
    m_seaSupplyCache.clear();   // and where the fleets were

    // Lambda to draw a loading screen frame (no-op if loading screen not active)
    auto drawFrame = [&](float pct, const char* status) {
        // BEFORE the early-out, and that ordering is the whole point.
        //
        // Feeding the audio used to happen inside setLoadingProgress(), below
        // this line -- so a turn processed WITHOUT the loading screen fed it
        // exactly never. On the web that means the browser's audio callback
        // never gets the thread back for the length of the turn, and the
        // "process turn" sound that plays at the start of it repeats for as
        // long as the turn takes. The sound is not looping; it is the last
        // buffer being replayed because nothing drained it.
        //
        // Whether a progress bar is on screen has nothing to do with whether
        // the speakers need feeding.
        Audio::get().pump();
        if (!m_showLoadingScreen) return;
        setLoadingProgress(pct, status);
        BeginDrawing();
        ClearBackground(BLACK);
        drawLoadingScreen();
    };
    Audio::get().playSfx("process_turn");
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
    // Ownership index first: refreshIncomeCache and every per-country pass
    // below read it, and a turn's worth of conquests can leave last turn's
    // splices slightly behind on the rare path that writes ownership without
    // reindexing. One O(provinces) scan a turn buys that back.
    rebuildCountryProvinceIndex();
    // Pre-compute all country incomes in a single province pass (avoids 356 redundant scans)
    refreshIncomeCache();
    m_rebellionsThisTurnByCid.clear();
    // Country AI: created lazily on the first processed turn so map load stays
    // instant; the model persists across saves in the game data directory.
    if (!m_ai) m_ai = new AISystem(this, m_dataDir + m_aiModelPath);
    // ── "AI LEARNING: OFF" MUST ALSO MEAN "DO NOT WRITE THE MODEL" ──
    //
    // aiLearning gates the learning itself -- AISystem::endTurn returns early
    // without it -- but it did NOT gate saving. saveModel() is called from the
    // destructor and from a timer every SAVE_INTERVAL_SECONDS with only
    // s_readOnlyModel to stop it, and the game binary set that nowhere. So a
    // player with learning switched off still had data/ai/model.bin rewritten
    // on every quit: nothing learned, and the trained model overwritten anyway
    // by whatever the session happened to hold.
    //
    // Set every turn rather than once at construction, because the setting is
    // a toggle in Experimental and a player may turn it off mid-game -- which
    // is exactly when they mean it.
    AISystem::s_readOnlyModel = !m_config.aiLearning;
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
        // Every country, not every batch. The progress bar only needs redrawing
        // occasionally; the audio needs feeding continuously, and one country's
        // turn on a large map is already long enough to be heard. pump()
        // rate-limits itself, so the extra calls cost nothing.
        Audio::get().pump();
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
    // After the diplomacy, because that is what changes who has standing
    // where: the ceasefire that strands a stack and the withdrawal that
    // rescues it land on the same turn. See repatriateStrandedArmies.
    repatriateStrandedArmies();
    // After the requests, so a claim made this turn gets its full window, and
    // beside decayWarWeariness below because it is the same kind of thing: the
    // world slowly letting go of what happened.
    ageCredibility();
    auto t4 = std::chrono::steady_clock::now();
    drawFrame(0.55f, "Processing population...");
    processPopulation();
    auto t5 = std::chrono::steady_clock::now();
    drawFrame(0.58f, "Updating policies...");
    updatePolicies();
    decayWarWeariness();
    decayRebellionCooldowns();
    auto t6 = std::chrono::steady_clock::now();
    drawFrame(0.60f, "Eliminating defeated countries...");
    eliminateDefeatedCountries();
    // Last, so both halves of the question are settled for the turn: who owns
    // each province, and who is still at war with whom.
    expelStrandedArmies();
    auto t7 = std::chrono::steady_clock::now();
    drawFrame(0.62f, "Generating political map...");
    // Self-play training never looks at the map: skip the full-raster texture
    // and label passes, they dominate turn time on big maps.
    //
    // ASKED FOR, NOT DONE HERE. The orders phase is decided at the bottom of
    // this function and has to be able to hold the old borders on screen while
    // it runs; see m_politicalRepaintPending.
    if (!m_aiTraining) m_politicalRepaintPending = true;
    // One label rebuild per turn no matter how many rebellions/ceasefires
    // marked them dirty — computeCountryLabels is a full-map raster scan.
    if (m_labelsDirty) {
        m_labelsDirty = false;
        if (!m_aiTraining) m_labelRepaintPending = true;
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
    delta.researchActiveNode = m_researchGroups[0].activeNode;
    for (int g = 0; g < 3; ++g) {
        delta.researchGroups[g].activeNode  = m_researchGroups[g].activeNode;
        delta.researchGroups[g].lastNode    = m_researchGroups[g].lastNode;
        delta.researchGroups[g].sharePct    = m_researchGroups[g].sharePct;
        delta.researchGroups[g].autoAdvance = m_researchGroups[g].autoAdvance;
    }
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
            const int prevOut = hadInd ? prevIndIt->second.output : -1;
            const int curOut  = hasInd ? curIndIt->second.output  : -1;
            if (prevOut != curOut) { pd.outputChanged = true; pd.newOutput = curOut; changed = true; }
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
        for (auto& u : units) ad.units.push_back({u.countryId, u.count, (uint8_t)u.type});
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
    // OD_OJH_NET=<clients>: report, for Objective Judge Horizon, the bytes this turn puts
    // on the wire in a multiplayer game -- the packed delta the host broadcasts to each
    // client (Game::mpResolveTurn), times the number of clients. Packing costs time, so it
    // only happens when asked and never during a turn-speed run.
    if (const char* ojhNet = std::getenv("OD_OJH_NET")) {
        const long clients = std::max(1L, std::strtol(ojhNet, nullptr, 10));
        const size_t packed = SaveManager::packTurn(delta).size();
        printf("OJH data 0 %zu\n", packed * (size_t)clients);
        fflush(stdout);
    }
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
        // TIMED, and reported below with the rest.
        //
        // The [TURN] line used to stop measuring before this point, so the one
        // phase that grew with the length of the game was the one phase nobody
        // could see -- and it grew until the window stopped answering and the
        // game was reported as freezing on Process Turn. Whatever the save
        // costs, it says so now.
        const auto ts0 = std::chrono::steady_clock::now();
        SaveManager::appendTurn(m_currentSavePath, delta, &stateJson, &rebelFiles);
        m_lastSaveMs = (float)(std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - ts0).count() * 1000.0);
        if (m_config.aiDebug)
            printf("[TURN] save=%.0fms (%s)\n", m_lastSaveMs, m_currentSavePath.c_str());
        m_turnCount++;
    }
    drawFrame(0.90f, "Cleaning up...");
    // WHAT IT HELD WHEN THIS TURN BEGAN, which is the figure a country is
    // allowed to publish about itself -- never the live number, because a
    // profile that updated mid-turn would let anyone watch a treasury drain in
    // real time. Taken before anything this turn spends it.
    for (const auto& [cid, c] : m_countries.getAll()) m_treasuryLastTurn[cid] = c.treasury;

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

    // ── The post ──
    //
    // AFTER the number advances, so a letter written on turn 10 is stamped as
    // arriving on 11 and the "a letter takes a turn" promise is literally true
    // in the data rather than only in the interface. Before the scripts tick,
    // so a map script can read what arrived this turn. See src/Mail.h.
    deliverMail();
    // Advisors answer AFTER the post, so they are replying to what actually
    // arrived. Their replies become ordinary pending letters and leave on the
    // next turn like everybody else's -- no speed advantage over a person.
    runAdvisors();

    // ── CHAT'S TURN, IF A CHANNEL IS READING ──
    //
    // A poll that has not been resolved by the time the turn does is applied
    // now rather than abandoned: a vote chat cast is a vote chat should see
    // land, and carrying it to the next turn would apply it to a board that has
    // moved. Opening the next one happens after the turn resolves, below.
    if (chatPlaysActive() && m_chatPoll.closesAt() > 0.0) applyChatWinner();
    // And the next one opens against the board as it now is: options built
    // before the turn resolved would name countries that may no longer border
    // this one.
    if (chatPlaysActive()) openChatVote();
    writeOverlayFeed();   // OBS reads these files; see stream/OverlayFeed.h

    // ── AND NO DISADVANTAGE EITHER ──
    //
    // "No speed advantage" was the intention; what the code did was slower than
    // that. runAdvisors ASKS here, and its answer was collected by the NEXT
    // turn's runAdvisors and only then written as a pending letter -- so it
    // left a turn after that. Ledger for a letter written on turn 1:
    //
    //   turn 1 resolves  letter delivered (turn 2), advisor asked
    //   turn 2 resolves  answer collected, written pending for turn 4
    //   turn 3 resolves  answer delivered           -> visible on turn 4
    //
    // A PERSON answering that same letter writes during turn 2 and it lands on
    // turn 3. The advisor was a full turn slower than the rule it was supposed
    // to be obeying, and slower again whenever the model missed a turn -- which
    // is how a reply to turn 1 arrived on turn 5.
    //
    // So the answer is collected in the SAME resolution that asked for it: wait
    // for the workers, post what came back, and it goes out as a pending letter
    // stamped exactly as a person's would be. Visible on turn 3, which is
    // parity -- not an advantage.
    //
    // BOUNDED, AND SKIPPED WHERE THERE IS NOBODY TO WAIT FOR. A stalled model
    // must never stop a turn resolving; headless self-play and the eval never
    // wait at all.
    if (!m_aiTraining && llmSettling()) {
        setLoadingProgress(0.0f, T("Waiting for replies..."));
        waitForAdvisors(kAdvisorTurnWait, [this](float p) {
            setLoadingProgress(p, T("Waiting for replies..."));
        });
        collectAdvisorAnswers();
    }

    // Resume any map scripts suspended on waitUntil now that turn/date advanced
    if (m_scriptEngine) {
        m_scriptEngine->tick();
        if (!m_scriptEngine->getErrors().empty()) m_scriptErrorTimer = 3.0f;
    }
    // ── AND NOW THE MIDDLE OF THE GAME ──
    //
    // The turn has resolved and m_turnOrderLog holds what every country did to
    // bring that about. Entering the phase here rather than offering a button
    // is the difference between a beat in the loop and a lens: it is the shape
    // GD4 uses, and it is the moment the information is worth anything.
    //
    // Not entered when there is nothing to show, when the player has said they
    // do not want it, or in any headless run -- a benchmark has nobody to press
    // continue and would sit in the phase for ever.
    // EVERY AUTOMATED DRIVER IS EXCLUDED, and the list is the point rather than
    // an afterthought: a phase that waits for a person is a hang for anything
    // that has no person. `m_aiTraining` covers --train-ai and --eval-ai,
    // `m_walk` the tutorial walk, and a screenshot tour is identified by its
    // output directory. Each of these presses the turn button and nothing
    // else; none of them would ever press Continue.
    const bool somebodyIsWatching =
        !m_aiTraining && !m_walk && m_shotDir.empty() && m_playerCountryId > 0;
    m_turnState = (somebodyIsWatching && !m_config.skipViewingOrders && !m_turnOrderLog.empty())
                      ? TURN_VIEWING_ORDERS : TURN_NORMAL;

    if (getenv("OD_RCAP") && (m_turnNumber % 40) == 0) dumpResearchCapacity();
    if (getenv("OD_ORDERLOG") && (m_turnNumber % 10) == 0) {
        int byKind[5] = {};
        for (const auto& m : m_turnOrderLog) byKind[(int)m.kind]++;
        printf("[ORDERLOG] turn %d: %zu marks  arty %d  move %d  ship %d  recruit %d  build %d\n",
               m_turnNumber, m_turnOrderLog.size(), byKind[0], byKind[1], byKind[2],
               byKind[3], byKind[4]);
    }
    if (getenv("OD_NAV_STATS") && (m_turnNumber % 20) == 0)
        printf("[NAVROUTE] turn %d: %lld call(s), %lld unroutable (%.1f%%)\n",
               m_turnNumber, g_navRouteCalls, g_navRouteFails,
               g_navRouteCalls ? 100.0 * (double)g_navRouteFails / (double)g_navRouteCalls : 0.0);

    // ── AND THE LAND CATCHES UP NOW, UNLESS THE PHASE NEEDS IT NOT TO ──
    //
    // Deferring this to the draw loop was right for the orders phase and wrong
    // for every other turn: the turn returned in a blink and then the game sat
    // on a black screen for seconds regenerating the map with nothing drawn.
    // Reported as two things -- "process turn feels slow" and "a few seconds of
    // black screen" -- which are the same event seen from either end.
    //
    // The loading screen is still up here, so this costs no second screen and
    // lands exactly where it used to: inside the turn, behind its progress bar.
    // When the phase IS coming the note is left for Continue to honour, which
    // is the one case that has to wait.
    if (m_turnState != TURN_VIEWING_ORDERS) {
        drawFrame(0.98f, "Generating political map...");
        flushMapRepaint();
    }

    // GameProcess post-turn hook. This also services a reload that was asked
    // for mid-turn: reloads happen between turns, never inside one.
    ModManager::get().postTurn(m_turnNumber);

    // ── THE BENCHMARK ENDS ITSELF ──
    //
    // A person's seat has to stop on the same turn the model's did or the two
    // numbers are not the same measurement, and "please stop at 120" is not a
    // protocol. Same arithmetic the eval prints: the seat's share of every
    // province anybody owns. See Game::startBenchSeat.
    if (m_benchPlayUntilTurn > 0 && m_turnNumber >= m_benchPlayUntilTurn) {
        long long mine = 0, owned = 0;
        for (int owner : m_provinceCountryLookup) {
            if (owner <= 0 || owner >= REBEL_CID_MIN) continue;
            ++owned;
            if (owner == m_playerCountryId) ++mine;
        }
        const double share = owned ? 100.0 * (double)mine / (double)owned : 0.0;
        const Country* pc = m_countries.getCountry(m_playerCountryId);
        printf("[BENCH] seat %s  score %.1f  (share of the world held after "
               "%d turns)\n", pc ? pc->isoA3.c_str() : "?", share,
               m_benchPlayUntilTurn);
        fflush(stdout);
        m_benchScoreShare = (float)share;
        m_benchPlayUntilTurn = 0;   // report once
    }

    drawFrame(1.0f, "Done!");
    if (m_config.aiDebug) printf("[TURN] Turn %d processed.\n", turnNum);

    // DETERMINISM TRACE. OD_DET_TRACE=1 prints a hash of the world after every
    // turn, so two runs of the same seed can be diffed to find the exact turn
    // they first disagree -- which is the only cheap way to localise a
    // divergence in a 400-turn simulation.
    //
    // Every accumulation below is COMMUTATIVE (+= into a sum, never a sequence
    // mix) wherever it walks an unordered container. A hash that depends on
    // iteration order would report a divergence whenever the containers merely
    // rehashed differently, which is exactly the false positive that would send
    // someone hunting a bug that is not there.
    static const bool detTrace = std::getenv("OD_DET_TRACE") != nullptr;
    if (detTrace) {
        uint64_t owners = 0, armies = 0, money = 0;
        for (size_t pid = 0; pid < m_provinceCountryLookup.size(); ++pid)
            owners += (uint64_t)(pid + 1) * 1000003ULL *
                      (uint64_t)(m_provinceCountryLookup[pid] + 1);
        for (const auto& [pid, units] : m_provinceArmies)
            for (const auto& u : units)
                armies += (uint64_t)(pid + 1) * 7919ULL +
                          (uint64_t)(u.countryId + 1) * 104729ULL + (uint64_t)u.count;
        for (const auto& [cid, c] : m_countries.getAll())
            money += (uint64_t)(cid + 1) * 31ULL + (uint64_t)(int64_t)llround(c.treasury);
        printf("[DET] turn=%d owners=%llu armies=%llu money=%llu\n", turnNum,
               (unsigned long long)owners, (unsigned long long)armies,
               (unsigned long long)money);
        fflush(stdout);
    }

    // Resource limiter. The frame cap in Settings > Display does nothing for
    // this loop -- it is single-threaded, runs as hard as the machine allows,
    // and during self-play it IS the CPU load. Idling for a share of the work
    // just done is what actually leaves the machine usable. No-op at 100%.
    m_lastTurnMs = (float)(std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - t0).count() * 1000.0);
    throttleForBudget(std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - t0).count(),
                      // Self-play has nobody waiting on it, so it can idle as
                      // long as the budget actually asks for.
                      m_aiTraining ? 60.0 : 1.0);
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
    // THE ONLY MOMENT A COUNTRY'S WHOLE TURN IS ON THE TABLE. It has decided,
    // and nothing it decided has been consumed yet. See m_turnOrderLog.
    // Where this country's pacification money goes. A reflex, not a decision
    // the policy net takes -- see updateAIDistricts.
    if (!getenv("OD_AI_DISTRICTS_OFF")) updateAIDistricts(countryId);
    // What it is willing to say about itself, on the same terms the player has.
    if (!getenv("OD_AI_DISCLOSE_OFF")) updateAIDisclosure(countryId);
    // And the law its districts run, which is the part of them that works
    // without a pacification budget. See updateAIDistrictLaws.
    if (!getenv("OD_AI_DLAW_OFF")) updateAIDistrictLaws(countryId);

    recordTurnOrders(countryId);
    // How often wartime diplomacy was AVAILABLE, against how often anybody used
    // it. The gap is the point: the ask existed only for the player and only
    // through a button, so a rule that could have fired five hundred times in a
    // world fired three. Same instrument as [WIDTH], [SUPPLY] and [BATTLES] --
    // an opportunity nobody takes is indistinguishable from a rule that is not
    // there, and only counting tells them apart.
    if (!callableFriends(countryId).empty()) ++m_callableFriendTurns;
    auto pt1 = std::chrono::steady_clock::now();
    processArtilleryOrders(countryId);
    auto pt2 = std::chrono::steady_clock::now();
    processShipBombardOrders(countryId);
    auto pt3 = std::chrono::steady_clock::now();
    processShipDisembarks(countryId);
    auto pt4 = std::chrono::steady_clock::now();
    // BEFORE this turn's movement, and the order matters. Withdrawals resolve
    // first (inside this), then every standing battle fights a round, and only
    // then do new orders arrive -- so a reinforcement joins the fight and is
    // counted in NEXT turn's round rather than being thrown at the line the
    // moment it arrives, and a fresh assault still resolves the turn it is
    // ordered. See Game::processBattles.
    processBattles(countryId);
    processCampaigns();   // close what succeeded, timed out or ran dry
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
    // These two were timed as one segment labelled "embark", which made the
    // most expensive phase of the country turn unattributable.
    processEmbarkations(countryId);
    auto pt10 = std::chrono::steady_clock::now();
    processRebellions(countryId);
    auto pt11 = std::chrono::steady_clock::now();
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
    te = (long long)std::chrono::duration_cast<std::chrono::microseconds>(pt11-pt10).count();
    if (te > 1000) printf("[PROCESS] cid=%d rebellion=%lldus\n", countryId, te);
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
        // The flag as a PATTERN, which is the only form that survives. The
        // rebellion/<cid>.svg written alongside this is a flat rectangle of the
        // primary colour -- flagPatternToSvg does not draw stripes or symbols --
        // and nothing ever rasterised it back anyway, so a reloaded breakaway
        // had no flag at all.
        e["flag_actual"] = nlohmann::json::parse(flagPatternToJsonString(c.flagActual));
        e["flag_censored"] = nlohmann::json::parse(flagPatternToJsonString(c.flagCensored));
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
            // Rasterise the flag, exactly as createRebelCountry does at the
            // moment of the rebellion. Loading the record alone left the
            // texture map with no entry for the cid, so every restored
            // breakaway state showed an empty grey rectangle where its flag
            // should be -- in the country panel and everywhere else.
            // ...but not without a GPU to put it on. FlagRenderer::render ends
            // in LoadTextureFromImage, and --export-timelapse runs with no
            // window and therefore no GL context, so the call lands on a null
            // function pointer and the process dies at address zero. The SVG is
            // kept either way; a headless GIF export never draws a flag.
            const Country* rc = m_countries.getCountry(cid);
            if (rc && !m_headless) {
                auto fit = m_countryFlags.find(cid);
                if (fit != m_countryFlags.end() && fit->second.id > 0)
                    UnloadTexture(fit->second);
                // m_dataDir: a rebel flag carries a symbol too, and an empty
                // baseDir is what stops FlagRenderer finding the SVG for it.
                m_countryFlags[cid] = FlagRenderer::render(rc->flagActual, 256, 128, m_dataDir, &m_odmJsonData);
            }
        }
        LoadLog() << "  Restored " << j.size() << " rebel countries" << std::endl;
    } catch (...) { LoadLog() << "  Failed to parse rebels.json" << std::endl; }
}

void Game::rebuildIsoIndex() {
    m_isoToCid.clear();
    for (auto& [cid, c] : m_countries.getAll())
        if (!c.isoA3.empty()) m_isoToCid[c.isoA3] = cid;
}

// ─── Credibility ─────────────────────────────────────────────────────────
//
// Tuned to be slow. One caught lie should be a thing a player notices and
// remembers, not a thing that ends a relationship; and forgiveness has to be
// slower than lying, or the cheapest strategy is to lie constantly and wait.
static constexpr float CRED_HIT_CAUGHT   = 0.35f;  // said something already disprovable
    /**
     * A broken TREATY. A non-aggression pact was, until 2026-09-04, free to
     * break: declareWar cleared it silently and break_nap charged nothing,
     * while a false STATEMENT about intentions cost CRED_HIT_CAUGHT. So the
     * game punished lying and not betrayal, and an AI that refused every
     * pact was playing correctly -- there was nothing to lose by refusing and
     * nothing to gain by signing. Priced at the caught-lie rate: a signed
     * treaty is at least as much of a promise as a spoken claim.
     */
    static constexpr float CRED_HIT_PACT = CRED_HIT_CAUGHT;
static constexpr float CRED_HIT_CONDUCT  = 0.25f;  // conduct disproved it later
static constexpr float CRED_RECOVER      = 0.004f; // per turn, back toward trust
static constexpr int   CRED_CLAIM_WINDOW = 25;     // turns a claim can be broken in

void Game::noteRefusalStatement(const std::string& speakerIso,
                                const std::string& hearerIso, int statedReason) {
    if (statedReason == REFUSE_NONE) return;   // nothing claimed, nothing to lose
    const int sp = cidForIso(speakerIso), he = cidForIso(hearerIso);
    if (sp < 0 || he < 0) return;
    // Already disprovable when it was said. The AI never does this to itself;
    // a player may, and pays for it on the spot.
    if (refusalIsContradicted(he, sp, statedReason)) {
        loseCredibility(speakerIso, hearerIso, CRED_HIT_CAUGHT);
        return;
    }
    // Unfalsifiable today, and conduct may still disprove it. See SpokenClaim.
    if (statedReason == REFUSE_WEARINESS)
        recordSpokenClaim(speakerIso, hearerIso, SpokenClaim::CLAIM_WEARY);
}

void Game::noteWarGoalStatement(const std::string& attackerIso,
                                const std::string& defenderIso, int statedGoal) {
    if (statedGoal == WAR_GOAL_NONE) return;
    const int att = cidForIso(attackerIso), def = cidForIso(defenderIso);
    if (att < 0 || def < 0) return;
    if (warGoalIsContradicted(def, att, def, statedGoal)) {
        loseCredibility(attackerIso, defenderIso, CRED_HIT_CAUGHT);
        return;
    }
    // "To recover what is ours" is true today by construction -- a claim exists
    // -- and becomes a lie the moment the war goes past it.
    if (statedGoal == WAR_GOAL_RECONQUEST)
        recordSpokenClaim(attackerIso, defenderIso,
                          SpokenClaim::CLAIM_RECONQUEST, defenderIso);
}

float Game::credibility(const std::string& speakerIso,
                        const std::string& hearerIso) const {
    auto s = m_credibility.find(speakerIso);
    if (s == m_credibility.end()) return 1.0f;
    auto h = s->second.find(hearerIso);
    return h == s->second.end() ? 1.0f : h->second;
}

void Game::loseCredibility(const std::string& speakerIso,
                           const std::string& hearerIso, float amount) {
    if (speakerIso.empty() || hearerIso.empty() || speakerIso == hearerIso) return;
    float& c = m_credibility[speakerIso].emplace(hearerIso, 1.0f).first->second;
    c = std::max(0.0f, c - amount);
    m_credibilityHits++;
    m_credibilityLow = std::min(m_credibilityLow, c);
    // Only ever announced to the player, and only when it is the player being
    // lied to. Learning that somebody's story did not hold up is the entire
    // payoff of the mechanic; burying it in a log would waste it.
    const Country* pc = m_countries.getCountry(m_playerCountryId);
    if (pc && pc->isoA3 == hearerIso)
        addNotification(diploDisplayName(speakerIso) +
                        " has been caught out — their word counts for less",
                        Color{225, 175, 90, 255}, 8.0f);
}

void Game::recordSpokenClaim(const std::string& speakerIso,
                             const std::string& hearerIso, int kind,
                             const std::string& aboutIso) {
    if (speakerIso.empty() || hearerIso.empty() || speakerIso == hearerIso) return;
    m_openClaims.push_back({speakerIso, hearerIso, kind, m_turnNumber, aboutIso});
}

void Game::claimsBrokenByDeclaration(const std::string& speakerIso) {
    for (auto it = m_openClaims.begin(); it != m_openClaims.end(); ) {
        if (it->kind == SpokenClaim::CLAIM_WEARY && it->speakerIso == speakerIso) {
            // Told them the country could not face a war, and then started one.
            loseCredibility(it->speakerIso, it->hearerIso, CRED_HIT_CONDUCT);
            it = m_openClaims.erase(it);
        } else {
            ++it;
        }
    }
}

void Game::claimsBrokenByConquest(int winnerCid, int loserCid, int provinceId) {
    const Country* w = m_countries.getCountry(winnerCid);
    const Country* l = m_countries.getCountry(loserCid);
    if (!w || !l) return;
    // Was this province ever ours to recover? The claim is checked BEFORE the
    // conquest grants one back to the previous owner, which is why this is
    // called from the same place the transfer happens.
    bool claimedByUs = false;
    auto cl = m_claimsByProvince.find(provinceId);
    if (cl != m_claimsByProvince.end())
        for (const auto& iso : cl->second)
            if (iso == w->isoA3) { claimedByUs = true; break; }
    if (claimedByUs) return;   // exactly what they said they were doing

    for (auto it = m_openClaims.begin(); it != m_openClaims.end(); ) {
        if (it->kind == SpokenClaim::CLAIM_RECONQUEST &&
            it->speakerIso == w->isoA3 && it->aboutIso == l->isoA3) {
            // A war of recovery that has just taken land it never claimed.
            loseCredibility(it->speakerIso, it->hearerIso, CRED_HIT_CONDUCT);
            it = m_openClaims.erase(it);
        } else {
            ++it;
        }
    }
}

void Game::ageCredibility() {
    // Claims nobody can reasonably be held to any longer.
    m_openClaims.erase(
        std::remove_if(m_openClaims.begin(), m_openClaims.end(),
                       [&](const SpokenClaim& c) {
                           return m_turnNumber - c.madeOnTurn > CRED_CLAIM_WINDOW;
                       }),
        m_openClaims.end());
    // ...and slow forgiveness. Deliberately far slower than a single lie costs,
    // so a country cannot simply out-wait its own reputation.
    for (auto& [speaker, byHearer] : m_credibility)
        for (auto& [hearer, c] : byHearer)
            if (c < 1.0f) c = std::min(1.0f, c + CRED_RECOVER);
}

int Game::statedWarGoal(const std::string& attackerIso,
                        const std::string& defenderIso) const {
    auto a = m_relations.find(attackerIso);
    if (a == m_relations.end()) return WAR_GOAL_NONE;
    auto d = a->second.find(defenderIso);
    return d == a->second.end() ? WAR_GOAL_NONE : d->second.warGoalStated;
}

bool Game::warGoalIsContradicted(int observerCid, int attackerCid,
                                 int defenderCid, int goal) const {
    const Country* att = m_countries.getCountry(attackerCid);
    const Country* def = m_countries.getCountry(defenderCid);
    if (!att || !def) return false;
    (void)observerCid;   // all of this is public; nobody sees a different map

    switch (goal) {
        case WAR_GOAL_RECONQUEST: {
            // Claims are public. Announcing a war of recovery against a country
            // holding nothing you claim is a story the map refuses.
            for (int pid : provincesOf(defenderCid)) {
                auto it = m_claimsByProvince.find(pid);
                if (it == m_claimsByProvince.end()) continue;
                for (const auto& iso : it->second)
                    if (iso == att->isoA3) return false;
            }
            return true;
        }
        case WAR_GOAL_SECURITY: {
            // Only believable from someone they can actually reach: a shared
            // border, or a coast each with the sea between.
            for (int pid : provincesOf(attackerCid)) {
                auto nIt = m_provinceNeighbors.find(pid);
                if (nIt == m_provinceNeighbors.end()) continue;
                for (int nid : nIt->second) {
                    const int owner = (nid >= 0 && nid < (int)m_provinceCountryLookup.size())
                                          ? m_provinceCountryLookup[nid] : 0;
                    if (owner == defenderCid) return false;
                }
            }
            return true;   // not even neighbours
        }
        case WAR_GOAL_HUMBLE: {
            // "They have grown too large" from someone larger than they are is
            // not a reason, it is an admission. Judged on land, which is drawn.
            const size_t mine = provincesOf(attackerCid).size();
            const size_t theirs = provincesOf(defenderCid).size();
            return theirs <= mine;
        }
        case WAR_GOAL_ALLY: {
            // Checkable: is anyone we are allied to actually at war with them.
            auto rel = m_relations.find(att->isoA3);
            if (rel == m_relations.end()) return true;
            for (const auto& [iso, r] : rel->second) {
                if (!r.alliance && !r.guarantee) continue;
                auto their = m_relations.find(iso);
                if (their == m_relations.end()) continue;
                auto w = their->second.find(def->isoA3);
                if (w != their->second.end() && w->second.war) return false;
            }
            return true;
        }
        // "We want your land" cannot be disproved by anybody. The honest goal
        // is the safe one to state, which is a pleasing thing for the game to
        // be true about.
        case WAR_GOAL_CONQUEST:
        case WAR_GOAL_NONE:
        default:
            return false;
    }
}

void Game::tellRefusal(const std::string& toIso, int statedReason) {
    const int toCid = cidForIso(toIso);
    if (toCid < 0 || m_playerCountryId <= 0) return;
    const Country* pc = m_countries.getCountry(m_playerCountryId);
    if (pc) noteRefusalStatement(pc->isoA3, toIso, statedReason);
    if (m_ai) m_ai->noteRefusalHeard(toCid, m_playerCountryId, statedReason);
    if (m_config.aiDebug)
        printf("[DIPLO] Player -> %s: %s\n", toIso.c_str(),
               refusalText(statedReason) ? refusalText(statedReason) : "(no reason given)");
}

bool Game::refusalIsContradicted(int observerCid, int subjectCid, int reason) const {
    const Country* subj = m_countries.getCountry(subjectCid);
    if (!subj) return false;

    // Everything the observer would have to look at is on the map or in the
    // relations table, both of which it can already read. Nothing here consults
    // anything private -- that is the point, and it is why the three reasons
    // that describe an internal state or a preference can never be caught.
    switch (reason) {
        case REFUSE_OWN_WARS: {
            // Wars are public. Claiming to be busy while at peace is checkable.
            auto rel = m_relations.find(subj->isoA3);
            if (rel == m_relations.end()) return true;
            int wars = 0;
            for (const auto& [iso, r] : rel->second) {
                if (!r.war) continue;
                const int ocid = cidForIso(iso);
                if (ocid >= 0 && ocid < REBEL_CID_MIN) wars++;
            }
            return wars == 0;
        }
        case REFUSE_LOSING_GROUND: {
            // Also public: is there an enemy stack next to anything they own.
            for (int pid : provincesOf(subjectCid)) {
                auto nIt = m_provinceNeighbors.find(pid);
                if (nIt == m_provinceNeighbors.end()) continue;
                for (int nid : nIt->second) {
                    const int owner = (nid >= 0 && nid < (int)m_provinceCountryLookup.size())
                                          ? m_provinceCountryLookup[nid] : 0;
                    if (owner <= 0 || owner == subjectCid) continue;
                    auto rel = m_relations.find(subj->isoA3);
                    if (rel == m_relations.end()) continue;
                    const Country* oc = m_countries.getCountry(owner);
                    if (!oc) continue;
                    auto rr = rel->second.find(oc->isoA3);
                    if (rr != rel->second.end() && rr->second.war) return false;
                }
            }
            return true;   // nobody is anywhere near them
        }
        case REFUSE_OUTGUNNED: {
            // Roughly checkable: garrisons are drawn on the map. A country
            // pleading weakness while visibly twice the observer's size is not
            // believed. Deliberately generous -- the observer is estimating.
            long long mine = 0, theirs = 0;
            auto armyOf = [&](int cid) {
                long long n = 0;
                for (int pid : provincesOf(cid)) {
                    auto it = m_provinceArmies.find(pid);
                    if (it == m_provinceArmies.end()) continue;
                    for (const auto& u : it->second)
                        if (u.countryId == cid) n += u.count;
                }
                return n;
            };
            theirs = armyOf(subjectCid);
            mine = armyOf(observerCid);
            return theirs > mine * 2;
        }
        // Private state and preferences. A country's war weariness, its
        // interests and whom it trusts are not on anybody else's map, so these
        // cannot be caught out -- which is exactly what makes them the useful
        // things to say when the truth is something you would rather not admit.
        case REFUSE_WEARINESS:
        case REFUSE_NO_INTEREST:
        case REFUSE_DISTRUST:
        case REFUSE_NONE:
        default:
            return false;
    }
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
    LoadLog() << "  Synthesized " << missing.size()
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
namespace {


}  // namespace

void Game::createRebelCountry(int rebelCid, int parentCid,
                              const std::vector<int>& provinceIds, bool peaceful) {
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
    // i18n-ignore: builds the English name; the display forms live in Locale.cpp
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
    // The parent's UNSTYLED name, for building a breakaway's own.
    //
    // Country::name may now carry a political form -- "Autocracy of
    // Afghanistan" once its government went far enough (PoliticalIdentity.h) --
    // and naming a secession off that produced "Republic of Central Autocracy
    // of Afghanistan". The root is what a new state on the same ground would
    // actually name itself after.
    auto parentRootName = [&](int cid) -> std::string {
        auto& all = m_countries.getAll();
        auto it = all.find(cid);
        if (it == all.end()) return std::string();
        const std::string raw = it->second.rootSaved && !it->second.rootName.empty()
                              ? it->second.rootName : it->second.name;
        // And without its form of government: a secession from the "Kingdom of
        // Italy" names itself after Italy, not after the kingdom.
        const std::string core = politid::geographicCoreOf(raw);
        if (core.empty()) return raw;
        // ...and as a PLACE, not an adjective. "French Republic" cores to
        // "French", which is right for "the French army" and wrong for
        // "Republic of Eastern French" -- and this name is only ever used as
        // the second kind.
        if (std::string place = politid::properPlaceName(core); !place.empty()) return place;
        return core;
    };

    // The country a demonym is ALREADY the demonym OF, if it is on this map.
    //
    // This is what stops the invention. "Mexican" must not become "Mexicia"
    // while Mexico is sitting right there as the parent -- and it did, for
    // 11.9% of all breakaway names measured over 1,375 of them: Mexicia,
    // Canadia, Brazilia, Norwegia, Belarusia, Greekia. Each was returned
    // precisely BECAUSE it was invented: a made-up stem collides with nothing,
    // so it passed the conflict check that the real name would have failed, and
    // the directional fallback that exists for exactly this case never ran.
    //
    // Returning the real name instead puts it back on that path -- the caller
    // conflict-checks it, finds the parent, and asks for a direction. "Northern
    // Mexico" is both correct and the thing such a state would actually call
    // itself.
    //
    // Matched on a PREFIX, because English builds demonyms by mangling the
    // tail and no suffix rule survives it: Mexico/Mexican share "Mexic",
    // Canada/Canadian share "Canad", Norway/Norwegian share "Norw", Greece/
    // Greek share "Gree". Four characters is enough to be the same place, and
    // long enough that Chad does not marry Chadic.
    auto existingReferent = [&](const std::string& r) -> std::string {
        if (r.size() < 4) return std::string();
        std::string rl = r;
        std::transform(rl.begin(), rl.end(), rl.begin(), ::tolower);
        std::string best; size_t bestN = 0;
        for (auto& [cid, c] : m_countries.getAll()) {
            if (cid == rebelCid || c.name.empty()) continue;
            // The GEOGRAPHIC core: a country that has renamed itself "People's
            // Republic of Mexico" is still the country Mexican refers to.
            const std::string core = politid::geographicCoreOf(
                c.rootSaved && !c.rootName.empty() ? c.rootName : c.name);
            if (core.size() < 4) continue;
            std::string cl = core;
            std::transform(cl.begin(), cl.end(), cl.begin(), ::tolower);
            size_t n = 0;
            while (n < cl.size() && n < rl.size() && cl[n] == rl[n]) ++n;
            if (n >= 4 && n > bestN) { bestN = n; best = core; }
        }
        return best;
    };

    auto deriveTerritory = [&](const std::string& ethnicity) -> std::string {
        std::string root = ethnicity;
        if (root.empty()) return ethnicity;

        // THE LAST WORD IS THE DEMONYM; everything before it says which group,
        // not which place. Minority names arrive qualified -- "Mestizo
        // Mexican", "English Canadian", "White Brazilian", "Han Chinese" -- and
        // suffixing the whole string is what produced "Mestizo Mexicia" and
        // "Han Chin", the two most common breakaway names on the map. A state
        // is named for its ground, so the ground is what the rules run on.
        if (auto sp = root.find_last_of(' '); sp != std::string::npos)
            root = root.substr(sp + 1);
        // Strip trailing 's' (e.g., "Assyrians" → "Assyrian")
        if (!root.empty() && root.back() == 's') root.pop_back();
        if (root.empty()) return ethnicity;

        // The irregulars first, because no rule below reaches them, and they are
        // independent of who is on the map -- which is what makes this work in
        // 1914, where the referent lookup finds no Canada because Canada is
        // still a dominion.
        if (std::string place = politid::properPlaceName(root); !place.empty()) return place;

        // Then the map itself: a demonym whose country is already on it.
        //
        // Only when the place already exists: "Assyrian" finds no Assyria on the
        // map and still becomes Assyria below, which is right -- inventing is
        // only wrong when the real answer was available and ignored.
        if (std::string ref = existingReferent(root); !ref.empty()) return ref;

        bool endsInI = root.back() == 'i';
        bool endsInN = root.back() == 'n';
        bool endsInE = root.back() == 'e';
        bool endsInAn = root.size() >= 3 && root.substr(root.size()-2) == "an";
        bool endsInIn = root.size() >= 3 && root.substr(root.size()-2) == "in";
        bool endsInIan = root.size() >= 4 && root.substr(root.size()-3) == "ian";
        bool endsInIsh = root.size() >= 4 && root.substr(root.size()-3) == "ish";
        bool endsInEse = root.size() >= 4 && root.substr(root.size()-3) == "ese";
        // French, Dutch, Welsh. These are demonyms that no suffix rule can turn
        // into a country -- France and the Netherlands are not derivable from
        // them -- and leaving them out of the demonym test let the generic trial
        // below return the word itself, because its suffix list contains "" and
        // "French" does not literally contain "France". That shipped states
        // called "Socialist Republic of French" and "West French".
        bool endsInCh  = root.size() >= 4 && root.substr(root.size()-2) == "ch";
        bool endsInSh  = root.size() >= 4 && root.substr(root.size()-2) == "sh";

        // Helper: add suffix avoiding doubled vowels
        auto addSuffix = [&](const std::string& base, const std::string& sfx) -> std::string {
            if (base.empty()) return sfx;
            if (sfx.empty()) return base;
            char lastB = tolower(base.back());
            char firstS = tolower(sfx.front());
            // If last letter of base equals first letter of suffix, drop one
            if (lastB == firstS) return base + sfx.substr(1);
            // A vowel-initial suffix on a vowel-final base collides: Korea+ia
            // is "Koreia", Chinese+ia is "Chineseia", Cape Verde+ia is "Cape
            // Verdeia". English drops the base vowel instead -- Korea+ia is
            // just Korea -- so the suffix adds nothing and the caller should
            // move on to a directional form rather than take this.
            auto isVowel = [](char ch) {
                return ch=='a'||ch=='e'||ch=='i'||ch=='o'||ch=='u';
            };
            if (isVowel(lastB) && isVowel(firstS)) return std::string();
            return base + sfx;
        };

        // Try suffixes in shuffled order until a non-conflicting name emerges
        std::vector<int> suffixOrder = {0,1,2,3,4,5};
        for (size_t si = 0; si < suffixOrder.size(); ++si) {
            size_t j = si + simRand() % (suffixOrder.size() - si);
            std::swap(suffixOrder[si], suffixOrder[j]);
        }

        // For endsInIan (Austrian → Austria, Russian → Russia): strip "ian" + "ia"
        if (endsInIan) {
            std::string t = root.substr(0, root.size()-3) + "ia";
            if (!nameConflicts(t)) return t;
            // Also try just dropping the 'n' (Russian → Russia)
            t = root.substr(0, root.size()-1);
            if (!nameConflicts(t)) return t;
            // NO "-ia" -> "-a" fallback. It produced Lithuanian -> "Lithuana"
            // and Bulgarian -> "Bulgara", which are not words in any language
            // and read as a typo rather than a country. When the real name is
            // taken -- and it usually is, by the parent -- falling through to
            // the directional republic below is the honest answer.
        }
        // For endsInAn (Assyrian → Assyria): strip "an" + "ia" (but only if not also endsInIan which we already tried)
        if (endsInAn && !endsInIan) {
            // Through addSuffix, not raw concatenation: "Korean" minus "an" is
            // "Kore", and "Kore" + "ia" is "Koreia". addSuffix refuses a
            // vowel-on-vowel join, which is what makes that come out empty and
            // fall through to a directional name instead.
            std::string t = addSuffix(root.substr(0, root.size()-2), "ia");
            if (!t.empty() && !nameConflicts(t)) return t;
            // No "-a" fallback: "Assyra" is not a fallback, it is a typo.
        }
        // For endsInIn (Montenegrin → Montenegro): strip "in" + "o"
        if (endsInIn) {
            std::string t = root.substr(0, root.size()-2) + "o";
            if (!nameConflicts(t)) return t;
        }
        // For endsInI (Punjabi → Punjab): drop the 'i'.
        //
        // Drop it, and stop. This used to be `base + "ab"`, which is not what
        // its own comment said and not what English does: "Punjabi" minus 'i'
        // is already "Punjab", so adding "ab" produced "Punjabab", and
        // "Hindustani" produced "Hindustanab". Measured at 2% of breakaway
        // names. Dropping the 'i' alone gives Punjab, Hindustan and Bengal.
        if (endsInI) {
            std::string t = root.substr(0, root.size()-1);
            if (!nameConflicts(t)) return t;
        }
        // For endsInish (English → England): strip "ish" + "land"
        //
        // Through addSuffix, not raw concatenation. Half the demonyms this fires
        // on end in the letter "land" begins with: "English" minus "ish" is
        // "Engl", and "Engl" + "land" is "Englland". Measured over one training
        // run: Polland x504, Orlland x504, Brelland x314, Englland x138.
        // addSuffix already collapses a doubled letter, which is exactly what
        // turns those back into England and Poland.
        if (endsInIsh) {
            std::string t = addSuffix(root.substr(0, root.size()-3), "land");
            if (!t.empty() && !nameConflicts(t)) return t;
        }
        // For endsInEse (Japanese → Japan): strip "ese" + leave as root base
        if (endsInEse) {
            // The bare stem FIRST. Japanese -> Japan, not Japana: this had the
            // two the wrong way round, so the invented form won whenever it was
            // free, which it always was.
            std::string base = root.substr(0, root.size()-3);
            if (!nameConflicts(base)) return base;
            // and nothing else. Japanese -> Japan; if Japan is taken, the
            // answer is a direction on Japan, not "Japana".
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

        // The generic trial bolts a suffix onto whatever it is given, and what
        // it is given here is a DEMONYM. "Albanian" + "istan" is
        // "Albanianistan"; "Bulgarian" + "stan" is "Bulgarianstan"; "Croatian"
        // + "istan" is "Croatianistan". None of those is a place.
        //
        // The demonym cases above already know the right transformation for
        // each ending, so if they did not produce a free name it is because the
        // real country name is taken -- almost always by the parent this is
        // seceding from. Inventing a misspelling is the wrong answer to that.
        const bool isDemonym = endsInIan || endsInAn || endsInIsh ||
                               endsInEse || endsInI || endsInIn ||
                               endsInCh || endsInSh;
        if (!isDemonym) {
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
        }

        // Last resort: the REAL country name with a direction on it, which is
        // what a state in this position calls itself -- there are two Koreas
        // and neither is spelled wrong. Built from the properly-formed name
        // even though that name is taken, because the direction is what makes
        // it distinct.
        // Built with the same joins as the branches above, for the same reason:
        // this path is where "Western Englland" came from. A country reached by
        // the last resort is still a country and still has to be spelled like
        // one -- a direction on a misspelling is just a longer misspelling.
        //
        // Where addSuffix refuses a join (a vowel meeting a vowel) the properly
        // formed name is the demonym without its final "n" -- Korean -> Korea,
        // Russian -> Russia -- which is what English actually does and what the
        // -ian branch above already tries first.
        // Empty, NOT root. Seeding this with the demonym meant the guard below
        // could never fire -- "French" was already sitting in `proper`, so
        // "nothing could be derived" looked exactly like "the answer is French",
        // and the last resort put a direction on it.
        std::string proper;
        auto joinOrDropN = [&](size_t strip) {
            std::string t = addSuffix(root.substr(0, root.size() - strip), "ia");
            if (!t.empty()) return t;
            return root.back() == 'n' ? root.substr(0, root.size() - 1) : root;
        };
        if (endsInIan)      proper = joinOrDropN(3);
        else if (endsInAn)  proper = joinOrDropN(2);
        else if (endsInIsh) proper = addSuffix(root.substr(0, root.size() - 3), "land");
        else if (endsInEse) proper = root.substr(0, root.size() - 3);
        else if (endsInIn)  proper = root.substr(0, root.size() - 2) + "o";
        // A demonym nothing could turn into a place name is not a place name.
        //
        // Falling back to the word itself is how "West French" happened: a
        // direction on an adjective. Returning nothing hands the decision to the
        // caller, which names the state after the country it is actually
        // breaking away FROM -- which is what such a state is.
        if (proper.empty()) {
            if (isDemonym) return std::string();
            proper = root;
        }
        proper = stripDirection(proper);
        if (!proper.empty() && !nameConflicts(proper)) return proper;

        // i18n-ignore: builds the English name; the display forms live in Locale.cpp
        static const char* lastDirs[] = {"Northern ", "Southern ", "Eastern ",
                                         "Western ", "Central "};
        for (int d = 0; d < 5; ++d) {
            std::string t = std::string(lastDirs[(simRand() + d) % 5]) + proper;
            if (!nameConflicts(t)) return t;
        }
        return proper.empty() ? root : proper;
    };

    auto makeDirectionalName = [&](const std::string& base, const std::string& parentName) -> std::string {
        // i18n-ignore: builds the English name; the display forms live in Locale.cpp
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
        int di = simRand() % 5;
        return std::string(dirs[di]) + place;
    };

    if (hasEthnicBasis) {
        // Step 1: See if the ethnicity name itself is too close to an existing country
        // (e.g., "Japanese" → Japan exists, "Georgian" → Georgia exists)
        std::string parentName = parentRootName(parentCid);
        std::string territory = deriveTerritory(ethnicName);

        // Step 2: if territory conflicts with existing countries, try directional
        //
        // Empty counts as conflicting. deriveTerritory now returns nothing
        // rather than hand back a bare demonym, and makeDirectionalName reads an
        // empty base as "use the parent" -- so a Breton rising in France becomes
        // "North France" instead of "West French".
        if (territory.empty() || nameConflicts(territory)) {
            territory = makeDirectionalName(territory, parentName);
        }

        // Step 3: wrap with ideology prefix/suffix (50% plain, 25% prefix, 25% suffix)
        int nr = simRand() % 100;
        if (nr < 50) {
            countryName = territory;
        } else if (nr < 75) {
            if (avgEcon < -20) countryName = "People's Republic of " + territory;
            else if (avgEcon > 30) countryName = "Free State of " + territory;
            else if (avgSoc < -20) countryName = "State of " + territory;
            else countryName = "Republic of " + territory;
        } else {
            // A state, not a campaign. "Liberation Front" and "Independence
            // Movement" name the thing that FOUGHT for a country; they are not
            // what the country is called once it exists.
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* genSuffixes[] = {" Republic", " Union", " Federation", " State"};
            int si = simRand() % 4;
            countryName = territory + genSuffixes[si];
        }

        // Final conflict check: if still conflicting, fall back to directional
        if (nameConflicts(countryName)) {
            countryName = makeDirectionalName(territory, parentName);
        }
    } else {
        // Region-based naming — try directional first if parent country has many provinces
        std::string parentName = parentRootName(parentCid);
        std::string nameBase = regionName;
        std::string baseName;

        // Use region name, but if it's too similar to parent, make directional
        if (nameBase.empty()) nameBase = parentName;

        if (simRand() % 100 < 40 && !nameConflicts("South " + nameBase)) {
            // 40% chance of directional name with ideology prefix
            std::string dirStr = makeDirectionalName(nameBase, parentName);
            if (simRand() % 100 < 50) {
                countryName = dirStr;
            } else {
                if (avgEcon < -20) countryName = "People's Republic of " + dirStr;
                else if (avgEcon > 30) countryName = "Free State of " + dirStr;
                else countryName = "Republic of " + dirStr;
            }
        } else {
            // Original ideology template approach
            // Every one of these names a government. The old tables were built
            // from insurgency language -- Liberation Army, Separatist Movement,
            // Freedom Fighters, Resistance Council -- which reads as a faction
            // that does not believe its own claim. They also now use the same
            // vocabulary as PoliticalIdentity.h, so a breakaway that is
            // communist and a country that TURNS communist speak alike.
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* leftPre[] = {
                "Socialist Union of ", "People's Republic of ",
                "Workers' Republic of ", "Socialist Republic of ",
                "People's Federation of "
            };
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* farLeftPre[] = {
                "People's Republic of ", "Socialist Union of ",
                "Workers' Republic of ", "People's Commune of "
            };
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* rightPre[] = {
                "Free State of ", "Republic of ", "Commonwealth of ",
                "Democratic Alliance of "
            };
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* farRightPre[] = {
                "National State of ", "Free State of ", "Dominion of "
            };
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* authPre[] = {
                "State of ", "Autocracy of ", "Directorate of "
            };
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* libPre[] = {
                "Free Republic of ", "Free State of ", "Commonwealth of "
            };
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* neutralPre[] = {
                "Republic of ", "State of ", "Federation of ", "Union of "
            };
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* leftOneWord[] = {
                "The Commune", "The Collective", "Soviet", "The Union"
            };
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* farLeftOneWord[] = {
                "The Commune", "The Collective", "Soviet", "Red Star", "The Proletariat"
            };
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* rightOneWord[] = {
                "The Alliance", "The Federation", "The Directorate"
            };
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* farRightOneWord[] = {
                "The Dominion", "The Imperium", "The Authority"
            };
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* authOneWord[] = {
                "The Junta", "The Council", "The Order"
            };
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* libOneWord[] = {
                "The Republic", "The Commonwealth", "The Concord"
            };
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* neutralOneWord[] = {
                "The Republic", "The Free State", "The Federation", "The Union"
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
            if (simRand() % 100 < 60) {
                int ti = simRand() % oneWordCount;
                countryName = oneWordSet[ti];
            } else {
                int ti = simRand() % multiWordCount;
                countryName = std::string(multiWordSet[ti]) + nameBase;
            }
        }
        // Final conflict check for region-based path
        if (nameConflicts(countryName)) {
            std::string parentName = parentRootName(parentCid);
            countryName = makeDirectionalName(regionName.empty() ? parentName : regionName, parentName);
        }
    }

    // Safety net: if name still conflicts, use a generic pattern
    if (nameConflicts(countryName)) {
        std::string parentName = parentRootName(parentCid);
        // "<Dir> <Parent> Breakaway" was the parent's word for them, not their
        // own. A government that has taken and holds territory believes it IS
        // the country -- nobody declares independence and then files the paper
        // under "Breakaway". A directional republic is what such a state
        // actually calls itself.
        // i18n-ignore: builds the English name; the display forms live in Locale.cpp
        static const char* fallbackDirs[] = {"Northern ", "Southern ", "Eastern ",
                                             "Western ", "Central "};
        countryName = "Republic of " + std::string(fallbackDirs[simRand() % 5]) + parentName;
    }

    // Nothing ships with a blank name. An empty territory string used to reach
    // the templates and come out as " Federation" -- a leading space and a form
    // of government belonging to nowhere.
    {
        std::string trimmed = countryName;
        while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && trimmed.back()  == ' ') trimmed.pop_back();
        // A form of government is not a country name.
        //
        // The length test below catches "" and " ", but " Federation" trims to
        // a perfectly respectable ten characters and shipped as a country
        // called "Federation" -- a constitutional arrangement belonging to
        // nowhere. "The Federation" and "The Imperium" are deliberate and stay:
        // a junta that names itself after its own institution reads as a junta.
        // A BARE one reads as a missing string.
        static const char* const BARE_FORMS[] = {
            "Federation", "Republic", "Union", "State", "Commonwealth",
            "Confederation", "Directorate", "Alliance", "Council", "Authority",
        };
        bool bare = false;
        for (const char* f : BARE_FORMS)
            if (trimmed == f) { bare = true; break; }
        if (bare || trimmed.size() < 3) {
            // i18n-ignore: builds the English name; the display forms live in Locale.cpp
            static const char* dirs[] = {"Northern ", "Southern ", "Eastern ",
                                         "Western ", "Central "};
            std::string base = parentRootName(parentCid);
            if (base.empty()) base = "Territory";
            trimmed = "Republic of " + std::string(dirs[simRand() % 5]) + base;
        }
        countryName = trimmed;
    }

    int rebelNum = rebelCid - 60000;
    std::string isoA3 = (rebelNum < 10) ? "R0" + std::to_string(rebelNum) : "R" + std::to_string(rebelNum);

    // ── Rich flag generation (HSL-based harmonious palettes) ──
    Color c1, c2;
    {
        // Pick a primary hue based on ideology
        int hue;
        int sat = 70 + simRand() % 25;     // 70-95%
        int lig = 40 + simRand() % 20;     // 40-60%

        if (avgEcon < -30) {
            // Far-left: reds (0±20)
            hue = (simRand() % 40 - 20 + 360) % 360;
        } else if (avgEcon > 30) {
            // Far-right: deep blues (220±20)
            hue = 220 + simRand() % 40 - 20;
        } else if (avgSoc < -30) {
            // Authoritarian: dark greens (120±25)
            hue = 120 + simRand() % 50 - 25;
            sat = 60 + simRand() % 25;     // more muted greens
            lig = 30 + simRand() % 20;     // darker
        } else if (avgSoc > 30) {
            // Libertarian: golds/yellows (45±20)
            hue = 45 + simRand() % 40 - 20;
            lig = 45 + simRand() % 15;     // 45-60%
        } else if (avgEcon < -20) {
            // Center-left: warm reds/oranges (10±15)
            hue = 10 + simRand() % 30 - 15;
        } else if (avgEcon > 20) {
            // Center-right: moderate blues (210±15)
            hue = 210 + simRand() % 30 - 15;
        } else {
            // Centrist: purples or teals (random, muted)
            int ci = simRand() % 3;
            if (ci == 0) { hue = 270 + simRand() % 40 - 20; }     // purple
            else if (ci == 1) { hue = 180 + simRand() % 40 - 20; } // teal
            else { hue = simRand() % 360; }                         // any
            sat = 40 + simRand() % 30;     // more muted
            lig = 35 + simRand() % 25;     // wider range
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
        int c2choice = simRand() % 100;
        if (c2choice < 30) {
            // Complementary: hue + 150..210
            int cHue = (hue + 150 + simRand() % 60) % 360;
            c2 = hslToRgb(cHue, std::min(100, sat + 10), std::min(100, lig + 15));
        } else if (c2choice < 55) {
            // Analogous: hue ±20..40
            int offset = (simRand() % 2 == 0) ? 20 + simRand() % 20 : -(20 + simRand() % 20);
            int cHue = (hue + offset + 360) % 360;
            c2 = hslToRgb(cHue, std::max(30, sat - 20), std::min(100, lig + 10));
        } else if (c2choice < 75) {
            // Triadic: hue + 110..130
            int cHue = (hue + 110 + simRand() % 20) % 360;
            c2 = hslToRgb(cHue, sat, lig);
        } else if (c2choice < 90) {
            // White/off-white
            c2 = (simRand() % 2 == 0) ? WHITE : Color{255, 255, 230, 255};
        } else {
            // Dark neutral (black, dark gray)
            int gv = 20 + simRand() % 40;
            c2 = Color{(uint8_t)gv, (uint8_t)gv, (uint8_t)gv, 255};
        }
    }

    // ── Parent country color influence ──
    {
        auto& parentCountry = m_countries.getAll()[parentCid];
        auto& parentFlagColors = parentCountry.flagActual.colors;
        if (parentFlagColors.size() >= 1 && simRand() % 100 < 60) {
            Color pc = parentFlagColors[0];
            float blend = 0.2f + (float)(simRand() % 30) / 100.0f;
            c1 = {
                (uint8_t)(c1.r * (1.0f - blend) + pc.r * blend),
                (uint8_t)(c1.g * (1.0f - blend) + pc.g * blend),
                (uint8_t)(c1.b * (1.0f - blend) + pc.b * blend),
                255
            };
        }
        if (parentFlagColors.size() >= 2 && simRand() % 100 < 40) {
            Color pc = parentFlagColors[1];
            float blend = 0.2f + (float)(simRand() % 20) / 100.0f;
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
            int ci = simRand() % 3;
            if (ci == 0) return Color{20, 20, 20, 255};
            else if (ci == 1) return Color{180, 30, 30, 255};
            else return Color{20, 40, 120, 255};
        } else {
            int ci = simRand() % 3;
            if (ci == 0) return WHITE;
            else if (ci == 1) return Color{255, 230, 100, 255};
            else return Color{220, 220, 220, 255};
        }
    };

    struct FlagPatternChoice { FlagType ft; float weight; };
    auto pickPattern = [&](const std::vector<FlagPatternChoice>& choices) -> FlagType {
        float total = 0;
        for (auto& c : choices) total += c.weight;
        float roll = (float)simRand() / (float)RAND_MAX * total;
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
        int symRoll = simRand() % 100;
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
        int symRoll = simRand() % 100;
        if (extremeRadical && symRoll < 3) {  // <--- MUCH rarer: 3% instead of 30%
            // Both are censored symbols, and both are reached only here.
            pickSVG(simRand() % 2 == 0 ? "swastika.svg" : "eagle_nazi.svg");
            symbol.size = 0.35f;
        } else if (symRoll < 30) {
            pickSVG("cross_latin.svg");
            symbol.size = 0.38f;
        } else if (symRoll < 55) {
            // There is no generic eagle. The hand-drawn one did not read as a
            // bird at any size a flag is drawn, and Wikimedia's heraldic eagles
            // are 100 KB of linework that nanosvg renders as noise -- so it was
            // removed rather than shipped looking wrong. eagle_nazi.svg stays,
            // because a silhouette with a wreath survives being small.
            pickSVG(simRand() % 2 == 0 ? "sun.svg" : "star5.svg");
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
        int symRoll = simRand() % 100;
        if (symRoll < 30) {
            pickSVG(simRand() % 2 == 0 ? "sun.svg" : "sun_wavy.svg");
            symbol.size = 0.38f;
        } else if (symRoll < 55) {
            pickSVG("torch.svg");
            symbol.size = 0.38f;
        } else if (symRoll < 70) {
            pickSVG("crescent.svg");
            symbol.size = 0.32f;
        } else if (symRoll < 82) {
            pickSVG(simRand() % 2 == 0 ? "star5.svg" : "diamond.svg");
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
        int symRoll = simRand() % 100;
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
            pickSVG(simRand() % 2 == 0 ? "diamond.svg" : "circle_stars.svg");
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
        // Matched on the FILENAME. "eagle_nazi" is tested rather than "eagle"
        // so that the match stays exact if a generic eagle is ever added back:
        // "eagle.svg" does not contain "eagle_nazi", so it could not be
        // censored by accident. There is no generic eagle at present.
        if (sym.type == SymbolType::SVG_FILE &&
            (sym.text.find("swastika") != std::string::npos ||
             sym.text.find("eagle_nazi") != std::string::npos ||
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

    // WHEN IT BEGAN. Every country that reaches this line is being made now, by
    // a release or a revolt; the ones on the map at the start keep -1, which is
    // what lets a profile say "since the start" instead of inventing a date.
    rebel.foundedTurn = m_turnNumber;
    m_countries.getAll()[rebelCid] = rebel;
    if (!rebel.isoA3.empty()) m_isoToCid[rebel.isoA3] = rebelCid;
    m_countryCompass[rebelCid] = makeCompass(avgEcon, avgSoc);

    if (rebelCid >= (int)m_countryPixels.size())
        m_countryPixels.resize(rebelCid + 1);
    if (rebelCid >= (int)m_countryRelationColors.size())
        m_countryRelationColors.resize(rebelCid + 1, Color{80, 80, 80, 255});

    // Same reason as restoreRebels: this ends in LoadTextureFromImage, which
    // needs a GL context that a headless run does not have. Replaying a save's
    // history re-creates every rebellion in it, so a timelapse export of any
    // world that ever had one crashed here at address zero.
    if (!m_headless) {
        Texture2D tex = FlagRenderer::render(rebel.flagActual, 256, 128, m_dataDir, &m_odmJsonData);
        m_countryFlags[rebelCid] = tex;
    }

    if (m_config.aiDebug)
        printf("[REBELLION] Created '%s' (CID=%d, ISO=%s, %zu provinces, %lld pop, econ=%.1f soc=%.1f)\n",
           countryName.c_str(), rebelCid, isoA3.c_str(), provinceIds.size(), totalPop, avgEcon, avgSoc);

    // HOW IT BEGINS IS THE WHOLE DIFFERENCE. A revolt starts at war; a release
    // starts at peace, guaranteed by the country that let it go. Everything
    // above this line is identical, which is the reason the two share a
    // function.
    auto& parentIso = m_countries.getAll()[parentCid].isoA3;
    if (peaceful) {
        // The guarantee runs one way, from the releaser to the released: it is
        // a promise made, not a pact agreed, and the new state owes nothing for
        // it. That is what makes release buy a friend rather than merely cost
        // ground.
        m_relations[parentIso][isoA3].guarantee = true;
        if (m_config.aiDebug)
            printf("[RELEASE] %s released by %s, guaranteed\n",
                   isoA3.c_str(), parentIso.c_str());
    } else {
        m_relations[isoA3][parentIso].war = true;
        m_relations[parentIso][isoA3].war = true;
        if (m_config.aiDebug)
            printf("[REBELLION] War: %s vs %s\n", isoA3.c_str(), parentIso.c_str());
    }

    // Notify player if this affects them. Not on a release: the player asked
    // for it, and a popup announcing the thing they just clicked is noise.
    if (!peaceful && parentCid == m_playerCountryId) {
        std::string parentName = m_countries.getAll()[parentCid].name;
        // One sentence with the names in it, and the names put through
        // properName: this was three English fragments glued round two raw
        // country names, so the whole popup stayed in English.
        pushPopup(PopupType::REBELLION,
            T("Breakaway State!"),
            TextFormat(T("%s has declared independence!\nWar: %s vs %s"),
                       od::i18n::properName(countryName).c_str(),
                       od::i18n::properName(parentName).c_str(),
                       od::i18n::properName(countryName).c_str()),
            rebelCid);
    }

    // Transfer province ownership + populate m_countryPixels for population view
    bool anyMoved = false;
    for (int pid : provinceIds) {
        Province* pp = m_provinces.getProvinceById(pid);
        if (pp) {
            // noteRevolt teaches the AI that it LOST ground to unrest. A
            // release is a decision, not a failure, and counting it as a revolt
            // would train the policy away from a lever it is meant to use.
            if (m_ai && !peaceful) m_ai->noteRevolt(parentCid);
            pp->countryId = rebelCid;
            if ((size_t)pid < m_provinceCountryLookup.size())
                m_provinceCountryLookup[pid] = rebelCid;
            reindexProvinceOwner(pid, parentCid, rebelCid);
        }
        auto ppIt = m_provincePixels.find(pid);
        if (ppIt != m_provincePixels.end()) {
            for (int idx : ppIt->second) {
                if (idx >= 0 && idx < (int)m_pixelCountryArray.size())
                    m_pixelCountryArray[idx] = (uint16_t)rebelCid;
                // Populate rebel's m_countryPixels
                if (rebelCid >= 0 && rebelCid < (int)m_countryPixels.size())
                    m_countryPixels[rebelCid].push_back(idx);
                anyMoved = true;
            }
        }
    }
    // Remove the moved pixels from the parent in ONE pass.
    //
    // The predicate asks m_pixelCountryArray -- which the loop above has
    // already repointed at the rebel -- rather than probing a hash set of the
    // moved pixels. Same answer, but a country holding 700k pixels was paying
    // 700k hash lookups for a revolt in a SINGLE province: measured at 12-44 ms
    // per rebellion, roughly 40% of the entire turn on a self-play map. An
    // array read makes the same pass an order of magnitude cheaper.
    if (anyMoved && parentCid >= 0 && parentCid < (int)m_countryPixels.size()) {
        auto& parentPixels = m_countryPixels[parentCid];
        parentPixels.erase(std::remove_if(parentPixels.begin(), parentPixels.end(),
                                          [&](int idx) {
                                              return idx < 0 ||
                                                     idx >= (int)m_pixelCountryArray.size() ||
                                                     m_pixelCountryArray[idx] != parentCid;
                                          }),
                           parentPixels.end());
    }

    // Labels are rebuilt ONCE per turn (m_labelsDirty, consumed in
    // processTurn) instead of per rebellion — computeCountryLabels does a full
    // 8192x4096 raster scan, so ten rebellions used to mean ten full scans.
    m_labelsDirty = true;
}

// === releaseSubsetOk ===
//
// See the declaration. Contiguity is walked over the CHOSEN set only: a run
// that is connected through a province the player just excluded is two
// countries, and a country in two pieces with somebody else's ground between
// them is not a border anybody drew -- the same sentence releaseNation uses
// about refusing half a release.
bool Game::releaseSubsetOk(int ownerCid, const std::vector<int>& provs,
                           std::string& whyNot) const {
    if ((int)provs.size() < RELEASE_MIN_PROVINCES) {
        whyNot = TextFormat(T("A nation needs at least %d provinces"),
                            (int)RELEASE_MIN_PROVINCES);
        return false;
    }
    for (int pid : provs) {
        const Province* p = m_provinces.getProvinceById(pid);
        if (!p || p->countryId != ownerCid) {
            whyNot = T("Some of that ground is no longer theirs to give");
            return false;
        }
    }
    int held = 0;
    for (const auto& [pid, p] : m_provinces.getAllProvinces())
        if (p.countryId == ownerCid) ++held;
    if (held > 0 && (double)provs.size() > (double)held * (double)RELEASE_MAX_SHARE) {
        whyNot = T("That is too much of the country to give away at once");
        return false;
    }
    // One connected piece, walked over the subset itself.
    std::vector<int> seen;
    std::vector<int> stack{provs.front()};
    seen.push_back(provs.front());
    while (!stack.empty()) {
        const int cur = stack.back(); stack.pop_back();
        for (int pid : provs) {
            if (std::find(seen.begin(), seen.end(), pid) != seen.end()) continue;
            if (!provincesAdjacent(cur, pid)) continue;
            seen.push_back(pid);
            stack.push_back(pid);
        }
    }
    if (seen.size() != provs.size()) {
        whyNot = T("A nation has to be in one piece");
        return false;
    }
    return true;
}

// === releaseNation ===
//
// See the declaration, and ReleaseRules.h for what may be released.
int Game::releaseNation(int countryId, const ReleaseCandidate& region) {
    if (region.provinces.size() < RELEASE_MIN_PROVINCES) return -1;

    // RE-CHECKED, NOT TRUSTED. The panel's list can be a frame old, the
    // multiplayer host takes this from a client that may say anything, and the
    // bankruptcy cascade chooses several steps before it acts. Everything below
    // assumes these provinces are still ours; if one is not, the whole release
    // is refused rather than partially carried out, because half a nation is
    // not a border anybody drew.
    for (int pid : region.provinces) {
        const Province* p = m_provinces.getProvinceById(pid);
        if (!p || p->countryId != countryId) return -1;
    }
    // And it must still be legal at this size -- a country that has shrunk
    // since the list was built could otherwise release itself out of existence.
    int held = 0;
    for (int pid : provincesOf(countryId)) { (void)pid; ++held; }
    if (held <= 0) return -1;
    if ((double)region.provinces.size() > (double)held * (double)RELEASE_MAX_SHARE)
        return -1;

    const int newCid = allocateRebelCid();
    if (newCid <= 0) return -1;
    createRebelCountry(newCid, countryId, region.provinces, /*peaceful=*/true);
    return newCid;
}

// === processRebellions ===
void Game::processRebellions(int countryId) {
    // Not in the tutorial. The lesson stages exactly one revolt, at the point
    // it is ready to explain what unrest is -- and an organic one arriving
    // before that (or a second one during it) is a live game contradicting
    // the script mid-sentence. tutorialAct("rebellion") calls
    // createRebelCountry directly and is not affected by this.
    if (m_tutorialMode) return;
    if (m_scriptRebellionsOff) return;   // `set rules.rebellions false`

    // One census per turn, shared by every country processed in it. Counting
    // per faction would walk the country table thousands of times on the very
    // maps this exists to protect.
    if (m_rebelCensusTurn != m_turnNumber) {
        m_rebelCensusTurn = m_turnNumber;
        m_rebelsSpawnedThisTurn = 0;
        m_rebelCensus = 0;
        for (auto& [cid, c] : m_countries.getAll()) {
            (void)c;
            if (cid >= REBEL_CID_MIN && cid < SPC_CID) m_rebelCensus++;
        }
    }
    if (countryId <= 0 || countryId == SPC_CID || countryId == UNC_CID || countryId == BLC_CID) return;

    // Breakaway states must not immediately re-fracture. They own low-alignment
    // provinces that the parent auto-claims and declares war on, which keeps
    // rebellion chance high — so rebel countries processing rebellions produced
    // a runaway (dozens of new one-province rebel states every turn, each of
    // which then rebelled again). A newly formed rebel is simply skipped here.
    if (countryId >= REBEL_CID_MIN) return;

    int totalProvCount = 0;
    std::vector<int> rebellingProvs;
    // Was a scan of every province on the map, per country, per turn. On a
    // 30-country map this loop alone was the largest single cost in the turn.
    const auto& allProvs = m_provinces.getAllProvinces();
    for (int pid : provincesOf(countryId)) {
        auto pIt = allProvs.find(pid);
        // The index is a candidate set: a province taken from us earlier this
        // same turn is still listed until the next rebuild.
        if (pIt == allProvs.end() || pIt->second.countryId != countryId) continue;
        totalProvCount++;
        float chance = getProvinceRebellionChance(pid, countryId);
        float roll = (float)simRand() / (float)RAND_MAX * 100.0f;
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

    // Group by ideology (±20) AND geographic proximity (≤4 BFS steps).
    //
    // This was a PAIRWISE distance query: a fresh bounded BFS, with its own
    // unordered_map and queue, for every (seed, candidate) pair — O(R^2)
    // traversals of a densely connected province graph. Profiling a 30-country
    // self-play map, this function alone was 59 ms of a 133 ms turn, dwarfing
    // everything else including the neural net.
    //
    // One BFS per SEED answers the same question: flood four hops out, then
    // every candidate the flood reached is within four hops by definition.
    // O(R) traversals, and the scratch buffers are reused across seeds instead
    // of reallocated per pair.
    const int MAX_HOPS = 4;
    std::vector<int> dist(m_provinceCountryLookup.size(), -1);
    std::vector<int> touched;  // pids to reset, so the reset is O(visited)
    std::vector<int> frontier, nextFrontier;

    std::vector<std::vector<int>> factions;
    std::vector<bool> assigned(rebellingProvs.size(), false);
    for (size_t i = 0; i < rebellingProvs.size(); i++) {
        if (assigned[i]) continue;
        const int seed = rebellingProvs[i];
        std::vector<int> faction = {seed};
        assigned[i] = true;
        Vector2 baseComp = {0, 0};
        auto cit = m_provinceCompass.find(seed);
        if (cit != m_provinceCompass.end()) baseComp = cit->second;

        // Flood MAX_HOPS out from the seed.
        for (int pid : touched) if ((size_t)pid < dist.size()) dist[pid] = -1;
        touched.clear();
        frontier.clear();
        if ((size_t)seed < dist.size()) { dist[seed] = 0; touched.push_back(seed); }
        frontier.push_back(seed);
        for (int d = 0; d < MAX_HOPS && !frontier.empty(); ++d) {
            nextFrontier.clear();
            for (int cur : frontier) {
                auto nit = m_provinceNeighbors.find(cur);
                if (nit == m_provinceNeighbors.end()) continue;
                for (int n : nit->second) {
                    if ((size_t)n >= dist.size() || dist[n] >= 0) continue;
                    dist[n] = d + 1;
                    touched.push_back(n);
                    nextFrontier.push_back(n);
                }
            }
            frontier.swap(nextFrontier);
        }

        for (size_t j = i + 1; j < rebellingProvs.size(); j++) {
            if (assigned[j]) continue;
            const int cand = rebellingProvs[j];
            if ((size_t)cand >= dist.size() || dist[cand] < 0) continue; // out of range
            auto cjt = m_provinceCompass.find(cand);
            Vector2 comp = (cjt != m_provinceCompass.end()) ? cjt->second : Vector2{0,0};
            float ideoDist = sqrtf((comp.x - baseComp.x) * (comp.x - baseComp.x) +
                                   (comp.y - baseComp.y) * (comp.y - baseComp.y));
            if (ideoDist <= 20.0f) {
                faction.push_back(cand);
                assigned[j] = true;
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
                    float killPct = 0.3f + (float)simRand() / (float)RAND_MAX * 0.4f;
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

        // THE CEILING. Past it the revolt is suppressed rather than simulated:
        // the province still gets its cooldown below, so this does not become a
        // province that tries to rise every single turn and is refused every
        // single turn.
        //
        // Reaching either limit means something upstream is wrong -- unrest
        // does not legitimately produce twelve new states in a turn -- so it
        // says so once rather than failing silently, which is exactly how the
        // 24,030-country run went unnoticed for fourteen turns.
        if (m_rebelCensus + m_rebelsSpawnedThisTurn >= MAX_LIVE_REBELS ||
            m_rebelsSpawnedThisTurn >= MAX_NEW_REBELS_PER_TURN) {
            if (!m_rebelCeilingWarned) {
                m_rebelCeilingWarned = true;
                printf("[REBELLION] ceiling reached: %d live, %d new this turn. "
                       "Further revolts are being suppressed -- something is "
                       "driving unrest far beyond normal.\n",
                       m_rebelCensus, m_rebelsSpawnedThisTurn);
            }
            for (int pid : faction) m_provinceRebellionCooldown[pid] = REBELLION_COOLDOWN_TURNS;
            continue;
        }

        int rebelCid = allocateRebelCid();
        createRebelCountry(rebelCid, countryId, faction);
        m_rebelsSpawnedThisTurn++;
        m_rebellionsThisTurnByCid[countryId]++;
        // This province has now had its revolt. Whatever happens to it next --
        // the rebel holds it, the parent retakes it, a third party takes it --
        // it does not rise again for a while. Without this the same province
        // simply re-rolled its grievance every turn for the rest of the game.
        for (int pid : faction) m_provinceRebellionCooldown[pid] = REBELLION_COOLDOWN_TURNS;

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
                grantClaim(parentIso, pid);
                printf("[CLAIM] %s -> province %d (rebellion claim)\n", parentIso.c_str(), pid);
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
    // MATERIALS BEFORE MONEY. The production pass can pay the treasury -- the
    // auto-sale of surplus raw is income like any other -- so it has to run
    // before the balance is struck, or a country banks last turn's sale and the
    // panel disagrees with the ledger by exactly one turn for ever.
    //
    // Inert unless the goods economy is switched on for this world; see
    // Game::m_goodsEconomy.
    processProduction(countryId);

    auto cs = computeCountryIncome(countryId);
    auto& treasury = m_countries.getAll()[countryId].treasury;
    float net = cs.total - cs.expenses;
    // OD_ECON_TRACE=<cid>: the ledger for one country every turn, so a
    // bankruptcy can be traced to the expense that caused it rather than
    // read off the penalty line after the fact.
    {
        static const int traceCid = std::getenv("OD_ECON_TRACE") ? atoi(std::getenv("OD_ECON_TRACE")) : -1;
        if (traceCid == countryId)
            fprintf(stderr, "[ECON] turn %d cid=%d provinces=%zu treasury=%.2f net=%.2f income=%.2f (ind %.2f res %.2f pop %.2f)"
                    " expenses=%.2f (army %.2f navy %.2f policy %.2f minority %.2f research %.2f pacify %.2f industry %.2f)\n",
                    m_turnNumber, countryId, provincesOf(countryId).size(), treasury, net, cs.total, cs.gross, cs.resource, cs.pop, cs.expenses,
                    cs.armyExpenses, cs.navyExpenses, cs.policyCosts, cs.minorityCosts, cs.researchCost,
                    cs.pacificationCost, cs.industryUpkeep);
    }
    treasury += net;

    // Where the money goes, when asked. See src/ai/MoneyLedger.h.
    money::tick();
    money::add(money::INCOME_INDUSTRY,     cs.gross);
    money::add(money::INCOME_RESOURCE,     cs.resource);
    money::add(money::INCOME_POP,          cs.pop);
    money::add(money::UPKEEP_ARMY,        -cs.armyExpenses);
    money::add(money::UPKEEP_NAVY,        -cs.navyExpenses);
    money::add(money::UPKEEP_POLICY,      -cs.policyCosts);
    money::add(money::UPKEEP_MINORITY,    -cs.minorityCosts);
    money::add(money::BUDGET_RESEARCH,    -cs.researchCost);
    money::add(money::BUDGET_PACIFICATION,-cs.pacificationCost);
    money::add(money::UPKEEP_INDUSTRY,    -cs.industryUpkeep);

    // GOING BROKE USED TO BE FREE. The line here was `if (treasury < 0)
    // treasury = 0;` -- the shortfall was deleted and the turn moved on, so a
    // country could keep a fleet and an army it had no income for, for ever.
    // Nothing anywhere else noticed, because nothing else looked.
    if (treasury >= 0.0) {
        m_bankruptCountries.erase(countryId);
        m_bankruptStreak.erase(countryId);   // paid for itself: the clock resets
        return;
    }
    ++m_bankruptStreak[countryId];
    const float shortfall = (float)(-treasury);
    money::add(money::WRITTEN_OFF, -(double)shortfall);
    treasury = 0.0;
    m_bankruptCountries.insert(countryId);
    applyBankruptcyPenalties(countryId, shortfall, cs);
}

// === applyBankruptcyPenalties ===
//
// THE ORDER IS DELIBERATE, AND IT IS NOT THE OBVIOUS ONE.
//
// Cheapest to lose first, and "cheapest" here is measured in what it costs to
// undo, not in what it feels like:
//
//   budgets   free to cut and reversible next turn
//   policies  re-enactable, at the price of an implementation delay
//   minority  a slider, but the alignment it costs takes many turns to win back
//   ships     a carrier is 25/turn, a destroyer 10 (Game_Economy.cpp)
//   troops    10,000 men are 0.01/turn
//
// The two middle steps were missing entirely, and their absence was a trap: a
// country whose expenses were political — a stack of doctrines, a generous
// minority settlement — could sell its whole fleet and disband its whole army
// and still be bankrupt the next turn, because the cascade could not reach the
// thing it was actually paying for. It would then sit at maximum unrest with
// nothing left to give. Now every recurring cost is reachable, which is what
// makes BANKRUPTCY_UNREST_PCT a punishment rather than a death sentence.
//
// Those army numbers are why troops are not first. Closing a one-carrier
// shortfall out of the army means disbanding twenty-five million men -- most
// countries would lose their entire army and still be bankrupt, having saved
// almost nothing. Scrapping the ship fixes it outright. Disbanding stays in the
// cascade for the case where the army genuinely IS the expense.
//
// Whatever is still unpaid after all three becomes unrest, which is the point:
// a country that cannot pay for itself and has nothing left to sell is a
// country in trouble, and it should show.
void Game::applyBankruptcyPenalties(int countryId, float shortfall,
                                    const CountryIncomeSnapshot& cs) {
    if (countryId <= 0 || countryId >= SPC_CID) return;
    const bool isPlayer = (countryId == m_playerCountryId);
    float remaining = shortfall;

    // ── 1. Discretionary budgets ────────────────────────────────────────
    // computeCountryIncome() already scales these down to what is affordable,
    // so this recovers nothing THIS turn. It is still done, and done first,
    // because the slider otherwise sits where the player left it and silently
    // funds nothing every turn afterwards -- the budget has to actually come
    // down, not just go unpaid.
    bool cutBudgets = false;
    if (isPlayer) {
        if (m_researchAllocation > 0.0f || m_pacificationAllocation > 0.0f) cutBudgets = true;
        m_researchAllocation = 0.0f;
        m_pacificationAllocation = 0.0f;
    } else {
        auto ra = m_countryResearchAllocation.find(countryId);
        auto pa = m_countryPacification.find(countryId);
        if ((ra != m_countryResearchAllocation.end() && ra->second > 0.0f) ||
            (pa != m_countryPacification.end() && pa->second > 0.0f)) cutBudgets = true;
        m_countryResearchAllocation[countryId] = 0.0f;
        m_countryPacification[countryId] = 0.0f;
    }

    // ── 2. Repeal doctrines, dearest first ──────────────────────────────
    // Reversible: the policy can be enacted again once the country can pay for
    // it, at the cost of waiting out its implementation turns.
    int repealed = 0;
    if (remaining > 0.0f) {
        auto apIt = m_countryActivePolicyIndices.find(countryId);
        if (apIt != m_countryActivePolicyIndices.end()) {
            // (this turn's bill, activeIndex). The BILL, not the sticker price:
            // repealing a half-built doctrine frees only what it is costing
            // today, and counting the full price here would have the country
            // stop repealing while it was still overdrawn.
            std::vector<std::pair<float, int>> costly;
            for (int idx : apIt->second) {
                if (idx < 0 || idx >= (int)m_activePolicies.size()) continue;
                const ActivePolicy& ap = m_activePolicies[idx];
                if (ap.countryId != countryId || ap.turnsRemaining < 0) continue;
                for (const auto& p : m_allPolicies)
                    if (p.id == ap.policyId) {
                        const float bill = policyUpkeep(ap, p);
                        if (bill > 0.0f) costly.push_back({bill, idx});
                        break;
                    }
            }
            std::sort(costly.begin(), costly.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
            for (const auto& [cost, idx] : costly) {
                if (remaining <= 0.0f) break;
                cancelPolicy(idx);
                remaining -= cost;
                repealed++;
            }
        }
    }

    // ── 3. Minority settlements back to their free options ──────────────
    // BEFORE the ships and troops below, and that order is load-bearing.
    // Moving this step to last -- on the reasoning that minority cuts are
    // the only ones that raise unrest -- measured 107 -> 73 across six
    // seats, hurting five of them: 1914:SWE 200 -> 50, 1939:USA 84 -> 33,
    // and modern:CHN, the seat it was meant to rescue, 8 -> 4. Disbanding a
    // field army mid-war loses the country THIS turn; unrest is slow and
    // survivable. Materiel is not the cheap cut. Do not reorder this.
    // Every category has an option that costs nothing. Dropping to it saves the
    // whole minority bill at once, and is paid for in alignment — which is the
    // right price for a government that has run out of money, and one it will
    // spend the next fifty turns earning back.
    int minorityCut = 0;
    if (remaining > 0.0f && cs.minorityCosts > 0.0f) {
        std::unordered_set<std::string> seen;
        for (int pid : provincesOf(countryId)) {
            if (remaining <= 0.0f) break;
            auto mIt = m_provinceMinorities.find(pid);
            if (mIt == m_provinceMinorities.end()) continue;
            for (auto& mg : mIt->second) {
                if (remaining <= 0.0f) break;
                if (!seen.insert(mg.name).second) continue;
                for (size_t ci = 0; ci < m_ethnicPolicyCategories.size(); ++ci) {
                    const int cur = ethnicPolicyOption(countryId, mg.name, ci);
                    if (cur < 0) continue;
                    const float curCost = m_ethnicPolicyCategories[ci].options[cur].costPerTurn;
                    if (curCost <= 0.0f) continue;
                    // The cheapest option in this category, preferring the one
                    // that gives up the least goodwill among the free ones.
                    int best = cur; float bestCost = curCost, bestAlign = -1e9f;
                    for (size_t oi = 0; oi < m_ethnicPolicyCategories[ci].options.size(); ++oi) {
                        const auto& o = m_ethnicPolicyCategories[ci].options[oi];
                        if (o.costPerTurn > bestCost) continue;
                        if (o.costPerTurn < bestCost ||
                            o.alignmentPerTurn > bestAlign) {
                            bestCost = o.costPerTurn;
                            bestAlign = o.alignmentPerTurn;
                            best = (int)oi;
                        }
                    }
                    if (best == cur) continue;
                    setEthnicPolicyOption(countryId, mg.name, ci, best);
                    remaining -= (curCost - bestCost);
                    minorityCut++;
                    if (remaining <= 0.0f) break;
                }
            }
        }
    }

    // ── 4. Scrap ships, dearest first ───────────────────────────────────
    // Marked rather than erased: cleanupSunkShips() already removes UNC_CID
    // ships and shifts every pending order index that pointed past them, and
    // that index bookkeeping is not worth writing a second time.
    // Gathered and sorted once, for the same reason as the army below.
    int scrapped = 0;
    if (remaining > 0.0f) {
        std::vector<std::pair<float, int>> fleet;   // (saving, index)
        for (size_t i = 0; i < m_ships.size(); ++i) {
            if (m_ships[i].countryId != countryId) continue;
            float saving = (m_ships[i].type == "carrier")   ? 25.0f
                         : (m_ships[i].type == "destroyer") ? 10.0f : 0.0f;
            saving += (m_ships[i].crew / 10000.0f) * 0.2f;
            if (saving > 0.0f) fleet.push_back({saving, (int)i});
        }
        std::sort(fleet.begin(), fleet.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [saving, idx] : fleet) {
            if (remaining <= 0.0f) break;
            m_ships[idx].countryId = UNC_CID;   // cleanupSunkShips() collects it
            remaining -= saving;
            scrapped++;
        }
    }

    // ── 5. Disband troops ───────────────────────────────────────────────
    // Largest stacks first, so a bankrupt country loses its field army before
    // its garrisons rather than being hollowed out everywhere at once.
    long long disbanded = 0;
    if (remaining > 0.0f) {
        // 0.01 per 10,000 men, so one unit of upkeep is a million men.
        long long menNeeded = (long long)(remaining * 1000000.0f);

        // Gathered ONCE and sorted, not re-scanned per stack. The first version
        // searched the whole province-army map for the largest unit on every
        // iteration, which is a full map scan per stack drained -- with a
        // shortfall big enough to need thousands of stacks that is quadratic,
        // and it hung the game on turn one of a forced test. Nothing is erased
        // during the walk either, so these pointers stay valid; the empties are
        // swept afterwards.
        std::vector<ArmyUnit*> mine;
        for (auto& [pid, units] : m_provinceArmies)
            for (auto& u : units)
                if (u.countryId == countryId && u.count > 0) mine.push_back(&u);
        std::sort(mine.begin(), mine.end(),
                  [](const ArmyUnit* a, const ArmyUnit* b) { return a->count > b->count; });

        for (ArmyUnit* u : mine) {
            if (menNeeded <= 0) break;
            const long long take = std::min<long long>(u->count, menNeeded);
            u->count -= take;
            disbanded += take;
            menNeeded -= take;
        }
        if (disbanded > 0) {
            for (auto& [pid, units] : m_provinceArmies)
                units.erase(std::remove_if(units.begin(), units.end(),
                                           [](const ArmyUnit& u) { return u.count <= 0; }),
                            units.end());
        }
        remaining -= (float)disbanded / 1000000.0f;
    }

    // ── 6. LET GO OF WHAT CANNOT BE GOVERNED ────────────────────────────
    //
    // The last rung, and the only one that is not reversible. Everything above
    // can be undone by a country that recovers: budgets come back, doctrines
    // are re-enacted, ships and men are rebuilt. Ground that has been released
    // is gone, so it is the final thing tried and only while the books are
    // still short after all of that.
    //
    // WHY IT IS HERE AT ALL. A country that bankrupts itself placating
    // minorities, cuts every settlement at once, and then disintegrates under
    // the unrest that follows is a known and repeatable failure -- the AI's
    // worst seat dies exactly that way on every ruler. The cascade could take
    // its money, its fleet and its army and still leave it holding the
    // provinces that were bankrupting it. This is the lever that was missing:
    // shed the region, survive smaller.
    //
    // Only ONE per turn. A country in real trouble would otherwise dissolve
    // itself in a single turn's cascade, which is not a government making a
    // hard choice, it is a country evaporating.
    int releasedProvinces = 0;
    std::string releasedName;
    // NOT "still short after the cascade" -- that was the first trigger and it
    // fired zero times in a 120-turn run, because disbanding troops clears
    // almost any single shortfall. See m_bankruptStreak: what this is for is
    // the country that cannot pay for itself turn after turn, which is the
    // spiral rather than the bad year.
    {
        auto stIt = m_bankruptStreak.find(countryId);
        const int streak = (stIt != m_bankruptStreak.end()) ? stIt->second : 0;
        if (streak >= RELEASE_BANKRUPT_STREAK) {
        const auto regions = releasableRegions(countryId);
        const ReleaseCandidate* shed = regions.empty() ? nullptr : &regions.front();
        if (shed) {
            // releasableRegions returns largest-population first, which is also
            // the most expensive to keep and the most unrest to be rid of.
            const int newCid = releaseNation(countryId, *shed);
            if (newCid > 0) {
                releasedProvinces = (int)shed->provinces.size();
                releasedName = shed->minority;
                // The books do not improve this turn -- the province's income
                // was already counted and the shortfall already owed -- so
                // `remaining` is deliberately untouched. What changes is next
                // turn: a smaller country with a smaller bill and less unrest.
                //
                // The clock restarts, so a country sheds at most one region per
                // five broke turns. Without this a long depression would
                // dissolve a country region by region every turn, which is not
                // a government making hard choices, it is one evaporating.
                m_bankruptStreak[countryId] = 0;
            }
        }
        }
    }

    // ── 7. What is still unpaid becomes unrest ──────────────────────────
    float unrest = 0.0f;
    if (remaining > 0.0f) {
        // Scaled by how deep the hole is relative to what the country earns: a
        // rounding-error shortfall is not the same event as owing a year's
        // income, and a flat number would treat them alike.
        const float severity = (cs.total > 0.01f) ? std::min(2.0f, remaining / cs.total) : 2.0f;
        unrest = BANKRUPTCY_UNREST_PER_TURN * (0.5f + severity);
        addWarWeariness(countryId, unrest);
    }

    if (isPlayer) {
        std::string msg = "BANKRUPT — the treasury is empty.";
        if (cutBudgets) msg += " Research and pacification funding cut to zero.";
        if (repealed > 0) msg += " " + std::to_string(repealed) +
                                 (repealed == 1 ? " doctrine repealed." : " doctrines repealed.");
        if (minorityCut > 0) msg += " Minority programmes cut back.";
        if (scrapped > 0) msg += " " + std::to_string(scrapped) +
                                 (scrapped == 1 ? " ship scrapped." : " ships scrapped.");
        if (disbanded > 0) msg += " " + formatPop(disbanded / 100) + " troops disbanded.";
        if (releasedProvinces > 0)
            msg += " " + od::i18n::properName(releasedName) + " has been granted independence (" +
                   std::to_string(releasedProvinces) + " provinces).";
        if (unrest > 0.0f) msg += " Unrest is rising.";
        addNotification(msg, Color{235, 110, 110, 255}, 10.0f);
        Audio::get().playSfx("deny");
    }
    printf("[ECONOMY] %d bankrupt: short %.2f, %d doctrine(s) repealed, %d minority cut(s), "
           "%d ships scrapped, %lld troops disbanded, %d province(s) released, "
           "unrest +%.1f (+%.1f%% rebellion, streak %d)\n",
           countryId, shortfall, repealed, minorityCut, scrapped, disbanded,
           releasedProvinces, unrest, bankruptcyUnrestFor(countryId),
           // The CHARGED figure and the streak that set it, not the constant:
           // the charge ramps (see BANKRUPT_UNREST_FULL_STREAK), so printing
           // BANKRUPTCY_UNREST_PCT here would have reported a number the game
           // does not apply on the first two turns of any bankruptcy.
           [&]{ auto i = m_bankruptStreak.find(countryId);
                return i != m_bankruptStreak.end() ? i->second : 1; }());
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
        // SHELL YOUR OWN GROUND OR AN ENEMY'S, IN RANGE, AND NOTHING ELSE.
        //
        // The ammo-researched test above was the only gate here, so any fleet
        // that had unlocked a shell could flatten ANY province on the map --
        // a neutral's, an ally's, from the far side of the world. As with
        // engage, the rule existed only in the aiming overlay, which binds the
        // player's mouse and neither the AI nor a modified client.
        if (tgt->countryId != countryId && !atWarCids(countryId, tgt->countryId)) {
            printf("  [SHIPBOMBARD] skip: prov %d belongs to %d, no war\n",
                   bo.targetProvince, tgt->countryId);
            m_pendingShipBombardOrders.erase(m_pendingShipBombardOrders.begin() + i);
            continue;
        }
        {
            auto cit = m_provinceCenters.find(bo.targetProvince);
            if (cit != m_provinceCenters.end()) {
                int sx, sy;
                m_landSea.lonLatToPixel((float)ship.lon, (float)ship.lat, sx, sy);
                const float ddx = cit->second.x - (float)sx, ddy = cit->second.y - (float)sy;
                if (std::sqrt(ddx * ddx + ddy * ddy) > shipMaxRangePx(ship)) {
                    printf("  [SHIPBOMBARD] skip: prov %d out of range\n", bo.targetProvince);
                    m_pendingShipBombardOrders.erase(m_pendingShipBombardOrders.begin() + i);
                    continue;
                }
            }
        }
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
                if (eff.fortChance > 0 && (simRand() % 100) < eff.fortChance) dmg += 1;
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
    for (size_t i = 0; i < m_pendingShipDisembarks.size(); ) {
        auto& do_ = m_pendingShipDisembarks[i];
        if (do_.shipIndex < 0 || do_.shipIndex >= (int)m_ships.size()) { ++i; continue; }
        auto& ship = m_ships[do_.shipIndex];
        if (ship.countryId != countryId) { ++i; continue; }
        int pid = do_.targetProvince;
        Province* dst = m_provinces.getProvinceById(pid);
        if (!dst) { ++i; continue; }
        // TROOPS COME ASHORE FROM WITHIN THE HULL'S RANGE, whoever ordered it.
        //
        // The player's Move overlay refuses a landing beyond shipMaxRangePx
        // and draws the circle to prove it; this resolver checked nothing, so
        // the rule bound the mouse and not the game. The AI's own landing
        // window was a flat 12 degrees -- 273 px against a boat's 200 -- and a
        // modified multiplayer client had no window at all.
        {
            int sx = 0, sy = 0;
            m_landSea.lonLatToPixel((float)ship.lon, (float)ship.lat, sx, sy);
            const auto cit = m_provinceCenters.find(pid);
            if (cit == m_provinceCenters.end()) {
                m_pendingShipDisembarks.erase(m_pendingShipDisembarks.begin() + i);
                continue;
            }
            const float dx = cit->second.x - (float)sx, dy = cit->second.y - (float)sy;
            if (std::sqrt(dx * dx + dy * dy) > shipMaxRangePx(ship)) {
                m_navLandingsOutOfRange++;
                if (m_config.aiDebug)
                    printf("[LANDING] cid=%d prov %d out of range, order dropped\n",
                           countryId, pid);
                m_pendingShipDisembarks.erase(m_pendingShipDisembarks.begin() + i);
                continue;
            }
        }
        // The same rule the land march obeys. Checked BEFORE the crew leaves
        // the ship: this path resolves through resolveAssault exactly as a
        // march does, so without it a landing was the way round the entry
        // rule -- troops put ashore in a country at peace, unable to take the
        // ground and left standing on it. The two paths drifted once before;
        // see the note just below.
        if (!mayEnterProvince(countryId, pid)) {
            if (m_config.aiDebug)
                printf("[LANDING] cid=%d prov %d is not at war with us, order dropped\n",
                       countryId, pid);
            m_pendingShipDisembarks.erase(m_pendingShipDisembarks.begin() + i);
            continue;
        }
        int crew = ship.crew * 100; // Scale to army internal units (100x)
        ship.crew = 0;
        if (crew <= 0) { m_pendingShipDisembarks.erase(m_pendingShipDisembarks.begin() + i); continue; }
        // The landing is an assault like any other -- see resolveAssault, which
        // is now the only place a province is fought over. This used to be its
        // own copy of that code, and the copies had drifted: this one left a
        // tenth of a failed landing inside the defender's province forever, and
        // it took an ALLY's coast off them if they had no troops standing on it.
        int survivors = 0;
        // Whose shore this is, decided BEFORE the assault resolves -- taking
        // the province changes the answer, and an invasion that succeeds must
        // not be counted as bringing the cargo home.
        const bool hostileShore = dst->countryId != countryId &&
                                  atWarCids(countryId, dst->countryId);
        const bool took = resolveAssault(countryId, pid, crew, survivors);
        if (m_ai) m_ai->noteLanding(countryId, hostileShore);
        if (m_config.aiDebug && took)
            printf("[LANDING] cid=%d took prov %d with %d of %d\n",
                   countryId, pid, survivors, crew);
        // Remove disembark order; delete the boat immediately
        int shipIdx = do_.shipIndex;
        m_pendingShipDisembarks.erase(m_pendingShipDisembarks.begin() + i);
        if (shipIdx >= 0 && shipIdx < (int)m_ships.size()) {
            if (std::getenv("OD_BOAT_TRACE"))
                printf("[BOAT] t%d cid=%d DISEMBARK-ERASE ship=%d owner=%d crew=%d at prov %d (ships %zu -> %zu)\n",
                       m_turnNumber, countryId, shipIdx, m_ships[shipIdx].countryId, m_ships[shipIdx].crew,
                       pid, m_ships.size(), m_ships.size() - 1);
            m_ships.erase(m_ships.begin() + shipIdx);
            forgetShipOrders(shipIdx);
        }
    }
}

// === recruitCap ===
//
// ONE CAP, ASKED IN ONE PLACE. It was written twice and the two copies did not
// agree: the recruitment panel multiplied pop/5 by the conscription lever and
// by an unrest factor, and the AI used a bare pop/5. So Mass Mobilisation's
// +45% manpower, Conscription's +30%, and thirteen more doctrines plus eight
// research nodes raised the human's ceiling and no AI's, for as long as they
// have existed. Neither copy was wrong on its own, which is why nothing caught
// it -- a rule that lives in Game_Render binds the local player and nobody
// else, and the resolver is where it stops being a rule about one seat.
//
// The cap is not re-applied in processRecruitments, which clamps only to the
// population actually there. It binds where an order is CREATED, so both
// callers have to ask.
long long Game::recruitCap(long long pool, int provinceId, int countryId) const {
    // Whether the AI is bound by the same ceiling as the player. OFF: the AI
    // keeps its bare pool/5 and the player keeps the panel's formula, so the
    // decision hash does not move. ON, the two callers are one rule.
    static const bool shared = std::getenv("OD_AI_RECRUIT_CAP") &&
                               atoi(std::getenv("OD_AI_RECRUIT_CAP")) != 0;

    long long cap = pool / 5;                    // 20% of the pool per turn
    if (cap <= 0) return 0;

    const bool isPlayer = (countryId == m_playerCountryId);
    if (!isPlayer && !shared) return cap;        // exactly what the AI had

    // FLOAT, not double, in both multiplications. The panel did it in float --
    // `long long * float` is a float multiply in C++ -- and this has to be the
    // same number for the player it was before, down to the last bit, or the
    // "nothing moved" claim below is only approximately true.
    const float mod = 1.0f + getTotalEffect("conscriptionPct", countryId) / 100.0f;
    cap = (long long)(cap * mod);

    // Unrest reduces willingness to be conscripted. Floored at 0.1 so a
    // province in open revolt still yields something rather than nothing.
    float unrestFactor = 1.0f - getProvinceRebellionChance(provinceId);
    if (unrestFactor < 0.1f) unrestFactor = 0.1f;
    cap = (long long)(cap * unrestFactor);

    return std::max(0LL, cap);
}

// === processRecruitments ===
void Game::processRecruitments(int countryId) {
    for (size_t i = 0; i < m_pendingRecruitments.size(); ) {
        auto& r = m_pendingRecruitments[i];
        auto pit = m_provinces.getProvinceById(r.provinceId);
        if (!pit || pit->countryId != countryId) { ++i; continue; }
        r.turnsRemaining--;
        if (r.turnsRemaining <= 0) {
            // ── THE MEN COME FROM SOMEWHERE ──
            //
            // Recruiting used to add soldiers and leave the province's
            // population untouched, so a province was an infinite source of
            // men: pop/5 per order, every order, for ever, and the only real
            // limit on an army was how many orders a country was allowed to
            // give. Benched by hand that produced a 102-million-man army from
            // eighty provinces without the population moving at all.
            //
            // Deducted HERE, in the resolver, so it binds every path that can
            // raise troops -- the AI, the player's panel, multiplayer orders
            // and the tutorial -- rather than in whichever of them was edited
            // most recently. Clamped, because a province may have shrunk
            // between the order and its arrival.
            //
            // AND A BETTER SOLDIER COSTS MORE OF THEM. `count` is soldiers; the
            // draw on population is count x the type's manpower multiplier, so
            // ten thousand mechanised take forty thousand people out of the
            // province and ten thousand militia take six. See PendingRecruitment
            // and TROOP_TYPES -- line infantry is 1.0, so a world that has only
            // ever had line infantry is deducted exactly as it always was.
            const double perMan = (double)troopCost(r.type).manpower;
            auto popIt = m_provincePopulations.find(r.provinceId);
            if (popIt != m_provincePopulations.end()) {
                const long long want = (long long)std::llround((double)r.count * perMan);
                const long long taken = std::min<long long>(want, popIt->second);
                popIt->second = std::max(0LL, popIt->second - taken);
                // Short of people: raise as many soldiers as the people cover,
                // rather than raising the whole order out of nobody.
                if (taken < want)
                    r.count = (int)std::min<long long>(
                        r.count, (long long)((double)taken / std::max(1e-9, perMan)));
            }
            if (r.count <= 0) {
                m_pendingRecruitments.erase(m_pendingRecruitments.begin() + i);
                continue;
            }
            // addTroopsTo rather than a second copy of the merge: it knows that
            // a province holds one stack per (country, TYPE), and this loop's
            // own find_if did not.
            addTroopsTo(r.provinceId, countryId, r.count, r.type);
            m_recruitedByType[(int)r.type] += r.count;
            m_pendingRecruitments.erase(m_pendingRecruitments.begin() + i);
        } else ++i;
    }
}

long long Game::availableManpower(int provinceId) const {
    auto it = m_provincePopulations.find(provinceId);
    long long pool = it == m_provincePopulations.end() ? 0 : it->second;
    // What the ORDERS will actually spend, which is soldiers x their kind. An
    // order for mechanised has already claimed four times its own size.
    for (const auto& r : m_pendingRecruitments)
        if (r.provinceId == provinceId)
            pool -= (long long)std::llround((double)r.count * (double)troopCost(r.type).manpower);
    return std::max(0LL, pool);
}

// === disbandableProvinces / disbandAllArmies / cancelAllDisbands ===
//
// The province panel's "Disband All" for one province, offered for all of
// them at once. Same order, same rules, same reversibility -- this queues one
// PendingDisbandOrder per province and the turn resolves them exactly as it
// resolves a hand-placed one.
int Game::disbandableProvinces(long long& troopsOut) const {
    troopsOut = 0;
    int n = 0;
    if (m_playerCountryId <= 0 || m_playerCountryId == SPC_CID) return 0;
    for (const auto& [pid, units] : m_provinceArmies) {
        const Province* p = m_provinces.getProvinceById(pid);
        if (!p || p->countryId != m_playerCountryId) continue;
        long long here = 0;
        for (const auto& u : units)
            if (u.countryId == m_playerCountryId && u.count > 0) here += u.count;
        if (here <= 0) continue;
        bool queued = false;
        for (const auto& d : m_pendingDisbandOrders)
            if (d.provinceId == pid) { queued = true; break; }
        if (queued) continue;
        troopsOut += here;
        n++;
    }
    return n;
}

int Game::disbandAllArmies() {
    if (m_playerCountryId <= 0 || m_playerCountryId == SPC_CID) return 0;
    std::vector<int> targets;
    for (const auto& [pid, units] : m_provinceArmies) {
        const Province* p = m_provinces.getProvinceById(pid);
        if (!p || p->countryId != m_playerCountryId) continue;
        bool any = false;
        for (const auto& u : units)
            if (u.countryId == m_playerCountryId && u.count > 0) { any = true; break; }
        if (!any) continue;
        bool queued = false;
        for (const auto& d : m_pendingDisbandOrders)
            if (d.provinceId == pid) { queued = true; break; }
        if (!queued) targets.push_back(pid);
    }
    // count 0 means "everything here", the same sentinel the panel's button
    // uses -- so a garrison that grows before the turn resolves still goes.
    for (int pid : targets) { traceDisband("PUSH-disbandAll", pid, 0, m_playerCountryId); m_pendingDisbandOrders.push_back({pid, 0}); }
    return (int)targets.size();
}

int Game::cancelAllDisbands() {
    if (m_playerCountryId <= 0) return 0;
    int cancelled = 0;
    for (auto it = m_pendingDisbandOrders.begin(); it != m_pendingDisbandOrders.end(); ) {
        const Province* p = m_provinces.getProvinceById(it->provinceId);
        if (p && p->countryId == m_playerCountryId) {
            it = m_pendingDisbandOrders.erase(it);
            cancelled++;
        } else ++it;
    }
    return cancelled;
}

// OD_DISBAND_TRACE: one line per disband order pushed or resolved, with
// the turn, the province, the order's count (0 = everything here) and the
// troops standing there at that moment, so a "Disbanding" remnant can be
// traced back to the click (or load) that queued it.
void Game::traceDisband(const char* origin, int pid, int count, int countryId) const {
    static const bool on = std::getenv("OD_DISBAND_TRACE") != nullptr;
    if (!on) return;
    int here = 0;
    auto aIt = m_provinceArmies.find(pid);
    if (aIt != m_provinceArmies.end())
        for (const auto& u : aIt->second) if (countryId <= 0 || u.countryId == countryId) here += u.count;
    const Province* p = m_provinces.getProvinceById(pid);
    fprintf(stderr, "[DISBAND] turn %d %s pid=%d owner=%d count=%d troopsHere=%d pending=%zu\n",
            m_turnNumber, origin, pid, p ? p->countryId : -1, count, here, m_pendingDisbandOrders.size());
}

// === processDisbandOrders ===
// === campaigns ===
//
// See docs/ai/CAMPAIGNS.md and `struct Campaign` in Game.h. Everything here
// is a rule rather than a decision, deliberately: the two withdraw
// experiments on 2026-09-05 measured that a head cannot judge "this is going
// badly" from one turn's evidence, so opening is the only choice anyone
// makes and closing is arithmetic.

const Game::Campaign* Game::campaignAt(int countryId, int targetProvince) const {
    for (const auto& c : m_campaigns)
        if (c.countryId == countryId && c.targetProvince == targetProvince) return &c;
    return nullptr;
}

const Game::Campaign* Game::campaignAgainst(int countryId, int enemyCid) const {
    for (const auto& c : m_campaigns)
        if (c.countryId == countryId && c.targetCountry == enemyCid) return &c;
    return nullptr;
}

const Game::Campaign* Game::campaignOf(int countryId) const {
    for (const auto& c : m_campaigns)
        if (c.countryId == countryId) return &c;
    return nullptr;
}

bool Game::openCampaign(const Campaign& c) {
    if (c.countryId <= 0 || c.targetCountry <= 0 || c.stagingProvince < 0) return false;
    // TWO AT A TIME since ParrotZero 8.5.0. A great power with forty
    // frontier provinces can prosecute two wars at once and a small one
    // cannot; whether the AI benefits from that is a measurement, not an
    // assumption, so the cap is a knob -- and the measurement came out
    // for two: N24 253 -> 259, N37 250 -> 266, N43 227 -> 231, floors
    // flat (41, 31, 36 -> 33). Cap 3 is byte-identical to cap 2 on every
    // metric, so a third campaign is never wanted; two is the whole gain.
    // See OD_CAMPAIGN_MAX.
    {
        // A SECOND COMMITMENT IS FOR A COUNTRY THAT CAN AFFORD ONE.
        //
        // Measured per seat on hold-out worlds: allowing two campaigns and
        // two wars was worth +176 to the USA seat and -141 to Norway. The
        // aggressive setting is not wrong, it is size-dependent -- a great
        // power holds two fronts, a small country holding two has nothing
        // at home. OD_BIG_PROVINCES (0 = off) gates the second campaign on
        // owning at least that many provinces.
        static const int bigProv = std::getenv("OD_BIG_PROVINCES")
                                 ? atoi(std::getenv("OD_BIG_PROVINCES")) : 0;
        static const int maxOpen = std::getenv("OD_CAMPAIGN_MAX")
                                 ? atoi(std::getenv("OD_CAMPAIGN_MAX")) : 1;
        if (bigProv > 0 && (int)provincesOf(c.countryId).size() < bigProv) {
            int openNow = 0;
            for (const auto& x : m_campaigns) if (x.countryId == c.countryId) ++openNow;
            if (openNow >= 1) return false;
        }
        int open = 0;
        for (const auto& x : m_campaigns) if (x.countryId == c.countryId) ++open;
        if (open >= std::max(1, maxOpen)) return false;
        // A second campaign against the same country is not a second war.
        for (const auto& x : m_campaigns)
            if (x.countryId == c.countryId && x.targetCountry == c.targetCountry) return false;
    }
    m_campaigns.push_back(c);
    m_campaigns.back().startedTurn = m_turnNumber;
    if (std::getenv("OD_CAMPAIGN_TRACE"))
        fprintf(stderr, "[CAMPAIGN] turn %d cid=%d OPEN vs=%d first=%d staging=%d men=%lld deadline=%d\n",
                m_turnNumber, c.countryId, c.targetCountry, c.targetProvince, c.stagingProvince,
                c.committedMen, c.deadlineTurns);
    return true;
}

bool Game::closeCampaign(int countryId, const char* why) {
    for (size_t i = 0; i < m_campaigns.size(); ++i) {
        if (m_campaigns[i].countryId != countryId) continue;
        if (std::getenv("OD_CAMPAIGN_TRACE"))
            fprintf(stderr, "[CAMPAIGN] turn %d cid=%d CLOSE vs=%d after %d turns (%s, took %d)\n",
                    m_turnNumber, countryId, m_campaigns[i].targetCountry,
                    m_turnNumber - m_campaigns[i].startedTurn, why ? why : "asked",
                    m_campaigns[i].provincesTaken);
        m_campaigns.erase(m_campaigns.begin() + i);
        return true;
    }
    return false;
}

void Game::processCampaigns() {
    if (m_campaigns.empty()) return;   // inert, and cheap to prove so
    static const bool trace = std::getenv("OD_CAMPAIGN_TRACE") != nullptr;
    for (size_t i = 0; i < m_campaigns.size(); ) {
        Campaign& c = m_campaigns[i];
        const char* why = nullptr;
        // The war aim: is there anything of theirs left next to us, and are
        // we still at war with them?
        const Country* me = m_countries.getCountry(c.countryId);
        const Country* them = m_countries.getCountry(c.targetCountry);
        int reachable = 0;
        if (me && them) {
            for (int pid : provincesOf(c.countryId)) {
                auto nIt = m_provinceNeighbors.find(pid);
                if (nIt == m_provinceNeighbors.end()) continue;
                for (int nid : nIt->second) {
                    const Province* np = m_provinces.getProvinceById(nid);
                    if (np && np->countryId == c.targetCountry) { ++reachable; break; }
                }
                if (reachable) break;
            }
        }
        const Province* cur = m_provinces.getProvinceById(c.targetProvince);
        // Counted ONCE: the objective is retired when it falls, so the next
        // turn does not count the same province again (the first version
        // reported "took 431" for a twelve-turn campaign).
        if (cur && cur->countryId == c.countryId) { c.provincesTaken++; c.targetProvince = -1; }
        if (!me || !them) why = "target gone";
        else if (!hasRelation(me->isoA3, them->isoA3, &CountryRelation::war)) why = "at peace";
        else if (reachable == 0) why = "BEATEN";
        else if (m_turnNumber - c.startedTurn >= c.deadlineTurns) why = "deadline";
        else {
            // Spent: the staging province can no longer feed it.
            long long here = 0;
            auto it = m_provinceArmies.find(c.stagingProvince);
            if (it != m_provinceArmies.end())
                for (const auto& u : it->second)
                    if (u.countryId == c.countryId) here += u.count;
            const Battle* b = battleAt(c.targetProvince, c.countryId);
            if (b) c.roundsFought = b->rounds;
            // NOT on the opening turns: the staging province has just sent
            // its men at the target, so "the garrison is small" is what a
            // working campaign looks like on turn one. Give it time to be
            // refilled by the recruitment and reinforcement it steers.
            const int age = m_turnNumber - c.startedTurn;
            if (age >= CAMPAIGN_GRACE_TURNS && !b && here < c.committedMen / 8) why = "spent";
        }
        if (why) {
            if (trace)
                fprintf(stderr, "[CAMPAIGN] turn %d cid=%d CLOSE vs=%d after %d turns (%s, took %d)\n",
                        m_turnNumber, c.countryId, c.targetCountry,
                        m_turnNumber - c.startedTurn, why, c.provincesTaken);
            m_campaigns.erase(m_campaigns.begin() + i);
            continue;
        }
        ++i;
    }
}

void Game::processDisbandOrders(int countryId) {
    for (size_t i = 0; i < m_pendingDisbandOrders.size(); ) {
        auto& d = m_pendingDisbandOrders[i];
        auto pit = m_provinces.getProvinceById(d.provinceId);
        if (!pit || pit->countryId != countryId) {
            if (pit && countryId == m_playerCountryId)   // a player order outliving its province
                traceDisband("SKIP-unowned", d.provinceId, d.count, countryId);
            ++i; continue;
        }
        traceDisband("FIRE", d.provinceId, d.count, countryId);
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
//
// BOAT FIRST, MEN SECOND. The old order deducted the troops from the
// garrison and then went looking for something to put them on: a boat
// within 50 px, else a spawn point found by scanning m_provincePixels for a
// coastal pixel. That index is built lazily (Game_Loading.cpp, see
// ensureProvincePixels) and reindexProvinceOwner only builds it outside
// training -- the note above that gate ("It only ever worked by accident")
// describes the identical failure for conquest overlays. So in every training
// and eval run the scan missed, no boat was ever spawned, and each embarkation
// without a pre-existing boat nearby deleted its troops; for a player it was
// intermittent in exactly the way the note predicts (fine after opening the
// Claims view, an army lost before). Measured 2026-09-04: 5,204 embarkations,
// 52 loaded-boat-turns, 0% landed. Two smaller leaks in the same block: the
// crew was totalRemoved / 100 in integer division, so under 100 units embarked
// as nothing, and a missing province centre or water body lost the men too.
//
// Now: count the whole crews that can go; find or place the boat -- an
// existing hull within 50 px, else the nav grid's own water cell by this
// province (portApproach, which needs no pixel index and is water by
// construction); and only then deduct exactly crews * 100 units. A failed
// embarkation is a no-op that leaves the garrison standing, and is counted.
void Game::processEmbarkations(int countryId) {
    for (size_t i = 0; i < m_pendingEmbarkations.size(); ) {
        auto& e = m_pendingEmbarkations[i];
        auto pit = m_provinces.getProvinceById(e.provinceId);
        if (!pit || pit->countryId != countryId) { ++i; continue; }
        e.turnsRemaining--;
        if (e.turnsRemaining > 0) { ++i; continue; }

        // 1. Whole crews only; the remainder stays ashore.
        int avail = 0;
        auto aIt = m_provinceArmies.find(e.provinceId);
        if (aIt != m_provinceArmies.end())
            for (const auto& u : aIt->second)
                if (u.countryId == countryId) avail += u.count;
        const int crews = std::min(std::max(0, e.count), avail) / 100;
        if (crews <= 0) {
            m_navEmbarkTooSmall++;
            m_pendingEmbarkations.erase(m_pendingEmbarkations.begin() + i);
            continue;
        }

        // 2. A boat to put them on, before anything leaves the province.
        // The harbour's own water: the nav grid's cell by this province. A
        // landlocked province gets an answer too (navCellNear searches 224 px
        // in every direction and would happily find a sea beyond a mountain
        // range), so ask isProvinceCoastal first. Everything below is measured
        // from this point, not from the province centre.
        double approachLon = 0.0, approachLat = 0.0;
        // isProvinceCoastal reads m_provincePixels and answers "no" for every
        // province while that index is unbuilt -- i.e. in every training and
        // eval run (the same absent index that stopped boats spawning). A
        // harbour is coastal by construction, so ask the port table first; the
        // pixel test stays for the odd non-port coastal province.
        const bool coastal = (m_provincePorts.count(e.provinceId) > 0 ||
                              isProvinceCoastal(e.provinceId)) &&
                             portApproach(e.provinceId, approachLon, approachLat);
        NavyShip* boat = nullptr;
        if (coastal) {
            auto cit = m_provinceCenters.find(e.provinceId);
            if (cit != m_provinceCenters.end()) {
                const float cx = cit->second.x, cy = cit->second.y;
                float bestDist = 50.0f * 50.0f;
                for (auto& ship : m_ships) {
                    if (ship.countryId != countryId || ship.type != "boat") continue;
                    int spx, spy;
                    m_landSea.lonLatToPixel((float)ship.lon, (float)ship.lat, spx, spy);
                    const float dx = spx - cx, dy = spy - cy;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 >= bestDist) continue;
                    // 50 px is less than an isthmus: a hull in the Gulf of Mexico
                    // is within 50 px of a Pacific-coast province. Same sea body
                    // as the harbour, or it is not a boat these men can use.
                    if (!navReachable(ship.lon, ship.lat, approachLon, approachLat)) {
                        m_navEmbarkWrongSea++;
                        continue;
                    }
                    bestDist = d2; boat = &ship;
                }
            }
        }
        double spawnLon = approachLon, spawnLat = approachLat;
        bool canSpawn = false;
        if (!boat && coastal) {
            int wx, wy;
            m_landSea.lonLatToPixel((float)spawnLon, (float)spawnLat, wx, wy);
            canSpawn = !m_landSea.isLand(wx, wy);
            // portApproach returns a navigable cell by construction; if this
            // ever fires the nav grid and the land raster disagree, which is a
            // different and more interesting bug than a failed embarkation.
            if (!canSpawn) {
                m_navGridLandDisagree++;
                if (m_config.aiDebug)
                    printf("[EMBARK] cid=%d prov %d: nav grid cell is LAND at (%d,%d) -- grid/raster disagree\n",
                           countryId, e.provinceId, wx, wy);
            }
        }
        if (!boat && !canSpawn) {
            m_navEmbarkNoBoat++;
            if (m_config.aiDebug)
                printf("[EMBARK] cid=%d prov %d: no boat and no water to spawn one -- order dropped, garrison kept\n",
                       countryId, e.provinceId);
            m_pendingEmbarkations.erase(m_pendingEmbarkations.begin() + i);
            continue;
        }

        // 3. Only now: exactly crews * 100 units leave the garrison.
        int toTake = crews * 100, removed = 0;
        for (auto& u : aIt->second) {
            if (u.countryId != countryId) continue;
            const int r = std::min(toTake - removed, u.count);
            u.count -= r;
            removed += r;
            if (removed >= toTake) break;
        }
        aIt->second.erase(std::remove_if(aIt->second.begin(), aIt->second.end(),
            [](auto& u) { return u.count <= 0; }), aIt->second.end());
        if (aIt->second.empty()) m_provinceArmies.erase(e.provinceId);

        // 4. Aboard.
        if (boat) {
            boat->crew += crews;
        } else {
            NavyShip ns;
            ns.lon = spawnLon; ns.lat = spawnLat;
            ns.type = "boat"; ns.countryId = countryId;
            ns.health = 100; ns.crew = crews;
            m_ships.push_back(ns);
            if (m_config.aiDebug)
                printf("[EMBARK] Spawned boat for %d troops at province %d\n",
                       crews * 100, e.provinceId);
        }
        m_navMenEmbarked += (long long)crews * 100;
        m_pendingEmbarkations.erase(m_pendingEmbarkations.begin() + i);
    }
}

// === processScrapShips ===
void Game::processScrapShips(int countryId) {
    // AN ORDER THIS COUNTRY CANNOT CARRY OUT IS NOT THIS COUNTRY'S TO DISCARD.
    //
    // This used to erase every entry it looked at, whether or not it had done
    // anything with it -- the erase sat outside the ownership test. Turn
    // resolution calls this once per country, and processTurn walks
    // m_countries.getAll(), which is an unordered_map, so "first country" is
    // whatever the hash order happens to be. Whichever country that was
    // drained the whole queue on its own turn, and every scrap order belonging
    // to anybody else was thrown away without being carried out.
    //
    // For the player that meant clicking Destroy Ship, watching "Scrapping..."
    // appear over the hull, ending the turn, and finding the ship still there
    // -- unless their country happened to be first in the hash order, in which
    // case it worked. Intermittent, and dependent on nothing the player can
    // see. It was reported as "scrapping ships doesn't work, I don't think".
    //
    // Every sibling here -- disband, recruit, embark, bombard, disembark --
    // already had the right shape: skip what is not yours and leave it for the
    // turn of the country that owns it. This is that shape.
    for (size_t i = 0; i < m_pendingScrapShips.size(); ) {
        auto& s = m_pendingScrapShips[i];
        if (s.shipIndex < 0 || s.shipIndex >= (int)m_ships.size()) {
            // A stale index points at no ship at all, so no country's turn can
            // ever action it. Drop it rather than leave it circling forever.
            m_pendingScrapShips.erase(m_pendingScrapShips.begin() + i);
            continue;
        }
        if (m_ships[s.shipIndex].countryId != countryId) { ++i; continue; }
        m_ships[s.shipIndex].countryId = UNC_CID; // Mark as sunk/scrapped
        m_pendingScrapShips.erase(m_pendingScrapShips.begin() + i);
    }
}

// === scrappableShips / scrapAllShips / cancelAllScraps ===
//
// The fleet's answer to disbandAllArmies, queueing the same PendingScrapShip
// the ship panel's own Destroy button queues -- so the turn resolves them
// through the loop above and every hull shows "Scrapping..." until it does.
int Game::scrappableShips() const {
    if (m_playerCountryId <= 0 || m_playerCountryId == SPC_CID) return 0;
    int n = 0;
    for (size_t i = 0; i < m_ships.size(); ++i) {
        if (m_ships[i].countryId != m_playerCountryId) continue;
        bool queued = false;
        for (const auto& ss : m_pendingScrapShips)
            if (ss.shipIndex == (int)i) { queued = true; break; }
        if (!queued) n++;
    }
    return n;
}

int Game::scrapAllShips() {
    if (m_playerCountryId <= 0 || m_playerCountryId == SPC_CID) return 0;
    int queued = 0;
    for (size_t i = 0; i < m_ships.size(); ++i) {
        if (m_ships[i].countryId != m_playerCountryId) continue;
        bool already = false;
        for (const auto& ss : m_pendingScrapShips)
            if (ss.shipIndex == (int)i) { already = true; break; }
        if (already) continue;
        m_pendingScrapShips.push_back({(int)i});
        queued++;
    }
    return queued;
}

int Game::cancelAllScraps() {
    if (m_playerCountryId <= 0) return 0;
    int cancelled = 0;
    for (auto it = m_pendingScrapShips.begin(); it != m_pendingScrapShips.end(); ) {
        const bool mine = it->shipIndex >= 0 && it->shipIndex < (int)m_ships.size() &&
                          m_ships[it->shipIndex].countryId == m_playerCountryId;
        if (mine) { it = m_pendingScrapShips.erase(it); cancelled++; }
        else ++it;
    }
    return cancelled;
}

// === processArmyMovement ===
void Game::processArmyMovement(int countryId) {
    // WHAT A PERCENTAGE IS A PERCENTAGE OF.
    //
    // Every order says "move pct of the garrison", and the executor used to
    // take that share of whatever was left when the order came up. Two orders
    // of 50% therefore moved 50% and then 25%, leaving a quarter of the army
    // standing in a province the player had emptied on purpose -- the troops
    // that "get pushed back to where they attacked from". The move panel
    // disagrees in writing: it caps the orders out of one province at 100% in
    // total and draws a red limit mark at the remainder, which only makes
    // sense if a percentage is a share of the garrison as it stood at the
    // start of the turn.
    //
    // So that is what it now is. The garrison is snapshotted the first time an
    // order out of that province is executed, every share is taken from the
    // snapshot, and the running total is clamped to what is actually there --
    // so orders summing past 100% (nothing stops a mod or the reflexes below
    // from queueing them) share out the army instead of conjuring one.
    // KEYED BY PROVINCE **AND KIND**, not by province alone. "50% of the
    // militia" and "50% of the line infantry" are two orders sharing one
    // province and two different denominators; on a single per-province key the
    // second would have taken half of what the first left, which is precisely
    // the bug the cumulative-share rule below exists to prevent, reintroduced
    // one level up. Key: province << 4 | (kind + 1), so -1 (the whole garrison)
    // gets its own slot.
    auto shareKey = [](int pid, int type) -> long long {
        return ((long long)pid << 4) | (long long)(type + 1);
    };
    std::unordered_map<long long, long long> baseGarrison;  // troops at turn start
    std::unordered_map<long long, long long> movedOut;      // troops already sent
    std::unordered_map<long long, int> movedPct;            // share already allocated

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
        // ARMIES WALK. They do not teleport, and they do not multiply.
        //
        // Nothing here asked whether the destination touches the source, so
        // any owned province could send troops to ANY province on the map in
        // one turn. Nothing bounded pct either, so a pct of 1000 moved ten
        // times the garrison and left the source on a negative count --
        // soldiers conjured out of nothing.
        //
        // Neither was reachable through the game's own UI, which offers
        // neighbours and a 0-100 slider, and the AI only ever names adjacent
        // provinces. But the multiplayer host takes these orders off the wire
        // and validates only that the sender OWNS the source province, so both
        // were reachable by a modified client. Checked here because this is
        // where every order arrives, whoever wrote it.
        {
            bool adjacent = false;
            auto nIt = m_provinceNeighbors.find(mo.fromProvince);
            if (nIt != m_provinceNeighbors.end())
                for (int n : nIt->second) if (n == mo.toProvince) { adjacent = true; break; }
            if (!adjacent) {
                m_pendingMoveOrders.erase(m_pendingMoveOrders.begin() + i);
                continue;
            }
        }
        // Not into a country we are at peace with. BESIDE THE ADJACENCY CHECK,
        // not below the deduction, because both answer the same question --
        // whether this order may be carried out at all -- and the answer has to
        // arrive before anything is spent on it.
        //
        // It was below. By then the troops had been taken off the source stack,
        // that stack erased if it emptied, and its province erased from
        // m_provinceArmies if IT emptied -- and the refusal path then wrote the
        // men back through `uIt`, an iterator both of those erases had already
        // invalidated. A write through a dangling iterator into a freed vector:
        // it corrupts whatever now occupies the memory, so what it does depends
        // on the allocator rather than on the game, and "the turn hangs" is one
        // of the things it can do.
        //
        // Refused here, nothing has been taken from anybody: the order is
        // dropped and the garrison stays exactly where it was.
        if (!mayEnterProvince(countryId, mo.toProvince)) {
            m_pendingMoveOrders.erase(m_pendingMoveOrders.begin() + i);
            continue;
        }
        // How many soldiers this share is, in whole men.
        //
        // Integer arithmetic throughout: a garrison is an int and these run to
        // millions, so `count * pct` in float stops being exact above 2^24 and
        // a 100% order could ask for more men than the province held.
        // THE WHOLE GARRISON, ACROSS KINDS. A country holds one stack per
        // (country, type) in a province now, so "50% of the garrison" is half
        // of everything it has there -- not half of whichever stack the search
        // happened to find first, which would have quietly moved only the line
        // infantry and left the militia standing.
        // "The whole garrison" or "the militia": mo.troopType is -1 or a kind,
        // and it decides both what the percentage is a percentage OF and which
        // stacks the men come out of. An order for a kind the province no
        // longer holds moves nobody, which is correct -- they died or left.
        const int wantType = mo.troopType;
        auto inOrder = [&](const ArmyUnit& u) {
            return u.countryId == countryId && (wantType < 0 || (int)u.type == wantType);
        };
        long long here = 0;
        for (const auto& u : srcArmies) if (inOrder(u)) here += u.count;
        if (here <= 0) { m_pendingMoveOrders.erase(m_pendingMoveOrders.begin() + i); continue; }
        const long long sk = shareKey(mo.fromProvince, wantType);
        auto baseIt = baseGarrison.find(sk);
        if (baseIt == baseGarrison.end())
            baseIt = baseGarrison.emplace(sk, here).first;
        const long long base = baseIt->second;
        long long& sent = movedOut[sk];
        const int pct = std::clamp(mo.pct, 0, 100);

        // ── SHARES OF A GARRISON MUST ADD UP TO THE GARRISON ──
        //
        // This was `base * pct / 100` per order, and each order truncated
        // independently. Divide a garrison in two and both halves round DOWN:
        // an odd garrison of 100,001 sends 50,000 twice and leaves one man
        // standing, which the map draws as an army marker reading "<1". Half of
        // all garrisons are odd, so a player splitting a province in two saw it
        // about every other time, and reported it as happening consistently.
        //
        // The fix is to make the running total exact rather than each share
        // exact. Each order takes the difference between the cumulative share
        // up to and including it and the cumulative share before it, so the
        // truncations telescope: at 100% the sum is floor(base * 100 / 100),
        // which is the whole garrison, with no remainder to strand.
        //
        // Percentages, not men, because the percentages are what the player set
        // and what the arrows draw -- and a rule about how shares are rounded
        // has to be expressed in the same terms the player is thinking in.
        int& cumPct = movedPct[sk];
        const int nextPct = std::min(100, cumPct + pct);
        long long toMove = base * nextPct / 100 - base * cumPct / 100;
        cumPct = nextPct;

        if (toMove > base - sent) toMove = base - sent;      // the army is finite
        if (toMove > here) toMove = here;
        // Nothing left to send this turn -- the order stands and tries again
        // next turn, which is what it did before and what a player who queued
        // more than a province holds would expect.
        if (toMove <= 0) { ++i; continue; }
        sent += toMove;

        // Take the share out of every kind in proportion, and remember WHAT
        // marched: a mixed garrison sends a mixed column.
        ForceComposition moving;
        {
            // THE SHARES TELESCOPE, exactly as they do for the percentages
            // above and for the same reason -- see the note on cumulative
            // shares. `count x toMove / here` computed per stack in floating
            // point loses a man on a large garrison (the product passes 2^53),
            // and a single soldier's difference cascades through a campaign.
            // Running the cumulative total in integers makes every share exact
            // and their sum exactly `toMove`; with one stack it is `toMove`
            // itself, bit for bit, which is what makes an all-line world
            // identical to the one before kinds existed.
            long long seen = 0, allocated = 0;
            for (auto& u : srcArmies) {
                if (!inOrder(u) || u.count <= 0) continue;
                seen += u.count;
                long long want = toMove * seen / here - allocated;
                if (want > u.count) want = u.count;
                if (want < 0) want = 0;
                u.count   -= (int)want;
                allocated += want;
                moving.add(u.type, want);
            }
            toMove = moving.total();
            if (toMove <= 0) { ++i; continue; }
        }
        srcArmies.erase(std::remove_if(srcArmies.begin(), srcArmies.end(),
                                       [](const ArmyUnit& u) { return u.count <= 0; }),
                        srcArmies.end());
        if (srcArmies.empty()) m_provinceArmies.erase(mo.fromProvince);

        // ── ALREADY FIGHTING HERE? THEN THESE MEN ARE REINFORCEMENTS ──
        //
        // Not a second assault. Two assaults on one province in one turn would
        // each be weighed against the frontage separately, which is a way of
        // bringing more men to bear than the ground holds -- exactly what the
        // frontage exists to prevent -- and it is also not what the player
        // meant. They meant "send more men to that fight".
        if (Battle* b = battleAt(mo.toProvince, countryId)) {
            for (int t = 0; t < (int)TROOP_TYPE_COUNT; ++t) b->men.men[t] += moving.men[t];
            ++m_battlesReinforced;
            m_pendingMoveOrders.erase(m_pendingMoveOrders.begin() + i);
            continue;
        }

        // One assault, resolved the same way whether the troops walked or
        // landed. See resolveAssault: it fights the WHOLE garrison rather than
        // the first stack it finds, and places the survivors itself.
        ForceComposition survivors;
        // ── THE ORDER LEAVES THE QUEUE BEFORE THE ASSAULT, NOT AFTER IT ──
        //
        // resolveAssault can capture the province, and captureProvince drops
        // every queued move order out of the captured ground -- other countries'
        // orders, anywhere in this vector. Erasing by index afterwards removed
        // whichever order had slid into slot i, left this one queued to march
        // again next turn, and when the dropped order sat at the tail, erased
        // one past the end. libc++ memmoves a negative count there and the turn
        // dies in processArmyMovement (1914:SWE seed 20260801, turn 30, with a
        // random player); libstdc++ silently pops the last order instead, which
        // is why no Linux run or CI job ever showed it.
        const int fromPid = mo.fromProvince, toPid = mo.toProvince;
        m_pendingMoveOrders.erase(m_pendingMoveOrders.begin() + i);
        const size_t queued = m_pendingMoveOrders.size();
        const size_t aheadDropped = (size_t)std::count_if(
            m_pendingMoveOrders.begin(), m_pendingMoveOrders.begin() + i,
            [toPid](const PendingMoveOrder& o) { return o.fromProvince == toPid; });
        // The source province is the fallback: men who never got into the
        // fight march back to where they came from. See resolveAssault.
        const bool took = resolveAssault(countryId, toPid, moving, survivors, fromPid);
        // Orders before i that left with the province shift the unvisited ones
        // down by that many; step back so none of them is skipped.
        if (m_pendingMoveOrders.size() < queued) i -= aheadDropped;
        if (m_config.aiDebug && took)
            printf("[BATTLE] cid=%d took prov %d from prov %d with %lld of %lld\n",
                   countryId, toPid, fromPid, survivors.total(), toMove);
    }
}

// === forgetShipOrders ===
//
// One hull has left the world; every queue that names a hull by INDEX has to be
// told. Two things happen to each order: one that referred to the departing
// ship is dropped, and one that referred to a ship after it is shifted down.
//
// The shift was already done at both removal sites, by two copies of the same
// lambda. The DROP was not done anywhere, and it did not previously matter much
// because a ship order was created and consumed inside one turn -- an order
// left pointing at a dead index was about to be thrown away regardless. A move
// order now survives the turn (see PendingShipMoveOrder), so an index left
// dangling is an order that silently transfers to whichever hull slid into that
// slot: a sunk transport's voyage inherited by an unrelated destroyer.
void Game::forgetShipOrders(int removedIdx) {
    // The overlay's route cache is keyed by ship index, and every index above
    // the removed one is about to change meaning. Dropped whole rather than
    // shifted: it is display state, it costs one BFS per visible order to
    // rebuild, and a route drawn for the wrong hull is worse than no route.
    m_shipRoutePreview.clear();
    auto fix = [&](auto& vec, auto refersToRemoved) {
        for (auto it = vec.begin(); it != vec.end(); ) {
            if (refersToRemoved(*it)) { it = vec.erase(it); continue; }
            ++it;
        }
        for (auto& o : vec) if (o.shipIndex > removedIdx) o.shipIndex--;
    };
    fix(m_pendingShipMoveOrders,    [&](const auto& o){ return o.shipIndex == removedIdx; });
    fix(m_pendingShipBombardOrders, [&](const auto& o){ return o.shipIndex == removedIdx; });
    fix(m_pendingShipDisembarks,    [&](const auto& o){ return o.shipIndex == removedIdx; });
    fix(m_pendingScrapShips,        [&](const auto& o){ return o.shipIndex == removedIdx; });
    // Engage names TWO hulls, and an order whose TARGET has been sunk is as
    // dead as one whose firer has. Forgetting targetIndex here is what skewed
    // every queued engagement one ship over after a disembark.
    fix(m_pendingShipEngageOrders,
        [&](const PendingShipEngageOrder& o){
            return o.shipIndex == removedIdx || o.targetIndex == removedIdx; });
    for (auto& eo : m_pendingShipEngageOrders)
        if (eo.targetIndex > removedIdx) eo.targetIndex--;
    // The player's own selection, which the disembark path never shifted -- so
    // landing an army quietly moved the selection onto somebody else's hull.
    for (auto it = m_selectedShipIndices.begin(); it != m_selectedShipIndices.end(); ) {
        if (*it == removedIdx) it = m_selectedShipIndices.erase(it);
        else { if (*it > removedIdx) (*it)--; ++it; }
    }
}

// === processNavyMovement ===
//
// ONE TURN'S STEAMING ALONG A ROUTE THAT SURVIVES THE TURN.
//
// This used to take the destination, walk the straight line to it in 32 steps,
// stop at the last water, and then DELETE the order. Three consequences, all
// measured on the shipped scenarios:
//
//   - A crossing that had to round a headland never could. The hull was thrown
//     back on the same beach and re-aimed down the same chord every turn. 80%
//     of every ship move in the world covered less than a twentieth of what it
//     asked for, and after the aim-point bugs were fixed, every remaining one
//     was a destination that WAS water, in the SAME sea, behind a cape.
//   - The player could only ever give an order that finished this turn, so an
//     ocean crossing was eight clicks on eight turns per transport.
//   - Game::navRoute -- a working sea router with a real water graph behind it
//     -- had no caller in the resolver at all. The AI used it to pick an aim
//     point and then threw the rest of the path away.
//
// So the order now carries the route and the resolver spends a turn's range
// along it. The router's own legs are trusted: they run between adjacent cells
// of a graph flood-filled from the raster, and re-deriving that by sampling the
// chord between two proven water cells only rejects legs the router already
// proved -- which is most of what the old code was rejecting. The FINAL leg is
// different, because its endpoint was chosen by a person or by the AI rather
// than by the router, so that one is still walked and clamped at the coast.
void Game::processNavyMovement(int countryId) {
    if (std::getenv("OD_BOAT_TRACE") && !m_pendingShipMoveOrders.empty()) {
        printf("[BOAT] t%d cid=%d ROUTER-ENTER %zu order(s) pending:", m_turnNumber, countryId, m_pendingShipMoveOrders.size());
        for (const auto& o : m_pendingShipMoveOrders) {
            const bool ok = o.shipIndex >= 0 && o.shipIndex < (int)m_ships.size();
            printf(" [ship=%d owner=%d crew=%d planned=%d route=%zu]", o.shipIndex,
                   ok ? m_ships[o.shipIndex].countryId : -1, ok ? m_ships[o.shipIndex].crew : -1,
                   o.planned ? 1 : 0, o.route.size());
        }
        printf("\n");
    }
    for (size_t i = 0; i < m_pendingShipMoveOrders.size(); ) {
        auto& mo = m_pendingShipMoveOrders[i];
        if (mo.shipIndex < 0 || mo.shipIndex >= (int)m_ships.size()) { ++i; continue; }
        auto& ship = m_ships[mo.shipIndex];
        if (ship.countryId != countryId) { ++i; continue; }

        // ── Plan, once ──
        //
        // Every caller writes a destination and leaves the route empty, so this
        // is where a click becomes a voyage. Planned once and remembered:
        // re-planning each turn would cost a BFS per hull per turn and, worse,
        // would let a fleet oscillate between two equal-length paths.
        //
        // A destination the router cannot reach still gets an order -- the
        // single-leg straight line, which is what this function always did.
        // That keeps a mod or an old order that names a point in a lake, or a
        // map with no nav grid at all, behaving exactly as it used to instead
        // of silently doing nothing.
        if (!mo.planned) {
            mo.planned = true;
            mo.route.clear();
            std::vector<std::pair<double, double>> way;
            const bool routed = navRoute(ship.lon, ship.lat, mo.destLon, mo.destLat, way);
            if (routed)
                mo.route = std::move(way);
            // OD_BOAT_TRACE: which of navRoute's exits fired for a loaded boat
            // whose reachability test said yes (journal 37c: 158 stuck/80 turns).
            if (!routed && ship.crew > 0 && std::getenv("OD_BOAT_TRACE")) {
                int sx, sy, gx, gy;
                m_landSea.lonLatToPixel((float)ship.lon, (float)ship.lat, sx, sy);
                m_landSea.lonLatToPixel((float)mo.destLon, (float)mo.destLat, gx, gy);
                const int a = m_nav.ready() ? navCellNear(m_nav, sx, sy) : -2;
                const int b = m_nav.ready() ? navCellNear(m_nav, gx, gy) : -2;
                printf("[BOAT] t%d cid=%d ship=%d NOROUTE grid=%d startCell=%d goalCell=%d sameComponent=%d boatOnLand=%d\n",
                       m_turnNumber, countryId, mo.shipIndex, m_nav.ready() ? 1 : 0, a, b,
                       (a >= 0 && b >= 0 && m_nav.component[a] == m_nav.component[b]) ? 1 : 0,
                       m_landSea.isLand(sx, sy) ? 1 : 0);
            }
            // The destination itself is always the last leg. navRoute's final
            // waypoint is the water cell NEAREST the destination, which for a
            // port approach is the same point and for a hand-picked one is up
            // to a cell away.
            if (mo.route.empty() ||
                mo.route.back().first != mo.destLon || mo.route.back().second != mo.destLat)
                mo.route.emplace_back(mo.destLon, mo.destLat);
        }

        // A hull that is ALREADY beached -- from a save written before any of
        // this existed, or from a map whose ship placement disagrees with its
        // own raster -- has to be able to get off. It takes the FIRST water it
        // finds along the leg instead of the last, so old worlds heal rather
        // than staying stuck for ever.
        const bool beached = m_landSea.isLand((float)ship.lon, (float)ship.lat);

        // RANGE IS ENFORCED HERE, not in the UI that draws the circle. The
        // overlay refused an out-of-range click, so the rule bound the player
        // and nobody else: the AI wrote whatever destination it liked straight
        // into this queue, and multiplayer orders arrive here without ever
        // having passed a hover check.
        double budget = shipMaxRangeDeg(ship);
        const double startLon = ship.lon, startLat = ship.lat;
        // What the whole turn was ASKED for, for the routing diagnostic below:
        // the distance to this turn's furthest reachable point, not to a
        // destination that may be six turns away. Anything else would report a
        // voyage in progress as a stall for every turn but its last.
        double want = 0.0, got = 0.0;
        // See the skip note below.
        int skips = 0;
        const int MAX_SKIPS = 4;

        while (budget > 1e-9 && !mo.route.empty()) {
            const double legLon = mo.route.front().first;
            const double legLat = mo.route.front().second;
            // Wrapped: see Game::lonDelta. Both the LENGTH and the DIRECTION
            // come from this, so an antimeridian leg is now a short hop east
            // rather than a voyage west around the world.
            const double dLon = lonDelta(ship.lon, legLon), dLat = legLat - ship.lat;
            const double legDist = std::sqrt(dLon * dLon + dLat * dLat);
            if (legDist < 1e-9) { mo.route.erase(mo.route.begin()); continue; }

            const bool finalLeg = (mo.route.size() == 1);
            const double take = std::min(budget, legDist);
            want += take;
            // A SHORT HOP ONTO THE GRID IS NOT A COASTAL CRAWL. The route's
            // waypoints are navigable cell centres, but the hull may sit at a
            // coastal pixel with a spit of land between it and its own cell;
            // stepped pixel by pixel that leg moves zero, the skip rule burns
            // its four skips on the next legs, and the order dies as "stuck"
            // (71 of 100 loaded-boat orders per 80 turns, journal 37f, with
            // navRoute never failing). A leg no longer than two cells to a
            // non-final waypoint is taken as a jump, the way a completed
            // intermediate leg already is; the land test keeps guarding the
            // long legs and the final approach, where it means something.
            {
                // Eight cells, not two: navCellNear searches r = 0..7 cells, so a
                // hull in water the grid does not cover (a bay narrower than a
                // cell) can be up to eight cells from the route's first
                // waypoint, and the straight leg to it crosses the bay's shore.
                // Every later leg is one cell to the next, so only the grid
                // entry can be this long (v10 check: stuck legs of 9.5-12.9
                // cells, hull on water, first step on water).
                // Sixteen cells and SAILED BY BUDGET, not completed in one turn:
                // the observed entry legs are 9.5-12.9 cells (13-18 degrees),
                // longer than a hull's 8.8-degree turn, so a hop that must
                // finish the leg never fired (v10.1 check identical to v10).
                // The hull is in real water the grid cannot see and the
                // waypoint is a navigable cell, so this leg is taken straight
                // without the coast stop, as far as the budget allows.
                // ...AND EVERY PLANNED LEG BETWEEN CELLS. The extended trace
                // (48 of 48 stuck orders): hull on water, route planned, and the
                // router's own first stride along the leg on LAND -- the straight
                // line between two navigable cell centres clips a land corner,
                // which on a 32-px grid is ordinary, and the coast stop treats
                // it as a wall. A planned leg to a non-final waypoint is sailed
                // by budget; the pixel-level coast stop keeps the final approach
                // and unplanned straight-line fallbacks, where it means something.
                const double hopDeg = 16.0 * 360.0 * (double)m_nav.cell /
                                      std::max(1.0, (double)m_landSea.getWidth());
                if (!finalLeg && mo.route.size() > 1 && (mo.planned || legDist <= hopDeg)) {
                    const double f = take / legDist;
                    // WRAPPED. This is the one mover that takes its leg on
                    // trust -- no coast walk, by design, because a planned leg
                    // between two navigable cells clips land corners that the
                    // pixel test would read as a wall. That trust is fine for
                    // the land test and fatal for the arithmetic: with a
                    // correctly-signed dLon a hull at 179E steps to 182E, which
                    // is off the map, and every later lookup reads whatever is
                    // at that coordinate. It measured as eleven hulls "on land"
                    // in an eval that had never reported one.
                    double nlon = wrapLon(ship.lon + dLon * f), nlat = ship.lat + dLat * f;
                    // A TRUSTED LEG MAY BE CROSSED, BUT NOT PARKED ON.
                    //
                    // The leg is sailed on trust because the straight line
                    // between two navigable cells clips land corners that the
                    // pixel test would read as a wall -- that is the whole
                    // point of this branch, and crossing such a corner mid-leg
                    // is fine. ENDING THE TURN on one is not: the hull sits on
                    // land until it next moves, and every reader of the world
                    // sees a ship in a field.
                    //
                    // It only became reachable once the antimeridian fix let
                    // hulls actually traverse their legs -- before, a wrapped
                    // leg measured 359 degrees, f was a rounding error, and the
                    // hull never left the water it was already in. The eval's
                    // beached invariant caught it immediately: 0 hulls on land
                    // in every run before, 19 after.
                    //
                    // So the crossing stands and only the RESTING PLACE is
                    // checked: walk back along the leg to the last water. The
                    // leg's own start is water, so this always terminates.
                    if (m_landSea.isLand((float)nlon, (float)nlat)) {
                        const int BACK = 24;
                        for (int k = BACK - 1; k >= 0; --k) {
                            const double bt = f * (double)k / (double)BACK;
                            const double bl = wrapLon(ship.lon + dLon * bt);
                            const double ba = ship.lat + dLat * bt;
                            if (!m_landSea.isLand((float)bl, (float)ba)) {
                                nlon = bl; nlat = ba; break;
                            }
                        }
                    }
                    ship.lon = nlon; ship.lat = nlat;
                    budget -= take;
                    if (take >= legDist - 1e-9) { mo.route.erase(mo.route.begin()); continue; }
                    break;                          // partial entry leg ends the turn
                }
            }

            if (!finalLeg && take >= legDist - 1e-9) {
                // A whole router leg. Its endpoint is a water pixel the graph
                // chose, so the hull lands at sea by construction and the
                // chord between two adjacent cells is the router's business.
                ship.lon = wrapLon(legLon); ship.lat = legLat;
                budget -= legDist;
                mo.route.erase(mo.route.begin());
                continue;
            }

            // Either the last leg, or as far along this one as the turn
            // reaches. Walked and clamped, because the hull is going to STOP
            // somewhere along here and that somewhere has to be water.
            const double tEnd = take / legDist;
            const int STEPS = 32;
            double bestLon = ship.lon, bestLat = ship.lat;
            for (int k = 1; k <= STEPS; ++k) {
                const double t = tEnd * (double)k / (double)STEPS;
                // wrapLon because a wrapped dLon can carry the sample past
                // 180; isLand is asked about a real point on the map, not a
                // longitude of 181.
                const double lon = wrapLon(ship.lon + dLon * t);
                const double lat = ship.lat + dLat * t;
                if (!m_landSea.isLand((float)lon, (float)lat)) {
                    bestLon = lon; bestLat = lat;
                    if (beached) break;            // first water: refloat
                } else if (!beached) {
                    break;                          // last water: stop at the coast
                }
            }

            // ── A LEG THAT GOES NOWHERE IS SKIPPED, NOT SAT ON ──
            //
            // The router's legs run cell centre to cell centre, but the hull is
            // somewhere ARBITRARY inside its own cell -- and the chord from
            // where it actually floats to the next cell's water pixel is the
            // one segment on the whole route that nothing proved. A hull tucked
            // behind a spit inside its own cell is blocked on leg one and, with
            // the rest of a perfectly good route sitting behind it, went nowhere
            // for ever.
            //
            // So a leg that yields no progress is discarded and the next one
            // tried, in the same turn. The route is a chain of touching water
            // cells, so the leg after the blocked one usually clears the
            // obstacle the hull is tucked behind. Bounded, because the point is
            // to step over a local obstruction and not to let a fleet walk its
            // whole route in straight lines across a continent -- and it cannot
            // cheat in any case: every position is still clamped to water.
            const double moved = seaDistanceDeg(ship.lon, ship.lat, bestLon, bestLat);
            if (moved < legDist * 1e-3 && mo.route.size() > 1 && skips < MAX_SKIPS) {
                skips++;
                mo.route.erase(mo.route.begin());
                continue;                           // same budget, next waypoint
            }

            ship.lon = wrapLon(bestLon); ship.lat = bestLat;
            if (take >= legDist - 1e-9) mo.route.erase(mo.route.begin());
            budget = 0.0;                           // partial legs end the turn
            break;
        }

        {
            const double mLon = ship.lon - startLon, mLat = ship.lat - startLat;
            got = std::sqrt(mLon * mLon + mLat * mLat);
            // ONLY JOURNEYS COUNT. A hull holding station off a hostile shore
            // is re-ordered to close the last fraction of a degree every turn
            // and clamped by the beach every turn -- correct behaviour that the
            // first version of this counted as a stall, which is why it read
            // 93% while invasions were landing. 1 degree is about 23 raster
            // pixels: past station-keeping, short of a crossing.
            if (want > 1.0) {
                m_navMoves++;
                if (got < want * 0.05) {
                    m_navBlocked++;
                    if (m_landSea.isLand((float)mo.destLon, (float)mo.destLat))
                        m_navDestOnLand++;
                    else if (!navReachable(startLon, startLat, mo.destLon, mo.destLat))
                        m_navDestOtherSea++;
                }
            }
        }

        // ARRIVED, or STUCK. The order is kept for next turn only while it is
        // still making progress -- a voyage that covered nothing this turn is
        // one the resolver cannot execute, and holding it would leave the hull
        // grinding at the same coast for the rest of the game while the AI,
        // which skips ships that already have an order, never reconsiders it.
        const bool arrived = mo.route.empty();
        const bool stuck   = (got < 1e-6);
        // Loaded boats that never arrive are the "parked out of landing
        // range" of the amphibious eval line: a stuck order is erased here,
        // the reflex re-issues it next turn, and it sticks again. Counted so
        // the pathing failure is visible next to the landing rate.
        if (ship.crew > 0) { if (stuck) m_navBoatMovesStuck++; else if (arrived) m_navBoatMovesArrived++; }
        // OD_BOAT_TRACE: anatomy of a stuck loaded boat -- the first leg's
        // length in cells, the route size, and whether the first step from
        // the hull is a land pixel (the two-cell hop never fired, journal 37i).
        if (ship.crew > 0 && stuck && std::getenv("OD_BOAT_TRACE") && !mo.route.empty()) {
            const double lLon = mo.route.front().first - ship.lon, lLat = mo.route.front().second - ship.lat;
            const double legDeg = std::sqrt(lLon * lLon + lLat * lLat);
            const double cellDeg = 360.0 * (double)m_nav.cell / std::max(1.0, (double)m_landSea.getWidth());
            const double t1 = std::min(1.0, 0.02 / std::max(1e-9, legDeg));
            const bool firstStepLand = m_landSea.isLand((float)(ship.lon + lLon * t1), (float)(ship.lat + lLat * t1));
            // Which leg sticks: the router's own first step along it (a
            // 32nd of the turn's take, the same stride the coast-stop uses),
            // and whether the hull is within one cell of a grid waypoint --
            // i.e. whether this is still an entry leg or a leg between cells.
            const double stride = std::min(1.0, (std::min(shipMaxRangeDeg(ship), legDeg) / std::max(1e-9, legDeg)) / 32.0);
            const bool routerStepLand = m_landSea.isLand((float)(ship.lon + lLon * stride), (float)(ship.lat + lLat * stride));
            printf("[BOAT] t%d cid=%d ship=%d STUCK leg=%.2fdeg (%.1f cells) route=%zu firstStepLand=%d routerStepLand=%d hullOnLand=%d planned=%d\n",
                   m_turnNumber, countryId, mo.shipIndex, legDeg, legDeg / std::max(1e-9, cellDeg), mo.route.size(),
                   firstStepLand ? 1 : 0, routerStepLand ? 1 : 0,
                   m_landSea.isLand((float)ship.lon, (float)ship.lat) ? 1 : 0, mo.planned ? 1 : 0);
        }
        if (ship.crew > 0 && std::getenv("OD_BOAT_TRACE"))
            printf("[BOAT] t%d cid=%d ship=%d ROUTER got=%.3f route=%zu arrived=%d stuck=%d at (%.2f,%.2f) dest (%.2f,%.2f)\n",
                   m_turnNumber, countryId, mo.shipIndex, got, mo.route.size(), arrived ? 1 : 0, stuck ? 1 : 0,
                   ship.lon, ship.lat, mo.destLon, mo.destLat);
        if (arrived || stuck) m_pendingShipMoveOrders.erase(m_pendingShipMoveOrders.begin() + i);
        else ++i;
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
        // YOU MAY ONLY FIRE ON SOMEBODY YOU ARE AT WAR WITH, AND ONLY IN RANGE.
        //
        // Neither was checked here. The overlay refused to aim at a neutral and
        // drew a range circle, and that was the whole of the rule -- so it
        // bound the player's mouse and nothing else. The AI could engage any
        // hull on the map, and a modified client could too, because the host
        // ingest (Game_Multiplayer) validates only that you OWN the firing
        // ship before pushing the order straight into this queue. Distance was
        // a damage falloff with a 0.1 floor, so a shot from the far side of the
        // world still landed for a tenth.
        if (!atWarCids(countryId, tgt.countryId)) {
            m_pendingShipEngageOrders.erase(m_pendingShipEngageOrders.begin() + i);
            continue;
        }
        {
            const double ddx = src.lon - tgt.lon, ddy = src.lat - tgt.lat;
            if (std::sqrt(ddx * ddx + ddy * ddy) > shipMaxRangeDeg(src)) {
                m_pendingShipEngageOrders.erase(m_pendingShipEngageOrders.begin() + i);
                continue;
            }
        }

        // navyDefPct was in the same state armyDefPct was: defined on the nodes,
        // summed by getTotalEffect, and read by nobody.
        float atkMod = 1.0f + getTotalEffect("navyAtkPct", countryId) / 100.0f;
        float defMod = 1.0f + getTotalEffect("navyDefPct", tgt.countryId) / 100.0f;
        float dx = (float)(src.lon - tgt.lon), dy = (float)(src.lat - tgt.lat);
        float dist = sqrtf(dx*dx + dy*dy);
        // Closer = more damage: 1.0 at point blank, 0.1 at 15+ units
        float distFactor = std::max(0.1f, 1.0f - dist / 15.0f);

        // Carrier does most damage, boat least
        // EVERY HULL TYPE THE MAPS ACTUALLY CONTAIN, and a fallback.
        //
        // This chain had no branch for battleship (nor, until they were
        // retired, cruisers) and no else, so those hulls dealt ZERO damage --
        // they could be shot at and could never shoot back. That was a third of
        // every ship in the game, and none of it buildable: battleships exist
        // only in map files, so a sunk one is gone for good. Exactly the same
        // omission as the draw chain that left them with no sprite.
        //
        // The else is the real fix. A type nobody thought of must still fight.
        float baseDmg = 0;
        if (src.type == "battleship") baseDmg = 40;      // heaviest guns afloat
        else if (src.type == "carrier") baseDmg = 35;
        else if (src.type == "destroyer") baseDmg = 25;
        else if (src.type == "frigate") baseDmg = 20;
        else if (src.type == "boat") baseDmg = 5;
        else baseDmg = 15;                               // a mod's own hull still fights

        int damage = (int)(baseDmg * atkMod * distFactor / std::max(0.1f, defMod));
        if (damage < 1) damage = 1;
        tgt.health -= damage;
        m_navEngagements++;

        printf("[NAVY] Ship %d (%s) dealt %d dmg to ship %d (%s): health %d->%d\n",
               eo.shipIndex, src.type.c_str(), damage, eo.targetIndex, tgt.type.c_str(),
               tgt.health + damage, tgt.health);

        if (tgt.health <= 0) {
            m_navSinkings++;
            // BEFORE the owner is cleared and the crew forgotten: the men
            // aboard are the whole point of sinking a transport, and the AI is
            // scored on them. See AISystem::noteShipSunk.
            if (tgt.crew > 0) { m_navTransportsSunk++; m_navCrewDrowned += tgt.crew; }
            if (m_ai) m_ai->noteShipSunk(countryId, tgt.countryId, tgt.crew);
            printf("[NAVY] Ship %d (%s) SUNK ship %d (%s)%s!\n",
                   eo.shipIndex, src.type.c_str(), eo.targetIndex, tgt.type.c_str(),
                   tgt.crew > 0 ? " -- loaded, crew lost" : "");
            tgt.countryId = UNC_CID;
            tgt.crew = 0;
        }
        m_pendingShipEngageOrders.erase(m_pendingShipEngageOrders.begin() + i);
    }
}

// === cleanupSunkShips ===
void Game::cleanupSunkShips() {
    // Remove all ships with countryId == UNC_CID (sunk/scrapped) and shift pending order indices
    for (int i = (int)m_ships.size() - 1; i >= 0; i--) {
        if (m_ships[i].countryId == UNC_CID) {
            if (std::getenv("OD_BOAT_TRACE"))
                printf("[BOAT] t%d SUNK-ERASE ship=%d crew=%d (ships %zu -> %zu)\n",
                       m_turnNumber, i, m_ships[i].crew, m_ships.size(), m_ships.size() - 1);
            m_ships.erase(m_ships.begin() + i);
            forgetShipOrders(i);
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

        // A STATE WITH NO TERRITORY IS NOT FIGHTING ANYBODY.
        //
        // Rebels get their relation rows deleted outright below. Map countries
        // deliberately keep theirs -- an amphibious landing can revive them and
        // the UI still has to name them -- and the consequence nobody handled
        // is that their WARS stood forever. Conquer a neighbour and you were at
        // war with the corpse for the rest of the game.
        //
        // It is not only cosmetic. AISystem::refreshStats builds m_warWith
        // straight from these rows, and cidForIso still resolves a conquered
        // country, so every AI that had ever finished anyone off counted a
        // permanent war: atWar true forever, and with it warInWindow, which is
        // what the idleness charge tests. That charge could never fire for a
        // conqueror -- which is most of them by the late game.
        //
        // The rows survive; only the treaties are struck out. A revived country
        // comes back to a clean slate, which is the right answer anyway: taking
        // the last province is the end of that war, and whoever lands troops to
        // bring it back can declare a new one.
        {
            auto rowIt = m_relations.find(c.isoA3);
            if (rowIt != m_relations.end())
                for (auto& [otherIso, r] : rowIt->second)
                    r = CountryRelation{};
            for (auto& [otherIso, rels] : m_relations) {
                auto colIt = rels.find(c.isoA3);
                if (colIt != rels.end()) colIt->second = CountryRelation{};
            }

            // ── AND ITS CLAIMS DIE WITH IT ──
            //
            // Struck out for exactly the reason the treaties above are: a
            // state with no territory is not pressing anybody for land, and a
            // revived country comes back to a clean slate.
            //
            // This was done for REBELS and not for map countries, so conquering
            // a neighbour left its claims on the board for the rest of the
            // game. Reported by a player who destroyed Mexico and still had a
            // dozen Mexican claims listed against their provinces.
            //
            // IT IS NOT COSMETIC. getProvinceRebellionChance adds claimUnrest
            // for every foreign claim on a province -- 2 points, or 6 while at
            // war with the claimant -- and cidForIso still resolves a
            // conquered country, so every province a dead state had ever
            // claimed carried permanent unrest from a corpse. The player could
            // see the claims and had no way at all to answer them: the one
            // country that could drop them no longer existed.
            m_claims.erase(c.isoA3);
            for (auto& [pid, isos] : m_claimsByProvince)
                isos.erase(std::remove(isos.begin(), isos.end(), c.isoA3), isos.end());
            // Statements a dead country made, or that were made to it, can no
            // longer be caught out by anybody's conduct. SpokenClaim names
            // three parties, so all three are checked.
            m_openClaims.erase(
                std::remove_if(m_openClaims.begin(), m_openClaims.end(),
                    [&](const SpokenClaim& sc) {
                        return sc.speakerIso == c.isoA3 || sc.hearerIso == c.isoA3 ||
                               sc.aboutIso == c.isoA3;
                    }), m_openClaims.end());
            // ...and nothing still queued in their name.
            m_pendingDiplomaticActions.erase(
                std::remove_if(m_pendingDiplomaticActions.begin(),
                               m_pendingDiplomaticActions.end(),
                               [&](const PendingDiplomaticAction& da) {
                                   return da.sourceIso == c.isoA3 ||
                                          da.targetIso == c.isoA3;
                               }),
                m_pendingDiplomaticActions.end());
        }

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
//
// Only wars that START here carry that obligation. A war a map ships already
// set in relations.json is the situation its author chose, not a declaration
// this code gets to react to -- and it cannot tell attacker from defender in
// one anyway, so honouring guarantees over it produces nonsense like France
// and Poland opening the war as co-aggressors on the same turn. A map is free
// to set a guarantor sitting out a war it guaranteed; the rule governs what
// happens from there.
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
    // Apply penalty to attacker's minorities that have kin in defender.
    //
    // ONCE PER GROUP, not once per province holding it. This loop walks every
    // province the attacker owns, and it used to subtract the full 30 inside
    // the inner loop -- so declaring one war cost a group -30 for each province
    // it lived in. A minority spread over fifty provinces took -1500 from a
    // single declaration, which no policy could ever work off. The penalty is
    // "your own people object to this war", and that is one objection.
    std::unordered_set<std::string> penalised;
    for (auto& [pid, pv] : m_provinces.getAllProvinces()) {
        if (pv.countryId != attackerCid) continue;
        auto mit = m_provinceMinorities.find(pid);
        if (mit == m_provinceMinorities.end()) continue;
        for (auto& mg : mit->second) {
            if (penalised.count(mg.name)) continue;
            auto dpIt = defenderMinPop.find(mg.name);
            if (dpIt != defenderMinPop.end() && dpIt->second >= 500000) {
                penalised.insert(mg.name);
                addMinorityDrift(attackerCid, mg.name, -30.0f);
            }
        }
    }
}

bool Game::hasRelation(const std::string& isoA, const std::string& isoB,
                       bool CountryRelation::*flag) const {
    if (isoA.empty() || isoB.empty() || isoA == isoB) return false;
    auto row = m_relations.find(isoA);
    if (row != m_relations.end()) {
        auto cell = row->second.find(isoB);
        if (cell != row->second.end() && cell->second.*flag) return true;
    }
    row = m_relations.find(isoB);
    if (row != m_relations.end()) {
        auto cell = row->second.find(isoA);
        if (cell != row->second.end() && cell->second.*flag) return true;
    }
    return false;
}

void Game::declareWar(const std::string& attackerIso, const std::string& defenderIso,
                      bool chainGuarantees, int statedGoal) {
    if (attackerIso.empty() || defenderIso.empty() || attackerIso == defenderIso) return;
    CountryRelation& fwd = m_relations[attackerIso][defenderIso];
    if (fwd.war) return; // already at war — nothing to do, no double penalties
    // ── A PACT BROKEN BY A DECLARATION IS A BETRAYAL, AND IT COSTS ──
    // Before this the flag was simply cleared. See CRED_HIT_PACT.
    if (fwd.nonAggression) loseCredibility(attackerIso, defenderIso, CRED_HIT_PACT);
    fwd.war = true;
    fwd.alliance = false;
    fwd.nonAggression = false;
    // ...AND THE GUARANTEE, which was the one treaty left standing.
    //
    // A guarantee is a promise to join their wars; holding one against a
    // country you are now fighting is not a position, it is a contradiction,
    // and it happens without anybody choosing it -- a guarantee chain drags a
    // guarantor into a war against a country it also guarantees. What stood
    // afterwards was a pair the game described two ways at once: the AI's
    // target search skips a guaranteed neighbour as friendly, while the
    // diplomacy panel offered "Break Guarantee" where "Request Ceasefire"
    // belongs, because it tests the guarantee before it tests the war.
    fwd.guarantee = false;
    // On the attacker->defender direction only: this is what THEY said, and the
    // defender's own row is what the defender would say about the same war.
    fwd.warGoalStated = statedGoal;
    // Anyone recently told this country was too spent to fight has just watched
    // it declare a war. Before the statement below, so a country cannot escape
    // its own earlier claim by making a new one in the same breath.
    claimsBrokenByDeclaration(attackerIso);
    noteWarGoalStatement(attackerIso, defenderIso, statedGoal);
    CountryRelation& rev = m_relations[defenderIso][attackerIso];
    rev.war = true;
    rev.alliance = false;
    rev.nonAggression = false;
    rev.guarantee = false;

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
        // ...and what they said it was for, if anything. A CLAIM, not a fact:
        // see WarGoal. A declaration with no stated goal says something too.
        std::string decl = otherName + " has declared war on you!";
        if (const char* why = warGoalText(statedGoal))
            decl += std::string("\n\nThey declare it ") + why + ".";
        else
            decl += "\n\nThey give no reason.";
        pushPopup(PopupType::WAR_DECLARED, "War Declared!", decl, otherCid);
    }

    // Only wars the player is in. On a 185-country map the AI declares a dozen
    // a turn, and an alarm for each would mean nothing.
    if (!playerIso.empty() && (attackerIso == playerIso || defenderIso == playerIso))
        Audio::get().playSfx("war_alarm");

    printf("[WAR] %s declares war on %s\n", attackerIso.c_str(), defenderIso.c_str());

    if (!chainGuarantees) return;
    // Every guarantor of the defender joins against the attacker. Collect
    // first, declare after — declaring mutates m_relations while iterating.
    std::vector<std::string> guarantors;
    for (auto& [isoA, targets] : m_relations) {
        if (isoA == attackerIso || isoA == defenderIso) continue;
        if (hasRelation(isoA, defenderIso, &CountryRelation::guarantee) &&
            cidForIso(isoA) >= 0)
            guarantors.push_back(isoA);
    }
    for (auto& g : guarantors) {
        printf("[WAR] %s honours its guarantee of %s and joins against %s\n",
               g.c_str(), defenderIso.c_str(), attackerIso.c_str());
        declareWar(g, attackerIso, false); // one level only
        if (!playerIso.empty() && g == playerIso)
            addNotification(TextFormat(T("You honour your guarantee of %s and are now at war with %s"),
                                       defenderIso.c_str(), attackerIso.c_str()), RED, 8.0f);
    }

    // A guarantee compels; an alliance asks. Allies get a call to arms they can
    // turn down -- which is what makes signing one a decision rather than a
    // formality, and what gives the AI something to actually learn about it.
    issueCallsToArms(attackerIso, defenderIso);
}

// ─── War weariness / calls to arms ───────────────────────────────────────

void Game::addWarWeariness(int cid, float amount) {
    if (cid <= 0 || cid >= SPC_CID) return;
    float& w = m_countryWarWeariness[cid];
    // Capped: weariness should make holding a country together hard, not make
    // total collapse arithmetically certain the moment two allies call.
    w = std::min(WAR_WEARINESS_MAX, w + amount);
}

void Game::decayRebellionCooldowns() {
    for (auto it = m_provinceRebellionCooldown.begin();
         it != m_provinceRebellionCooldown.end(); ) {
        if (--it->second <= 0) it = m_provinceRebellionCooldown.erase(it);
        else ++it;
    }
}

void Game::decayWarWeariness() {
    for (auto it = m_countryWarWeariness.begin(); it != m_countryWarWeariness.end(); ) {
        it->second -= WAR_WEARINESS_DECAY;
        if (it->second <= 0.01f) it = m_countryWarWeariness.erase(it);
        else ++it;
    }
}

// What a diplomatic request was ASKING FOR, in words a player recognises.
// Returns nullptr for anything that is not one of the three proposals, which
// is what keeps break_* and the ceasefire flow out of these notifications.
static const char* diploRequestPhrase(const std::string& action) {
    // TRANSLATED HERE, because these leave as ARGUMENTS. The sentence around
    // them was translated and they were not, so a Ukrainian player was told
    // "Verrick прийняли вашу пропозицію: a non-aggression pact".
    if (action == "request_alliance")  return T("an alliance");
    if (action == "request_guarantee") return T("a mutual guarantee");
    if (action == "request_nap")       return T("a non-aggression pact");
    return nullptr;
}

// A country's name if we have one, its ISO code otherwise. "SWE accepted" is a
// worse sentence than "Sweden accepted", and the ISO is what was being shown.
std::string Game::diploDisplayName(const std::string& iso) const {
    const int cid = cidForIso(iso);
    if (cid >= 0) {
        if (const Country* c = m_countries.getCountry(cid))
            if (!c->name.empty()) return od::i18n::properName(c->name);
    }
    return iso;
}

std::vector<std::string> Game::callableFriends(int countryId) const {
    std::vector<std::string> out;
    const Country* me = m_countries.getCountry(countryId);
    if (!me) return out;
    const std::string myIso = me->isoA3;

    // At war with anybody? There is nothing to call anyone INTO otherwise.
    std::unordered_set<std::string> enemies;
    auto myRels = m_relations.find(myIso);
    if (myRels != m_relations.end())
        for (const auto& [iso, rel] : myRels->second) if (rel.war) enemies.insert(iso);
    for (const auto& [iso, targets] : m_relations) {
        auto it = targets.find(myIso);
        if (it != targets.end() && it->second.war) enemies.insert(iso);
    }
    if (enemies.empty()) return out;

    // Allies and guarantors, from whichever side of the pair recorded the pact.
    std::set<std::string> friends;
    if (myRels != m_relations.end())
        for (const auto& [iso, rel] : myRels->second)
            if (rel.alliance) friends.insert(iso);
    for (const auto& [iso, targets] : m_relations) {
        auto it = targets.find(myIso);
        if (it == targets.end()) continue;
        if (it->second.alliance || it->second.guarantee) friends.insert(iso);
    }

    for (const std::string& iso : friends) {
        if (iso == myIso || enemies.count(iso)) continue;
        const int cid = cidForIso(iso);
        if (cid < 0 || cid >= SPC_CID) continue;
        // Already in every war of ours? Then there is nothing to ask for.
        bool anyToJoin = false;
        for (const std::string& e : enemies)
            if (e != iso && !hasRelation(iso, e, &CountryRelation::war)) { anyToJoin = true; break; }
        if (!anyToJoin) continue;
        const long long key = ((long long)countryId << 24) | (long long)cid;
        auto cd = m_callToArmsCooldown.find(key);
        if (cd != m_callToArmsCooldown.end() && m_turnNumber < cd->second) continue;
        out.push_back(iso);
    }
    // Sorted: a list an AI picks from has to replay the same way.
    std::sort(out.begin(), out.end());
    return out;
}

bool Game::requestAllyJoinWar(const std::string& allyIso, std::string& outWhy) {
    return requestAllyJoinWar(m_playerCountryId, allyIso, outWhy);
}

bool Game::requestAllyJoinWar(int callerCid, const std::string& allyIso, std::string& outWhy) {
    const Country* me = m_countries.getCountry(callerCid);
    if (!me || allyIso.empty()) { outWhy = "No country selected."; return false; }
    const std::string myIso = me->isoA3;

    auto myRels = m_relations.find(myIso);
    if (myRels == m_relations.end()) { outWhy = "You have no relations yet."; return false; }
    // Either direction: the panel offers the button on a reverse-only alliance
    // (a scenario writing MCK->JPN and nothing back), and refusing here on the
    // same relation the button was drawn from is just a button that lies.
    // An ally OR a guarantor. A guarantee that was signed after the shooting
    // started never chains -- declareWar is the only place guarantees fire --
    // so without this the pact is worth nothing at all in the war it was signed
    // for, which is the war it was obviously signed for.
    if (!hasRelation(myIso, allyIso, &CountryRelation::alliance) &&
        !hasRelation(allyIso, myIso, &CountryRelation::guarantee)) {
        outWhy = "Only an ally or a guarantor can be called to arms.";
        return false;
    }

    // Which war? The ally is asked about ONE enemy, so it has to be chosen
    // rather than left ambiguous: the strongest enemy they are not already
    // fighting. Strongest because that is the one help is actually worth
    // asking for, and "not already fighting" because asking someone to join a
    // war they are in is nothing.
    const int allyCid = cidForIso(allyIso);
    if (allyCid < 0) { outWhy = "That country no longer exists."; return false; }

    // Our wars, from whichever side recorded them.
    std::unordered_set<std::string> enemies;
    for (auto& [iso, rel] : myRels->second)
        if (rel.war) enemies.insert(iso);
    for (auto& [iso, targets] : m_relations) {
        auto it = targets.find(myIso);
        if (it != targets.end() && it->second.war) enemies.insert(iso);
    }

    // One pass over the armies rather than one per candidate enemy: the map has
    // thousands of provinces and this runs on a button press.
    std::unordered_map<int, long long> armyByCid;
    for (auto& [pid, units] : m_provinceArmies)
        for (auto& u : units) armyByCid[u.countryId] += u.count;

    std::string bestEnemy;
    long long bestArmy = -1;
    for (const std::string& iso : enemies) {
        if (iso == allyIso || iso == myIso) continue;
        if (hasRelation(allyIso, iso, &CountryRelation::war)) continue;   // already in it
        const int ecid = cidForIso(iso);
        if (ecid < 0 || ecid >= SPC_CID) continue;
        auto ait = armyByCid.find(ecid);
        const long long army = (ait == armyByCid.end()) ? 0 : ait->second;
        // ISO tie-break: `enemies` is unordered, and self-play has to replay.
        if (army > bestArmy || (army == bestArmy && iso < bestEnemy))
            { bestArmy = army; bestEnemy = iso; }
    }
    if (bestEnemy.empty()) {
        outWhy = "No war they could join.";
        return false;
    }

    // The same cooldown the automatic calls use, and the same key, so a player
    // ask and a defensive ask cannot both land on one ally in the same breath.
    const long long key = ((long long)callerCid << 24) | (long long)allyCid;
    auto cd = m_callToArmsCooldown.find(key);
    if (cd != m_callToArmsCooldown.end() && m_turnNumber < cd->second) {
        outWhy = "You have already called them recently.";
        return false;
    }
    PendingDiplomaticAction da;
    da.sourceIso = myIso;
    da.targetIso = allyIso;
    da.action = "call_to_arms";
    da.subjectIso = bestEnemy;
    da.turnsRemaining = 1;
    // The cooldown is spent only if the call is actually made -- see
    // queueDiplomaticAction. Charging it first would burn thirty turns of
    // asking on a call the one-channel rule then refused to send.
    if (!queueDiplomaticAction(std::move(da))) {
        outWhy = "You are already in talks with them this turn.";
        return false;
    }
    m_callToArmsCooldown[key] = m_turnNumber + CALL_TO_ARMS_COOLDOWN_TURNS;
    if (m_ai) m_ai->noteCallIssued(callerCid);

    // Only the player is told, and only when it is the player asking: a
    // notification for somebody else's diplomacy is noise on the human's screen.
    if (callerCid == m_playerCountryId)
        addNotification(TextFormat(T("You call %s to arms against %s"),
                                   diploDisplayName(allyIso).c_str(),
                                   diploDisplayName(bestEnemy).c_str()),
                        Color{200, 200, 240, 255}, 7.0f);
    printf("[WAR] %s calls %s to arms against %s (%s)\n",
           myIso.c_str(), allyIso.c_str(), bestEnemy.c_str(),
           callerCid == m_playerCountryId ? "player" : "deliberate");
    outWhy.clear();
    return true;
}

void Game::issueCallsToArms(const std::string& attackerIso, const std::string& defenderIso) {
    if (attackerIso.empty() || defenderIso.empty()) return;

    // Collect first: pushing onto m_pendingDiplomaticActions is safe, but
    // reading m_relations while a later declareWar rewrites it is not.
    // The defender's allies, from whichever side of the pair recorded the
    // alliance — a scenario that wrote only the ally's row used to leave the
    // defender standing alone.
    std::set<std::string> defAllies;
    auto defRels = m_relations.find(defenderIso);
    if (defRels != m_relations.end())
        for (auto& [iso, rel] : defRels->second)
            if (rel.alliance) defAllies.insert(iso);
    for (auto& [iso, targets] : m_relations) {
        auto it = targets.find(defenderIso);
        if (it != targets.end() && it->second.alliance) defAllies.insert(iso);
    }

    std::vector<std::string> allies;
    for (const std::string& iso : defAllies) {
        if (iso == attackerIso || iso == defenderIso) continue; // the alliance the attack just broke
        int cid = cidForIso(iso);
        if (cid < 0 || cid >= SPC_CID) continue;
        // Already fighting them? Nothing to ask for.
        if (hasRelation(iso, attackerIso, &CountryRelation::war)) continue;
        // One ask per pair per cooldown. An ally who already marched, or who
        // already said no, should not be re-asked every time a new front opens.
        const long long key = ((long long)cidForIso(defenderIso) << 24) | (long long)cid;
        auto cd = m_callToArmsCooldown.find(key);
        if (cd != m_callToArmsCooldown.end() && m_turnNumber < cd->second) continue;
        allies.push_back(iso);
    }
    for (const std::string& iso : allies) {
        PendingDiplomaticAction da;
        da.sourceIso = defenderIso;
        da.targetIso = iso;
        da.action = "call_to_arms";
        da.subjectIso = attackerIso;
        da.turnsRemaining = 1;
        // Charged only where the call is actually sent, same as the player's
        // deliberate ask: an ally the one-channel rule turns away has not been
        // asked, so it must not go on cooldown as though it had.
        if (!queueDiplomaticAction(std::move(da))) continue;
        m_callToArmsCooldown[((long long)cidForIso(defenderIso) << 24) |
                             (long long)cidForIso(iso)] =
            m_turnNumber + CALL_TO_ARMS_COOLDOWN_TURNS;
        // Counted against the ALLY BEING ASKED, once per ally, not against the
        // defender doing the asking.
        //
        // It used to be the other way, and the readout was nonsense in two
        // directions at once. One call goes to every ally, so a single
        // "issued" could be followed by five answers -- and the stats are split
        // by cohort, so a model country's call refused by a random ally landed
        // in a different bucket entirely. Between them the eval panel printed
        // "calls 1 answered 0 refused 5 (500% refused)".
        //
        // One row per question asked, in the same bucket as its answer, so
        // answered + refused == issued and the percentage means what it says.
        if (m_ai) m_ai->noteCallIssued(cidForIso(iso));
        printf("[WAR] %s calls its ally %s to arms against %s\n",
               defenderIso.c_str(), iso.c_str(), attackerIso.c_str());
    }
}

// === queueDiplomaticAction ===
// The one door into m_pendingDiplomaticActions. See the note on the
// declarations in Game.h for what was coming through the wall beside it.
bool Game::hasPendingDiplomacy(const std::string& sourceIso,
                               const std::string& targetIso) const {
    for (const auto& da : m_pendingDiplomaticActions)
        if (da.sourceIso == sourceIso && da.targetIso == targetIso) return true;
    return false;
}

bool Game::hasPendingDeclaration(const std::string& sourceIso) const {
    for (const auto& da : m_pendingDiplomaticActions)
        if (da.sourceIso == sourceIso && da.action == "declare_war") return true;
    return false;
}

int Game::countPendingDeclarations(const std::string& sourceIso) const {
    int n = 0;
    for (const auto& da : m_pendingDiplomaticActions)
        if (da.sourceIso == sourceIso && da.action == "declare_war") ++n;
    return n;
}

int Game::warDeclarationLimit(int countryId) const {
    if (countryId <= 0) return WAR_DECLARATIONS_BASE;
    const float extra = getTotalEffect("warDeclarations", countryId);
    const int limit = WAR_DECLARATIONS_BASE + (int)std::lround(extra);
    // Never below one: a doctrine that took the last declaration away would
    // make a country unable to go to war at all, which is not a trade-off, it
    // is a broken state a player could enact by accident.
    return std::max(1, limit);
}

bool Game::queueDiplomaticAction(PendingDiplomaticAction da) {
    if (da.sourceIso.empty() || da.targetIso.empty() ||
        da.sourceIso == da.targetIso) return false;

    // BOOKKEEPING IS NOT AN OVERTURE. apply_ceasefire and cancel are the
    // deferred second halves of a decision already taken and already answered
    // -- the request they came from was erased when it resolved -- so the
    // one-channel rule has nothing to say about them.
    const bool bookkeeping = (da.action == "apply_ceasefire" ||
                              da.action == "apply_trade" || da.action == "cancel");
    if (!bookkeeping) {
        if (hasPendingDiplomacy(da.sourceIso, da.targetIso)) return false;
        if (da.action == "declare_war") {
            // Against the country's OWN limit, which doctrine may raise. See
            // Game::warDeclarationLimit for why this is a count and not the
            // boolean it used to be.
            int srcCid = 0;
            for (const auto& [cid, c] : m_countries.getAll())
                if (c.isoA3 == da.sourceIso) { srcCid = cid; break; }
            if (countPendingDeclarations(da.sourceIso) >= warDeclarationLimit(srcCid))
                return false;
        }
    }

    // The pair is checked in ONE direction only. Both sides offering each other
    // a ceasefire in the same turn is a real position the game already knows
    // how to settle (see the mutual-ceasefire branch in
    // processDiplomaticRequests), and it is not what this rule is for: the
    // confusion comes from one country speaking twice, not from two countries
    // speaking at once.
    m_pendingDiplomaticActions.push_back(std::move(da));
    return true;
}

// === processDiplomaticRequests ===
// === repatriateStrandedArmies ===
//
// See the declaration for why this exists. The shape of the rule:
//
//   STRANDED means: a real country's stack, in a real country's province,
//   with neither a war nor an alliance between them. During a war the stack
//   is an occupation; under an alliance it is a guest; with neither it is a
//   state no order the game accepts can produce, only a treaty landing under
//   an army that had marched somewhere legally.
//
//   HOME is the nearest owned province BY THE PROVINCE GRAPH, not by the map
//   distance -- armies walk, and the graph is the thing they walk on. A BFS
//   over m_provinceNeighbors from the stranded province finds it; a country
//   that no longer owns anything gets its stack disbanded, which is what
//   defeat already means everywhere else.
//
//   REBELS ARE EXEMPT in both directions. Rebel-held land is a war zone
//   whatever the relations table says, so a stack standing on it is not
//   stranded; and a rebel stack inside a country IS the insurrection --
//   sending it home would put down every revolt as a tidiness measure.
void Game::repatriateStrandedArmies() {
    struct Move { int fromPid; int cid; int count; };
    std::vector<Move> moves;

    for (auto& [pid, units] : m_provinceArmies) {
        const Province* p = m_provinces.getProvinceById(pid);
        if (!p) continue;
        const int host = p->countryId;
        if (host <= 0 || host >= REBEL_CID_MIN) continue;   // unowned or rebel-held
        const Country* hc = m_countries.getCountry(host);
        if (!hc) continue;
        for (const auto& u : units) {
            const int guest = u.countryId;
            if (u.count <= 0 || guest == host) continue;
            if (guest <= 0 || guest >= REBEL_CID_MIN) continue;   // rebels stay
            const Country* gc = m_countries.getCountry(guest);
            if (!gc) continue;
            bool standing = false;
            auto gr = m_relations.find(gc->isoA3);
            if (gr != m_relations.end()) {
                auto rr = gr->second.find(hc->isoA3);
                if (rr != gr->second.end())
                    standing = rr->second.war || rr->second.alliance;
            }
            if (!standing) moves.push_back({pid, guest, u.count});
        }
    }
    if (moves.empty()) return;
    // Applied in a sorted order, not in unordered_map order: two runs of the
    // same turn must send the same stacks home in the same sequence.
    std::sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) {
        return a.fromPid != b.fromPid ? a.fromPid < b.fromPid : a.cid < b.cid;
    });

    for (const Move& mv : moves) {
        // Nearest owned province by BFS over the graph the army would walk.
        int home = -1;
        std::unordered_set<int> seen{mv.fromPid};
        std::deque<int> q{mv.fromPid};
        while (!q.empty() && home < 0) {
            const int cur = q.front();
            q.pop_front();
            auto nIt = m_provinceNeighbors.find(cur);
            if (nIt == m_provinceNeighbors.end()) continue;
            for (int n : nIt->second) {
                if (!seen.insert(n).second) continue;
                const Province* np = m_provinces.getProvinceById(n);
                if (np && np->countryId == mv.cid) { home = n; break; }
                q.push_back(n);
            }
        }
        // Take the stack off where it stood, whatever happens to it next.
        auto aIt = m_provinceArmies.find(mv.fromPid);
        if (aIt == m_provinceArmies.end()) continue;
        auto& arr = aIt->second;
        arr.erase(std::remove_if(arr.begin(), arr.end(),
                  [&](const ArmyUnit& u) { return u.countryId == mv.cid; }),
                  arr.end());
        if (arr.empty()) m_provinceArmies.erase(aIt);
        if (home >= 0) {
            addTroopsTo(home, mv.cid, mv.count);
            printf("[REPATRIATE] cid=%d: %d troops walked home %d -> %d\n",
                   mv.cid, mv.count, mv.fromPid, home);
        } else {
            // Landless: nowhere to walk to. Defeat already disbands elsewhere.
            printf("[REPATRIATE] cid=%d: %d troops in prov %d disbanded (no home)\n",
                   mv.cid, mv.count, mv.fromPid);
        }
    }
}

void Game::processDiplomaticRequests() {
    // Get player ISO
    std::string playerIso;
    {
        const Country* pc = m_countries.getCountry(m_playerCountryId);
        if (pc) playerIso = pc->isoA3;
    }

    for (size_t i = 0; i < m_pendingDiplomaticActions.size(); ) {
        // A COPY, BECAUSE PROCESSING AN ACTION CAN QUEUE MORE OF THEM.
        //
        // This was a reference into the very vector the loop is walking, and
        // resolving a declaration grows it: declareWar calls issueCallsToArms,
        // which calls queueDiplomaticAction, which push_backs onto
        // m_pendingDiplomaticActions. A push_back that reallocates leaves the
        // reference dangling, and `da` is read afterwards -- the [DIPLO] trace
        // reads three strings out of it once the action has been applied.
        //
        // Reading freed memory is undefined behaviour, and the shape of it is
        // nasty: it depends on whether the vector happened to have spare
        // capacity, so it fires on some turns and not others with no pattern a
        // player could describe. That is worth stating because the game already
        // has a rule that LOOKS like a workaround for it -- one war declaration
        // per country per turn -- and the rule does not actually prevent it: a
        // single declaration with two allies to call queues two actions and can
        // reallocate just the same.
        //
        // THERE IS A SECOND INSTANCE of the same fault a few lines down: the
        // request-to-player branch ERASES element i and then reads `da` on the
        // next line to print the trace. Erase destroys or moves that element,
        // so the reference was dangling there too, on a path that runs whenever
        // an AI asks the player for anything.
        //
        // `da` is read 146 times in this loop and written once -- the
        // turnsRemaining countdown, which is written back to the queue entry
        // explicitly below, because the copy is ours and the entry is what
        // survives to next turn.
        PendingDiplomaticAction da = m_pendingDiplomaticActions[i];
        // Skip if this action involves the player and is a request — handled via popup instead
        bool isRequestToPlayer = !playerIso.empty() && da.targetIso == playerIso
            && da.sourceIso != playerIso
            && (da.action == "request_alliance" || da.action == "request_guarantee"
             || da.action == "request_nap" || da.action == "declare_war"
             || da.action == "request_ceasefire" || da.action == "propose_trade"
             || da.action == "call_to_arms");

        // NOT DURING THE TUTORIAL -- but only the ones aimed AT the player.
        //
        // Each of those is a modal popup landing on somebody a script has
        // just told to do something else: a window over the thing Mia is
        // pointing at, with buttons the gate will not let them press.
        // Ceasefires and trade are taught here, later and on purpose.
        //
        // THE PLAYER'S OWN ORDERS STILL RESOLVE. This check used to sit at
        // the top of the loop and skipped every pending action there was, so
        // the tutorial told the player to declare war on Verrick, they did,
        // and the declaration sat in the queue for ever -- the one page that
        // waits for that war could never pass.
        if (m_tutorialMode && isRequestToPlayer) { ++i; continue; }
        if (isRequestToPlayer) {
            // Find requesting country ID
            int reqCid = -1;
            for (auto& [cid, c] : m_countries.getAll()) {
                if (c.isoA3 == da.sourceIso) { reqCid = cid; break; }
            }
            std::string title, msg;
            const Country* srcC = m_countries.getCountry(reqCid);
            std::string srcName = srcC ? srcC->name : da.sourceIso;

            if (da.action == "request_ceasefire" || da.action == "propose_trade") {
                const bool isTrade = (da.action == "propose_trade");
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
                summary += srcName + (isTrade ? " proposes a trade.\n"
                                                : " proposes a ceasefire.\n");
                if (terms.ourMoney > 0)  summary += TextFormat(T("  Offers %d gold\n"), terms.ourMoney);
                if (terms.theirMoney > 0) summary += TextFormat(T("  Demands %d gold\n"), terms.theirMoney);
                if (!terms.ourProvs.empty()) summary += TextFormat(T("  Cedes %zu province(s)\n"), terms.ourProvs.size());
                if (!terms.theirProvs.empty()) summary += TextFormat(T("  Demands %zu province(s)\n"), terms.theirProvs.size());
                if (!terms.ourDropClaims.empty()) summary += TextFormat(T("  Drops %zu own claim(s)\n"), terms.ourDropClaims.size());
                if (!terms.theirDropClaims.empty()) summary += TextFormat(T("  Demands you drop %zu claim(s)\n"), terms.theirDropClaims.size());
                // Named in the summary line too, not only in the detail panel:
                // this is the text a player skims before deciding to open it.
                if (!terms.ourReleaseTag.empty())
                    summary += TextFormat(T("  Frees %s on %zu province(s)\n"),
                                          od::i18n::properName(terms.ourReleaseTag).c_str(),
                                          terms.ourReleaseProvs.size());
                if (!terms.theirReleaseTag.empty())
                    summary += TextFormat(T("  Demands you free %s on %zu province(s)\n"),
                                          od::i18n::properName(terms.theirReleaseTag).c_str(),
                                          terms.theirReleaseProvs.size());
                if (summary.back() == '\n') summary.pop_back();
                pushPopup(PopupType::CEASEFIRE_REQUEST,
                          isTrade ? "Trade Offer" : "Ceasefire Offer", summary,
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
                title = T("War Declared!");
                msg2 = srcName + " has declared war on " + (playerIso.empty() ? da.targetIso : "you") + "!";
                if (const char* why = warGoalText(da.statedGoal))
                    msg2 += std::string("\n\nThey declare it ") + why + ".";
                else
                    msg2 += "\n\nThey give no reason.";
            } else if (da.action == "request_alliance") {
                title = T("Alliance Request");
                msg2 = srcName + " proposes an alliance.";
            } else if (da.action == "request_guarantee") {
                title = T("Guarantee Request");
                msg2 = srcName + " requests a mutual guarantee.";
            } else if (da.action == "request_nap") {
                title = T("Non-Aggression Proposal");
                msg2 = srcName + " proposes a non-aggression pact.";
            } else if (da.action == "call_to_arms") {
                int aggCid = cidForIso(da.subjectIso);
                const Country* aggC = m_countries.getCountry(aggCid);
                std::string aggName = aggC ? aggC->name : da.subjectIso;
                title = "Call to Arms";
                msg2 = srcName + " invokes your alliance against " + aggName + ".\n"
                       "Join the war, or refuse and the alliance ends.\n"
                       "Joining will stir unrest at home.";
            }
            pushPopup(pt, title, msg2, reqCid, da.action, da.sourceIso, da.targetIso);
            if (da.action == "call_to_arms" && !m_popupQueue.empty())
                m_popupQueue.back().subjectIso = da.subjectIso;
            // War is not a request — it happens whether or not the player has
            // dismissed the popup yet. This used to be dropped entirely here.
            if (da.action == "declare_war")
                declareWar(da.sourceIso, da.targetIso, true, da.statedGoal);
            m_pendingDiplomaticActions.erase(m_pendingDiplomaticActions.begin() + i);
            printf("[DIPLO] Incoming request from %s → player: %s (pushed to popup queue)\n",
                   da.sourceIso.c_str(), da.action.c_str());
            continue;
        }

        // The countdown lives on the QUEUE ENTRY, not on our copy; the copy is
        // refreshed from it so the rest of the body reads the new value.
        --m_pendingDiplomaticActions[i].turnsRemaining;
        da.turnsRemaining = m_pendingDiplomaticActions[i].turnsRemaining;
        if (da.turnsRemaining <= 0) {
            // Requests aimed at an AI country go through its diplomacy net
            // instead of being auto-accepted.
            if (da.action == "call_to_arms") {
                // Allies choose. Accept = join the war and pay for it at home;
                // refuse = keep the peace and lose the alliance.
                int allyCid = cidForIso(da.targetIso);
                bool accept = false;
                int callStated = REFUSE_NONE;
                if (allyCid >= 0 && allyCid != m_playerCountryId && m_ai)
                    accept = m_ai->decideDiplomacy(allyCid, da.action, da.sourceIso,
                                                   da.subjectIso, &callStated);
                if (accept) {
                    // No further chaining: the ally's own guarantors and allies
                    // are not dragged in as well, or one border incident
                    // recursively enlists the entire map.
                    declareWar(da.targetIso, da.subjectIso, false);
                    addWarWeariness(allyCid, CALL_TO_ARMS_UNREST);
                    if (m_ai) m_ai->noteCallAnswered(allyCid);
                    printf("[WAR] %s answers %s's call and joins against %s\n",
                           da.targetIso.c_str(), da.sourceIso.c_str(), da.subjectIso.c_str());
                    // The counterpart of the refusal message below. An ally
                    // marching because you asked is the single most useful
                    // thing diplomacy does, and it was reported only to stdout.
                    if (!playerIso.empty() && da.sourceIso == playerIso) {
                        addNotification(TextFormat(T("%s answers your call and joins the war against %s"),
                                                   diploDisplayName(da.targetIso).c_str(),
                                                   diploDisplayName(da.subjectIso).c_str()),
                                        Color{140, 220, 150, 255}, 8.0f);
                        Audio::get().playSfx("deal_accepted");
                    }
                } else {
                    m_relations[da.sourceIso][da.targetIso].alliance = false;
                    m_relations[da.targetIso][da.sourceIso].alliance = false;
                    noteRefusalStatement(da.targetIso, da.sourceIso, callStated);
                    if (m_ai) { m_ai->noteCallRefused(allyCid);
                                m_ai->noteDiploRejected(cidForIso(da.sourceIso), allyCid); }
                    printf("[WAR] %s refuses %s's call to arms; the alliance is over\n",
                           da.targetIso.c_str(), da.sourceIso.c_str());
                    if (!playerIso.empty() && da.sourceIso == playerIso) {
                        // The refusal that costs the most, so the one most
                        // worth explaining -- truthfully or otherwise.
                        std::string msg = diploDisplayName(da.targetIso) +
                                          " refused your call to arms";
                        if (const char* why = refusalText(callStated))
                            msg += std::string(" — ") + why;
                        msg += " — the alliance is over";
                        addNotification(msg, Color{235, 130, 90, 255}, 8.0f);
                        Audio::get().playSfx("deal_rejected");
                    }
                }
                m_pendingDiplomaticActions.erase(m_pendingDiplomaticActions.begin() + i);
                continue;
            }
            if (m_ai && (da.action == "request_alliance" || da.action == "request_guarantee" ||
                         da.action == "request_nap")) {
                int aiTgtCid = cidForIso(da.targetIso);
                int stated = REFUSE_NONE;
                if (aiTgtCid >= 0 && aiTgtCid != m_playerCountryId &&
                    !m_ai->decideDiplomacy(aiTgtCid, da.action, da.sourceIso,
                                           std::string(), &stated)) {
                    // The AI's word is judged by the rule the player's is.
                    noteRefusalStatement(da.targetIso, da.sourceIso, stated);
                    // Remember the refusal so the proposer backs off properly.
                    m_ai->noteDiploRejected(cidForIso(da.sourceIso), aiTgtCid);
                    if (m_config.aiDebug)
                        printf("[DIPLO] %s rejected %s from %s\n", da.targetIso.c_str(),
                               da.action.c_str(), da.sourceIso.c_str());
                    if (!playerIso.empty() && da.sourceIso == playerIso) {
                        // Naming the request matters: a player who asked three
                        // countries for three different things in one turn was
                        // told only that somebody "rejected your request".
                        const char* what = diploRequestPhrase(da.action);
                        std::string msg =
                            TextFormat(T("%s declined your offer of %s"),
                                       diploDisplayName(da.targetIso).c_str(),
                                       what ? what : T("an agreement"));
                        // ...AND WHAT THEY SAID ABOUT IT, when they said
                        // anything. Not necessarily the truth: see
                        // AISystem::chooseStatedRefusal.
                        if (const char* why = refusalText(stated))
                            msg += std::string(" — ") + why;
                        addNotification(msg, Color{235, 130, 90, 255}, 7.0f);
                        Audio::get().playSfx("deal_rejected");
                    }
                    m_pendingDiplomaticActions.erase(m_pendingDiplomaticActions.begin() + i);
                    continue;
                }
            }
            // NO TREATY ACROSS A WAR, whoever agreed to it.
            //
            // The counterpart of the same check on the player's popup. A pact
            // and a war can only reach this point together if the war arrived
            // while the offer was in flight -- a guarantee chain dragging the
            // pair in, an ally's call answered -- and applying the pact anyway
            // leaves a pair recorded as both allied and at war, which every
            // reader of m_relations then answers differently. The war stands;
            // the offer is void, and ending it is what request_ceasefire is
            // for.
            if ((da.action == "request_alliance" || da.action == "request_guarantee" ||
                 da.action == "request_nap") &&
                hasRelation(da.sourceIso, da.targetIso, &CountryRelation::war)) {
                if (!playerIso.empty() && da.sourceIso == playerIso)
                    addNotification(TextFormat(T("Your offer to %s is void — you are at war"),
                                               diploDisplayName(da.targetIso).c_str()),
                                    Color{235, 130, 90, 255}, 7.0f);
                printf("[DIPLO] %s → %s: %s dropped, the pair is at war\n",
                       da.sourceIso.c_str(), da.targetIso.c_str(), da.action.c_str());
                m_pendingDiplomaticActions.erase(m_pendingDiplomaticActions.begin() + i);
                continue;
            }
            // Reaching here means the request was NOT refused, so it is about
            // to be applied. Acceptance was entirely silent before this: the
            // relation appeared on the diplomacy screen and nothing ever said
            // it had happened, so the only way to learn an alliance had been
            // agreed was to go looking for it. Said once, here, rather than in
            // each of the three branches below.
            if (!playerIso.empty() && da.sourceIso == playerIso) {
                if (const char* what = diploRequestPhrase(da.action)) {
                    // One sentence with two holes in it, not three pieces
                    // glued together: a translator has to be able to move the
                    // name and the thing agreed past each other, and gluing
                    // fixes the English order into every language.
                    addNotification(TextFormat(T("%s accepted your offer of %s"),
                                               diploDisplayName(da.targetIso).c_str(), what),
                                    Color{140, 220, 150, 255}, 7.0f);
                    Audio::get().playSfx("deal_accepted");
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
                    if (foreignCid == m_playerCountryId) return;
                    for (auto& [pid, units] : m_provinceArmies) {
                        Province* pp = m_provinces.getProvinceById(pid);
                        if (!pp || pp->countryId != localCid) continue;
                        // MERGED, not relabelled. Retagging the stack left the
                        // host with two stacks of its own in one province, and
                        // a move order reads the FIRST one and moves a share of
                        // that -- so half the garrison was invisible to every
                        // order given afterwards.
                        long long absorbed = 0;
                        for (auto it = units.begin(); it != units.end(); ) {
                            if (it->countryId == foreignCid) {
                                absorbed += it->count;
                                it = units.erase(it);
                            } else ++it;
                        }
                        if (absorbed <= 0) continue;
                        bool merged = false;
                        for (auto& u : units)
                            if (u.countryId == localCid) { u.count += (int)absorbed; merged = true; break; }
                        if (!merged) units.push_back({localCid, (int)absorbed});
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
                // Breaking it openly costs the same as breaking it by attack:
                // the promise is the thing being broken either way.
                if (rt.nonAggression) loseCredibility(da.sourceIso, da.targetIso, CRED_HIT_PACT);
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
                int cfStated = REFUSE_NONE;
                if (m_ai && cfTgtCid >= 0 && cfTgtCid != m_playerCountryId)
                    aiAccepts = m_ai->decideDiplomacy(cfTgtCid, "request_ceasefire",
                                                      da.sourceIso, std::string(), &cfStated);

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
                    } else {
                        // WHITE PEACE STILL SENDS THE TROOPS HOME. The
                        // withdrawal used to live only inside
                        // applyCeasefireTerms, which runs only when there are
                        // terms to apply -- so a ceasefire that traded nothing
                        // ended the war and left both armies standing exactly
                        // where they were, in a country they were now at peace
                        // with and which could never make them leave.
                        withdrawArmiesAfterPeace(cidForIso(da.sourceIso),
                                                 cidForIso(da.targetIso));
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
                            otherName + " has rejected your ceasefire offer" +
                            (refusalText(cfStated) ? std::string(" — ") + refusalText(cfStated)
                                                   : std::string()) +
                            ". The war continues.",
                            0, "ceasefire_rejected", da.sourceIso, da.targetIso);
                    }
                }
                printf("[CEASEFIRE] War ended: %s vs %s (aiAccepts=%d mutual=%d)\n",
                       da.sourceIso.c_str(), da.targetIso.c_str(), aiAccepts, mutualCeasefire);
            } else if (da.action == "propose_trade") {
                // A ceasefire without the war. The terms live in the same map
                // under the same key and move through the same executor; the
                // only differences are that no war ends, no armies are sent
                // home, and there is no mutual-offer tiebreak to run -- two
                // countries proposing trades to each other is not a conflict,
                // it is two trades.
                std::string key = da.sourceIso + "|" + da.targetIso;
                auto tit = m_pendingCeasefireTerms.find(key);
                int trTgtCid = cidForIso(da.targetIso);
                bool aiAccepts = true;
                int trStated = REFUSE_NONE;
                if (m_ai && trTgtCid >= 0 && trTgtCid != m_playerCountryId)
                    aiAccepts = m_ai->decideDiplomacy(trTgtCid, "propose_trade",
                                                      da.sourceIso, std::string(), &trStated);

                if (aiAccepts) {
                    if (tit != m_pendingCeasefireTerms.end()) {
                        // endsWar = false: this is the whole difference.
                        // WHAT THE DEAL WAS WORTH, to each side, in gold.
                        //
                        // Credited here rather than left to the end of the game,
                        // because that is the whole problem this answers: a trade
                        // resolved at turn 40 was previously rewarded only by how
                        // the country looked at turn 400, where one deal is noise.
                        // Land is valued the same way both halves of a trade
                        // already value it -- income times a payback period -- so
                        // the reward, the asking price and the receiver's features
                        // are finally three views of one number.
                        if (m_ai) {
                            const CeasefireTerms& tt = tit->second;
                            auto landGold = [&](const std::vector<int>& pids) {
                                float sum = 0.0f;
                                for (int pid : pids) {
                                    auto pit = m_provinceIndustry.find(pid);
                                    const float perTurn = (pit != m_provinceIndustry.end())
                                                        ? pit->second.income : 0.0f;
                                    sum += std::clamp(perTurn * 24.0f, 120.0f, 1400.0f);
                                }
                                return sum;
                            };
                            // Same asymmetry the receiver's valuation uses: a
                            // release costs the country doing it and is worth a
                            // quarter to the other side. See AISystem.
                            const float srcGain = landGold(tt.theirProvs) - landGold(tt.ourProvs)
                                                - landGold(tt.ourReleaseProvs)
                                                + 0.25f * landGold(tt.theirReleaseProvs)
                                                + (float)tt.theirMoney - (float)tt.ourMoney;
                            const int srcCid = cidForIso(da.sourceIso);
                            if (srcCid >= 0) m_ai->noteTradeOutcome(srcCid,  srcGain);
                            if (trTgtCid >= 0) m_ai->noteTradeOutcome(trTgtCid, -srcGain);
                        }
                        applyCeasefireTerms(da.sourceIso, da.targetIso, tit->second,
                                            da.sourceIso == playerIso, false);
                        m_pendingCeasefireTerms.erase(tit);
                    }
                    if (da.sourceIso == playerIso || da.targetIso == playerIso) {
                        const Country* otherC = m_countries.getCountryByCode(
                            (da.sourceIso == playerIso) ? da.targetIso : da.sourceIso);
                        std::string otherName = otherC ? otherC->name
                                              : (da.sourceIso == playerIso ? da.targetIso : da.sourceIso);
                        pushPopup(PopupType::WAR_DECLARED, "Trade Agreed",
                            otherName + " has accepted your trade proposal.",
                            0, "trade_accepted", da.sourceIso, da.targetIso);
                    }
                } else {
                    // Refunded on exactly the same rule as a rejected ceasefire:
                    // only the player pays up front, so only the player is owed
                    // anything back. Refunding an AI sender would mint treasury.
                    if (tit != m_pendingCeasefireTerms.end()) {
                        int refund = tit->second.ourMoney;
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
                        pushPopup(PopupType::WAR_DECLARED, "Trade Rejected",
                            otherName + " has rejected your trade proposal" +
                            (refusalText(trStated) ? std::string(" — ") + refusalText(trStated)
                                                   : std::string()) + ".",
                            0, "trade_rejected", da.sourceIso, da.targetIso);
                    }
                }
                printf("[TRADE] %s -> %s (accepted=%d)\n",
                       da.sourceIso.c_str(), da.targetIso.c_str(), aiAccepts);
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
                } else {
                    // Same white-peace hole as the branch above.
                    withdrawArmiesAfterPeace(cidForIso(da.sourceIso),
                                             cidForIso(da.targetIso));
                }
                printf("[CEASEFIRE] Accepted offer applied: %s vs %s\n", da.sourceIso.c_str(), da.targetIso.c_str());
            } else if (da.action == "apply_trade") {
                // The deferred half of a trade the player accepted. Same shape
                // as apply_ceasefire and deliberately missing the same two
                // lines: no war ends, and endsWar=false keeps the armies where
                // they are.
                std::string key = da.sourceIso + "|" + da.targetIso;
                auto tit = m_acceptedCeasefireTerms.find(key);
                if (tit != m_acceptedCeasefireTerms.end()) {
                    applyCeasefireTerms(da.sourceIso, da.targetIso, tit->second, false, false);
                    m_acceptedCeasefireTerms.erase(tit);
                }
                printf("[TRADE] Accepted offer applied: %s -> %s\n",
                       da.sourceIso.c_str(), da.targetIso.c_str());
            } else if (da.action == "declare_war") {
                // Guarantee chains + kin penalties + notifications all live in
                // the helper so every declaration behaves identically.
                declareWar(da.sourceIso, da.targetIso, true, da.statedGoal);
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
        if (m_ai) m_ai->noteTreatyTransfer(toCid, fromCid);
        pp->countryId = toCid;
        if ((size_t)pid < m_provinceCountryLookup.size())
            m_provinceCountryLookup[pid] = toCid;
        reindexProvinceOwner(pid, fromCid, toCid);
        // EVERY PROVINCE A COUNTRY OWNS SITS IN EXACTLY ONE OF ITS DISTRICTS.
        // Ground that changes hands would otherwise stay in the loser's
        // district -- policed with their budget, on somebody else's land -- and
        // arrive in none of the winner's, policed with nobody's. Both sides,
        // because the invariant is about both. No-ops for an undivided country,
        // which is most of them.
        reconcileDistricts(fromCid);
        reconcileDistricts(toCid);
        // Update per-pixel country array + move countryPixels
        auto ppIt = m_provincePixels.find(pid);
        if (ppIt != m_provincePixels.end()) {
            const auto& px = ppIt->second;
            for (int idx : px)
                if (idx >= 0 && idx < (int)m_pixelCountryArray.size())
                    m_pixelCountryArray[idx] = (uint16_t)toCid;
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

        // A claim is a demand for land you do not have. Winning the land ends
        // the demand -- so the new owner's own claim on this province is now
        // meaningless, and leaving it in place kept the province painted as
        // contested on the claims overlay, kept listing us under "Claimed by"
        // on ground we had just been ceded, kept it feeding the AI's
        // "reconquer our claimed land" war bar, and let a country justify a
        // fresh war over ground it already held.
        if (const Country* toC = m_countries.getCountry(toCid))
            revokeClaim(toC->isoA3, pid);
}

void Game::applyCeasefireTerms(const std::string& sourceIso, const std::string& targetIso, const CeasefireTerms& terms, bool alreadyDeducted, bool endsWar) {
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

    // ── NATIONS SET FREE BY THE TREATY ──
    //
    // AFTER the cessions above, deliberately: a province that changes hands in
    // the same settlement belongs to its new owner before anyone asks whether
    // it can be released, and doing it the other way round would let a country
    // free ground it is about to hand over anyway.
    //
    // releaseNation re-checks everything -- ownership, the minimum size, the
    // maximum share -- and refuses the whole release rather than carrying out
    // half of it. That matters more here than on the button it was written
    // for: these provinces were chosen when the offer was made, and a front
    // that moved in between is exactly the case its checks exist for. A
    // refusal leaves the rest of the treaty standing, which is the right
    // answer: the war still ends, the money still moves, and the nation
    // that could not be freed simply is not.
    auto freeNation = [&](int ownerCid, const std::string& tag,
                          const std::vector<int>& provs, const char* who) {
        if (tag.empty() || provs.empty() || ownerCid <= 0) return;
        ReleaseCandidate rc;
        rc.minority = tag;
        rc.provinces = provs;
        std::sort(rc.provinces.begin(), rc.provinces.end());
        for (int pid : rc.provinces) {
            auto popIt = m_provincePopulations.find(pid);
            if (popIt != m_provincePopulations.end()) rc.population += popIt->second;
        }
        const int newCid = releaseNation(ownerCid, rc);
        if (newCid > 0)
            printf("[CEASEFIRE] %s releases %s on %zu province(s)\n",
                   who, tag.c_str(), provs.size());
        else
            printf("[CEASEFIRE] %s could NOT release %s -- the ground no longer "
                   "supports it; the rest of the treaty stands\n", who, tag.c_str());
    };
    freeNation(srcCid, terms.ourReleaseTag,   terms.ourReleaseProvs,   sourceIso.c_str());
    freeNation(tgtCid, terms.theirReleaseTag, terms.theirReleaseProvs, targetIso.c_str());

    auto dropClaim = [&](const std::string& claimantIso, int pid) {
        revokeClaim(claimantIso, pid);
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
    if (!m_aiTraining) m_politicalRepaintPending = true;

    // The war is over, so nobody's troops may still be standing on the other's
    // soil. Done last, after every province transfer above, so ownership is
    // final when we decide what counts as foreign territory.
    //
    // Skipped for a trade, which ends no war: the two may still be at peace and
    // simply swapping ground, or at war with third parties whose fronts this
    // has nothing to do with. Walking armies home there would hand out a free
    // retreat in exchange for a province.
    if (endsWar) withdrawArmiesAfterPeace(srcCid, tgtCid);

    // A war the player has just ended with ground to show for it. That is the
    // moment the rating prompt waits for -- see maybeOfferRating(). Gated on
    // endsWar because a trade moves provinces without settling anything, and on
    // the player having GAINED because being made to cede territory is the same
    // event from the losing side and is nobody's idea of a high point.
    //
    // Not shown here: the prompt opens on a later quiet frame, not on top of the
    // peace terms the player is still reading.
    if (endsWar && !m_aiTraining) {
        const bool playerGained =
            (srcCid == m_playerCountryId && !terms.theirProvs.empty()) ||
            (tgtCid == m_playerCountryId && !terms.ourProvs.empty());
        if (playerGained) m_ratingMoment = true;
    }
}

// === expelStrandedArmies ===
//
// Sweeps the map once a turn for stacks standing on ground that is neither
// their own nor an ally's, and walks them home.
//
// This is a backstop, not the primary fix: the routes that STRAND troops are
// fixed where they occur (a revolt retreats the parent's garrison, a ceasefire
// withdraws both sides). But the number of ways a province can change hands --
// revolt, cession, conquest, a third party taking ground out from under an
// ally, a state dying with its army abroad -- means enumerating them all is a
// losing game, and every one that gets missed leaves an army parked in a
// neutral country forever, because nothing here attrits or expels one.
//
// There is no military-access treaty in this game, so foreign presence has no
// legitimate form outside an alliance, and the rule needs no exceptions beyond
// unowned land.
void Game::expelStrandedArmies() {
    struct Intrusion { int pid; int cid; };
    std::vector<Intrusion> work;
    for (const auto& [pid, units] : m_provinceArmies) {
        const int owner = (pid >= 0 && (size_t)pid < m_provinceCountryLookup.size())
                            ? m_provinceCountryLookup[pid] : 0;
        // Unowned, blocked and special tiles are nobody's sovereignty to violate.
        if (owner <= 0 || owner == SPC_CID || owner == UNC_CID || owner == BLC_CID) continue;
        const Country* oc = m_countries.getCountry(owner);
        if (!oc) continue;
        for (const auto& u : units) {
            if (u.count <= 0 || u.countryId <= 0 || u.countryId == owner) continue;
            const Country* uc = m_countries.getCountry(u.countryId);
            if (!uc) continue;
            const CountryRelation& r = m_relations[uc->isoA3][oc->isoA3];
            // An ALLY may stand here; that is what staging is. Nobody else
            // may, and that now includes an enemy.
            //
            // A war used to be a reason to leave a stack alone, on the
            // reasoning that an army in enemy territory is an invasion. It
            // cannot be one any more: resolveAssault settles every assault the
            // turn it happens, so a stack only ends up inside a country it is
            // fighting when some transition put it there without a battle --
            // war declared while it was staged on an ally's soil, or the
            // ground ceded, revolted or conquered out from under it. Those are
            // the armies the player sees sitting on their provinces having
            // never taken them. They march home, exactly as they would at the
            // end of a war.
            if (r.alliance) continue;
            work.push_back({pid, u.countryId});
        }
    }
    // withdrawArmiesAfterPeace already knows how to walk a stack home and how
    // to intern one that cannot get there, so this reuses it rather than
    // growing a second, subtly different copy of that logic.
    for (const auto& w : work) {
        const int owner = m_provinceCountryLookup[w.pid];
        withdrawArmiesAfterPeace(owner, w.cid);
    }
}

// === noteRealConquest ===
void Game::noteRealConquest(int newOwner, int prevOwner) {
    if (newOwner <= 0 || prevOwner <= 0 || newOwner == prevOwner) return;
    if (newOwner >= REBEL_CID_MIN || prevOwner >= REBEL_CID_MIN) return;
    if (newOwner == UNC_CID || newOwner == BLC_CID || newOwner == SPC_CID) return;
    if (prevOwner == UNC_CID || prevOwner == BLC_CID || prevOwner == SPC_CID) return;
    m_realConquests++;
}

// === provinceIsPlayers / shipIsPlayers ===
bool Game::provinceIsPlayers(int pid) const {
    if (m_playerCountryId == SPC_CID) return true;   // a spectator sees the lot
    const Province* p = m_provinces.getProvinceById(pid);
    return p && p->countryId == m_playerCountryId;
}

bool Game::shipIsPlayers(int shipIndex) const {
    if (m_playerCountryId == SPC_CID) return true;
    return shipIndex >= 0 && shipIndex < (int)m_ships.size() &&
           m_ships[shipIndex].countryId == m_playerCountryId;
}

// === atWarCids ===
bool Game::atWarCids(int a, int b) const {
    if (a <= 0 || b <= 0 || a == b) return false;
    const Country* ca = m_countries.getCountry(a);
    const Country* cb = m_countries.getCountry(b);
    if (!ca || !cb) return false;
    auto it = m_relations.find(ca->isoA3);
    if (it == m_relations.end()) return false;
    auto jt = it->second.find(cb->isoA3);
    return jt != it->second.end() && jt->second.war;
}

// === alliedCids ===
bool Game::alliedCids(int a, int b) const {
    if (a <= 0 || b <= 0 || a == b) return false;
    const Country* ca = m_countries.getCountry(a);
    const Country* cb = m_countries.getCountry(b);
    if (!ca || !cb) return false;
    return hasRelation(ca->isoA3, cb->isoA3, &CountryRelation::alliance);
}

// === transferCountryPixels ===
void Game::transferCountryPixels(int pid, int newOwner, int oldOwner) {
    if (m_countryPixels.empty()) return;
    // m_provincePixels IS BUILT ON DEMAND, AND THIS IS ONE OF THE DEMANDS.
    //
    // Without this the lookup below missed and the function returned having
    // done nothing at all -- no m_pixelCountryArray update, no move between
    // the two countries' pixel lists. Those lists are what the political,
    // relations, population and claims overlays paint from, so every province
    // taken by force kept being drawn in its FORMER owner's colour: the
    // Relations view showing someone else's province as yours, indefinitely.
    //
    // It only ever worked by accident. ensureProvincePixels() is called by the
    // Claims view, so a player who had opened Claims before conquering
    // anything got correct overlays and a player who had not did not, which is
    // why this looks intermittent.
    //
    // Skipped while training: that path returns below without touching the
    // lists anyway, and building the index there would cost memory nothing
    // reads.
    if (!m_aiTraining) ensureProvincePixels();
    auto ppIt = m_provincePixels.find(pid);
    if (ppIt == m_provincePixels.end()) return;
    const auto& provincePixels = ppIt->second;
    for (int idx : provincePixels)
        if (idx >= 0 && idx < (int)m_pixelCountryArray.size())
            m_pixelCountryArray[idx] = (uint16_t)newOwner;
    // m_countryPixels only feeds texture generation (political, relations,
    // population, claims overlays) -- never game logic. Maintaining it costs a
    // full scan of the owning country's pixel list on every province capture,
    // which profiled at ~39% of self-play runtime. Headless training draws its
    // minimap from m_provinceCountryLookup, so the list is pure overhead there.
    if (m_aiTraining) return;
    if (oldOwner >= 0 && (size_t)oldOwner < m_countryPixels.size() &&
        newOwner >= 0 && (size_t)newOwner < m_countryPixels.size()) {
        auto& oldPx = m_countryPixels[oldOwner];
        auto& newPx = m_countryPixels[newOwner];
        std::unordered_set<int> pxSet(provincePixels.begin(), provincePixels.end());
        std::vector<int> transferred;
        transferred.reserve(provincePixels.size());
        auto newEnd = std::remove_if(oldPx.begin(), oldPx.end(), [&](int idx) {
            if (pxSet.count(idx)) { transferred.push_back(idx); return true; }
            return false;
        });
        oldPx.erase(newEnd, oldPx.end());
        newPx.insert(newPx.end(), transferred.begin(), transferred.end());
    }
}

// === addTroopsTo ===
void Game::addTroopsTo(int pid, int cid, int count, TroopType type) {
    if (count <= 0 || cid <= 0) return;
    auto& units = m_provinceArmies[pid];
    // MERGED ON COUNTRY AND TYPE, not country alone. A province holds one entry
    // per (country, type) now, so a country with militia and mechanised in the
    // same place has two -- and merging by country would quietly turn one kind
    // of soldier into another, which is the sort of bug that shows up as a
    // balance complaint months later rather than as a crash.
    for (auto& u : units)
        if (u.countryId == cid && u.type == type) { u.count += count; return; }
    ArmyUnit nu; nu.countryId = cid; nu.count = count; nu.type = type;
    units.push_back(nu);
}

// === mayTakeProvince ===
bool Game::mayTakeProvince(int cid, int pid) const {
    const Province* p = m_provinces.getProvinceById(pid);
    if (!p || cid <= 0) return false;
    const int owner = p->countryId;
    if (owner <= 0 || owner == cid || owner == BLC_CID) return false;
    if (alliedCids(cid, owner)) return false;
    // Unclaimed ground is colonised by standing on it. Everything else is
    // somebody's country, and taking one of those needs a war -- which was
    // checked in Game::canMoveTo, i.e. in the player's move validator and
    // nowhere else. The AI happens to only ever target countries it is
    // fighting; a mod calling the move API and a modified multiplayer client
    // had no such manners.
    if (owner == UNC_CID) return true;
    return atWarCids(cid, owner);
}

// === mayEnterProvince ===
//
// Whether an army may set foot in a province at all, as opposed to whether it
// may KEEP it (mayTakeProvince). The two came apart with visible results: a
// move into a neutral country found no defenders, could not take the ground
// because there was no war, and then put the troops there anyway -- so the
// United States kept a garrison inside Mexico while Mexico still owned it, at
// peace, and the province panel dutifully reported both.
//
// The entry rule used to live in the player's move validator and nowhere
// else, which is the same shape of bug as every other rule written in the UI:
// it binds the human and nothing else. The AI, a mod calling the move API and
// a modified multiplayer client all walked straight through it. It lives here
// now, where the move is actually carried out.
bool Game::mayEnterProvince(int cid, int pid) const {
    const Province* p = m_provinces.getProvinceById(pid);
    if (!p || cid <= 0) return false;
    const int owner = p->countryId;
    if (owner <= 0 || owner == cid) return true;      // ours, or nobody's
    if (owner == UNC_CID || owner == BLC_CID) return true;   // unclaimed ground
    if (alliedCids(cid, owner)) return true;          // staging on allied soil
    return atWarCids(cid, owner);                     // otherwise it takes a war
}

// === captureProvince ===
void Game::captureProvince(int newOwner, int pid, bool contested) {
    // OD_ECON_TRACE=<cid>: every province that country takes or loses, so a
    // ledger step can be tied to the map event that caused it.
    {
        static const int traceCid = std::getenv("OD_ECON_TRACE") ? atoi(std::getenv("OD_ECON_TRACE")) : -1;
        if (traceCid >= 0) {
            const Province* p0 = m_provinces.getProvinceById(pid);
            const int oldOwner = p0 ? p0->countryId : -1;
            if (oldOwner == traceCid || newOwner == traceCid)
                fprintf(stderr, "[CAPTURE] turn %d pid=%d %d -> %d%s\n", m_turnNumber, pid, oldOwner, newOwner,
                        contested ? " (contested)" : "");
        }
    }
    Province* p = m_provinces.getProvinceById(pid);
    if (!p || newOwner <= 0) return;
    const int prevOwner = p->countryId;
    if (prevOwner == newOwner) return;

    // The ground has moved, so both sides' supply routes have. Only these two
    // countries are forgotten rather than the whole cache: a busy turn takes
    // hundreds of provinces, and rebuilding every country's map on each one
    // would cost more than the rule is worth. Everyone else's routes are
    // unchanged by a province passing between two other countries -- except an
    // ally supplying ACROSS it, which is a smaller error than a stale answer
    // for the two countries actually fighting, and it is corrected next turn.
    invalidateSupply(prevOwner, newOwner);

    if (m_ai) m_ai->noteConquest(newOwner, prevOwner, contested);
    noteRealConquest(newOwner, prevOwner);
    // BEFORE the auto-claim below hands the previous owner a claim: this asks
    // whether the ATTACKER ever claimed it.
    claimsBrokenByConquest(newOwner, prevOwner, pid);
    p->countryId = newOwner;
    if (pid > 0 && (size_t)pid < m_provinceCountryLookup.size())
        m_provinceCountryLookup[pid] = newOwner;
    reindexProvinceOwner(pid, prevOwner, newOwner);

    // ── AN ORDER DIES WITH THE GROUND IT WAS GIVEN FOR ──
    //
    // A disband order names a province and nothing else, and
    // processDisbandOrders SKIPS one whose province is not currently owned by
    // the country being processed -- it does not erase it. So an order queued
    // over a province that was then lost survived indefinitely, and fired on
    // the NEW garrison if the province was ever retaken, arbitrarily many turns
    // later: a player recaptures a province and the army that took it disbands
    // itself for a decision somebody made before the province changed hands
    // twice.
    //
    // Move orders out of the province are dropped for the same reason: they
    // were an instruction about an army that is no longer there.
    m_pendingDisbandOrders.erase(
        std::remove_if(m_pendingDisbandOrders.begin(), m_pendingDisbandOrders.end(),
            [pid](const PendingDisbandOrder& d) { return d.provinceId == pid; }),
        m_pendingDisbandOrders.end());
    m_pendingMoveOrders.erase(
        std::remove_if(m_pendingMoveOrders.begin(), m_pendingMoveOrders.end(),
            [pid](const PendingMoveOrder& mo) { return mo.fromProvince == pid; }),
        m_pendingMoveOrders.end());
    transferCountryPixels(pid, newOwner, prevOwner);
    // Conquered ground: its minorities like the new government rather less.
    auto minIt = m_provinceMinorities.find(pid);
    if (minIt != m_provinceMinorities.end())
        for (auto& mg : minIt->second)
            addMinorityDrift(newOwner, mg.name, -25.0f);
    // Auto-claim: the previous owner wants it back. After the transfer, not
    // before -- grantClaim refuses a claim on ground the claimant still holds.
    if (prevOwner > 0 && prevOwner != UNC_CID && prevOwner != BLC_CID &&
        prevOwner != newOwner)
        if (const Country* prevC = m_countries.getCountry(prevOwner))
            grantClaim(prevC->isoA3, pid);
    if (const Country* conqueror = m_countries.getCountry(newOwner))
        revokeClaim(conqueror->isoA3, pid);
}

// === weighAssault ===
//
// Everything both a fresh assault and a battle round have to agree about. See
// Game::AssaultPowers: the point of it existing is that there is one of it.
Game::AssaultPowers Game::weighAssault(int attackerCid, int pid, const ForceComposition& attackers,
                                       bool fromTheSea) const {
    AssaultPowers w;
    const Province* dst = m_provinces.getProvinceById(pid);
    if (!dst) return w;

    float fortDef = 0;
    auto indIt = m_provinceIndustry.find(pid);
    if (indIt != m_provinceIndustry.end()) fortDef = indIt->second.fortification * 10.0f;
    const double fortMul = 1.0 + fortDef / 100.0;
    w.atkMod = 1.0 + getTotalEffect("armyAtkPct", attackerCid) / 100.0;

    auto armIt = m_provinceArmies.find(pid);
    static const std::vector<ArmyUnit> kNone;
    const std::vector<ArmyUnit>& dstArmies = (armIt != m_provinceArmies.end()) ? armIt->second : kNone;
    auto isHostile = [&](const ArmyUnit& u) {
        return u.count > 0 && u.countryId > 0 && u.countryId != attackerCid &&
               !alliedCids(attackerCid, u.countryId);
    };
    for (const auto& u : dstArmies) if (isHostile(u)) w.defTroops += u.count;

    w.width = combatWidth(pid);

    // ── HOW MUCH OF EACH SIDE IS ON THE LINE ──
    //
    // The frontage is consumed by men WEIGHTED BY THEIR KIND, not by headcount:
    // mechanised deploy on less of it than militia do. An all-line force needs
    // exactly its own headcount, so this is the old arithmetic for every world
    // that has one.
    // Computed as MEN FIRST and the fraction derived from it, not the other way
    // round. `min(total, width)` is exact integer arithmetic; `total x
    // (width/total)` is the same number in algebra and one ulp away from it in
    // floating point, and a single man's difference in who is on the line
    // cascades through a whole campaign. For an all-line force perMan is
    // exactly 1.0 and every line below reduces to what it was.
    const long long atkTotal = attackers.total();
    const double atkPerMan =
        atkTotal > 0 ? attackers.frontageNeeded() / (double)atkTotal : 1.0;
    const long long atkFits = (long long)((double)w.width / std::max(1e-9, atkPerMan));
    w.engagedAtk = std::min(atkTotal, atkFits);
    w.reserveAtk = atkTotal - w.engagedAtk;
    w.atkEngagedFrac = atkTotal > 0 ? (double)w.engagedAtk / (double)atkTotal : 0.0;

    ForceComposition defComp;
    for (const auto& u : dstArmies) if (isHostile(u)) defComp.add(u.type, u.count);
    // Same shape the old rule had, including returning exactly 1.0 when the
    // defence fits -- so a fight nobody overfills is untouched by any of this.
    const double defNeed = defComp.frontageNeeded();
    w.defShare = (defNeed > (double)w.width && defNeed > 0.0)
                     ? (double)w.width / defNeed : 1.0;

    for (const auto& u : dstArmies) {
        if (!isHostile(u)) continue;
        const double defMod = 1.0 + getTotalEffect("armyDefPct", u.countryId) / 100.0;
        const double defSupply = (double)supplyFactor(u.countryId, pid);
        ++m_supplyDefChecks;
        if (defSupply < 1.0f - 1e-6) ++m_supplyPenalisedDefender;
        if (defSupply <= (double)SUPPLY_CUTOFF + 1e-6) ++m_supplyCutOffDefender;
        // ...and weighted by what kind of soldier he is. TROOP_TYPES[].def is
        // 1.0 for line infantry, so this is `u.count` for every existing world.
        const double worth = (double)u.count * (double)troopCost(u.type).def;
        w.defPower += worth * w.defShare * fortMul * defMod * defSupply;
    }
    // Depth is reserves behind the line, so it is measured against how many men
    // the line HOLDS -- which depends on what they are. For line infantry that
    // is the width itself, exactly as before.
    const long long defTotal = defComp.total();
    const double defPerMan =
        defTotal > 0 ? defNeed / (double)defTotal : 1.0;
    const long long defFits = (long long)((double)w.width / std::max(1e-9, defPerMan));
    w.atkDepth = (double)depthFactor(atkTotal, atkFits);
    w.defPower *= (double)depthFactor(w.defTroops, defFits);
    // ONE PATH. A landing used to be handed SUPPLY_BEACHHEAD outright; it now
    // asks the same question everyone else asks, and gets the beachhead answer
    // because its own hull is by definition within range. The difference shows
    // on the turns AFTER the landing, when the fleet may not be.
    // ── OD_TYPE_INVARIANT=1: THE PROOF THAT THIS CHANGED NOTHING ──
    //
    // Recomputes the pre-types arithmetic beside the typed arithmetic and
    // complains if they ever differ for an ALL-LINE fight, which is every fight
    // in every world that has not researched another kind.
    //
    // A property, not a comparison, and that is the point: the shared tree
    // moved under a two-run comparison FIVE times in one day, and each time the
    // honest answer was "that is not your change" arrived at after the fact.
    // This cannot be wrong about whose change it is. Measured 0 divergences
    // across three seeds and 60 turns, covering the men on the line, both
    // sides' share of the frontage, and both powers.
    //
    // Kept rather than deleted: when a second kind exists it still proves that
    // every all-line fight -- which will remain most of them -- is untouched.
    // The env is read once into a static, so it costs nothing when off.
    static const bool checkInvariant = std::getenv("OD_TYPE_INVARIANT") != nullptr;
    if (checkInvariant) {
        bool allLine = true;
        for (int t = 1; t < (int)TROOP_TYPE_COUNT; ++t) if (attackers.men[t]) allLine = false;
        for (const auto& u : dstArmies) if (isHostile(u) && u.type != TROOP_LINE) allLine = false;
        if (allLine) {
            const long long oldEngaged = std::min(atkTotal, w.width);
            const double oldDefShare = (w.defTroops > w.width && w.defTroops > 0)
                                           ? (double)w.width / (double)w.defTroops : 1.0;
            double oldDefPower = 0.0;
            for (const auto& u : dstArmies) {
                if (!isHostile(u)) continue;
                const double dm = 1.0 + getTotalEffect("armyDefPct", u.countryId) / 100.0;
                oldDefPower += (double)u.count * oldDefShare * fortMul * dm *
                               (double)supplyFactor(u.countryId, pid);
            }
            oldDefPower *= (double)depthFactor(w.defTroops, w.width);
            const double oldAtkSupply = (double)supplyFactor(attackerCid, pid);
            const double oldAtkPower = (double)oldEngaged * w.atkMod *
                                       (double)depthFactor(atkTotal, w.width) * oldAtkSupply;
            const double newAtkPower = (double)w.engagedAtk *
                (atkTotal > 0 ? attackers.weighted(&TroopCost::atk) / (double)atkTotal : 1.0) *
                w.atkMod * w.atkDepth * oldAtkSupply;
            if (oldEngaged != w.engagedAtk || oldDefShare != w.defShare ||
                oldDefPower != w.defPower || oldAtkPower != newAtkPower)
                fprintf(stderr, "[TYPEINV] engaged %lld/%lld defShare %.17g/%.17g defPower %.17g/%.17g atkPower %.17g/%.17g\n",
                        oldEngaged, w.engagedAtk, oldDefShare, w.defShare,
                        oldDefPower, w.defPower, oldAtkPower, newAtkPower);
        }
    }
    (void)fromTheSea;   // the sea-supply question is asked inside supplyFactor
    w.atkSupply = (double)supplyFactor(attackerCid, pid);
    // Attack power is the men on the line weighted by their kind, which for an
    // all-line force is simply the men on the line.
    // The MEN ON THE LINE times what their kind is worth. For an all-line force
    // the average weight is exactly 1.0 and this is `engagedAtk`, to the bit.
    const double atkAvg = atkTotal > 0
        ? attackers.weighted(&TroopCost::atk) / (double)atkTotal : 1.0;
    w.atkPower = (double)w.engagedAtk * atkAvg * w.atkMod * w.atkDepth * w.atkSupply;
    return w;
}

// === resolveAssault ===
//
// THE WHOLE GARRISON DEFENDS, AND EVERY DEFENDER IS SETTLED.
//
// What this replaces fought `find_if(first hostile stack)` and nothing else.
// Two enemies in one province meant one of them was invisible: not counted in
// the defence, not killed by the assault, and still standing there afterwards
// on ground that had just changed hands. That is the "enemy troops on my
// territory that never took the province" report, and it is also why the same
// attack could win or lose depending on which stack happened to be first in
// the vector.
//
// THE ARITHMETIC, stated once so both sides read the same way:
//
//   attack  = troops x (1 + armyAtkPct/100)                 -- attacker's research
//   defence = SUM over hostile stacks of
//             troops x (1 + fort x 10/100) x (1 + armyDefPct/100)
//                                                           -- each stack's own
//   the attack carries if attack > defence.
//
// Losses are the other side's power converted back into bodies at your own
// multiplier: the winner loses `loser power / winner multiplier`. Carried, the
// attacker keeps `(attack - defence) / atkMod`; repulsed, the defenders lose
// `attack x stackTroops / defence`, spread over the stacks in proportion to
// what each contributed.
//
// That last part is the fix to the defender's side of the sum. Losses used to
// be `attackers x atkMod x (1 - fort/200)` with no armyDefPct anywhere in it,
// so defensive research decided whether you held the province and then counted
// for nothing in what holding it cost -- Fortress Doctrine bought a coin flip
// and no lives. With no research and no fort the two formulas agree exactly,
// so this is the same game with the missing term restored.
// === supply ===
//
// See SUPPLY_FREE_HOPS in Game.h for what this is for. In one line: a stack
// twenty provinces deep used to fight exactly as well as one at home.

void Game::invalidateSupply(int cidA, int cidB) {
    if (cidA > 0) { m_supplyCache.erase(cidA); m_seaSupplyCache.erase(cidA); }
    if (cidB > 0) { m_supplyCache.erase(cidB); m_seaSupplyCache.erase(cidB); }
}

int Game::supplyHops(int countryId, int provinceId) const {
    if (countryId <= 0 || provinceId <= 0) return -1;

    auto cached = m_supplyCache.find(countryId);
    if (cached == m_supplyCache.end()) {
        // ── BUILD THE MAP ──
        std::unordered_map<int, int> dist;

        // Ground this country may be supplied ACROSS: its own and its allies'.
        auto passable = [&](int pid) {
            const Province* p = m_provinces.getProvinceById(pid);
            if (!p || p->countryId <= 0) return false;
            return p->countryId == countryId || alliedCids(countryId, p->countryId);
        };

        // Sources: every port it holds, and its largest industrial province.
        // The .odmap has no capital, and the biggest factory town is the
        // closest honest stand-in for where an army is supplied from.
        std::vector<int> frontier;
        int bestInd = -1, bestIndPid = -1;
        auto ownIt = m_countryProvinces.find(countryId);
        if (ownIt != m_countryProvinces.end()) {
            for (int pid : ownIt->second) {
                auto po = m_provincePorts.find(pid);
                if (po != m_provincePorts.end() && po->second.level >= 1) {
                    if (!dist.count(pid)) { dist[pid] = 0; frontier.push_back(pid); }
                }
                auto ind = m_provinceIndustry.find(pid);
                const int lvl = (ind != m_provinceIndustry.end()) ? ind->second.level : 0;
                if (lvl > bestInd) { bestInd = lvl; bestIndPid = pid; }
            }
            // A country with no port still has somewhere its army comes from.
            // Without this a landlocked country would read as cut off
            // everywhere, which would be the rule firing hardest on exactly the
            // countries it has least to say about.
            if (bestIndPid > 0 && !dist.count(bestIndPid)) {
                dist[bestIndPid] = 0;
                frontier.push_back(bestIndPid);
            }
        }

        for (size_t head = 0; head < frontier.size(); ++head) {
            const int pid = frontier[head];
            const int d = dist[pid];
            auto nb = m_provinceNeighbors.find(pid);
            if (nb == m_provinceNeighbors.end()) continue;
            for (int n : nb->second) {
                if (dist.count(n)) continue;
                if (!passable(n)) continue;      // cannot supply across someone else's land
                dist[n] = d + 1;
                frontier.push_back(n);
            }
        }
        cached = m_supplyCache.emplace(countryId, std::move(dist)).first;
    }

    const auto& dist = cached->second;
    auto it = dist.find(provinceId);
    if (it != dist.end()) return it->second;

    // Ground we do not hold: an attacker is supplied INTO it from whichever of
    // its own neighbouring provinces is best supplied, one hop further on.
    int best = -1;
    auto nb = m_provinceNeighbors.find(provinceId);
    if (nb != m_provinceNeighbors.end())
        for (int n : nb->second) {
            auto d = dist.find(n);
            if (d == dist.end()) continue;
            if (best < 0 || d->second + 1 < best) best = d->second + 1;
        }
    return best;   // -1 when nothing of ours touches it
}

bool Game::seaSupplied(int countryId, int provinceId) const {
    auto& forCountry = m_seaSupplyCache[countryId];
    auto memo = forCountry.find(provinceId);
    if (memo != forCountry.end()) return memo->second;

    bool supplied = false;
    auto cit = m_provinceCenters.find(provinceId);
    if (cit != m_provinceCenters.end()) {
        for (const auto& ship : m_ships) {
            if (ship.countryId <= 0) continue;
            if (ship.countryId != countryId && !alliedCids(countryId, ship.countryId)) continue;
            int sx = 0, sy = 0;
            m_landSea.lonLatToPixel((float)ship.lon, (float)ship.lat, sx, sy);
            const float dx = cit->second.x - (float)sx, dy = cit->second.y - (float)sy;
            if (std::sqrt(dx * dx + dy * dy) <= shipMaxRangePx(ship)) { supplied = true; break; }
        }
    }
    forCountry[provinceId] = supplied;
    return supplied;
}

float Game::supplyFactor(int countryId, int provinceId) const {
    const int hops = supplyHops(countryId, provinceId);
    ++m_supplyChecks;
    if (hops < 0) {
        // No road home. A HULL OFFSHORE IS A SUPPLY LINE -- see seaSupplied.
        // This is the difference between a landing and an encirclement, which
        // a walk over land cannot tell apart: both have no route home, and one
        // of them has a fleet.
        ++m_supplyPenalised;
        if (seaSupplied(countryId, provinceId)) { ++m_supplySeaSupplied; return SUPPLY_BEACHHEAD; }
        ++m_supplyCutOff;
        return SUPPLY_CUTOFF;
    }
    if (hops <= SUPPLY_FREE_HOPS) return 1.0f;
    ++m_supplyPenalised;
    const float f = 1.0f - SUPPLY_FALLOFF * (float)(hops - SUPPLY_FREE_HOPS);
    return std::max(SUPPLY_MIN, f);
}

// === depthFactor ===
//
// See DEPTH_PER_DOUBLING in Game.h for why this exists and why it is a log.
float Game::depthFactor(long long troops, long long width) {
    if (width <= 0 || troops <= width) return 1.0f;
    const double ratio = (double)troops / (double)width;
    const double doublings = std::log2(ratio);
    const double f = 1.0 + (double)DEPTH_PER_DOUBLING * doublings;
    return (float)std::min((double)DEPTH_MAX, f);
}

// === combatWidth ===
//
// See COMBAT_WIDTH_PER_AREA. Ground and fortification, nothing else -- there is
// no terrain layer on these maps, and inventing one from the land raster would
// be a guess dressed as a rule.
long long Game::combatWidth(int provinceId) const {
    const double area = (double)provinceArea(provinceId);
    double w = std::max((double)COMBAT_WIDTH_MIN, area * (double)COMBAT_WIDTH_PER_AREA);
    auto ind = m_provinceIndustry.find(provinceId);
    if (ind != m_provinceIndustry.end() && ind->second.fortification > 0) {
        const double narrow = 1.0 - (double)ind->second.fortification *
                                    (double)COMBAT_WIDTH_FORT_PCT / 100.0;
        // Floored well above zero: a fort narrows a front, it does not seal a
        // province against any assault whatsoever.
        w *= std::max(0.25, narrow);
    }
    return (long long)std::max(1.0, w);
}

bool Game::resolveAssault(int attackerCid, int pid, int attackers, int& survivors,
                          int fallbackPid) {
    // The headcount form, kept for callers that have no composition to give --
    // the amphibious path, which lands whatever the hull carried. It raises
    // line infantry, which is what every one of them was already doing.
    ForceComposition c;
    c.add(TROOP_LINE, attackers);
    ForceComposition out;
    const bool took = resolveAssault(attackerCid, pid, c, out, fallbackPid);
    survivors = (int)std::min(out.total(), (long long)INT32_MAX);
    return took;
}

bool Game::resolveAssault(int attackerCid, int pid, const ForceComposition& attackers,
                          ForceComposition& survivorsOut, int fallbackPid) {
    survivorsOut = ForceComposition{};
    const long long attackerCount = attackers.total();
    int survivors = 0;
    Province* dst = m_provinces.getProvinceById(pid);
    if (!dst || attackerCid <= 0 || attackerCount <= 0) return false;

    // fallbackPid < 0 IS the amphibious path, by construction -- there is no
    // ground behind a landing to fall back to, which is the same fact that
    // makes a failed landing drown.
    const bool fromTheSea = (fallbackPid < 0);
    const AssaultPowers w = weighAssault(attackerCid, pid, attackers, fromTheSea);
    ++m_assaultsTotal;
    if (w.engagedAtk < attackerCount || w.defTroops > w.width) ++m_assaultsWidthBound;
    if (w.defTroops > 0) {
        ++m_assaultsContested;
        if (w.atkPower <= w.defPower) ++m_assaultsRepulsed;
    }

    auto& dstArmies = m_provinceArmies[pid];
    auto isHostile = [&](const ArmyUnit& u) {
        return u.count > 0 && u.countryId > 0 && u.countryId != attackerCid &&
               !alliedCids(attackerCid, u.countryId);
    };
    const bool mayTake = mayTakeProvince(attackerCid, pid);
    bool captured = false;

    // OD_ECON_TRACE=<cid>: every assault that country makes or receives.
    {
        static const int traceCid = std::getenv("OD_ECON_TRACE") ? atoi(std::getenv("OD_ECON_TRACE")) : -1;
        if (traceCid >= 0 && (attackerCid == traceCid || dst->countryId == traceCid)) {
            long long defOwn = 0;
            for (const auto& u : dstArmies) if (isHostile(u) && u.countryId == dst->countryId) defOwn += u.count;
            fprintf(stderr, "[BATTLE] turn %d pid=%d owner=%d atk=%d attackers=%lld engaged=%lld def=%lld (owner's %lld) width=%lld atkSupply=%.2f defSupply=%.2f atkPower=%.0f defPower=%.0f => %s\n",
                    m_turnNumber, pid, dst->countryId, attackerCid, attackerCount, w.engagedAtk, w.defTroops, defOwn, w.width,
                    w.atkSupply, w.defTroops > 0 ? (double)supplyFactor(dst->countryId, pid) : 1.0,
                    w.atkPower, w.defPower, w.defTroops <= 0 ? "walk-in" : w.atkPower > w.defPower ? "carried" : "repulsed");
        }
    }

    if (w.defTroops <= 0) {
        // Nobody home: the province is taken by walking into it, or simply
        // occupied if it is ours, an ally's, or somebody we are at peace with.
        if (mayTake) { captureProvince(attackerCid, pid, /*contested=*/false); captured = true; }
        survivorsOut = attackers;      // nobody home: everyone walks in, unchanged
        survivors = (int)std::min(attackerCount, (long long)INT32_MAX);
    } else if (w.atkPower > w.defPower) {
        // Carried. The whole garrison falls, and the attacker's reserve walks
        // in behind the men who won it.
        for (auto& u : dstArmies) if (isHostile(u)) u.count = 0;
        // BACK INTO MEN, and EVERY multiplier has to come out again or the
        // fight invents them: atkPower is engaged x atkMod x atkDepth x
        // atkSupply, so dividing by atkMod alone leaves a survivor count scaled
        // by the rest, and a deep, well-supplied stack could walk out of a
        // province with more men than the order sent into it. Divided by all of
        // them, then clamped to the men who actually marched.
        const double scale = w.atkMod * std::max(1e-9, w.atkDepth * w.atkSupply);
        const double throughEngaged = std::max(0.0, (w.atkPower - w.defPower) / scale);
        const double out = std::min(throughEngaged + (double)w.reserveAtk, (double)attackerCount);
        survivors = (int)std::min((double)INT32_MAX, out);
        // WHO survived, not just how many. Losses fall proportionally across
        // the kinds that were committed, so an army that went in mixed comes
        // out mixed rather than turning into whichever type the arithmetic
        // happened to name.
        survivorsOut = attackers;
        survivorsOut.removeMen(attackerCount - (long long)out);
        if (mayTake) { captureProvince(attackerCid, pid, /*contested=*/true); captured = true; }
    } else {
        // ── REPULSED, AND THE FIGHT IS NOT OVER ──
        //
        // The men who reached the line are lost, as they always were, and the
        // defenders lose in proportion to what the attack was worth. What has
        // changed is what happens to the RESERVE.
        //
        // It used to march home. That made a war a sequence of instants: there
        // was never a contested province at the end of a turn, so there was
        // never anything to decide INSIDE a war, only before one. Now it
        // stands -- see `struct Battle` -- and fights again next turn, and the
        // way out is a withdrawal somebody has to actually order. Retreat was
        // free and automatic; it is a decision now, which is the whole feature.
        //
        // A FAILED LANDING STILL DROWNS. fromTheSea has no ground to hold and
        // nowhere to withdraw to, which keeps the rule the amphibious doctrine
        // was measured against.
        for (auto& u : dstArmies) {
            if (!isHostile(u)) continue;
            const long long killed =
                (long long)std::llround(w.atkPower * (double)u.count * w.defShare /
                                        std::max(1e-9, w.defPower));
            u.count = (int)std::max(0LL, (long long)u.count - killed);
        }
        if (w.reserveAtk > 0 && !fromTheSea && fallbackPid > 0 && fallbackPid != pid) {
            // The men who never reached the line, by kind: the engaged are lost
            // and the reserve stands. Proportional, for the same reason as
            // above.
            ForceComposition reserve = attackers;
            reserve.removeMen(w.engagedAtk);
            if (Battle* existing = battleAt(pid, attackerCid)) {
                for (int t = 0; t < (int)TROOP_TYPE_COUNT; ++t)
                    existing->men.men[t] += reserve.men[t];
                ++m_battlesReinforced;
            } else {
                Battle nb;
                nb.provinceId = pid;
                nb.attackerCid = attackerCid;
                nb.men = reserve;
                nb.fromProvince = fallbackPid;
                nb.lastAtkPower = w.atkPower;
                nb.lastDefPower = w.defPower;
                nb.openingDefPower = w.defPower;
                nb.lastAtkLosses = w.engagedAtk;
                nb.totalAtkLosses = w.engagedAtk;
                m_battles.push_back(nb);
                ++m_battlesStarted;
            }
        }
        if (m_ai) m_ai->noteAssaultRepulsed(attackerCid, (int)w.engagedAtk);
    }

    if (survivors > 0) addTroopsTo(pid, attackerCid, survivors);
    auto it = m_provinceArmies.find(pid);
    if (it != m_provinceArmies.end()) {
        auto& units = it->second;
        units.erase(std::remove_if(units.begin(), units.end(),
                                   [](const ArmyUnit& u) { return u.count <= 0; }),
                    units.end());
        if (units.empty()) m_provinceArmies.erase(it);
    }
    return captured;
}

// === standing battles ===
//
// See `struct Battle` in GameStructs.h. In one line: an assault that does not
// finish now stands in the province instead of marching home, and the way out
// is a withdrawal somebody has to order.

long long Game::battleTroops(int countryId) const {
    long long n = 0;
    for (const auto& b : m_battles) if (b.attackerCid == countryId) n += b.attackers();
    return n;
}

long long Game::countryTroops(int countryId) const {
    long long n = battleTroops(countryId);
    for (const auto& [pid, units] : m_provinceArmies)
        for (const auto& u : units)
            if (u.countryId == countryId) n += u.count;
    return n;
}

Battle* Game::battleAt(int provinceId, int attackerCid) {
    for (auto& b : m_battles)
        if (b.provinceId == provinceId && b.attackerCid == attackerCid) return &b;
    return nullptr;
}
const Battle* Game::battleAt(int provinceId, int attackerCid) const {
    for (const auto& b : m_battles)
        if (b.provinceId == provinceId && b.attackerCid == attackerCid) return &b;
    return nullptr;
}
const Battle* Game::anyBattleAt(int provinceId) const {
    for (const auto& b : m_battles) if (b.provinceId == provinceId) return &b;
    return nullptr;
}

bool Game::hasPendingWithdraw(int provinceId) const {
    return std::find(m_pendingWithdraws.begin(), m_pendingWithdraws.end(), provinceId)
           != m_pendingWithdraws.end();
}

void Game::queueWithdraw(int provinceId) {
    auto it = std::find(m_pendingWithdraws.begin(), m_pendingWithdraws.end(), provinceId);
    if (it != m_pendingWithdraws.end()) m_pendingWithdraws.erase(it);   // a second press cancels
    else m_pendingWithdraws.push_back(provinceId);
}

void Game::withdrawFromBattle(int provinceId, int attackerCid) {
    for (size_t i = 0; i < m_battles.size(); ++i) {
        Battle& b = m_battles[i];
        if (b.provinceId != provinceId || b.attackerCid != attackerCid) continue;
        // Home only if we still hold it. A province lost while the battle was
        // being fought is not somewhere to withdraw to, and men with nowhere to
        // go are lost -- the same rule a repulsed landing follows.
        const Province* home = m_provinces.getProvinceById(b.fromProvince);
        if (b.attackers() > 0 && home && home->countryId == attackerCid)
            addTroopsTo(b.fromProvince, attackerCid,
                        (int)std::min(b.attackers(), (long long)INT32_MAX));
        m_battles.erase(m_battles.begin() + i);
        ++m_battlesWithdrawn;
        return;
    }
}

void Game::processBattles(int countryId) {
    // Withdrawals first: a player who ordered one this turn gets out BEFORE the
    // round is fought, which is what "withdraw" has to mean or the order is
    // only a way of choosing where the survivors of one more round end up.
    for (size_t i = 0; i < m_pendingWithdraws.size(); ) {
        const int pid = m_pendingWithdraws[i];
        if (battleAt(pid, countryId)) {
            withdrawFromBattle(pid, countryId);
            m_pendingWithdraws.erase(m_pendingWithdraws.begin() + i);
            continue;
        }
        ++i;
    }

    for (size_t i = 0; i < m_battles.size(); ) {
        Battle& b = m_battles[i];
        if (b.attackerCid != countryId) { ++i; continue; }
        const Province* dst = m_provinces.getProvinceById(b.provinceId);

        // Reasons a battle simply ends, before any fighting.
        const bool gone      = (dst == nullptr);
        const bool ours      = (dst && dst->countryId == countryId);
        const bool atPeace   = (dst && !mayEnterProvince(countryId, b.provinceId));
        const bool exhausted = (b.attackers() <= 0);
        if (gone || ours || atPeace || exhausted) {
            // If the ground became ours or friendly while we were fighting for
            // it -- somebody else took it, or a ceasefire was signed -- the men
            // are not lost, they are just no longer in a battle.
            if (!gone && !exhausted && (ours || atPeace))
                addTroopsTo(ours ? b.provinceId : b.fromProvince, countryId,
                            (int)std::min(b.attackers(), (long long)INT32_MAX));
            if (exhausted) ++m_battlesLost;
            m_battles.erase(m_battles.begin() + i);
            continue;
        }

        const AssaultPowers w = weighAssault(countryId, b.provinceId, b.men,
                                             /*fromTheSea=*/false);
        ++m_battleRounds;
        ++b.rounds;
        b.lastAtkPower = w.atkPower;
        b.lastDefPower = w.defPower;
        if (b.openingDefPower <= 0.0) b.openingDefPower = w.defPower;

        auto& dstArmies = m_provinceArmies[b.provinceId];
        auto isHostile = [&](const ArmyUnit& u) {
            return u.count > 0 && u.countryId > 0 && u.countryId != countryId &&
                   !alliedCids(countryId, u.countryId);
        };

        if (w.defTroops <= 0 || w.atkPower > w.defPower) {
            // The line broke. Same arithmetic as a carried assault.
            long long before = w.defTroops;
            for (auto& u : dstArmies) if (isHostile(u)) u.count = 0;
            long long survivors = b.attackers();
            if (w.defTroops > 0) {
                const double scale = w.atkMod * std::max(1e-9, w.atkDepth * w.atkSupply);
                const double through = std::max(0.0, (w.atkPower - w.defPower) / scale);
                survivors = (long long)std::min((double)b.attackers(), through + (double)w.reserveAtk);
            }
            b.lastDefLosses = before;
            b.lastAtkLosses = b.attackers() - survivors;
            b.totalDefLosses += before;
            b.totalAtkLosses += b.lastAtkLosses;
            if (mayTakeProvince(countryId, b.provinceId))
                captureProvince(countryId, b.provinceId, /*contested=*/true);
            if (survivors > 0)
                addTroopsTo(b.provinceId, countryId, (int)std::min(survivors, (long long)INT32_MAX));
            ++m_battlesWon;
            m_battles.erase(m_battles.begin() + i);
        } else {
            // Another round of grinding. The men on the line are lost and the
            // defenders lose in proportion, exactly as a repulse costs today --
            // the casualty model is deliberately unchanged.
            long long defLost = 0;
            for (auto& u : dstArmies) {
                if (!isHostile(u)) continue;
                const long long killed =
                    (long long)std::llround(w.atkPower * (double)u.count * w.defShare /
                                            std::max(1e-9, w.defPower));
                const int was = u.count;
                u.count = (int)std::max(0LL, (long long)u.count - killed);
                defLost += was - u.count;
            }
            // The men on the line are lost, spread across the kinds that were
            // on it -- not taken off whichever type the array happens to list
            // first.
            b.men.removeMen(w.engagedAtk);
            b.lastAtkLosses = w.engagedAtk;
            b.lastDefLosses = defLost;
            b.totalAtkLosses += w.engagedAtk;
            b.totalDefLosses += defLost;
            if (m_ai) m_ai->noteAssaultRepulsed(countryId, (int)w.engagedAtk);

            // THE SAFETY RAIL, not a rule anybody should meet. See
            // BATTLE_MAX_ROUNDS: without it a battle that never quite resolves
            // stands for the length of a campaign, and an attacker with no
            // withdraw reflex feeds it until it has no army.
            if (b.attackers() <= 0) {
                ++m_battlesLost;
                m_battles.erase(m_battles.begin() + i);
                continue;
            }
            if (b.rounds >= BATTLE_MAX_ROUNDS) {
                withdrawFromBattle(b.provinceId, countryId);
                continue;
            }
            ++i;
        }
        auto ai = m_provinceArmies.find(b.provinceId);
        if (ai != m_provinceArmies.end()) {
            auto& units = ai->second;
            units.erase(std::remove_if(units.begin(), units.end(),
                                       [](const ArmyUnit& u) { return u.count <= 0; }),
                        units.end());
            if (units.empty()) m_provinceArmies.erase(ai);
        }
    }
}

// === shipMaxRangePx / shipMaxRangeDeg ===
//
// ONE RULE FOR EVERY FLEET. These numbers used to exist only inside the ship
// action overlay in Game_Render -- so they were the PLAYER's rule and nothing
// else obeyed them. The AI steamed a flat 18 degrees a turn whatever it was
// sailing and bombarded at a flat 10, while the resolver checked no range at
// all, which made an AI boat cover 410 px against a player boat's 200 and let
// every AI hull ignore the navySpeedPct research the player benefits from.
//
// Owner-scoped, not player-scoped: getTotalEffect defaults to the human's
// research, so even the overlay was crediting the player's doctrine to
// whatever hull happened to be selected.
float Game::shipMaxRangePx(const NavyShip& s) const {
    const float base = (s.type == "boat")      ? 200.0f :
                       (s.type == "destroyer") ? 350.0f :
                       (s.type == "carrier")   ? 450.0f : 300.0f;
    return base * (1.0f + getTotalEffect("navySpeedPct", s.countryId) / 100.0f);
}

double Game::shipMaxRangeDeg(const NavyShip& s) const {
    // Equirectangular: 8192 px spans 360 degrees of longitude and 4096 spans
    // 180 of latitude, so both axes carry the same pixels-per-degree and one
    // conversion serves a Euclidean distance in either space.
    const int w = m_landSea.getWidth();
    if (w <= 0) return 18.0;   // pre-load fallback: the old constant
    return (double)shipMaxRangePx(s) / ((double)w / 360.0);
}

// === buildNavGrid ===
//
// See Game::NavGrid. One pass over the land raster, then a flood fill to label
// which stretches of water actually join up.
void Game::buildNavGrid() {
    m_nav = NavGrid{};
    const int W = m_landSea.getWidth(), H = m_landSea.getHeight();
    if (W <= 0 || H <= 0) return;

    // 32 raster pixels a side. On the shipped 8192x4096 maps that is a 256x128
    // grid: small enough to flood fill in a blink, fine enough that no strait a
    // fleet could actually use is missed.
    const int CELL = 32;
    m_nav.cell = CELL;
    m_nav.w = (W + CELL - 1) / CELL;
    m_nav.h = (H + CELL - 1) / CELL;
    const size_t n = (size_t)m_nav.w * m_nav.h;
    m_nav.navigable.assign(n, 0);
    m_nav.px.assign(n, -1);
    m_nav.py.assign(n, -1);
    m_nav.component.assign(n, -1);

    // ── TRUE CONNECTIVITY, AT RASTER RESOLUTION ──
    //
    // The components below used to be flood-filled over the CELLS: a cell was
    // navigable if it held any water at all, and two navigable cells were
    // joined if they touched. On the shipped 8192x4096 map a cell is 32 px --
    // about 1.4 degrees -- so a cell holding one pixel of an inland lake and a
    // neighbouring cell holding one pixel of ocean were declared the same sea.
    //
    // Measured on data/STDmaps/map.odmap: that rule put 23,907 of 24,111
    // navigable cells -- 99.2% of all water on Earth -- into a single body,
    // and among the things it declared reachable from the mid-Atlantic were
    // Lake Michigan, Lake Superior and LAKE TITICACA, 3,800 m up in the Andes.
    // It is why a Russian transport was screenshotted sitting in the Great
    // Lakes: navReachable said yes, so findEnemyPort targeted a Canadian
    // harbour and navRoute obligingly produced a route through Quebec.
    //
    // So connectivity is now decided by the raster and not by the grid: water
    // pixels are flood-filled at full resolution with a run-based union-find,
    // and a cell inherits the component of the water in it. The grid stays a
    // grid -- it is still what the BFS in navRoute walks -- but it can no
    // longer invent a canal that the map does not have.
    //
    // ONE PASS, ROW BY ROW: a component id per pixel would be 134 MB, so only
    // the runs are kept (a few tens per row) and a cell looks its pixel up by
    // binary search. This is the same shape of union-find the province
    // component labelling uses, for the same reason.
    struct Run { int x0, x1; int id; };
    std::vector<std::vector<Run>> rows((size_t)H);
    std::vector<int> parent;
    parent.reserve(1u << 16);
    auto makeSet = [&]() { parent.push_back((int)parent.size()); return (int)parent.size() - 1; };
    auto find = [&](int x) {
        int r = x;
        while (parent[r] != r) r = parent[r];
        while (parent[x] != r) { const int nx = parent[x]; parent[x] = r; x = nx; }
        return r;
    };
    auto unite = [&](int a, int b) {
        a = find(a); b = find(b);
        if (a != b) parent[b] = a;
    };

    // HOW WIDE A NECK OF LAND STILL COUNTS AS A STRAIT.
    //
    // The raster is a picture of coastlines, not a chart, and at 8192 px the
    // Bosphorus and the Dardanelles are simply not drawn -- so pixel-exact
    // connectivity locks the Black Sea, the Sea of Azov and the Sea of Marmara
    // away from the Mediterranean, which is a worse answer than the one being
    // replaced. Closing gaps of up to 8 px (about 40 km at the equator) opens
    // all three and nothing else: measured across the whole shipped raster,
    // every value from 8 to 16 px gives the identical answer, and the Great
    // Lakes, the Caspian, Baikal, Ladoga, Balkhash, Victoria, the Great Salt
    // Lake and Titicaca all stay where they belong. 24 px starts swallowing
    // Ladoga, so the plateau is wide and this sits at its near edge.
    const int STRAIT = 8;

    for (int y = 0; y < H; ++y) {
        auto& rs = rows[(size_t)y];
        bool inRun = false;
        for (int x = 0; x < W; ++x) {
            const bool water = !m_landSea.isLand(x, y);
            if (water && !inRun) { rs.push_back({x, x + 1, makeSet()}); inRun = true; }
            else if (water)      { rs.back().x1 = x + 1; }
            else                 { inRun = false; }
        }
        // A strait crossed left-to-right: two runs with only a neck between.
        for (size_t k = 1; k < rs.size(); ++k)
            if (rs[k].x0 - rs[k - 1].x1 <= STRAIT) unite(rs[k - 1].id, rs[k].id);
        // The world wraps in x, so the first and last run of a row touch.
        if (rs.size() > 1 && rs.front().x0 == 0 && rs.back().x1 == W)
            unite(rs.front().id, rs.back().id);
    }

    // Vertically, the same rule. Two runs g rows apart that overlap in x mean a
    // column where both ends are water and at most g-1 rows of land lie
    // between, so walking g from 1 to STRAIT+1 closes every vertical gap up to
    // STRAIT -- and g == 1 is ordinary 4-connectivity. The +/-1 slack on the
    // overlap test makes the adjacency 8-connected, matching what the cell
    // flood fill did before.
    for (int y = 0; y + 1 < H; ++y) {
        const auto& a = rows[(size_t)y];
        if (a.empty()) continue;
        for (int g = 1; g <= STRAIT + 1 && y + g < H; ++g) {
            const auto& b = rows[(size_t)(y + g)];
            if (b.empty()) continue;
            size_t i = 0, j = 0;
            while (i < a.size() && j < b.size()) {
                // Slack only for directly adjacent rows: a diagonal touch is
                // contact, a diagonal near-miss eight rows away is not.
                const int slack = (g == 1) ? 1 : 0;
                if (a[i].x1 + slack > b[j].x0 && b[j].x1 + slack > a[i].x0)
                    unite(a[i].id, b[j].id);
                if (a[i].x1 < b[j].x1) ++i; else ++j;
            }
            if (g == 1) {   // the wrap seam, between neighbouring rows
                if (a.front().x0 == 0 && b.back().x1 == W)  unite(a.front().id, b.back().id);
                if (b.front().x0 == 0 && a.back().x1 == W)  unite(b.front().id, a.back().id);
            }
        }
    }

    // A cell counts as navigable if it holds ANY water, and remembers a real
    // water pixel inside it -- storing the cell's geometric centre instead
    // would very often name a beach.
    //
    // THE PIXEL IS CHOSEN FROM THE LARGEST BODY IN THE CELL, not simply the one
    // nearest the middle. A coastal cell frequently holds both the sea and a
    // pond behind the dunes; letting the pond win would hand the cell the
    // pond's component and punch a hole in the coastal route for no reason
    // anyone could see on the map.
    std::unordered_map<int, long long> bodySize;
    for (int y = 0; y < H; ++y)
        for (const Run& r : rows[(size_t)y]) bodySize[find(r.id)] += r.x1 - r.x0;

    std::unordered_map<int, int> denseLabel;
    for (int cy = 0; cy < m_nav.h; ++cy) {
        for (int cx = 0; cx < m_nav.w; ++cx) {
            const int x0 = cx * CELL, y0 = cy * CELL;
            const int x1 = std::min(x0 + CELL, W), y1 = std::min(y0 + CELL, H);
            const double mx = (x0 + x1) * 0.5, my = (y0 + y1) * 0.5;
            long long bestBody = -1;
            double bestD = 1e18;
            int bx = -1, by = -1, broot = -1;
            for (int y = y0; y < y1; ++y) {
                for (const Run& r : rows[(size_t)y]) {
                    if (r.x1 <= x0) continue;
                    if (r.x0 >= x1) break;
                    const int root = find(r.id);
                    const long long size = bodySize[root];
                    for (int x = std::max(r.x0, x0); x < std::min(r.x1, x1); ++x) {
                        const double d = (x - mx) * (x - mx) + (y - my) * (y - my);
                        if (size > bestBody || (size == bestBody && d < bestD)) {
                            bestBody = size; bestD = d; bx = x; by = y; broot = root;
                        }
                    }
                }
            }
            if (bx >= 0) {
                const size_t i = (size_t)cy * m_nav.w + cx;
                m_nav.navigable[i] = 1;
                m_nav.px[i] = bx; m_nav.py[i] = by;
                auto ins = denseLabel.emplace(broot, (int)denseLabel.size());
                m_nav.component[i] = ins.first->second;
            }
        }
    }

    // ── WHICH NEIGHBOURS CAN ACTUALLY BE SAILED TO ──
    //
    // Everything above decides which cells hold water and which sea each one
    // belongs to. Neither answers the question a route actually asks: can a
    // hull get from THIS cell's water to THAT one's? A cell is navigable if it
    // holds ANY water, and its remembered pixel may sit anywhere inside it, so
    // two cells straddling a peninsula each hold real Black Sea water, sit in
    // one body, touch on the grid -- and the leg between them runs overland.
    // That is the Crimea crossing, and no test in this function saw it.
    //
    // So each edge is now walked. Land is tolerated up to STRAIT, the SAME
    // width the component pass calls a strait: it has to be, or this would
    // slam shut the Bosphorus and the Dardanelles that the pass above went to
    // such trouble to open. A neck wider than that is a coast, and a coast is
    // not an edge. Crimea's waist is about 40 px; Perekop, the isthmus, is 1.
    // HOW MUCH LAND ONE LEG MAY CROSS -- and why it is not STRAIT.
    //
    // STRAIT is measured across a run, straight along a row or column. A leg
    // hits the same neck at whatever angle the two cell pixels happen to make,
    // so the very same Bosphorus that is 8 px wide on the raster is 15 px of
    // land along the diagonal a route wants to sail. Reusing STRAIT here
    // therefore SEALED THE BLACK SEA: measured, Odessa and the Sea of Azov
    // dropped into their own body that no hull on Earth could enter.
    //
    // Swept 8/12/16/20/24/28/32 on data/STDmaps/map.odmap. 16 is the first
    // value that puts the Black Sea and the Sea of Azov back in the world
    // ocean, and it still leaves the Caspian and the Great Lakes in bodies of
    // their own. Above it nothing further reconnects that should -- the seas
    // that keep merging from 20 upward are the ones meant to stay apart -- so
    // 16 is the bottom of the correct plateau rather than a value that merely
    // works.
    //
    // What it buys, on the same map, worst overland run along a real route:
    //     Odessa->Kerch      53 px -> 11    (it went ACROSS CRIMEA)
    //     Brest->Kiel        49 px -> 11    (across Jutland)
    //     Gibraltar->Suez    46 px -> 16
    //     Naples->Venice     37 px -> 14    (across Italy)
    //     Athens->Odessa     36 px -> 15
    const int LEGNECK = getenv("OD_LEGNECK") ? atoi(getenv("OD_LEGNECK")) : 16;
    m_nav.link.assign(n, 0);
    auto sailable = [&](int ax, int ay, int bx, int by) {
        int dx = bx - ax;
        if (dx > W / 2) dx -= W; else if (dx < -W / 2) dx += W;   // the seam
        const int dy = by - ay;
        const int steps = std::max(std::abs(dx), std::abs(dy));
        if (steps <= 0) return true;
        int run = 0;
        for (int k = 1; k < steps; ++k) {
            int x = ax + (int)std::lround((double)dx * k / steps);
            int y = ay + (int)std::lround((double)dy * k / steps);
            if (x < 0) x += W; else if (x >= W) x -= W;
            if (y < 0 || y >= H) continue;
            if (m_landSea.isLand(x, y)) { if (++run > LEGNECK) return false; }
            else run = 0;
        }
        return true;
    };
    for (int cy = 0; cy < m_nav.h; ++cy)
        for (int cx = 0; cx < m_nav.w; ++cx) {
            const size_t i = (size_t)cy * m_nav.w + cx;
            if (!m_nav.navigable[i]) continue;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    int nx = cx + dx, ny = cy + dy;
                    if (ny < 0 || ny >= m_nav.h) continue;
                    if (nx < 0) nx += m_nav.w; else if (nx >= m_nav.w) nx -= m_nav.w;
                    const size_t ni = (size_t)ny * m_nav.w + nx;
                    if (!m_nav.navigable[ni]) continue;
                    if (sailable(m_nav.px[i], m_nav.py[i], m_nav.px[ni], m_nav.py[ni]))
                        m_nav.link[i] |= (uint16_t)(1u << ((dy + 1) * 3 + (dx + 1)));
                }
        }

    // ── AND THE SEAS ARE RELABELLED OVER THOSE EDGES ──
    //
    // component now has to mean the same thing the router means, or the two
    // disagree in the one direction that hurts: navReachable says yes, the AI
    // sends a fleet, navRoute cannot find a path, and the order sticks. The
    // raster bodies above answer "is this the same water"; a route needs "can
    // this hull get there", which is the same flood fill run over the edges
    // that were just measured.
    std::vector<int32_t> sea((size_t)n, -1);
    int seas = 0;
    std::vector<int> stack;
    for (size_t s0 = 0; s0 < n; ++s0) {
        if (!m_nav.navigable[s0] || sea[s0] >= 0) continue;
        sea[s0] = seas;
        stack.assign(1, (int)s0);
        while (!stack.empty()) {
            const int cur = stack.back(); stack.pop_back();
            const int cx = cur % m_nav.w, cy = cur / m_nav.w;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((!dx && !dy) || !m_nav.linked((size_t)cur, dx, dy)) continue;
                    int nx = cx + dx, ny = cy + dy;
                    if (ny < 0 || ny >= m_nav.h) continue;
                    if (nx < 0) nx += m_nav.w; else if (nx >= m_nav.w) nx -= m_nav.w;
                    const size_t ni = (size_t)ny * m_nav.w + nx;
                    if (sea[ni] >= 0) continue;
                    sea[ni] = seas;
                    stack.push_back((int)ni);
                }
        }
        ++seas;
    }
    // ── AND THE RELABELLING IS NOT APPLIED. MEASURED, NOT REASONED. ──
    //
    // The argument for swapping it in was good: component decides navReachable,
    // so if it says yes where the router says no, the AI sails at a target it
    // can never arrive at and the order sticks. That is a real failure mode --
    // it is the one that produced 71 stuck orders once before.
    //
    // It just is not this one. With honest edges the router's failure rate is
    // 0.0% of 747 voyages, so there was nothing for the stricter label to
    // protect against, and being stricter cost real play. One procedural map,
    // 60 turns, seed 4242, the three arms:
    //
    //     loose edges + raster seas   54 embarks, 20 landings, 37% arrive
    //     honest edges + raster seas  78 embarks, 45 landings, 58% arrive
    //     honest edges + link seas    56 embarks, 24 landings, 43% arrive
    //
    // The middle row is the fix and the bottom row is the fix plus this, and
    // this takes half the landings back. It over-fragments: 198 seas against
    // the raster's 79, most of them pockets a noisy procedural coastline makes
    // and no hull cares about, and every pocket is a port the AI stops even
    // TRYING to reach. Kept behind a flag because the reasoning still holds if
    // the failure rate ever stops being zero.
    if (getenv("OD_NAVSEAS") && std::string(getenv("OD_NAVSEAS")) == "links")
        m_nav.component.swap(sea);

    const int label = (int)denseLabel.size();
    long long edges = 0, cells = 0;
    for (size_t i = 0; i < n; ++i)
        if (m_nav.navigable[i]) { ++cells; edges += std::popcount(m_nav.link[i]); }
    printf("  Sea routing: %dx%d cells, %d raster body(ies), %lld navigable cells, "
           "%lld sailable edges, %d reachable sea(s)\n",
           m_nav.w, m_nav.h, label, cells, edges, seas);

    if (getenv("OD_NAV_AUDIT")) {
        std::vector<long long> size((size_t)seas, 0);
        for (size_t i = 0; i < n; ++i)
            if (m_nav.component[i] >= 0) ++size[(size_t)m_nav.component[i]];
        std::vector<int> ord((size_t)seas);
        for (int i = 0; i < seas; ++i) ord[(size_t)i] = i;
        std::sort(ord.begin(), ord.end(),
                  [&](int a, int b) { return size[(size_t)a] > size[(size_t)b]; });
        printf("  [NAV] largest seas:");
        for (int k = 0; k < std::min(6, seas); ++k)
            printf(" %lld", size[(size_t)ord[(size_t)k]]);
        long long solo = 0;
        for (int i = 0; i < seas; ++i) if (size[(size_t)i] <= 2) ++solo;
        printf("   (%lld seas of <=2 cells)\n", solo);
        struct Spot { const char* name; double lon, lat; };
        const Spot spots[] = {
            {"Atlantic", -30, 40}, {"Pacific", -150, 0}, {"Indian", 75, -10},
            {"Med", 15, 35}, {"BlackSea", 34, 44}, {"Azov", 36.5, 46},
            {"Baltic", 19, 57}, {"NorthSea", 3, 56}, {"RedSea", 38, 20},
            {"Caspian", 51, 42}, {"GreatLakes", -87, 44},
        };
        // THE TEST THAT MATTERS: not "is it the same sea" -- both shores of a
        // peninsula are -- but "what does the route DRAWN AND SAILED actually
        // cross". Worst overland run along the polyline, in raster pixels.
        struct Voy { const char* name; double alon, alat, blon, blat; };
        const Voy voys[] = {
            {"Odessa->Kerch",     30.7, 46.4,  36.6, 45.3},
            {"Sevastopol->Azov",  33.4, 44.6,  38.3, 46.8},
            {"Athens->Odessa",    23.6, 37.4,  30.7, 46.4},
            {"Gibraltar->Suez",   -5.6, 35.9,  32.6, 29.9},
            {"Brest->Kiel",       -4.8, 48.3,  10.2, 54.4},
            {"Naples->Venice",    14.2, 40.6,  12.8, 45.0},
        };
        printf("  [NAV] worst overland run along a real route (px):\n");
        for (const Voy& v : voys) {
            std::vector<std::pair<double, double>> way;
            const bool ok = navRoute(v.alon, v.alat, v.blon, v.blat, way);
            int worst = 0;
            if (ok) {
                double plon = v.alon, plat = v.alat;
                for (auto& wp : way) {
                    int ax, ay, bx, by;
                    m_landSea.lonLatToPixel((float)plon, (float)plat, ax, ay);
                    m_landSea.lonLatToPixel((float)wp.first, (float)wp.second, bx, by);
                    int dx = bx - ax;
                    if (dx > W / 2) dx -= W; else if (dx < -W / 2) dx += W;
                    const int dy = by - ay;
                    const int steps = std::max(std::abs(dx), std::abs(dy));
                    int run = 0;
                    for (int k = 1; k < steps; ++k) {
                        int x = ax + (int)std::lround((double)dx * k / steps);
                        int y = ay + (int)std::lround((double)dy * k / steps);
                        if (x < 0) x += W; else if (x >= W) x -= W;
                        if (y < 0 || y >= H) continue;
                        if (m_landSea.isLand(x, y)) worst = std::max(worst, ++run);
                        else run = 0;
                    }
                    plon = wp.first; plat = wp.second;
                }
            }
            printf("        %-18s %s  legs=%zu  worst-land=%d px\n", v.name,
                   ok ? "routed" : "NO ROUTE", way.size(), worst);
        }
        printf("  [NAV] sea id at:");
        for (const Spot& sp : spots) {
            int px, py; m_landSea.lonLatToPixel((float)sp.lon, (float)sp.lat, px, py);
            const int c = navCellNear(m_nav, px, py);
            printf(" %s=%d", sp.name, c >= 0 ? m_nav.component[(size_t)c] : -1);
        }
        printf("\n");
    }
}

// Nearest navigable cell to a lon/lat, searching outward. A ship sitting in a
// cell the grid calls land (it holds no water at this resolution) still has to
// be able to plan.
int Game::navCellNear(const NavGrid& g, int px, int py) {
    if (!g.ready()) return -1;
    const int cx = std::clamp(px / g.cell, 0, g.w - 1);
    const int cy = std::clamp(py / g.cell, 0, g.h - 1);
    for (int r = 0; r < 8; ++r) {
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (r > 0 && std::abs(dx) != r && std::abs(dy) != r) continue;
                int nx = cx + dx, ny = cy + dy;
                if (ny < 0 || ny >= g.h) continue;
                if (nx < 0) nx += g.w; else if (nx >= g.w) nx -= g.w;
                const size_t i = (size_t)ny * g.w + nx;
                if (g.navigable[i]) return (int)i;
            }
    }
    return -1;
}

bool Game::portApproach(int provinceId, double& lon, double& lat) const {
    if (!m_nav.ready()) return false;
    auto cIt = m_provinceCenters.find(provinceId);
    if (cIt == m_provinceCenters.end()) return false;
    const int mapW = m_provinces.getWidth(), mapH = m_provinces.getHeight();
    if (mapW <= 0 || mapH <= 0) return false;
    // The province centre is in PROVINCE-raster pixels; the nav grid indexes
    // the LAND/SEA raster. They are the same size on every shipped map, but
    // going through lon/lat rather than assuming that is what keeps a mod with
    // mismatched rasters from aiming its navies into the void.
    const double cLon = cIt->second.x / mapW * 360.0 - 180.0;
    const double cLat = 90.0 - cIt->second.y / mapH * 180.0;
    int px, py;
    m_landSea.lonLatToPixel((float)cLon, (float)cLat, px, py);
    const int cell = navCellNear(m_nav, px, py);
    if (cell < 0) return false;
    float wLon, wLat;
    m_landSea.pixelToLonLat(m_nav.px[cell], m_nav.py[cell], wLon, wLat);
    lon = wLon; lat = wLat;
    return true;
}

int Game::seaBodyOfPort(int provinceId) const {
    if (!m_nav.ready()) return -1;
    double lon = 0.0, lat = 0.0;
    if (!portApproach(provinceId, lon, lat)) return -1;
    int px = 0, py = 0;
    m_landSea.lonLatToPixel((float)lon, (float)lat, px, py);
    const int cell = navCellNear(m_nav, px, py);
    return cell >= 0 ? m_nav.component[cell] : -1;
}

bool Game::navReachable(double lon1, double lat1, double lon2, double lat2) const {
    if (!m_nav.ready()) return true;   // no grid: do not block anything
    int x1, y1, x2, y2;
    m_landSea.lonLatToPixel((float)lon1, (float)lat1, x1, y1);
    m_landSea.lonLatToPixel((float)lon2, (float)lat2, x2, y2);
    const int a = navCellNear(m_nav, x1, y1), b = navCellNear(m_nav, x2, y2);
    if (a < 0 || b < 0) return false;
    return m_nav.component[a] == m_nav.component[b];
}

bool Game::navLineClear(double lon1, double lat1, double lon2, double lat2) const {
    // Wrapped, or a segment crossing the antimeridian is sampled right around
    // the far side of the world and reports land that is nowhere near it.
    const double dLon = Game::lonDelta(lon1, lon2), dLat = lat2 - lat1;
    const double dist = std::sqrt(dLon * dLon + dLat * dLat);
    // DELIBERATELY TOLERANT: about one sample per third of a routing cell.
    //
    // Sampling per raster pixel was tried and is WORSE. This test only decides
    // how far ahead along the route to aim; processNavyMovement then walks the
    // segment and stops at the last water it finds. So an optimistic long hop
    // that gets partially blocked still delivers more progress than a cautious
    // short one, and being strict here just makes fleets crawl between adjacent
    // cells. Measured over two 300-turn scenario runs, share of embarkations
    // reaching a hostile shore: no check 39%, tolerant check 59%, per-pixel
    // check 43%.
    const double degPerCell = 360.0 * (double)m_nav.cell /
                              std::max(1.0, (double)m_landSea.getWidth());
    const int steps = std::clamp((int)std::ceil(dist / std::max(1e-6, degPerCell / 3.0)), 2, 512);
    for (int i = 0; i <= steps; ++i) {
        const double t = (double)i / (double)steps;
        if (m_landSea.isLand((float)Game::wrapLon(lon1 + dLon * t), (float)(lat1 + dLat * t)))
            return false;
    }
    return true;
}

// Counted so a change to the sea graph can be judged on whether it strands
// hulls, not on whether the routes it does produce look better. A stricter
// graph that quietly makes 20% of voyages unroutable is a worse game than a
// loose one that sends them over Crimea.
bool Game::navRoute(double fromLon, double fromLat, double toLon, double toLat,
                    std::vector<std::pair<double, double>>& out) const {
    ++g_navRouteCalls;
    out.clear();
    if (!m_nav.ready()) { ++g_navRouteFails; return false; }
    int x1, y1, x2, y2;
    m_landSea.lonLatToPixel((float)fromLon, (float)fromLat, x1, y1);
    m_landSea.lonLatToPixel((float)toLon, (float)toLat, x2, y2);
    const int start = navCellNear(m_nav, x1, y1), goal = navCellNear(m_nav, x2, y2);
    if (start < 0 || goal < 0) { ++g_navRouteFails; return false; }
    if (m_nav.component[start] != m_nav.component[goal]) { ++g_navRouteFails; return false; }
    if (start == goal) return true;   // already there; no waypoints needed

    // Plain BFS. The grid is small and every hop costs the same, so the extra
    // machinery of A* would buy nothing measurable here.
    //
    // CONFINED TO ONE BODY OF WATER. The components come from the raster now
    // (see buildNavGrid), but two cells that touch on the grid can belong to
    // different seas -- a coastal cell and the cell holding the lake behind it
    // -- and without this test the BFS would walk straight from one into the
    // other and hand back a "route" over the beach between them. The
    // component check at the top of this function only screens the endpoints;
    // this is what keeps every hop in between honest.
    const int body = m_nav.component[start];
    const size_t n = m_nav.navigable.size();
    std::vector<int32_t> prev(n, -2);
    std::deque<int> q;
    prev[start] = -1;
    q.push_back(start);
    bool found = false;
    while (!q.empty() && !found) {
        const int cur = q.front(); q.pop_front();
        const int cx = cur % m_nav.w, cy = cur / m_nav.w;
        for (int dy = -1; dy <= 1 && !found; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                // THE EDGE, NOT THE ADJACENCY. Two cells touching on the grid
                // is not a leg a hull can sail; see buildNavGrid.
                if (!m_nav.linked((size_t)cur, dx, dy)) continue;
                int nx = cx + dx, ny = cy + dy;
                if (ny < 0 || ny >= m_nav.h) continue;
                if (nx < 0) nx += m_nav.w; else if (nx >= m_nav.w) nx -= m_nav.w;
                const size_t ni = (size_t)ny * m_nav.w + nx;
                if (!m_nav.navigable[ni] || prev[ni] != -2) continue;
                if (m_nav.component[ni] != body) continue;
                prev[ni] = cur;
                if ((int)ni == goal) { found = true; break; }
                q.push_back((int)ni);
            }
    }
    if (!found) { ++g_navRouteFails; return false; }

    std::vector<int> rev;
    for (int c = goal; c >= 0; c = prev[c]) rev.push_back(c);
    out.reserve(rev.size());
    for (auto it = rev.rbegin(); it != rev.rend(); ++it) {
        if (*it == start) continue;
        float lon, lat;
        m_landSea.pixelToLonLat(m_nav.px[*it], m_nav.py[*it], lon, lat);
        out.emplace_back((double)lon, (double)lat);
    }
    return !out.empty();
}

// === nudgeShipToWater ===
//
// Move a hull sitting on land to the nearest navigable pixel, searching
// outward in rings. Returns false and leaves the ship alone if no water turns
// up inside the cap -- an inland sea barely wider than the ship, or a map whose
// raster disagrees with its own ship placement, is better left visibly wrong
// than teleported across a continent.
bool Game::nudgeShipToWater(NavyShip& s) {
    const int w = m_landSea.getWidth(), h = m_landSea.getHeight();
    if (w <= 0 || h <= 0) return false;
    int px, py;
    m_landSea.lonLatToPixel((float)s.lon, (float)s.lat, px, py);
    // 64 pixels is about as far as a coastal misplacement ever is; beyond that
    // the ship was not "just inland", and guessing gets worse, not better.
    const int MAX_R = 64;
    for (int r = 1; r <= MAX_R; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                // ring only -- the interior was covered by a smaller r
                if (std::abs(dx) != r && std::abs(dy) != r) continue;
                int nx = px + dx, ny = py + dy;
                if (nx < 0) nx += w; else if (nx >= w) nx -= w;  // world wraps in x
                if (ny < 0 || ny >= h) continue;
                if (m_landSea.isLand(nx, ny)) continue;
                float lon, lat;
                m_landSea.pixelToLonLat(nx, ny, lon, lat);
                s.lon = lon; s.lat = lat;
                return true;
            }
        }
    }
    return false;
}

// === processUpgrades ===
void Game::processUpgrades() {
    // Process pending building upgrades
    for (auto it = m_pendingUpgrades.begin(); it != m_pendingUpgrades.end(); ) {
        it->turnsRemaining--;
        if (it->turnsRemaining <= 0) {
            // INDEX, DO NOT FIND. These three maps only carry entries for what
            // the map file shipped, so a province with no industry and no port
            // is absent from them entirely -- which is exactly the province a
            // FIRST factory or a NEW port gets built in. Looking the entry up
            // and skipping when it was missing quietly threw away every such
            // build after the money had already been taken and the turns had
            // already been waited out, for the AI and the player alike.
            if (it->type == "industry") {
                auto& ind = m_provinceIndustry[it->provinceId];
                // RE-CHECKED AT APPLY TIME, NOT ONLY WHEN ORDERED.
                //
                // A build takes up to ten turns, and a province can lose the
                // population that justified it in between -- conquest, a
                // rebellion, recruitment draining it, a minority policy moving
                // people out. The gate that let this order be placed answered a
                // question about a province that no longer exists, so it is
                // asked again here, against the province as it is now.
                //
                // A build that no longer fits is CAPPED, not cancelled: the
                // money and the turns are already spent, and destroying both
                // for something the player could not have foreseen reads as the
                // game cheating. They get the largest factory the province can
                // now carry, which is at worst the level it already had.
                const int cap = provinceIndustryCapacity(it->provinceId);
                ind.level = std::max(ind.level, std::min(it->targetLevel, cap));
                ind.income = provinceIndustryIncome(it->provinceId, ind.level);
            } else if (it->type == "fortification") {
                m_provinceIndustry[it->provinceId].fortification = it->targetLevel;
            } else if (it->type == "port") {
                m_provincePorts[it->provinceId].level = it->targetLevel;
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
    // ONE READER. This was a second, inline copy of the research rule --
    // Game::addResearchPoints held the other, and being uncalled is the only
    // reason the two never disagreed. Groups would have had to be implemented
    // in both, and the dead one would have been the copy that quietly won the
    // day somebody called it.
    if (m_playerCountryId > 0) addResearchPoints(m_playerCountryId);
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
    const float baseMigrationRate = 0.005f; // 0.5% base migration per turn

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
            income = indIt->second.income + provinceResourceIncome(pid) + indIt->second.popIncome;
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
        // This country's own migration research, not the player's. The bonus
        // was hoisted out of the loop and read the global tree, so a player who
        // researched better internal migration sped it up for every country on
        // the map at once.
        const float migrationRate =
            baseMigrationRate * (1.0f + getTotalEffect("migrationRate", cid));
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
                float align = getMinorityAlignment(cid, mg.name);
                float alignPush = (100.0f - align) / 50.0f; // 0 at 100% align, 2.0 at 0% align
                // Higher unrest + lower alignment = more emigration
                float surge = refugeeSurge.count(srcPid) ? refugeeSurge[srcPid] : 1.0f;
                // Behind aiDebug: this fires per province per minority per
                // turn. On a self-play run it was most of a gigabyte an hour of
                // stdout, and formatting it is not free either.
                if (surge > 1.0f && m_config.aiDebug)
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
                // ── AND WHAT THIS COUNTRY PUBLISHES ABOUT ITSELF ──
                //
                // A country that opens its books and has good figures in them
                // is somewhere people choose to go. Applied to the DESTINATION
                // and read from the destination's owner, so publishing pulls
                // people in rather than pushing its own outward. Zero for every
                // country that publishes nothing, which is all of them until
                // somebody ticks a box.
                if ((size_t)dstPid < m_provinceCountryLookup.size()) {
                    const int dstCid = m_provinceCountryLookup[dstPid];
                    if (dstCid > 0) dstAttr *= (1.0f + disclosureAppeal(dstCid));
                }
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
                score += (simRand() % 100) * 0.01f;
                if (score > bestScore) { bestScore = score; bestDst = dstPid; }
            }
            if (bestDst < 0 || bestScore <= 0) continue;

            // Execute migration: move people from source to destination
            long long srcPop = m_provincePopulations.count(mg.sourcePid) ? m_provincePopulations[mg.sourcePid] : 0;
            long long dstPop = m_provincePopulations.count(bestDst) ? m_provincePopulations[bestDst] : 0;
            float wcSurge = refugeeSurge.count(mg.sourcePid) ? refugeeSurge[mg.sourcePid] : 1.0f;
            long long moveCap = (wcSurge > 1.0f) ? srcPop / 2 : srcPop / 10; // 50% cap for refugees, 10% normally
            long long moveCount = std::min(mg.totalPop, moveCap);
            // Don't let a destination be migrated past the province ceiling. Capping
            // the move (rather than clamping the result) keeps migration zero-sum and
            // keeps the minority-percentage recalculation below consistent.
            moveCount = std::min(moveCount, std::max(0LL, MAX_PROVINCE_POP - dstPop));
            {   // ...and never more of a people than the source actually holds:
                // the cap above is against the PROVINCE, and moving more than a
                // group has drove its share negative. See src/MinorityShares.h.
                auto sIt = m_provinceMinorities.find(mg.sourcePid);
                moveCount = std::min(moveCount, sIt == m_provinceMinorities.end()
                    ? 0LL : od::groupPopAt(sIt->second, srcPop, mg.name));
            }
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
                od::renormaliseShares(m_provinceMinorities[bestDst]);
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
                    od::renormaliseShares(sMit->second);
                    if (sMit->second.empty()) m_provinceMinorities.erase(mg.sourcePid);
                }
            }
        }
    }

    // Phase 2b: Cross-border ethnic migration
    // Minorities migrate between countries at a reduced rate,
    // driven by unrest at source and economic opportunity at destination
    for (auto& [cid, pids] : countryProvinces) {
        if (cid == SPC_CID) continue;
        // Rates from the SOURCE country's own research, for the same reason as
        // the within-country phase above.
        const float migrationBonus = getTotalEffect("migrationRate", cid);
        const float crossBorderRate = baseMigrationRate * 0.2f * (1.0f + migrationBonus);
        // High rate for refugees fleeing conquered provinces.
        const float crossBorderRefugeeRate = baseMigrationRate * 1.5f * (1.0f + migrationBonus);

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
                float cbAlign = getMinorityAlignment(cid, mg.name);
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
                                  + (simRand() % 100) * 0.01f;

                    if (score > bestScore) { bestScore = score; bestDst = dstPid; bestDstCid = cid2; }
                }

                if (bestDst < 0 || bestScore <= 0) continue;

                long long dstPop = m_provincePopulations.count(bestDst) ? m_provincePopulations[bestDst] : 0;
                // Refugee surge: fleeing conquered/war-torn provinces at much higher rate
                float cbSurge = refugeeSurge.count(srcPid) ? refugeeSurge[srcPid] : 1.0f;
                if (cbSurge > 1.0f && cbAlign < 40.0f && m_config.aiDebug)
                    printf("[DIAG] Cross-border refugee surge %.1fx in province %d for %s\n",
                           cbSurge, srcPid, mg.name.c_str());
                float cbRate = (cbSurge > 1.0f && cbAlign < 40.0f) ? crossBorderRefugeeRate : crossBorderRate;
                long long moveCount = std::max(1LL, (long long)(minorityPop * cbRate));
                long long cbMoveCap = cbSurge > 1.0f ? srcPop / 2 : srcPop / 10;
                moveCount = std::min(moveCount, cbMoveCap);
                // Keep the destination under the province ceiling (see within-country
                // migration above — cap the move, don't clamp the result).
                moveCount = std::min(moveCount, std::max(0LL, MAX_PROVINCE_POP - dstPop));
                moveCount = std::min(moveCount, od::groupPopAt(srcGroups, srcPop, mg.name));
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
                    od::renormaliseShares(m_provinceMinorities[bestDst]);
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
                        od::renormaliseShares(sMit->second);
                        if (sMit->second.empty()) m_provinceMinorities.erase(srcPid);
                    }
                }
            }
        }
    }

    // ── Phase 2c: assimilation ──
    //
    // WHAT WAS HERE BEFORE: nothing. Three research nodes -- Cultural
    // Programs, Educational Reform, National Identity -- set
    // indoctrinationPct and advertised "Minority alignment +5/10/20%/turn",
    // and NOTHING EVER READ THE FIELD. getResearchEffect("indoctrinationPct")
    // had no caller, so 10, 20 and 30 research bought three tooltips and no
    // effect. It is the same fault Game_Policies.cpp records for unrest
    // reduction: "the tooltip promised a reduction that never happened".
    //
    // Now it converts people. Share moves from every other group toward the
    // country's OWN culture -- the titular group, computed per country rather
    // than per province, because a state teaches its own culture and not
    // whatever happens to be locally dominant. A Tajik-majority province of a
    // Pashtun state assimilates toward Pashtun, which is the whole point.
    //
    // SCALED BY ALIGNMENT, so it is not a free erase button: a minority that
    // has been repressed into the ground does not adopt the culture of the
    // state repressing it. Conciliation and indoctrination have to be bought
    // together, and a country that only indoctrinates converts almost nobody.
    assimilateMinorities();

    // Phase 3: compute stored unrest (for display)
    for (auto& [pid, p] : m_provinces.getAllProvinces()) {
        if (p.countryId <= 0 || p.countryId == UNC_CID || p.countryId == BLC_CID) continue;
        float unrest = provinceUnrest[pid];
        // Minority-based unrest
        float minorityUnrest = 0;
        auto mit = m_provinceMinorities.find(pid);
        if (mit != m_provinceMinorities.end()) {
            for (auto& mg : mit->second) {
                float align = getMinorityAlignment(p.countryId, mg.name);
                if (align < 40.0f)
                    minorityUnrest += mg.pct * 0.3f * (1.0f - align / 40.0f);
            }
        }
        unrest += minorityUnrest;
    }
}


// === recordTurnOrders ===
//
// See m_turnOrderLog in Game.h for why the middle-state view is a record of the
// turn that resolved rather than an overlay of orders not yet given.
// === flushMapRepaint ===
//
// ONE PLACE DECIDES WHEN THE LAND CHANGES. Called every frame; does nothing
// almost every time. While the orders are being read it deliberately does
// nothing at all, which is what keeps the borders showing the world the orders
// were given in.
//
// ── AND IT PUTS SOMETHING ON SCREEN WHILE IT WORKS ──
//
// Regenerating the political map is a pass over 8192x4096 pixels and a BFS for
// the border gradient; it takes seconds. It used to run inside processTurn,
// under the loading screen, behind "Generating political map...". Deferring it
// to the draw loop moved it somewhere nothing is drawn at all -- reported as
// "a few seconds of black screen, no loading or anything". A long job with a
// progress bar is a game loading; the same job with nothing on screen is a game
// that has crashed.
void Game::flushMapRepaint() {
    if (m_turnState == TURN_VIEWING_ORDERS) return;
    if (!m_politicalRepaintPending && !m_labelRepaintPending) return;
    // Only worth a screen when there is really work to do, and only when
    // somebody is looking: a headless run has no loading screen to show.
    // Called from Game::draw BEFORE it opens its frame, so this owns the
    // screen outright and can put a progress bar up the way the turn does.
    const bool wasShowing = m_showLoadingScreen;
    if (!m_aiTraining && !wasShowing && m_renderer && m_shotDir.empty()) {
        showLoadingScreen();
        setLoadingProgress(0.9f, "Generating political map...");
        BeginDrawing();
        ClearBackground(BLACK);
        drawLoadingScreen();
        EndDrawing();
    }
    struct Restore {
        Game* g; bool was;
        ~Restore() { if (!was) g->hideLoadingScreen(); }
    } restore{this, wasShowing};
    if (m_politicalRepaintPending) {
        m_politicalRepaintPending = false;
        if (!m_aiTraining) generatePoliticalTexture();
    }
    if (m_labelRepaintPending) {
        m_labelRepaintPending = false;
        if (!m_aiTraining) {
            computeCountryLabels();
            if (m_renderer) m_renderer->setCountryLabels(&m_countryLabels);
        }
    }
}

void Game::recordTurnOrders(int countryId) {
    // Nothing reads this except the overlay, and training has no overlay. This
    // is the one kind of thing the `if (m_aiTraining)` gate is actually right
    // for -- it has bitten this codebase three times when used to skip
    // something the simulation needed, and this is not that: no rule, no
    // resolver and no save reads m_turnOrderLog.
    if (m_aiTraining) return;

    // A new turn replaces the last one wholesale. The log describes ONE turn;
    // keeping two would draw a country's cancelled plan on top of its real one.
    if (m_turnOrderLogTurn != m_turnNumber) {
        m_turnOrderLog.clear();
        m_turnOrderLogTurn = m_turnNumber;
    }

    for (const auto& ao : m_pendingArtilleryOrders) {
        const Province* src = m_provinces.getProvinceById(ao.fromProvince);
        if (!src || src->countryId != countryId) continue;
        TurnOrderMark m;
        m.kind = TurnOrderMark::Kind::Artillery;
        m.countryId = countryId;
        m.fromProvince = ao.fromProvince;
        m.toProvince = ao.targetProvince;
        m.detail = ao.ammoType;
        m_turnOrderLog.push_back(std::move(m));
    }

    for (const auto& mo : m_pendingMoveOrders) {
        if (mo.countryId != countryId) continue;
        TurnOrderMark m;
        m.kind = TurnOrderMark::Kind::ArmyMove;
        m.countryId = countryId;
        m.fromProvince = mo.fromProvince;
        m.toProvince = mo.toProvince;
        m.detail = std::to_string(mo.pct) + "%";
        m_turnOrderLog.push_back(std::move(m));
    }

    // ── WHAT IT IS RAISING AND WHAT IT IS BUILDING ──
    //
    // Ownership is by PROVINCE, not by a field on the order: these queues carry
    // no country id, and the province's owner is the country that paid. That
    // also makes them self-correcting when a province changes hands mid-queue.
    for (const auto& r : m_pendingRecruitments) {
        const Province* p = m_provinces.getProvinceById(r.provinceId);
        if (!p || p->countryId != countryId || r.count <= 0) continue;
        TurnOrderMark m;
        m.kind = TurnOrderMark::Kind::Recruit;
        m.countryId = countryId;
        m.fromProvince = m.toProvince = r.provinceId;
        m.detail = TextFormat("%s %s", formatBalance((float)r.count).c_str(),
                              T(troopCost(r.type).name));
        m_turnOrderLog.push_back(std::move(m));
    }
    for (const auto& u : m_pendingUpgrades) {
        const Province* p = m_provinces.getProvinceById(u.provinceId);
        if (!p || p->countryId != countryId) continue;
        TurnOrderMark m;
        m.kind = TurnOrderMark::Kind::Build;
        m.countryId = countryId;
        m.fromProvince = m.toProvince = u.provinceId;
        m.detail = T(u.type.c_str());
        m_turnOrderLog.push_back(std::move(m));
    }
    for (const auto& b : m_pendingShipBuilds) {
        const Province* p = m_provinces.getProvinceById(b.provinceId);
        if (!p || p->countryId != countryId) continue;
        TurnOrderMark m;
        m.kind = TurnOrderMark::Kind::Build;
        m.countryId = countryId;
        m.fromProvince = m.toProvince = b.provinceId;
        m.detail = T(b.type.c_str());
        m_turnOrderLog.push_back(std::move(m));
    }

    // ── AND WHAT THE FLEET IS SHELLING ──
    //
    // A naval bombardment is the same shell as a land one, priced by the same
    // call -- and it was the one kind of attack the orders view could not show,
    // so a coast being worked over by a carrier group looked like a quiet turn.
    for (const auto& bo : m_pendingShipBombardOrders) {
        if (bo.shipIndex < 0 || bo.shipIndex >= (int)m_ships.size()) continue;
        const NavyShip& ship = m_ships[bo.shipIndex];
        if (ship.countryId != countryId) continue;
        TurnOrderMark m;
        m.kind = TurnOrderMark::Kind::NavalBombard;
        m.countryId = countryId;
        m.fromLon = ship.lon;  m.fromLat = ship.lat;
        m.fromProvince = -1;
        m.toProvince = bo.targetProvince;
        m.detail = bo.ammoType;
        m_turnOrderLog.push_back(std::move(m));
    }

    for (const auto& so : m_pendingShipMoveOrders) {
        if (so.shipIndex < 0 || so.shipIndex >= (int)m_ships.size()) continue;
        const NavyShip& ship = m_ships[so.shipIndex];
        if (ship.countryId != countryId) continue;
        TurnOrderMark m;
        m.kind = TurnOrderMark::Kind::ShipVoyage;
        m.countryId = countryId;
        // Where the hull IS as the order resolves. By the time this is drawn it
        // has sailed, so looking the position up later would draw the leg it
        // has already finished.
        m.fromLon = ship.lon;  m.fromLat = ship.lat;
        m.destLon = so.destLon; m.destLat = so.destLat;
        m.turnRangeDeg = shipMaxRangeDeg(ship);   // how far it gets THIS turn
        m.toProvince = so.destProvince;
        // ── PLAN IT IF THE RESOLVER HAS NOT YET ──
        //
        // An order is queued with a destination and an EMPTY route;
        // processNavyMovement fills it in, and that runs AFTER this. Measured
        // on a 12-turn eval, 42% of the voyages recorded here (33 of 78 on one
        // turn) had no route at all -- so without this the overlay would fall
        // back to a straight line to the destination for nearly half of them,
        // which is the exact misleading line this whole feature removed for the
        // player's own ships.
        //
        // One BFS per unrouted voyage per turn, tens of them, and never in
        // training -- the early return above sees to that.
        m.route = so.route;
        if (m.route.empty())
            navRoute(ship.lon, ship.lat, so.destLon, so.destLat, m.route);
        if (m.route.empty() ||
            m.route.back().first != so.destLon || m.route.back().second != so.destLat)
            m.route.emplace_back(so.destLon, so.destLat);
        m_turnOrderLog.push_back(std::move(m));
    }
}
