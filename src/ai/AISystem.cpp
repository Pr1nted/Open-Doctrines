#include "AISystem.h"
#include "AIVersion.h"
#include "ModelBlob.h"
#include "MoneyLedger.h"
#include "../OdFile.h"
#include "../Game.h"
#include "../BuildCosts.h"
#include "../GameInternals.h"
#include "../util/WebAssets.h"
#include <algorithm>
#include <chrono>
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
// The build tables used to be duplicated here, under AI_ names, identical in
// value to the player's. Identical values were never the problem: the panel
// multiplied each one by the research cost modifier and this file did not, so
// a country that had finished the industry tree built at half price when a
// player ran it and full price when the AI did -- and the economy module spent
// every training run learning from the expensive version. One table now, and
// buildCostMod() beside it, so the discount cannot be forgotten again.

static const char* MODULE_NAMES[] = {"econ", "politics", "war", "navy"};

// ─── What a country will pay to get its way without a war ───────────────────
//
// A trade offer is money for land, or money for a renounced claim. These are
// the prices, and they are deliberately generous compared to what a war costs:
// the point of the action is that a country with a full treasury and a border
// grievance has an alternative to invading, and an alternative nobody accepts
// is not an alternative.
//
// Never bet the treasury on a deal that can still be refused. A country that
// emptied itself into one offer would be defenceless for the turns it takes to
// be answered.
//
// RAISED FROM 0.40, because the risk it guards against is the PLAYER'S and not
// the AI's: the player's money leaves the treasury the moment the offer is sent
// (Game_UI.cpp) and is refunded on refusal, while an AI sender is charged by
// applyCeasefireTerms only when the deal actually resolves. So for the side
// this constant governs, a pending offer costs nothing until it is accepted,
// and 40% was buying protection from an exposure that does not exist. What is
// left is the real reason to keep some back: a country that has just spent
// everything on land still has to garrison it.
static constexpr double TRADE_MAX_TREASURY_SHARE = 0.70;
// What land costs: a multiple of what that land actually earns.
//
// This was a flat 900 per province, and a flat price is wrong in both
// directions at once. Against BuildCosts.h a carrier is 40 and industry X is
// 300, so 900 bought a swamp for the price of twenty-two carriers -- while an
// industrial core went for the same money. Measured over a 21.5-hour run: 6,537
// trades at a 95% ACCEPT rate, which is what a market looks like when the
// seller is always overpaid. The buyer held more land and its countries died
// faster (survival 90% against 98%), which is what draining a treasury for
// ground you then cannot defend looks like from the outside.
//
// So: price = the province's own income per turn, times a payback period. That
// is how anything income-producing is valued, and it makes the AI's offer track
// the same thing the RECEIVER's deal-value features are reading -- the two sides
// finally arguing about one number rather than two.
static constexpr double TRADE_PAYBACK_TURNS = 24.0;
// Floors and ceilings, because income alone misprices the tails. A province
// earning nothing is still a port, a border, or somebody's homeland; a
// metropolis is not worth a treasury nobody could raise.
static constexpr double TRADE_PRICE_PROV_MIN = 120.0;
static constexpr double TRADE_PRICE_PROV_MAX = 1400.0;
// A claim is a promise rather than territory, and stays flat: what it is worth
// is the war it prevents, which has nothing to do with the ground's income.
static constexpr double TRADE_PRICE_PER_CLAIM = 250.0;
// Below this there is nothing to offer that anyone would take, and the country
// is better off keeping its money.
//
// DERIVED, NOT CHOSEN. This was a flat 400, and it was the single reason
// AI-to-AI trade had never once happened: an AI country's economy spends to
// zero every turn (the same fact that killed a $10 charge at the quayside), so
// a flat treasury floor on an action is not a price, it is a prohibition.
// Measured over 400 turns of the shipped scenarios: the trade action was
// offered on 3.1% of politics decisions and taken zero times, ever -- and with
// no offers there is no gradient, so no amount of training could have taught
// the head anything about it either.
//
// The floor that is actually needed is exactly the one that keeps the mask and
// the executor honest with each other: enough treasury that the cheapest thing
// a trade can buy fits inside the share above. Writing it as arithmetic rather
// than as a number means the two cannot drift apart the next time either is
// tuned, and it cannot silently become a prohibition again.
static constexpr double TRADE_MIN_TREASURY =
    TRADE_PRICE_PROV_MIN / TRADE_MAX_TREASURY_SHARE;
// One province at a time. Buying three at once is how a trade stops reading as
// a border settlement and starts reading as a partition -- and it is far more
// likely to be refused, which costs a turn and a cooldown for nothing.
static constexpr size_t TRADE_MAX_PROVS  = 1;
static constexpr size_t TRADE_MAX_CLAIMS = 2;

// Research is a player-only system, so AI countries would report level-0 caps
// forever and could never build anything. They get a baseline capability
// instead; researched levels still raise the cap when a map grants them.
// The same eligibility and price as nextIndustryBuy, asked about ONE
// province instead of the best one. Used by the war economy: a campaign
// steers the factory to its staging province, and must not be able to
// order an upgrade the ordinary rule would refuse (over the cap, over the
// province's capacity, or already pending).
bool AISystem::industryBuyAt(int cid, int pid, int& outLevel, float& outCost) const {
    Game& g = *m_g;
    if (pid < 0) return false;
    const Province* p = g.m_provinces.getProvinceById(pid);
    if (!p || p->countryId != cid) return false;
    auto ind = g.m_provinceIndustry.find(pid);
    const int lvl = ind != g.m_provinceIndustry.end() ? ind->second.level : 0;
    if (lvl >= industryCap(cid)) return false;
    if (lvl >= g.provinceIndustryCapacity(pid)) return false;
    for (const auto& pu : g.m_pendingUpgrades)
        if (pu.provinceId == pid && pu.type == "industry") return false;
    const int next = lvl + 1;
    if (next > IND_MAX_LEVEL) return false;
    outLevel = next;
    outCost = (float)IND_COST[next] * buildCostMod(g.getTotalEffect("industryCostPct", cid));
    return true;
}

bool AISystem::nextIndustryBuy(int cid, int& outPid, int& outLevel,
                               float& outCost) const {
    Game& g = *m_g;
    const int cap = industryCap(cid);
    const float mod = buildCostMod(g.getTotalEffect("industryCostPct", cid));
    int bestPid = -1; long long bestScore = -1;
    for (int pid : g.provincesOf(cid)) {
        auto ind = g.m_provinceIndustry.find(pid);
        const int lvl = ind != g.m_provinceIndustry.end() ? ind->second.level : 0;
        if (lvl >= cap) continue;
        // AND WHAT THE GROUND WILL CARRY, through the same call the province
        // panel makes. The AI reading its own idea of where a factory may stand
        // is how the build-cost tables diverged -- the panel discounted every
        // price by research and the AI did not, so the player built at half
        // price and the AI paid full, forever, while its economy module learned
        // from that world. One rule, one call site each. See
        // industryCapacity() in BuildCosts.h.
        //
        // `>=` against the level being BUILT, so a grandfathered province -- one
        // already above its own capacity, which a loaded save may legally hold
        // -- is skipped rather than treated as an error or clamped down.
        if (lvl >= g.provinceIndustryCapacity(pid)) continue;
        bool pending = false;
        for (const auto& pu : g.m_pendingUpgrades)
            if (pu.provinceId == pid && pu.type == "industry") { pending = true; break; }
        if (pending) continue;
        auto pop = g.m_provincePopulations.find(pid);
        const long long people = pop != g.m_provincePopulations.end() ? pop->second : 0;
        // Resource-rich provinces pay industry back faster (and enable a later
        // specialisation), so weight population by resources -- the executor's
        // own ranking, kept here because this IS that choice.
        float resBoost = 0.0f;
        auto res = g.m_provinceResources.find(pid);
        if (res != g.m_provinceResources.end())
            resBoost = res->second.oil.amount + res->second.gold.amount +
                       res->second.metal.amount + res->second.rubber.amount +
                       res->second.gemstones.amount;
        const long long score = (long long)(people * (1.0f + resBoost / 100.0f));
        if (score > bestScore) { bestScore = score; bestPid = pid; }
    }
    if (bestPid < 0) return false;
    auto ind = g.m_provinceIndustry.find(bestPid);
    const int next = (ind != g.m_provinceIndustry.end() ? ind->second.level : 0) + 1;
    if (next > IND_MAX_LEVEL) return false;
    outPid = bestPid; outLevel = next;
    outCost = (float)IND_COST[next] * mod;
    return true;
}

bool AISystem::nextSpecBuy(int cid, int& outPid, const char*& outRes,
                           float& outCost) const {
    Game& g = *m_g;
    // `bestBoost` now holds the best RETURN PER GOLD, not a percentage.
    int bestPid = -1; float bestBoost = 0.0f; const char* bestRes = nullptr;
    for (int pid : g.provincesOf(cid)) {
        auto ind = g.m_provinceIndustry.find(pid);
        if (ind == g.m_provinceIndustry.end() || ind->second.level < 1) continue;
        if (!ind->second.specialization.empty()) continue;
        bool pending = false;
        for (const auto& ps : g.m_pendingSpecializations)
            if (ps.provinceId == pid) { pending = true; break; }
        if (pending) continue;
        const char* best = g.bestSpecializationFor(pid);
        if (!best) continue;
        // ── BY WHAT IT PAYS BACK, NOT BY THE PERCENTAGE ──
        //
        // This ranked candidates by the boost PERCENTAGE, and a large
        // percentage of a tiny resource income is worth nothing. Measured
        // across the 1914 map, the payback on specialising ranges from FOUR
        // turns on the best province -- as good as the first level of industry,
        // the best-paying thing in the game -- to a median of 103 and a long
        // tail beyond that. Picking by percentage lands somewhere in the tail
        // most of the time, so the economy head was offered a bad deal, took it
        // 0.3% of the time it was offered, and was right to.
        //
        // Ranked by return per gold instead: the extra income the boost
        // actually produces, over what the specialisation costs. Same action,
        // same executor, same price -- a better candidate.
        //
        // AND IT IS INERT ON THE CURRENT MODEL, which is the honest half. The
        // economy head's probability for `specialize` is 0.0 and stays there:
        // a better candidate does not make a collapsed head take an action, the
        // same wall the port note above records. On the seat bench four of six
        // seats moved by EXACTLY zero. The rating rose 124 -> 141, and all of
        // that is modern China, a seat whose result across five seeds runs
        // 0.0, 1.9, 6.4, 5.1, 0.2 -- from annihilated to two and a half times
        // par. A change that is never chosen cannot systematically improve
        // play; it perturbed one bimodal world onto the good side of its edge.
        // Do not cite that 17 points as the value of this fix.
        //
        // Kept anyway, on the same grounds as the embark-port fix: offering a
        // four-turn payback instead of a hundred-turn one is right whether or
        // not today's policy notices, it costs nothing, and it is what a
        // retrained head would need in order to find the action worth taking.
        auto res = g.m_provinceResources.find(pid);
        if (res == g.m_provinceResources.end()) continue;
        const std::string b = best;
        const float boost = (b == "Oil")    ? res->second.oil.boost
                          : (b == "Gold")   ? res->second.gold.boost
                          : (b == "Metal")  ? res->second.metal.boost
                          : (b == "Rubber") ? res->second.rubber.boost
                                            : res->second.gemstones.boost;
        if (boost <= 0.0f) continue;
        const float gain = ind->second.resourceIncome * (boost / 100.0f);
        const float price = (float)IND_COST[std::clamp(ind->second.level, 0, IND_MAX_LEVEL)] *
                            SPECIALIZE_COST_MULT;
        if (gain <= 0.0f || price <= 0.0f) continue;
        const float perGold = gain / price;   // higher is a shorter payback
        if (perGold > bestBoost) { bestBoost = perGold; bestPid = pid; bestRes = best; }
    }
    if (bestPid < 0 || !bestRes) return false;
    auto ind = g.m_provinceIndustry.find(bestPid);
    outPid = bestPid; outRes = bestRes;
    outCost = IND_COST[std::clamp(ind->second.level, 0, IND_MAX_LEVEL)] *
              SPECIALIZE_COST_MULT *
              buildCostMod(g.getTotalEffect("industryCostPct", cid));
    return true;
}

bool AISystem::bestEmbarkPort(int cid, int& outPid, int& outGarrison) const {
    Game& g = *m_g;
    // ── A GARRISON GUARD WAS TRIED HERE, AND MEASURED, AND REMOVED ──
    //
    // Embarking takes half a port's garrison, and once the mask below stopped
    // wrongly refusing this action the head started taking it in earnest --
    // 4,095 embarkations against 1,810 -- while the model's own survival fell
    // from 62% to 48%. The obvious reading is that it was shipping out the
    // defence of its own harbours, and a player would not empty a port with an
    // enemy army on its border.
    //
    // Refusing to embark from a frontier province facing a country we are at
    // war with does fix the survival: 48% back to 59%. It also costs a third of
    // everything the AI takes -- land held against the scripted player fell
    // 1.86x to 1.33x, and the landing rate 8% to 3%. The quiet harbours behind
    // the front are the wrong ones to sail from, because the front is where the
    // war is; the AI was not being careless, it was being aggressive, and the
    // aggression is what wins it ground.
    //
    // That was the reasoning when the guard was removed: keep the mask
    // honest, let the policy choose. IT IS BACK, as condition 3 of the
    // amphibious doctrine below, because the user chose a doctrine with
    // preconditions over constant attempts (2026-09-04) once landings became
    // real (the resolver used to delete the men, so the "8%" above was never
    // a landing rate). The cost measured then -- about a third of what the AI
    // takes -- was measured in that broken world too, so the REAL cost of
    // this condition is UNKNOWN: the only honest measurement is v8.3 against
    // v8.2, and a smaller number there is the old measurement being wrong,
    // not the doctrine being cheap. Do not chase whatever it turns out to be
    // as a regression. What the old measurement also showed,
    // that 3,520 of 3,940 loads came home, was half a training problem and
    // half the wrong-ocean bug fixed in the next paragraph.
    // ── ...AND A PORT ON THE RIGHT OCEAN ──
    //
    // The paragraph above concluded that the loads coming home again were a
    // training problem. Half of that is right and this half is not: the port
    // was chosen by GARRISON SIZE ALONE, so a country with coasts on two seas
    // loaded its largest harbour whether or not any enemy could be reached
    // from it. The mask upstream (validNavy v[3]) asks whether the COUNTRY
    // shares a body of water with somebody it is at war with; the amphibious
    // reflex then asks whether THIS HULL can reach a hostile port from where it
    // actually is. Those are different questions, and a hull loaded on the
    // wrong sea answers yes to the first and no to the second -- so the reflex
    // takes its "no war left to fight" branch and sails the cargo home.
    //
    // Measured before this: 2,115 embarkations, 45 landings, 1,807 returned
    // home. Two per cent. The rate was 9% before the nav grid was corrected to
    // stop treating enclosed lakes as open ocean, which is the giveaway --
    // making connectivity HONEST made more enemy ports genuinely unreachable,
    // and nothing on the loading end had ever been asking about reachability.
    //
    // So collect the seas an enemy harbour actually sits on, and load only from
    // a port that touches one of them. After: landings went 2% -> 20% on the
    // map where the AI has coasts on two oceans, and 2% -> 3% on the one where
    // it does not, which is exactly the shape the diagnosis predicts.
    //
    // AND IT DOES NOT MOVE THE RESULT. Paired over five worlds: +1.2 points
    // against the scripted rung (band +/-2.7, better on 4/5) and -1.6 against a
    // rusher (band +/-7.3, better on 2/5). Both NOT SEPARABLE. This is kept on
    // correctness rather than on land share -- loading an invasion at a harbour
    // no enemy can be reached from is wrong however the game ends, and a player
    // watching transports sail in circles for two hundred turns sees it. Do not
    // cite this change as a win; the honest claim is that the amphibious system
    // now does what it was written to do, at no measured cost.
    std::unordered_set<int> hostileBodies;
    const Country* me2 = g.m_countries.getCountry(cid);
    auto relIt2 = me2 ? g.m_relations.find(me2->isoA3) : g.m_relations.end();
    if (relIt2 == g.m_relations.end()) return false;
    {
        const Country* me = me2;
        auto relIt = relIt2;
        if (relIt != g.m_relations.end()) {
            for (const auto& [pid, port] : g.m_provincePorts) {
                (void)port;
                const Province* p = g.m_provinces.getProvinceById(pid);
                if (!p || p->countryId == cid) continue;
                const Country* ec = g.m_countries.getCountry(p->countryId);
                if (!ec) continue;
                auto rr = relIt->second.find(ec->isoA3);
                if (rr == relIt->second.end() || !rr->second.war) continue;
                const int body = g.seaBodyOfPort(pid);
                if (body >= 0) hostileBodies.insert(body);
            }
        }
    }
    if (hostileBodies.empty()) return false;

    // ── THE AMPHIBIOUS DOCTRINE (see AMPHIB_ARMY_SHARE in the header) ──
    //
    // 1. A national share. Men already aboard plus the force this port
    //    would load may not exceed AMPHIB_ARMY_SHARE of the army; six ports
    //    do not mean six invasions.
    long long aboard = 0;
    for (const auto& s : g.m_ships)
        if (s.countryId == cid && s.type == "boat") aboard += (long long)s.crew * 100;
    auto stIt = m_stats.find(cid);
    const long long armyTotal = stIt != m_stats.end() ? stIt->second.army : 0;
    const long long shareCap = (long long)(AMPHIB_ARMY_SHARE * (float)armyTotal);
    // Fails closed: no army figure means no embarkation. Said out loud under
    // aiDebug so a quiet no-amphibious window early in a run is not silent.
    if (armyTotal <= 0 && g.m_config.aiDebug)
        printf("[AI] t%d cid=%d: no army total in m_stats -- amphibious doctrine refuses\n", m_turn, cid);
    if (aboard >= shareCap) return false;
    // 2. A target it could hold -- FOR THE SCRIPTED COHORT ONLY. Conditions
    //    1 and 3 are rules about what a country can physically do and bind
    //    every cohort; whether a landing that might not win outright is still
    //    worth making (to fix a garrison, force a recall, take a province
    //    that will be weakly held next turn) is a judgment, and masking it
    //    would stop the war head ever trying, scoring or learning it -- the
    //    same trap that made the diplomacy head unreadable (journal 35c-35g).
    //    So the script believes in AMPHIB_ODDS; the model is free to be wrong.
    //    Recorded as a deliberate split, 2026-09-04.
    //    LOOSENESS, on purpose: the odds are taken against the WEAKEST
    //    hostile-port garrison on the port's sea body, and the province the
    //    boat is finally aimed at is chosen later by the amphibious reflex
    //    (nearest landable). Passing here means "a target this force could
    //    take exists on that sea", not "this force is aimed at it".
    //    m_scriptedThisCountry is the canonical flag (takeTurn sets it from all
    //    four scripted cases: tutorial, script duel, the vs-script control
    //    cohort, and the exploiter league) before the masks and executors of
    //    the same country run, so it is fresh on both paths that reach here
    //    (validWar's mask and execNavy's executor).
    const bool scriptedOdds = m_scriptedThisCountry;
    auto garrisonOf = [&](int pid, int owner) {
        long long n = 0;
        auto it = g.m_provinceArmies.find(pid);
        if (it != g.m_provinceArmies.end())
            for (const auto& u : it->second) if (u.countryId == owner) n += u.count;
        return n;
    };
    std::unordered_map<int, long long> weakestTargetOnBody;   // body -> smallest hostile garrison
    for (const auto& [pid, port] : g.m_provincePorts) {
        (void)port;
        const Province* p = g.m_provinces.getProvinceById(pid);
        if (!p || p->countryId == cid) continue;
        const Country* ec = g.m_countries.getCountry(p->countryId);
        if (!ec) continue;
        auto rr = relIt2->second.find(ec->isoA3);
        if (rr == relIt2->second.end() || !rr->second.war) continue;
        const int body = g.seaBodyOfPort(pid);
        if (body < 0) continue;
        const long long gar = garrisonOf(pid, p->countryId);
        auto w = weakestTargetOnBody.find(body);
        if (w == weakestTargetOnBody.end() || gar < w->second) weakestTargetOnBody[body] = gar;
    }
    int bestPid = -1, bestG = 0;
    for (const auto& [pid, port] : g.m_provincePorts) {
        (void)port;
        const Province* p = g.m_provinces.getProvinceById(pid);
        if (!p || p->countryId != cid) continue;
        // The sea this harbour opens onto has to be one an enemy is on.
        const int myBody = g.seaBodyOfPort(pid);
        if (myBody < 0 || !hostileBodies.count(myBody)) continue;
        bool pending = false;
        for (const auto& pe : g.m_pendingEmbarkations)
            if (pe.provinceId == pid) { pending = true; break; }
        if (pending) continue;
        // 3. Never from a province under threat: hostile troops on any
        //    neighbouring province means this garrison is the defence.
        {
            bool threatened = false;
            auto nIt = g.m_provinceNeighbors.find(pid);
            if (nIt != g.m_provinceNeighbors.end()) {
                for (int nid : nIt->second) {
                    const int o = (nid >= 0 && nid < (int)g.m_provinceCountryLookup.size())
                                      ? g.m_provinceCountryLookup[nid] : 0;
                    if (o <= 0 || o == cid) continue;
                    const Country* oc = g.m_countries.getCountry(o);
                    if (!oc) continue;
                    auto rr = relIt2->second.find(oc->isoA3);
                    if (rr == relIt2->second.end() || !rr->second.war) continue;
                    if (garrisonOf(nid, o) > 0) { threatened = true; break; }
                }
            }
            if (threatened) continue;
        }
        auto aIt = g.m_provinceArmies.find(pid);
        if (aIt == g.m_provinceArmies.end()) continue;
        int gsz = 0;
        for (const auto& u : aIt->second) if (u.countryId == cid) gsz += u.count;
        // The force is half the garrison (the executor's rule); it must fit
        // under the national share and outnumber the weakest target on its
        // sea by the doctrine's odds.
        const long long force = gsz / 2;
        if (aboard + force > shareCap) continue;
        if (scriptedOdds) {
            auto w = weakestTargetOnBody.find(myBody);
            if (w == weakestTargetOnBody.end()) continue;
            if ((float)force < AMPHIB_ODDS * (float)std::max(1LL, w->second)) continue;
        }
        if (gsz > bestG) { bestG = gsz; bestPid = pid; }
    }
    // The executor's own floor: half the garrison goes aboard, and half of
    // fewer than a thousand men is not an invasion.
    if (bestPid < 0 || bestG < 1000) return false;
    outPid = bestPid; outGarrison = bestG;
    return true;
}

const std::vector<AISystem::AttackCandidate>&
AISystem::attackCandidates(int cid) const {
    auto& slot = m_attackScanCache[cid];
    if (slot.turn == m_turn) return slot.cands;
    slot.turn = m_turn;
    slot.cands.clear();

    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    auto stIt = m_stats.find(cid);
    if (!c || stIt == m_stats.end()) return slot.cands;
    const CountryStat& st = stIt->second;
    auto relIt = g.m_relations.find(c->isoA3);

    auto garrisonOf = [&](int pid, int owner) {
        auto it = g.m_provinceArmies.find(pid);
        if (it == g.m_provinceArmies.end()) return 0;
        int n = 0;
        for (const auto& u : it->second) if (u.countryId == owner) n += u.count;
        return n;
    };
    // EVERYTHING THAT WILL SHOOT BACK, which is what resolveAssault fights --
    // an ally of the defender standing on the province is real defence, and an
    // estimate that cannot see it sends the army into a battle already lost.
    auto hostileGarrisonAt = [&](int pid) {
        auto it = g.m_provinceArmies.find(pid);
        if (it == g.m_provinceArmies.end()) return 0;
        int n = 0;
        for (const auto& u : it->second)
            if (u.count > 0 && u.countryId > 0 && u.countryId != cid &&
                !g.alliedCids(cid, u.countryId)) n += u.count;
        return n;
    };
    auto atWarWith = [&](int otherCid) {
        const Country* oc = g.m_countries.getCountry(otherCid);
        if (!oc || relIt == g.m_relations.end()) return false;
        auto rr = relIt->second.find(oc->isoA3);
        return rr != relIt->second.end() && rr->second.war;
    };

    const float atkMod = 1.0f + g.getTotalEffect("armyAtkPct", cid) / 100.0f;

    // One province's worth of candidates, from a launch province we hold or
    // are merely standing in. `floorMen` differs between the two only because
    // putting down a revolt is worth committing a smaller force to.
    auto scanFrom = [&](int fromPid, int floorMen, bool fromAlly) {
        const int myG = garrisonOf(fromPid, cid);
        if (myG < floorMen) return;
        auto nIt = g.m_provinceNeighbors.find(fromPid);
        if (nIt == g.m_provinceNeighbors.end()) return;
        for (int nid : nIt->second) {
            const int nOwner = (nid >= 0 && nid < (int)g.m_provinceCountryLookup.size())
                                   ? g.m_provinceCountryLookup[nid] : 0;
            if (nOwner <= 0 || nOwner == cid || !atWarWith(nOwner)) continue;
            const int defG = hostileGarrisonAt(nid);
            auto ind = g.m_provinceIndustry.find(nid);
            const float fort = ind != g.m_provinceIndustry.end()
                                 ? (float)ind->second.fortification : 0.0f;
            // ── SCORE THE ASSAULT THE WAY resolveAssault SCORES IT ──
            //
            // OD_WIDTH_MARGIN=1: both sides are capped at the province's
            // combat width, so past the cap extra men add nothing to the
            // fight, and an even contest is decided by the modifiers alone --
            // with every engaged attacker forfeited on a repulse. Norway,
            // 1939, one rushing Sweden (seed 20260801): 174,800 men sent at a
            // 177,769 garrison, 72,673 engaged each, atkPower 72,673 vs
            // defPower 77,033, repulsed, a third of the army gone on turn 1.
            // The old score (attackers over defenders, no cap) read that as a
            // fair fight; this one reads it as 0.94.
            static const bool widthMargin = std::getenv("OD_WIDTH_MARGIN") && atoi(std::getenv("OD_WIDTH_MARGIN")) != 0;   // OFF by default from v22: it was worth 20-50 on the OLD resolver, where an above-frontage repulse deleted the engaged men; depth pays for the reserve, and with it the gate costs 15 mean (journal 43a). OD_WIDTH_MARGIN=1 restores it.
            const float sent = myG * 0.75f;
            const float width = widthMargin ? (float)g.combatWidth(nid) : 1e30f;
            // Men beyond the frontage are the reserve: they do not widen the
            // fight, they deepen it (Game::depthFactor, both sides). The gate
            // mirrors the resolver's comparison, so it carries the same term
            // -- without it the gate refuses 2:1 attacks above the frontage
            // that now carry.
            const float atkDepth = widthMargin ? Game::depthFactor((long long)sent, (long long)width) : 1.0f;
            const float defDepth = widthMargin ? Game::depthFactor((long long)defG, (long long)width) : 1.0f;
            const float atk = std::min(sent, width) * atkMod * atkDepth;
            // Mirrors processArmyMovement: fortification AND the defender's own
            // defensive research.
            const float def = std::min((float)defG, width) * defDepth * (1.0f + fort * 0.1f) *
                              (1.0f + g.getTotalEffect("armyDefPct", nOwner) / 100.0f);
            // ── SUPPLY, WHICH THE RESOLVER APPLIES AND THIS DID NOT ──
            //
            // processArmyMovement multiplies attack power by
            // supplyFactor(attacker, pid) and every defender's contribution by
            // supplyFactor(defender, pid). This scan mirrored frontage, depth,
            // fortification and defensive research, and omitted supply -- so
            // since the supply model landed, the AI has been computing a
            // margin the resolver does not use: too optimistic attacking
            // beyond its free hops, too pessimistic against a cut-off
            // defender. Asked of the resolver's own function rather than
            // re-derived, so the two cannot drift apart again.
            // OD_SUPPLY_MARGIN=0 restores the blind margin.
            // OFF pending a settled game. Measured 2026-09-07 against a
            // same-binary control: at 400 turns -3 rating, +7 survival,
            // +22 floor; at 120 turns mildly negative on all three. That is
            // a gain on the metrics that generalise, but it was taken while
            // the other session was editing the resolver -- the control on
            // these seeds moved 349 -> 175 within the hour -- so it is a fact
            // about a binary that no longer exists. OD_SUPPLY_MARGIN=1 to
            // re-measure once the game is settled; the term itself is simply
            // the supply factor processArmyMovement already applies and this
            // scan omits.
            static const bool supplyMargin = std::getenv("OD_SUPPLY_MARGIN") &&
                                             atoi(std::getenv("OD_SUPPLY_MARGIN")) != 0;
            const float atkSup = supplyMargin ? g.supplyFactor(cid, nid)    : 1.0f;
            const float defSup = supplyMargin ? g.supplyFactor(nOwner, nid) : 1.0f;
            float margin = (def * defSup) > 0 ? (atk * atkSup) / (def * defSup) : 10.0f;
            // Both sides above the frontage: the resolver's comparison is the
            // modifiers alone, and numbers -- ours or a reinforcement's --
            // cannot change it. A margin over 1.0 is then a certain carry, so
            // it clears the winnability bar outright; under 1.0 it is a
            // certain repulse and the bar refuses it as before.
            // Above the frontage the comparison is no longer modifiers-only:
            // depth still separates the sides until both hit the cap, so the
            // old "certain carry" shortcut is gone with the depth rule.
            // Claimed provinces are priority targets: taking one both expands
            // us AND satisfies the claim.
            auto clIt = g.m_claimsByProvince.find(nid);
            if (clIt != g.m_claimsByProvince.end())
                for (const auto& iso : clIt->second)
                    if (iso == c->isoA3) { margin += 0.4f; break; }
            // Secession outranks foreign conquest: every turn a breakaway
            // survives it entrenches.
            if (nOwner >= Game::REBEL_CID_MIN) margin += 1.0f;
            // ── THE WINNABILITY BAR, AND WHY IT IS STILL A COIN FLIP ──
            //
            // 1.05 means the AI will throw 85% of a garrison at a defender it
            // barely outnumbers (the sizing below is 0.75 * ATTACK_SAFETY /
            // margin, which clamps at this margin). It shows: benched as France
            // in a world at war it issues 4.008 attacks per turn against the
            // blitz script's 1.754, and 40% of them are REPULSED -- 189 of 473.
            // That is the collapse. It does not start outnumbered; it spends
            // its army on coin flips and becomes outnumbered, then its income
            // falls, then it is under $8 on 58% of turns and can no longer
            // afford the recruits its own war head asks for 70% of the time.
            // It finishes that seat with 3 provinces.
            //
            // TRIED, 1.05 -> 1.40, and it is the single clearest instance of
            // the pattern that runs through this whole file:
            //
            //     seat                 before  after
            //     modern China              5     77
            //     1914 Sweden             120    167
            //     1939 USA                196    170
            //     1914 France, at war      97     29
            //     1939 Norway, one rusher 144     85
            //     rating                  129    124
            //
            // Caution rescues the positions that were collapsing and ruins the
            // ones that were winning. China is not losing for the same reason
            // France is winning: a weak country dies by spending its army, a
            // strong one dies by hoarding it. Raising the declaration bar
            // (AI_WAR_BAR_CLAIMED 0.85 -> 1.5) gives the same split from the
            // other end -- China 5 -> 51, France 97 -> 7.
            //
            // Cherry-picking the better of the two per seat would rate 166
            // against this build's 129. That number is the prize for making the
            // bar CONDITIONAL on position, and it is unreachable by any
            // constant here, because one constant is applied to a great power
            // and a doomed minor at the same moment. The policy can see
            // position; this line cannot.
            //
            // ── AND A CONDITIONAL RULE WAS TRIED, AND THE SIGNAL WAS BACKWARDS ──
            //
            // The obvious next move is to make the bar depend on the position
            // rather than pick one number for everybody: demand real odds when
            // the attack gambles a large share of the national army (myG /
            // st.army > 0.15) and keep the opportunistic bar when it does not.
            // The reasoning was that France, spread over eighty provinces,
            // risks little on any one assault while a small country risks
            // everything.
            //
            //     seat                 before  after
            //     modern China              5      4
            //     1914 France, at war      97      1
            //     1914 Sweden             120    167
            //     1939 USA                196    204
            //     rating                  129    117
            //
            // It failed at the two seats it was designed for, in both
            // directions: China got the cheap bar it did not want and France
            // got the dear one. The geometry is the opposite of the intuition
            // -- in a world at war France's army is MASSED on the threatened
            // front, so one garrison is a large share of it, while a small
            // country's small army is spread thin across its few provinces.
            //
            // The lesson is not that conditioning is wrong; it is that choosing
            // the conditioning signal is the hard part, and a plausible one
            // derived from first principles was exactly inverted. That is what
            // the policy's features are for: it does not have to be told which
            // signal matters. Anyone trying again should get the signal from a
            // trained head rather than from an argument.
            if (margin <= 1.05f) continue;   // the winnability bar
            slot.cands.push_back({fromPid, nid, nOwner, margin, fromAlly, myG, defG,
                                  (int)fort,
                                  ind != g.m_provinceIndustry.end()
                                      ? ind->second.level : 0});
        }
    };

    for (const auto& fr : st.frontiers)
        scanFrom(fr.pid, fr.enemyCid >= Game::REBEL_CID_MIN ? 150 : 500, false);
    // Assaults launched from allied soil -- what turns a staged army into an
    // offensive instead of a garrison on somebody else's border.
    for (int apid : st.abroadPids) scanFrom(apid, 500, true);
    return slot.cands;
}

bool AISystem::stageAvailable(int cid) const {
    Game& g = *m_g;
    auto stIt = m_stats.find(cid);
    if (stIt == m_stats.end()) return false;
    auto garrisonOf = [&](int pid) {
        auto it = g.m_provinceArmies.find(pid);
        if (it == g.m_provinceArmies.end()) return 0;
        int n = 0;
        for (const auto& u : it->second) if (u.countryId == cid) n += u.count;
        return n;
    };
    for (const auto& st : stIt->second.staging) {
        // The executor's own floor: not worth splitting a token garrison.
        if (garrisonOf(st.fromPid) < 500) continue;
        bool busy = false;
        for (const auto& mo : g.m_pendingMoveOrders)
            if (mo.fromProvince == st.fromPid && mo.countryId == cid) { busy = true; break; }
        if (!busy) return true;
    }
    return false;
}

bool AISystem::shipDestination(int cid, double fromLon, double fromLat,
                               int& outPid, double& outLon, double& outLat) const {
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    if (!c) return false;
    auto relIt = g.m_relations.find(c->isoA3);
    auto atWarWith = [&](int otherCid) {
        const Country* oc = g.m_countries.getCountry(otherCid);
        if (!oc || relIt == g.m_relations.end()) return false;
        auto rr = relIt->second.find(oc->isoA3);
        return rr != relIt->second.end() && rr->second.war;
    };

    // At war, steam at the enemy. Nearest by straight line among the ports this
    // hull can actually REACH by sea -- see the note on navReachable.
    double bestD = 1e18;
    bool found = false;
    for (const auto& [pid, port] : g.m_provincePorts) {
        (void)port;
        const Province* p = g.m_provinces.getProvinceById(pid);
        if (!p || !atWarWith(p->countryId)) continue;
        double lon, lat;
        if (!g.portApproach(pid, lon, lat)) continue;
        if (!g.navReachable(fromLon, fromLat, lon, lat)) continue;
        const double dLon = Game::lonDelta(fromLon, lon), dLat = lat - fromLat;   // wrapped at the antimeridian
        const double d = dLon * dLon + dLat * dLat;
        if (d < bestD) { bestD = d; outPid = pid; outLon = lon; outLat = lat; found = true; }
    }
    if (found) return true;

    // Peacetime: a fleet with nothing to attack still has somewhere to be --
    // its own ports, where it can resupply and where it covers the coast it is
    // supposed to be covering.
    for (const auto& [pid, port] : g.m_provincePorts) {
        (void)port;
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
        double lon, lat;
        if (!g.portApproach(pid, lon, lat)) continue;
        if (!g.navReachable(fromLon, fromLat, lon, lat)) continue;
        const double dLon = Game::lonDelta(fromLon, lon), dLat = lat - fromLat;   // wrapped at the antimeridian
        const double d = dLon * dLon + dLat * dLat;
        if (d < 0.25) return false;   // already on station here
        if (d < bestD) { bestD = d; outPid = pid; outLon = lon; outLat = lat; found = true; }
    }
    return found;
}

const std::vector<int>& AISystem::shipsWithDestination(int cid) const {
    auto& slot = m_shipScanCache[cid];
    if (slot.turn == m_turn) return slot.ships;
    slot.turn = m_turn;
    slot.ships.clear();
    Game& g = *m_g;
    for (size_t i = 0; i < g.m_ships.size(); ++i) {
        if (g.m_ships[i].countryId != cid) continue;
        int pid = -1; double lon = 0, lat = 0;
        if (shipDestination(cid, g.m_ships[i].lon, g.m_ships[i].lat, pid, lon, lat))
            slot.ships.push_back((int)i);
    }
    return slot.ships;
}

bool AISystem::navyMoveAvailable(int cid) const {
    const std::vector<int>& ships = shipsWithDestination(cid);
    if (ships.empty()) return false;
    // A hull already under orders is not one this action can move. Re-derived
    // rather than cached: it is the one thing that changes inside a turn.
    for (int idx : ships) {
        bool busy = false;
        for (const auto& mo : m_g->m_pendingShipMoveOrders)
            if (mo.shipIndex == idx) { busy = true; break; }
        if (!busy) return true;
    }
    return false;
}

const Policy* AISystem::enactablePolicy(int cid) const {
    auto& slot = m_enactCache[cid];
    if (slot.turn == m_turn) return slot.policy;
    slot.turn = m_turn;
    slot.policy = nullptr;

    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    if (!c || g.m_allPolicies.empty()) return nullptr;
    // No new upkeep while the map moves against us: the projection below
    // still counts the provinces about to be lost. See AI_LOSS_FREEZE_TURNS.
    if (losingGround(cid)) return nullptr;
    const CountryIncomeSnapshot inc = g.projectIncome(cid, planHorizon());
    const float committed = inc.policyCosts + inc.minorityCosts + inc.pacificationCost;
    const float budget = std::max(0.0f, inc.total * AI_DOCTRINE_BUDGET_SHARE);
    if (committed >= budget) return nullptr;

    // Compass fit dominates -- a government does not enact things it disagrees
    // with -- but a cheap doctrine wins ties, which over a long game is the
    // difference between a budget and a slow bleed. The executor's own rule.
    const Policy* best = nullptr; float bestScore = -1e9f;
    for (const auto& p : g.m_allPolicies) {
        if (!g.canCountryEnactPolicy(cid, p)) continue;
        if (committed + (float)p.costPerTurn > budget) continue;
        const float d = std::fabs(c->compassEconomic / 25.0f - p.econShift) +
                        std::fabs(c->compassSocial / 25.0f - p.socShift);
        float score = -d;
        if (inc.total > 1.0f) score -= 3.0f * (p.costPerTurn / inc.total);
        // A doctrine the advisor asked for, scored INSIDE the loop and after
        // both gates above -- so one the country may not legally enact, or
        // cannot afford, is never reached and naming it does nothing. As an
        // early return this would be the war-bar mistake in another function.
        if (!p.id.empty() && p.id == m_g->llmPreferredDoctrine(cid))
            score += AI_LLM_DOCTRINE;
        if (score > bestScore) { bestScore = score; best = &p; }
    }
    slot.policy = best;
    return best;
}

// ── IS THERE ANYTHING TO BOMBARD ──
//
// The mask asked only "do we own a warship", so the navy head chose
// bombard 1,124 times in an 80-turn eval and 1,123 of those did nothing:
// no researched ammunition it could afford, or no enemy port inside any
// hull's range. That is the same waste the doctrine action had before
// enactablePolicy (97.4% no-ops), and the same fix -- offer the action
// only when the executor would actually fire, asked once per turn and
// cached, so the mask and the executor cannot disagree.
bool AISystem::bombardAvailable(int cid) const {
    auto& slot = m_bombardCache[cid];
    if (slot.turn == m_turn) return slot.ok;
    slot.turn = m_turn; slot.ok = false;
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    if (!c) return false;
    static const char* AMMO_NODE[] = {"arty3", "arty2", "arty1"};
    static const char* AMMO_TYPE[] = {"heavy", "light", "mortar"};
    bool haveAmmo = false;
    for (int i = 0; i < 3; ++i) {
        if (!g.hasResearched(AMMO_NODE[i], cid)) continue;
        const Game::WarPrice p2 = g.artilleryPrice(AMMO_TYPE[i], cid);
        if (c->treasury < p2.money || !g.canAffordWarMaterials(cid, p2)) continue;
        haveAmmo = true; break;
    }
    if (!haveAmmo) return false;
    const int mapW = g.m_provinces.getWidth(), mapH = g.m_provinces.getHeight();
    if (mapW <= 0 || mapH <= 0) return false;
    for (const auto& s2 : g.m_ships) {
        if (s2.countryId != cid || s2.type == "boat") continue;
        for (const auto& [pid, port] : g.m_provincePorts) {
            (void)port;
            const Province* p = g.m_provinces.getProvinceById(pid);
            if (!p || p->countryId <= 0 || p->countryId == cid) continue;
            const Country* oc = g.m_countries.getCountry(p->countryId);
            if (!oc || !g.hasRelation(c->isoA3, oc->isoA3, &CountryRelation::war)) continue;
            auto cIt = g.m_provinceCenters.find(pid);
            if (cIt == g.m_provinceCenters.end()) continue;
            const double lon = cIt->second.x / mapW * 360.0 - 180.0;
            const double lat = 90.0 - cIt->second.y / mapH * 180.0;
            if (Game::seaDistanceDeg(s2.lon, s2.lat, lon, lat) <= g.shipMaxRangeDeg(s2)) {
                slot.ok = true; return true;
            }
        }
    }
    return false;
}

bool AISystem::attackAvailable(int cid) const {
    const std::vector<AttackCandidate>& cands = attackCandidates(cid);
    if (cands.empty()) return false;
    // A launch province already carrying an order from an earlier decision is
    // spoken for -- the executor's own first test. Re-derived rather than
    // cached: this is the one thing that changes inside a turn.
    std::unordered_set<int> preOrdered;
    for (const auto& mo : m_g->m_pendingMoveOrders)
        if (mo.countryId == cid) preOrdered.insert(mo.fromProvince);
    for (const AttackCandidate& ch : cands)
        if (!preOrdered.count(ch.fromPid)) return true;
    return false;
}

const AISystem::PactTargets& AISystem::pactTargets(int cid) const {
    auto& slot = m_pactTargetCache[cid];
    if (slot.turn == m_turn) return slot;
    slot = PactTargets{};
    slot.turn = m_turn;

    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    if (!c) return slot;
    auto relIt = g.m_relations.find(c->isoA3);

    // Land we claim belongs to somebody we mean to conquer, not befriend --
    // otherwise the politics module keeps pacting the very targets the war
    // module wants and the map freezes. Same rule as the executor's.
    std::unordered_set<int> claimTargets;
    auto myClaims = g.m_claims.find(c->isoA3);
    if (myClaims != g.m_claims.end())
        for (int pid : myClaims->second) {
            const int owner = (pid >= 0 && pid < (int)g.m_provinceCountryLookup.size())
                                  ? g.m_provinceCountryLookup[pid] : 0;
            if (owner > 0 && owner != cid && owner < Game::SPC_CID)
                claimTargets.insert(owner);
        }

    auto stIt = m_stats.find(cid);
    if (stIt == m_stats.end()) return slot;

    std::unordered_set<int> seen;
    for (const auto& fr : stIt->second.frontiers) {
        if (!seen.insert(fr.enemyCid).second) continue;
        const Country* ec = g.m_countries.getCountry(fr.enemyCid);
        if (!ec || ec->isoA3.empty()) continue;
        if (claimTargets.count(fr.enemyCid) || !diploReady(cid, fr.enemyCid)) continue;
        bool pendingReq = false;
        for (const auto& da : g.m_pendingDiplomaticActions)
            if (da.sourceIso == c->isoA3 && da.targetIso == ec->isoA3) { pendingReq = true; break; }
        if (pendingReq) continue;

        bool war = false, allied = false, nap = false, guar = false;
        if (relIt != g.m_relations.end()) {
            auto rr = relIt->second.find(ec->isoA3);
            if (rr != relIt->second.end()) {
                war = rr->second.war;
                allied = rr->second.alliance;
                nap = rr->second.nonAggression;
                guar = rr->second.guarantee;
            }
        }
        if (war) continue;
        // An alliance already implies non-aggression and mutual defence, so an
        // allied pair has nothing left to ask for -- the executor's rule.
        if (!allied)          slot.any[0] = true;   // alliance
        if (!allied && !nap)  slot.any[1] = true;   // non-aggression
        if (!allied && !guar) slot.any[2] = true;   // guarantee
        if (slot.any[0] && slot.any[1] && slot.any[2]) break;
    }
    return slot;
}

bool AISystem::nextPortBuy(int cid, int& outPid, float& outCost) const {
    // Per country, per turn. See the declaration: the fallback branch runs
    // isProvinceCoastal, and the economy mask is rebuilt up to three times in a
    // country's turn.
    auto cIt = m_portBuyCache.find(cid);
    if (cIt != m_portBuyCache.end() && cIt->second.turn == m_turn) {
        outPid = cIt->second.pid; outCost = cIt->second.cost;
        return cIt->second.pid >= 0;
    }
    Game& g = *m_g;
    PortBuy found;
    found.turn = m_turn;

    const int cap = portCap(cid);
    for (const auto& [pid, port] : g.m_provincePorts) {
        const Province* p = g.m_provinces.getProvinceById(pid);
        if (!p || p->countryId != cid || port.level >= cap) continue;
        bool pending = false;
        for (const auto& pu : g.m_pendingUpgrades)
            if (pu.provinceId == pid && pu.type == "port") { pending = true; break; }
        if (pending) continue;
        found.pid = pid;
        found.cost = 60.0f * (float)(port.level + 1);
        break;
    }
    if (found.pid < 0 && cap >= 1) {
        // The most populous coastal province with no harbour. Only the best few
        // are tested for coast, exactly as the executor does.
        std::vector<std::pair<long long, int>> cands;
        for (int pid : g.provincesOf(cid)) {
            if (g.m_provincePorts.count(pid)) continue;
            const auto pop = g.m_provincePopulations.find(pid);
            cands.push_back({pop != g.m_provincePopulations.end() ? pop->second : 0, pid});
        }
        std::sort(cands.rbegin(), cands.rend());
        for (size_t i = 0; i < cands.size() && i < 4; ++i) {
            if (!g.isProvinceCoastal(cands[i].second)) continue;
            found.pid = cands[i].second;
            found.cost = 60.0f;
            break;
        }
    }
    m_portBuyCache[cid] = found;
    outPid = found.pid; outCost = found.cost;
    return found.pid >= 0;
}

void AISystem::updateCoalition() {
    Game& g = *m_g;
    const float pressure = coalitionPressure();
    const int target = m_world.largestCid;
    // No pressure, or nobody to be the problem: any standing coalition lapses.
    if (pressure <= 0.0f || target <= 0) { m_coalition = CoalitionState{}; return; }
    // A coalition already formed against this power and still inside its window
    // stands. Re-forming it every turn would reset the window continuously and
    // turn the burst back into the drip it exists to replace.
    if (m_coalition.target == target &&
        (m_turn - m_coalition.formedTurn) < COALITION_WINDOW)
        return;

    const Country* tc = g.m_countries.getCountry(target);
    if (!tc) { m_coalition = CoalitionState{}; return; }
    auto tRel = g.m_relations.find(tc->isoA3);
    const long long targetArmy = m_stats.count(target) ? m_stats[target].army : 0;

    CoalitionState next;
    next.target = target;
    next.formedTurn = m_turn;
    for (auto& [cid, st] : m_stats) {
        if (cid == target || cid <= 0 || cid >= Game::REBEL_CID_MIN) continue;
        if (st.provinces == 0) continue;
        const Country* c = g.m_countries.getCountry(cid);
        if (!c) continue;
        // Only somebody who can actually reach the target. A coalition of
        // countries on the far side of the world is a sentiment.
        bool borders = false;
        for (const auto& fr : st.frontiers)
            if (fr.enemyCid == target) { borders = true; break; }
        if (!borders) continue;
        // ...and not somebody already on its side. An ally of the leader is
        // part of the problem, not part of the answer.
        if (tRel != g.m_relations.end()) {
            auto rr = tRel->second.find(c->isoA3);
            if (rr != tRel->second.end() && (rr->second.alliance || rr->second.guarantee))
                continue;
        }
        next.members.insert(cid);
        next.combinedArmy += st.army;
    }

    // ── AND IT ONLY FORMS IF IT CAN ACTUALLY WIN ──
    // See COALITION_ODDS. A coalition that cannot clear the bar together is one
    // whose members would each be throwing an army away, which is precisely the
    // failure the first version of this shipped.
    if (next.members.size() < 2 ||
        (double)next.combinedArmy < (double)targetArmy * COALITION_ODDS) {
        m_coalition = CoalitionState{};
        return;
    }
    m_coalition = std::move(next);
    if (g.m_config.aiDebug)
        printf("[COALITION] t%d %d member(s) vs %s (%lld v %lld men)\n", m_turn,
               (int)m_coalition.members.size(), tc->name.c_str(),
               m_coalition.combinedArmy, targetArmy);
}

std::vector<float> AISystem::dynamicsInput(const std::vector<float>& emb,
                                           int module, int action) {
    std::vector<float> in(emb);
    in.resize(emb.size() + DYN_ACTION_ONEHOT, 0.0f);
    if (module >= 0 && module < MOD_COUNT &&
        action >= 0 && action < MAX_MODULE_ACTIONS)
        in[emb.size() + module * MAX_MODULE_ACTIONS + action] = 1.0f;
    return in;
}

float AISystem::embeddingValue(int module, const std::vector<float>& emb) const {
    // ── WHAT A PREDICTED STATE IS WORTH, READ BY SOMETHING THAT CAN READ IT ──
    //
    // The obvious choice is m_value[module], and it is the wrong one: the value
    // heads take the RAW FEATURE VECTOR (FEATURE_COUNT, 143 floats), while the
    // dynamics head predicts an EMBEDDING (TRUNK_OUT, 320). NeuralNet::forward
    // returns an empty vector on a size mismatch rather than complaining, so
    // feeding one to the other produced a score of exactly zero for every
    // action, on every decision -- a search that cost seven times the thinking
    // time and could not, by construction, express a preference.
    //
    // Q is the head that already takes an embedding, and max over its actions
    // IS a state value: what the position is worth is the best thing available
    // from it. So the planner and the critic read the same latent space, which
    // is the only arrangement in which planning in that space means anything.
    if (module < 0 || module >= MOD_COUNT || emb.empty()) return 0.0f;
    const std::vector<float>& q = const_cast<NeuralNet&>(m_q[module]).forward(emb);
    if (q.empty()) return 0.0f;
    float best = -1e30f;
    for (float v : q) if (std::isfinite(v)) best = std::max(best, v);
    return best > -1e29f ? best : 0.0f;
}

int AISystem::mctsSims() {
    static const int v = [] {
        if (const char* e = std::getenv("OD_MCTS_SIMS")) {
            const int n = atoi(e);
            if (n >= 0 && n <= 4096) return n;
        }
        return 0;                      // off: shipped behaviour
    }();
    return v;
}

float AISystem::mctsCpuct() {
    static const float v = [] {
        if (const char* e = std::getenv("OD_MCTS_CPUCT")) {
            const float f = (float)atof(e);
            if (f > 0.0f && f < 20.0f) return f;
        }
        return 1.4f;
    }();
    return v;
}

/**
 * Latent MCTS over one module's action set. See the note in the header.
 *
 * The tree lives in embedding space: a child is `m_dynamics(parent, action)`,
 * and a leaf is scored by the same `embeddingValue` the beam search used. What
 * differs is that this AGGREGATES -- the output is the visit distribution over
 * root actions, which is a better policy than the prior it started from,
 * whereas a beam returns the best leaf and therefore cannot be better than its
 * own evaluation function.
 */
void AISystem::mctsPolicy(int module, const std::vector<float>& emb,
                          const std::vector<bool>& valid,
                          std::vector<float>& visitsOut, bool rootNoise) {
    visitsOut.clear();
    const int sims = mctsSims();
    if (sims <= 0 || emb.empty() || module < 0 || module >= MOD_COUNT) return;
    if (m_dynamics.updateCount() < DYN_WARMUP_UPDATES) return;   // untrained model = noise

    const int nActs = (module == MOD_ECONOMY) ? ECON_ACTIONS
                    : (module == MOD_POLITICS) ? POL_ACTIONS
                    : (module == MOD_WAR) ? WAR_ACTIONS : NAVY_ACTIONS;

    struct Node {
        std::vector<float> emb;
        std::vector<float> P;      // prior over actions
        std::vector<int>   N;      // visits per action
        std::vector<float> W;      // summed value per action
        std::vector<int>   kid;    // child node index, -1 unexpanded
        int totalN = 0;
    };
    std::vector<Node> pool;
    pool.reserve((size_t)sims + 2);

    // Prior from the policy itself, masked to the legal set. A node whose
    // priors are all but degenerate is exactly the case root noise exists for.
    auto makeNode = [&](const std::vector<float>& e,
                        const std::vector<bool>* mask) -> int {
        Node n;
        n.emb = e;
        // The acting policy for this module. `m_leagueThisCountry` is not
        // consulted: a league opponent is frozen and does not search.
        const std::vector<float>& logits = m_policy[module].forward(e);
        n.P.assign((size_t)nActs, 0.0f);
        double sum = 0.0;
        float mx = -1e30f;
        for (int a = 0; a < nActs && a < (int)logits.size(); ++a)
            if (!mask || a >= (int)mask->size() || (*mask)[a])
                mx = std::max(mx, logits[a]);
        for (int a = 0; a < nActs && a < (int)logits.size(); ++a) {
            if (mask && a < (int)mask->size() && !(*mask)[a]) continue;
            const double p = std::exp((double)(logits[a] - mx));
            n.P[(size_t)a] = (float)p;
            sum += p;
        }
        if (sum > 0.0) for (float& p : n.P) p = (float)(p / sum);
        n.N.assign((size_t)nActs, 0);
        n.W.assign((size_t)nActs, 0.0f);
        n.kid.assign((size_t)nActs, -1);
        pool.push_back(std::move(n));
        return (int)pool.size() - 1;
    };

    const int root = makeNode(emb, &valid);
    if (pool[(size_t)root].P.empty()) return;

    // ── Dirichlet noise at the root, TRAINING ONLY ──
    // The one mechanism in this codebase that can give a 1e-33 action a visit.
    // Symmetric Dirichlet via normalised Gamma(alpha,1) draws.
    if (rootNoise) {
        std::gamma_distribution<double> g(MCTS_ROOT_ALPHA, 1.0);
        std::vector<double> d((size_t)nActs, 0.0);
        double dsum = 0.0;
        for (int a = 0; a < nActs; ++a) {
            if (a < (int)valid.size() && !valid[a]) continue;
            d[(size_t)a] = g(m_rng);
            dsum += d[(size_t)a];
        }
        if (dsum > 0.0)
            for (int a = 0; a < nActs; ++a)
                pool[(size_t)root].P[(size_t)a] =
                    (float)((1.0 - MCTS_ROOT_NOISE) * pool[(size_t)root].P[(size_t)a]
                            + MCTS_ROOT_NOISE * (d[(size_t)a] / dsum));
    }

    const float cpuct = mctsCpuct();
    std::vector<std::pair<int,int>> path;    // (node, action) taken this sim
    for (int sim = 0; sim < sims; ++sim) {
        path.clear();
        int cur = root;
        float leafValue = 0.0f;
        for (int d = 0; d < MCTS_MAX_DEPTH; ++d) {
            Node& nd = pool[(size_t)cur];
            // PUCT. The exploration term is proportional to the prior, which is
            // why noise at the root and not here: an action at 1e-33 gets no
            // help from U and must be handed visits directly.
            int best = -1; float bestScore = -1e30f;
            const float sqrtN = std::sqrt((float)std::max(1, nd.totalN));
            for (int a = 0; a < nActs; ++a) {
                if (cur == root && a < (int)valid.size() && !valid[a]) continue;
                if (nd.P[(size_t)a] <= 0.0f && nd.N[(size_t)a] == 0) continue;
                const float q = nd.N[(size_t)a] > 0
                              ? nd.W[(size_t)a] / (float)nd.N[(size_t)a] : 0.0f;
                const float u = cpuct * nd.P[(size_t)a] * sqrtN /
                                (1.0f + (float)nd.N[(size_t)a]);
                if (q + u > bestScore) { bestScore = q + u; best = a; }
            }
            if (best < 0) break;
            path.push_back({cur, best});
            if (pool[(size_t)cur].kid[(size_t)best] < 0) {
                const std::vector<float> nx =
                    m_dynamics.forward(dynamicsInput(pool[(size_t)cur].emb, module, best));
                if (nx.empty()) break;
                leafValue = embeddingValue(module, nx);
                pool[(size_t)cur].kid[(size_t)best] = makeNode(nx, nullptr);
                break;                              // expand one node per sim
            }
            cur = pool[(size_t)cur].kid[(size_t)best];
            leafValue = embeddingValue(module, pool[(size_t)cur].emb);
        }
        // Back up. Discounted, because a latent rollout drifts and a distant
        // estimate deserves less weight than a near one.
        float v = leafValue;
        for (int i = (int)path.size() - 1; i >= 0; --i) {
            Node& nd = pool[(size_t)path[(size_t)i].first];
            const int a = path[(size_t)i].second;
            nd.N[(size_t)a]++;
            nd.W[(size_t)a] += v;
            nd.totalN++;
            v *= MCTS_DISCOUNT;
        }
    }

    const Node& r = pool[(size_t)root];
    if (r.totalN <= 0) return;
    visitsOut.assign((size_t)nActs, 0.0f);
    for (int a = 0; a < nActs; ++a)
        visitsOut[(size_t)a] = (float)r.N[(size_t)a] / (float)r.totalN;
}

void AISystem::searchScores(int module, const std::vector<float>& emb,
                            const std::vector<bool>& valid,
                            std::vector<float>& out) {
    out.clear();
    if (!searchReady() || emb.empty()) return;
    const int depth = difficulty().searchDepth;
    const int nActs = (module == MOD_ECONOMY) ? ECON_ACTIONS
                    : (module == MOD_POLITICS) ? POL_ACTIONS
                    : (module == MOD_WAR) ? WAR_ACTIONS : NAVY_ACTIONS;
    out.assign((size_t)nActs, 0.0f);

    // ── ONE PLY: what is the world worth after I do this? ──
    //
    // The value head reads a state, so scoring an ACTION means predicting the
    // state it leads to and reading that. This is the step Q approximates by
    // learning it directly; done through the model it is exact about the
    // arithmetic and only wrong about the dynamics.
    std::vector<std::pair<float,int>> ranked;
    ranked.reserve((size_t)nActs);
    for (int a = 0; a < nActs; ++a) {
        if (a < (int)valid.size() && !valid[a]) continue;
        const std::vector<float> nx = m_dynamics.forward(dynamicsInput(emb, module, a));
        if (nx.empty()) continue;
        const float score = embeddingValue(module, nx);
        out[a] = score;
        ranked.push_back({score, a});
    }
    if (ranked.empty()) { out.clear(); return; }

    // ── AND A SECOND PLY, OVER A BEAM ──
    //
    // Every action at every depth would be |A|^2 -- 144 forward passes for the
    // economy on every decision of every country, which is the difference
    // between a third of a millisecond per country-turn and several. Only the
    // most promising few are worth expanding, and a beam of three over twelve
    // actions is 3 x 12 = 36 extra passes rather than 132.
    //
    // The second ply is scored as the BEST reply, not the average: what a
    // position is worth is what you can do from it, and averaging over actions
    // the policy would never take prices in mistakes nobody was going to make.
    if (depth >= 2 && ranked.size() > 1) {
        std::sort(ranked.rbegin(), ranked.rend());
        const size_t beam = std::min<size_t>(3, ranked.size());
        for (size_t bi = 0; bi < beam; ++bi) {
            const int a = ranked[bi].second;
            const std::vector<float> mid =
                m_dynamics.forward(dynamicsInput(emb, module, a));
            if (mid.empty()) continue;
            const std::vector<float> midCopy(mid);
            float best = ranked[bi].first;
            for (int a2 = 0; a2 < nActs; ++a2) {
                const std::vector<float> nx2 =
                    m_dynamics.forward(dynamicsInput(midCopy, module, a2));
                if (nx2.empty()) continue;
                // Discounted, so a reply two turns out is worth less than the
                // move in front of us -- and so the two plies are on the same
                // scale as the bootstrapped value they came from.
                best = std::max(best, ranked[bi].first +
                                      BOOTSTRAP_DISCOUNT * embeddingValue(module, nx2));
            }
            out[a] = best;
        }
    }

    // CENTRED, for the reason the Q blend is: a constant added to every logit
    // changes nothing after a softmax, so what should move is the preference
    // between actions rather than the overall confidence.
    float mean = 0.0f; int n = 0;
    for (int a = 0; a < nActs; ++a)
        if (a >= (int)valid.size() || valid[a]) { mean += out[a]; ++n; }
    if (n > 0) {
        mean /= (float)n;
        for (int a = 0; a < nActs; ++a) out[a] = SEARCH_BLEND * (out[a] - mean);
    }
}

float AISystem::coalitionPressure() const {
    if (!difficulty().useCoalition) return 0.0f;
    const float share = m_world.largestShare;
    if (share <= COALITION_SHARE) return 0.0f;
    return std::clamp((share - COALITION_SHARE) /
                      std::max(1e-6f, COALITION_FULL - COALITION_SHARE), 0.0f, 1.0f);
}

bool AISystem::isCoalitionTarget(int cid, int other) const {
    if (other <= 0 || other == cid) return false;
    if (other != m_world.largestCid) return false;
    return coalitionPressure() > 0.0f;
}

int AISystem::actionsPerModule(int cid) const {
    // ── NO THROTTLE THE PLAYER DOES NOT HAVE ──
    //
    // A person may build in every province they can afford in one turn. Capping
    // the AI at three was a cap on competence rather than a difficulty, and it
    // is why a rich country hoards: benched by hand as France, income ran at
    // 250 a turn against roughly 150 of purchasing capacity, and the treasury
    // climbed to 7,325 with everything worth buying already bought.
    //
    // Spam is handled where spam actually happens -- see AI_REQUESTS_PER_TURN,
    // which limits what a country ASKS OF OTHER COUNTRIES rather than what it
    // does at home. Measured: requests run at 0.04 per country-turn with take
    // rates of 0.1-5.5%, so that cap is a guard rather than a constraint --
    // the politics head does not currently choose to ask often.
    //
    // ── AND IT COSTS SEVEN POINTS TODAY, ON PURPOSE ──
    //
    // Seat bench, three seeds: 122 against 129 for the old flat three.
    //
    //     1914 Sweden             120 -> 333
    //     modern China              5 ->  29
    //     1939 USA                196 -> 123
    //     1914 France, at war      97 ->  33
    //     1939 Norway, one rusher 144 ->  15
    //
    // Both halves are the same fact. A small rich country can finally convert
    // income into power -- Sweden triples, which is the hoarding this exists to
    // end -- and a country under attack spends its new actions on whatever
    // ranked fourth through eighth, which on a threatened border is how an army
    // is lost. The policy was trained making THREE choices a turn; the extra
    // five are not free until it has been trained to have them.
    //
    // Self-play already trains under the wide budget (see the DIFFICULTY
    // self-play row), so this is expected to pay once a model raised that way
    // ships. Kept because the throttle was a cap on competence that the player
    // does not carry, and a difficulty that comes from hobbling the opponent is
    // not one. Revert by returning ACTIONS_PER_MODULE_PER_TURN here.
    return ACTIONS_PER_MODULE_MAX;
    const float scale = difficulty().actionScale;
    if (scale <= 0.0f) return ACTIONS_PER_MODULE_PER_TURN;
    auto it = m_stats.find(cid);
    const int provinces = (it != m_stats.end()) ? it->second.provinces : 0;
    // One extra go per 25 provinces held. A 25-province country is the scale
    // the flat 3 was chosen at, so that country is unchanged and everything
    // larger stops being throttled.
    const int scaled = ACTIONS_PER_MODULE_PER_TURN + (int)(scale * (provinces / 40));
    return std::clamp(scaled, ACTIONS_PER_MODULE_PER_TURN, ACTIONS_PER_MODULE_MAX);
}

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
    // ~1M parameters across the twenty nets (~6MB on disk with Adam state).
    // Most are HEADS on the shared trunk -- {TRUNK_OUT, N} apiece -- so the
    // encoder is paid for once rather than per module. Still fast: a forward
    // pass is ~0.2M multiply-adds, so hundreds of countries per turn stay in
    // the low milliseconds.
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
    m_diplo = NeuralNet({TRUNK_OUT, DIPLO_OUTPUTS}, 300);
    // Constructed rather than left default so an OPPONENT model file written
    // before the per-kind layout migrates through deserialize's gained-outputs
    // path instead of silently reshaping this head back to two.
    m_leagueDiplo = NeuralNet({TRUNK_OUT, DIPLO_OUTPUTS}, 301);
    // Own state and one candidate in, one score out. Scored once per candidate
    // and softmaxed across them, so the output is deliberately a single number
    // rather than a fixed-width action layer -- the candidate list changes size
    // every turn.
    m_target = NeuralNet({FEATURE_COUNT + TARGET_FEATURES, 256, 128, 1}, 500);
    m_attack = NeuralNet({FEATURE_COUNT + ATTACK_FEATURES, 256, 128, 1}, 700);
    m_diploValue = NeuralNet({FEATURE_COUNT, 160, 1}, 600);
    // The forward model. One hidden layer: it is fitting a one-step transition
    // in a space the trunk has already made linear-ish, not learning the game.
    // tanh on the output because the trunk's own output is tanh'd -- the thing
    // it is predicting lives in [-1, 1] and a linear head would spend its early
    // updates discovering that.
    m_dynamics = NeuralNet({TRUNK_OUT + DYN_ACTION_ONEHOT, 320, TRUNK_OUT}, 800);
    m_dynamics.setTanhOutput(true);
    // An empty path is a scratch model used for merging peer files, not a
    // model anybody is training. It loads nothing, saves nothing, and should
    // say nothing.
    if (m_modelPath.empty()) return;
    // Web: the trained weights are excluded from the preload -- 5 MB of a
    // package the menu waits on, for a file nothing reads until a game starts.
    // Fetched here, which is the first moment anything wants it. A failed
    // download is not fatal: loadModel() then finds nothing and the game plays
    // on a fresh net, exactly as it does on a machine with no model file.
    odEnsureAsset(m_modelPath);
    if (loadModel()) {
        printf("[AI] Model loaded from %s (%.1f MB)\n", m_modelPath.c_str(),
               serializedSize() / 1048576.0);
        // "0.0 MB on disk" was not a size, it was an uninitialised counter:
        // m_lastSaveBytes is written only by saveModel(), so it read zero until
        // the first checkpoint -- and permanently under --ai-readonly, which
        // never saves at all. Measure the model we actually hold instead.
        m_lastSaveBytes = serializedSize();
    }
    else if (!m_loadError.empty()) {
        // LOUD, AND ON stderr. A refused model means this run is playing on
        // random weights, and a run that scores random weights while claiming a
        // model's name is worse than a run that does not start.
        fprintf(stderr,
                "[AI] ERROR: %s exists but was REFUSED: %s\n"
                "[AI] ERROR: playing on a FRESH, UNTRAINED net -- any measurement "
                "from this run is of random weights, not of that file.\n",
                m_modelPath.c_str(), m_loadError.c_str());
    } else {
        printf("[AI] Fresh model (no file at %s)\n", m_modelPath.c_str());
    }

    // The control cohort's brain, when --vs-model named one. Loaded here rather
    // than by the caller because --eval-ai rebuilds this object on every map,
    // and an opponent that survived only the first map would leave the rest of
    // the run measuring dice under an "OPPONENT" heading.
    if (!s_opponentModelPath.empty()) loadOpponentModel(s_opponentModelPath);
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

int AISystem::warsWithTheDead() const {
    int n = 0;
    for (const auto& [cid, enemies] : m_warWith) {
        auto me = m_stats.find(cid);
        if (me == m_stats.end() || me->second.provinces <= 0) continue;
        for (int other : enemies) {
            auto them = m_stats.find(other);
            if (them == m_stats.end() || them->second.provinces <= 0) n++;
        }
    }
    return n;
}

void AISystem::compassGap(float& meanOut, float& worstOut) const {
    double m = 0.0; float w = 0.0f; int n = 0;
    for (const auto& [cid, st] : m_stats) {
        if (cid >= Game::REBEL_CID_MIN || st.provinces <= 0) continue;
        m += st.compassGapMean; w = std::max(w, st.compassGapWorst); n++;
    }
    meanOut = n ? (float)(m / n) : 0.0f;
    worstOut = w;
}

void AISystem::noteAssaultRepulsed(int attackerCid, int troopsLost) {
    if (attackerCid <= 0) return;
    TrainStats& s = statsFor(attackerCid);
    s.attacksRepulsed++;
    s.troopsLostAttacking += std::max(0, troopsLost);
}

void AISystem::noteShipSunk(int attackerCid, int victimCid, int crew) {
    if (crew > 0) {
        if (attackerCid > 0) m_crewDrownedThisTurn[attackerCid] += crew;
        if (victimCid   > 0) m_crewLostThisTurn[victimCid]     += crew;
    }
}

std::string AISystem::didNothing(std::string why) {
    m_execNoop = true;
    // OD_ACT_HIST counts refusals by reason. There are 58 of these across the
    // four exec functions and not one has ever been measured. Each is a place
    // where the POLICY chose an action and a rule then declined to carry it
    // out -- the same shape as the "repress: already hardest" no-op, which
    // burned a decision every turn and, in the source's own words, kept
    // "generating a gradient, teaching the politics head that the action is
    // safe and free". A refusal that fires often is not free: it costs the
    // country its turn and teaches the head something untrue.
    static const bool histOn = std::getenv("OD_ACT_HIST") != nullptr;
    if (histOn) ++s_noopWhy[why];
    return why;
}

std::map<std::string, long long> AISystem::s_noopWhy;

void AISystem::dumpNoopHistogram() {
    if (s_noopWhy.empty()) return;
    long long tot = 0;
    for (auto& kv : s_noopWhy) tot += kv.second;
    std::vector<std::pair<long long, std::string>> rows;
    for (auto& kv : s_noopWhy) rows.push_back({kv.second, kv.first});
    std::sort(rows.rbegin(), rows.rend());
    printf("[NOOP] %lld refused executions, by reason:\n", tot);
    for (auto& r : rows)
        printf("[NOOP] %9lld  %5.1f%%  %s\n", r.first,
               100.0 * (double)r.first / (double)tot, r.second.c_str());
}

void AISystem::noteLanding(int cid, bool hostileShore) {
    if (cid <= 0) return;
    if (hostileShore) { statsFor(cid).landings++; m_landingsThisTurn[cid]++; }
    else                statsFor(cid).unloadsHome++;
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

void AISystem::setRandomCountries(std::unordered_set<int> cids) {
    m_randomCids = std::move(cids);
    // THE CONTROL COHORT IS ONE SET OF COUNTRIES WITH TWO POSSIBLE BRAINS.
    //
    // Membership, the counters (statsFor above) and every line of the report
    // are the same either way; only where the choice comes from differs. So an
    // opponent model reuses the split verbatim and simply names those same
    // countries as league countries, which is the switch takeTurn already reads
    // to run a frozen brain. Nothing downstream has to know which mode it is.
    if (m_opponentLoaded) m_leagueCids = m_randomCids;
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
    m_overturesRefusedThisTurn.clear();
    m_crewDrownedThisTurn.clear();
    m_crewLostThisTurn.clear();
    m_shipsBoughtThisTurn.clear();
    m_shipsScrappedThisTurn.clear();
    m_requestsThisTurn.clear();   // see AI_REQUESTS_PER_TURN
    refreshStats();
    updateWorld();
    // Reads m_world.largestCid and every country's army, so it goes after both.
    updateCoalition();
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
    m_worldProvinces = 0;
    m_worldPixels = 0;

    Game& g = *m_g;

    // Provinces / population / industry — one pass each over existing maps.
    for (const auto& [pid, prov] : g.m_provinces.getAllProvinces()) {
        int cid = prov.countryId;
        if (cid <= 0 || cid >= Game::SPC_CID) continue;
        CountryStat& st = m_stats[cid];
        st.provinces++;
        if (cid < Game::REBEL_CID_MIN) m_worldProvinces++;
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
                // The policy dial, not the observed trend: ensureTrendBounds
                // derives m_trendMin/m_trendMax from the option table, and the
                // validity mask compares against them to decide whether there
                // is anywhere left to move. The observed trend goes to zero at
                // the drift bound, which would read as "no options left" on a
                // country that has every option available.
                trendSum += g.getMinorityPolicyRate(cid, name);
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

    // ── Political distance between a government and its own provinces ──
    //
    // One pass over owned provinces, alongside everything else refreshStats
    // already walks. See CountryStat::compassGapMean.
    {
        for (auto& [cid, st] : m_stats) {
            if (cid >= Game::SPC_CID) continue;
            const Country* c = g.m_countries.getCountry(cid);
            if (!c) continue;
            double sum = 0.0; float worst = 0.0f; int n = 0, far = 0;
            for (int pid : g.provincesOf(cid)) {
                auto pcIt = g.m_provinceCompass.find(pid);
                if (pcIt == g.m_provinceCompass.end()) continue;
                const float dx = pcIt->second.x - c->compassEconomic;
                const float dy = pcIt->second.y - c->compassSocial;
                // Normalised by the width of the space, so 1.0 is a province at
                // the opposite corner of the compass from its own government.
                const float d = std::min(1.0f, std::sqrt(dx * dx + dy * dy) / 200.0f);
                sum += d; n++;
                if (d > worst) worst = d;
                if (d > COMPASS_FAR) far++;
            }
            if (n > 0) {
                st.compassGapMean  = (float)(sum / n);
                st.compassGapWorst = worst;
                st.compassGapShare = (float)far / (float)n;
            }
        }
    }

    // THE TYPICAL COUNTRY'S GARRISON DENSITY, as a median and not a mean.
    //
    // This is the bar the army term measures sufficiency against, so what
    // matters is that a normal country can actually reach it. The aggregate
    // mean cannot serve: a handful of large empires drag it up, and measured on
    // the shipped scenarios it sat at 342,687 troops per province against a
    // typical country's 231,409 -- so 19 of 21 countries were below the bar
    // even at the parity that is arithmetically satisfiable. A target most of
    // the world can never reach makes recruit pay forever, which is precisely
    // the failure the third shape of this term died of. The median puts roughly
    // half the world on each side of the line by construction.
    {
        std::vector<double> dens;
        dens.reserve(m_stats.size());
        for (const auto& [cid, st] : m_stats) {
            if (cid >= Game::REBEL_CID_MIN || st.provinces <= 0) continue;
            dens.push_back((double)st.army / (double)st.provinces);
        }
        if (dens.empty()) {
            m_medianArmyPerProvince = 0.0;
        } else {
            const size_t mid = dens.size() / 2;
            std::nth_element(dens.begin(), dens.begin() + mid, dens.end());
            m_medianArmyPerProvince = dens[mid];
        }
    }

    // Provinces lost since last turn. "Am I losing?" is not derivable from a
    // single snapshot, and it is the signal a defensive policy needs most.
    for (auto& [cid, st] : m_stats) {
        auto prev = m_prevProvinces.find(cid);
        if (prev != m_prevProvinces.end() && prev->second > st.provinces) {
            st.provincesLost = prev->second - st.provinces;
            m_lastLossTurn[cid] = m_turn;
        }
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
        // ── WHICH SEA EACH COUNTRY'S HARBOURS ARE ON ──
        //
        // A naval target used to be any at-war coastal country that was not a
        // land neighbour, with NO test that the water between them joins up.
        // So a country on the Black Sea counted an enemy on the Pacific as an
        // invasion target: the navy mask offered "embark", the head took it,
        // half a port's garrison went aboard, and the transports sailed nowhere
        // for the rest of the game. Measured after the embark mask was fixed
        // and the action started actually firing: 4,095 embarkations, 3,589 of
        // them carried home again, 8% reaching a hostile shore.
        //
        // The nav grid knows one sea from another now (see buildNavGrid), so
        // "can I get there at all" is answerable -- and cheaply, by collecting
        // the water bodies each country's harbours touch and intersecting two
        // small sets, rather than testing every pair of ports on the map.
        std::unordered_map<int, std::unordered_set<int>> portBodies;
        std::unordered_set<int> seenCoastal;
        for (auto& [pid, port] : g.m_provincePorts) {
            int owner = (pid >= 0 && pid < (int)g.m_provinceCountryLookup.size())
                            ? g.m_provinceCountryLookup[pid] : 0;
            if (owner <= 0 || owner >= Game::REBEL_CID_MIN) continue;
            if (seenCoastal.insert(owner).second) coastal.push_back(owner);
            const int body = g.seaBodyOfPort(pid);
            if (body >= 0) portBodies[owner].insert(body);
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
                // Our own harbours' seas, looked up once for this country.
                auto myBodiesIt = portBodies.find(cid);
                for (int oc : coastal) {
                    if (oc == cid || landNbr.count(oc)) continue;
                    const Country* ec = g.m_countries.getCountry(oc);
                    if (!ec) continue;
                    // REACHABLE, not merely overseas. No shared body of water
                    // means no fleet of ours can ever arrive.
                    if (myBodiesIt != portBodies.end()) {
                        auto theirIt = portBodies.find(oc);
                        bool shared = false;
                        if (theirIt != portBodies.end())
                            for (int b : theirIt->second)
                                if (myBodiesIt->second.count(b)) { shared = true; break; }
                        if (!shared) continue;
                    }
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
        // Ascending, so the last is the biggest. See rivalShareFor.
        w.largestCid  = byLand.back().second;
        w.secondShare = byLand.size() > 1
                            ? (float)byLand[byLand.size() - 2].first / (float)total
                            : 0.0f;
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

    //
    // BOTH ENVS TAKE A COMMA LIST, so several features can go at once --
    // "0,4" with "0.5760,0.2267" ablates the army ratio and the war flag
    // together, each to its own mean. That is how you ask whether two
    // features that cost the same alone are one signal expressed twice (the
    // pair costs what either does) or two that happen to cost the same (the
    // pair costs about double). A single value with several indices applies
    // to all of them; a mismatched pair of lists is refused loudly rather
    // than silently ablating half the arms.
    struct RelAblation {
        bool  on[REL_FEATURES] = {};
        float to[REL_FEATURES] = {};
        bool  any = false;
        /// OD_AI_REL_ABLATE_MODE=rotate permutes the feature across the
        /// candidates instead of pinning it to `to`. See the note where it
        /// is applied -- it is the only neutral ablation for a binary
        /// feature, and the stricter one for a continuous feature.
        bool  rotate = false;
    };
    static const RelAblation abl = [] {
        RelAblation a;
        const char* f = std::getenv("OD_AI_REL_ABLATE_FEATURE");
        if (!f) return a;
        const char* v = std::getenv("OD_AI_REL_ABLATE_VALUE");
        auto split = [](const char* csv) {
            std::vector<std::string> out;
            if (!csv) return out;
            std::string cur;
            for (const char* p2 = csv; ; ++p2) {
                if (*p2 == ',' || *p2 == '\0') {
                    if (!cur.empty()) out.push_back(cur);
                    cur.clear();
                    if (*p2 == '\0') break;
                } else if (!isspace((unsigned char)*p2)) {
                    cur += *p2;
                }
            }
            return out;
        };
        const std::vector<std::string> fs = split(f), vs = split(v);
        if (!vs.empty() && vs.size() != 1 && vs.size() != fs.size()) {
            fprintf(stderr, "[REL-ABLATE] %zu feature(s) but %zu value(s): "
                            "give one value or one per feature. Ignoring.\n",
                    fs.size(), vs.size());
            return a;
        }
        for (size_t i = 0; i < fs.size(); ++i) {
            const int idx = atoi(fs[i].c_str());
            if (idx < 0 || idx >= REL_FEATURES) {
                fprintf(stderr, "[REL-ABLATE] no feature %d; the slice is 0..%d. Ignoring.\n",
                        idx, REL_FEATURES - 1);
                return RelAblation{};
            }
            a.on[idx] = true;
            a.to[idx] = vs.empty() ? 0.0f
                      : (float)atof(vs[vs.size() == 1 ? 0 : i].c_str());
            a.any = true;
        }
        const char* mode = std::getenv("OD_AI_REL_ABLATE_MODE");
        a.rotate = mode && std::strcmp(mode, "rotate") == 0;
        if (a.any && a.rotate) {
            fprintf(stderr, "[REL-ABLATE] rotating");
            for (int k = 0; k < REL_FEATURES; ++k)
                if (a.on[k]) fprintf(stderr, " r%d", k);
            fprintf(stderr, " across the candidates\n");
        } else if (a.any) {
            fprintf(stderr, "[REL-ABLATE] holding");
            for (int k = 0; k < REL_FEATURES; ++k)
                if (a.on[k]) fprintf(stderr, " r%d=%.4f", k, (double)a.to[k]);
            fprintf(stderr, "\n");
        }
        return a;
    }();
    // The world's mean war weariness, for the fog below. Computed over the real
    // countries rather than over the six neighbours in hand: a mean taken from
    // the candidate set would still carry information about that set, which is
    // the thing being withheld.
    float meanWeariness = 0.0f;
    {
        int n = 0;
        for (const auto& [ocid2, ost] : m_stats) {
            (void)ost;
            if (ocid2 <= 0 || ocid2 >= Game::REBEL_CID_MIN) continue;
            meanWeariness += g.warWearinessOf(ocid2);
            ++n;
        }
        if (n > 0) meanWeariness /= (float)n;
    }

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
        // ── WHAT IT KNOWS ABOUT SOMEBODY ELSE'S MONEY ──
        //
        // This reads the other country's ACTUAL treasury, and so does every
        // other feature here: the AI has perfect information about army,
        // provinces, industry and cash, and has never consulted a country
        // profile in its life. That is worth writing down, because it means
        // the disclosure mechanic -- a country choosing to publish its books --
        // changes nothing about how the AI treats it. Only migration reads it.
        //
        // SO IT NO LONGER DOES. This feature respects publication: the real
        // figure when the country published it, and otherwise a guess built
        // from what anyone can see anyway -- its industry and its ground. A
        // country that keeps its books shut is now genuinely harder to read,
        // which is what makes publishing them a decision rather than a tick box
        // that only migration notices.
        //
        // THIS NEEDS A RETRAIN, AND THE SHIPPED MODEL HAS NOT HAD ONE. Feature
        // 3 meant "their treasury" for every model in this repository; under a
        // frozen policy a changed input is a changed language, and the policy
        // reads it as though nothing happened. The measured cost of the switch
        // against the current model is in the changelog. OD_AI_FOG_TREASURY_OFF
        // restores perfect information, which is what a model trained before
        // this expects -- keep it in the trainer's control arm.
        static const bool fogTreasury = std::getenv("OD_AI_FOG_TREASURY_OFF") == nullptr;
        const bool published = oc && g.discloses(ocid, Game::DISCLOSE_TREASURY);
        if (!oc) {
            r[3] = 0.0f;
        } else if (!fogTreasury || published) {
            r[3] = std::tanh((float)oc->treasury / 500.0f);
        } else {
            // A country that publishes nothing is judged on what it cannot
            // hide: factories and ground.
            const float guess = o.industrySum * 8.0f + (float)o.provinces * 4.0f;
            r[3] = std::tanh(guess / 500.0f);
        }
        if (relIt != g.m_relations.end() && oc) {
            auto rr = relIt->second.find(oc->isoA3);
            if (rr != relIt->second.end()) {
                r[4] = rr->second.war ? 1.0f : 0.0f;
                r[5] = rr->second.alliance ? 1.0f : 0.0f;
            }
        }
        // ── ABLATING THE FEATURE, TO BOUND WHAT FOGGING IT CAN COST ──
        //
        // OD_AI_REL_ABLATE_TREASURY=<v> replaces this feature with a constant
        // for every read. If the bench does not move, the policy is not leaning
        // on it and the treasury fog cannot matter to play whatever its read
        // count says; if it does move, that is an UPPER BOUND on the fog, since
        // destroying the feature outright must be at least as damaging as
        // replacing 11% of its reads with a proxy. Cheaper and more direct than
        // a gradient probe, which measures the sensitivity of a logit rather
        // than the effect on play. OD_AI_REL_TREASURY_STATS reports its mean,
        // so the constant can be the feature's own average rather than a number
        // chosen to be flattering.
        {
            static const char* abl = std::getenv("OD_AI_REL_ABLATE_TREASURY");
            // ── WHICH READS TO DESTROY, WHICH IS WHAT MAKES THIS FALSIFIABLE ──
            //
            // Ablating EVERY read bounds what the fog can cost, and that is all
            // it does: after a retrain, a policy that has genuinely learned the
            // mechanic and one that has leaned on the proxy both lose MORE to it
            // -- the first because it now trusts a real figure it can identify,
            // the second because it trusts a guess. One number, two opposite
            // stories, no way to tell them apart.
            //
            // Ablating CONDITIONALLY separates them. A policy that learned the
            // mechanic should be nearly indifferent to losing the fogged reads
            // (it already treats those as a guess) and sensitive to losing the
            // published ones. A policy that leaned on the proxy shows the
            // reverse. OD_AI_REL_ABLATE_WHERE picks: all (default), fogged,
            // published.
            //
            // AND A THIRD OUTCOME, WHICH IS THE LIKELIEST AND READS AS SUCCESS.
            // A policy trained where a feature is a guess one read in six has an
            // easy way out: stop using it. That shows up as the PUBLISHED loss
            // shrinking toward zero, and it is a failure EVEN IF THE RATING GOES
            // UP -- it has thrown away a real figure in the 84% of reads where
            // the figure is true, and bought a better aggregate by knowing less.
            //
            // MEASURED ON TWO POLICIES 65 RATING POINTS APART, and they agree:
            // ablating the published reads costs 5 on both, ablating the fogged
            // reads costs nothing on both, and neither moves survival or the
            // floor at all. So the -5 is a property of what this feature CARRIES
            // rather than of how well a policy uses it, and it can be quoted
            // without naming a model -- which is the opposite of what the a
            // priori argument suggested. That argument still holds in principle:
            // two policies COULD read a feature by different amounts, and a
            // control taken from a different lineage than the child would then
            // report the gap between lineages as a training effect. It just
            // happens not to bite here.
            //
            // It also sharpens the failure modes. If a retrained child loses
            // MUCH more than 5 to the published ablation, that is not "it
            // learned to trust the real figure" -- there appear to be only about
            // five points in the feature to win. It is more likely to have
            // become dependent on r[3] in a way neither of these policies is,
            // which is a fragility rather than a skill.
            static const int ablWhere = [] {
                const char* w = std::getenv("OD_AI_REL_ABLATE_WHERE");
                if (!w) return 0;
                if (std::strcmp(w, "fogged") == 0) return 1;
                if (std::strcmp(w, "published") == 0) return 2;
                return 0;
            }();
            const bool ablate = abl && (ablWhere == 0 ||
                                        (ablWhere == 1 && !published) ||
                                        (ablWhere == 2 && published));
            if (ablate) r[3] = (float)atof(abl);
            if (std::getenv("OD_AI_REL_TREASURY_STATS")) {
                extern double g_r3Sum; extern long long g_r3N;
                g_r3Sum += r[3]; ++g_r3N;
            }
        }
        // ── WAR WEARINESS IS THE ONE NOBODY CAN SEE ──
        //
        // Every other relational feature describes something a player can read
        // off the game: garrisons show in a province panel, industry shows in
        // the industry view, borders are on the map, wars and alliances are
        // listed on a country, claims have their own screen, and a treasury is
        // published or it is not. War weariness is drawn NOWHERE -- there is no
        // caller of warWearinessOf outside the resolver -- so a player cannot
        // learn another country's exhaustion by any amount of clicking, and the
        // AI was reading it exactly.
        //
        // Fogged to the world's mean rather than to a proxy, because unlike the
        // treasury there is no visible quantity to build a proxy FROM. The mean
        // is what "everybody knows the war is dragging on, nobody knows who is
        // closest to breaking" looks like as a number, and for a continuous
        // feature the mean is the neutral value -- see the note on the binary
        // features, where it is not.
        static const bool fogWeariness = std::getenv("OD_AI_FOG_WEARINESS_OFF") == nullptr;
        r[6] = std::tanh((fogWeariness ? meanWeariness : g.warWearinessOf(ocid)) / 5.0f);
        r[7] = (float)std::min(1.0, (double)o.claimsAgainstMe / 4.0);

        // ── THE SAME ABLATION, FOR ANY FEATURE IN THE SLICE ──
        //
        // OD_AI_REL_ABLATE_FEATURE=<0..7> with OD_AI_REL_ABLATE_VALUE=<v>
        // replaces one feature with a constant on every read. The r[3] knobs
        // above are the special case that also knows about publication; this is
        // the general one, for asking which of the eight are load-bearing at
        // all.
        //
        // READ IT ON SURVIVAL AND FLOOR, NOT ON RATING. Ablating r[3] cost 5
        // rating points and moved survival and the floor by nothing, on two
        // policies 65 rating points apart -- and rating is the column that
        // changes sign between world sets. Eight rating numbers would look like
        // a ranking and would not be one. Read on survival and floor the sweep
        // can only say "this input pays for seat competence" or "it does not",
        // and all-zeros is a finding rather than a failed experiment: it would
        // mean the relational slice is not where a strong model's advantage
        // lives.
        //
        // Use each feature's OWN mean as the constant (OD_AI_REL_STATS reports
        // all eight). A shared constant would ablate some of them toward a value
        // they never take, which is a different experiment.
        {
            if (!abl.rotate)
                for (int k = 0; k < REL_FEATURES; ++k)
                    if (abl.on[k]) r[(size_t)k] = abl.to[k];
            if (std::getenv("OD_AI_REL_STATS")) {
                extern double g_relSum[]; extern long long g_relN;
                for (int k = 0; k < REL_FEATURES; ++k) g_relSum[k] += r[(size_t)k];
                ++g_relN;
            }
        }
        cand.push_back(std::move(r));
    }

    // ── PERMUTING A FEATURE, WHICH IS THE ONLY NEUTRAL ABLATION FOR A BIT ──
    //
    // r[4] (at war) and r[5] (allied) are BINARY, and holding a bit at its mean
    // does not remove information -- it injects a falsehood. Their means are
    // just the fraction of pairs at war (0.2267) and allied (0.0370), so
    // holding r[4] there tells the policy "you are 23% at war with everybody",
    // including the country it is actually fighting. That is a different
    // intervention from "you do not know who you are at war with", and it is
    // the one a constant ablation runs. There is no value that encodes
    // ignorance in one bit: a bit can only be ablated to a lie.
    //
    // Rotating the feature across the candidates does encode it. The multiset
    // is preserved exactly -- the policy still sees that it is at war with one
    // of these six -- and only the PAIRING is destroyed, which is the
    // information the feature carries. Deterministic, because a shuffle in a
    // simulation that has to replay identically is not an option, and a
    // rotation by one is a derangement whenever there are two or more
    // candidates.
    //
    // It is the right ablation for the continuous features too, and a stricter
    // one: it removes "which neighbour" while leaving "what the neighbourhood
    // looks like", where a constant removes both.
    if (abl.any && abl.rotate && cand.size() >= 2) {
        const bool trace = std::getenv("OD_REL_DUMP") && m_turn <= 3;
        for (int k = 0; k < REL_FEATURES; ++k) {
            if (!abl.on[k]) continue;
            std::vector<float> before;
            if (trace) for (const auto& cd : cand) before.push_back(cd[(size_t)k]);
            const float first = cand[0][(size_t)k];
            for (size_t i = 0; i + 1 < cand.size(); ++i)
                cand[i][(size_t)k] = cand[i + 1][(size_t)k];
            cand.back()[(size_t)k] = first;
            // BEFORE AND AFTER IN THE SAME RUN. Comparing a rotated run's dump
            // against a separate baseline run cannot check this: the ablation
            // changes decisions, the two runs diverge, and the second is a
            // different world by the time anyone looks. Printed side by side
            // here, the multiset is visibly preserved and only the order moves.
            if (trace) {
                printf("[REL-ROT] t=%d cid=%d r%d:", m_turn, cid, k);
                for (float v : before) printf(" %.3g", v);
                printf("  ->");
                for (const auto& cd : cand) printf(" %.3g", cd[(size_t)k]);
                printf("\n");
            }
        }
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
    if (std::getenv("OD_ACT_HIST")) {
        s_expense[0] += inc.armyExpenses;   s_expense[1] += inc.navyExpenses;
        s_expense[2] += inc.policyCosts;    s_expense[3] += inc.minorityCosts;
        s_expense[4] += inc.researchCost;   s_expense[5] += inc.pacificationCost;
        s_expense[6] += inc.industryUpkeep; s_expense[7] += inc.total;
        ++s_expenseN;
    }

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
    f[35] = (c->treasury >= IND_COST[1]) ? 1.0f : 0.0f;
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

    // 140-142: THE POLITICAL GEOGRAPHY OF OUR OWN COUNTRY. How far the
    // provinces sit from the government on the compass, on average, at the
    // worst, and how much of the country is a long way off. See
    // CountryStat::compassGapMean -- this drives rebel faction formation and
    // was the one thing the politics module could not see while being judged
    // almost entirely on rebellions.
    f[140] = st.compassGapMean;
    f[141] = st.compassGapWorst;
    f[142] = st.compassGapShare;

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
    const DifficultyProfile& d = difficulty();
    temperature = d.temperature;
    // OD_PLAY_TEMP: bench-only override of the sampling temperature at play.
    // Search at play (4/8 sims) turned out to be plain greedy play and scored
    // 32 below the sampled policy (journal 35h), so the temperature is doing
    // work; this is the knob to measure it with. Absolute, 0.01..5.
    if (const char* e = std::getenv("OD_PLAY_TEMP")) {
        const float t = (float)atof(e);
        if (t >= 0.01f && t <= 5.0f) temperature = t;
    }
    epsilon = d.epsilon;
    // OD_PLAY_EPS: bench-only override of the random-action rate at play.
    // The temperature sweep left three seats IDENTICAL across 0.2..0.9 --
    // the policy is peaked enough that sampling rarely leaves the argmax --
    // yet greedy play (search at 4/8 sims, which bypasses this roll too)
    // scored 32 lower. That leaves the epsilon roll as the stochasticity
    // doing the work (journal 35j). Absolute, 0..0.5.
    if (const char* e = std::getenv("OD_PLAY_EPS")) {
        const float x = (float)atof(e);
        if (x >= 0.0f && x <= 0.5f) epsilon = x;
    }
}

const AISystem::DifficultyProfile& AISystem::difficulty() const {
    // Self-play always runs at the top of the ladder: a training opponent that
    // aims with the old rule teaches the aiming heads nothing, and a policy
    // trained against a handicapped version of itself learns to beat the
    // handicap. The exploration schedule above is what supplies noise there.
    const int t = selfPlayLearning()
                      ? DIFFICULTY_SELFPLAY
                      : std::clamp(m_g ? m_g->m_config.aiDifficulty : 2, 0, 3);
    return DIFFICULTY[t];
}

int AISystem::pickAction(NeuralNet& net, const std::vector<float>& feats,
                         const std::vector<bool>& valid, float& scoreOut,
                         int graveAction, const std::vector<float>* logitBias,
                         float* logProbOut, std::vector<float>* neutralProbsOut) {
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

    // ONLY WHERE THERE WAS A CHOICE TO MAKE.
    //
    // A turn on which one action is legal is not evidence about a preference,
    // and this is the second measurement today to be wrong for that shape of
    // reason. "hold" is valid on every turn and is usually the ONLY valid war
    // action, because most country-turns are peaceful -- so averaging P(hold)
    // over every turn it was offered averages in tens of thousands of turns
    // where P(hold) was 1 by arithmetic rather than by opinion. It read as a
    // policy collapsed onto doing nothing. The per-action advantage measured at
    // the same time said the opposite: attack was earning +0.34 against hold's
    // -0.01, on 415 samples against 44,065.
    const bool hadAChoice = validCount >= 2;

    // WHAT THE POLICY THINKS, before temperature has an opinion. Computed here
    // from the same masked logits the choice is about to be made from, and
    // touching nothing: no RNG is drawn and no decision reads it. See
    // TrainStats::warProbMass for why a take rate alone cannot answer this.
    if (neutralProbsOut && hadAChoice)
        NeuralNet::softmax(masked, 1.0f, *neutralProbsOut);

    float temperature, epsilon;
    difficultyParams(temperature, epsilon);
    // The control group ignores the model entirely. Not "mostly random" — a
    // benchmark whose baseline occasionally consults the thing being measured
    // is not a baseline.
    if (m_randomThisCountry) epsilon = 1.0f;

    // The exploration pool, built whether or not this draw explores: the
    // recorded probability below needs it either way.
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

    int a;
    std::uniform_real_distribution<float> d(0.0f, 1.0f);
    if (epsilon > 0.0f && d(m_rng) < epsilon && !pool.empty()) {
        // Deliberately dumb: uniform over the valid actions.
        a = pool[(size_t)(d(m_rng) * pool.size()) % pool.size()];
    } else {
        // Either the policy's turn, or every valid action was grave — fall
        // back to the policy rather than action 0, which would silently make
        // the module inert in exactly the situations that matter most.
        a = NeuralNet::samplePolicy(masked, temperature, m_rng);
    }
    if (a < 0 || a >= (int)logits.size()) a = 0;
    scoreOut = std::isfinite(logits[a]) ? logits[a] : 0.0f;
    // THE PROBABILITY THE SAMPLE WAS ACTUALLY DRAWN AT, not the policy's.
    //
    // Training acts through an epsilon-mixture: with probability epsilon a
    // uniform draw over the pool, otherwise the policy. Recording only the
    // policy's probability handed PPO a lie about how the data was gathered,
    // and the lie is largest exactly on rare actions: an action the policy
    // gives 0.1% is actually PLAYED at ~epsilon/|pool| — tens of times more
    // often — so its samples were over-represented by the same factor, with
    // no importance correction, and the ratchet tightens as the probability
    // shrinks. Whether a head collapsed an action to 0% or to 100% was
    // decided by which sign that amplified gradient happened to carry first,
    // which is why five training runs collapsed in DIFFERENT directions from
    // one seed. Recording the mixture makes the estimator honest again.
    //
    // The policy term uses the masked logits at temperature 1, which is what
    // training plays (difficultyParams pins T=1 while learning); outside
    // training this value trains nothing.
    // The mixture this draw came from, kept for the update. Members rather
    // than out-parameters: every call site would otherwise have to thread two
    // more floats it has no other use for. See Experience::mixScale.
    m_lastMixScale = 1.0f - epsilon;
    m_lastMixFloor = 0.0f;
    {
        const bool inPool = std::find(pool.begin(), pool.end(), a) != pool.end();
        if (inPool && !pool.empty()) m_lastMixFloor = epsilon / (float)pool.size();
    }
    if (logProbOut) {
        const float pPol = std::exp(NeuralNet::logProbOf(masked, a));
        const float pMix = m_lastMixScale * pPol + m_lastMixFloor;
        *logProbOut = std::log(std::max(1e-8f, pMix));
    }
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


// ---------------------------------------------------------------------------
// Ablation harness for the SHIPPED reflexes.
//
// Every reflex below runs unconditionally on every country every turn -- they
// are not candidates, they ARE the AI. Each was justified by a measurement,
// and most of those measurements predate the corrections in
// docs/ai/LOOP_JOURNAL.md: fitted seeds, the 120-turn horizon, and the
// held/par floor magnifier. A rule that earned its place against a bad
// instrument can go on costing indefinitely (see the note on heuristics
// outliving the bug they worked around), and nothing here re-checks them.
//
// OD_ABLATE="fortify,peace" skips the named reflexes for one bench arm, so a
// shipped rule can be priced the same way a candidate is. Unset -- which is
// every non-bench run -- this returns false on the first branch and the play
// path is unchanged.
static bool reflexAblated(const char* name) {
    static const std::string spec = [] {
        const char* e = std::getenv("OD_ABLATE");
        return std::string(e ? e : "");
    }();
    if (spec.empty()) return false;               // the shipped path
    const std::string hay = "," + spec + ",";
    return hay.find(std::string(",") + name + ",") != std::string::npos;
}

void AISystem::takeTurn(int cid) {
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    if (!c) return;
    // Countries with nothing left don't think (eliminated but not yet culled)
    const CountryStat& st = m_stats[cid];
    if (st.provinces == 0) return;

    // What this country's turn costs to think. Charged to whichever cohort it
    // belongs to; see TrainStats::thinkMicros.
    const auto thinkStart = std::chrono::steady_clock::now();
    struct ThinkTimer {
        TrainStats& s;
        std::chrono::steady_clock::time_point t0;
        ~ThinkTimer() {
            s.thinkMicros += std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - t0).count();
            s.thinkCalls++;
        }
    } thinkTimer{statsFor(cid), thinkStart};

    // Dice ONLY when there is no opponent model. With one loaded the control
    // cohort plays that model instead, and leaving this set would have it
    // sampling at random from a frozen brain's logits -- a third policy that is
    // neither of the two files anyone asked to compare.
    // Which brain the control cohort uses. Dice, a frozen model, or the
    // scripted rung -- exactly one of the three, and none of them for the
    // countries being measured.
    // In a duel BOTH sides are scripted: the control cohort attacks, the other
    // turtles, and no net is consulted anywhere.
    // In the tutorial EVERY country is scripted and every one of them
    // turtles: the lesson makes promises about what the neighbours will do,
    // and a learned policy has never agreed to keep them.
    m_scriptedThisCountry = s_tutorialAI || s_scriptDuel ||
                            (isRandomCountry(cid) && s_scriptedControl) ||
                            // The league slot is the rusher: its countries play
                            // the script, so they must NOT also be treated as
                            // league countries below (there are no weights).
                            (m_leagueIsExploiter && m_leagueCids.count(cid) > 0);
    // An exploit variant, when one is selected, replaces the ordinary rung for
    // the control cohort -- the model cohort is never scripted. See
    // ScriptVariant and --vs-exploit.
    // ── THE OPPONENT THE POLICY NEVER MET ──
    //
    // Until this, every scripted country outside --vs-exploit played
    // SCRIPT_AGGRESSOR, so across an entire training run the policy faced
    // exactly one strategy. It shows: benched at Insane it holds 72.8% of the
    // land against the tech rush, 75.1% against the naval one and 69.0%
    // against the pact hub -- and 29.0% against SCRIPT_BLITZ, the opponent that
    // never stops fighting and never makes peace. It is not that the blitz is
    // unanswerable; it is that nothing in training ever asked the question.
    //
    // Note what this is NOT: it is not the control cohort getting stronger for
    // its own sake. AI_TRAINING_VARIANT_SHARE keeps most scripted countries on
    // the ordinary rung, because a control cohort that mostly blitzes would
    // move the reward landscape rather than widen it. A quarter is enough for
    // the policy to meet one regularly without the run becoming a different
    // experiment.
    //
    // Deterministic in cid alone, which is what makes it STABLE: a country has
    // to play the same strategy for the whole game, and hashing in anything
    // that moves would have it turn from a turtle into a blitz halfway through.
    // Different maps bring different country sets, so the mix still varies
    // across the rotation. TRAINING ONLY --
    // evaluation must keep facing the rung it has always been measured against,
    // or every number in this file stops being comparable to the ones above it.
    int trainingVariant = SCRIPT_AGGRESSOR;
    if (selfPlayLearning() && s_exploitVariant < 0 && isRandomCountry(cid)) {
        const uint32_t h = (uint32_t)cid * 2654435761u;
        if ((h % 100u) < (uint32_t)(AI_TRAINING_VARIANT_SHARE * 100.0f)) {
            static constexpr int MIX[] = {SCRIPT_BLITZ, SCRIPT_TECH,
                                          SCRIPT_DIPLO, SCRIPT_NAVY, SCRIPT_TURTLE};
            trainingVariant = MIX[(h / 100u) % (sizeof(MIX) / sizeof(MIX[0]))];
        }
    }
    // ...and, when the exploit is scoped to a neighbourhood, only for the
    // countries in it. See s_exploitCids.
    const bool exploitHere =
        s_exploitVariant >= 0 && isRandomCountry(cid) &&
        (s_exploitCids.empty() || s_exploitCids.count(cid) > 0);
    const int scriptVariant =
        exploitHere ? s_exploitVariant
      : (s_tutorialAI || (s_scriptDuel && !isRandomCountry(cid))) ? SCRIPT_TURTLE
                                                                  : trainingVariant;
    m_randomThisCountry = isRandomCountry(cid) && !m_opponentLoaded &&
                          !m_scriptedThisCountry;
    // A league country acts with a frozen past policy and teaches nothing.
    // NOT when the slot is the exploiter: those countries play SCRIPT_BLITZ and
    // there are no league weights to reach for. See m_leagueIsExploiter.
    m_leagueThisCountry = !m_leagueIsExploiter && m_leagueCids.count(cid) > 0;

    Experience exp;
    buildFeatures(cid, exp.features);
    exp.provinces = st.provinces;
    exp.treasury = c->treasury;
    exp.army = st.army;
    exp.ships = st.boats + st.destroyers + st.carriers;
    // INCOME, NOT ABSTINENCE. See the note on dNet in the reward: research
    // spending is added back, so declining to invest does not read as earning
    // more.
    {
        const CountryIncomeSnapshot ci = g.computeCountryIncome(cid);
        exp.netIncome = ci.net + ci.researchCost;
        {
            TrainStats& ps = statsFor(cid);
            ps.purseTreasury += c->treasury;
            ps.purseGross    += ci.total;
            ps.purseNet      += ci.net;
            ps.purseUpkeep   += ci.policyCosts + ci.minorityCosts + ci.pacificationCost;
            ps.upkeepPolicy   += ci.policyCosts;
            ps.upkeepMinority += ci.minorityCosts;
            ps.upkeepPacify   += ci.pacificationCost;
            ps.expArmy        += ci.armyExpenses;
            ps.expNavy        += ci.navyExpenses;
            ps.expIndustry    += ci.industryUpkeep;
            ps.expResearch    += ci.researchCost;
            ps.purseTurns++;
            if (ci.net < 0.0f)    ps.purseNetNegative++;
            if (c->treasury < 8.0) ps.purseBroke++;
        }
        // The standing political bill as the window opens. See Experience.
        exp.polUpkeep = ci.policyCosts + ci.minorityCosts + ci.pacificationCost;
    }
    exp.industrySum = st.industrySum;
    exp.threatened = st.threatenedProvinces;
    exp.enemyAdjArmy = st.enemyAdjArmy;   // the bar armyTerm measures against
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
    // Standing at decision time, against which the window's standing is scored.
    // m_world is rebuilt in beginTurn, so this is current.
    exp.worldRank  = m_world.rankOf(cid);
    exp.ownShare   = m_world.shareOf(st.provinces);
    exp.rivalShare = m_world.rivalShareFor(cid);

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
    // A league country picks one too, WHEN IT BROUGHT A HEAD TO PICK WITH.
    //
    // A checkpoint from the rotation carries a trunk and the policy heads and
    // nothing else, so a frozen training opponent still holds no posture. An
    // opponent loaded from a full model file under --vs-model does have one,
    // and has to use it: the stance now steers action selection, so a side
    // without one is a side playing a different game, and a head-to-head where
    // only the challenger gets a posture is not measuring the challenger.
    //
    // Its experience is discarded at the end of this function either way, so
    // writing the choice into `exp` costs nothing and keeps one code path.
    const bool pickStance = !m_leagueThisCountry || m_leagueStanceLoaded;
    if (pickStance) {
        auto stIt = m_stance.find(cid);
        const bool due = stIt == m_stance.end() ||
                         (m_turn - stIt->second.second) >= STANCE_WINDOW;
        if (due) {
            std::vector<bool> anyStance((size_t)STANCE_COUNT, true);
            float sScore = 0.0f;
            float sLogProb = 0.0f;
            const int sc = pickAction(m_leagueThisCountry ? m_leagueStance
                                                          : m_stanceHead,
                                      emb, anyStance, sScore,
                                      /*graveAction=*/-1, nullptr, &sLogProb);
            m_stance[cid] = {sc, m_turn};
            exp.action[MOD_COUNT + 1] = sc;
            exp.acted[MOD_COUNT + 1]  = true;
            exp.logProb[MOD_COUNT + 1] = sLogProb;
        }
    }

    // The posture in force, applied to every module below. See STANCE_BIAS:
    // this is the only thing that makes the stance a decision rather than a
    // note to self. -1 when no stance has been chosen yet (the first window of
    // a country's life, and every league country, which has no stance head).
    const int stance = stanceOf(cid);
    if (stance >= 0 && stance < STANCE_COUNT) statsFor(cid).stanceHeld[stance]++;

    std::vector<float> qbias;
    auto qBiasFor = [&](int m) -> const std::vector<float>* {
        const int nActs = (m == MOD_ECONOMY) ? ECON_ACTIONS
                        : (m == MOD_POLITICS) ? POL_ACTIONS
                        : (m == MOD_WAR) ? WAR_ACTIONS : NAVY_ACTIONS;
        qbias.assign((size_t)nActs, 0.0f);
        bool any = false;

        // ── The posture's lean ──
        // Off below Hard: a country without a plan is one of the faculties the
        // ladder takes away. See DifficultyProfile.
        if (difficulty().usePosture && stance >= 0 && stance < STANCE_COUNT) {
            const float* row = (m == MOD_ECONOMY) ? STANCE_ECON[stance]
                             : (m == MOD_POLITICS) ? STANCE_POL[stance]
                             : (m == MOD_WAR) ? STANCE_WAR[stance]
                                              : STANCE_NAVY[stance];
            for (int i = 0; i < nActs; ++i)
                if (row[i] != 0.0f) { qbias[i] += STANCE_BIAS * row[i]; any = true; }
        }

        // ── STAGING, WHICH THE POLICY WILL NOT CHOOSE ── See s_warStageBias.
        // Applied to the war module's action 7 only, and only where the mask
        // has already said a crossing exists, so this can never invent an
        // opportunity — it can only stop the policy declining one it has been
        // measured to profit from.
        if (m == MOD_WAR) {
            for (int i = 0; i < nActs && i < 8; ++i)
                if (s_warBias[i] != 0.0f) { qbias[i] += s_warBias[i]; any = true; }
        }
        if (m == MOD_NAVY) {   // see s_navyBias
            for (int i = 0; i < nActs && i < 7; ++i)
                if (s_navyBias[i] != 0.0f) { qbias[i] += s_navyBias[i]; any = true; }
        }

        // ── THE COALITION DECLARES TOGETHER, OR IT IS NOT ONE ──
        //
        // Forming up (updateCoalition) only makes the declaration LEGAL for a
        // member -- the bar is measured against the combined army. Whether it
        // is actually made is still the war head's call, and a head that comes
        // round to it independently over the next sixty turns reproduces
        // exactly the drip this replaces: the leader fights its neighbours one
        // at a time, which is how it got large.
        //
        // So while the window is open, members lean toward declaring. 4 is
        // declare war; see COALITION_WINDOW.
        if (m == MOD_WAR && inCoalition(cid) && WAR_ACTIONS > 4) {
            qbias[4] += COALITION_WEIGHT * coalitionPressure();
            any = true;
        }

        // ── What this country's own advisor has asked for ──
        //
        // The optional language-model module, and exactly 0.0f without it --
        // llmIntentFor checks the module itself rather than trusting callers,
        // so a game with no advisor takes no branch a game without one would
        // not also take. See AI_LLM_INTENT for what 0.25 buys at this
        // temperature: it leans a close call and cannot move a settled one.
        //
        // SCOPE: this reaches actions the policy SAMPLES. The reflexes below
        // never consult the net, and are leaned separately by suppression --
        // see llmSuppressesReflex, which is capped at one.
        for (int i = 0; i < nActs; ++i) {
            const float want = m_g->llmIntentFor(cid, m, i);
            if (want == 0.0f) continue;
            qbias[i] += AI_LLM_INTENT * want;
            any = true;
        }

        // ── The critic's opinion ──
        // A frozen opponent is exactly the policy it was checkpointed as.
        // Letting the CURRENT critic re-rank its actions would make it a
        // moving target again, which is the one thing the league exists to
        // prevent. The stance lean above is NOT skipped for it: that is a rule
        // of the game rather than a judgement of ours, and a frozen policy
        // played under it too.
        // ── THE SEARCH'S OPINION ──
        // A frozen league opponent is exactly the policy it was checkpointed
        // as; letting the CURRENT forward model re-rank its actions would make
        // it a moving target again, which is the one thing the league exists to
        // prevent. Same rule the critic answers to below.
        if (!m_leagueThisCountry && searchReady()) {
            std::vector<float> ss;
            // `emb`, the turn-opening embedding -- the same one the Q head
            // below is evaluated on. Later picks in a module see a state the
            // trunk has re-read (curEmb in runModule), and feeding that here
            // instead would be more accurate for both; it is left alone
            // because it would change what the shipped critic does, and that
            // is a separate change with its own measurement.
            searchScores(m, emb, valid, ss);
            for (size_t i = 0; i < ss.size() && i < qbias.size(); ++i) {
                qbias[i] += ss[i];
                any = true;
            }
        }

        if (Q_BLEND > 0.0f && difficulty().useCritic && !m_leagueThisCountry &&
            m_q[m].updateCount() >= qWarmup()) {
            const std::vector<float>& q = m_q[m].forward(emb);
            if (!q.empty()) {
                float mean = 0.0f;
                for (float v : q) mean += v;
                mean /= (float)q.size();
                for (size_t i = 0; i < q.size() && i < qbias.size(); ++i) {
                    qbias[i] += Q_BLEND * (q[i] - mean);
                    any = true;
                }
            }
        }
        return any ? &qbias : nullptr;
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

    // Only where a net is driving: the random cohort's choices are dice and the
    // scripted rung never consults a net, so a distribution recorded for either
    // would describe a policy that had no say in what they did.
    const bool netDriven = !m_randomThisCountry && !m_scriptedThisCountry;
    std::vector<float> nprob;
    int a = 0;

    // ── One module's turn: keep acting until it passes ──
    //
    // See Experience::extras and ACTIONS_PER_MODULE_PER_TURN. The mask is
    // recomputed before every pick, so a choice sees the money the previous one
    // spent; action 0 is the always-valid pass and ends the module's turn.
    // Everything a module does shares the window's reward, exactly as the
    // single action used to.
    auto runModule = [&](int mod, int graveAction) {
        // Each pick after the first re-reads the world and re-runs the trunk:
        // the previous pick spent money, moved troops and queued orders, and a
        // decision made blind to that is both chosen wrong and trained wrong.
        // See Experience::ExtraAction::features.
        std::vector<float> curFeat;
        std::vector<float> curEmb;
        // ── POLITICS IS RATIONED, AND THE OTHER THREE ARE NOT ──
        //
        // Measured 2026-09-02 on 1914 over 25 deterministic turns, by
        // throttling one module at a time back to three actions and watching
        // the bankruptcy cascade: politics is the whole of it. Capping it
        // alone takes bankruptcy events 74 -> 45, the countries that go broke
        // 32 -> 24, rebel states 133 -> 109, and stops the engine's rebellion
        // ceiling being tripped at all. Capping ECONOMY makes it worse
        // (74 -> 83), war and navy are noise.
        //
        // What lives here is `conciliate a minority`, executed 3,158 times in
        // those 25 turns -- more than any other action in the game, more than
        // research fund-up at 1,937, six times every economy purchase put
        // together -- against `repress`, the step that gives the money back,
        // executed ZERO times. Every step moves a group to a dearer option and
        // adds its costPerTurn for ever, and the minority bill is 9.9 of a
        // gross of 41.7 per country-turn, second only to research. Unlike
        // research, NOTHING scales it down when it stops being affordable:
        // computeCountryIncome clamps the research and pacification sliders to
        // what is left, while this is charged in full, drives net income
        // negative, and hands the shortfall to enforceAusterity -- whose third
        // act is to revoke those same settlements, which is what the unrest
        // spike and the rebellion are made of.
        //
        // Rationing rather than gating, and that distinction is measured. An
        // affordability test on conciliation is a POVERTY TRAP with an extra
        // failure mode: the country that most needs to placate its minorities
        // is the one that cannot pay, so denying it merely converts a money
        // problem into an unrest one -- 0.40 of gross as the threshold gave 83
        // events and 140 rebel states, worse than doing nothing. A per-country
        // QUOTA is no better and for the same reason: three steps a turn makes
        // very nearly the same number of conciliations as this cap (1,044
        // against 1,064) and yields 197 rebel states against 109, because a
        // flat quota starves the country in trouble while a calm one spends
        // its allowance. Only the module cap allocates by need, because the
        // head is still the thing choosing -- it just has to choose between
        // conciliating and everything else politics can do.
        //
        // This does not restore the throttle that was removed. The other three
        // modules keep the wide budget and the competence that came with it;
        // and politics was already the one module with a rate limit in it, see
        // AI_REQUESTS_PER_TURN.
        const int budget = (mod == MOD_POLITICS) ? ACTIONS_PER_MODULE_PER_TURN
                                                 : actionsPerModule(cid);
        for (int k = 0; k < budget; ++k) {
            // The previous pick spent money, laid down a hull or signed a
            // settlement, and computeCountryIncome would otherwise answer this
            // one out of a cache written for the HUD. Measured on 1914: without
            // this, a country asked eight times whether it could afford a
            // recurring bill and was told eight times that it could afford the
            // FIRST one. See Game::invalidateIncomeCache.
            m_g->invalidateIncomeCache();
            if (k > 0) {
                buildFeatures(cid, curFeat);
                curEmb = m_leagueThisCountry ? m_leagueTrunk.forward(curFeat)
                                             : m_trunk.forward(curFeat);
            }
            const std::vector<float>& useEmb = (k == 0) ? emb : curEmb;
            switch (mod) {
                case MOD_ECONOMY:  validEconomy(cid, valid);  break;
                case MOD_POLITICS: validPolitics(cid, valid); break;
                case MOD_WAR:      validWar(cid, valid);      break;
                default:           validNavy(cid, valid);     break;
            }
            // OD_ACT_HIST also counts what the MASK offered, which separates
            // "the policy never wants this" from "the policy is never asked".
            // An action at 0% of picks means nothing until you know whether it
            // was on the menu.
            {
                static const bool offHist = std::getenv("OD_ACT_HIST") != nullptr;
                if (offHist && mod >= 0 && mod < MOD_COUNT)
                    for (int a = 0; a < MAX_MODULE_ACTIONS && a < (int)valid.size(); ++a)
                        if (valid[a]) s_offHist[mod][a]++;
            }
            float lp = 0.0f;
            nprob.clear();
            // THE OPENING BOOK. See AI_OPENING_TURNS: the first turns are
            // played from the script by everyone, so training and play meet the
            // same midgame. The choice is still recorded below, so the policy
            // learns the book rather than merely being overridden by it.
            const bool inBook = (m_turn < openingTurns()) && !m_leagueThisCountry;
            // ── SEARCH, WHEN IT IS ON ──
            // The visit distribution replaces the policy as the BEHAVIOUR
            // policy, so `lp` must be its log-probability: PPO's ratio is
            // measured against whatever actually chose the action, and
            // recording the raw policy's log-prob here would make every
            // searched sample look off-policy by exactly the amount the search
            // improved it. Root noise only while learning.
            std::vector<float> visits;
            if (!m_scriptedThisCountry && !inBook && !m_leagueThisCountry &&
                mod >= 0 && mod < MOD_COUNT && mctsSims() > 0)
                mctsPolicy(mod, useEmb, valid, visits, g.m_config.aiLearning);
            int act;
            if (!visits.empty() && mod >= 0 && mod < MOD_COUNT)
                exp.visits[mod] = visits;      // the policy target; see Experience
            if (!visits.empty()) {
                double tot = 0.0;
                for (int a = 0; a < (int)visits.size(); ++a)
                    if (a >= (int)valid.size() || valid[a]) tot += visits[(size_t)a];
                double r = tot * std::uniform_real_distribution<double>(0.0, 1.0)(m_rng);
                act = -1;
                for (int a = 0; a < (int)visits.size(); ++a) {
                    if (a < (int)valid.size() && !valid[a]) continue;
                    r -= visits[(size_t)a];
                    if (r <= 0.0) { act = a; break; }
                }
                if (act < 0) act = 0;
                const double p = tot > 0.0 ? visits[(size_t)act] / tot : 1.0;
                lp = (float)std::log(std::max(1e-8, p));
                score = visits[(size_t)act];
            } else {
                act = (m_scriptedThisCountry || inBook)
                    ? scriptedChoice(mod, cid, valid, scriptVariant,
                                     /*bookTurn=*/inBook && !m_scriptedThisCountry)
                    : pickAction(brainFor(mod), useEmb, valid, score, graveAction,
                                 qBiasFor(mod), &lp, netDriven ? &nprob : nullptr);
            }
            // A booked move was not sampled from the policy AT ALL: the book
            // is deterministic, so the behaviour probability is 1 and the
            // importance ratio pi_new/b carries none of the pi_old/pi_new the
            // surrogate is built from.
            //
            // This previously recorded the POLICY's own log-prob and let the
            // sample into PPO as though the policy had chosen it. It cannot be
            // patched by writing lp = 0 either -- that makes the ratio
            // pi_new(a), which drives every book action's probability toward 1
            // regardless of what it was worth. The sample is simply not a
            // policy-gradient sample, so it is marked and the surrogate skips
            // it; its value target, its Q target and its demonstration value
            // are all still used. See Experience::fromBook.
            const bool booked = inBook && !m_scriptedThisCountry;
            if (booked) { lp = 0.0f; m_lastMixScale = 1.0f; m_lastMixFloor = 0.0f; }
            // ACCUMULATED ONLY WHERE THE ACTION WAS OFFERED, so the denominator
            // is the take rate's denominator and the two are directly
            // comparable. Averaged over every decision instead, an action that
            // is rarely valid reads as near-zero probability purely because it
            // was masked out most turns -- which is a fact about the mask, not
            // about the policy.
            if (netDriven && !nprob.empty() && (mod == MOD_ECONOMY || mod == MOD_WAR)) {
                TrainStats& ps = statsFor(cid);
                const int nAct = (mod == MOD_ECONOMY) ? ECON_ACTIONS : WAR_ACTIONS;
                if (mod == MOD_ECONOMY) ps.econDecisions++; else ps.warDecisions++;
                for (int i = 0; i < nAct && i < (int)nprob.size(); ++i)
                    if (i < (int)valid.size() && valid[i]) {
                        if (mod == MOD_ECONOMY) { ps.econProbMass[i] += nprob[i]; ps.econProbN[i]++; }
                        else                    { ps.warProbMass[i]  += nprob[i]; ps.warProbN[i]++;  }
                    }
            }
            {
                TrainStats& ts = statsFor(cid);
                if (mod == MOD_ECONOMY) {
                    for (int i = 0; i < ECON_ACTIONS; ++i)
                        if (i < (int)valid.size() && valid[i]) ts.econOffered[i]++;
                    if (act >= 0 && act < ECON_ACTIONS) ts.econChosen[act]++;
                    if (act >= 9 && act <= 11) ts.researchPicked++;
                } else if (mod == MOD_WAR) {
                    for (int i = 0; i < WAR_ACTIONS; ++i)
                        if (i < (int)valid.size() && valid[i]) ts.warOffered[i]++;
                    if (act >= 0 && act < WAR_ACTIONS) ts.warChosen[act]++;
                } else if (mod == MOD_POLITICS) {
                    for (int i = 0; i < POL_ACTIONS; ++i)
                        if (i < (int)valid.size() && valid[i]) ts.polOffered[i]++;
                    if (act >= 0 && act < POL_ACTIONS) ts.polChosen[act]++;
                } else {
                    for (int i = 0; i < NAVY_ACTIONS; ++i)
                        if (i < (int)valid.size() && valid[i]) ts.navyOffered[i]++;
                    if (act >= 0 && act < NAVY_ACTIONS) ts.navyChosen[act]++;
                }
            }
            // The first pick keeps the scalar slots so nothing downstream has to
            // learn about extras; the rest become their own training samples.
            // ── THE TEACHER'S ANSWER, when cloning is on ──
            //
            // Asked for the SAME state and the SAME mask the policy just chose
            // under, because the teacher can only be asked while the world
            // still stands where the decision stood. RECORDED, not applied:
            // the gradient goes in with the batch, next to the policy
            // gradient, at one gradient per sample. Applying it here instead --
            // an immediate weight update per decision, ~247k per map -- is what
            // made cloning the entire objective and collapsed the war head.
            // See BC_LR for the measurement.
            //
            // Skipped for the scripted and league cohorts: one is already the
            // teacher and the other is frozen on purpose.
            int teach = -1;
            if ((s_bcWeight > 0.0f || s_bcObserve) && selfPlayLearning() &&
                !m_scriptedThisCountry && !m_leagueThisCountry &&
                bcCloneModule(mod)) {
                const int t = scriptedChoice(mod, cid, valid, scriptVariant);
                if (t >= 0 && t < (int)valid.size() && valid[t]) {
                    // Counted whether or not it is applied -- see s_bcObserve.
                    statsFor(cid).bcSamples++;
                    if (t == act) statsFor(cid).bcAgreed++;
                    if (s_bcWeight > 0.0f) teach = t;
                }
            }
            // A booked move is a demonstration whether or not cloning is on --
            // the book IS the teacher for those turns, and it is the only thing
            // those samples can now teach the policy head.
            if (booked && teach < 0 && act >= 0 && act < (int)valid.size() && valid[act])
                teach = act;
            // ── THE MARGINAL THE COLLAPSE GUARD READS ──
            //
            // Only the learning policy's own choices. A booked turn is the
            // script's answer, and the scripted and league cohorts are not this
            // policy at all; counting any of them would let a healthy-looking
            // marginal be produced entirely by players that are not learning.
            // THE POLICY'S OWN MASS, not the action that was played. Counting
            // played actions was tried and does not work: training explores
            // through an epsilon mixture, so the collapsed model's war head --
            // which the bench shows choosing `hold` 100.00% of the time at
            // eval -- still produced a healthy-looking marginal of 1.37 here,
            // because most of that variety was exploration rather than policy.
            // nprob is the masked softmax at temperature 1, so this measures
            // pi(a) = E_s[pi(a|s)]: what the model would do if nothing were
            // added to it, which is exactly what ships.
            if (netDriven && !nprob.empty() && !booked && !m_scriptedThisCountry &&
                !m_leagueThisCountry && mod >= 0 && mod < MOD_COUNT) {
                for (size_t vi = 0; vi < valid.size() && vi < MAX_MODULE_ACTIONS; ++vi)
                    if (valid[vi]) {
                        m_marginalOffered[mod][vi] += 1.0;
                        if (vi < nprob.size()) m_marginalChosen[mod][vi] += nprob[vi];
                    }
            }

            // The class balance the weighting reads, counted at decision time
            // so it reflects what the teacher was actually asked -- including
            // the book, which is the densest teacher in the run and would
            // otherwise skew the balance it is not counted in.
            if (teach >= 0 && mod >= 0 && mod < MOD_COUNT && teach < MAX_MODULE_ACTIONS) {
                m_teacherCount[mod][teach] += 1.0;
                m_teacherTotal[mod]        += 1.0;
            }

            // The mask travels with the sample: the update must renormalise
            // over the same support the decision saw. See Experience::validMask.
            std::vector<uint8_t> maskCopy(valid.size());
            for (size_t vi = 0; vi < valid.size(); ++vi) maskCopy[vi] = valid[vi] ? 1 : 0;
            if (k == 0) {
                m_policy[mod].snapshotActs(exp.acts[mod]);
                exp.action[mod] = act; exp.acted[mod] = true; exp.logProb[mod] = lp;
                exp.validMask[mod] = std::move(maskCopy);
                exp.mixScale[mod] = m_lastMixScale;
                exp.mixFloor[mod] = m_lastMixFloor;
                exp.teacher[mod]  = teach;
                exp.fromBook[mod] = booked;
            } else {
                Experience::ExtraAction ea;
                ea.module = mod; ea.action = act; ea.logProb = lp;
                ea.features = curFeat;          // the state THIS action saw
                ea.validMask = std::move(maskCopy);
                ea.mixScale = m_lastMixScale;
                ea.mixFloor = m_lastMixFloor;
                ea.teacher = teach;
                ea.fromBook = booked;
                m_policy[mod].snapshotActs(ea.acts);
                exp.extras.push_back(std::move(ea));
            }
            a = act;

            std::string label;
            m_execNoop = false;
            // Asking somebody for something is the rate-limited kind of action;
            // see AI_REQUESTS_PER_TURN. Counted before the executor runs so a
            // refusal still spends the request -- the cost is the asking.
            if (mod == MOD_POLITICS && (act == 5 || act == 6 || act == 7 || act == 11))
                m_requestsThisTurn[cid]++;
            // OD_ACT_HIST=1: how often each module actually picks each action,
            // printed at process exit. The claim that most of the econ head is
            // dead has never been re-measured on the current game, and a head
            // that only ever returns two of twelve actions makes every rule
            // built on the others silently inert.
            static const bool actHist = std::getenv("OD_ACT_HIST") != nullptr;
            if (actHist && mod >= 0 && mod < MOD_COUNT &&
                act >= 0 && act < MAX_MODULE_ACTIONS) {
                static const bool reg = (atexit(&AISystem::dumpActionHistogram), true);
                (void)reg;
                s_actHist[mod][act]++;
            }
            switch (mod) {
                case MOD_ECONOMY:  label = execEconomy(cid, act);  break;
                case MOD_POLITICS: label = execPolitics(cid, act); break;
                case MOD_WAR:      label = execWar(cid, act);      break;
                default:           label = execNavy(cid, act);     break;
            }
            // OD_ECON_TRACE=<cid>: every module decision that country makes.
            {
                static const int traceCid = std::getenv("OD_ECON_TRACE") ? atoi(std::getenv("OD_ECON_TRACE")) : -1;
                if (traceCid == cid)
                    fprintf(stderr, "[ACTION] turn %d cid=%d %s %d -> %s\n", m_turn, cid,
                            mod == MOD_ECONOMY ? "econ" : mod == MOD_POLITICS ? "politics" : mod == MOD_WAR ? "war" : "navy",
                            act, label.empty() ? "(pass)" : label.c_str());
            }
            // Action 0 is the pass; doing nothing IS what it is for.
            if (m_execNoop && act > 0 && act < MAX_MODULE_ACTIONS &&
                mod >= 0 && mod < MOD_COUNT)
                statsFor(cid).noopChosen[mod][act]++;
            if (mod == MOD_WAR) {
                // Captured AFTER execWar, because that is what runs the chooser.
                // A declaration that fell through to the old rule leaves this
                // empty and simply trains nothing. Only the FIRST declaration or
                // assault of the turn is recorded: Experience carries one
                // candidate set, and a turn that both declares and assaults is
                // rare enough that a second set is not worth the width.
                if (m_pendingTargetChosen >= 0 && exp.targetChosen < 0) {
                    exp.targetCand = std::move(m_pendingTargetCand);
                    exp.targetChosen = m_pendingTargetChosen;
                }
                m_pendingTargetCand.clear();
                m_pendingTargetChosen = -1;
                if (m_pendingAttackChosen >= 0 && exp.attackChosen < 0) {
                    exp.attackCand = std::move(m_pendingAttackCand);
                    exp.attackChosen = m_pendingAttackChosen;
                }
                m_pendingAttackCand.clear();
                m_pendingAttackChosen = -1;
            }
            logDecision(cid, mod, act, score, label);
            if (act == 0) break;   // passed: this module is done for the turn
        }
    };

    // ── WHERE THE MONEY WENT. See TrainStats::spendEcon ──
    // Sampled at the module boundaries rather than inside the executors, so it
    // catches every path that moves the treasury -- including the reflexes
    // below, which are billed to the module they run before.
    double cashMark = c->treasury;
    {
        TrainStats& ps = statsFor(cid);
        ps.spendTurns++;
        runModule(MOD_ECONOMY, /*graveAction=*/-1);
        ps.spendEcon += cashMark - c->treasury;  cashMark = c->treasury;
        runModule(MOD_POLITICS, /*graveAction=*/-1);
        ps.spendPol  += cashMark - c->treasury;  cashMark = c->treasury;
    }

    // Defence runs before the sampled war action, unconditionally. See the
    // note on garrisonReflex: holding a threatened border is not a choice the
    // policy should be gambling on once every eight turns.
    static const bool actHistOn = std::getenv("OD_ACT_HIST") != nullptr;
    if (actHistOn) {
        const auto raIt = g.m_countryResearchAllocation.find(cid);
        if (raIt != g.m_countryResearchAllocation.end()) {
            s_researchSum += raIt->second; ++s_researchN;
        }
    }
    if (!reflexAblated("garrison") && !m_g->llmSuppressesReflex(cid, "garrison"))
        garrisonReflex(cid);
    if (!reflexAblated("fortify") && !m_g->llmSuppressesReflex(cid, "fortify"))
        fortifyReflex(cid);
    // Peacetime housekeeping, same reasoning: neither of these is a gamble.
    if (!reflexAblated("redeploy") && !m_g->llmSuppressesReflex(cid, "redeploy"))
        redeployReflex(cid);
    // Solvency before manpower: austerity cuts things that come back, the
    // manpower reflex cuts men who do not.
    if (!reflexAblated("austerity") && !m_g->llmSuppressesReflex(cid, "austerity"))
        austerityReflex(cid);
    if (!reflexAblated("manpower") && !m_g->llmSuppressesReflex(cid, "manpower"))
        manpowerReflex(cid);
    if (!reflexAblated("siege") && !m_g->llmSuppressesReflex(cid, "siege"))
        siegeReflex(cid);
    researchAusterityReflex(cid);
    industryReflex(cid);
    navalReflex(cid);
    if (!reflexAblated("campaign") && !m_g->llmSuppressesReflex(cid, "campaign"))
        campaignReflex(cid);
    // ...and end the wars it is not about
    if (!reflexAblated("peace") && !m_g->llmSuppressesReflex(cid, "peace"))
        peaceReflex(cid);
    if (!reflexAblated("pacification") && !m_g->llmSuppressesReflex(cid, "pacification"))
        pacificationReflex(cid);
    if (!reflexAblated("withdraw") && !m_g->llmSuppressesReflex(cid, "withdraw"))
        withdrawReflex(cid);
    if (!reflexAblated("callToArms")) callToArmsReflex(cid);
    // Finish any crossing already under way. Runs BEFORE the navy action is
    // sampled so that action sees the move orders it has already issued.
    amphibiousReflex(cid);

    m_declaredUnprovoked = false;
    // 4 = declare war; see the graveAction note in pickAction.
    {
        TrainStats& ps = statsFor(cid);
        // Taken AFTER the reflexes above, because that is the money the war
        // head is actually offered -- austerityReflex in particular can move
        // the treasury between the boundary above and this pick.
        ps.cashAtWar += c->treasury;
        if (c->treasury >= 8.0) ps.warRich++;
        cashMark = c->treasury;
        runModule(MOD_WAR, /*graveAction=*/4);
        const double spent = cashMark - c->treasury;
        if (spent > 0.0) ps.warSpent++;
        ps.spendWar += spent;  cashMark = c->treasury;
    }
    exp.aggressor = m_declaredUnprovoked;

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
    {
        TrainStats& ps = statsFor(cid);
        runModule(MOD_NAVY, /*graveAction=*/-1);
        ps.spendNavy += cashMark - c->treasury;
    }

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
    // Gated on the DISCOUNTED price, or the mask forbids a build the country
    // could actually afford -- which with a finished tree is most of them.
    const float indMod = buildCostMod(m_g->getTotalEffect("industryCostPct", cid));
    // Split into "could this country do it at all" and "can it pay", so the
    // report can tell a head that has stopped choosing from an action the
    // treasury never lets onto the menu. See TrainStats::econCashBlocked.

    // ── A SAVINGS RESERVE WAS TRIED HERE, AND MEASURED, AND REMOVED ──
    //
    // The reasoning still looks right. An AI country's mean net income is
    // +11.9 a turn and only 7% of country-turns run at a loss, yet 64% of them
    // hold under $8: the money is not missing, it is spent the instant there is
    // enough for the cheapest thing on the menu, which is always the next
    // industry level at $8. For income that greed is very nearly correct -- a
    // level costs IND_COST and returns 2 a turn, so the cheapest rung has the
    // shortest payback. It is exactly wrong for CAPABILITY: a port is $60 and
    // returns no income at all, so it loses every comparison against a factory
    // for ever -- and a port at level 2 is what unlocks a destroyer, and level
    // 3 a carrier. The whole naval economy sits behind a purchase the country
    // never happens to have the money for.
    //
    // So the country was allowed to hold money back: if a harbour was reachable
    // inside AI_PLAN_HORIZON turns at its projected income (Game::projectIncome),
    // that price was reserved and the cheap builds could not touch it. Pure
    // arithmetic, no forecast, and it did exactly what it was asked to do --
    // 1,100 cheap builds withheld over a 100-turn map, and the port action
    // offered on 716 decisions instead of being priced out.
    //
    // THE POLICY CHOSE IT FOUR TIMES. 0.6%. Ports built over the run: zero,
    // against +1,180 industry levels where the unreserved run built +2,007. A
    // mask can create an opportunity; it cannot make a collapsed head take one,
    // and all the reserve achieved was to stop the economy buying the thing it
    // does want while it waited for a decision that never came.
    //
    // The blocker is therefore the policy and not the purse, which is worth
    // knowing precisely: it is why the fix for ports is a retrain on the
    // corrected action space (the research deadlock that capped every country
    // at port level 1 was only just lifted, so no model has ever been trained
    // in a world where port level 2 was reachable), and not another rule here.
    // Do not re-add this without first checking that econ action 3's take rate
    // is above the floor -- the report prints it.
    TrainStats& cash = statsFor(cid);
    // OD_ACT_HIST also totals WHY an action was absent from the menu, which the
    // per-country counter has recorded since it was written and nothing has ever
    // reported. Industry is offered on 2.1% of econ decisions and taken on half
    // of those -- so the question "why does the AI not industrialise" is a
    // question about this gate, not about the policy.
    static const bool histOn = std::getenv("OD_ACT_HIST") != nullptr;
    auto gate = [&](int i, bool possible, double price) {
        v[i] = possible && t >= price;
        if (possible && !v[i]) cash.econCashBlocked[i]++;
        if (histOn) {
            if (!possible)      ++s_gateImpossible[i];
            else if (!v[i])     ++s_gateNoCash[i];
            else                ++s_gateOffered[i];
        }
    };
    int portPid = -1; float portCost = 0.0f;
    const bool portPossible = nextPortBuy(cid, portPid, portCost);
    // The province the executor would pick, at THAT province's price -- not a
    // country-wide proxy at the level-1 price. See nextIndustryBuy.
    int indPid = -1, indLvl = 0; float indCost = 0.0f;
    const bool indPossible = nextIndustryBuy(cid, indPid, indLvl, indCost);
    gate(1, indPossible, indPossible ? indCost : IND_COST[1] * indMod);
    gate(2, !st.frontiers.empty(),                         FORT_COST[1] * indMod);
    // Offered only when there is a harbour to actually buy, and priced at what
    // that one costs. This used to be a flat "treasury >= 60" with no question
    // of whether a port could be built at all, so a country whose every harbour
    // was at its cap was offered the action, took it, and got back "port: no
    // candidate" -- a wasted decision that still generated a gradient. See
    // nextPortBuy, which the executor now asks as well.
    gate(3, portPossible,                    portPossible ? portCost : 60.0);
    int spPid = -1; float spCost = 0.0f; const char* spRes = nullptr;
    const bool spPossible = nextSpecBuy(cid, spPid, spRes, spCost);
    gate(4, spPossible, spPossible ? spCost : 2.0);                 // specialize
    gate(5, st.maxPort >= 2,                               15.0);   // destroyer
    gate(6, st.maxPort >= 3,                               40.0);   // carrier
    // Research funding + branch focus
    auto raIt = g.m_countryResearchAllocation.find(cid);
    float alloc = raIt != g.m_countryResearchAllocation.end() ? raIt->second : 0.0f;
    // ── THE RESEARCH RATCHET, AND WHY IT IS STILL UNGATED ──
    //
    // Every other action on this menu is gated on cash by `gate` above. This
    // one is gated on nothing but its own ceiling, and it is the only action
    // here that commits income PERMANENTLY rather than spending a sum once --
    // so at decision time it looks free, pays a reward, and the head takes it
    // 81.1% of the time it is offered while choosing `fund down` 0.4% of the
    // time. A one-way ratchet: the share climbs to the cap and stays there.
    //
    // What that costs, measured against SCRIPT_BLITZ at turn 80, per
    // country-turn: the AI spent 44.92 on research out of 106.26 gross -- 42%,
    // its single largest expense and more in absolute terms than the opponent
    // spent out of a gross half again as large. Industry was then unaffordable
    // on 77.9% of the turns it was wanted (the opponent: 45.1%), the opponent
    // carried twice the industry upkeep, and its gross was 163.52 against
    // 106.26. That is what "the AI does not industrialise" looks like from the
    // ledger, and the ratchet is a real defect.
    //
    // TRIED, and it is worse: lower the CEILING instead of masking the action,
    // 0.5 -> 0.30, so the head keeps a working `fund up` and only saturates
    // earlier. It does exactly what it says -- research 38.19 -> 27.86 per
    // country-turn, army spending 2.76 -> 9.94, expenses 95.5% -> 90.2% of
    // gross -- and paired over five worlds it is NOT SEPARABLE against a rusher
    // (-0.8, helped on 3/5) and WORSE against the scripted rung (-4.7, helped
    // on 0/5). So the 39% is not overspending: the research buys more than the
    // army the money would otherwise raise, and the ratchet, real as it is,
    // is not costing the AI the game. Note both single-seed probes of this
    // said +0.6 and +0.1 before the five-world run said otherwise.
    //
    // ALSO TRIED, and it is worse: offer `fund up` only when the country could also
    // pay for the factory it is choosing between (`t >= indCost`). Land against
    // the scripted rung 60.6% -> 55.8%, and against the blitzer 29.4% -> 17.3%,
    // with gross income collapsing on the poorer map (67.24 -> 36.26). The rule
    // is a POVERTY TRAP: a country needs industry before it is allowed to fund
    // research, and research before it can unlock the industry levels that
    // would make it rich, so the countries that most need to grow are the ones
    // it locks out of both. Whatever fixes the ratchet has to leave the poor a
    // way up -- and note the eval is biased against any mask change of this
    // shape anyway, since the frozen policy puts 93.8% on `fund up` when
    // offered and that mass has to go somewhere when the action is removed.
    // 0.45 against the executor's 0.5 ceiling is not a mismatch: the step is
    // 0.05, so this stops offering the action exactly when one more step would
    // reach the cap.
    // THE ACTUAL CEILING, and it is here rather than in the action.
    // case 7 clamps with min(0.5f, alloc + 0.05f), but this bar stops OFFERING
    // fund-up at 0.45, so allocation can never exceed 0.4999 and that clamp is
    // unreachable by construction -- OD_RESEARCH_CAP at 0.50, 0.60 and 0.75 all
    // produced byte-identical benches, which is what sent me looking here.
    // 0.45 is a bare constant with nothing measured against it, and it is the
    // one that binds: with the research ratchets removed the mean settles at
    // 0.4339, just under this bar.
    //
    // CORRECTION to the reasoning that found it: the head does NOT push in one
    // direction only. N24 takes `fund up` on 97.3% of offers AND `fund down` on
    // 37.9% -- the equilibrium is a tug of war, not a ratchet against a
    // ceiling. (The 0-of-114,650 fund-down figure that suggested otherwise was
    // a DIFFERENT model, not the one any bench here uses.) The bar is still
    // worth sweeping, because a head that wants up 97% of the time it is asked
    // gains room from a higher bar -- but it is not releasing a jammed ratchet.
    static const float offerBar = std::getenv("OD_RESEARCH_BAR")
                                ? (float)atof(std::getenv("OD_RESEARCH_BAR")) : 0.45f;
    v[7] = alloc < offerBar;   // fund up
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
    // zeroed it again, and at the time the policy defunded research far more
    // often than it funded it (fund down taken at 63-81%, fund up at 6-8%), so
    // in practice there was no way out at all. Those rates have since INVERTED
    // -- fund up 81.1%, fund down 0.4% -- which is the ratchet described above;
    // the exit is kept regardless, because a subsystem no action can re-enable
    // is a trap whichever way the head happens to lean this month.
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
    // ── WHAT MAY BE ASKED OF OTHER PEOPLE, THIS TURN ──
    //
    // See AI_REQUESTS_PER_TURN. The four request actions -- alliance, NAP,
    // guarantee and trade -- are the ones with an audience, and the only ones
    // an uncapped module could turn into spam. Everything else on this menu
    // acts on the country's own ground and is left alone.
    const bool mayAsk = m_requestsThisTurn[cid] < AI_REQUESTS_PER_TURN;
    // ...AND ONE THIS COUNTRY COULD ACTUALLY ENACT, INSIDE ITS BUDGET.
    // See enactablePolicy: asking only whether doctrines EXIST wasted 97.4% of
    // every enact decision.
    v[1] = enactablePolicy(cid) != nullptr;
    auto pacIt = g.m_countryPacification.find(cid);
    float pac = pacIt != g.m_countryPacification.end() ? pacIt->second : 0.0f;
    // ── WHAT GOVERNING IS ALLOWED TO COST ──
    // See AI_SOCIAL_BUDGET_SHARE. Spending MORE is gated on the bill; spending
    // less never is, so a country that has overcommitted can always climb back
    // out. Computed once here for both this pair and the minority actions.
    const CountryIncomeSnapshot socInc = g.computeCountryIncome(cid);
    const float socialBill = socInc.minorityCosts + socInc.pacificationCost;
    const bool socialRoom =
        socialBill < std::max(0.0f, socInc.total) * AI_SOCIAL_BUDGET_SHARE;
    v[2] = pac < 0.99f && socialRoom;
    v[3] = pac > 0.01f;
    auto apIt = g.m_countryActivePolicyIndices.find(cid);
    v[4] = apIt != g.m_countryActivePolicyIndices.end() && !apIt->second.empty();
    // Diplomacy proposals need someone to talk to (target picked at exec) AND
    // this country's overture budget. Marking them permanently valid parked
    // ~3/8 of the politics softmax on "propose something" every single turn,
    // and no reward term ever taught the net that was wasteful — so it just
    // kept proposing forever.
    bool hasNeighbor = !m_stats[cid].frontiers.empty();
    // ...AND SOMEBODY IT MAY ACTUALLY ASK. See pactTargets: without this the
    // executor walked the frontier, found nobody and did nothing on 97.9% of
    // the turns this action was chosen, and every one of those wasted
    // country-turns still recorded a sample and trained the head.
    const PactTargets& pt = pactTargets(cid);
    const bool budget = hasNeighbor && diploBudgetReady(cid);
    v[5] = mayAsk && budget && pt.any[0];
    v[6] = mayAsk && budget && pt.any[1];
    v[7] = mayAsk && budget && pt.any[2];

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
    // Conciliation raises the bill for ever; repression lowers it, so only the
    // first answers to the budget. See AI_SOCIAL_BUDGET_SHARE -- including the
    // third experiment recorded there, which tried to make a factory take
    // priority over a minority programme and cost a fifth of the AI's land.
    v[9]  = st.minorities > 0 && st.minorityTrend < m_trendMax - 1e-3f && socialRoom;
    v[10] = st.minorities > 0 && st.minorityTrend > m_trendMin + 1e-3f;

    // Trade: money to buy with, a neighbour to buy from, and the same overture
    // budget the pact proposals answer to.
    //
    // WHAT is bought and from WHOM is decided in exec, which already walks the
    // frontier list. Deciding it here as well would pay for that walk on every
    // country's turn instead of only on the turns this action is chosen -- the
    // same reasoning that keeps the mask for actions 5-7 down to "has a
    // neighbour". The treasury floor is the one thing worth checking early:
    // an offer of nothing is not a trade, and a broke country would otherwise
    // burn its overture budget discovering that in exec.
    const Country* selfC = g.m_countries.getCountry(cid);
    v[11] = mayAsk && hasNeighbor && diploBudgetReady(cid) &&
            selfC && selfC->treasury >= TRADE_MIN_TREASURY;
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
float AISystem::predictAcceptance(int partnerCid, const char* requestKind,
                                  int askerCid) const {
    if (!requestKind) return 0.5f;
    // Their features, not ours: this is their decision, and buildFeatures is
    // what their own answer would be computed from.
    std::vector<float> feats;
    const_cast<AISystem*>(this)->buildFeatures(partnerCid, feats);
    if ((int)feats.size() != FEATURE_COUNT) return 0.5f;

    // WHAT IS BEING ASKED, in the slots decideDiplomacy writes it to.
    //
    // This predicted an answer without telling the net what the question was,
    // so it returned the same number for "will you accept a ceasefire" and
    // "will you join my war" -- and the whole point of the call is to tell
    // those apart before spending an overture on one. The odds and terms slots
    // stay zero: a caller asking "would they say yes" has not put a deal on the
    // table yet, and zero is what "no terms offered" means to the answerer too.
    if (strcmp(requestKind, "request_ceasefire") == 0)      feats[89] = 1.0f;
    else if (strcmp(requestKind, "request_alliance") == 0)  feats[90] = 1.0f;
    else if (strcmp(requestKind, "request_nap") == 0)       feats[91] = 1.0f;
    else if (strcmp(requestKind, "request_guarantee") == 0) feats[92] = 1.0f;
    else if (strcmp(requestKind, "call_to_arms") == 0)      feats[80] = 1.0f;

    // Through the trunk, for the reason spelled out in decideDiplomacy: this is
    // a head over the shared embedding, and handing it raw features returned an
    // empty vector, which fell through to the 0.5 below. Every caller has been
    // planning against a coin flip.
    const std::vector<float> emb =
        const_cast<NeuralNet&>(m_trunk).forward(feats);
    const std::vector<float> wide = const_cast<NeuralNet&>(m_diplo).forward(emb);
    if ((int)wide.size() != DIPLO_OUTPUTS) return 0.5f;
    // This kind's pair, so everything below still reasons about [reject, accept]
    // and the thumbs on the scale keep their existing indices.
    const int kind = offerKindOf(requestKind);
    std::vector<float> logits(wide.begin() + kind * DIPLO_ACTIONS,
                              wide.begin() + (kind + 1) * DIPLO_ACTIONS);
    // The same thumb on the scale answerDiplomacy puts there. Predicting
    // without it would model a different policy from the one that answers.
    if (strcmp(requestKind, "request_nap") == 0)
        logits[1] += AI_NAP_WILLINGNESS;
    else if (strcmp(requestKind, "call_to_arms") == 0)
        logits[1] -= AI_CALL_RELUCTANCE;
    // ...INCLUDING THE PACT CAP, which is the whole point of asking. The
    // composer uses this to rank whom to approach; without the cap modelled
    // here it would keep ranking a partner who is certain to refuse as the
    // best prospect on the board, spend its overture budget on them, and be
    // refused -- the exact waste predictAcceptance exists to avoid.
    if (strcmp(requestKind, "request_alliance") == 0 ||
        strcmp(requestKind, "request_nap") == 0 ||
        strcmp(requestKind, "request_guarantee") == 0) {
        auto pactsOf = [&](int cid) {
            auto it = m_stats.find(cid);
            return it != m_stats.end() ? it->second.pacts : 0;
        };
        const int mine = pactsOf(partnerCid), theirs = pactsOf(askerCid);
        if (mine >= AI_ALLY_MAX_PACTS || theirs >= AI_ALLY_MAX_PACTS) return 0.0f;
        logits[1] -= AI_ALLY_CROWDING *
                     (float)std::max(mine, theirs) / (float)AI_ALLY_MAX_PACTS;
        if (strcmp(requestKind, "request_alliance") == 0 ||
            strcmp(requestKind, "request_guarantee") == 0) {
            logits[1] -= AI_ALLY_WAR_RELUCTANCE *
                         std::min(1.0f, (float)foreignWarCount(askerCid) / 2.0f);
            if (strcmp(requestKind, "request_guarantee") == 0)
                logits[1] -= AI_GUARANTEE_RELUCTANCE;
        }
    }
    // ...and whether we are the power everybody has decided to stop, which is
    // the other reason a partner says no. Without this the leader keeps
    // ranking the neighbours who will refuse it as its best prospects.
    if (isCoalitionTarget(partnerCid, askerCid))
        logits[1] -= COALITION_WEIGHT * coalitionPressure();
    // ...and what our own word is worth to them, which is the other half of
    // the same reply. See CREDIBILITY_WEIGHT.
    {
        const Country* me = m_g->m_countries.getCountry(askerCid);
        const Country* them = m_g->m_countries.getCountry(partnerCid);
        if (me && them) {
            const float cred = m_g->credibility(me->isoA3, them->isoA3);
            if (cred < 1.0f) logits[1] -= CREDIBILITY_WEIGHT * (1.0f - cred);
        }
    }

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

void AISystem::buildAttackFeatures(int cid, const AttackCandidate& cand,
                                   std::vector<float>& out) const {
    out.assign(ATTACK_FEATURES, 0.0f);
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    if (!c) return;

    // 0: the OLD RULE'S OWN SCORE, handed over as an input rather than thrown
    // away. The head starts life having to beat a heuristic that is not stupid,
    // and the cheapest way for it to be no worse is to learn to follow this
    // one; everything else here is what it needs to learn when not to.
    out[0] = std::tanh(cand.margin - 1.0f);
    out[1] = std::tanh((float)cand.myGarrison / 5000.0f);
    out[2] = std::tanh((float)cand.theirGarrison / 5000.0f);
    out[3] = std::min(1.0f, (float)cand.fortLevel / 5.0f);
    out[4] = std::min(1.0f, (float)cand.indLevel / 10.0f);
    // 5: ours by right. A claimed province ends a war goal as well as taking
    // ground, which the rule expressed as a flat bonus and could not weigh.
    auto clIt = g.m_claimsByProvince.find(cand.toPid);
    if (clIt != g.m_claimsByProvince.end())
        for (auto& iso : clIt->second)
            if (iso == c->isoA3) { out[5] = 1.0f; break; }
    out[6] = cand.enemyCid >= Game::REBEL_CID_MIN ? 1.0f : 0.0f;
    out[7] = cand.fromAlly ? 1.0f : 0.0f;

    // 8: how surrounded the target already is. A province we half-encircle
    // falls cheaply and is hard to lose again; the old rule could not see the
    // shape of the front at all, only one province at a time.
    auto nIt = g.m_provinceNeighbors.find(cand.toPid);
    if (nIt != g.m_provinceNeighbors.end() && !nIt->second.empty()) {
        int mine = 0;
        for (int nb : nIt->second)
            if (nb >= 0 && nb < (int)g.m_provinceCountryLookup.size() &&
                g.m_provinceCountryLookup[nb] == cid) mine++;
        out[8] = (float)mine / (float)nIt->second.size();
    }

    // 9-11: the country on the other side, not just the province. Taking a
    // province off a great power at war with three others is a different
    // proposition from taking one off a neighbour with nothing else to do.
    auto meIt = m_stats.find(cid);
    auto themIt = m_stats.find(cand.enemyCid);
    if (meIt != m_stats.end() && themIt != m_stats.end()) {
        const double mine = (double)std::max(1LL, meIt->second.army);
        out[9] = (float)std::tanh(std::log((double)std::max(1LL, themIt->second.army) / mine));
        out[11] = std::tanh(themIt->second.provinces / 30.0f);
    }
    out[10] = std::tanh(foreignWarCount(cand.enemyCid) / 2.0f);
}

int AISystem::chooseAttack(int cid, const std::vector<AttackCandidate>& cands,
                           std::vector<int>* rankingOut) {
    m_pendingAttackCand.clear();
    m_pendingAttackChosen = -1;
    if (rankingOut) rankingOut->clear();
    // Nothing to choose between, and nothing to learn from a choice of one.
    if (cands.size() < 2) return -1;

    // Built even below the threshold, so the head warms up by watching the rule
    // it will replace. See chooseWarTarget for the chicken-and-egg this avoids.
    const bool maySteer = difficulty().useLearnedAim &&
                          m_attack.updateCount() >= attackWarmup();

    std::vector<float> own;
    buildFeatures(cid, own);

    const size_t n = std::min(cands.size(), (size_t)ATTACK_MAX_CANDIDATES);
    std::vector<float> scores(n, 0.0f);
    std::vector<float> candFeat;
    for (size_t i = 0; i < n; ++i) {
        buildAttackFeatures(cid, cands[i], candFeat);
        std::vector<float> in = own;
        in.insert(in.end(), candFeat.begin(), candFeat.end());
        const std::vector<float>& o = m_attack.forward(in);
        scores[i] = o.empty() || !std::isfinite(o[0]) ? 0.0f : o[0];
        m_pendingAttackCand.push_back(std::move(in));
    }

    if (!maySteer) return -1;   // recorded, but the rule still decides

    float temperature, epsilon;
    difficultyParams(temperature, epsilon);
    const int pick = NeuralNet::samplePolicy(scores, temperature, m_rng);
    if (pick < 0 || pick >= (int)n) return -1;
    m_pendingAttackChosen = pick;

    // THE WHOLE ORDER, not only the winner. See ATTACK_ORDERS_PER_TURN: one
    // decision to attack covers several fronts, and they should be pressed in
    // the order this head rates them rather than in whatever order the frontier
    // walk happened to produce. The sampled pick leads, because that is the one
    // recorded for training and the orders should match the sample; the rest
    // follow by score.
    if (rankingOut) {
        rankingOut->reserve(n);
        rankingOut->push_back(pick);
        std::vector<int> rest;
        for (size_t i = 0; i < n; ++i)
            if ((int)i != pick) rest.push_back((int)i);
        std::sort(rest.begin(), rest.end(),
                  [&](int a, int b) { return scores[a] > scores[b]; });
        rankingOut->insert(rankingOut->end(), rest.begin(), rest.end());
    }
    return pick;
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
    const bool maySteer = difficulty().useLearnedAim &&
                          m_target.updateCount() >= targetWarmup();

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

    // A DECLARATION ALREADY IN FLIGHT IS THIS TURN'S WAR.
    //
    // The war module gets ACTIONS_PER_MODULE_PER_TURN goes, and a queued
    // declaration changes no relation until the turn resolves -- so every
    // later pick saw the same peaceful border and declared the same war again,
    // and the count below, which reads wars FOUGHT, never noticed. One
    // country-turn produced up to three copies of one declaration, or three
    // separate wars past a cap that says two.
    //
    // Checked here rather than in exec because validWar asks this same
    // function: a rule enforced only at exec would leave "declare war" offered
    // to the policy all turn and refused every time.
    if (g.hasPendingDeclaration(c->isoA3)) return false;

    // Already fighting two? Nothing is worth a third front.
    //
    // The constant was measured at 2 long before campaigns existed, and the
    // campaign cap went to 2 in 8.5.0 -- so a country may now hold two
    // commitments while being allowed only one war it CHOSE, which means the
    // second campaign can only ever point at somebody already fighting it.
    // Whether that contradiction costs anything is a measurement, not an
    // assumption, and the old one was taken in a different game.
    // OD_MAX_WARS overrides.
    static const int maxWars = std::getenv("OD_MAX_WARS")
                             ? atoi(std::getenv("OD_MAX_WARS")) : AI_MAX_CONCURRENT_WARS;
    // Same size gate as the campaign cap: the second chosen war belongs to
    // a country with the provinces to hold a second front. See
    // OD_BIG_PROVINCES in Game::openCampaign.
    static const int bigProvW = std::getenv("OD_BIG_PROVINCES")
                              ? atoi(std::getenv("OD_BIG_PROVINCES")) : 0;
    const int effMaxWars = (bigProvW > 0 && (int)g.provincesOf(cid).size() < bigProvW)
                         ? 1 : std::max(1, maxWars);
    if (myWars >= effMaxWars) return false;
    // A country coming apart at home does not go looking for more.
    static const float wearyBlock = std::getenv("OD_WEARY_BLOCK")
                                  ? (float)atof(std::getenv("OD_WEARY_BLOCK"))
                                  : AI_WAR_WEARINESS_BLOCK;
    if (g.warWearinessOf(cid) >= wearyBlock) return false;

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
        // ONE CONVERSATION AT A TIME, and this is the half of that rule the
        // war module kept breaking. The politics module runs first and may
        // already have offered this neighbour an alliance, a pact or a
        // guarantee this turn; declaring war on the country we are mid-sentence
        // with is the confusion the player sees, and it is also how a pact got
        // broken and the war declared in the same breath, since a pending
        // break_nap lands here too.
        if (g.hasPendingDiplomacy(c->isoA3, ec->isoA3)) continue;
        // A NAP does not make this target off-limits, but it does mean the pact
        // has to be broken FIRST -- see the break-then-declare note where the
        // action is issued. The target is still chosen here so the AI can want
        // a war it is not yet allowed to start.
        if (nap) napBlocked.insert(fr.enemyCid);
        long long ea = m_stats[fr.enemyCid].army;
        // ── THE ENEMY IS THE ENEMY PLUS EVERYONE WHO SIGNED FOR THEM ──
        //
        // OD_COALITION_BAR=1: a guarantor joins the moment war is declared
        // (declareWar chains guarantees) and an ally is called to arms, so
        // the army the bar is measured against is the target's plus every
        // guarantor's and ally's not already at war with us. 1914 France,
        // rush world, seed 20260801: the war head declared on Belgium on
        // turn 1, the British Empire honoured its guarantee and took 172
        // French provinces; the learned chooser sees how many guarantors a
        // target has (out[6]), not their size.
        {
            static const bool coalitionBar = std::getenv("OD_COALITION_BAR") && atoi(std::getenv("OD_COALITION_BAR")) != 0;
            if (coalitionBar) {
                long long backers = 0;
                for (const auto& [isoA, targets] : g.m_relations) {
                    if (isoA == ec->isoA3 || isoA == c->isoA3) continue;
                    auto rt = targets.find(ec->isoA3);
                    if (rt == targets.end() || !(rt->second.guarantee || rt->second.alliance)) continue;
                    if (g.hasRelation(isoA, c->isoA3, &CountryRelation::war)) continue;
                    const int bcid = g.cidForIso(isoA);
                    if (bcid < 0 || bcid == cid) continue;
                    auto bs = m_stats.find(bcid);
                    if (bs != m_stats.end()) backers += bs->second.army;
                }
                ea += backers;
            }
            // OD_GUARANTOR_BAR=1: narrower -- an UNCLAIMED war on a country
            // whose guarantor (not an ally; guarantors join at the
            // declaration) holds more provinces than we do is refused
            // outright. Potential, not the standing army: Britain's turn-1
            // army cleared the 2.0 bar and its 334 provinces did not.
            // PER RUNG (DifficultyProfile::useGuarantorBar): +61 on hard, +1
            // on normal, -39 on EASY, where the protector it fears does not
            // punish the war it declines. OD_GUARANTOR_BAR forces it.
            const char* barEnv = std::getenv("OD_GUARANTOR_BAR");
            const bool guarantorBar = barEnv ? atoi(barEnv) != 0 : difficulty().useGuarantorBar;
            // OD_GUARANTOR_CLAIMED=1 applies the bar to CLAIMED wars too
            // (reconquest is otherwise exempt); OD_GUARANTOR_BY=army compares
            // armies instead of provinces.
            static const bool guarClaimed = std::getenv("OD_GUARANTOR_CLAIMED") && atoi(std::getenv("OD_GUARANTOR_CLAIMED")) != 0;
            static const bool guarByArmy = std::getenv("OD_GUARANTOR_BY") && std::string(std::getenv("OD_GUARANTOR_BY")) == "army";
            if (guarantorBar && (guarClaimed || !claimTargets.count(fr.enemyCid))) {
                bool bigGuarantor = false;
                for (const auto& [isoA, targets] : g.m_relations) {
                    if (isoA == ec->isoA3 || isoA == c->isoA3) continue;
                    auto rt = targets.find(ec->isoA3);
                    if (rt == targets.end() || !rt->second.guarantee) continue;
                    if (g.hasRelation(isoA, c->isoA3, &CountryRelation::war)) continue;
                    const int bcid = g.cidForIso(isoA);
                    auto bs = bcid >= 0 ? m_stats.find(bcid) : m_stats.end();
                    if (bs == m_stats.end()) continue;
                    const bool bigger = guarByArmy ? bs->second.army > st.army
                                                   : bs->second.provinces > st.provinces;
                    if (bigger) { bigGuarantor = true; break; }
                }
                if (bigGuarantor) continue;
            }
        }
        bool claimed = claimTargets.count(fr.enemyCid) > 0;
        // Reconquering CLAIMED land stays cheap: it removes unrest and
        // satisfies the claim, and it is the expansion that is supposed to
        // happen. Attacking a neighbour it has NO claim on now needs a real
        // edge rather than a coin-flip one -- 1.05 meant "very slightly ahead",
        // which is why the map was permanently on fire. Land is still taken; it
        // just has to be worth taking.
        double bar = claimed ? AI_WAR_BAR_CLAIMED : unclaimedBar(false);
        // Opening a SECOND war costs more again, claim or no claim: one front
        // at a time unless the second is genuinely easy.
        if (myWars >= 1) bar += secondFrontBar();
        // ── WHOSE ARMY THE BAR IS MEASURED AGAINST ──
        //
        // Normally this country's own, which is right: it is the one doing the
        // fighting. Against a power the world has formed up against it is the
        // COALITION's, because that is who the leader will be fighting.
        //
        // This is the whole correction over the version that shipped and was
        // measured backwards. That one lowered the bar for each member, so each
        // picked a fight it could not win on its own -- they lost their armies
        // one at a time and the leader took the ground, ending the run MORE
        // concentrated than with no coalition at all. The bar was never wrong.
        // Measuring it against one country's army when four are coming was.
        const bool joint = m_coalition.target == fr.enemyCid && inCoalition(cid);
        const long long side = joint ? m_coalition.combinedArmy : st.army;
        // ── ARMIES ARE NOT INTERCHANGEABLE (OD_WAR_BAR_RESEARCH, off) ──
        //
        // The bar compares headcounts. Two armies of the same size are not
        // the same army: armyAtkPct and armyDefPct are country-level effects
        // the game already computes, and a country thirty percent ahead on
        // military research fields a thirty percent better army. Same move
        // as the supply term and the threat ranking -- use the number the
        // game keeps rather than the proxy that ignores it -- and scoped to
        // match, a COUNTRY-level multiplier for a country-level decision.
        // Fortification, supply and depth are deliberately absent: those are
        // properties of a province, and this is a question about a war.
        static const bool barResearch = std::getenv("OD_WAR_BAR_RESEARCH") &&
                                        atoi(std::getenv("OD_WAR_BAR_RESEARCH")) != 0;
        double mySide = (double)side, theirSide = (double)ea;
        if (barResearch) {
            mySide    *= 1.0 + g.getTotalEffect("armyAtkPct", cid) / 100.0;
            theirSide *= 1.0 + g.getTotalEffect("armyDefPct", fr.enemyCid) / 100.0;
        }
        if (mySide < theirSide * bar + 200.0) continue;
        // EVERY neighbour that clears the bars is a candidate, not just the
        // best one by the old rule. The rule still decides who is ALLOWED to be
        // attacked; which of them actually is, is chosen below.
        cands.push_back({fr.enemyCid, claimed, false,
                         napBlocked.count(fr.enemyCid) > 0, ea});
        // The old rule, kept as the fallback and as the mask's answer: a
        // claimed neighbour beats any unclaimed one, and within a class the
        // weakest wins.
        //
        // It does NOT prefer the coalition's target, and that is deliberate
        // after measurement rather than an oversight. Pointing both this and
        // the learned chooser at the leader was tried -- see COALITION_SHARE --
        // and moved nothing: 63.2% -> 62.5% against the scripted rung and
        // 16.0% -> 16.9% against a rusher, both inside the band. The coalition
        // does not hurt because it aims badly.
        if (target < 0 || (claimed && !targetClaimed) ||
            (claimed == targetClaimed && ea < targetArmy)) {
            target = fr.enemyCid; targetArmy = ea; targetClaimed = claimed;
        }
    }
    if (target >= 0) {
        // ── THE ADVISOR'S NAMED TARGET, WHEN IT IS ADMISSIBLE ──
        //
        // Chosen HERE rather than as a weight inside chooseWarTarget, and the
        // reason is a measurement rather than a preference: that function fills
        // scores[] and then returns -1 whenever difficulty().useLearnedAim is
        // false, which it is on easy AND normal. A term added there would have
        // been computed and discarded on the two rungs most players use --
        // recorded, inert, and looking like advice that sometimes does nothing.
        //
        // It also removes a temperature dependence nobody would expect: the
        // sampling temperature runs 1.60 on easy to 0.30 on insane, so one
        // constant would have been a 1.17x nudge for one player and 2.30x for
        // another.
        //
        // BOUNDED BY WHAT IS ALREADY ADMISSIBLE. cands holds only neighbours
        // that cleared the power bars above; naming somebody the country cannot
        // beat finds nothing here and changes nothing. The advisor picks among
        // the wars its generals would already accept -- it cannot start one
        // they would refuse.
        const int pressed = m_g->llmPressTarget(cid);
        if (pressed > 0) {
            for (size_t i = 0; i < cands.size(); ++i) {
                if (cands[i].cid != pressed) continue;
                // Tell the aiming head what was picked, exactly as the rule
                // path below does, so a head that is still learning has a
                // label rather than a gap.
                if (!m_pendingTargetCand.empty() && i < m_pendingTargetCand.size())
                    m_pendingTargetChosen = (int)i;
                out.cid = cands[i].cid;
                out.claimed = cands[i].claimed;
                out.naval = false;
                out.napBlocked = cands[i].napBlocked;
                return true;
            }
        }
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
        double bar = claimed ? 1.0 : unclaimedBar(true);
        if (myWars >= 1) bar += secondFrontBar();
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
    // Recruit. The munitions half of the price is checked at the SMALLEST
    // order the executor will actually place (1,000 men, see execWar case 1),
    // so the mask offers the action exactly when some recruit is possible
    // rather than when the largest one is.
    v[1] = c->treasury >= 1 && st.population > 10000 &&
           g.canAffordWarMaterials(cid, g.recruitPrice(1000, cid));
    // OD_RECRUIT_MASK=1: ask the question the EXECUTOR asks.
    //
    // "recruit: too poor/small" is 108,650 refusals in two 400-turn games --
    // 50.4% of every refused execution in the AI. The mask tests country-wide
    // POPULATION (headcount); the executor tests the chosen province's
    // availableManpower (what is left to conscript after orders already placed
    // this turn) and refuses when maxRecruit = manpower/5 is under 1000. A
    // country of ten million whose provinces are conscripted out passes the
    // mask every turn and is refused every turn.
    //
    // This is the same defect the repress no-op had -- "the validity mask asks
    // whether the country's MEAN trend still has room to move" -- and the
    // economy module was already fixed this way ("the province the executor
    // would pick, not a country-wide proxy"). War's recruit never was.
    //
    // Necessary condition only: if NO province can yield 1000 men the executor
    // is certain to refuse, so offering it is certainly waste. Where some
    // province could, the executor may still pick a different one.
    if (v[1]) {
        static const bool tightRecruit = std::getenv("OD_RECRUIT_MASK") &&
                                         atoi(std::getenv("OD_RECRUIT_MASK")) != 0;
        if (tightRecruit) {
            long long bestMp = 0;
            for (int p2 : g.provincesOf(cid))
                bestMp = std::max(bestMp, g.availableManpower(p2));
            if (bestMp / 5 < 1000) v[1] = false;
        }
    }

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
    // A source already carrying a move order cannot send again --
    // reinforceProvince refuses it -- so the mask has to know that too, or the
    // head keeps choosing reinforce for the rest of the turn and every call
    // after the first answers "nothing to move". That was 4,542 of 7,201
    // reinforce decisions (63%) in an 80-turn eval, the largest single waste
    // left in any head. attackAvailable already does exactly this with
    // preOrdered; this is the same rule for the same reason.
    // OFF by default for the same reason as the bombard gate above: the
    // no-ops are real but removing them is measured worse. OD_REINFORCE_GATE=1.
    static const bool reinforceGate = std::getenv("OD_REINFORCE_GATE") && atoi(std::getenv("OD_REINFORCE_GATE")) != 0;
    std::unordered_set<int> preOrderedSrc;
    if (reinforceGate)
        for (const auto& mo : g.m_pendingMoveOrders)
            if (mo.countryId == cid) preOrderedSrc.insert(mo.fromProvince);
    bool canReinforce = false;
    for (auto& fr : st.frontiers) {
        auto nIt = g.m_provinceNeighbors.find(fr.pid);
        if (nIt == g.m_provinceNeighbors.end()) continue;
        for (int nid : nIt->second) {
            if (nid < 0 || nid >= (int)g.m_provinceCountryLookup.size() ||
                g.m_provinceCountryLookup[nid] != cid) continue;
            if (preOrderedSrc.count(nid)) continue;
            if (garrisonOf(nid, cid) >= 200) { canReinforce = true; break; }
        }
        if (canReinforce) break;
    }
    // ── REINFORCEMENT WAS MADE A REFLEX, AND MEASURED, AND PUT BACK ──
    //
    // The war head chooses this on 79% of the turns it is offered -- 4,676
    // reinforcements against 409 attacks -- and it overlaps with two reflexes
    // that already run unconditionally (garrisonReflex, redeployReflex). It
    // looks exactly like a comfortable default: always available, never loses
    // an army, and it lets the head avoid choosing between building an army and
    // using one. A one-note aggressor beats this AI 76.6% of the land to 23.4%
    // (ScriptVariant::SCRIPT_BLITZ) by attacking 4.5x as often, not by fighting
    // better -- both sides lose about half their assaults.
    //
    // So it was moved out of the policy and run every turn instead. Measured on
    // one frozen model, both directions:
    //
    //                       action    reflex
    //     land vs script      1.50x     1.03x
    //     land vs blitz       23.4%     31.4%
    //
    // It buys a little against the exploit and costs a THIRD of everything the
    // AI holds against an ordinary opponent. Reinforcement is load-bearing:
    // topping up a frontier at the moment the policy judges it necessary is
    // apparently worth much more than doing it on a fixed rule every turn.
    //
    // And it did not do what it was for. The freed probability mass went to
    // `hold` (14.1% -> 56.1%), not to `attack` -- mask out the head's favourite
    // safe action and it takes the next safe one. The war module's passivity is
    // not a menu problem and will not be fixed by removing options from it.
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
    // ...AND AN ASSAULT IT COULD ACTUALLY WIN, FROM A PROVINCE NOT ALREADY
    // SPOKEN FOR. This tested only "at war somewhere, and has an army", while
    // the executor needs a frontier garrison over its floor, an adjacent enemy
    // province, and a margin over the winnability bar once fortification and
    // the defender's research are priced in -- so 78% of every attack decision
    // did nothing. See attackCandidates.
    v[3] = (anyWarFrontier || !st.abroadPids.empty()) && st.army > 0 &&
           attackAvailable(cid);
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
    // The SEVENTH copy of the artillery price list lived here, in the mask,
    // and it is the one that mattered most: a mask that prices a shell
    // differently from the executor offers an action the executor then refuses,
    // and the head learns from a choice that never happened. Prices come from
    // ARTY_COSTS via Game::artilleryPrice; only the node ids are the AI's.
    bool haveShell = false;
    if (anyWarFrontier) {
        static const struct { const char* node; const char* type; } SHELLS[] = {
            {"arty6a","nuclear"}, {"arty6b","biological"}, {"arty5","chemical"},
            {"arty4a","napalm"},  {"arty4b","carpet"},     {"arty3","heavy"},
            {"arty2","light"},    {"arty1","mortar"}};
        for (auto& s : SHELLS) {
            if (!g.hasResearched(s.node, cid)) continue;
            const Game::WarPrice p2 = g.artilleryPrice(s.type, cid);
            // Money AND materials, the same pair execWar checks.
            if (c->treasury >= p2.money && g.canAffordWarMaterials(cid, p2)) {
                haveShell = true; break;
            }
        }
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
    // ...AND 500 MEN ON THE CROSSING ITSELF, not merely in the country. See
    // stageAvailable: the whole-army test wasted 85.7% of stage decisions.
    v[7] = !st.staging.empty() && stageAvailable(cid);
}

void AISystem::validNavy(int cid, std::vector<bool>& v) {
    Game& g = *m_g;
    const CountryStat& st = m_stats[cid];
    v.assign(NAVY_ACTIONS, false);
    v[0] = true;
    int ships = st.boats + st.destroyers + st.carriers;
    // ...AND SOMEWHERE FOR ONE OF THEM TO GO. Having hulls is not having a
    // destination: at peace with every ship already on station there is nothing
    // to order, which is 48% of what this action used to do. See
    // navyMoveAvailable.
    v[1] = ships > 0 && navyMoveAvailable(cid);
    // A warship, ammunition it can afford, and something in range: see
    // bombardAvailable. Was "own a warship", which no-opped 99.9% of the time.
    // OFF by default. Gating the action on "the executor would fire" cut the
    // no-ops from 1123/1124 to 3/4 and lifted three trained models, but
    // isolated on ONE binary it costs: shipped 148 -> 89 (Sweden and China
    // annihilated, survival 82 -> 48) and N24 249 -> 239. Same lesson the
    // war head's reinforce mask already carries in this file -- mask out a
    // head's favourite safe action and the freed probability goes to the
    // next safe one, which is worse. A wasted action is not free, but it is
    // cheaper than the action that replaces it. OD_BOMBARD_GATE=1 to test.
    static const bool bombardGate = std::getenv("OD_BOMBARD_GATE") && atoi(std::getenv("OD_BOMBARD_GATE")) != 0;
    v[2] = st.destroyers + st.carriers > 0 && (!bombardGate || bombardAvailable(cid));
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
    // ...AND A THOUSAND MEN STANDING AT ONE OF OUR OWN PORTS, which is what
    // the executor needs. This tested the country's WHOLE army, so a country
    // with a large field army and an empty harbour was offered the action,
    // chose it, and loaded nobody: 67.8% of embark decisions did nothing.
    int embPid = -1, embG = 0;
    v[3] = st.maxPort >= 1 && st.navalWarTargets > 0 &&
           bestEmbarkPort(cid, embPid, embG);
    // "Land" is valid only when it can DO something: the executor lands at
    // an at-war port within one hull's range and otherwise does nothing (its
    // unload-at-home fallback was removed, journal 37a). A mask that offers
    // the action whenever a loaded boat exists lets the head choose a no-op
    // and learn from a choice that never happened -- the same mask/executor
    // split the artillery table had. Same test as the executor's.
    v[4] = false;
    if (st.boatsWithCrew > 0) {
        Game& g = *m_g;
        const int mapW = g.m_provinces.getWidth(), mapH = g.m_provinces.getHeight();
        const Country* me = g.m_countries.getCountry(cid);
        auto relIt = me ? g.m_relations.find(me->isoA3) : g.m_relations.end();
        if (mapW > 0 && mapH > 0 && relIt != g.m_relations.end()) {
            for (const auto& s : g.m_ships) {
                if (s.countryId != cid || s.crew <= 0) continue;
                const double reach = g.shipMaxRangeDeg(s);
                for (const auto& [pid, port] : g.m_provincePorts) {
                    (void)port;
                    const Province* p = g.m_provinces.getProvinceById(pid);
                    if (!p) continue;
                    const Country* oc = g.m_countries.getCountry(p->countryId);
                    if (!oc) continue;
                    auto rr = relIt->second.find(oc->isoA3);
                    if (rr == relIt->second.end() || !rr->second.war) continue;
                    auto cIt = g.m_provinceCenters.find(pid);
                    if (cIt == g.m_provinceCenters.end()) continue;
                    const double lon = cIt->second.x / mapW * 360.0 - 180.0;
                    const double lat = 90.0 - cIt->second.y / mapH * 180.0;
                    if (Game::seaDistanceDeg(s.lon, s.lat, lon, lat) <= reach) { v[4] = true; break; }
                }
                if (v[4]) break;
            }
        }
    }

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
    // AGAINST NET, NOT THE PRE-EXPENSE TOTAL. inc.total is gross+resource+pop
    // before a single bill is paid, so a fleet costing 105 a turn against a
    // total of 1062 reads as 10% -- comfortably under the 15% bar -- while
    // actually eating more than twice the 44 left after everything else.
    // A country in that position cannot buy industry, troops or a war,
    // because the ships have already spent it, and `broke` does not catch it
    // either: net is still positive, just barely. Found in a played game
    // where scrap was refused at net 20 while the navy drew 105 and had not
    // moved a hull in 200 turns.
    const bool squeezed = inc.net >= 0.0f && inc.navyExpenses > inc.net;
    // Nothing to fight, nowhere to sail, and more than one hull to pay for.
    const bool idleFleet = !atWar && st.navalTargets == 0 &&
                           st.navalWarTargets == 0 && ships > 1;
    v[5] = haveScrappable && (costly || broke || squeezed || idleFleet);

    // ── Engage: an enemy hull we can actually reach ──
    //
    // Gated on the same two things processNavyCombat enforces -- an actual war,
    // and a target inside this hull's range -- so the action is never offered
    // when the executor would only have its order thrown away. Boats are
    // excluded as attackers: they are transports, and sending one to trade
    // damage with a destroyer loses the cargo for nothing.
    v[6] = false;
    for (const auto& mine : g.m_ships) {
        if (mine.countryId != cid || mine.type == "boat") continue;
        const double reach = g.shipMaxRangeDeg(mine);
        for (const auto& them : g.m_ships) {
            if (them.countryId <= 0 || them.countryId == cid) continue;
            if (!g.atWarCids(cid, them.countryId)) continue;
            const double dl = them.lon - mine.lon, dt = them.lat - mine.lat;
            if (std::sqrt(dl * dl + dt * dt) <= reach) { v[6] = true; break; }
        }
        if (v[6]) break;
    }
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
        case 1: { // upgrade industry in the province the mask costed
            // ONE CHOICE, ASKED IN ONE PLACE. See nextIndustryBuy: the mask
            // offers this action only when that function finds a province and
            // the treasury clears ITS price, so re-deriving the choice here
            // would let the two disagree again.
            int bestPid = -1, nextLv = 0; float cost = 0.0f;
            if (!nextIndustryBuy(cid, bestPid, nextLv, cost))
                return didNothing("industry: no eligible province");
            // ── THE WAR ECONOMY (OD_CAMPAIGN_ECON) ──
            //
            // A campaign already steers recruitment, reinforcement and the
            // attack chooser; this is the same commitment reaching the
            // economy. While one is open the next factory goes up in the
            // province the war is being fed through, rather than wherever
            // the ordinary rule ranks best this turn. Extending the one
            // mechanism that raised the floor rather than inventing another.
            // MEASURED AND OFF: mean 229 -> 189 across four models (265 ->
            // 195, 202 -> 217, 238 -> 190, 212 -> 152). A factory takes
            // IND_TURNS to build and pays back over the rest of the game,
            // so a twelve-turn commitment must not choose where it goes: the
            // campaign's horizon is twelve turns and industry's is a hundred.
            // A commitment may steer decisions that pay back inside its own
            // deadline, and no others.
            static const bool warEcon = std::getenv("OD_CAMPAIGN_ECON") &&
                                        atoi(std::getenv("OD_CAMPAIGN_ECON")) != 0;
            if (warEcon)
                if (const Game::Campaign* camp = g.campaignOf(cid)) {
                    int lv = 0; float c2 = 0.0f;
                    if (industryBuyAt(cid, camp->stagingProvince, lv, c2)) {
                        bestPid = camp->stagingProvince; nextLv = lv; cost = c2;
                    }
                }
            if (c.treasury < cost) return didNothing("industry: cannot afford");
            if (c.treasury - cost < siegeEarmark(cid)) return didNothing("industry: earmarked for the front");
            c.treasury -= cost;
            money::add(money::BUY_INDUSTRY, -(double)cost);
            g.m_pendingUpgrades.push_back({bestPid, "industry", nextLv, IND_TURNS[nextLv]});
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
            if (bestPid < 0) return didNothing("fort: no eligible frontier");
            int nextLv = bestLvl + 1;
            float cost = (float)FORT_COST[std::min(nextLv, 5)] *
                         buildCostMod(g.getTotalEffect("industryCostPct", cid));
            if (c.treasury < cost) return didNothing("fort: cannot afford");
            c.treasury -= cost;
            money::add(money::BUY_FORT, -(double)cost);
            g.m_pendingUpgrades.push_back({bestPid, "fortification", nextLv, 1});
            return TextFormat("fort lvl %d in prov %d ($%.0f)", nextLv, bestPid, cost);
        }
        case 3: { // port: upgrade an existing one, else found a new one
            // ONE RULE, ASKED IN ONE PLACE. The mask offers this action only
            // when nextPortBuy finds something, and the savings reserve holds
            // money back for exactly the price it quotes -- so this must buy
            // that, and not re-derive a choice of its own. See nextPortBuy.
            int pid = -1; float cost = 0.0f;
            if (!nextPortBuy(cid, pid, cost)) return didNothing("port: no candidate");
            if (c.treasury < cost) return didNothing("port: cannot afford");
            if (c.treasury - cost < siegeEarmark(cid)) return didNothing("port: earmarked for the front");
            const auto ex = g.m_provincePorts.find(pid);
            const int next = (ex != g.m_provincePorts.end()) ? ex->second.level + 1 : 1;
            c.treasury -= cost;
            money::add(money::BUY_PORT, -(double)cost);
            g.m_pendingUpgrades.push_back({pid, "port", next, 3});
            // The answer this turn has just changed. nextPortBuy caches per
            // country per turn, and a module now gets up to
            // ACTIONS_PER_MODULE_MAX goes in one -- so a stale cache would send
            // the next pick at the harbour just bought and turn it into a
            // no-op, which is the exact defect the cache is meant to serve.
            m_portBuyCache.erase(cid);
            return TextFormat("port lvl %d in prov %d ($%.0f)", next, pid, cost);
        }
        case 4: { // specialize the province the mask costed -- see nextSpecBuy
            int bestPid = -1; float cost = 0.0f; const char* bestRes = nullptr;
            if (!nextSpecBuy(cid, bestPid, bestRes, cost))
                return didNothing("spec: no candidate");
            if (c.treasury < cost) return didNothing("spec: cannot afford");
            if (c.treasury - cost < siegeEarmark(cid)) return didNothing("spec: earmarked for the front");
            c.treasury -= cost;
            money::add(money::BUY_SPECIALIZE, -(double)cost);
            g.m_pendingSpecializations.push_back({bestPid, bestRes, 3});
            return TextFormat("specialize %s in prov %d", bestRes, bestPid);
        }
        case 5: case 6: { // build ship at the best own port
            const char* type = action == 5 ? "destroyer" : "carrier";
            float cost = action == 5 ? 15.0f : 40.0f;
            int needPort = action == 5 ? 2 : 3;
            if (c.treasury < cost) return didNothing("ship: cannot afford");
            if (c.treasury - cost < siegeEarmark(cid)) return didNothing("ship: earmarked for the front");

            // ── AND THE BILL THAT ARRIVES EVERY TURN AFTERWARDS ──
            //
            // A carrier is 40 once and 25 A TURN FOR EVER; a destroyer is 15
            // and 10. Nothing checked the second number. The sticker price is
            // the smaller half of what a hull costs and the only half anything
            // asked about, so a country with 40 in the bank and no spare income
            // could buy a carrier and then never pay for it again.
            //
            // Measured 2026-08-25 against the scripted rung over 300 turns:
            // the model built 2.97 carriers and 1.49 destroyers per thousand
            // country-turns where the script built NONE, and spent 144.55 turns
            // bankrupt against the script's 3.92 -- 37x -- taking 226 austerity
            // cuts per thousand against 9. The navy was the difference.
            //
            // A PLAYER faces this constraint by reading their net income going
            // negative and not clicking the button. The AI had no equivalent,
            // which is the asymmetry rather than the difficulty setting.
            //
            // Checked as a constraint and not priced as a penalty: the reward
            // already charges bankruptcy and it did not stop this, because a
            // saturating penalty stops mattering once you are already broke.
            // Something a player cannot do should be something the AI cannot
            // do.
            const float berth = (action == 5) ? 10.0f : 25.0f;
            // ── AGAINST THE INCOME IT WILL HAVE, NOT THE ONE IT HAS ──
            //
            // This asked computeCountryIncome, which answers for right now, and
            // "right now" is the one turn a hull costs nothing: it is three
            // turns in the yard and 10 or 25 a turn for ever afterwards. A
            // country with two carriers already on the slipway had 50 a turn of
            // bills that had not started yet, and every one of them read as
            // spare income to the next purchase.
            //
            // Game::projectIncome walks the build queues forward, so the berth
            // is now checked against the income of the turn the hull actually
            // floats, with every other hull already ordered paid for first. The
            // horizon is the one the economy is scored over, AI_PLAN_HORIZON.
            const auto csNow = g.projectIncome(cid, planHorizon());
            if (csNow.net < berth)
                return didNothing(TextFormat("ship: %.0f/turn upkeep, only %.0f spare in %d turns",
                                  berth, csNow.net, AI_PLAN_HORIZON));
            for (auto& [pid, port] : g.m_provincePorts) {
                const Province* p = g.m_provinces.getProvinceById(pid);
                if (!p || p->countryId != cid || port.level < needPort) continue;
                c.treasury -= cost;
                money::add(money::BUY_SHIP, -(double)cost);
                g.m_pendingShipBuilds.push_back({pid, type, 3});
                if (action == 5) statsFor(cid).destroyersBuilt++;
                else             statsFor(cid).carriersBuilt++;
                m_shipsBoughtThisTurn[cid]++;
                return TextFormat("build %s at prov %d", type, pid);
            }
            return didNothing("ship: no port");
        }
        case 7: { // research funding up
            // While a campaign is open, research funding does not RISE: the
            // laboratory pays back over the rest of the game and the war is
            // decided in twelve turns. This is the siege reflex's trade --
            // which measured well -- applied to a war of choice rather than a
            // war at the gates. OD_CAMPAIGN_LABS=0 keeps the old behaviour.
            {
                // MEASURED AND OFF. The old note here read "N24 265 -> 223,
                // N37 238 -> 220"; those numbers are from a pre-8.3 build and
                // do NOT reproduce -- on the current one this knob is neutral
                // at 120 turns (229 -> 233). Re-measured 2026-09-07 at the
                // horizon that decides: 400 turns, hold-out worlds, 349/96/74
                // -> 274/87/29. Still off, and now for a reason that survives.
                //
                // WHY it fails where the siege cut (+113 at 400 turns)
                // succeeds: the siege version is gated on the EARMARK -- a
                // fort is actually owed -- so it moves money to an expense
                // that exists. This one holds research for twelve turns
                // whether the campaign needs the money or not. A reflex that
                // forced the same cut on a cash threshold failed the same way
                // (N24 325 -> 181). The lever is "fund a specific expense",
                // not "spend less on research while busy".
                static const bool campLabs = std::getenv("OD_CAMPAIGN_LABS") &&
                                             atoi(std::getenv("OD_CAMPAIGN_LABS")) != 0;
                if (campLabs && g.campaignOf(cid))
                    return didNothing("research: the campaign first");
            }
            {
                // Same knob as siegeReflex, same sense: default OFF is what
                // ships, OD_SIEGE_RESEARCH=1 restores the old behaviour.
                static const bool cutResearch = std::getenv("OD_SIEGE_RESEARCH") && atoi(std::getenv("OD_SIEGE_RESEARCH")) != 0;
                // Gated on the EARMARK (a fort is owed), not on the siege alone:
                // refusing fund-up whenever besieged measured 67.9 world
                // survival against 69.8 for this form on the peer's aggregate
                // (seed 4242, 60 turns); the 69.8 build is the v19 record.
                if (cutResearch && siegeEarmark(cid) > 0.0f) return didNothing("research: the front first");
            }
            float& alloc = g.m_countryResearchAllocation[cid];
            // THE CEILING. 0.5 is a bare constant with no note and nothing
            // measured against it. The head takes this action on 96.4% of the
            // turns it is offered and takes `fund down` on 0 of 114,650, so it
            // is pressing against this cap continuously; with the research
            // ratchets removed the mean allocation settles at 0.4339, which is
            // 87% of it. Whether 0.5 is the right ceiling has never been
            // asked -- OD_RESEARCH_CAP asks it.
            static const float cap = std::getenv("OD_RESEARCH_CAP")
                                   ? (float)atof(std::getenv("OD_RESEARCH_CAP")) : 0.5f;
            alloc = std::min(cap, alloc + 0.05f);
            return TextFormat("research funding up to %.0f%%", alloc * 100);
        }
        case 8: { // research funding down
            float& alloc = g.m_countryResearchAllocation[cid];
            alloc = std::max(0.0f, alloc - 0.05f);
            return TextFormat("research funding down to %.0f%%", alloc * 100);
        }
        case 9: case 10: case 11: { // pick the next research node by branch
            // ── THE HEAD PICKS ARMY EVERY TIME, AND THAT IS NOT THE BUG ──
            //
            // Marginal probability at temperature 1 on the shipped model:
            // focus army 100.0%, focus buildings 0.0%, focus navy 0.0% over 495
            // offers. It looks like a collapsed head, and the reward gives it a
            // reason to be one -- research pays log1p(NODES COMPLETED), the
            // executor below takes the cheapest available node in the branch,
            // and army is both the cheaper branch and the larger one (33 nodes,
            // median cost 25, against buildings' 28 at median 40). Cheap-tech
            // farming is what that reward pays for.
            //
            // MEASURED ANYWAY, by forcing `want` to "buildings" for every focus
            // action: land against the scripted rung 62.0% -> 57.5%. The head
            // is choosing the better branch. Building research unlocks HIGHER
            // levels of things the AI already cannot afford to build -- industry
            // was withheld for want of money on 90.4% of the turns it was
            // wanted -- so the unlock buys nothing while an army tech applies to
            // the men it already has. Do not "fix" this direction without
            // fixing the poverty upstream of it first.
            static const char* FOCUS[] = {"buildings", "army", "navy"};
            const char* want = FOCUS[action - 9];
            // OD_FOCUS_BRANCH forces the branch, to test the UNLOCK half of
            // "why does the AI not industrialise". The head takes focus-army on
            // 98.4% of research picks and focus-buildings on 1.6%, so
            // industryCap stays at max(3, researchedIndustryLevel) = 3, every
            // province reaches level III, and nextIndustryBuy then returns
            // false on 91.7% of checks -- industry is not refused, it is
            // impossible. Building research is what raises the ceiling.
            static const char* forcedBranch = std::getenv("OD_FOCUS_BRANCH");
            if (forcedBranch && *forcedBranch) want = forcedBranch;
            // Cheapest available node in the focused branch; if the branch is
            // exhausted, cheapest available anywhere (population/misc land here).
            int bestIdx = -1, bestCost = INT32_MAX;
            int fallbackIdx = -1, fallbackCost = INT32_MAX;
            for (int i = 0; i < (int)g.m_researchNodes.size(); ++i) {
                const ResearchNode& n = g.m_researchNodes[i];
                if (n.infinite) continue;
                if (!g.isNodeAvailableFor(n, cid)) continue;
                // ── THE SKIP THAT LOCKED THE AI OUT OF ITS OWN TECH TREE ──
                //
                // This used to also refuse any node whose granted build level
                // was already covered by industryCap/fortCap/portCap. Those
                // three do not report what the country has RESEARCHED; they
                // report max(baseline, researched), and the baselines are 3, 2
                // and 1 (see industryCap and friends -- an AI with no research
                // is still allowed to build a little).
                //
                // So ind1, ind2 and ind3 all looked like nodes that "change
                // nothing" and were skipped for ever. ind4 requires ind3, and
                // isNodeAvailableFor enforces that -- so the AI could never
                // reach it. Every AI country in the game was permanently
                // capped at industry III, fortification II and PORT I. The
                // last of those is why the fleet never grew: a destroyer needs
                // a level 2 port and a carrier a level 3, so outside whatever
                // harbours the map happened to hand out, no AI could build a
                // warship at all, ever. Measured over 400 turns of the shipped
                // scenarios: 4 destroyers and 1 carrier built, world-wide.
                //
                // A node the country already holds is excluded by
                // isNodeAvailableFor a line above, which is the only exclusion
                // this ever needed. What remains here is the honest version of
                // the original intent: a node that grants a level the country
                // has genuinely already RESEARCHED, and carries nothing else,
                // teaches it nothing -- which after the dependency check can
                // only happen on a map that shipped the tree out of order.
                if ((n.fortLevel > 0 || n.industryLevel > 0 || n.portLevel > 0) &&
                    n.fortLevel <= g.getResearchedFortLevel(cid) &&
                    n.industryLevel <= g.getResearchedIndustryLevel(cid) &&
                    n.portLevel <= g.getResearchedPortLevel(cid) && !n.unlockShips &&
                    n.armyDefPct == 0 && n.armyAtkPct == 0 && n.conscriptionCostPct == 0 &&
                    n.maintenanceCostPct == 0 && n.navyCostPct == 0 && n.navyAtkPct == 0 &&
                    n.navyDefPct == 0 && n.navySpeedPct == 0 && n.popModPct == 0 &&
                    n.resourceModPct == 0 && n.industryCostPct == 0 && n.passiveIncome == 0 &&
                    n.popGrowthPct == 0 && n.migrationRate == 0 && n.indoctrinationPct == 0 &&
                    n.conscriptionPct == 0 && n.artilleryType.empty())
                    continue;
                // ── WHAT "want" HAS TO MATCH (OD_RESEARCH_FOCUS) ──
                //
                // FOCUS is {"buildings", "army", "navy"} and this compared
                // against the node's CATEGORY only. "navy" is a SUBCATEGORY
                // (its category is "army"), so the third research action has
                // never once selected a navy node in the history of this
                // file: it always fell through to "cheapest node anywhere".
                // That is the same silent-degradation shape as the capped
                // industry bug above, and it is why the navy branch advances
                // by accident when it advances at all.
                //
                // And research has grown a "formations" category -- the three
                // nodes that unlock militia, assault infantry and mechanised.
                // Nothing pointed at it, so the AI could only reach the kinds
                // by stumbling over them as the cheapest thing available.
                // Formations are what an ARMY is made of, so the army focus
                // now covers them; that keeps three actions and needs no new
                // net shape.
                // OFF by default and the bug is REAL: fixing it costs every
                // frozen model (265->220, 238->189, 202->198, 212->200)
                // because they were fitted where this action meant "cheapest
                // node". Ship it WITH a retrain; see journal 66.
                static const bool focusFix = std::getenv("OD_RESEARCH_FOCUS") &&
                                             atoi(std::getenv("OD_RESEARCH_FOCUS")) != 0;
                const bool wanted = focusFix
                    ? (n.category == want || n.subcategory == want ||
                       (strcmp(want, "army") == 0 && n.category == "formations"))
                    : (n.category == want);
                if (wanted) {
                    if (n.cost < bestCost) { bestCost = n.cost; bestIdx = i; }
                } else if (n.cost < fallbackCost) {
                    fallbackCost = n.cost; fallbackIdx = i;
                }
            }
            int pick = bestIdx >= 0 ? bestIdx : fallbackIdx;
            if (pick < 0) {
                statsFor(cid).researchNothingLeft++;
                return didNothing(TextFormat("research: nothing left (%s)", want));
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
        case 1: { // enact the doctrine the mask costed -- see enactablePolicy
            // ONE CHOICE, ASKED IN ONE PLACE. The mask offers this action only
            // when that function finds a doctrine that fits inside what is left
            // of the political budget, so re-deriving the choice here would let
            // the two disagree -- which is how 97.4% of enact decisions came to
            // do nothing.
            const Policy* best = enactablePolicy(cid);
            if (!best) return didNothing("policy: none enactable");
            const std::string pid = best->id;
            g.enactPolicy(cid, pid);
            // The budget this turn has just changed, and a module gets several
            // goes in one -- a stale answer would send the next pick at a
            // doctrine there is no longer room for.
            m_enactCache.erase(cid);
            return "enact policy " + pid;
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
                return didNothing("cancel: none active");
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
            if (bestIdx < 0) return didNothing("cancel: none active");
            std::string pid = g.m_activePolicies[bestIdx].policyId;
            g.cancelPolicy(bestIdx);
            return "cancel policy " + pid;
        }
        case 5: case 6: case 7: { // propose alliance / NAP / guarantee
            static const char* REQ[] = {"request_alliance", "request_nap", "request_guarantee"};
            const char* req = REQ[action - 5];
            const Country* c = g.m_countries.getCountry(cid);
            if (!c) return didNothing("diplo: no country");
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
                const float pAccept = predictAcceptance(fr.enemyCid, req, cid);
                // Floored, so a partner the model currently dislikes is
                // unlikely rather than impossible: a hard zero would let an
                // early, badly-calibrated diplomacy net permanently rule out
                // whole classes of ally and never learn otherwise.
                double worth = std::log1p((double)ea) * (0.15 + 0.85 * pAccept);
                // ── WHILE LEARNING, ASK PEOPLE WE WOULD NOT NORMALLY ASK ──
                //
                // The answering head only ever sees the requests this composer
                // chooses to send, and it sends the strongest partner most
                // likely to say yes. So "should I ally with someone weak, or
                // already in two wars, or who will drag me somewhere?" is a
                // question self-play never puts to it -- the same blindness
                // that let a player walk through the trade head with an offer
                // no AI would ever have made, and the same fix: widen the
                // distribution of situations rather than the answers.
                //
                // Multiplicative noise rather than a random pick, so the
                // ordering is disturbed rather than destroyed: a good partner
                // usually still wins, and the tail gets sampled.
                if (selfPlayLearning()) {
                    std::uniform_real_distribution<double> jitter(0.35, 1.65);
                    worth *= jitter(m_rng);
                }
                if (worth > bestWorth) { bestWorth = worth; targetArmy = ea; target = fr.enemyCid; }
            }
            statsFor(cid).pactTried++;
            if (target < 0) {
                statsFor(cid).pactNoTarget++;
                return didNothing(TextFormat("%s: no suitable target", req));
            }
            const Country* ec = g.m_countries.getCountry(target);
            // The loop above already skips a neighbour we have something
            // pending with, so this refuses nothing in practice -- it is here
            // so the cooldown and the stat are spent on overtures that were
            // actually made.
            if (!g.queueDiplomaticAction({c->isoA3, ec->isoA3, req, 1}))
                return didNothing(TextFormat("%s: already in talks with %s", req, ec->name.c_str()));
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
            // A calming doctrine the country cannot pay for calms nothing: the
            // bankruptcy cascade repeals it and charges unrest for the trouble.
            // OFF by default: gating the calming doctrine on current headroom
            // moved 1914:SWE seed 20260801 from 3.4 to 2.4 and N24 from 227 to
            // 209 (all seats). A calming doctrine the treasury cannot carry
            // still calms; the cascade repeals it later at a price smaller
            // than the rebellion it prevented. Same lesson as capping
            // conciliation (-8.5 paired): the unrest levers are not where to
            // save money. OD_CALM_GATE=1 to measure it again.
            static const bool calmGate = std::getenv("OD_CALM_GATE") && atoi(std::getenv("OD_CALM_GATE")) != 0;
            const float calmHeadroom = !calmGate ? 1e9f : losingGround(cid) ? 0.0f
                                     : std::max(0.0f, inc.total - inc.expenses);
            for (auto& p : g.m_allPolicies) {
                if (!g.canCountryEnactPolicy(cid, p)) continue;
                // publicOpinionShift moves provinces toward the government,
                // which is exactly what the political half of unrest measures.
                float score = 2.0f * p.effect.unrestReduction
                            + 1.0f * std::fabs(p.effect.publicOpinionShift)
                            + 0.5f * p.effect.minorityGrowthRate;
                if (score <= 0.0f) continue;
                if ((float)p.costPerTurn > calmHeadroom) continue;
                if (inc.total > 1.0f) score -= 2.0f * (p.costPerTurn / inc.total);
                if (score > bestScore) { bestScore = score; best = &p; }
            }
            if (!best) return didNothing("calm: no policy would help");
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
            if (st.minorities <= 0) return didNothing("minority: none here");

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
            if (candidates.empty()) return didNothing("minority: none here");

            // Best single change: the largest move in the wanted direction per
            // unit of extra cost. Ties on cost break toward the bigger move.
            const CountryIncomeSnapshot inc = g.computeCountryIncome(cid);
            // Nothing that costs more while losing ground; the free options
            // are still on the table. See AI_LOSS_FREEZE_TURNS.
            const float headroom = losingGround(cid) ? 0.0f
                                 : std::max(0.0f, inc.total - inc.expenses);
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
        case 11: { // buy it instead of invading it
            // WHY THIS EXISTS
            //
            // Trade shipped with a complete RECEIVING half -- the diplomacy net
            // judges an incoming offer on the value of the goods, and the player
            // gets a popup with terms -- and no sending half at all. Nothing in
            // this file ever queued a propose_trade, so AI-to-AI trade could not
            // occur, and feature 112 ("this is a trade") was therefore zero in
            // every self-play turn ever run: the weight on it could never
            // receive a gradient, and no amount of training could teach the AI
            // anything about trade. This is the half that was missing.
            //
            // WHAT IT OFFERS: money, and only money.
            //
            // Land-for-land is a swap CeasefireTerms can express and the deal
            // features cannot yet price. netProv counts provinces, not what they
            // are worth, so a country that traded its industrial core for two
            // border marshes would look to the net like it broke even. Until
            // there is a valuation to trade on, the AI buys and does not barter.
            const Country* c = g.m_countries.getCountry(cid);
            if (!c) return didNothing("trade: no country");

            // What one province costs: a payback period on its own income.
            // Clamped at both ends -- see the constants for why income alone
            // misprices a port that earns nothing and a metropolis that earns
            // more than anyone can pay.
            auto provincePrice = [&](int pid) -> double {
                auto it = g.m_provinceIndustry.find(pid);
                const double perTurn = (it != g.m_provinceIndustry.end())
                                     ? (double)it->second.income : 0.0;
                return std::clamp(perTurn * TRADE_PAYBACK_TURNS,
                                  TRADE_PRICE_PROV_MIN, TRADE_PRICE_PROV_MAX);
            };

            // What we will spend. Capped as a share of the treasury because the
            // offer can still be refused, and a country that emptied itself into
            // a proposal would be defenceless while it waited for an answer.
            const double budget0 = c->treasury * TRADE_MAX_TREASURY_SHARE;
            // Against the CHEAPEST thing on the menu, which is a province at
            // its price floor rather than a claim. Testing the claim price here
            // while the mask tests the province price let the two disagree, and
            // a country between the two figures burned its turn discovering
            // that in here -- exactly the waste the mask's own floor exists to
            // prevent. One number, derived in one place: see TRADE_MIN_TREASURY.
            if (budget0 < std::min(TRADE_PRICE_PROV_MIN, TRADE_PRICE_PER_CLAIM))
                return didNothing("trade: cannot afford anything");

            auto relIt = g.m_relations.find(c->isoA3);
            auto myClaims = g.m_claims.find(c->isoA3);

            int bestTarget = -1; double bestWorth = -1.0;
            CeasefireTerms bestTerms; double bestPrice = 0.0;

            std::unordered_set<int> seen;
            for (auto& fr : m_stats[cid].frontiers) {
                const int tcid = fr.enemyCid;
                if (!seen.insert(tcid).second) continue;
                const Country* ec = g.m_countries.getCountry(tcid);
                if (!ec || ec->isoA3.empty()) continue;
                // At peace, by definition: a proposal made across a live front is
                // a ceasefire, and that is action 6 of the war module.
                if (relIt != g.m_relations.end()) {
                    auto rr = relIt->second.find(ec->isoA3);
                    if (rr != relIt->second.end() && rr->second.war) continue;
                }
                if (!diploReady(cid, tcid)) continue;
                bool pendingReq = false;
                for (auto& da : g.m_pendingDiplomaticActions)
                    if (da.sourceIso == c->isoA3 && da.targetIso == ec->isoA3) { pendingReq = true; break; }
                if (pendingReq) continue;

                // THE ASK, built inside the budget rather than trimmed to fit
                // afterwards. Land first because it is what the country actually
                // wants; renunciations spend whatever is left.
                CeasefireTerms terms;
                double budget = budget0, value = 0.0;

                // Land we claim and they hold. This is the grievance that would
                // otherwise become a war goal.
                //
                // Priced per province off what that province earns, so the offer
                // is proportionate to the thing being bought rather than to a
                // constant that fitted neither end of the map.
                if (myClaims != g.m_claims.end() && budget >= TRADE_PRICE_PROV_MIN) {
                    for (int pid : myClaims->second) {
                        if (terms.theirProvs.size() >= TRADE_MAX_PROVS) break;
                        const int owner = (pid >= 0 && pid < (int)g.m_provinceCountryLookup.size())
                                              ? g.m_provinceCountryLookup[pid] : 0;
                        if (owner != tcid) continue;
                        const double price = provincePrice(pid);
                        if (price > budget) continue;   // a richer one may still fit
                        terms.theirProvs.push_back(pid);
                        budget -= price;
                        value  += price;
                        if (budget < TRADE_PRICE_PROV_MIN) break;
                    }
                }
                // Claims of theirs on our land. Buying one off is the cheapest
                // border security there is -- it removes the stated reason for a
                // war before anyone has to garrison against it.
                auto theirClaims = g.m_claims.find(ec->isoA3);
                if (theirClaims != g.m_claims.end()) {
                    for (int pid : theirClaims->second) {
                        if (terms.theirDropClaims.size() >= TRADE_MAX_CLAIMS) break;
                        if (budget < TRADE_PRICE_PER_CLAIM) break;
                        const int owner = (pid >= 0 && pid < (int)g.m_provinceCountryLookup.size())
                                              ? g.m_provinceCountryLookup[pid] : 0;
                        if (owner != cid) continue;
                        terms.theirDropClaims.push_back(pid);
                        budget -= TRADE_PRICE_PER_CLAIM;
                        value  += TRADE_PRICE_PER_CLAIM;
                    }
                }
                if (terms.theirProvs.empty() && terms.theirDropClaims.empty()) continue;

                // WORTH HAVING, AND LIKELY TO AGREE -- the same product the pact
                // proposals score on, and for the same reason: every refusal
                // costs a turn and a cooldown, so the partner most likely to say
                // yes is worth more than the prize that is merely largest.
                const float pAccept = predictAcceptance(tcid, "propose_trade", cid);
                const double worth = value * (0.15 + 0.85 * pAccept);
                if (worth > bestWorth) {
                    bestWorth  = worth;
                    bestTarget = tcid;
                    bestTerms  = terms;
                    bestPrice  = value;
                }
            }
            if (bestTarget < 0) return didNothing("trade: nothing worth buying nearby");

            const Country* ec = g.m_countries.getCountry(bestTarget);
            // Priced at exactly what was asked for. The money is deducted when
            // the offer RESOLVES, not now: applyCeasefireTerms charges an AI
            // sender itself (alreadyDeducted is player-only), and charging here
            // as well would bill the country twice for one deal.
            // ...EXCEPT WHILE LEARNING, when the price is deliberately spread
            // from a bare demand to an overpayment. See AI_TRADE_TRAIN_PRICE_*.
            // An overpayment above the treasury is clamped at transfer time by
            // applyCeasefireTerms, so nothing to guard here.
            double askPrice = bestPrice;
            if (selfPlayLearning()) {
                std::uniform_real_distribution<float> k(AI_TRADE_TRAIN_PRICE_MIN,
                                                        AI_TRADE_TRAIN_PRICE_MAX);
                askPrice = bestPrice * (double)k(m_rng);
            }
            bestTerms.ourMoney = (int)askPrice;

            if (!g.queueDiplomaticAction({c->isoA3, ec->isoA3, "propose_trade", 1}))
                return didNothing(TextFormat("trade: already in talks with %s", ec->name.c_str()));
            g.m_pendingCeasefireTerms[c->isoA3 + "|" + ec->isoA3] = bestTerms;
            diploCoolDown(cid, bestTarget);
            statsFor(cid).tradesOffered++;
            return TextFormat("offer trade to %s (%d gold for %zu prov, %zu claim)",
                              ec->name.c_str(), bestTerms.ourMoney,
                              bestTerms.theirProvs.size(), bestTerms.theirDropClaims.size());
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
    // EVERYTHING THAT WILL SHOOT BACK, which is what the resolver now fights.
    //
    // The assault estimate below used to weigh only the province OWNER's
    // troops, because that was all the old resolver fought. resolveAssault
    // counts every stack hostile to the attacker, so an ally of the defender
    // standing on the same province is real defence -- and an estimate that
    // cannot see it sends the army into a battle it has already lost on paper.
    auto hostileGarrisonAt = [&](int pid) -> int {
        auto it = g.m_provinceArmies.find(pid);
        if (it == g.m_provinceArmies.end()) return 0;
        int n = 0;
        for (auto& u : it->second)
            if (u.count > 0 && u.countryId > 0 && u.countryId != cid &&
                !g.alliedCids(cid, u.countryId)) n += u.count;
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
        // ── BY POWER, NOT HEADCOUNT (OD_THREAT_POWER) ──
        //
        // The same correction as garrisonReflex's ranking, on the action that
        // fires most: this lambda orders both RECRUITMENT (the war head's
        // most-picked action, 20,648 of 39,809 on a Norway game) and the
        // reinforce ordering. Raw men ignore fortification, research and
        // supply, all of which the resolver applies when the fight happens.
        // Shares the knob with garrisonReflex deliberately -- two rankings of
        // the same thing that disagreed about what a threat is would be worse
        // than either alone.
        // MEASURED AND OFF, on its own knob so it cannot be switched on by
        // accident with the garrison ranking. Extending the power ranking
        // from garrisonReflex to HERE -- the same correction, on an action
        // that fires eight times more often -- scored 209/73/3 against the
        // narrow version's 250/82/36 and a control of 175/73/9. More
        // frequency is strictly worse: 41 rating, 9 survival and 33 floor
        // worse than applying it to reinforcement alone.
        //
        // So it is not how OFTEN the correction fires, it is WHICH decision
        // it governs. Where reinforcements go is a question about threat;
        // where new men are raised apparently is not, at least not only.
        static const bool threatPower = std::getenv("OD_THREAT_POWER_RECRUIT") &&
                                        atoi(std::getenv("OD_THREAT_POWER_RECRUIT")) != 0;
        float s;
        if (threatPower) {
            const auto ind = g.m_provinceIndustry.find(pid);
            const float fort = ind != g.m_provinceIndustry.end()
                                 ? (float)ind->second.fortification : 0.0f;
            const double ours = (double)garrisonOf(pid, cid) * (1.0 + fort * 0.1) *
                                (1.0 + g.getTotalEffect("armyDefPct", cid) / 100.0) *
                                (double)g.supplyFactor(cid, pid);
            double theirs = 0.0;
            if (nIt != g.m_provinceNeighbors.end())
                for (int nid : nIt->second) {
                    const int nOwner = (nid >= 0 && nid < (int)g.m_provinceCountryLookup.size())
                                           ? g.m_provinceCountryLookup[nid] : 0;
                    if (nOwner <= 0 || nOwner == cid || !atWarWith(nOwner)) continue;
                    theirs += (double)garrisonOf(nid, nOwner) *
                              (1.0 + g.getTotalEffect("armyAtkPct", nOwner) / 100.0) *
                              (double)g.supplyFactor(nOwner, pid);
                }
            s = (float)(theirs - ours);
        } else {
            s = (float)enemy - (float)garrisonOf(pid, cid);
        }
        // Any province with a live enemy opposite outranks every quiet one.
        return enemy > 0 ? s + 1.0e6f : s;
    };

    // ── HOME FIRST (OD_CAMPAIGN_HOMEFIRST, off by default) ──
    //
    // Campaigns are worth +66 rating on N43 and -20 on the worst seat: they
    // win where the seat can afford an offensive and lose where it cannot.
    // The existing answer, recall, closes the whole campaign and gives back
    // more mean than it buys floor, because a commitment that can be
    // abandoned is not one. This is the untested middle. A campaign steers
    // three things -- where new men are raised, where men are moved, and who
    // is attacked -- and only the third is the commitment. So when the home
    // front is losing ground, the campaign keeps AIMING at its victim and
    // stops SOAKING UP the reinforcements: recruit and reinforce fall back
    // to the ordinary threat rule, the attack stays constant.
    static const bool homeFirst = std::getenv("OD_CAMPAIGN_HOMEFIRST") &&
                                  atoi(std::getenv("OD_CAMPAIGN_HOMEFIRST")) != 0;
    const bool campYields = homeFirst && (st.provincesLost > 0 || st.worstDeficit > 0);
    switch (action) {
        case 1: { // recruit in the most threatened frontier province (or richest)
            int pid = -1;
            // A campaign is a commitment, and this is where it becomes one:
            // while it is open, new men are raised where the campaign is
            // staged rather than wherever the threat rule points this turn.
            if (const Game::Campaign* camp = g.campaignOf(cid))
                if (!campYields) pid = camp->stagingProvince;
            if (pid < 0 && !st.frontiers.empty()) {
                // Was st.frontiers[0] — index 0 of a vector built in hash order,
                // i.e. an arbitrary border province unrelated to any threat,
                // despite the comment claiming otherwise.
                float best = -1.0e30f;
                for (auto& fr : st.frontiers) {
                    float s = threatScore(fr.pid);
                    if (s > best) { best = s; pid = fr.pid; }
                }
            }
            else if (pid < 0) {
                // OD_RECRUIT_PICK=1: choose by MEN AVAILABLE, not headcount.
                //
                // "recruit: too poor/small" is 104,895 refusals in two 400-turn
                // games, 48% of every refused execution in the AI, and this is
                // why: the fallback picks the most POPULOUS province, then the
                // line below computes availableManpower(pid) -- what is left to
                // conscript after orders already placed -- and refuses when
                // manpower/5 is under 1000. The biggest province is exactly the
                // one that gets conscripted out first, so the AI returns to it
                // every turn and is refused every turn.
                //
                // Tightening the MASK against this does not help (measured:
                // 108,650 -> 104,895 refusals) because the mask can only ask
                // whether SOME province would serve, while the executor commits
                // to this one. The choice is what is wrong, not the gate.
                static const bool pickByManpower = std::getenv("OD_RECRUIT_PICK") &&
                                                   atoi(std::getenv("OD_RECRUIT_PICK")) != 0;
                long long bp = -1;
                for (int p2 : g.provincesOf(cid)) {
                    long long key = pickByManpower
                                  ? g.availableManpower(p2)
                                  : (g.m_provincePopulations.count(p2) ? g.m_provincePopulations[p2] : 0);
                    if (key > bp) { bp = key; pid = p2; }
                }
            }
            if (pid < 0) return didNothing("recruit: no province");
            // What is left to conscript, not the headcount: see
            // Game::availableManpower. Orders already placed this turn have
            // spent part of the pool even though the men have not arrived yet.
            const long long pop = g.availableManpower(pid);
            long long maxRecruit = pop / 5;
            // Spend at most 20% of treasury on this order. Clamp BEFORE the
            // cast: a runaway treasury times 10000 overflows long long (UB).
            long long budgetCount = (long long)std::min((double)INT32_MAX,
                                                        c.treasury * 0.20 * 10000.0);
            int count = (int)std::min((long long)INT32_MAX,
                                      std::min(maxRecruit, budgetCount));
            if (count < 1000) return didNothing("recruit: too poor/small");
            // ONE PRICE, ASKED IN ONE PLACE. This used to apply
            // conscriptionCostMod itself -- correctly, after the "armyCostPct
            // is not a real effect name" bug was fixed here and in the panel
            // separately -- and a recruit now costs MUNITIONS as well, which
            // would have been a third place to get it right. Game::recruitPrice
            // carries the modifier, the $1 floor and the materials together.
            const TroopType kind = chooseTroopType(cid);
            const Game::WarPrice price = g.recruitPrice(count, cid, kind);
            if (!g.payWarMaterials(cid, price))
                return didNothing("recruit: no munitions");
            c.treasury -= price.money;
            money::add(money::BUY_TROOPS, -(double)price.money);
            g.m_pendingRecruitments.push_back({pid, count, 1, kind});
            return TextFormat("recruit %d in prov %d ($%.0f%s)", count, pid, price.money,
                              price.munitions > 0.005f
                                  ? TextFormat(", %.1f mun", price.munitions) : "");
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
            if (ranked.empty()) return didNothing("reinforce: no frontier");
            std::sort(ranked.rbegin(), ranked.rend());
            int issued = 0;
            // The staging province first while a campaign is open: the
            // commitment decides where force goes, not the turn's worst
            // frontier. Everything after it is the ordinary order.
            if (const Game::Campaign* camp = g.campaignOf(cid))
                if (!campYields && reinforceProvince(cid, camp->stagingProvince)) ++issued;
            for (auto& [score, dstPid] : ranked) {
                if (issued >= MAX_REINFORCE_ORDERS) break;
                if (!reinforceProvince(cid, dstPid)) continue;
                ++issued;
            }
            if (!issued) return didNothing("reinforce: nothing to move");
            return TextFormat("reinforce %d province(s), worst prov %d", issued, ranked[0].second);
        }
        case 3: { // attack the weakest adjacent at-war enemy province we can beat
            // ONE SCAN, ASKED IN ONE PLACE. The mask offers this action only
            // when attackCandidates finds a winnable assault from a province
            // that is not already carrying an order, so re-deriving the list
            // here would let the two disagree again -- which is exactly how
            // 78% of attack decisions came to do nothing. See attackCandidates.
            const std::vector<AttackCandidate>& scan = attackCandidates(cid);
            std::vector<AttackCandidate> cands(scan.begin(), scan.end());
            int bestFrom = -1, bestTo = -1; float bestMargin = 1.05f;
            bool fromAlly = false;
            // A campaign's target is attacked while the campaign is open,
            // even when another candidate looks better this turn -- that
            // constancy is the whole point of the commitment.
            if (const Game::Campaign* camp = g.campaignOf(cid)) {
                float campBest = 1.0f;
                for (const AttackCandidate& ch : cands)
                    if (ch.enemyCid == camp->targetCountry && ch.margin > campBest) {
                        campBest = ch.margin; bestMargin = ch.margin;
                        bestFrom = ch.fromPid; bestTo = ch.toPid; fromAlly = ch.fromAlly;
                    }
            }
            for (const AttackCandidate& ch : cands)
                if (bestTo < 0 && ch.margin > bestMargin) {
                    bestMargin = ch.margin; bestFrom = ch.fromPid; bestTo = ch.toPid;
                    fromAlly = ch.fromAlly;
                }
            (void)fromAlly;
            if (bestFrom < 0) {
                statsFor(cid).attackNoTarget++;
                return didNothing("attack: no winnable target");
            }
            // WHICH of them, and IN WHAT ORDER. The ranking is empty while the
            // head is warming up, in which case the margin rule's ordering
            // stands -- exactly what this code did before, just applied to more
            // than the winner.
            std::vector<int> order;
            {
                const int pick = chooseAttack(cid, cands, &order);
                if (pick >= 0) {
                    statsFor(cid).attackSteered++;
                } else {
                    if (!m_pendingAttackCand.empty()) {
                        // Below warmup: record WHICH candidate the rule took, so
                        // the head learns from a policy that already works.
                        // Without this the samples carry inputs and no label.
                        for (size_t i = 0; i < cands.size() &&
                                           i < m_pendingAttackCand.size(); ++i)
                            if (cands[i].fromPid == bestFrom && cands[i].toPid == bestTo) {
                                m_pendingAttackChosen = (int)i;
                                break;
                            }
                    }
                    // The rule's own preference order: best margin first.
                    order.resize(cands.size());
                    for (size_t i = 0; i < cands.size(); ++i) order[i] = (int)i;
                    std::sort(order.begin(), order.end(),
                              [&](int a, int b) { return cands[a].margin > cands[b].margin; });
                }
            }

            // ONE ORDER PER FRONT, up to the cap. See ATTACK_ORDERS_PER_TURN.
            // A province that already has a move order queued is skipped rather
            // than abandoning the whole action, which is what made "attack:
            // order pending" a wasted turn: with one order to give, a single
            // busy province meant the country did nothing at all.
            // ── Several prongs from one province, each sized to its target ──
            //
            // This used to send a flat 75% of the garrison at exactly one
            // target per province: `usedFrom` rejected any second candidate
            // launching from the same ground, and the 75 was a literal. Both
            // were capabilities the PLAYER has and the AI did not -- a person
            // can split a garrison across two borders and pick each
            // percentage by hand -- and parity is the rule here.
            //
            // The size of each prong is derived from the same combat
            // arithmetic that decided the candidate was winnable: send the
            // force that clears ATTACK_SAFETY, not a fixed fraction. margin
            // was computed at 0.75 of the garrison, and attack power is linear
            // in the troops committed, so the fraction that lands exactly on
            // the safety bar is 0.75 * SAFETY / margin. An overwhelming
            // target-- margin 4x -- therefore commits under a fifth of the
            // garrison and leaves the rest to open a second front, which is
            // the entire point of doing this.
            //
            // Percentages are of the CURRENT garrison because the resolver
            // applies them sequentially (Game_TurnLogic: toMove = count * pct,
            // order after order), so two 50s send half and then a quarter.
            // Tracking commitment as a fraction of the ORIGINAL and dividing
            // by what is left converts between the two.
            // 1.25 SURVIVED A SWEEP, AND THE SWEEP FOUND NOTHING TO BEAT IT.
            //
            // Swept 1.05 / 1.15 / 1.25 / 1.40 / 1.60 / 2.00 over all six
            // shipped scenarios at 300 turns, with the scripted rung pinned at
            // 1.25 so the opponent stayed still while the model's value moved.
            // The first pass looked bimodal -- 1.52x at 1.05 and 1.47x with a
            // 1.01x trough at 1.25 -- and every bit of that was noise. Three
            // fresh seeds per value:
            //     1.05  1.52 0.95 1.15 1.20   mean 1.21  SD 0.24
            //     1.25  1.01 1.31 1.15 1.38   mean 1.21  SD 0.17
            //     1.60  1.47 1.16 1.04 1.27   mean 1.24  SD 0.18
            // The standard error on each mean is about three times the largest
            // gap between them, and successful assaults (issued minus
            // repulsed) are equally flat across the whole range.
            //
            // What DOES move monotonically is the mechanism: assaults issued
            // fall 15428 -> 12773 and the repulse rate falls 57% -> 45% as this
            // rises. So the constant does exactly what it is meant to; the
            // outcome simply does not care where in this range it sits.
            //
            // Do not re-sweep this against ADVANTAGE hoping for a sharper
            // answer -- resolving a 0.03 difference against a 0.2 SD needs
            // hundreds of runs. If it ever needs settling, it needs a
            // lower-variance metric, not more seeds.
            // ── HOW MANY MEN A BATTLE IS WORTH, BEFORE THE FIRST ROUND ──
            //
            // With standing battles this is the decision that settles a fight:
            // 356 measured rounds have the attacker at a MEDIAN 0.73 of the
            // defender's power, killing 0.74 men per man lost, and winning
            // anyway -- because the surplus outlasts the garrison. Everything
            // after the commitment is arithmetic. Knobs so the size can be
            // measured rather than argued: OD_ATTACK_SAFETY (the odds the
            // prong is sized to clear) and OD_ATTACK_MAX_COMMIT (the share of
            // a garrison a province may ever send).
            static const float ATTACK_SAFETY = std::getenv("OD_ATTACK_SAFETY") ? (float)atof(std::getenv("OD_ATTACK_SAFETY")) : 1.25f;
            static const float ATTACK_MAX_COMMIT = std::getenv("OD_ATTACK_MAX_COMMIT") ? (float)atof(std::getenv("OD_ATTACK_MAX_COMMIT")) : 0.85f;
            constexpr int   PRONGS_PER_PROVINCE = 3;
            int issued = 0, blocked = 0;
            std::unordered_map<int, float> committed;   // fromPid -> fraction of original
            std::unordered_map<int, int>   prongs;      // fromPid -> orders issued
            // Provinces already carrying an order from an earlier decision are
            // off limits, but an order pushed by THIS loop must not lock the
            // province against its own second prong -- so the set is taken once,
            // before anything is queued.
            std::unordered_set<int> preOrdered;
            for (auto& mo : g.m_pendingMoveOrders)
                if (mo.countryId == cid) preOrdered.insert(mo.fromProvince);
            const AttackCandidate* first = nullptr;
            for (int idx : order) {
                if (issued >= ATTACK_ORDERS_PER_TURN) break;
                if (idx < 0 || idx >= (int)cands.size()) continue;
                const AttackCandidate& ch = cands[idx];
                if (preOrdered.count(ch.fromPid)) { blocked++; continue; }
                if (prongs[ch.fromPid] >= PRONGS_PER_PROVINCE) continue;
                if (ch.margin <= 0.0f) continue;
                const float already = committed[ch.fromPid];
                float need = 0.75f * ATTACK_SAFETY / ch.margin;   // of the ORIGINAL garrison
                // ── THE CLAMP COMES FIRST, AND THAT IS NOT A BUG ──
                //
                // It looks like one: `need` is the share required to win with
                // ATTACK_SAFETY's 1.25 margin, clamping it to
                // ATTACK_MAX_COMMIT rounds 0.89 down to 0.85, and the test
                // below then compares the rounded value -- so an assault at
                // margin 1.05 goes in with less than the line above asked for.
                //
                // TRIED, testing before the clamp so those attacks are skipped:
                //
                //     seat                 before  after
                //     modern China              5     16
                //     1939 Norway, one rusher 144     54
                //     rating                  129    118
                //
                // What the clamp actually means is "commit what you have, up to
                // the cap" -- the attack still wins at a thinner safety factor
                // (1.19 rather than 1.25), it is not a fight that must be lost.
                // Skipping those attacks costs more than it saves, most sharply
                // on the seat that has the fewest chances to take ground.
                need = std::clamp(need, 0.10f, ATTACK_MAX_COMMIT);
                if (need > ATTACK_MAX_COMMIT - already) continue;  // not enough left to win
                // `need` is a share of the ORIGINAL garrison and an order's pct
                // now means exactly that, so it goes straight across.
                //
                // It used to be divided by what previous prongs had left
                // behind, because the resolver took each share off a shrinking
                // pool -- the same arithmetic that made two 50% orders move
                // 75% of an army. The resolver was the thing that was wrong;
                // see processArmyMovement. Compensating for it here also meant
                // the AI and the player's move panel meant different things by
                // the same number.
                const int pct = (int)std::lround(100.0f * need);
                if (pct <= 0 || pct > 100) continue;
                g.m_pendingMoveOrders.push_back({ch.fromPid, ch.toPid, pct, cid});
                {
                    static const int traceCid = std::getenv("OD_ECON_TRACE") ? atoi(std::getenv("OD_ECON_TRACE")) : -1;
                    if (traceCid == cid)
                        fprintf(stderr, "[ATTACK-ORDER] turn %d cid=%d from=%d to=%d pct=%d myG=%d defG=%d margin=%.2f\n",
                                m_turn, cid, ch.fromPid, ch.toPid, pct, ch.myGarrison, ch.theirGarrison, ch.margin);
                }
                committed[ch.fromPid] = already + need;
                prongs[ch.fromPid]++;
                statsFor(cid).attackIssued++;
                if (!first) first = &ch;
                issued++;
            }
            if (issued == 0) {
                statsFor(cid).attackPending += blocked;
                return didNothing("attack: every launch province already has orders");
            }
            if (issued == 1)
                return TextFormat("attack prov %d from %s%d (margin %.1fx)",
                                  first->toPid, first->fromAlly ? "allied prov " : "",
                                  first->fromPid, first->margin);
            return TextFormat("attack on %d fronts (best: prov %d from %d, margin %.1fx)",
                              issued, first->toPid, first->fromPid, first->margin);
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
                return didNothing("war: no suitable target");
            const Country* ec = g.m_countries.getCountry(wt.cid);
            if (!ec) return didNothing("war: target vanished");

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
                if (!g.queueDiplomaticAction({c.isoA3, ec->isoA3, "break_nap", 1}))
                    return std::string("war: already in talks with ") + ec->name;
                return std::string("break NAP with ") + ec->name +
                       (wt.naval ? " (naval war next turn)" : " (war next turn)");
            }

            // What this war is really for, kept to ourselves, and what we tell
            // the world it is for, which may be a different thing. Decided here
            // rather than when the declaration resolves next turn: this is the
            // state the country decided from.
            {
                // Asked BEFORE the goal is chosen, not after. findWarTarget has
                // already ruled out a target we are mid-sentence with and a
                // country that has declared once this turn, so this only ever
                // fires as a backstop -- but chooseStatedWarGoal draws from the
                // RNG and moves the honesty counters, and a declaration that
                // never happens must cost neither. (The RNG stream is replayed
                // by determinism_check.sh; a stray draw is a desync.)
                // A refused declaration is a NO-OP, not an action: didNothing
                // sets m_execNoop so the choice is counted as one the executor
                // could not honour (noopChosen), rather than as a war declared.
                // The pre-check mirrors queueDiplomaticAction's own rule (one
                // declaration per country per turn, any pending talk with the
                // pair); the return value is checked too, so no refusal is
                // silent. Same shape as the validWar artillery table.
                if (g.hasPendingDiplomacy(c.isoA3, ec->isoA3) ||
                    g.hasPendingDeclaration(c.isoA3))
                    return didNothing(std::string("war: already in talks with ") + ec->name);
                PendingDiplomaticAction pda{c.isoA3, ec->isoA3, "declare_war", 1};
                const int truth = trueWarGoal(cid, wt.cid);
                pda.statedGoal = chooseStatedWarGoal(cid, wt.cid, truth);
                if (!g.queueDiplomaticAction(std::move(pda)))
                    return didNothing(std::string("war: declaration refused for ") + ec->name);
            }
            statsFor(cid).warsDeclared++;
            m_declaredUnprovoked = !wt.claimed;
            if (wt.naval)
                return std::string("declare NAVAL war on ") + ec->name +
                       (wt.claimed ? " (claims)" : " (overseas)");
            return std::string("declare war on ") + ec->name +
                   (wt.claimed ? " (claims)" : "");
        }
        case 5: { // artillery: best researched ammo on an adjacent enemy province
            // The AI kept its own copy of the artillery price list -- two of
            // them, in fact, this one and the naval bombard case below -- and
            // both are gone. Only the RESEARCH NODE mapping is the AI's own
            // business; the prices come from ARTY_COSTS in BuildCosts.h, so a
            // shell costs the AI what it costs the player, in every currency.
            struct Ammo { const char* type; const char* node; };
            static const Ammo AMMO[] = {
                {"nuclear", "arty6a"}, {"biological", "arty6b"},
                {"chemical", "arty5"}, {"napalm", "arty4a"},
                {"carpet", "arty4b"},  {"heavy", "arty3"},
                {"light", "arty2"},    {"mortar", "arty1"}};
            const Ammo* use = nullptr;
            Game::WarPrice usePrice;
            for (auto& a2 : AMMO) {
                if (!g.hasResearched(a2.node, cid)) continue;
                const Game::WarPrice p2 = g.artilleryPrice(a2.type, cid);
                if (c.treasury < p2.money) continue;
                // Materials as well as money: an AI that queued a shell it had
                // no munitions for would have the order refused downstream and
                // waste the action, which is exactly the sort of silent
                // no-op the mask is supposed to prevent.
                if (!g.canAffordWarMaterials(cid, p2)) continue;
                use = &a2; usePrice = p2; break;
            }
            if (!use) return didNothing("artillery: no researched ammo");
            // ── SHELLS GO WHERE THE WAR IS (OD_CAMPAIGN_GUNS) ──
            //
            // A shell is spent this turn and its effect lands this turn, so
            // by the horizon rule a twelve-turn commitment may direct it --
            // unlike a factory, which outlives the campaign and measured a
            // 40-point loss when the campaign chose its province. The
            // ordinary rule fires at the first at-war neighbour the frontier
            // list happens to reach; while a campaign is open, the target's
            // ground is shelled first and everything else is the fallback.
            // MEASURED AND OFF: N24 265 -> 234, N37 238 -> 218. Shells were
            // already going where they were needed; see journal 66.
            static const bool campGuns = std::getenv("OD_CAMPAIGN_GUNS") &&
                                         atoi(std::getenv("OD_CAMPAIGN_GUNS")) != 0;
            const Game::Campaign* guncamp = campGuns ? g.campaignOf(cid) : nullptr;
            for (int pass = guncamp ? 0 : 1; pass < 2; ++pass)
            for (auto& fr : st.frontiers) {
                if (!atWarWith(fr.enemyCid)) continue;
                if (pass == 0 && fr.enemyCid != guncamp->targetCountry) continue;
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
                    c.treasury -= usePrice.money;
                    g.payWarMaterials(cid, usePrice);
                    g.m_pendingArtilleryOrders.push_back({fr.pid, nid, use->type});
                    return TextFormat("%s shell prov %d", use->type, nid);
                }
            }
            return didNothing("artillery: no target");
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
            if (target < 0) return didNothing("ceasefire: no war to end");
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

            auto provsOf = [&](int owner, int adjacentTo, int maxN,
                               std::vector<int>* into = nullptr) {
                std::vector<int> local;
                std::vector<int>& out = into ? *into : local;
                for (int pid : g.provincesOf(owner)) {
                    if ((int)out.size() >= maxN) break;
                    if (std::find(out.begin(), out.end(), pid) != out.end()) continue;
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

            // WHAT THE WAR WAS FOR, at the table where it ends.
            //
            // These terms used to be composed with no reference to the war goal
            // at all: "demand provinces" meant provsOf(), which walks the
            // defender's territory in map order and takes the first N that
            // happen to touch us. So a war declared to recover a specific claim
            // was settled for whichever province the iterator reached first,
            // and the goal -- the thing the AI privately holds, the thing a
            // player is supposed to deduce -- never reached the negotiation.
            //
            // Claimed land is asked for FIRST, and a country fighting to
            // recover it will not trade the claim away. That is what makes the
            // goal observable: not a label on a panel, but a settlement that
            // consistently bends around the same provinces.
            const int warGoal = trueWarGoal(cid, target);
            auto claimedProvsOf = [&](int owner, const std::string& claimant, int maxN,
                                      std::vector<int>& out) {
                for (int pid : g.provincesOf(owner)) {
                    if ((int)out.size() >= maxN) break;
                    auto clIt = g.m_claimsByProvince.find(pid);
                    if (clIt == g.m_claimsByProvince.end()) continue;
                    for (const auto& iso : clIt->second)
                        if (iso == claimant) { out.push_back(pid); break; }
                }
            };

            const char* posture;
            if (edge > 1.5) {
                // Winning: take something for stopping. Demand border provinces
                // (capped so a victory doesn't annex a whole country in one
                // deal) and a slice of their treasury.
                posture = "demanding";
                const int wantN = edge > 3.0 ? 3 : 1;
                // The claim first, then whatever border land is left over.
                claimedProvsOf(target, c.isoA3, wantN, terms.theirProvs);
                provsOf(target, cid, wantN, &terms.theirProvs);
                // Measured, not assumed: see TrainStats::ceasefireProvsAsked.
                {
                    TrainStats& st2 = statsFor(cid);
                    st2.ceasefireProvsAsked += (long long)terms.theirProvs.size();
                    for (int pid : terms.theirProvs) {
                        auto clIt2 = g.m_claimsByProvince.find(pid);
                        if (clIt2 == g.m_claimsByProvince.end()) continue;
                        for (const auto& iso : clIt2->second)
                            if (iso == c.isoA3) { st2.ceasefireClaimedAsked++; break; }
                    }
                }
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
                // A country losing a war of RECOVERY does not buy peace by
                // renouncing the thing it went to war for. It pays, it cedes
                // ground elsewhere, and it keeps the claim -- which is the
                // whole of what "this war was about Danzig" looks like from the
                // other side of the table, and is exactly how a player is meant
                // to work out what the war was about.
                if (warGoal == WAR_GOAL_RECONQUEST) statsFor(cid).ceasefireHeldClaim++;
                if (warGoal != WAR_GOAL_RECONQUEST) {
                    auto clIt = g.m_claims.find(c.isoA3);
                    if (clIt != g.m_claims.end())
                        for (int pid : clIt->second) {
                            if (terms.ourDropClaims.size() >= 3) break;
                            int owner = (pid >= 0 && pid < (int)g.m_provinceCountryLookup.size())
                                            ? g.m_provinceCountryLookup[pid] : 0;
                            if (owner == target) terms.ourDropClaims.push_back(pid);
                        }
                }
                if (edge < 0.4) provsOf(cid, target, 1, &terms.ourProvs);
            } else {
                posture = "white peace"; // evenly matched — no demands
            }

            // Same reasoning as the pact proposals: the target loop already
            // skipped anyone we have an offer out to, and the terms below, the
            // cooldown and the stat all belong to an offer that was sent.
            if (!g.queueDiplomaticAction({c.isoA3, ec->isoA3, "request_ceasefire", 1}))
                return didNothing(TextFormat("ceasefire: already in talks with %s", ec->name.c_str()));
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
            if (bestFrom < 0) return didNothing("stage: no allied crossing available");
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
bool AISystem::reinforceProvince(int cid, int dstPid, long long want) {
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
    // SOURCE: the neighbour with the most men, THREATENED OR NOT.
    //
    // A comment here used to claim "never strip a province that is itself
    // under threat"; no such check has ever existed. Corrected rather than
    // implemented, because implementing it was measured and is not clearly
    // better: OD_REINF_GUARD (off) skips neighbours with an enemy stack
    // adjacent, and across two models and three seed sets it is +34/+19 on
    // the fitted seeds, +24/+8 on one hold-out set, and -1/-11 on another,
    // where it also costs N24 two thirds of its worst seat (87 -> 21).
    // Mean +12 rating, -11 floor. Another trade, not a fix.
    //
    // What IS established: the flat 50 below is load-bearing. It performs
    // this guard by accident, because moving fifty men cannot strip anything.
    // Sizing the move to the deficit (OD_REINF_SIZED, off) removes that
    // accident and takes the worst seat to ZERO on both models. Do not make
    // the quantity dynamic without solving source selection properly first --
    // they are one rule, and this pair is the evidence.
    static const bool guard = std::getenv("OD_REINF_GUARD") &&
                              atoi(std::getenv("OD_REINF_GUARD")) != 0;
    auto atRisk = [&](int pid) -> bool {
        if (!guard) return false;
        auto wIt = m_warWith.find(cid);
        if (wIt == m_warWith.end() || wIt->second.empty()) return false;
        auto pn = g.m_provinceNeighbors.find(pid);
        if (pn == g.m_provinceNeighbors.end()) return false;
        for (int nb : pn->second) {
            const int owner = (nb >= 0 && nb < (int)g.m_provinceCountryLookup.size())
                                  ? g.m_provinceCountryLookup[nb] : 0;
            if (owner > 0 && owner != cid && wIt->second.count(owner) &&
                garrisonOf(nb, owner) > 0)
                return true;
        }
        return false;
    };
    int srcPid = -1; long long srcG = 0;
    for (int nid : nIt->second) {
        if (nid < 0 || nid >= (int)g.m_provinceCountryLookup.size() ||
            g.m_provinceCountryLookup[nid] != cid) continue;
        if (atRisk(nid)) continue;   // see the guard above
        long long gsz = garrisonOf(nid, cid);
        if (gsz > srcG) { srcG = gsz; srcPid = nid; }
    }
    if (srcPid < 0 || srcG < 200) return false;
    for (auto& mo : g.m_pendingMoveOrders)
        if (mo.fromProvince == srcPid && mo.countryId == cid) return false;
    // ── HOW MANY MEN, NOT JUST WHERE ──
    //
    // This moved a flat 50 no matter what was asked for. garrisonReflex ranks
    // frontier provinces by the exact number of men they are outnumbered BY,
    // sorts on it, and then calls this -- which throws that number away and
    // sends 50. The rule computes the right quantity and discards it, which
    // is the same defect as re-deriving a resolver's arithmetic, seen from the
    // other side.
    //
    // Sized: send what was asked for, never more than half the source
    // garrison (a province emptied to feed a neighbour is the next deficit),
    // never less than the old 50 so that no call gets weaker than before.
    // want <= 0 keeps the flat behaviour, which is what the campaign and
    // ordinary reinforce paths pass -- they have no deficit to quote.
    static const bool sized = std::getenv("OD_REINF_SIZED") &&
                              atoi(std::getenv("OD_REINF_SIZED")) != 0;
    long long send = 50;
    if (sized && want > 0)
        send = std::max(50LL, std::min<long long>(want, srcG / 2));
    g.m_pendingMoveOrders.push_back({srcPid, dstPid, (int)send, cid});
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
        // ── RANKED BY POWER, NOT BY HEADCOUNT (OD_THREAT_POWER, off) ──
        //
        // WITHDRAWN. Measured at 400 turns on three hold-out sets, each
        // against its own control on one binary:
        //     set C  175 -> 250   surv +9   floor +27
        //     set A  221 -> 168   surv -16  floor -29
        //     set D  183 -> 244   surv  -9  floor -19
        // Mean rating +28, sign flipped between sets, and survival AND floor
        // negative on two of three -- including set D, where rating gained
        // 61. The +75 that made this look like the session's one real gain
        // was a property of set C. Kept as a knob and as evidence; do not
        // switch it on without re-measuring all three.
        //
        // This ranks by raw men -- their garrison minus ours -- while the
        // resolver decides the fight by POWER, which carries fortification,
        // both sides' research and supply. A province behind a level 3 fort
        // is safer than its headcount says and gets over-reinforced; an
        // unsupplied neighbour is less dangerous than its headcount says and
        // pulls men away from a real threat. Same class as the supply term
        // missing from attackCandidates: a quantity the resolver already
        // computes that the AI declines to use.
        //
        // The multipliers are asked of the game rather than re-derived, so
        // the two cannot drift. Depth and frontage are left out on purpose:
        // the attack scan omits them symmetrically by measurement (see
        // OD_WIDTH_MARGIN), and this rule should not disagree with that one.
        static const bool threatPower = std::getenv("OD_THREAT_POWER") &&
                                        atoi(std::getenv("OD_THREAT_POWER")) != 0;
        long long deficit;
        if (threatPower) {
            const auto ind = g.m_provinceIndustry.find(fr.pid);
            const float fort = ind != g.m_provinceIndustry.end()
                                 ? (float)ind->second.fortification : 0.0f;
            double ourPower = (double)garrisonOf(fr.pid, cid) * (1.0 + fort * 0.1) *
                              (1.0 + g.getTotalEffect("armyDefPct", cid) / 100.0) *
                              (double)g.supplyFactor(cid, fr.pid);
            double enemyPower = 0.0;
            for (int nid : nIt->second) {
                const int nOwner = (nid >= 0 && nid < (int)g.m_provinceCountryLookup.size())
                                       ? g.m_provinceCountryLookup[nid] : 0;
                if (nOwner <= 0 || nOwner == cid || !warIt->second.count(nOwner)) continue;
                enemyPower += (double)garrisonOf(nid, nOwner) *
                              (1.0 + g.getTotalEffect("armyAtkPct", nOwner) / 100.0) *
                              (double)g.supplyFactor(nOwner, fr.pid);
            }
            deficit = (long long)(enemyPower - ourPower);
        } else {
            deficit = enemy - garrisonOf(fr.pid, cid);
        }
        if (deficit > 0) deficits.push_back({deficit, fr.pid});
    }
    if (deficits.empty()) return;
    std::sort(deficits.rbegin(), deficits.rend());
    int issued = 0;
    for (auto& [deficit, pid] : deficits) {
        if (issued >= MAX_GARRISON_ORDERS) break;
        if (reinforceProvince(cid, pid, deficit)) ++issued;
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
// ── THE FORT THE ECONOMY HEAD WILL NOT BUY ──
//
// The econ head's marginal probability on `fort` is exactly 0.0 across 173
// offers, along with port, specialize, destroyer and carrier: eight of its
// twelve actions are dead. A mask cannot make a collapsed head take an action
// -- the note in bestEmbarkPort says the same about ports -- so this buys the
// wall the way garrisonReflex mans the border, using execEconomy case 2's own
// selection rule (most threatened frontier, weakest walls first).
//
// Measured on the frozen model at Insane, five worlds, paired: land against a
// relentless rusher 24.5% -> 29.3%, BETTER ON ALL FIVE WORLDS. Against the
// scripted rung 60.0% -> 56.9%, which the band calls not separable but which
// was worse on all five, so read it as a real cost of about three points.
// Taken deliberately: the rusher is the case the AI loses and the rung is one
// it wins comfortably either way.
//
// TRIED, and it is worse: fire only where the enemy army exceeds the garrison
// actually standing at that frontier -- "a country that is winning does not
// need walls", which is a better story than this one has. It gives up exactly
// what makes this worth keeping: +2.7 on the rusher against +4.7, and helping
// on 3 worlds of 5 rather than 5. On the one seed both were measured on first
// the gate looked strictly better on both scenarios (64.7 and 34.1); it is not.
// Consistency across worlds is the signal here, and the plain rule has it.
void AISystem::fortifyReflex(int cid) {
    Game& g = *m_g;
    Country* c = g.m_countries.getCountry(cid);
    if (!c) return;
    if (foreignWarCount(cid) <= 0) return;
    const CountryStat& st = m_stats[cid];
    int bestPid = -1, bestLvl = 0; double bestScore = -1;
    const int cap = fortCap(cid);
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
    if (bestPid < 0) return;
    const int nextLv = bestLvl + 1;
    const float cost = (float)FORT_COST[std::min(nextLv, 5)] *
                       buildCostMod(g.getTotalEffect("industryCostPct", cid));
    if (c->treasury < cost) return;
    c->treasury -= cost;
    money::add(money::BUY_FORT, -(double)cost);
    g.m_pendingUpgrades.push_back({bestPid, "fortification", nextLv, 1});
}

// OD_LOSS_FREEZE (turns, default AI_LOSS_FREEZE_TURNS; 0 = off) and
// OD_CRASH_CUTS (default AI_AUSTERITY_MAX_CUTS; 1 = one cut a turn, as before)
// exist so the two rules can be benched separately on one binary.
static int lossFreezeTurns() {
    static const int v = std::getenv("OD_LOSS_FREEZE") ? atoi(std::getenv("OD_LOSS_FREEZE"))
                                                       : 0;   // OFF by default: measured neutral (211 vs 209) and it blocks conciliation
    return v;
}
// OD_AUSTERITY_RESEARCH_LAST=1 moves the research cut from FIRST to LAST.
//
// The stated reason research comes down first is that it "comes back up for
// free the moment income recovers". That assumes the deficit is episodic.
// It is not -- AI treasuries run at zero, so austerity fires again and again
// and the slider ratchets down 0.15 a turn with nothing pushing it back up.
// Ablating the whole reflex gains 15.0 points of the world (SWE 9.8 -> 19.7)
// while costing bankrupt CHN 4.2, so the cuts ARE load-bearing for a failing
// country and ruinous for a growing one. This keeps the solvency floor and
// spares the growth engine.
static bool austerityResearchLast() {
    static const bool v = !std::getenv("OD_AUSTERITY_RESEARCH_LAST") ||
                          atoi(std::getenv("OD_AUSTERITY_RESEARCH_LAST")) != 0;
    return v;
}

static int crashCuts() {
    static const int v = std::getenv("OD_CRASH_CUTS") ? atoi(std::getenv("OD_CRASH_CUTS"))
                                                      : 1;   // one cut a turn: 6 measured 181 vs 209 -- the deep cascade destroys more than it saves
    return v;
}

bool AISystem::losingGround(int cid) const {
    if (lossFreezeTurns() <= 0) return false;
    auto it = m_lastLossTurn.find(cid);
    return it != m_lastLossTurn.end() && m_turn - it->second <= lossFreezeTurns();
}

void AISystem::austerityReflex(int cid) {
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    if (!c) return;

    CountryIncomeSnapshot inc = g.computeCountryIncome(cid);

    // ── WHY THIS IS STILL A PLAIN SOLVENCY TEST ──
    //
    // `net >= 0` reads as "paying its way", and against a rush it is the reason
    // this function never runs. Measured against SCRIPT_BLITZ on seed 20260801,
    // per country-turn:
    //
    //                     model    the blitzer
    //     gross           110.93        353.08
    //     standing bill    27.71          4.20   (of which minorities 25.58 / 0.00)
    //     NET               3.26        124.93
    //     spent on war      2.26         33.90
    //
    // The AI is solvent on every one of those turns, so it cuts nothing, while
    // the minority bill runs at EIGHT TIMES its entire war budget. That looks
    // exactly like the explanation for the rush, and it is not one.
    //
    // TRIED: at war, trim minority settlements once the bill exceeds net income
    // (every other branch here guarded off, so the trim was the only change).
    // It did what it was designed to do -- minorities 25.58 -> 17.42, war
    // spending 2.26 -> 2.90, turns with $8 in hand 13.1% -> 19.3% -- and the
    // result did not move: 29.4% -> 29.5% of land against the blitzer, while
    // the ORDINARY game fell 60.6% -> 54.5%. Freeing $8 a turn cannot answer an
    // opponent spending $36. Two things it also showed, worth keeping:
    // pacification rose 0.51 -> 6.63 because the goodwill given up comes
    // straight back as unrest, so a good part of any such saving is refunded to
    // the wrong account; and the gap that actually decides this game is GROSS,
    // 112 against 321, which is mostly the blitzer having already won.
    //
    // WHERE THAT MINORITY BILL COMES FROM, since the obvious answer is wrong.
    // It is not inherited: the scenario maps (1914, 1918, and every other
    // STDmap) ship NO starting_minority_policies at all, so every country
    // begins on the category defaults and all six of those are free. The bill
    // is the AI's own `conciliate`, taken on 78.1% of the turns it is offered
    // against `repress` on 0.2% -- the same one-way ratchet `fund up` runs at
    // 81.1%/0.4%, and for the same reason: both commit income PERMANENTLY
    // while costing nothing at the moment they are chosen. The opponent's 0.00
    // is simply a script that never conciliates.
    //
    // And conciliating is not in itself the mistake -- capping it is measured
    // WORSE (see AI_SOCIAL_BUDGET_SHARE, -8.5 points paired over five worlds),
    // because the alignment it buys is what keeps provinces from rebelling.
    // The defect is the ratchet, not the direction.
    //
    // Separately, and NOT what is happening here: m_ethnicPolicies is keyed by
    // countryId, so a conqueror inherits provinces without their settlements
    // and pays nothing for land the previous owner was paying for. That is
    // inert on these maps and very much not inert on map.odmap, where all 182
    // countries start on a paid footing totalling 1,970/turn. It is a rule
    // rather than a policy, so it is written up for the rule owner rather than
    // papered over here.
    if (inc.net >= 0.0f) return;   // paying its way; nothing to do

    // How long the treasury lasts at this rate. Already empty counts as no
    // runway at all, which is the case the bankruptcy cascade is handling this
    // same turn — cutting here as well just gets there sooner and cheaper.
    const double burn = -(double)inc.net;
    const double runway = burn > 1e-6 ? c->treasury / burn : 1e9;
    if (runway > AI_AUSTERITY_RUNWAY_TURNS && !g.isBankrupt(cid)) return;

    // CRASH: the treasury goes negative THIS turn. Then the cascade below runs
    // again after each cut, on the books as they now stand, until it balances
    // or nothing is left (AI_AUSTERITY_MAX_CUTS). Otherwise one cut a turn, as
    // before. Pending scraps are not in the snapshot yet, so they are counted
    // by hand.
    const bool crash = (c->treasury + (double)inc.net) < 0.0;
    double scrapSaving = 0.0;
    int cuts = 0;
    for (;;) {
    const char* what = nullptr;

    // ── 1. Discretionary budgets ──
    // Research and pacification are sliders. They come down first because they
    // come back up for free the moment income recovers.
    auto raIt = g.m_countryResearchAllocation.find(cid);
    if (!austerityResearchLast() &&
        !what && raIt != g.m_countryResearchAllocation.end() && raIt->second > 0.01f) {
        raIt->second = std::max(0.0f, raIt->second - 0.15f);
        what = "cut research funding"; ++s_austBranch[0];
    }
    auto pacIt = g.m_countryPacification.find(cid);
    if (!what && pacIt != g.m_countryPacification.end() && pacIt->second > 0.01f) {
        // Pacification is unrest suppression, and unrest is exactly what an
        // empty treasury causes — so this is cut second-to-last among the
        // budgets and never below a quarter while anything else remains.
        pacIt->second = std::max(0.25f, pacIt->second - 0.125f);
        if (pacIt->second < 0.25f + 1e-3f && inc.policyCosts <= 0.0f) what = nullptr;
        else { what = "cut pacification"; ++s_austBranch[1]; }
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
            if (bestIdx >= 0) { g.cancelPolicy(bestIdx); what = "repealed a doctrine"; ++s_austBranch[2]; }
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
            what = "trimmed a minority programme"; ++s_austBranch[3];
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
            // The resolver's own figure (BuildCosts.h), not a third copy of it:
            // these two sites carried 25/10 through the naval repricing and
            // fed scrapSaving, so austerity believed scrapping a carrier
            // closed a 25-point hole that is now 4.
            const float cost = shipUpkeep(s.type, s.crew);
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
            m_shipsScrappedThisTurn[cid]++;
            scrapSaving += bestCost;
            what = "scrapped a warship"; ++s_austBranch[4];
        }
    }

    // ── 5. Research, only when nothing else is left to give ──
    if (austerityResearchLast() &&
        !what && raIt != g.m_countryResearchAllocation.end() && raIt->second > 0.01f) {
        raIt->second = std::max(0.0f, raIt->second - 0.15f);
        what = "cut research funding"; ++s_austBranch[5];
    }

    if (what) statsFor(cid).austerityCuts++;
    if (what && g.m_config.aiDebug)
        printf("[AI] t%d %s [austerity] %s (net %.1f, treasury %.0f, %.1f turns left)\n",
               m_turn, c->name.c_str(), what, inc.net, c->treasury, runway);
    if (!what || !crash || ++cuts >= crashCuts()) break;
    inc = g.computeCountryIncome(cid);   // the books after the cut
    if (c->treasury + (double)inc.net + scrapSaving >= 0.0) break;
    }
}

// ── SIEGE REFLEX (OD_SIEGE_REFLEX=1) ──
//
// Norway, 1939, one rushing Sweden, v17 maps, seed 20260801, the decision
// stream under OD_ECON_TRACE: on the turn Sweden declares, the economy head
// ratchets research funding 0 -> 40% in eight goes, next turn to 45%, and
// upgrades industry in four provinces, two of which Sweden takes two turns
// later; it never fortifies -- every Norwegian province meets Sweden's
// 174,800-man stack at fort 0. "Fortify the threatened frontier" is one of
// the economy head's dead actions (chosen ~0.0), and "fund research down"
// another; this is a reflex for both, fired only while an adjacent enemy
// army exceeds one of our garrisons (st.worstDeficit > 0):
//   1. research funding steps down (0.15 a turn) while above 10%, so the
//      surplus goes to the front rather than the laboratory;
//   2. the most threatened under-fortified frontier gets a fort, through the
//      economy head's own action so there is one fort-buying rule.
// Off by default until measured; see the journal.
// The fort the siege reflex is trying to buy, or 0. While it is owed, the
// economy head's other lump sums wait (industry, port, specialisation, ships)
// and research funding does not go up: the treasury runs at zero every turn
// otherwise, and a 20-cost fort is never affordable on the turn it matters.
// BESIEGED: an adjacent enemy army exceeds one of our garrisons by at least
// OD_SIEGE_SHARE of our whole army (default 0.25). Any deficit at all
// annihilated modern China on N43 and N35 (a large power at war always has
// some outnumbered garrison, and the reflex then starves its economy);
// Norway's 574k against a 210k army is what the reflex is for.
bool AISystem::besieged(const CountryStat& st) const {
    static const float share = std::getenv("OD_SIEGE_SHARE") ? (float)atof(std::getenv("OD_SIEGE_SHARE")) : 0.25f;
    if (st.worstDeficit <= 0 || st.worstThreatPid < 0) return false;
    return (double)st.worstDeficit >= share * (double)std::max(1LL, st.army);
}

// ── PACIFICATION REFLEX (OD_PACIFY_REFLEX=1) ──
//
// Modern China, N24, seed 20260801, OD_UNREST_TRACE=3: at turn 1 all 96
// provinces carry a 4-10% rebellion chance (war weariness 6.8 on every
// province, base ~2, ethnic ~1.3, minus the loyalty floor 6) with the
// pacification slider at 0; 95 provinces become 43 by turn 20, and the
// politics head spends its goes conciliating minorities, which is not the
// term. Suppression is pac x 50, SUBTRACTED from the chance, so the slider
// that zeroes the worst province is worstChance / 50 -- 0.2 for China, a
// fifth of income while it lasts. The reflex sets the slider to that,
// bounded, and lets it decay when nothing is at risk (the austerity reflex
// cuts it first when the treasury is short).
// ── CALL TO ARMS REFLEX (OD_CALL_REFLEX, on by default) ──
//
// Norway, 1939, one rushing Sweden: Sweden takes 13 of 17 provinces in
// five turns and Norway never asks anyone for help. This morning's reading
// was that a pact signed mid-war was dead paper; the peer found the deeper
// half -- requestAllyJoinWar was hard-wired to the player, so wartime
// diplomacy existed for exactly one country. With that fixed their counter
// says roughly 500 country-turns a world have a callable friend and three
// to seven calls are made, because nobody was asking.
//
// The rule: while we are losing ground (a province lost recently, or an
// adjacent enemy army above one of our garrisons), call ONE friend a turn
// from the list the RULE produced -- callableFriends already excludes the
// enemy, the already-committed and the cooled-down, so this does not
// re-derive eligibility and cannot drift from it.
long long AISystem::s_callPickDecisions = 0;
long long AISystem::s_callPickReorders = 0;

void AISystem::callToArmsReflex(int cid) {
    // OFF by default. It fills the hole it was written for -- calls go from
    // 3 to 54 a world, and ~500 country-turns a world had a callable friend
    // nobody could ask -- but it costs on both instruments: N24 230 against
    // 249 and N37 224 against 225 on the seat bench, and the peer measured
    // rebellions tripling (54.53 -> 156.24 per 1k) because answered calls
    // drag countries into wars whose weariness they carry home. Whether a
    // world where alliances really pull people in is the game that is
    // wanted is the user's decision, not a default. OD_CALL_REFLEX=1.
    static const bool on = std::getenv("OD_CALL_REFLEX") && atoi(std::getenv("OD_CALL_REFLEX")) != 0;
    if (!on) return;
    Game& g = *m_g;
    const auto sIt = m_stats.find(cid);
    if (sIt == m_stats.end()) return;
    const CountryStat& st = sIt->second;
    const bool losing = st.provincesLost > 0 || st.worstDeficit > 0 ||
                        (st.enemyAdjArmy > st.defenderArmy && st.threatenedProvinces > 0);
    if (!losing) return;
    const std::vector<std::string> friends = g.callableFriends(cid);
    if (friends.empty()) return;
    static const int traceCid = std::getenv("OD_ECON_TRACE") ? atoi(std::getenv("OD_ECON_TRACE")) : -1;
    // WHO TO ASK. Only one call a turn, so the choice is the whole decision.
    // Strength alone (OD_CALL_PICK=army) asks the biggest friend every turn
    // until the cooldown bites, and the peer measured the answer rate halving
    // from 33% to 17% when the reflex started asking often. So the default
    // weighs the army the call would bring by the chance it is answered --
    // predictAcceptance runs the diplomacy net on THEIR features, which is
    // the same estimate the pact head already trusts.
    static const bool byArmyOnly = std::getenv("OD_CALL_PICK") && std::string(std::getenv("OD_CALL_PICK")) == "army";
    const std::string* best = nullptr; double bestScore = -1; long long bestArmy = 0;
    const std::string* byArmyBest = nullptr; long long byArmyBestArmy = -1;
    for (const std::string& iso : friends) {
        const int fcid = g.cidForIso(iso);
        if (fcid < 0) continue;
        auto fs = m_stats.find(fcid);
        const long long army = fs != m_stats.end() ? fs->second.army : 0;
        // predictAcceptance is USELESS here: the call_to_arms head is
        // saturated by AI_CALL_RELUCTANCE and returns the same 0.277 for
        // every candidate, so the ranking collapsed to army and the two
        // pickers measured byte-identical (the peer caught it). What
        // actually decides the answer is whether the friend is free: the
        // scripted rung answers `!atWar` outright, and the trained head's
        // own features say the same (feats[83] "already busy at home").
        // So weigh the army by that, and keep a busy friend as a last
        // resort rather than dropping them.
        const bool busy = const_cast<AISystem*>(this)->foreignWarCount(fcid) > 0;
        const double p = byArmyOnly ? 1.0 : (busy ? 0.2 : 1.0);
        const double score = std::log1p((double)army) * p;
        if (traceCid == cid)
            fprintf(stderr, "[CALL-PICK] turn %d cid=%d cand %s army=%lld p=%.3f score=%.3f\n",
                    m_turn, cid, iso.c_str(), army, p, score);
        if (score > bestScore) { bestScore = score; bestArmy = army; best = &iso; }
        // What the army-only picker would have taken, so a reordering can be
        // COUNTED. A picker that changes no ranking is not the same thing as
        // one that is not running, and outcomes cannot tell them apart.
        if (army > byArmyBestArmy) { byArmyBestArmy = army; byArmyBest = &iso; }
    }
    if (best && byArmyBest && best != byArmyBest) {
        ++s_callPickReorders;
        if (traceCid == cid)
            fprintf(stderr, "[CALL-PICK] turn %d cid=%d REORDER: %s over %s\n",
                    m_turn, cid, best->c_str(), byArmyBest->c_str());
    }
    ++s_callPickDecisions;
    if (!best) return;
    std::string why;
    if (g.requestAllyJoinWar(cid, *best, why)) {
        statsFor(cid).callsIssued++;
        if (traceCid == cid)
            fprintf(stderr, "[CALL] turn %d cid=%d asked %s (army %lld) while losing\n",
                    m_turn, cid, best->c_str(), bestArmy);
    } else if (traceCid == cid) {
        fprintf(stderr, "[CALL] turn %d cid=%d could not ask %s: %s\n", m_turn, cid, best->c_str(), why.c_str());
    }
}

// ── WITHDRAW REFLEX (OD_WITHDRAW_REFLEX, on by default) ──
//
// A repulsed assault above the frontage no longer ends: the reserve stands
// as a battle and fights a round a turn until somebody pulls it out. No
// head has a withdraw action, so a frozen model commits and grinds --
// the peer measured frozen models losing 15/17/42 battles against 13/9/14
// won. This reads the resolver's OWN last comparison (Battle::lastAtkPower
// vs lastDefPower, which already carries frontage, fort, depth, supply and
// both sides' research) rather than re-deriving it from troop counts:
// pull out when the fight has gone against us for OD_WITHDRAW_ROUNDS
// rounds AND the gap is not closing. A landing (fromProvince < 0) has
// nowhere to go, so it is never withdrawn -- it fights or it drowns.
void AISystem::withdrawReflex(int cid) {
    // OFF by default: measured a LOSS on both models tried (N24 250 vs 257,
    // N37 211 vs 249, floors 18/8 vs 28/28). A repulse grinds the defender
    // too, so "losing this round" is not "losing this battle", and pulling
    // out forfeits fights that persistence wins. See journal 46; a better
    // rule would compare the TREND in lastDefPower across rounds.
    static const bool on = std::getenv("OD_WITHDRAW_REFLEX") && atoi(std::getenv("OD_WITHDRAW_REFLEX")) != 0;
    if (!on) return;
    static const int minRounds = std::getenv("OD_WITHDRAW_ROUNDS") ? atoi(std::getenv("OD_WITHDRAW_ROUNDS")) : 2;
    Game& g = *m_g;
    static const int traceCid = std::getenv("OD_ECON_TRACE") ? atoi(std::getenv("OD_ECON_TRACE")) : -1;
    for (const Battle& b : g.m_battles) {
        if (b.attackerCid != cid) continue;
        if (b.fromProvince < 0) continue;                 // a landing cannot fall back
        if (b.rounds < minRounds) continue;               // give it a chance to turn
        if (g.hasPendingWithdraw(b.provinceId)) continue;
        // ── ONE ROUND IS NOT A TREND ──
        //
        // The first version of this rule read the LAST round only ("losing
        // and our losses were not smaller") and measured 7-38 points worse
        // than doing nothing: province 824 is four repulses in a row, each
        // killing about as many defenders as attackers, and the fourth
        // carries. So this asks the two questions the whole battle can
        // answer, and quits only when BOTH say no:
        //   1. is the grind working -- has the defence been worn below
        //      OD_WITHDRAW_GRIND of what it was worth on round one; and
        //   2. is the exchange ours -- totalDefLosses against
        //      totalAtkLosses over every round, which counts what the fight
        //      has cost them however many times they reinforce (a RISING
        //      lastDefPower may be the enemy feeding a fight it is losing).
        static const double grind = std::getenv("OD_WITHDRAW_GRIND") ? atof(std::getenv("OD_WITHDRAW_GRIND")) : 0.9;
        const bool grindWorking = b.openingDefPower > 0.0 &&
                                  b.lastDefPower < b.openingDefPower * grind;
        const bool exchangeOurs = b.totalDefLosses >= b.totalAtkLosses;
        if (grindWorking || exchangeOurs) continue;
        g.queueWithdraw(b.provinceId);
        statsFor(cid).withdrawsOrdered++;
        if (traceCid == cid)
            fprintf(stderr, "[WITHDRAW] turn %d cid=%d prov=%d after %d rounds (def %.0f of opening %.0f; total losses ours %lld theirs %lld)\n",
                    m_turn, cid, b.provinceId, b.rounds, b.lastDefPower, b.openingDefPower, b.totalAtkLosses, b.totalDefLosses);
    }
}

// ── CAMPAIGN REFLEX (OD_CAMPAIGNS, off until measured) ──
//
// See docs/ai/CAMPAIGNS.md. The AI's credit horizon is twelve turns and
// every action it had resolved inside one, so a plan could not be expressed
// and therefore could not be rewarded; lengthening the horizon (N59: 134
// against 249) and switching on search (228 against 249) both failed for the
// same reason. This gives it something a plan can be MADE of.
//
// Phase 1 opens the commitment from a reflex rather than a head action,
// because a ninth war action changes the net shape and would refuse every
// model file we have. What the campaign then does is steer the ordinary
// actions: recruitment and reinforcement prefer the staging province while
// one is open (see recruit and reinforceProvince), so the commitment shapes
// the turn instead of competing with it.
// ── WHICH KIND OF SOLDIER (OD_TROOP_KINDS) ──
//
// The recruit order carried no kind, so every man the AI ever raised was
// line infantry -- the peer measured 67,456,039 recruits at 100% line with
// the other three kinds unlocked and unused. There was nothing for a model
// to converge on because there was nothing to choose.
//
// The choice is a RULE rather than a head action for the same reason
// campaigns are: a fourth recruit action would change the net shape and
// refuse every model file. It reads the columns in TROOP_TYPES rather than
// naming kinds, so moving those numbers (they are a documented first draft)
// moves the AI's behaviour with them instead of leaving a stale rule behind:
//
//   attacking  -- a campaign is open and this is its staging province, so
//                 the men being raised are for taking ground: the best
//                 attack column we can afford.
//   besieged   -- an adjacent enemy army outnumbers a garrison: the best
//                 DEFENCE per unit of money, which is what militia is for.
//   otherwise  -- line infantry, the neutral column, as before.
//
// Affordability is checked against what the country actually has, because
// the expensive kinds cost money, munitions AND four times the people
// (the manpower multiplier is on the population draw, not in the price).
TroopType AISystem::chooseTroopType(int cid) const {
    static const bool on = std::getenv("OD_TROOP_KINDS") && atoi(std::getenv("OD_TROOP_KINDS")) != 0;
    if (!on) return TROOP_LINE;
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    if (!c) return TROOP_LINE;
    const std::vector<TroopType> unlocked = g.unlockedTroopTypes(cid);
    if (unlocked.size() <= 1) return TROOP_LINE;
    const auto sIt = m_stats.find(cid);
    if (sIt == m_stats.end()) return TROOP_LINE;
    const CountryStat& st = sIt->second;
    const Game::Campaign* camp = g.campaignOf(cid);
    const bool attacking = camp != nullptr;
    const bool besieged  = st.worstDeficit > 0 || st.provincesLost > 0;
    if (!attacking && !besieged) return TROOP_LINE;
    // One yardstick, so the two cases differ only in which column they read:
    // value per unit of money, on the axis this situation needs.
    TroopType best = TROOP_LINE; float bestScore = -1.0f;
    for (TroopType t : unlocked) {
        const TroopCost& tc = troopCost(t);
        if (tc.money <= 0.0f) continue;
        // Affordable at all: a kind that costs four times the people is not
        // a kind a small country can raise in numbers that matter.
        if (attacking && tc.manpower > 3.0f && st.population < 20000000) continue;
        const float axis = attacking ? tc.atk : tc.def;
        const float score = axis / tc.money;
        if (score > bestScore) { bestScore = score; best = t; }
    }
    return best;
}

// ── LOOKING AHEAD, EXACTLY (OD_CAMPAIGN_PROJECT) ──
//
// The AI's one attempt at foresight -- the latent MCTS -- rolls forward
// through a learned dynamics model and measured WORSE at play time (N24 249
// -> 228 at 32 simulations, 227 at 128), which is what its own header
// predicted: the payoff of that search is training the policy toward the
// visit distribution, not re-ranking a move.
//
// But this game has a better forward model than any net: its own combat
// arithmetic, which is deterministic and cheap. So before committing a third
// of the army to finishing a country, the AI plays the war forward in
// closed form -- round by round, for the campaign's whole deadline -- using
// the SAME quantities the resolver uses (frontage, depth beyond it, fort and
// supply on the defence) and asks two questions a plan has to answer:
//
//   can we finish them inside the deadline, and what will be left of us?
//
// A projection is not a prediction of the world; it is a prediction of THIS
// fight, which is the part the commitment is about. Every term is one the
// resolver computes, so when the peer changes the resolver the projection
// follows rather than drifting.
AISystem::Projection AISystem::projectCampaign(int cid, int enemyCid,
                                               int stagingPid, int targetPid,
                                               long long committed) const {
    Projection out;
    Game& g = *m_g;
    const auto me = m_stats.find(cid), them = m_stats.find(enemyCid);
    if (me == m_stats.end() || them == m_stats.end()) return out;
    const long long width = std::max(1LL, g.combatWidth(targetPid));
    // Their whole army is what has to be beaten, not the garrison in front of
    // us: a campaign ends when they have nothing left within reach, and they
    // reinforce from everywhere they hold.
    double theirMen = (double)std::max(1LL, them->second.army);
    double ourMen   = (double)std::max(1LL, committed);
    // The multipliers the resolver would apply to each side in this province.
    const double fortMul = 1.0 + (double)(g.m_provinceIndustry.count(targetPid)
                                 ? g.m_provinceIndustry.at(targetPid).fortification * 10 : 0) / 100.0;
    const double atkMod = 1.0 + g.getTotalEffect("armyAtkPct", cid) / 100.0;
    const double defMod = 1.0 + g.getTotalEffect("armyDefPct", enemyCid) / 100.0;
    const double atkSupply = (double)g.supplyFactor(cid, targetPid);
    // Their recruitment, at the rate the population allows: a country that
    // can replace what it loses cannot be finished by attrition, and that is
    // exactly the campaign we must not open.
    const double theirGrowth = (double)them->second.population * 0.0015;
    for (int turn = 1; turn <= AI_CAMPAIGN_DEADLINE; ++turn) {
        const double engagedUs   = std::min(ourMen,   (double)width);
        const double engagedThem = std::min(theirMen, (double)width);
        const double ourPower   = engagedUs * atkMod * atkSupply *
                                  (double)Game::depthFactor((long long)ourMen, width);
        const double theirPower = engagedThem * defMod * fortMul *
                                  (double)Game::depthFactor((long long)theirMen, width);
        // The resolver's own shape: the engaged men are lost when repulsed,
        // and the defence is ground down in proportion to the exchange.
        if (ourPower > theirPower) {
            theirMen -= engagedThem;                 // carried: the garrison falls
            ourMen   -= engagedUs * (theirPower / std::max(1.0, ourPower)) * 0.5;
        } else {
            ourMen   -= engagedUs;                   // repulsed: the engaged are lost
            theirMen -= engagedThem * (ourPower / std::max(1.0, theirPower)) * 0.5;
        }
        theirMen += theirGrowth;
        out.ourLosses = (double)committed - ourMen;
        out.turns = turn;
        if (theirMen <= 0.0) { out.finishes = true; break; }
        if (ourMen <= (double)committed * 0.15) break;   // spent
    }
    out.survivingShare = ourMen / std::max(1.0, (double)committed);
    return out;
}

// ── ONE WAR AT A TIME (OD_PEACE_REFLEX) ──
//
// France in the rush world fights five wars at once and, in 120 turns,
// chooses the ceasefire action exactly ZERO times: its war head spends 726
// goes on reinforce, 99 on recruit, 94 on attack and never once asks anybody
// for peace. The action is not masked out; the policy simply never picks it,
// the way the econ head never picked fortify.
//
// This is the complement of the campaign, and the two together are a
// doctrine rather than a pair of tricks: commit to ONE war and end the
// others. A country at war with four neighbours has its army spread across
// four fronts and a campaign it cannot feed. So while a campaign is open,
// every OTHER war gets a white-peace offer, cheapest enemy first -- the
// resolver's own diplomacy decides whether it is taken.
void AISystem::peaceReflex(int cid) {
    // OFF by default and measured a loss on 4 of 4 models (265 -> 199,
    // 202 -> 166, 238 -> 188, 212 -> 168). A white peace surrenders every
    // claim and gain against that enemy, and the bench scores land held: the
    // AI was not fighting five wars badly, it was holding five fronts and
    // three of them were feeding it. Concentration is a maxim from a game
    // where wars cost upkeep and peace is free. OD_PEACE_REFLEX=1.
    static const bool on = std::getenv("OD_PEACE_REFLEX") && atoi(std::getenv("OD_PEACE_REFLEX")) != 0;
    if (!on) return;
    Game& g = *m_g;
    const Game::Campaign* camp = g.campaignOf(cid);
    if (!camp) return;                       // nothing to concentrate on
    const Country* c = g.m_countries.getCountry(cid);
    if (!c) return;
    auto relIt = g.m_relations.find(c->isoA3);
    if (relIt == g.m_relations.end()) return;
    // OD_PEACE_BAR: sue for peace only while genuinely OUTMATCHED.
    //
    // Enabling this reflex globally is +3.86 land but a redistribution:
    // FRA-under-a-rusher +4.57 against CHN -5.30. Ending side wars is
    // survival for a country being overrun and a forfeited conquest for one
    // that is winning, so the same rule has opposite value depending on who
    // runs it. The bar is enemy army over ours; 0 keeps the ungated
    // behaviour that produced those numbers.
    {
        // OD_PEACE_WARS: fire only while carrying at least N foreign wars.
        //
        // The strength gate (OD_PEACE_BAR) was measured and INVERTED the
        // result: FRA-under-a-rusher went 5.60 -> 0.97 because a bar of 1.5x
        // only fires once the enemy already outnumbers you, which is past the
        // point where a ceasefire buys anything. The value of ending side wars
        // is PREVENTIVE. So condition on over-extension -- how many wars are
        // open -- rather than on how badly they are going.
        static const int peaceWars = std::getenv("OD_PEACE_WARS")
                                   ? atoi(std::getenv("OD_PEACE_WARS")) : 0;
        if (peaceWars > 0 && foreignWarCount(cid) < peaceWars) return;
        static const double peaceBar = std::getenv("OD_PEACE_BAR")
                                     ? atof(std::getenv("OD_PEACE_BAR")) : 0.0;
        if (peaceBar > 0.0) {
            const long long ours = m_stats.count(cid) ? m_stats[cid].army : 0;
            long long theirs = 0;
            for (const auto& [iso2, r2] : relIt->second) {
                if (!r2.war) continue;
                const int oc = g.cidForIso(iso2);
                if (oc >= 0 && m_stats.count(oc)) theirs += m_stats[oc].army;
            }
            if ((double)theirs < peaceBar * (double)std::max(1LL, ours)) return;
        }
    }
    static const int traceCid = std::getenv("OD_ECON_TRACE") ? atoi(std::getenv("OD_ECON_TRACE")) : -1;
    // The weakest enemy first: a small war is the cheapest one to be rid of,
    // and being rid of it frees the most army per offer made.
    int target = -1; long long best = -1;
    for (const auto& [iso, r] : relIt->second) {
        if (!r.war) continue;
        const int ocid = g.cidForIso(iso);
        if (ocid < 0 || ocid == camp->targetCountry) continue;   // not the war we chose
        if (!diploReady(cid, ocid)) continue;
        bool pending = false;
        for (const auto& da : g.m_pendingDiplomaticActions)
            if (da.sourceIso == c->isoA3 && da.targetIso == iso &&
                da.action == "request_ceasefire") { pending = true; break; }
        if (pending) continue;
        const long long army = m_stats.count(ocid) ? m_stats[ocid].army : 0;
        if (best < 0 || army < best) { best = army; target = ocid; }
    }
    if (target < 0) return;
    const Country* ec = g.m_countries.getCountry(target);
    if (!ec) return;
    if (g.queueDiplomaticAction({c->isoA3, ec->isoA3, "request_ceasefire", 1})) {
        statsFor(cid).ceasefiresOffered++;
        if (traceCid == cid)
            fprintf(stderr, "[PEACE] turn %d cid=%d offers white peace to %s (army %lld) to concentrate on %d\n",
                    m_turn, cid, ec->isoA3.c_str(), best, camp->targetCountry);
    }
}

void AISystem::campaignReflex(int cid) {
    // ON by default from ParrotZero 8.2.0. Same binary, controls reproducing
    // their earlier numbers exactly: N24 249 -> 265 with survival 88 -> 100
    // and worst seat 28 -> 110 (every seat above par, a first on any ruler);
    // N37 225 -> 238, worst 18 -> 31. Norway, the seat no rule reached all
    // day, goes 28 -> 110. Nothing else changed: same reward, same twelve-turn
    // credit horizon, same weights -- one decision simply now owns twelve
    // turns, so the credit window covers a whole decision instead of a
    // twelfth of one. OD_CAMPAIGNS=0 turns it off.
    static const bool on = !std::getenv("OD_CAMPAIGNS") || atoi(std::getenv("OD_CAMPAIGNS")) != 0;
    if (!on) return;
    Game& g = *m_g;
    const auto sIt = m_stats.find(cid);
    if (sIt == m_stats.end()) return;
    const CountryStat& st = sIt->second;
    // ── COME HOME ──
    //
    // A campaign holds a third of the army pointed at somebody else's
    // country. That is the right bet until the war arrives here: N47's rush
    // seat fell 187 -> 69 and N35 lost 22 rating with campaigns on, which is
    // what a commitment abroad costs a country being invaded at home. So the
    // one decision anybody makes after opening is to abandon: the moment an
    // adjacent enemy army outnumbers one of our garrisons, or we lose
    // ground, the campaign closes and the ordinary defensive rules take the
    // army back. OD_CAMPAIGN_RECALL=0 keeps the old behaviour.
    // OFF by default: mean 229 -> 215 and the worst seat 50 -> 23 across
    // four models. It does exactly what it was written for -- N47, the model
    // that lost most to campaigns, recovers 202 -> 238 -- and it takes more
    // from the models campaigns helped than it gives back (N24 265 -> 222,
    // floor 110 -> 23). A commitment that can be abandoned when things get
    // difficult is not a commitment, and the floor was the whole gain.
    // OD_CAMPAIGN_RECALL=1 to measure again.
    static const bool recall = std::getenv("OD_CAMPAIGN_RECALL") && atoi(std::getenv("OD_CAMPAIGN_RECALL")) != 0;
    const bool homeThreatened = st.provincesLost > 0 || st.worstDeficit > 0;
    if (recall && homeThreatened) {
        if (const Game::Campaign* open = g.campaignOf(cid)) {
            // ...unless the country threatening us IS the one we are
            // campaigning against. Then the campaign is the defence: Norway
            // holds because it commits against the neighbour invading it,
            // and recalling there would abandon the only fight that matters.
            bool targetIsTheThreat = false;
            for (const auto& fr : st.frontiers)
                if (fr.enemyCid == open->targetCountry) { targetIsTheThreat = true; break; }
            if (!targetIsTheThreat) {
                g.closeCampaign(cid, "recalled: home threatened elsewhere");
                return;
            }
        }
    }
    // The cap itself lives in Game::openCampaign (OD_CAMPAIGN_MAX), so the
    // reflex simply tries and is refused; asking twice would be two rules
    // for one fact.
    if (st.army <= 0) return;
    // Only while we are not the one in trouble: a country losing ground has
    // the siege reflex and its garrisons to think about, and committing a
    // quarter of its army to somebody else's province is how Norway died.
    // A besieged country may still open a campaign, but only against the
    // neighbour besieging it; the check is in the candidate loop below.
    // The target is the best attack candidate the ordinary rule already
    // found -- so a campaign commits to a fight the AI would have picked
    // anyway, and the difference is that it KEEPS committing.
    const std::vector<AttackCandidate>& cands = attackCandidates(cid);
    if (cands.empty()) return;
    // WHICH TARGET IS WORTH A COMMITMENT. Not the easiest: the first version
    // took the best margin and 71 of 82 campaigns closed on the turn they
    // opened, because the province fell to the ordinary attack anyway and
    // the commitment never did anything. A campaign is for a target that
    // will NOT fall this turn -- the fortified, the industrial, the
    // well-garrisoned -- so it is scored by what it is worth and how hard it
    // is, among the candidates the ordinary rule already calls winnable.
    // WHICH ENEMY IS WORTH A COMMITMENT. Not which province: 84 of 89
    // province campaigns closed on the turn they opened, because an adjacent
    // province the AI can beat falls to the ordinary attack in a single turn.
    // A country is the thing that takes many turns to finish, so the campaign
    // picks a VICTIM -- the enemy we have the best foothold against, weighted
    // by what taking their ground is worth -- and the first objective is the
    // candidate province that gets us in.
    const AttackCandidate* best = nullptr; float bestScore = -1.0f;
    for (const AttackCandidate& ch : cands) {
        if (ch.margin < AI_CAMPAIGN_MIN_MARGIN) continue;
        // ── DEFENSIVE CAMPAIGNS (OD_CAMPAIGN_DEFENSIVE, off) ──
        //
        // Added when the projection and the recall rule collided over
        // Norway, and left ON by accident: at the 265 measurement a
        // threatened country opened NO campaign at all, and letting it
        // open one against its attacker costs 53 points at hard (265 ->
        // 212, found by forcing both per-rung rules on and still reading
        // 212 -- the profile was innocent). The commitment is worth making
        // when we choose the war; when the war is on our ground the
        // ordinary defensive rules do better.
        static const bool defensive = std::getenv("OD_CAMPAIGN_DEFENSIVE") &&
                                      atoi(std::getenv("OD_CAMPAIGN_DEFENSIVE")) != 0;
        if (homeThreatened && !defensive) return;
        if (homeThreatened) {
            bool isTheThreat = false;
            for (const auto& fr : st.frontiers)
                if (fr.enemyCid == ch.enemyCid) { isTheThreat = true; break; }
            if (!isTheThreat) continue;
        }
        const auto es = m_stats.find(ch.enemyCid);
        if (es == m_stats.end() || es->second.provinces <= 0) continue;
        // ── A VICTIM WORTH COMMITTING TO ──
        //
        // Found by tracing a Norway seat: of 297 campaigns that closed, 96
        // closed on the turn they OPENED, all 96 as BEATEN, and 90 of those
        // had taken exactly one province. The victim had a single province,
        // the ordinary attack rule took it, and the enemy was finished --
        // the commitment never made a decision. This is the same failure the
        // province-grain version had, surviving the move to country grain,
        // because log1p(1) is still positive and a one-province neighbour
        // has a huge margin, so it outscores real targets.
        //
        // A campaign is for a war that takes turns. If the victim can be
        // finished by the attack we are already making, there is nothing to
        // commit to, and the slot is better left for a target that needs
        // one. That reasoning is WRONG, and the bench says so: gating these
        // out cost N24 253 -> 229 and N37 250 -> 193. The one-province
        // campaign is not waste. It takes a province, it takes it inside the
        // grace window, and the country it beats stops existing -- and the
        // slot it "wastes" would otherwise go to a harder target the model
        // does worse against. DEFAULT 1 (off): the gate is kept only as
        // evidence. OD_CAMPAIGN_MIN_VICTIM=2 to measure it again.
        static const int minVictim = std::getenv("OD_CAMPAIGN_MIN_VICTIM")
                                   ? atoi(std::getenv("OD_CAMPAIGN_MIN_VICTIM")) : 1;
        if (es->second.provinces < minVictim) continue;
        // Worth: how much of them there is to take, and how good the ground
        // is. Feasibility: the margin the ordinary rule already computed.
        const float worth = std::log1p((float)es->second.provinces) *
                            (1.0f + (float)ch.indLevel);
        float score = worth * std::min(ch.margin, 3.0f);
        // ── AND THEN LOOK AHEAD ──
        //
        // The margin says whether the first assault carries. The projection
        // says whether the WAR can be finished inside the deadline and what
        // is left of the army afterwards, which is the question a commitment
        // actually poses. A campaign that cannot be finished is a third of
        // the army parked in someone else's country until the deadline.
        // OFF by default: measured a loss on 3 of 4 models (mean 229 -> 209,
        // survival 90 -> 85, worst seat 50 -> 14). The projection is correct
        // arithmetic and the wrong POLICY -- it refuses wars whose value is
        // not in finishing the enemy, and the floor collapse says the ones it
        // refuses are the defensive commitments that were holding small
        // countries together. Kept because it is the only exact lookahead in
        // the codebase and the next attempt should start from it rather than
        // from a net. OD_CAMPAIGN_PROJECT=1 to measure again.
        static const bool project = std::getenv("OD_CAMPAIGN_PROJECT") &&
                                    atoi(std::getenv("OD_CAMPAIGN_PROJECT")) != 0;
        if (project) {
            const Projection p = projectCampaign(cid, ch.enemyCid, ch.fromPid, ch.toPid,
                                                 (long long)(st.army * AI_CAMPAIGN_SHARE));
            // A war we are already in against a neighbour who is beating us
            // is not a war we chose, and declining to focus on it does not
            // make it go away -- so the "can we finish them" test applies to
            // wars of choice only. Norway's campaign against the neighbour
            // invading it is the case: unfinishable on paper, and the reason
            // its floor went 28 -> 110.
            const bool defensive = homeThreatened &&
                                   [&]{ for (const auto& fr : st.frontiers)
                                            if (fr.enemyCid == ch.enemyCid) return true;
                                        return false; }();
            if (!defensive) {
                if (!p.finishes) continue;                   // do not start what we cannot end
                if (p.survivingShare < AI_CAMPAIGN_MIN_LEFT) continue;  // not at this price
            }
            // Sooner is better, and cheaper is better: a war won in four
            // turns with two thirds of the force intact is worth more than
            // the same conquest that takes twelve and costs everything.
            score *= (float)(p.survivingShare * (2.0 - (double)p.turns / AI_CAMPAIGN_DEADLINE));
        }
        if (score > bestScore) { bestScore = score; best = &ch; }
    }
    if (!best) return;
    Game::Campaign c;
    c.countryId = cid;
    c.targetCountry = best->enemyCid;
    c.targetProvince = best->toPid;
    c.stagingProvince = best->fromPid;
    // How much of the army a commitment is worth. 0.35 was a guess made
    // when campaigns were written; it is the parameter most likely to be
    // wrong, and it is the one that decides whether a campaign is a
    // spearhead or the whole country. OD_CAMPAIGN_SHARE overrides it.
    static const float share = std::getenv("OD_CAMPAIGN_SHARE")
                             ? (float)atof(std::getenv("OD_CAMPAIGN_SHARE")) : AI_CAMPAIGN_SHARE;
    c.committedMen = (long long)(st.army * share);
    static const int deadline = std::getenv("OD_CAMPAIGN_DEADLINE")
                              ? atoi(std::getenv("OD_CAMPAIGN_DEADLINE")) : AI_CAMPAIGN_DEADLINE;
    c.deadlineTurns = deadline;
    g.openCampaign(c);
}

void AISystem::pacificationReflex(int cid) {
    static const bool on = std::getenv("OD_PACIFY_REFLEX") && atoi(std::getenv("OD_PACIFY_REFLEX")) != 0;
    if (!on) return;
    Game& g = *m_g;
    if (g.m_countries.getCountry(cid) == nullptr) return;
    float worst = 0.0f;
    for (int pid : g.provincesOf(cid))
        worst = std::max(worst, g.getProvinceRebellionChance(pid, cid));
    float& pac = g.m_countryPacification[cid];
    static const int traceCid = std::getenv("OD_ECON_TRACE") ? atoi(std::getenv("OD_ECON_TRACE")) : -1;
    if (worst > 0.0f) {
        // The chance already has today's suppression taken off; add what is
        // still showing, capped at a quarter of the slider per turn.
        const float step = std::min(0.25f, worst / 50.0f + 0.01f);
        const float before = pac;
        pac = std::min(1.0f, pac + step);
        if (traceCid == cid)
            fprintf(stderr, "[PACIFY] turn %d cid=%d worst %.1f%% -> pacification %.2f -> %.2f\n", m_turn, cid, worst, before, pac);
    } else if (pac > 0.0f) {
        pac = std::max(0.0f, pac - 0.05f);   // nothing at risk: let it fall
    }
}

// The reflex is on and this country is besieged (see besieged()).
bool AISystem::underSiege(int cid) const {
    // PER RUNG (DifficultyProfile::useSiegeReflex): +35 hard, +8 easy, -32 on
    // NORMAL, where it trades research for forts against a threat that is not
    // real. OD_SIEGE_REFLEX forces it either way for measurement.
    const char* siegeEnv1 = std::getenv("OD_SIEGE_REFLEX");
    const bool on = siegeEnv1 ? atoi(siegeEnv1) != 0 : difficulty().useSiegeReflex;
    if (!on) return false;
    const auto sIt = m_stats.find(cid);
    return sIt != m_stats.end() && besieged(sIt->second);
}

float AISystem::siegeEarmark(int cid) const {
    // PER RUNG (DifficultyProfile::useSiegeReflex): +35 hard, +8 easy, -32 on
    // NORMAL, where it trades research for forts against a threat that is not
    // real. OD_SIEGE_REFLEX forces it either way for measurement.
    const char* siegeEnv2 = std::getenv("OD_SIEGE_REFLEX");
    const bool on = siegeEnv2 ? atoi(siegeEnv2) != 0 : difficulty().useSiegeReflex;
    static const bool buyFort = !std::getenv("OD_SIEGE_FORT") || atoi(std::getenv("OD_SIEGE_FORT")) != 0;   // ON by default: +5 mean rating and +3.8 world survival for -35 on the China seat (journal 39h); OD_SIEGE_FORT=0 to drop it
    if (!on || !buyFort) return 0.0f;
    const auto sIt = m_stats.find(cid);
    if (sIt == m_stats.end() || !besieged(sIt->second)) return 0.0f;
    const int cap = fortCap(cid);
    for (auto& fr : sIt->second.frontiers) {
        auto ind = m_g->m_provinceIndustry.find(fr.pid);
        const int fl = ind != m_g->m_provinceIndustry.end() ? ind->second.fortification : 0;
        if (fl < cap) return (float)FORT_COST[std::min(fl + 1, 5)] *
                             buildCostMod(m_g->getTotalEffect("industryCostPct", cid));
    }
    return 0.0f;
}

void AISystem::siegeReflex(int cid) {
    // PER RUNG (DifficultyProfile::useSiegeReflex): +35 hard, +8 easy, -32 on
    // NORMAL, where it trades research for forts against a threat that is not
    // real. OD_SIEGE_REFLEX forces it either way for measurement.
    const char* siegeEnv3 = std::getenv("OD_SIEGE_REFLEX");
    const bool on = siegeEnv3 ? atoi(siegeEnv3) != 0 : difficulty().useSiegeReflex;
    if (!on) return;
    Game& g = *m_g;
    const auto sIt = m_stats.find(cid);
    if (sIt == m_stats.end()) return;
    const CountryStat& st = sIt->second;
    if (!besieged(st)) return;
    static const int traceCid = std::getenv("OD_ECON_TRACE") ? atoi(std::getenv("OD_ECON_TRACE")) : -1;
    // OD_SIEGE_RESEARCH=1 puts the research slider back INTO the reflex.
    //
    // The default is OFF, and off is what shipped: keeping the slider out of
    // the reflex (the fort and its earmark stay) is worth +71 rating on
    // hold-out C and +42 on D, paired with austerityResearchLast. Journal 239.
    //
    // This comment used to read "OD_SIEGE_RESEARCH=0 keeps the slider out",
    // which was true at 4cea3bc when the gate defaulted ON. c06b3cc flipped
    // the default to ship the win and left the comment describing the old
    // semantics, so =0 became a no-op that looks like a control. Anyone
    // following it would disable something already disabled, see no change,
    // and conclude the research half is free.
    static const bool cutResearch = std::getenv("OD_SIEGE_RESEARCH") && atoi(std::getenv("OD_SIEGE_RESEARCH")) != 0;
    auto raIt = g.m_countryResearchAllocation.find(cid);
    if (cutResearch && raIt != g.m_countryResearchAllocation.end() && raIt->second > 0.10f) {
        raIt->second = std::max(0.10f, raIt->second - 0.15f);
        if (traceCid == cid)
            fprintf(stderr, "[SIEGE] turn %d cid=%d research funding down to %.0f%% (deficit %lld at prov %d)\n",
                    m_turn, cid, raIt->second * 100.0f, st.worstDeficit, st.worstThreatPid);
    }
    // OD_SIEGE_FORT=0: no fort and no earmark (the research cut alone).
    static const bool buyFort = !std::getenv("OD_SIEGE_FORT") || atoi(std::getenv("OD_SIEGE_FORT")) != 0;   // ON by default: +5 mean rating and +3.8 world survival for -35 on the China seat (journal 39h); OD_SIEGE_FORT=0 to drop it
    if (!buyFort) return;
    const std::string what = execEconomy(cid, 2);
    if (traceCid == cid)
        fprintf(stderr, "[SIEGE] turn %d cid=%d fortify -> %s\n", m_turn, cid, what.c_str());
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
        g.traceDisband("PUSH-ai", pid, (int)take, 0);   // count is real, not the 0 sentinel
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
    // WHERE THE SHIP GOES, WHICH IS NOT THE MIDDLE OF THE PROVINCE.
    //
    // This read m_provinceCenters directly, and a province centre is a land
    // pixel by construction. Every order this reflex issued therefore aimed a
    // hull at dry land, the resolver clamped it at the last water on the line,
    // and the next turn re-issued the same order down the same blocked line.
    // execNavy's own target pickers were moved onto Game::portApproach; this
    // one was missed, and it is the one that actually sails the invasions --
    // which is why 85% of all ship movement in the world still went nowhere
    // after those were fixed. See Game::portApproach.
    auto portAt = [&](int pid, double& lon, double& lat) -> bool {
        return g.portApproach(pid, lon, lat);
    };

    // THE HULL'S OWN RANGE, not a constant. This reflex kept the flat 18
    // degrees the navy module was moved off: 18 degrees is 410 px on the
    // shipped maps against a player boat's 200, so an AI transport ordered
    // twice the distance a human's could and its 12-degree landing window let
    // it unload from 273 px offshore where the player must be within 200. The
    // resolver clamped the movement, which hid the first half of it and left
    // the second half working -- the player saw a fleet order stretching
    // across an ocean and troops coming ashore from further out than they
    // could manage themselves.
    //
    // Landing range must still exceed the per-turn step, or a fleet straddles
    // the gap -- passing from "too far" to "too far on the other side" without
    // ever being able to unload. It IS the step now, so that holds by
    // construction, and the last approach is still made in small steps.

    // Is this hull already under way to that port? See the note at the
    // re-order below: leaving a voyage alone is what lets it finish.
    auto sailingTo = [&](size_t shipIdx, int pid) {
        for (const auto& mo : g.m_pendingShipMoveOrders)
            if (mo.shipIndex == (int)shipIdx) return mo.destProvince == pid;
        return false;
    };

    for (size_t i = 0; i < g.m_ships.size(); ++i) {
        auto& s = g.m_ships[i];
        if (s.countryId != cid || s.crew <= 0) continue;
        const double LAND_RANGE = g.shipMaxRangeDeg(s);
        bool busy = false;
        for (auto& dd : g.m_pendingShipDisembarks)
            if (dd.shipIndex == (int)i) { busy = true; break; }
        if (busy) continue;

        // ── TWO DISTANCES, BECAUSE THERE ARE TWO QUESTIONS ──
        //
        // WHERE DO I SAIL is answered by the water beside the harbour: a
        // province centre is a land pixel and ordering a hull to one beaches
        // it (Game::portApproach).
        //
        // AM I CLOSE ENOUGH TO PUT AN ARMY ASHORE is answered by the province
        // CENTRE, because that is the rule processShipDisembarks enforces and
        // the rule the player's own landing circle is drawn from. Measuring the
        // approach here instead made the AI believe it was in range while the
        // resolver disagreed, and it threw the order away: 93 landing orders
        // dropped against 168 issued on a single 100-turn map -- more than a
        // third of every invasion the AI thought it had launched, silently.
        int enemyPid = -1, homePid = -1;
        double enemyD = 1e18, homeD = 1e18;      // to the CENTRE: may I land?
        double enemyLon = 0, enemyLat = 0, homeLon = 0, homeLat = 0;  // where to sail
        const int mapWc = g.m_provinces.getWidth(), mapHc = g.m_provinces.getHeight();
        auto centreOf = [&](int pid, double& lon, double& lat) {
            auto cIt = g.m_provinceCenters.find(pid);
            if (cIt == g.m_provinceCenters.end()) return false;
            lon = cIt->second.x / mapWc * 360.0 - 180.0;
            lat = 90.0 - cIt->second.y / mapHc * 180.0;
            return true;
        };
        for (auto& [pid, port] : g.m_provincePorts) {
            const Province* p = g.m_provinces.getProvinceById(pid);
            if (!p) continue;
            double lon, lat;
            if (!portAt(pid, lon, lat)) continue;
            double cLon, cLat;
            if (!centreOf(pid, cLon, cLat)) continue;
            // NEAREST IS NOT THE SAME AS REACHABLE -- the lesson findEnemyPort
            // learned and this reflex never did. Chosen on straight-line
            // distance alone, a transport in the Atlantic picks a Pacific port
            // that happens to be closer across land than any Atlantic one is
            // across water, and then spends the rest of the game pressed
            // against the intervening coast. Now that the nav grid knows one
            // sea from another (see Game::buildNavGrid) this test means what it
            // says.
            if (!g.navReachable(s.lon, s.lat, lon, lat)) continue;
            // Ranked by how far there is to SAIL, judged by the landing rule.
            const double d = Game::seaDistanceDeg(s.lon, s.lat, cLon, cLat);
            if (atWarWith(p->countryId)) {
                // LANDABLE, not merely nearest. The boat sails to the harbour's
                // approach cell; the landing test measures to the province
                // CENTRE. A coastal province whose centre lies further inland
                // than one hull's range can never be landed on from its own
                // approach, and a boat sent there parks for the rest of the
                // game: 1,326 parked boat-turns against 2 sailing on the first
                // v8.1 check (journal 36k). Skip those.
                if (Game::seaDistanceDeg(lon, lat, cLon, cLat) > LAND_RANGE) continue;
                if (d < enemyD) { enemyD = d; enemyPid = pid; enemyLon = lon; enemyLat = lat; }
            } else if (p->countryId == cid) {
                if (d < homeD) { homeD = d; homePid = pid; homeLon = lon; homeLat = lat; }
            }
        }

        // In range of a hostile shore: land, now. This is the whole point.
        if (enemyPid >= 0 && enemyD <= LAND_RANGE) {
            g.m_pendingShipDisembarks.push_back({(int)i, enemyPid});
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
            } else if (homePid >= 0 && !sailingTo(i, homePid)) {
                PendingShipMoveOrder ord;
                ord.shipIndex = (int)i;
                ord.destLon = homeLon; ord.destLat = homeLat;
                ord.destProvince = homePid;
                g.m_pendingShipMoveOrders.push_back(std::move(ord));
            }
            continue;
        }

        // Otherwise close on the target -- as a VOYAGE, which the resolver
        // now sails on its own (see processNavyMovement). Two things follow
        // from that and neither is optional:
        //
        //   A hull already sailing to the port we would pick is LEFT ALONE.
        //   Re-issuing the same order every turn is what made this a one-turn
        //   hop machine: the route was thrown away and re-planned from
        //   wherever the clamp had left the hull, so it never rounded anything.
        //
        //   A hull sailing somewhere ELSE is re-ordered. The nearest hostile
        //   port changes when a war ends or a coast is taken, and a transport
        //   that keeps its heading through that is carrying an army to a
        //   country it is no longer fighting.
        // Diagnostic for the embarkation sink (journal 35m): a loaded boat
        // with an enemy port known, not within landing range of that port's
        // CENTRE, and with no move order left -- i.e. parked where sailing
        // took it and still not allowed to land. If this counter is the size
        // of the fleet, the landing test and the sailing target disagree.
        {
            bool underOrders = false;
            for (const auto& mo : g.m_pendingShipMoveOrders)
                if (mo.shipIndex == (int)i) { underOrders = true; break; }
            if (!underOrders) statsFor(cid).boatsParkedOutOfRange++;
            else               statsFor(cid).boatsSailing++;
            // OD_BOAT_TRACE=1: one line per loaded boat per turn, the
            // instrument for "1,273 parked boat-turns, 0 arrived, 0 stuck".
            static const bool trace = std::getenv("OD_BOAT_TRACE") != nullptr;
            if (trace)
                printf("[BOAT] t%d cid=%d ship=%zu crew=%d enemyPid=%d enemyD=%.2f range=%.2f order=%d sailingTo=%d homePid=%d\n",
                       m_turn, cid, i, s.crew, enemyPid, enemyD, LAND_RANGE, underOrders ? 1 : 0,
                       (enemyPid >= 0 && sailingTo(i, enemyPid)) ? 1 : 0, homePid);
        }
        if (sailingTo(i, enemyPid)) continue;
        for (auto it = g.m_pendingShipMoveOrders.begin();
             it != g.m_pendingShipMoveOrders.end(); ) {
            if (it->shipIndex == (int)i) it = g.m_pendingShipMoveOrders.erase(it);
            else ++it;
        }
        PendingShipMoveOrder ord;
        ord.shipIndex = (int)i;
        ord.destLon = enemyLon; ord.destLat = enemyLat;
        if (std::getenv("OD_BOAT_TRACE"))
            printf("[BOAT] t%d cid=%d ship=%zu PUSH order -> prov %d at (%.2f,%.2f) from (%.2f,%.2f); orders now %zu\n",
                   m_turn, cid, i, enemyPid, enemyLon, enemyLat, s.lon, s.lat, g.m_pendingShipMoveOrders.size() + 1);
        ord.destProvince = enemyPid;
        g.m_pendingShipMoveOrders.push_back(std::move(ord));
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
            // THE WATER BESIDE THE HARBOUR, NOT THE MIDDLE OF THE PROVINCE.
            // A province centre is a land pixel by construction, so aiming at
            // one orders the hull ashore and the resolver clamps it on the
            // beach. See Game::portApproach.
            double lon, lat;
            if (!g.portApproach(pid, lon, lat)) continue;
            if (!g.navReachable(fromLon, fromLat, lon, lat)) continue;  // as above
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
            // As findHomePort: the approach, not the province centre.
            double lon, lat;
            if (!g.portApproach(pid, lon, lat)) continue;
            // NEAREST IS NOT THE SAME AS REACHABLE. Chosen on straight-line
            // distance alone, this picked ports on the far side of a continent
            // and the fleet then spent the rest of the game pressed against the
            // nearest beach -- 93% of all ship moves went nowhere.
            if (!g.navReachable(fromLon, fromLat, lon, lat)) continue;
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
                best = std::min(best, Game::seaDistanceDeg(s.lon, s.lat, lon, lat));
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
                // ONE RULE, ASKED IN ONE PLACE. The mask offers this action
                // only when shipDestination finds somewhere for a hull to go,
                // so re-deriving the answer here would let the two disagree --
                // which is how 48% of navy-move decisions came to do nothing.
                // `continue`, not `break`: one hull having nowhere to sail says
                // nothing about the next one, and breaking abandoned the whole
                // fleet on the first ship that happened to be on station.
                if (!shipDestination(cid, s.lon, s.lat, tp, tLon, tLat)) continue;
                // STEER FOR THE NEXT WAYPOINT, NOT THE DESTINATION.
                //
                // Aiming at the target itself is what produced the 93% stall:
                // the resolver clamps a move at the last navigable point, so a
                // port behind a headland parked the hull on that headland and
                // every following turn re-aimed down the same blocked line. The
                // route bends around the coast, so each leg is water the ship
                // can actually cross. Falls back to the direct line when no
                // route is available, which is what the open-sea case wants
                // THE WHOLE VOYAGE, NOT THIS TURN'S HOP.
                //
                // This used to pick an aim point one turn away and re-decide
                // the next turn, because the resolver threw the order away at
                // the end of it. The resolver now routes and keeps the order
                // (see processNavyMovement), so the destination goes in whole
                // and the range clamp, the coast-following and the turn count
                // are all its business. The `busy` check above then means what
                // it says: a hull already at sea under orders is left to sail
                // rather than re-ordered from scratch every turn.
                PendingShipMoveOrder ord;
                ord.shipIndex = (int)i;
                ord.destLon = tLon; ord.destLat = tLat;
                ord.destProvince = tp;
                g.m_pendingShipMoveOrders.push_back(std::move(ord));
                if (++moved >= 3) break; // a few ships per turn is plenty
            }
            if (moved) return std::string(TextFormat("move %d ship(s) to station", moved));
            // "no target" conflated three very different situations, which made
            // the archipelago stall impossible to read off the dashboard.
            {
                int tp; double tl, ta;
                if (!findEnemyPort(0, 0, tp, tl, ta))
                    return didNothing("navy move: every ship already on station");
                return didNothing("navy move: all ships already under orders");
            }
        }
        case 2: { // bombard the nearest at-war enemy province in range
            struct Ammo { const char* type; const char* node; };
            static const Ammo AMMO[] = {
                {"heavy", "arty3"}, {"light", "arty2"}, {"mortar", "arty1"}};
            const Ammo* use = nullptr;
            Game::WarPrice usePrice;
            for (auto& a2 : AMMO) {
                if (!g.hasResearched(a2.node, cid)) continue;
                const Game::WarPrice p2 = g.artilleryPrice(a2.type, cid);
                if (c.treasury < p2.money || !g.canAffordWarMaterials(cid, p2)) continue;
                use = &a2; usePrice = p2; break;
            }
            if (!use) return didNothing("bombard: no ammo");
            int mapW = g.m_provinces.getWidth(), mapH = g.m_provinces.getHeight();
            if (mapW <= 0 || mapH <= 0) return didNothing("bombard: no map");
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
                    double d = Game::seaDistanceDeg(s.lon, s.lat, lon, lat);
                    // The hull's real range, same as the player's bombard
                    // circle, rather than the flat 10 degrees this used to
                    // call a "rough range gate". The resolver enforces it now
                    // regardless, so an out-of-range order is simply dropped.
                    if (d > g.shipMaxRangeDeg(s)) continue;
                    c.treasury -= usePrice.money;
                    g.payWarMaterials(cid, usePrice);
                    g.m_pendingShipBombardOrders.push_back({(int)i, pid, use->type});
                    return TextFormat("bombard prov %d (%s)", pid, use->type);
                }
            }
            return didNothing("bombard: nothing in range");
        }
        case 3: { // embark troops at the port the mask found -- see bestEmbarkPort
            int bestPid = -1, bestG = 0;
            if (!bestEmbarkPort(cid, bestPid, bestG))
                return didNothing("embark: no garrison at port");
            g.m_pendingEmbarkations.push_back({bestPid, bestG / 2, 1});
            statsFor(cid).embarks++;
            return TextFormat("embark %d from prov %d", bestG / 2, bestPid);
        }
        case 4: { // amphibious landing: nearest at-war coastal (port) province
            int mapW = g.m_provinces.getWidth(), mapH = g.m_provinces.getHeight();
            if (mapW <= 0 || mapH <= 0) return didNothing("disembark: no map");
            for (size_t i = 0; i < g.m_ships.size(); ++i) {
                auto& s = g.m_ships[i];
                if (s.countryId != cid || s.crew <= 0) continue;
                bool busy = false;
                for (auto& dd : g.m_pendingShipDisembarks)
                    if (dd.shipIndex == (int)i) { busy = true; break; }
                if (busy) continue;
                std::vector<int> landCands;   // see OD_LANDING_PICK below
                for (auto& [pid, port] : g.m_provincePorts) {
                    const Province* p = g.m_provinces.getProvinceById(pid);
                    if (!p || !atWarWith(p->countryId)) continue;
                    auto cIt = g.m_provinceCenters.find(pid);
                    if (cIt == g.m_provinceCenters.end()) continue;
                    double lon = cIt->second.x / mapW * 360.0 - 180.0;
                    double lat = 90.0 - cIt->second.y / mapH * 180.0;
                    // THE HULL'S OWN RANGE, which is the rule
                    // processShipDisembarks enforces and the rule the player's
                    // landing circle is drawn from. This was a flat 12 degrees
                    // -- 273 px on the shipped maps against a boat's 200 -- so
                    // the AI issued landings from a third further out than it
                    // was allowed and the resolver threw them away. Measured on
                    // one 100-turn map: 108 landing orders dropped as out of
                    // range. The reflex was moved off the flat 12 when it was
                    // found there; this copy was missed.
                    if (Game::seaDistanceDeg(s.lon, s.lat, lon, lat) > g.shipMaxRangeDeg(s)) continue;
                    // ── WHICH SHORE (OD_LANDING_PICK, off by default) ──
                    //
                    // This returns on the FIRST hostile port in range, which is
                    // whichever m_provincePorts happens to yield first -- so
                    // among several reachable shores the landing site is
                    // decided by container order. A landing is adjudicated by
                    // processArmyMovement like any other assault, and the men
                    // who lose one are gone along with the hull; choosing the
                    // shore without looking at who holds it is the same
                    // omission as the supply term missing from the margin.
                    //
                    // Scored, not gated: the weakest defence in range wins,
                    // weighted by fortification and defensive research the way
                    // the resolver weights them. No landing that would have
                    // happened is REFUSED -- making this head decline has
                    // measured badly before -- the question is only where.
                    static const bool pickWeakest = std::getenv("OD_LANDING_PICK") &&
                                                    atoi(std::getenv("OD_LANDING_PICK")) != 0;
                    if (pickWeakest) { landCands.push_back(pid); continue; }
                    g.m_pendingShipDisembarks.push_back({(int)i, pid});
                    return TextFormat("disembark %d troops at prov %d", s.crew * 100, pid);
                }
                if (!landCands.empty()) {
                    int bestPid = -1; double bestDef = 1e30;
                    for (int lp : landCands) {
                        const int owner = (lp >= 0 && lp < (int)g.m_provinceCountryLookup.size())
                                              ? g.m_provinceCountryLookup[lp] : 0;
                        long long garrison = 0;
                        auto ait = g.m_provinceArmies.find(lp);
                        if (ait != g.m_provinceArmies.end())
                            for (const auto& u : ait->second)
                                if (u.countryId == owner) garrison += u.count;
                        const auto ind = g.m_provinceIndustry.find(lp);
                        const float fort = ind != g.m_provinceIndustry.end()
                                             ? (float)ind->second.fortification : 0.0f;
                        const double defence = (double)garrison * (1.0 + fort * 0.1) *
                                               (1.0 + g.getTotalEffect("armyDefPct", owner) / 100.0);
                        if (defence < bestDef) { bestDef = defence; bestPid = lp; }
                    }
                    landCands.clear();
                    if (bestPid >= 0) {
                        g.m_pendingShipDisembarks.push_back({(int)i, bestPid});
                        return TextFormat("disembark %d troops at prov %d", s.crew * 100, bestPid);
                    }
                }
            }
            // No hostile shore to land on. Put the troops back ashore at one of
            // our own ports instead of leaving them floating: a war that ends in
            // a ceasefire mid-crossing used to strand the cargo permanently,
            // with the army subtracted from the land total and never returned.
            // ── THE "UNLOAD HOME" FALLBACK IS GONE ──
            //
            // This action used to fall back, when no hostile port was within
            // one hull's range, to unloading the cargo at any OWN port within
            // range -- which, for a boat loaded this turn and still lying off
            // its harbour, is always true. The script prefers this action
            // whenever it is valid and the head often picks it, so a boat was
            // emptied and scrapped one turn after loading, and the sail order
            // the amphibious reflex had just pushed died with the hull
            // (processShipDisembarks erases a hull after unloading). Traced
            // 2026-09-05: 646 loaded-boat-turns, 2 with an order; 438 orders
            // pushed, 2 routed; "1,465 came home" out of 1,771 embarkations.
            // Nine thousand embarkations had reached a hostile shore 0% of
            // the time for as long as the game existed, first because the
            // resolver deleted the men, then because this line sent them
            // home. The reflex already sails a boat with no reachable target
            // home and unloads it there; a head that has nothing in range
            // now does nothing, and the boat keeps sailing.
            {
                bool anyPort = false;
                double d = nearestLandingRange(anyPort);
                if (!anyPort)
                    return didNothing("disembark: no at-war enemy port (returning home)");
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
                float cost = shipUpkeep(s.type, s.crew);   // BuildCosts.h, not a copy
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
                    idle = Game::seaDistanceDeg(s.lon, s.lat, tLon, tLat);
                // Costliest first; among equals, the one furthest from a front.
                if (cost > bestCost || (cost == bestCost && idle > bestIdleness)) {
                    bestCost = cost; bestIdleness = idle; bestIdx = (int)i;
                }
            }
            if (bestIdx < 0) return didNothing("scrap: nothing worth scrapping");
            g.m_pendingScrapShips.push_back({bestIdx});
            statsFor(cid).shipsScrapped++;
            m_shipsScrappedThisTurn[cid]++;
            return TextFormat("scrap %s #%d (saves %.0f/turn)",
                              g.m_ships[bestIdx].type.c_str(), bestIdx, bestCost);
        }
        case 6: { // engage: bring an enemy hull to action
            // WHAT A PERSON SHOOTS AT, in order.
            //
            // A loaded transport is the highest-value target on the map: sink
            // it and the invasion it carries dies with it, which is worth far
            // more than the hull. After that, finish what is already hurt --
            // damage does not heal, so a half-dead destroyer is the cheapest
            // kill available. Ties go to the closest, because processNavyCombat
            // scales damage by distance.
            //
            // Transports do not attack. They are the cargo, not the escort.
            int bestMine = -1, bestThem = -1;
            double bestScore = -1e18;
            for (size_t i = 0; i < g.m_ships.size(); ++i) {
                const auto& mine = g.m_ships[i];
                if (mine.countryId != cid || mine.type == "boat") continue;
                bool busy = false;
                for (auto& eo : g.m_pendingShipEngageOrders)
                    if (eo.shipIndex == (int)i) { busy = true; break; }
                if (busy) continue;
                const double reach = g.shipMaxRangeDeg(mine);
                for (size_t j = 0; j < g.m_ships.size(); ++j) {
                    const auto& them = g.m_ships[j];
                    if (them.countryId <= 0 || them.countryId == cid) continue;
                    if (!g.atWarCids(cid, them.countryId)) continue;
                    const double dl = them.lon - mine.lon, dt = them.lat - mine.lat;
                    const double d = std::sqrt(dl * dl + dt * dt);
                    if (d > reach) continue;
                    double score = 0.0;
                    if (them.crew > 0)          score += 1000.0;  // troops aboard
                    score += (100.0 - (double)them.health);       // finish the wounded
                    score -= d;                                   // closer hits harder
                    if (score > bestScore) {
                        bestScore = score; bestMine = (int)i; bestThem = (int)j;
                    }
                }
            }
            if (bestMine < 0) return didNothing("engage: nothing in range");
            g.m_pendingShipEngageOrders.push_back({bestMine, bestThem});
            const auto& t = g.m_ships[bestThem];
            return TextFormat("engage %s #%d (hp %d%s)", t.type.c_str(), bestThem,
                              t.health, t.crew > 0 ? ", loaded" : "");
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
    // ...and it costs the PROPOSER something in the reward, which is the half
    // that was missing. See AI_OVERTURE_REFUSED_CHARGE: a cooldown makes a
    // refusal expensive in the game and says nothing to the net about it.
    m_overturesRefusedThisTurn[sourceCid]++;
}

// ─── Rung one: a player written down ─────────────────────────────────────
//
// See s_scriptedControl. Every rule here is one somebody would give you in a
// tutorial, in the order a person would apply them, and the whole thing is
// deliberately simple: this is the standard to beat, not an attempt at a good
// player. If the trained model cannot reach parity with it, the model is not an
// intermediate opponent whatever its ADVANTAGE against dice says.
int AISystem::scriptedChoice(int module, int cid, const std::vector<bool>& valid,
                             int variant, bool bookTurn) const {
    Game& g = *m_g;
    const Country* c = g.m_countries.getCountry(cid);
    auto stIt = m_stats.find(cid);
    if (!c || stIt == m_stats.end()) return 0;
    const CountryStat& st = stIt->second;

    // First entry the mask allows. Everything below is a preference order, so
    // no rule has to re-derive a precondition the mask already knows.
    auto pick = [&](std::initializer_list<int> prefs) {
        for (int a : prefs)
            if (a >= 0 && a < (int)valid.size() && valid[a]) return a;
        for (size_t i = 0; i < valid.size(); ++i)
            if (valid[i]) return (int)i;
        return 0;
    };

    const bool broke     = g.isBankrupt(cid);
    const bool atWar     = const_cast<AISystem*>(this)->foreignWarCount(cid) > 0;
    const bool threatened = st.threatenedProvinces > 0;
    const bool outmatched = st.enemyAdjArmy > st.army;

    // ── THE EXPLOITS ──
    //
    // Each of these plays ONE human idea and will not be talked out of it. They
    // are deliberately not balanced and not clever; the point is that a person
    // who finds something that works does it every turn, and an AI that has no
    // answer to a one-note opponent has a hole a player will fall straight
    // into. See ScriptVariant.
    if (variant >= SCRIPT_TECH) {
        switch (variant) {
            case SCRIPT_TECH:
                // Never fights. Outgrows the world and dares it to do anything.
                switch (module) {
                    case MOD_ECONOMY:  return pick({9, 7, 1, 4, 3, 2, 0});
                    case MOD_POLITICS: return pick({9, 8, 6, 0});
                    // Defends and sues for peace; never attacks, never declares.
                    case MOD_WAR:      return pick({2, 1, 6, 0});
                    default:           return pick({0});
                }
            case SCRIPT_BLITZ:
                // Never stops. No ceasefires, no economy beyond the army.
                switch (module) {
                    case MOD_ECONOMY:  return pick({10, 1, 7, 0});
                    case MOD_POLITICS: return pick({0});
                    case MOD_WAR:      return pick({3, 4, 1, 2, 5, 0});
                    default:           return pick({3, 4, 1, 0});
                }
            case SCRIPT_DIPLO:
                // The pact hub. Asks everybody for everything, every turn --
                // the screenshot that started all of this, played on purpose.
                switch (module) {
                    case MOD_ECONOMY:  return pick({1, 7, 9, 0});
                    case MOD_POLITICS: return pick({5, 6, 7, 11, 9, 0});
                    case MOD_WAR:      return pick({2, 1, 6, 0});
                    default:           return pick({0});
                }
            case SCRIPT_NAVY:
                // Buys the sea the AI never contests, then uses it.
                switch (module) {
                    case MOD_ECONOMY:  return pick({3, 11, 5, 6, 1, 0});
                    case MOD_POLITICS: return pick({9, 0});
                    case MOD_WAR:      return pick({1, 2, 3, 0});
                    default:           return pick({3, 1, 4, 6, 2, 0});
                }
            default: break;
        }
    }

    switch (module) {
        case MOD_ECONOMY:
            // Pay the bills, keep research running, then build. "Keep research
            // running" above "build" is the one piece of real advice in here:
            // a player who stops researching loses slowly and never notices.
            if (broke) return pick({0, 8});                 // save, cut research
            if (atWar || threatened) return pick({10, 7, 1, 0});  // army focus, fund, industry
            return pick({9, 7, 1, 4, 2, 0});               // build focus, fund, industry, specialise, fort
        case MOD_POLITICS:
            // Hold the country together first; make friends when it is quiet.
            if (st.worstAlignment < 40.0f) return pick({9, 8, 0});   // conciliate, calm
            if (st.provincesLost > 0)       return pick({8, 9, 0});
            if (st.pacts < 2)               return pick({6, 5, 7, 0}); // NAP, alliance, guarantee
            return pick({0});
        case MOD_WAR: {
            // ── The turtle ──
            // Identical in every other respect; it simply never goes on the
            // offensive. It still builds to sufficiency, still holds its
            // borders, still fires artillery at whoever is on them, and still
            // makes peace. What it will not do is attack or declare -- which is
            // the single variable this whole comparison is about.
            if (variant == SCRIPT_TURTLE) {
                if (atWar && outmatched && st.provincesLost > 0) return pick({6, 2, 1, 0});
                const bool adequateT =
                    st.army >= (long long)(std::max(1LL, st.enemyAdjArmy) * 1.5);
                if (!adequateT)  return pick({1, 2, 0});      // recruit, reinforce
                // ARTILLERY STAYS BEHIND REINFORCE, ON EVIDENCE. pick() takes
                // the first valid entry and reinforce is valid on nearly every
                // threatened turn, so naming 5 here has never once fired a
                // shell -- 0 out of 2,049 offers over two 300-turn scenarios.
                //
                // That was tried both ways rather than assumed. Promoting
                // artillery, and separately using it to soften a front the
                // script could not storm, both worked mechanically and both
                // made the script WORSE, monotonically in how much it fired:
                //     0% of war turns  ->  script holds 51.5% of the land
                //    14-15%            ->  42.5%
                //    17-18%            ->  40.2%
                // (same model, same seeds, --eval-ai 2 300 4242 2 --scenarios).
                // A turn and 5-80 treasury buys less than recruiting,
                // reinforcing or attacking with the same turn does, so the
                // trained model's ~0.3% artillery rate is not a policy failure
                // to be corrected -- it is the correct read of the payoff. If
                // artillery is meant to matter, the shell has to get cheaper or
                // hit harder first; until then, leave the ordering alone.
                if (threatened)  return pick({2, 5, 1, 0});   // hold the line
                if (atWar)       return pick({6, 5, 2, 1, 0});// end it if we can
                return pick({1, 0});                          // keep the reserve up
            }
            // AN ARMY FIRST, AND THEN A WAR. In that order, and the order is
            // the whole rule.
            //
            // The first version of this put "attack" above everything whenever
            // the mask allowed it -- and the mask only asks whether we are at
            // war with a frontier and own any troops at all, not whether there
            // is anything we can beat. So it attacked on 97.7% of the turns it
            // was offered, found no target on a third of them, and recruited on
            // 0.1%: a berserker with no army, which is not an intermediate
            // player, it is the mirror image of the recruit-forever collapse
            // this project spent so long digging out of. A turtle beat it 4.35x
            // and the yardstick was useless.
            //
            // Sufficiency is what a person actually uses: build until the army
            // can handle what is on the border, then use it.
            const bool adequate =
                st.army >= (long long)(std::max(1LL, st.enemyAdjArmy) * 1.2);
            if (atWar && outmatched && st.provincesLost > 0)
                return pick({6, 2, 1, 0});                 // losing: get out
            if (!adequate)
                return pick({1, 2, 0});                    // recruit, reinforce
            if (valid.size() > 3 && valid[3]) return pick({3});   // now attack
            if (threatened)                   return pick({2, 5, 1, 0});
            if (atWar)                        return pick({5, 2, 6, 1, 0});
            // ── "DECLARE FROM STRENGTH" NEVER MEASURED ANY STRENGTH ──
            //
            // `enemyAdjArmy` is hostile troops standing ON OUR BORDERS, which
            // is ZERO for a country at peace. So `max(1, 0) * 2` is 2, and the
            // test read "do I have more than two soldiers" — every peaceful
            // country with an army declared war on somebody.
            //
            // It shows up worst on the seat it matters most for. `1939:NOR
            // hood` opens with Norway declaring war on a third party in the
            // book turns before Sweden blitzes it, and no policy work can reach
            // that: the book plays the script, and biasing `declare war` to -30
            // was bit-identical on all three seeds.
            //
            // Compare against the army of the country we would actually attack.
            // findWarTarget is what the mask already asks and what exec issues,
            // so this measures the strength the comment always claimed to.
            //
            // Behind OD_SCRIPT_DECLARE_FIX because the script is the BENCHMARK'S
            // RULER — the rung every seat is scored against — and changing it
            // silently would make every stored rating incomparable. Off is the
            // shipped behaviour, bug and all.
            bool strongEnough = st.army > (long long)(std::max(1LL, st.enemyAdjArmy) * 2);
            if (strongEnough && s_scriptDeclareFix) {
                WarTarget wt;
                if (const_cast<AISystem*>(this)->findWarTarget(cid, wt) && wt.cid >= 0) {
                    auto tIt = m_stats.find(wt.cid);
                    const long long theirs = tIt != m_stats.end() ? tIt->second.army : 0;
                    strongEnough = st.army > (long long)(std::max(1LL, theirs) * 2);
                    // ── AND STRONGER THAN THE TARGET'S WHOLE BLOC ──
                    //
                    // The test above compares us with the country we would
                    // attack, never with who would come to its aid. On the
                    // 1939:NOR hood seat Norway's 55,000 clears Macedonia with
                    // room to spare -- and Macedonia is Sweden's ally, so the
                    // opening war of choice is a war with Sweden, several times
                    // larger, and Norway is dead by turn 40 (journal 15, 34).
                    //
                    // The first fix (journal 34b/c) refused to declare whenever
                    // ANY bigger non-allied army stood next door. That stopped
                    // Norway, and also stopped France and Sweden opening against
                    // a lone weak neighbour while Germany or Russia sat at
                    // peace next to them -- openings the shipped model was
                    // winning (FRA rung 206 -> 65, SWE 247 -> 127). A big
                    // neighbour that is not tied to the target is not in the
                    // war. This asks the right question: sum the target's
                    // allies and guarantors (either direction -- guarantees
                    // are stored one way in places) and refuse if that bloc
                    // out-armies us. Twice the target, once the bloc.
                    // BOOK TURNS ONLY, for the reason journal 34b gives: the
                    // rung keeps its wars so every number stays comparable.
                    // OD_SCRIPT_LOOM_FIX=0 disables it for a comparison.
                    if (strongEnough && s_scriptLoomFix && bookTurn) {
                        const Country* tc = g.m_countries.getCountry(wt.cid);
                        long long bloc = theirs;
                        if (tc) for (const auto& kv : m_stats) {
                            const int ocid = kv.first;
                            if (ocid == cid || ocid == wt.cid || ocid >= Game::REBEL_CID_MIN) continue;
                            const Country* oc = g.m_countries.getCountry(ocid);
                            if (!oc) continue;
                            bool tied = false;
                            auto a = g.m_relations.find(oc->isoA3);
                            if (a != g.m_relations.end()) {
                                auto r = a->second.find(tc->isoA3);
                                if (r != a->second.end()) tied = r->second.alliance || r->second.guarantee;
                            }
                            if (!tied) {
                                auto b = g.m_relations.find(tc->isoA3);
                                if (b != g.m_relations.end()) {
                                    auto r = b->second.find(oc->isoA3);
                                    if (r != b->second.end()) tied = r->second.alliance || r->second.guarantee;
                                }
                            }
                            if (tied) bloc += kv.second.army;
                        }
                        if (bloc >= st.army) strongEnough = false;   // their friends outnumber us
                    }
                } else {
                    strongEnough = false;
                }
            }
            if (strongEnough)
                return pick({4, 1, 0});                    // declare from strength
            return pick({1, 0});
        }
        case MOD_NAVY:
            // Get the men ashore, and stop paying for hulls with nothing to do.
            if (valid.size() > 4 && valid[4]) return pick({4});      // land them
            // An enemy hull within reach is worth answering before anything
            // else: it is the one naval opportunity that expires. The rung
            // needs this as much as the model does -- an opponent that never
            // fights at sea cannot teach anything about fighting at sea.
            if (valid.size() > 6 && valid[6]) return pick({6});      // engage
            if (atWar && st.navalWarTargets > 0) return pick({3, 1, 2, 0});
            if (st.navalTargets > 0)             return pick({1, 3, 0});
            return pick({5, 0});                                     // scrap the idle fleet
        default:
            return pick({0});
    }
}

bool AISystem::scriptedDiplomacy(int targetCid, const std::string& action,
                                 const std::string& sourceIso) const {
    Game& g = *m_g;
    auto stIt = m_stats.find(targetCid);
    if (stIt == m_stats.end()) return false;
    const CountryStat& st = stIt->second;
    const int srcCid = g.cidForIso(sourceIso);
    const bool atWar = const_cast<AISystem*>(this)->foreignWarCount(targetCid) > 0;

    if (action == "request_ceasefire") {
        // Take the peace unless clearly winning. A player who cannot stop a war
        // they are losing is the single most common thing this AI does wrong.
        const long long theirs = srcCid >= 0 && m_stats.count(srcCid)
                                     ? m_stats.at(srcCid).army : 0;
        return st.army < theirs * 1.5;
    }
    if (action == "call_to_arms") return !atWar;      // honour it if we are free
    if (action == "request_nap")  return true;        // peace is cheap
    if (action == "request_alliance" || action == "request_guarantee")
        return st.pacts < 4;                          // friends, but not everybody's
    return false;
}

int AISystem::trueWarGoal(int selfCid, int defenderCid) const {
    Game& g = *m_g;
    const Country* me = g.m_countries.getCountry(selfCid);
    if (!me) return WAR_GOAL_CONQUEST;

    // Dragged in rather than chosen: an ally asked, and that is the whole of
    // the reason. Checked first because it overrides any ambition of our own.
    auto rel = g.m_relations.find(me->isoA3);
    if (rel != g.m_relations.end()) {
        const Country* def = g.m_countries.getCountry(defenderCid);
        if (def) {
            for (const auto& [iso, r] : rel->second) {
                if (!r.alliance && !r.guarantee) continue;
                auto their = g.m_relations.find(iso);
                if (their == g.m_relations.end()) continue;
                auto w = their->second.find(def->isoA3);
                if (w != their->second.end() && w->second.war) return WAR_GOAL_ALLY;
            }
        }
    }
    // Land we claim, which is what findWarTarget preferred when it chose this
    // war and what the attack head is already steering toward.
    if (!g.warGoalIsContradicted(selfCid, selfCid, defenderCid, WAR_GOAL_RECONQUEST))
        return WAR_GOAL_RECONQUEST;
    // A neighbour with troops on our border that we did not pick a fight with.
    auto meIt = m_stats.find(selfCid);
    if (meIt != m_stats.end() && meIt->second.threatenedProvinces > 0)
        return WAR_GOAL_SECURITY;
    // The country running away with the map.
    if (defenderCid == m_world.largestCid && m_world.largestCid != selfCid)
        return WAR_GOAL_HUMBLE;
    return WAR_GOAL_CONQUEST;
}

int AISystem::chooseStatedWarGoal(int selfCid, int defenderCid, int trueGoal) {
    Game& g = *m_g;
    TrainStats& s = statsFor(selfCid);
    auto safe = [&](int goal) {
        return !g.warGoalIsContradicted(defenderCid, selfCid, defenderCid, goal);
    };

    int stated = trueGoal;
    // A naked land grab is the one goal nobody can argue with and the one that
    // reads worst -- so a country that wants land goes looking for a pretext
    // that happens to be true, in descending order of how respectable it
    // sounds. Every candidate here is checked against the same public map the
    // victim can read, so the pretext is never one that falls apart.
    if (trueGoal == WAR_GOAL_CONQUEST || trueGoal == WAR_GOAL_HUMBLE) {
        if (safe(WAR_GOAL_RECONQUEST))    stated = WAR_GOAL_RECONQUEST;
        else if (safe(WAR_GOAL_ALLY))     stated = WAR_GOAL_ALLY;
        else if (safe(WAR_GOAL_SECURITY)) stated = WAR_GOAL_SECURITY;
    }
    if (!safe(stated)) stated = WAR_GOAL_CONQUEST;   // never the checkable lie

    // ...or nothing at all. Declaring war and offering no explanation is a
    // statement of its own, and a country that always has a justification ready
    // is one a player can stop reading.
    {
        std::uniform_real_distribution<float> d(0.0f, 1.0f);
        if (d(m_rng) < WAR_GOAL_SILENCE_CHANCE) stated = WAR_GOAL_NONE;
    }

    if (stated == WAR_GOAL_NONE)        s.warGoalSilent++;
    else if (stated == trueGoal)        s.warGoalTrue++;
    else {
        s.warGoalPretext++;
        if (g.warGoalIsContradicted(defenderCid, selfCid, defenderCid, stated))
            s.warGoalCaught++;
    }
    return stated;
}

void AISystem::noteRefusalHeard(int speakerCid, int hearerCid, int statedReason) {
    if (statedReason == REFUSE_NONE) return;   // nothing was claimed
    if (speakerCid <= 0 || hearerCid <= 0 || !m_g) return;
    // Judged by the same rule the AI holds itself to, from the HEARER's side.
    // A statement the hearer can check against the map and find false is the
    // one that will cost credibility when credibility exists.
    if (m_g->refusalIsContradicted(hearerCid, speakerCid, statedReason))
        statsFor(hearerCid).refusalsHeardFalse++;
    else
        statsFor(hearerCid).refusalsHeard++;
}

int AISystem::chooseStatedRefusal(int selfCid, int askerCid, int trueReason) {
    Game& g = *m_g;
    TrainStats& s = statsFor(selfCid);

    // ── Would this admit something we would rather not? ──
    //
    // "We are losing ground on our borders" and "they are too strong" are both
    // true things that invite the person you are talking to to have a go at
    // you. A country with anything to hide reaches for the excuse that gives
    // nothing away, which is what makes the lie worth telling rather than
    // gratuitous.
    const bool revealing = (trueReason == REFUSE_LOSING_GROUND ||
                            trueReason == REFUSE_OUTGUNNED ||
                            trueReason == REFUSE_WEARINESS);

    auto safe = [&](int r) {
        return r != REFUSE_NONE && !g.refusalIsContradicted(askerCid, selfCid, r);
    };

    int stated = trueReason;
    if (revealing) {
        // Reach for a reason that is both unfalsifiable and says nothing about
        // our condition. Preference over pretext: claiming other wars is a
        // better story but the map may disprove it, so it is only used when it
        // happens to be true.
        if (safe(REFUSE_NO_INTEREST))    stated = REFUSE_NO_INTEREST;
        else if (safe(REFUSE_OWN_WARS))  stated = REFUSE_OWN_WARS;
    }
    // Never say the checkable false thing, whatever the reasoning above wanted.
    if (!safe(stated)) stated = safe(REFUSE_NO_INTEREST) ? REFUSE_NO_INTEREST
                                                        : REFUSE_NONE;

    // SILENCE, sometimes, and not as a fallback. A country that always has an
    // answer is as legible as one that always tells the truth; saying nothing
    // is a real move and the AI should have it. Rare enough that a refusal is
    // usually informative.
    if (stated != REFUSE_NONE) {
        std::uniform_real_distribution<float> d(0.0f, 1.0f);
        if (d(m_rng) < REFUSAL_SILENCE_CHANCE) stated = REFUSE_NONE;
    }

    if (stated == REFUSE_NONE)            s.refusalsSilent++;
    else if (stated == trueReason)        s.refusalsTrue++;
    else {
        s.refusalsLied++;
        // The invariant. See TrainStats::refusalsCaught.
        if (g.refusalIsContradicted(askerCid, selfCid, stated)) s.refusalsCaught++;
    }
    return stated;
}

// ─── What was asked ──────────────────────────────────────
//
// The strings are the ones Game_TurnLogic passes to decideDiplomacy, and the
// same ones the request one-hots in buildFeatures key off. Kept beside the
// decision rather than in a header so a new request kind is one edit away from
// being counted, not two.
int AISystem::offerKindOf(const std::string& action) {
    if (action == "request_ceasefire") return OFFER_CEASEFIRE;
    if (action == "request_alliance")  return OFFER_ALLIANCE;
    if (action == "request_nap")       return OFFER_NAP;
    if (action == "request_guarantee") return OFFER_GUARANTEE;
    if (action == "call_to_arms")      return OFFER_CALL_TO_ARMS;
    if (action == "propose_trade")     return OFFER_TRADE;
    return OFFER_OTHER;
}

int AISystem::offerKindFromFeatures(const std::vector<float>& feats) {
    // The slots decideDiplomacy writes; see the one-hots there. Ordered most
    // common first only for readability -- they are mutually exclusive.
    if ((int)feats.size() <= 112) return OFFER_OTHER;
    if (feats[89] > 0.5f)  return OFFER_CEASEFIRE;
    if (feats[91] > 0.5f)  return OFFER_NAP;
    if (feats[90] > 0.5f)  return OFFER_ALLIANCE;
    if (feats[92] > 0.5f)  return OFFER_GUARANTEE;
    if (feats[112] > 0.5f) return OFFER_TRADE;
    if (feats[80] > 0.5f)  return OFFER_CALL_TO_ARMS;
    return OFFER_OTHER;
}

const char* AISystem::offerKindName(int kind) {
    switch (kind) {
        case OFFER_CEASEFIRE:     return "ceasefire";
        case OFFER_ALLIANCE:      return "alliance";
        case OFFER_NAP:           return "non-aggression";
        case OFFER_GUARANTEE:     return "guarantee";
        case OFFER_CALL_TO_ARMS:  return "call to arms";
        case OFFER_TRADE:         return "trade";
        default:                  return "other";
    }
}

bool AISystem::decideDiplomacy(int targetCid, const std::string& action,
                               const std::string& sourceIso,
                               const std::string& subjectIso,
                               int* statedReasonOut) {
    // What we will say if the answer turns out to be no. Assigned at every
    // refusing return below; a request we accept explains nothing.
    int trueReason = REFUSE_NO_INTEREST;
    // ALWAYS chosen, whether or not the caller asked for it. The choice draws
    // from m_rng for the silence roll and increments the counters, so making it
    // conditional on the out-parameter would give a ceasefire refusal a
    // different random stream from an alliance refusal and would leave whole
    // categories of refusal uncounted -- a difference in behaviour created by
    // which call site happened to want the answer.
    // Trade outcomes are booked against the PROPOSER, next to the tradesOffered
    // that this is the answer to. Booking them against the answerer would put
    // the offer and its fate in different rows of a split report, and the only
    // question worth asking of a trade run -- of the offers made, how many were
    // taken -- would have no single place to be asked.
    const int proposerCid = m_g->cidForIso(sourceIso);
    const bool isTrade = (action == "propose_trade");
    auto refuse = [&](int why) {
        if (isTrade && proposerCid >= 0) statsFor(proposerCid).tradesRefused++;
        const int stated = chooseStatedRefusal(targetCid, m_g->cidForIso(sourceIso), why);
        if (statedReasonOut) *statedReasonOut = stated;
        return false;
    };
    auto accept = [&]() {
        if (isTrade && proposerCid >= 0) statsFor(proposerCid).tradesAccepted++;
        return true;
    };
    // The control group answers diplomacy the way it does everything else. The
    // heuristic gates below still apply to it, because those are machinery
    // rather than policy and the baseline is meant to differ in exactly one
    // thing.
    m_randomThisCountry = isRandomCountry(targetCid) && !m_opponentLoaded;
    // Counted here, before the gates: this country was asked, whatever it goes
    // on to answer. See TrainStats::diploRequests.
    statsFor(targetCid).diploRequests++;
    // ...and the same tally split by kind. See TrainStats::diploAskedOf. Here
    // rather than lower down so it shares the "before the gates" guarantee the
    // line above has: sum(diploAskedOf) == diploRequests is an invariant.
    const int offerKind = offerKindOf(action);
    statsFor(targetCid).diploAskedOf[offerKind]++;
    struct ClearFlag {
        bool& f;
        ~ClearFlag() { f = false; }
    } clearFlag{m_randomThisCountry};

    // WHOSE DIPLOMACY NET ANSWERS THIS.
    //
    // Only ever the opponent's under --vs-model, where it was loaded from a
    // full model file. A league checkpoint carries no diplomacy net, so in
    // training this stays false and the frozen side keeps answering with the
    // current model's, exactly as before. See m_leagueDiplo.
    const bool opponentAnswers =
        m_leagueDiploLoaded && m_leagueCids.count(targetCid) > 0;

    std::vector<float> feats;
    buildFeatures(targetCid, feats);
    // Request-specific context in the spare feature slots
    int srcCid = m_g->cidForIso(sourceIso);
    long long srcArmy = srcCid >= 0 ? m_stats[srcCid].army : 0;
    long long myArmy = std::max(1LL, m_stats[targetCid].army);
    // Who is doing the asking, in the one term that matters for whether an
    // agreement is submission or business. Recorded before every gate below,
    // for the reason given at diploAskedOf: the denominators have to match.
    const bool askerIsStronger = srcArmy > myArmy;
    // War state at a ceasefire request: 0 winning, 1 losing, 2 even, -1 not
    // a ceasefire. Computed ahead of every gate because two of the gates
    // below use it; counted after the head answers (TrainStats::cfAsked) or
    // when a rule answers instead.
    int cfState = -1;
    if (offerKind == OFFER_CEASEFIRE) {
        const CountryStat& ms = m_stats[targetCid];
        const bool winning = myArmy > srcArmy + srcArmy / 4 && ms.provincesLost == 0;
        const bool losing  = srcArmy > myArmy + myArmy / 4 || ms.provincesLost > 0 ||
                             ms.enemyAdjArmy > ms.defenderArmy;
        cfState = (winning && !losing) ? 0 : (losing && !winning) ? 1 : 2;
    }
    if (askerIsStronger) statsFor(targetCid).diploAskedFromStronger[offerKind]++;
    feats[88] = (float)std::tanh(std::log1p((double)srcArmy / (double)myArmy));
    feats[89] = (action == "request_ceasefire") ? 1.0f : 0.0f;
    feats[90] = (action == "request_alliance") ? 1.0f : 0.0f;
    feats[91] = (action == "request_nap") ? 1.0f : 0.0f;
    feats[92] = (action == "request_guarantee") ? 1.0f : 0.0f;
    // Trade, in slot 112 rather than a new one on the end.
    //
    // 112-139 are inside FEATURE_COUNT already and have never been written, so
    // the input vector does not change shape and every existing model.bin
    // still loads. The weight on this slot has never seen a gradient -- the
    // input was always zero -- so it starts neutral and the decision rests on
    // the features that ARE trained, chiefly the deal-value pair below. That
    // is the right starting behaviour for a trade: judge it on what is being
    // offered, and let training sharpen the rest.
    feats[112] = (action == "propose_trade") ? 1.0f : 0.0f;

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
        // AND THESE GATES ARE RIGHT, WHICH IS NOT OBVIOUS. The AI refuses
        // essentially every call to arms it receives, and that reads like the
        // AI abandoning its allies -- weak, and the least charitable thing a
        // player can watch it do. It was tried as the top rung's missing
        // faculty, twice:
        //
        //   1. A thumb on the scale toward accepting (bias[1] += weight).
        //      INERT AT ANY WEIGHT, including 50: these gates answer before
        //      the net is ever consulted, so the bias was downstream of a
        //      decision already taken. Nothing in the output said so -- the
        //      paired test simply returned +0.0 on all five worlds, which is
        //      the signature of a flag that never reaches the decision.
        //   2. Relaxing the gates themselves at the top rung -- one more
        //      concurrent war, and pressure at home no longer a refusal on its
        //      own. That DOES change behaviour (calls answered 53% -> 56%) and
        //      it is WORSE: land against a rusher 29.3% -> 27.7%, worse on
        //      four worlds of five.
        //
        // A country that is already at war, already weary or already being
        // invaded has nothing to send. Answering anyway is the same mistake the
        // offensive coalition made -- arriving separately and dying separately
        // -- and these four gates are what stop it. The 100% refusal rate is
        // not the AI failing to keep its promises; it is the AI correctly
        // noticing it cannot afford them, in a world where somebody is always
        // attacking.
        //
        //
        // Each of these is a state in which joining does not merely cost more
        // than the alliance is worth, it costs more than the country has. They
        // are checked before the net is consulted, so no sample is recorded:
        // this is not a decision that was taken, it is one that was not
        // available. See AI_CALL_* in the header.
        // The gate that fired, as a reason rather than only as a log string.
        // These four ARE the true reasons -- the country genuinely cannot join
        // -- and what it goes on to tell the asker is decided separately.
        int gate = REFUSE_NONE;
        const int myWars = foreignWarCount(targetCid);
        if (myWars >= AI_CALL_MAX_OWN_WARS)
            gate = REFUSE_OWN_WARS;
        else if (m_g->warWearinessOf(targetCid) >= AI_CALL_WEARINESS_BLOCK)
            gate = REFUSE_WEARINESS;
        // Being invaded is a reason to stay home; having a neighbour's stack
        // parked on a quiet border is not, and neither is losing a province to
        // a rebellion somewhere. The old test fired on either, which on a busy
        // map is most of the time.
        else if (ts.threatenedProvinces > 0 && ts.enemyAdjArmy > ts.defenderArmy)
            gate = REFUSE_LOSING_GROUND;
        else if (!subjectIso.empty()) {
            // Who we would actually be fighting, and with whom. The aggressor
            // is the third party in the request, not the ally making it — the
            // ally's strength is a reason to join, the aggressor's is a reason
            // not to, and only the caller knows which is which.
            const int aggCid = m_g->cidForIso(subjectIso);
            const long long ourSide = std::max(1LL, ts.army + srcArmy);
            const long long theirArmy = aggCid >= 0 ? m_stats[aggCid].army : 0;
            if (theirArmy > (long long)(ourSide * AI_CALL_MAX_ENEMY_ODDS))
                gate = REFUSE_OUTGUNNED;
        }
        if (gate != REFUSE_NONE) {
            trueReason = gate;
            logDecision(targetCid, MOD_POLITICS, 0, 0.0f,
                        std::string("REFUSE call_to_arms from ") + sourceIso +
                        " (" + refusalText(gate) + ")");
            return refuse(gate);
        }

        // No gate fired: the net decides, but against a thumb on the scale.
        // Answering is a war and seven points of unrest; the alliance it saves
        // is worth that only when the net actively wants the fight.
        bias.assign(DIPLO_ACTIONS, 0.0f);
        bias[1] = -AI_CALL_RELUCTANCE;
    }
    if (action == "request_alliance" || action == "request_nap" ||
        action == "request_guarantee") {
        // ── A STANDING AGREEMENT IS A PROMISE, AND NOBODY MAY MAKE ENDLESSLY
        //    MANY OF THEM ── See AI_ALLY_MAX_PACTS for the measurement.
        //
        // BOTH SIDES ARE COUNTED. Capping only the answerer does nothing about
        // the case the cap was written for: one country asking the entire map
        // is a hundred answerers holding one pact each, all of them far under
        // any limit. The asker's own ledger is what makes a treaty network
        // finite -- and all three standing agreements are counted, because a
        // world where nobody may ally but everybody signs a non-aggression
        // pact with everybody is the same frozen map in a different colour.
        const CountryStat& ts = m_stats[targetCid];
        const int mine = ts.pacts;
        const int theirs = (srcCid >= 0) ? m_stats[srcCid].pacts : 0;
        if (mine >= AI_ALLY_MAX_PACTS || theirs >= AI_ALLY_MAX_PACTS) {
            trueReason = REFUSE_NO_INTEREST;
            logDecision(targetCid, MOD_POLITICS, 0, 0.0f,
                        TextFormat("REFUSE %s from %s (pacts: we hold %d, they "
                                   "hold %d, cap %d)", action.c_str(),
                                   sourceIso.c_str(), mine, theirs,
                                   AI_ALLY_MAX_PACTS));
            return refuse(REFUSE_NO_INTEREST);
        }
        bias.assign(DIPLO_ACTIONS, 0.0f);
        // A non-aggression pact still costs nothing and still buys peace
        // outright, so it keeps its thumb toward yes -- under the cap.
        if (action == "request_nap") bias[1] += AI_NAP_WILLINGNESS;
        // Approaching the cap from either side, the next signature is worth
        // less than the last. Continuous rather than a second gate: a country
        // holding five pacts should be a harder sell than one holding none,
        // and a step function cannot say that.
        bias[1] -= AI_ALLY_CROWDING *
                   (float)std::max(mine, theirs) / (float)AI_ALLY_MAX_PACTS;
        if (action == "request_alliance" || action == "request_guarantee") {
            // Not a gate: a country fighting a war is not an illegitimate ally,
            // it is an expensive one, and how expensive is exactly the judgement
            // the net should be making. So this is a thumb on the scale and the
            // policy still has the last word.
            //
            // A GUARANTEE ANSWERS TO THIS TOO, and to a standing charge on top.
            // Every war the asker is in is a war the guarantor gets dragged
            // into -- and unlike an alliance it is dragged in automatically,
            // with no call to arms to weigh and no gate to refuse at. See
            // AI_GUARANTEE_RELUCTANCE.
            const int askerWars = (srcCid >= 0) ? foreignWarCount(srcCid) : 0;
            bias[1] -= AI_ALLY_WAR_RELUCTANCE *
                       std::min(1.0f, (float)askerWars / 2.0f);
            if (action == "request_guarantee") bias[1] -= AI_GUARANTEE_RELUCTANCE;
        }
    }

    // ── AND WHETHER THEY ARE THE ONE EVERYONE IS WORRIED ABOUT ──
    //
    // A power taking the map finds the table colder: nobody wants to be the
    // one that signed with them. Applies to every request type, because the
    // point is that the leader's diplomacy stops working, not that one
    // particular pact does. See COALITION_SHARE.
    if (srcCid >= 0 && isCoalitionTarget(targetCid, srcCid)) {
        if (bias.empty()) bias.assign(DIPLO_ACTIONS, 0.0f);
        bias[1] -= COALITION_WEIGHT * coalitionPressure();
    }

    // ── AND WHETHER THEY HAVE BEEN TALKING TO US ──
    //
    // The one place the optional language-model module reaches a decision.
    // Without it this is exactly 0.0f -- llmDispositionToward checks the
    // module itself rather than trusting callers -- so it adds a term worth
    // nothing and takes no branch that a game without the module would not
    // also take. That inertness is asserted, not assumed: see
    // tests/llm_influence_test.cpp.
    //
    // What it buys is that writing to a country MATTERS. An advisor that
    // conducts a warm correspondence and then answers every request exactly as
    // it would have anyway is a chat window bolted to a strategy game, and a
    // player works that out inside two turns.
    if (srcCid >= 0) {
        const float warmth = m_g->llmDispositionToward(targetCid, srcCid);
        if (warmth != 0.0f) {
            if (bias.empty()) bias.assign(DIPLO_ACTIONS, 0.0f);
            bias[1] += AI_LLM_DISPOSITION * warmth;
        }
    }
    // ...and a call to arms AGAINST them is the one worth answering. This is
    // the half that turns a cold shoulder into a coalition: the leader's
    // enemies find allies, so a war against it is a war on several fronts.
    if (action == "call_to_arms" && !subjectIso.empty()) {
        const int aggCid = m_g->cidForIso(subjectIso);
        if (isCoalitionTarget(targetCid, aggCid)) {
            if (bias.empty()) bias.assign(DIPLO_ACTIONS, 0.0f);
            bias[1] += COALITION_WEIGHT * coalitionPressure();
        }
    }

    // WHAT THEIR WORD IS WORTH HERE. Applies to every kind of request, so it
    // sits after the per-action thumbs above rather than inside one of them.
    // Scaled by the shortfall: somebody who has never been caught pays nothing,
    // which is what keeps this a cost of lying rather than a tax on asking.
    {
        const Country* tc = m_g->m_countries.getCountry(targetCid);
        if (tc) {
            const float cred = m_g->credibility(sourceIso, tc->isoA3);
            if (cred < 1.0f) {
                if (bias.empty()) bias.assign(DIPLO_ACTIONS, 0.0f);
                bias[1] -= CREDIBILITY_WEIGHT * (1.0f - cred);
            }
        }
    }

    // The deal on the table. Without these the diplomacy net judged a ceasefire
    // purely on army ratios: the player could offer three provinces and a
    // fortune, or demand them, and the answer was identical, because the terms
    // were never looked at. Signed from the RECIPIENT's point of view — what
    // they gain minus what they give up.
    // A trade is the same question as a ceasefire minus the war, so it is
    // valued through the same pair -- the terms live in the same map under the
    // same key. Without this a trade offer would reach the net with an empty
    // deal and be judged on army ratios alone, which is the exact bug the
    // comment above describes.
    float netProv = 0.0f, netMoney = 0.0f;
    if (action == "request_ceasefire" || action == "propose_trade") {
        const Country* tc = m_g->m_countries.getCountry(targetCid);
        if (tc) {
            auto tit = m_g->m_pendingCeasefireTerms.find(sourceIso + "|" + tc->isoA3);
            if (tit != m_g->m_pendingCeasefireTerms.end()) {
                const CeasefireTerms& t = tit->second;
                // LAND IN GOLD, not land by the head.
                //
                // This counted provinces: a swamp and an industrial core both
                // scored 1, so the feature could say how MUCH ground moved and
                // never what it was worth. Worse, the two halves of a deal were
                // on scales that did not meet -- 3 provinces to 500 gold -- so
                // the ordinary offer of ~400 gold for one province arrived as
                // +0.66 money against -0.32 land and was simply correct to
                // accept. Measured across two 3,400+ trade runs: 95.0% accepted
                // under flat pricing and 95.3% after the asker started pricing
                // by income, which is what a threshold nobody can fail looks
                // like.
                //
                // Valuing both sides in gold is what lets a refusal happen. The
                // basis is the asker's own (provincePrice, exec case 11), so the
                // two ends of a trade finally measure the same thing.
                auto landGold = [&](const std::vector<int>& pids) {
                    double sum = 0.0;
                    for (int pid : pids) {
                        auto pit = m_g->m_provinceIndustry.find(pid);
                        const double perTurn = (pit != m_g->m_provinceIndustry.end())
                                             ? (double)pit->second.income : 0.0;
                        sum += std::clamp(perTurn * TRADE_PAYBACK_TURNS,
                                          TRADE_PRICE_PROV_MIN, TRADE_PRICE_PROV_MAX);
                    }
                    return sum;
                };
                // ── A DEMANDED RELEASE IS LAND LOST, NOT LAND TRADED ──
                //
                // `theirReleaseProvs` leaves the recipient and does NOT arrive
                // anywhere the sender owns -- it becomes a third country. So it
                // is a straight loss to the recipient, priced like any other
                // ground, and it must be: without it a country would sign away
                // a third of itself for free because the net read zero.
                //
                // The other direction is worth something and not the same
                // thing. When the SENDER dismantles itself the recipient gains
                // no ground; it gains a weaker rival and a buffer between them.
                // Valued at a quarter, which is a guess made explicit rather
                // than a zero pretending the event did not happen.
                netProv = (float)(landGold(t.ourProvs) - landGold(t.theirProvs)
                        - landGold(t.theirReleaseProvs)
                        + 0.25 * landGold(t.ourReleaseProvs)
                        + TRADE_PRICE_PER_CLAIM * ((double)t.ourDropClaims.size() -
                                                   (double)t.theirDropClaims.size()));
                netMoney = (float)t.ourMoney - (float)t.theirMoney;

                // ── NOBODY VOLUNTEERS TO BE DISMANTLED ──
                // A gate in the same sense as the call-to-arms gates: checked
                // before the net, so this is not a decision taken badly but one
                // never available. Fires only when BOTH hold -- a ruinous share
                // is leaving AND the deal is net negative -- so paying the going
                // rate for a third of a country still gets a real answer.
                {
                    const CountryStat& st = m_stats[targetCid];
                    // GROUND LEAVING IS GROUND LEAVING, whoever ends up with
                    // it. A demand to release half the country is exactly the
                    // offer this gate exists to refuse, and counting only
                    // ceded provinces would have walked straight past it.
                    const double provShare = st.provinces > 0
                        ? (double)(t.theirProvs.size() + t.theirReleaseProvs.size())
                          / (double)st.provinces : 0.0;
                    const double cashShare = tc->treasury > 1.0
                        ? (double)t.theirMoney / (double)tc->treasury : 0.0;
                    const bool ruinous = provShare >= AI_TRADE_RUIN_PROV_SHARE ||
                                         cashShare >= AI_TRADE_RUIN_CASH_SHARE;
                    const float net = netProv + netMoney;

                    // ── NOBODY VOLUNTEERS TO BE DISMANTLED ──
                    // Now for a CEASEFIRE as well as a trade. It was written
                    // for trades alone, and a ceasefire carries the same terms
                    // through the same struct -- so the one offer a country is
                    // under most pressure to sign was the one with no floor
                    // under it at all.
                    if (ruinous && net < 0.0f) {
                        trueReason = REFUSE_NO_INTEREST;
                        logDecision(targetCid, MOD_POLITICS, 0, 0.0f,
                                    TextFormat("REFUSE %s from %s "
                                               "(ruinous: %.0f%% of provinces, "
                                               "%.0f%% of treasury, net %.0f)",
                                               action.c_str(), sourceIso.c_str(),
                                               provShare * 100.0, cashShare * 100.0,
                                               (double)net));
                        return refuse(REFUSE_NO_INTEREST);
                    }

                    // ── AND A BAD BARGAIN IS STILL A BAD BARGAIN ──
                    //
                    // The gate above only ever fired when a ruinous share was
                    // leaving AND the deal was negative, so handing over a
                    // third of a country for nothing passed it and went to the
                    // head -- which took 95.3% of everything it was offered
                    // (measured over two 3,400-trade runs; see the pricing note
                    // above). That is the "they give you land for nothing"
                    // report, and no amount of pricing fixes it while the
                    // answer is a judgement call on an input the head has
                    // barely been trained on.
                    //
                    // A peacetime trade has no coercion in it: there is no
                    // reason to accept one that loses you a province's worth
                    // and returns nothing. Refused here, in the resolver, so
                    // the rule binds whatever the head thinks -- and left to
                    // the head inside the band, where it is a real judgement.
                    //
                    // Deliberately NOT applied to a ceasefire: signing away
                    // ground to end a war you are losing is a good deal that
                    // prices as a bad one.
                    if (action == "propose_trade" && net < -AI_TRADE_NET_FLOOR) {
                        trueReason = REFUSE_NO_INTEREST;
                        logDecision(targetCid, MOD_POLITICS, 0, 0.0f,
                                    TextFormat("REFUSE propose_trade from %s "
                                               "(net %.0f, below floor %.0f)",
                                               sourceIso.c_str(), (double)net,
                                               (double)-AI_TRADE_NET_FLOOR));
                        return refuse(REFUSE_NO_INTEREST);
                    }
                    // ── TWO RULES, NOT A REWARD (user decision, 2026-09-04) ──
                    //
                    // The head decides only the genuinely open middle. Two
                    // outcomes are not open and are settled here, for every
                    // model and for the scripted cohort alike:
                    //   1. Land is never ceded at a loss. A trade that takes a
                    //      province of ours and nets below zero at our own
                    //      prices is refused, whatever the head would say.
                    //   2. A gift -- we give nothing and receive something --
                    //      or a deal a province's worth in our favour is
                    //      accepted without consulting the head. A head that
                    //      refuses free land (this evening's wall refused
                    //      everything) is not exercising judgement.
                    // `t.theirProvs` etc. are what WE, the recipient, give.
                    if (action == "propose_trade") {
                        if (!t.theirProvs.empty() && net < 0.0f) {
                            trueReason = REFUSE_NO_INTEREST;
                            statsFor(targetCid).tradeRuleRefusals++;
                            logDecision(targetCid, MOD_POLITICS, 0, 0.0f,
                                        TextFormat("REFUSE propose_trade from %s "
                                                   "by rule: cedes land at a loss (net %.0f)",
                                                   sourceIso.c_str(), (double)net));
                            return refuse(REFUSE_NO_INTEREST);
                        }
                        const bool givesNothing = t.theirProvs.empty() && t.theirMoney <= 0 &&
                                                  t.theirDropClaims.empty();
                        if (net > 0.0f && (givesNothing || net >= AI_TRADE_NET_FLOOR)) {
                            statsFor(targetCid).tradeRuleAccepts++;
                            statsFor(targetCid).diploAccepted++;
                            statsFor(targetCid).diploSaidYes[offerKind]++;
                            if (askerIsStronger) statsFor(targetCid).diploYesToStronger[offerKind]++;
                            logDecision(targetCid, MOD_POLITICS, 0, 0.0f,
                                        TextFormat("ACCEPT propose_trade from %s by rule (%s, net +%.0f)",
                                                   sourceIso.c_str(),
                                                   givesNothing ? "gift" : "clearly favourable",
                                                   (double)net));
                            return accept();
                        }
                    }
                }
            }
        }
    }
    // Same divisor for both, because both are now gold. That is the whole point:
    // a deal is good when what arrives outweighs what leaves, and neither side
    // can be made to look bigger by the units it happens to be counted in.
    // ── CEASEFIRE RULES, the mirror of the trade rules (journal 35g) ──
    //
    // Every reward-side attempt to make this kind conditional failed: with
    // the head unfrozen it accepted 100% at two pact weights and two
    // lengths, and with the per-decision credit it refused 100% -- 0/12
    // while LOSING (N6 split). The open middle is the head's; the two ends
    // are settled here, for every model and the scripted cohort alike:
    //   A. Losing, and the ceasefire costs us nothing (white peace, or terms
    //      in our favour, no land of ours in them): accept by rule.
    //   B. Winning, and the terms give us nothing (net <= 0): refuse by rule.
    //      A ceasefire from a winning position is sold, not given.
    if (action == "request_ceasefire" && cfState >= 0) {
        const float cfNet = netProv + netMoney;          // 0 for a white peace
        bool cfCedes = false;
        if (const Country* tc2 = m_g->m_countries.getCountry(targetCid)) {
            auto tit2 = m_g->m_pendingCeasefireTerms.find(sourceIso + "|" + tc2->isoA3);
            cfCedes = tit2 != m_g->m_pendingCeasefireTerms.end() && !tit2->second.theirProvs.empty();
        }
        if (cfState == 1 && cfNet >= 0.0f && !cfCedes) {
            statsFor(targetCid).ceasefireRuleAccepts++;
            statsFor(targetCid).cfAsked[1]++;
            statsFor(targetCid).cfYes[1]++;
            statsFor(targetCid).diploAccepted++;
            statsFor(targetCid).diploSaidYes[offerKind]++;
            if (askerIsStronger) statsFor(targetCid).diploYesToStronger[offerKind]++;
            logDecision(targetCid, MOD_POLITICS, 0, 0.0f,
                        TextFormat("ACCEPT request_ceasefire from %s by rule (losing, costs nothing, net %.0f)",
                                   sourceIso.c_str(), (double)cfNet));
            return accept();
        }
        // B applies only when the winner has something to take: a claim on
        // the asker's land. Applied to every winner it bound the rung too,
        // and a winning Russia that used to grant Sweden a free peace stopped
        // -- SWE rung 127 -> 23, shipped model 108 -> 78 (journal 35h). A
        // winner with no claim lets the head weigh weariness against pride.
        bool hasClaimOnAsker = false;
        if (const Country* tc3 = m_g->m_countries.getCountry(targetCid)) {
            auto cl = m_g->m_claims.find(tc3->isoA3);
            if (cl != m_g->m_claims.end())
                for (int pid : cl->second)
                    if (pid >= 0 && pid < (int)m_g->m_provinceCountryLookup.size() &&
                        m_g->m_provinceCountryLookup[pid] == srcCid) { hasClaimOnAsker = true; break; }
        }
        if (cfState == 0 && cfNet <= 0.0f && hasClaimOnAsker) {
            trueReason = REFUSE_NO_INTEREST;
            statsFor(targetCid).ceasefireRuleRefusals++;
            statsFor(targetCid).cfAsked[0]++;
            logDecision(targetCid, MOD_POLITICS, 0, 0.0f,
                        TextFormat("REFUSE request_ceasefire from %s by rule (winning, nothing offered, net %.0f)",
                                   sourceIso.c_str(), (double)cfNet));
            return refuse(REFUSE_NO_INTEREST);
        }
    }
    feats[93] = std::tanh(netProv / 500.0f);
    feats[94] = std::tanh(netMoney / 500.0f);

    // ONLY THIS KIND'S PAIR IS SELECTABLE. Every other pair is masked out, which
    // is the whole mechanism: the gradient for a ceasefire reaches the ceasefire
    // rows and cannot move the pact rows. See DIPLO_OUTPUTS.
    std::vector<bool> valid(DIPLO_OUTPUTS, false);
    valid[offerKind * DIPLO_ACTIONS + 0] = true;
    valid[offerKind * DIPLO_ACTIONS + 1] = true;
    float score;
    float diploLogProb = 0.0f;
    // THROUGH THE TRUNK. m_diplo is a {TRUNK_OUT, DIPLO_ACTIONS} head -- it
    // reads the shared embedding, exactly like every policy head, and that is
    // what runLearningWork trains it on.
    //
    // This used to hand it `feats` directly: FEATURE_COUNT floats into a net
    // whose first layer is TRUNK_OUT wide. NeuralNet::forward returns an EMPTY
    // vector on a size mismatch, pickAction answers `if (logits.empty()) return
    // 0`, and action 0 is REJECT. So every ceasefire, alliance, non-aggression
    // pact, guarantee and call to arms was declined unconditionally, by every
    // country, in shipped games as well as in training -- and the head was
    // never consulted at all. It is also why "calls answered" measured 0% on
    // every seed, and why no reward change to the coalition terms ever moved
    // it: nothing downstream of this line was running.
    //
    // Deliberately the SAME call shape for both nets: the two sides of a
    // head-to-head have to be fed identically or the match is not one.
    // The scripted rung answers by rule, before any net is consulted. Its
    // refusal still goes through chooseStatedRefusal below, because what a
    // country SAYS is part of the game rather than part of its brain.
    if (isRandomCountry(targetCid) && s_scriptedControl) {
        const bool yes = scriptedDiplomacy(targetCid, action, sourceIso);
        if (yes) {
            statsFor(targetCid).diploAccepted++;
            statsFor(targetCid).diploSaidYes[offerKind]++;
            if (askerIsStronger) statsFor(targetCid).diploYesToStronger[offerKind]++;
            return accept();
        }
        return refuse(trueReason);
    }

    // Past every gate: from here the policy decides. See diploReachedNet — this
    // is the denominator that makes a refusal rate a statement about the HEAD.
    statsFor(targetCid).diploReachedNet[offerKind]++;

    // Copied, not referenced: forward() hands back the net's own activation
    // buffer, and takeTurn learned the hard way that holding a reference to one
    // across another forward pass is a bug waiting for the next edit.
    const std::vector<float> demb =
        opponentAnswers ? m_leagueTrunk.forward(feats) : m_trunk.forward(feats);
    // The thumbs on the scale above are all written as [reject, accept], because
    // that is the decision they are about. Widened here, at the one place that
    // needs the layout, so none of them has to know about it.
    std::vector<float> wideBias;
    if (!bias.empty()) {
        wideBias.assign(DIPLO_OUTPUTS, 0.0f);
        for (int i = 0; i < DIPLO_ACTIONS; ++i)
            wideBias[offerKind * DIPLO_ACTIONS + i] = bias[i];
    }
    // `a` is the RAW output index and is what gets recorded: the update has to
    // move the row that was actually chosen. `answer` is the decision.
    // neutralProbs: the policy's OWN distribution at temperature 1. The eval
    // difficulty runs at T=0.18, where a one-unit logit lead is about 4:1, so
    // entropy computed from the SAMPLED log-probability measures the
    // temperature and not the head — it reads 0.000 for any head that is merely
    // confident. See TrainStats::diploEntropySum.
    std::vector<float> neutralProbs;
    int a = pickAction(opponentAnswers ? m_leagueDiplo : m_diplo, demb, valid,
                       score, /*graveAction=*/-1,
                       wideBias.empty() ? nullptr : &wideBias, &diploLogProb,
                       &neutralProbs);
    const int answer = a - offerKind * DIPLO_ACTIONS;
    // War state at a ceasefire answer, the test the credit uses: 0 winning,
    // 1 losing, 2 even. Counted here, past the gates, so it is the net's own
    // answers that are split. See TrainStats::cfAsked.
    if (cfState >= 0 && !opponentAnswers) {
        statsFor(targetCid).cfAsked[cfState]++;
        if (answer == 1) statsFor(targetCid).cfYes[cfState]++;
    }
    m_lastDiploLogProb = diploLogProb;
    // See TrainStats::diploEntropySum. Over the two VALID outputs only, at
    // temperature 1, so this is the policy's own uncertainty and is comparable
    // across difficulties. ln2 = 0.693 is undecided; 0 is a head that has made
    // its mind up and will not be moved by a reward change without a retrain.
    // Marginals for the collapse guard, in the ANSWER space so the ceiling is
    // ln2 rather than ln14 -- twelve of the fourteen outputs are masked off on
    // every decision and counting them would make the head look healthy while
    // its actual choice collapsed.
    if (!opponentAnswers && (int)neutralProbs.size() == DIPLO_OUTPUTS) {
        for (int i = 0; i < DIPLO_ACTIONS; ++i) {
            m_marginalOffered[MOD_COUNT + offerKind][i] += 1.0;
            // PROBABILITY MASS, not a count -- the module path records
            // nprob[vi] and the controller computes H over that mean
            // distribution. A count would measure action FREQUENCY instead,
            // which is a different quantity and would not compare with the
            // four heads the guard already watches.
            m_marginalChosen[MOD_COUNT + offerKind][i] +=
                neutralProbs[offerKind * DIPLO_ACTIONS + i];
        }
    }
    if ((int)neutralProbs.size() == DIPLO_OUTPUTS) {
        double H = 0.0;
        for (int i = 0; i < DIPLO_ACTIONS; ++i) {
            const double pr = neutralProbs[offerKind * DIPLO_ACTIONS + i];
            if (pr > 1e-9) H -= pr * std::log(pr);
        }
        statsFor(targetCid).diploEntropySum[offerKind] += H;
        statsFor(targetCid).diploEntropyN[offerKind]++;
    }
    // Record in the country's experience so the diplo net learns too
    auto it = m_pending.find(targetCid);
    if (it != m_pending.end() && !it->second.empty()) {
        it->second.back().action[MOD_COUNT] = a;
        it->second.back().acted[MOD_COUNT] = true;
        // The probability the policy gave this answer, for PPO's ratio. Without
        // it the ratio is measured against zero and every diplomatic sample
        // looks infinitely off-policy.
        it->second.back().logProb[MOD_COUNT] = m_lastDiploLogProb;
        // ── THE CEASEFIRE CREDIT ──
        //
        // Priced under the window reward, accepting a ceasefire is a certain
        // small positive (gated loss terms go to zero, weariness eases) and
        // refusing is a forecast the value head has to make. Certain beat
        // forecast in every state: N3 and N5 accepted 23/23, 11/11, 25/25 at
        // pact weights 2.5 and 1.0 alike (journal 35d). The head can see the
        // army ratio (feature 88); nothing paid it for using it. This does:
        // winning = our army clearly larger and nothing lost this turn;
        // losing = the mirror, or hostile troops on our border outnumbering
        // ours there. Even wars earn nothing either way.
        if (cfState >= 0 && s_ceasefireCredit != 0.0f) {
            const bool accepted = (answer == 1);
            float credit = 0.0f;
            if (cfState == 0)      credit = accepted ? -0.8f : 0.4f;   // winning
            else if (cfState == 1) credit = accepted ? 0.6f : -0.4f;   // losing
            it->second.back().ceasefireCredit += s_ceasefireCredit * credit;
        }
        // ...AND THE STATE THAT ANSWER WAS GIVEN IN. See Experience::
        // diploFeatures: the request lives in slots the country's own turn
        // features leave at zero, so without this the update re-derives the
        // embedding from a state with no request in it and teaches the head to
        // answer a question it was never asked.
        it->second.back().diploFeatures = feats;
        it->second.back().diploRelCand  = m_lastRelCand;
    }
    if (answer == 1) {
        statsFor(targetCid).diploAccepted++;
        statsFor(targetCid).diploSaidYes[offerKind]++;
        if (askerIsStronger) statsFor(targetCid).diploYesToStronger[offerKind]++;
        logDecision(targetCid, MOD_POLITICS, answer, score,
                    std::string("ACCEPT ") + action + " from " + sourceIso);
        return accept();
    }
    // No gate fired and the policy still said no, so the true reason is simply
    // that it did not want to. That is not a lesser reason than the gates --
    // it is the one the net was consulted about.
    logDecision(targetCid, MOD_POLITICS, answer, score,
                std::string("REJECT ") + action + " from " + sourceIso);
    return refuse(trueReason);
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
    // ...AND THE WORLD, for the same reason. m_world is otherwise built once in
    // beginTurn, so the standing terms below would compare where a country
    // stood at the start of the window against where it stood at the start of
    // THIS turn -- one turn stale, and stale in the direction that hides
    // exactly the thing they exist to catch: ground changing hands this turn.
    // One sort over the living countries, once a turn.
    updateWorld();

    float rewardSum[MOD_COUNT] = {0, 0, 0, 0};
    int rewardN = 0;

    // ── Phase 1: rewards and normalisation (sequential) ──
    //
    // The running reward statistics are order-dependent, so this half has to
    // stay serial. It is also cheap — a handful of tanh calls per experience.
    // All the expensive work (every net's forward and backward passes)
    // is deferred into `m_work` and run in parallel below.
    m_work.clear();
    // `nextFeats` is the state the window ended in, or nullptr when the window
    // ended the episode. See WorkItem::nextFeatures for why it exists.
    auto applyUpdate = [&](int cid, Experience& exp, const float* rewards,
                           float diploReward,
                           const std::vector<float>* nextFeats = nullptr) {
        // Keep this state for the map-end outcome regression -- the same state
        // the value head is about to be trained on with the bootstrapped
        // target, so the two objectives are fitted to the same input. See
        // VALUE_MC_WEIGHT.
        if (s_valueMcWeight > 0.0f && m_outcomeBuf.size() < VALUE_MC_MAX)
            m_outcomeBuf.push_back({exp.features, cid});
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
            // Clamped like the diplomacy head's already is. The running
            // variance SHRINKS as a policy settles, and a z-score divided by a
            // shrinking deviation turns hair-line reward differences into
            // full-scale advantages: the quieter the policy, the louder every
            // fluctuation, which is collapse dynamics, not learning. +-3 keeps
            // the scale honest without touching ordinary samples.
            norm = std::clamp(norm, -3.0f, 3.0f);

            // EVERY ACTION THE MODULE TOOK, not just its first. Each shares
            // this window's reward and baseline -- they were taken in the same
            // state toward the same outcome -- and differs only in which action
            // the ratio is measured against. See Experience::extras.
            // ── TRANSITIONS FOR THE FORWARD MODEL ──
            //
            // A dynamics sample needs two states that genuinely FOLLOW one
            // another. Within a module they do: pick k+1 re-reads the world
            // after pick k spent the money and queued the orders (see the note
            // in runModule), so ea.features are consecutive by construction.
            //
            // ACROSS modules they do not, and that is why this only pairs picks
            // of the same module. Every module's FIRST pick is taken against
            // the embedding computed at the top of the country's turn, so the
            // politics head's opening state is stale by however much the
            // economy head just did. Training a forward model on a pair like
            // that would teach it that economy actions are caused by politics
            // ones.
            {
                const Experience::ExtraAction* prev = nullptr;
                for (const auto& ea : exp.extras) {
                    if (ea.module != m || ea.action < 0 || ea.features.empty()) {
                        continue;
                    }
                    if (prev) {
                        WorkItem dw;
                        dw.module = MOD_COUNT + 2;   // the dynamics sentinel
                        dw.dynModule = m;
                        dw.action = prev->action;
                        dw.features = prev->features;
                        dw.nextFeatures = ea.features;
                        dw.cid = cid;
                        m_work.push_back(std::move(dw));
                    }
                    prev = &ea;
                }
                // ...and the turn's opening state IS current for the first
                // module to act, so its k=0 -> k=1 step is a real transition.
                if (m == MOD_ECONOMY && !exp.features.empty() &&
                    exp.acted[m] && exp.action[m] >= 0) {
                    for (const auto& ea : exp.extras)
                        if (ea.module == m && !ea.features.empty()) {
                            WorkItem dw;
                            dw.module = MOD_COUNT + 2;
                            dw.dynModule = m;
                            dw.action = exp.action[m];
                            dw.features = exp.features;
                            dw.nextFeatures = ea.features;
                            dw.cid = cid;
                            m_work.push_back(std::move(dw));
                            break;
                        }
                }
            }

            for (auto& ea : exp.extras) {
                if (ea.module != m || ea.action < 0) continue;
                WorkItem xw;
                xw.module = m;
                xw.action = ea.action;
                xw.norm = norm;
                // ITS OWN STATE, so the value baseline -- and therefore the
                // advantage R - V(s) -- is this action's rather than the whole
                // turn's. Sharing exp.features gave every action in a turn an
                // identical baseline, which is what buried the rare expensive
                // decisions among the frequent cheap ones.
                xw.features = ea.features.empty() ? exp.features : ea.features;
                xw.validMask = ea.validMask;
                xw.mixScale = ea.mixScale;
                xw.mixFloor = ea.mixFloor;
                xw.teacher  = ea.teacher;
                xw.fromBook = ea.fromBook;
                xw.relCand = exp.relCand;
                xw.acts = std::move(ea.acts);
                xw.cid = cid;
                xw.oldLogProb = ea.logProb;
                if (nextFeats) {
                    xw.nextFeatures = *nextFeats;
                    xw.bootDiscount = BOOTSTRAP_DISCOUNT;
                }
                m_work.push_back(std::move(xw));
            }

            WorkItem w;
            w.module = m;
            w.action = exp.action[m];
            w.norm = norm;
            w.features = exp.features;
            w.relCand = exp.relCand;
            w.acts = std::move(exp.acts[m]);
            w.cid = cid;
            w.oldLogProb = exp.logProb[m];
            w.visits = exp.visits[m];      // MCTS policy target; see Experience
            w.validMask = std::move(exp.validMask[m]);
            w.mixScale = exp.mixScale[m];
            w.mixFloor = exp.mixFloor[m];
            w.teacher  = exp.teacher[m];
            w.fromBook = exp.fromBook[m];
            if (m == MOD_WAR && exp.targetChosen >= 0) {
                w.targetCand = exp.targetCand;
                w.targetChosen = exp.targetChosen;
            }
            if (m == MOD_WAR && exp.attackChosen >= 0) {
                w.attackCand = exp.attackCand;
                w.attackChosen = exp.attackChosen;
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
            // THE STATE THE ANSWER WAS GIVEN IN, request included. See
            // Experience::diploFeatures. `exp.features` is the country's own
            // turn state, which leaves every request slot at zero -- training
            // on it showed the head a world where nobody had asked anything.
            //
            // Falls back to exp.features only for windows recorded before this
            // existed, which cannot happen within a process but keeps the
            // handling total rather than relying on that.
            const bool haveDiploFeats =
                (int)exp.diploFeatures.size() == FEATURE_COUNT;
            w.features = haveDiploFeats ? exp.diploFeatures : exp.features;
            w.relCand = haveDiploFeats ? exp.diploRelCand : exp.relCand;
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
        int refusedNow = 0;
        auto orIt = m_overturesRefusedThisTurn.find(cid);
        if (orIt != m_overturesRefusedThisTurn.end()) refusedNow = orIt->second;
        long long drownNow = 0, lostNow = 0;
        {
            auto dIt = m_crewDrownedThisTurn.find(cid);
            if (dIt != m_crewDrownedThisTurn.end()) drownNow = dIt->second;
            auto cIt2 = m_crewLostThisTurn.find(cid);
            if (cIt2 != m_crewLostThisTurn.end()) lostNow = cIt2->second;
        }
        int boughtNow = 0, soldNow = 0;
        {
            auto bIt = m_shipsBoughtThisTurn.find(cid);
            if (bIt != m_shipsBoughtThisTurn.end()) boughtNow = bIt->second;
            auto sIt = m_shipsScrappedThisTurn.find(cid);
            if (sIt != m_shipsScrappedThisTurn.end()) soldNow = sIt->second;
        }
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
            exp.overturesRefused += refusedNow;
            exp.crewDrowned += drownNow;
            exp.crewLost += lostNow;
            exp.shipsBought += boughtNow;
            exp.shipsSold += soldNow;
            exp.bankruptTurns += brokeNow;
            exp.warInWindow = exp.warInWindow || atWarNow;
        }

        float dNetNow = 0.0f;
        float polUpkeepNow = 0.0f;
        if (!dead) {
            const CountryIncomeSnapshot ci = g.computeCountryIncome(cid);
            dNetNow = ci.net + ci.researchCost;
            polUpkeepNow = ci.policyCosts + ci.minorityCosts + ci.pacificationCost;
        }

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
                // CHANGE IN EARNING POWER, WITH RESEARCH SPENDING ADDED BACK.
                //
                // net = total - expenses, and expenses INCLUDES researchCost.
                // So cutting research raised net, and the economy module was
                // paid +1.2 x tanh(dNet/15) for doing it -- immediately,
                // reliably, every turn. Finishing a node pays +0.8 x
                // tanh(dResearch/2), slowly and only if it completes. That is
                // the same trap the war module was in when army growth was
                // rewarded unconditionally: a certain small gain against an
                // uncertain larger one, and a policy gradient takes the certain
                // one every time. Measured: research fell to a fifth (446 nodes
                // to 107) while everything else improved.
                //
                // Adding researchCost back makes funding research NEUTRAL for
                // this term rather than negative. It is not made free -- the
                // money still leaves the treasury, so dTre and the bankruptcy
                // charge still price it. What changes is that the module is no
                // longer paid a bonus for refusing to invest.
                //
                // Pacification spending has the same shape and is deliberately
                // left alone: it is ongoing upkeep rather than an investment
                // with a delayed payoff, and one change at a time is the only
                // way the next A/B stays readable.
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
                // RAISING TROOPS IS NOT DOING SOMETHING WITH THEM.
                //
                // This used to exempt any country whose army moved by 500 men,
                // which is a loophole the size of the whole module: recruit,
                // never fight, never pay the idleness charge. The model found
                // it and settled there -- zero wars declared per thousand
                // country-turns against a random control's 2.73, while
                // out-recruiting everyone and winning only the wars it was
                // dragged into.
                //
                // Mobilising still exempts you when there is something to
                // mobilise AGAINST: exp.threatened is the same test armyTerm
                // uses to decide whether troops are an asset or a standing
                // bill. At peace, unthreatened, gaining no ground, an army that
                // merely grows is a country doing nothing expensively.
                // ...AND THE ESCAPE WAS STILL HERE. The paragraph above says
                // this loophole was closed and describes exactly how; the
                // `dArmy` clause that IS the loophole was left in the condition
                // underneath it. `exp.threatened == 0` was meant to replace it,
                // not to join it. So a country at peace, unthreatened, taking
                // no ground still walked away from the idleness charge for the
                // price of five hundred recruits -- which, with armyTerm paying
                // for the same recruits on the way in, made "raise men and do
                // nothing" the best-paid thing the war module could do. Both
                // halves of the collapse were in these ten lines.
                // ONE test for a wasted window, whether or not there is a war
                // on. See IDLE_CHARGE for what the two separate tests this
                // replaces cost the project. A declaration buys one window of
                // grace and nothing after it.
                const bool graceOfWar = exp.warInWindow && exp.warTurns < nStep();
                // ...AND A WINDOW THAT ENDED A WAR IS NOT A WASTED ONE EITHER.
                //
                // This is what drove war:ceasefire to a policy shape of exactly
                // 0.0% in all three workers of the 2026-08-08 pool while the
                // model they were seeded from still offered 110 per thousand
                // country-turns. Making peace is, by definition, something you
                // do in a window where you took no ground -- so `dProv <= 0`
                // was true precisely when the ceasefire landed, and the -0.5
                // arrived on top of the +0.30..1.00 peaceTerm below. Ending a
                // war one is losing therefore netted -0.20, and offering a
                // ceasefire that got REFUSED netted a clean -0.5 against
                // attacking's +1.16 for two provinces. The module was being
                // charged for the one action the blunder checklist exists to
                // keep alive, and it read the arithmetic correctly.
                //
                // Same idea as the two exemptions above: this tests for a
                // WASTED window, and concluding a war is an outcome, not a
                // stall. Declared here rather than at its use below so the
                // charge can see it.
                const bool warEnded =
                    exp.atWar && !atWarNow && exp.warTurns >= nStep();
                const bool idle = dProv <= 0.0f && exp.threatened == 0 &&
                                  !graceOfWar && !warEnded;

                // ── Standing, not stock ── see STANDING_WEIGHT.
                // Read from m_world, which endTurn refreshes before settling
                // any window, so both ends of the comparison are real.
                const float nowRank  = m_world.rankOf(cid);
                const float nowOwn   = m_world.shareOf(now.provinces);
                const float nowRival = m_world.rivalShareFor(cid);
                const float dRank = nowRank - exp.worldRank;
                // The GAP to the strongest other country. Rises when we gain on
                // them and when they lose to anyone at all; falls when they
                // grow and we do not. That second half is the signal this whole
                // reward was missing.
                const float dLead = (nowOwn - nowRival)
                                  - (exp.ownShare - exp.rivalShare);

                // ── POTENTIAL-BASED SHAPING. See PHI_PROV and friends. ──
                //
                // Every growth term below is Phi(s') - Phi(s), so a round trip
                // through any state is worth exactly zero and the optimal
                // policy is unchanged by construction. The old form -- tanh of
                // a DELTA -- paid +0.203 for losing three provinces and taking
                // them back, and a policy that no longer collapses finds that.
                //
                // Signed log for the money terms: a treasury or an income can
                // be negative, and sgn(x)*log1p(|x|) is monotone across zero
                // rather than folding the two halves together.
                auto phiPos = [](double x) { return std::log1p(std::max(0.0, x)); };
                auto phiSgn = [](double x) {
                    return (x < 0 ? -1.0 : 1.0) * std::log1p(std::fabs(x));
                };
                const double treNow = (double)c->treasury;
                // Land, with the LOSING half optionally weighted. See s_lossAversion:
                // the term is symmetric as shipped, and every change that lifted
                // the rating on 2026-09-04 did so by selling the survival seats.
                float dLand = (float)(phiPos(now.provinces) - phiPos(exp.provinces));
                if (dLand < 0.0f) dLand *= s_lossAversion;
                float global = PHI_PROV     * dLand
                             + PHI_TREASURY * (float)(phiSgn(treNow)        - phiSgn(exp.treasury))
                             + PHI_NET      * (float)(phiSgn(dNetNow)       - phiSgn(exp.netIncome))
                             // NOT potentials, on purpose: costs a competent
                             // player does not pay, rather than proxies for
                             // winning. See the note on PHI_PROV.
                             - UNREST_WEIGHT * std::tanh(rebels / 2.0f)
                             - 0.5f * broke
                             // Already differences of state functions; only the
                             // tanh-of-a-delta wrapper had to go.
                             + STANDING_WEIGHT * (dRank / STANDING_SCALE)
                             + LEAD_WEIGHT * (dLead / LEAD_SCALE);
                // Each module is now judged mostly on what it actually controls.
                rewards[MOD_ECONOMY]  = global
                                      // ...and the economy module's failure in
                                      // particular. Growing income is what it is
                                      // paid for; an empty treasury is what
                                      // happens when it never stops spending.
                                      - 1.2f * broke
                                      // Same conversion as `global`: these are
                                      // the economy module's own growth terms
                                      // and had the same cycle in them.
                                      + 2.2f * (float)(phiSgn(dNetNow) - phiSgn(exp.netIncome))
                                      + PHI_INDUSTRY * (float)(phiPos(now.industrySum) - phiPos(exp.industrySum))
                                      + PHI_RESEARCH * (float)(phiPos(researchedNow) - phiPos(exp.researched))
                                      + 0.5f * (float)(phiSgn(treNow) - phiSgn(exp.treasury))
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
                                      // PACTS, RAISED FROM 0.4. Measured: the
                                      // model proposed 7.50 per thousand
                                      // country-turns against a random
                                      // control's 49.82 -- it had decided
                                      // friends were not worth the overture
                                      // budget, and it was reading the reward
                                      // correctly. An ally is what makes a call
                                      // to arms possible at all, and with
                                      // almost none the diplomacy head was
                                      // never asked a question in a whole game.
                                      + 1.0f * std::tanh((float)now.pacts / 3.0f)
                                      // ...and what the asking cost. Agreements
                                      // held are paid for above; overtures that
                                      // came back refused were free, so the
                                      // module proposed constantly. See
                                      // AI_OVERTURE_REFUSED_CHARGE.
                                      - AI_OVERTURE_REFUSED_CHARGE *
                                        std::tanh((float)exp.overturesRefused / 2.0f)
                                      // Alignment is already a level in [0,100];
                                      // the difference of its potential is the
                                      // honest form and cannot be farmed by
                                      // driving it down and back up.
                                      + 0.5f * (float)(phiSgn(now.meanAlignment) -
                                                       phiSgn(now.meanAlignment - dAlign))
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
                                      // THE UPKEEP CHARGE THAT USED TO BE HERE
                                      // IS REVERTED, and the measurement is why.
                                      //
                                      // It charged politics -0.8 x tanh(rise in
                                      // doctrine + minority + pacification
                                      // upkeep) and raised its share of `broke`
                                      // to 0.7, to stop the module signing
                                      // recurring bills it could not carry.
                                      // Bankruptcy went from 57 turns per run to
                                      // 378 out of 400 -- ten times the random
                                      // control's 37.5 -- so whatever it did, it
                                      // was not that.
                                      //
                                      // Two candidate reasons it backfired, both
                                      // untested: charging only the INCREASE
                                      // makes standing at a ruinous level free,
                                      // so the cheapest policy is to spend to
                                      // the ceiling once and never move; and a
                                      // module punished for commitments while
                                      // still paid +1.0 x tanh(pacts/3) and
                                      // +0.6 x alignment has been handed a
                                      // contradiction rather than a price.
                                      //
                                      // Reverted rather than retuned: it was
                                      // added in the same run as the doctrine
                                      // levers and the estimator fixes, so its
                                      // effect was never measured alone, and
                                      // tuning a weight whose sign we cannot
                                      // establish is guessing with extra steps.
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
                // ...AND ONLY UNTIL IT IS ENOUGH. See ARMY_SUFFICIENCY: a
                // country permanently at war on the defensive satisfied the old
                // test every single turn, so "conditional" was unconditional in
                // practice and the module recruited on 98.5% of the turns it
                // could.
                // PAID FOR ARRIVING, NOT FOR MARCHING.
                //
                // The old shape paid 0.3 x tanh(dArmy) every window the gate was
                // open. That is an ANNUITY: recruiting is riskless, pays again
                // next turn, and the gate ("at war or threatened, and not yet at
                // twice the adjacent threat") is open almost permanently for a
                // country that is at war a lot -- which this one is, because it
                // never makes peace. Sufficiency was supposed to close the gate,
                // and it does, but only after the army is already enormous;
                // everything up to that point still paid per turn.
                //
                // Measured at argmax over four seeds before this change, the war
                // module chose recruit on 100.000% of the turns it was offered,
                // interval of zero width. Not a preference -- a constant
                // function. With the idleness escape above it, "raise men and do
                // nothing" both paid and dodged the charge for doing nothing.
                //
                // Progress toward sufficiency, clamped at 1, pays for CLOSING
                // the gap and pays exactly nothing once it is closed. The total
                // available over a whole game is bounded, so there is no trough
                // to settle in, and the module has to find its next reward
                // somewhere else -- which is the point.
                // THE SHORTFALL ITSELF, not the marching and not the arriving.
                // See ARMY_SHAPING_WEIGHT for the three shapes this replaces and
                // what each of them taught the policy to do instead.
                //
                // The bar is frozen at the window's start so a neighbour's
                // mobilisation cannot charge this country for a decision it did
                // not make; the army is measured at the end, so recruiting
                // during the window is credited within it.
                // PHI: how far this country's army goes toward handling what is
                // on its borders, as a pure function of the state. Both ends
                // use their OWN threat figure -- that is what makes it a
                // function of the state rather than of the window, and the
                // telescoping property depends on it. A neighbour's
                // mobilisation does lower PHI, and the next window's PHI(s)
                // starts equally low, so it cancels rather than accumulating.
                // THE BAR HAS A PEACETIME FLOOR, OR IT IS NOT A BAR AT ALL.
                //
                // max(1, adjacentThreat) meant that a country with no hostile
                // troops on its borders -- every country, most turns -- had a
                // denominator of ONE. PHI therefore saturated at an army of
                // two men, so holding a real standing army earned nothing while
                // costing upkeep and treasury every window. Recruit's only
                // surviving signal was its price.
                //
                // That is exactly what the overnight run of 2026-08-06 learned:
                // war:recruit collapsed to 0.0%, the model stopped building
                // armies at all, and it lost to the scripted rung outright
                // (ADVANTAGE 0.55-0.62) while being the most SOLVENT thing on
                // disk -- 0.63 bankrupt turns against the shipping model's 8.2.
                // It was rich because it did nothing.
                //
                // The bar scales with what there is to defend, so readiness is
                // worth buying before the enemy is already on the border --
                // which is when an army is actually needed and far too late to
                // start. It does NOT reopen the "recruit is free money" failure
                // that shape three died of, because PHI is still clamped at 1:
                // troops past sufficiency still pay exactly nothing.
                //
                // WORLD-RELATIVE, BECAUSE A CONSTANT CANNOT KNOW THE SCALE.
                //
                // The first attempt at this floor used a hand-set 200 troops
                // per province. Countries actually hold ~231,000 per province
                // in self-play -- 578x more -- so the floor never once bound,
                // PHI stayed pinned at 1, and the term remained exactly as dead
                // as before. The run that followed drove war:recruit to 0.0%
                // again and scored ADVANTAGE 0.655.
                //
                // The absolute size of an army is a property of the economy,
                // which changes whenever the economy does -- as it did today
                // when industry started working. Expressing the bar as a
                // fraction of the world's mean garrison density means it tracks
                // that automatically and cannot fall out of range again.
                const double worldPerProv = worldArmyPerProvince();
                // ONE YARDSTICK PER WINDOW, READ AT THE START OF IT.
                //
                // The bar is a multiple of our OWN province count, so taking
                // ground raises it -- and PHI, being army over the bar, falls
                // the moment a province is conquered even though not one man
                // was lost. Measuring both ends of the window against their own
                // bar therefore paid the module to recruit back to a line that
                // its own conquests kept moving away from it: a renewable
                // payment, available only to a country that is winning, and
                // exactly the "recruit is free money" failure the comment on
                // ARMY_SHAPING_WEIGHT says PHI's clamp had closed for good.
                //
                // Measured on the 2026-08-08 pool: w2 -- the ONLY worker that
                // beat its seed, and the one that conquered most (62% of the
                // world) -- was also the only one whose policy shape for
                // recruit collapsed to 100.0%. w0 and w1 took far less ground
                // and sat at 33-48%. The collapse tracked conquest, not
                // training time.
                //
                // The clamp cannot stop this on its own: it bounds what a
                // STATIONARY country can collect, and this country is not
                // stationary. Holding the bar fixed at the window's opening
                // state is what makes the term measure the only thing it was
                // ever meant to measure -- whether recruiting closed the gap
                // that existed when the decision was taken. Between windows the
                // bar still tracks the country as it grows, which is the part
                // that was always legitimate.
                const double bar = std::max((double)exp.enemyAdjArmy,
                    PEACETIME_PARITY * worldPerProv * (double)std::max(1, exp.provinces));
                const double need = ARMY_SUFFICIENCY * std::max(1.0, bar);
                auto sufficiency = [&](long long army) {
                    return (float)std::min(1.0, (double)army / need);
                };
                const float phiBefore = sufficiency(exp.army);
                const float phiAfter  = sufficiency(now.army);
                // Potential-based shaping. See ARMY_SHAPING_WEIGHT for why this
                // shape and not the three that preceded it.
                const float armyTerm = ARMY_SHAPING_WEIGHT * (phiAfter - phiBefore);

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
                // warEnded is computed with the idleness charge above, which
                // now has to know about it.
                //
                // Scaled by ground, but FLOORED. The old shape paid
                // 0.5 x (1 + tanh(dProv/3)), so ending a war one is losing --
                // exactly when peace is the right move and the most human thing
                // an AI can do -- paid least of all. A country bleeding
                // provinces should be pulled towards the exit, not away from
                // it. Winning still pays more; losing now pays something.
                const float peaceTerm =
                    warEnded ? WAR_END_REWARD *
                                   std::max(0.6f, 1.0f + std::tanh(dProv / 3.0f))
                             : 0.0f;

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
                                      // POTENTIAL, like `global`. This was
                                      // tanh(dProv/3) -- and the war module is
                                      // where that mattered most, because it is
                                      // the module that can lose ground and
                                      // retake it. Losing three provinces and
                                      // taking them back paid +0.203 here on top
                                      // of the same bug in `global`, doubled by
                                      // the 2.0 weight. See PHI_PROV.
                                      + 2.0f * (float)(phiPos(now.provinces) - phiPos(exp.provinces))
                                      - 2.0f * std::tanh(std::max(0.0f, dLost) / 2.0f)
                                      + armyTerm
                                      // A wasted window costs the same whether
                                      // it was wasted at peace or wasted in a
                                      // war nobody is fighting. See IDLE_CHARGE.
                                      + (idle ? IDLE_CHARGE : 0.0f)
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
                // THE FLEET, CHARGED TO WHOEVER MADE THE DECISION.
                //
                // This used to be one term on dShips -- the fleet's NET change
                // -- sitting entirely in the navy's reward. But the ECONOMY
                // module buys ships (execEconomy cases 5 and 6, out of its own
                // treasury) and the NAVY module scraps them, so a single net
                // figure credited each for what the other did: a navy that paid
                // off an idle hull was charged for the fall, and an economy that
                // bought a fleet it could not use was never told. It is the same
                // misattribution the solvency terms already had to be split for.
                //
                // Now each module answers for its own decision. Buying is good
                // when the fleet has work and wasteful when it has none; paying
                // off a hull with nothing to do is the right call and is paid
                // for as one.
                rewards[MOD_ECONOMY] += fleetUseful
                                      ?  0.4f * std::tanh((float)exp.shipsBought / 2.0f)
                                      : -0.5f * std::tanh((float)exp.shipsBought / 2.0f);
                rewards[MOD_NAVY]     = global
                                      + (fleetUseful ? 0.0f
                                                     : 0.4f * std::tanh((float)exp.shipsSold / 2.0f))
                                      // Same conversion as the war head's. See PHI_PROV.
                                      + 0.8f * (float)(phiPos(now.provinces) - phiPos(exp.provinces))
                                      // Troops actually put ashore on a hostile
                                      // coast. The ground a landing wins often
                                      // falls outside the twelve-turn window,
                                      // so without this the module is paid for
                                      // the invasion only when it happens to
                                      // conclude quickly — and charged for the
                                      // army and the hulls every other time.
                                      + 1.0f * std::tanh((float)exp.landings / 1.5f)
                                      // ── What happens at sea ──
                                      //
                                      // Sinking a loaded transport kills the
                                      // invasion it carries; losing one deletes
                                      // those men from our own land army. Both
                                      // were worth nothing here, so the engage
                                      // action had a capability and no reason
                                      // to use it.
                                      //
                                      // Crew, not hulls, and deliberately only
                                      // these two terms. Warships sunk and lost
                                      // are left out on purpose: they matter
                                      // only instrumentally, this reward is one
                                      // of the few not yet spoiled by
                                      // over-shaping, and armyTerm took five
                                      // shapes and failed all five. Add more
                                      // only if the data asks.
                                      + 1.2f * std::tanh((float)exp.crewDrowned / 400.0f)
                                      - 1.2f * std::tanh((float)exp.crewLost / 400.0f);

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
                // ── AND WHAT WINNING IS WORTH ──
                //
                // The mirror of dLost, and it was missing. Ground lost had a
                // dedicated term at 1.2 -- the largest in this reward -- while
                // ground GAINED reached it only through `global`, as
                // PHI_PROV * (log1p(now) - log1p(then)). A log difference at
                // 2.4 is almost nothing at any realistic size, so the two
                // outcomes of fighting on were priced 5.3 to 1 against each
                // other. Measured over one window at 20 provinces: gaining two
                // paid +0.218, losing two cost -1.154, and refusing a ceasefire
                // AND WINNING came out NEGATIVE (-0.010) against a flat 0.000
                // for accepting. Accepting therefore dominated every outcome of
                // fighting on, including the good one -- and the head duly
                // accepted 82 ceasefires out of 82.
                //
                // That is the same failure as the one recorded at
                // AI_CALL_RELUCTANCE, where an 18:1 asymmetry taught the policy
                // to refuse every call to arms: the reward working as written
                // rather than the model failing.
                //
                // Gated exactly as dLost is, on `exp.threatened > 0`, so the
                // two are the same measurement with opposite signs: ground that
                // changed hands while somebody was on our borders. Ground
                // acquired at peace is not diplomacy's doing and is not
                // credited here. Same weight and same tanh shape, so a province
                // is worth what it costs -- which is the property that was
                // missing, not the size of either number.
                const float dGained = (float)(exp.threatened > 0
                                                  ? now.provinces - exp.provinces : 0);
                diploReward = global
                            // Scaled together: both are "an agreement is worth
                            // holding", and journal 08 found the pair
                            // collectively outbid 4.7 to 1 by the gain term.
                            + s_diploPactWeight * 0.6f * std::tanh((float)now.coBelligerents / 2.0f)
                            + s_diploPactWeight * 0.6f * std::tanh((float)now.pacts / 3.0f)
                            - 0.8f * std::tanh(pactsLost)
                            - 0.6f * std::tanh(std::max(0.0f, dWeary) / 5.0f)
                            - 1.2f * std::tanh(std::max(0.0f, dLost) / 2.0f)
                            + 1.2f * std::tanh(std::max(0.0f, dGained) / 2.0f);
            }
            // See Experience::ceasefireCredit -- the one per-decision term
            // on an answer, credited to the window the answer was given in.
            diploReward += exp.ceasefireCredit;
            // tanh(NaN) is still NaN: overflowed treasuries/incomes must not
            // poison the weight update (a single NaN reward corrupts the net
            // permanently, including the model file saved to disk).
            for (int m = 0; m < MOD_COUNT; ++m)
                if (!std::isfinite(rewards[m])) rewards[m] = 0.0f;
            // THE DEAL ITSELF, credited to the decision that made it.
            //
            // Everything above is how the country is doing in general, which is
            // the signal a single trade drowns in. 0.5 puts a good deal at an
            // eighth of winning the map (+4.0): enough to be felt, not enough to
            // be chased instead of the win.
            {
                auto toIt = m_tradeOutcome.find(cid);
                if (toIt != m_tradeOutcome.end()) {
                    diploReward += std::tanh(toIt->second / 500.0f) * 0.5f;
                    m_tradeOutcome.erase(toIt);
                }
            }
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
    // The trust region scales the POLICY step only. Value and Q are
    // regressions onto targets, not distributions that can collapse; shrinking
    // them because the policy overshot would just slow the baseline down
    // exactly when the policy most needs an accurate one.
    const float pol = LR_POLICY * lrScale() * m_klStepScale;
    for (int m = 0; m < MOD_COUNT; ++m) {
        m_policy[m].flushBatch(pol);
        m_value[m].flushBatch(LR_VALUE * lrScale());
        m_q[m].flushBatch(LR_Q * lrScale());
    }
    // The trunk takes the policy learning rate: it is trained by the same
    // gradients, from four policy heads, four Q heads and diplomacy at once.
    m_trunk.flushBatch(pol);
    m_stanceHead.flushBatch(LR_POLICY * lrScale());
    m_relEncoder.flushBatch(LR_POLICY * lrScale());
    m_relScore.flushBatch(LR_POLICY * lrScale());
    m_target.flushBatch(LR_TARGET);
    m_attack.flushBatch(LR_TARGET);
    m_diploValue.flushBatch(LR_VALUE * lrScale());
    // The forward model is a plain regression, so it takes the value learning
    // rate rather than the policy one -- it is fitting a target, not shifting a
    // distribution, and nothing about it answers to the KL trust region.
    m_dynamics.flushBatch(LR_VALUE * lrScale());
    m_diplo.flushBatch(LR_DIPLO);

    // How far that step actually moved the policy, and therefore how big the
    // next one may be. See AISystem::KLProbe.
    measureStepAndSetScale();

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


double g_relSum[AISystem::REL_FEATURES] = {};
long long g_relN = 0;
namespace {
// Every relational feature's mean, for choosing an ablation constant that is
// the feature's own average rather than a value it never takes.
struct RelReport {
    ~RelReport() {
        if (!g_relN) return;
        printf("[REL-MEAN] over %lld reads:", g_relN);
        for (int k = 0; k < AISystem::REL_FEATURES; ++k)
            printf(" r%d=%.4f", k, g_relSum[k] / (double)g_relN);
        printf("\n");
    }
} g_relReport;
}

double g_r3Sum = 0.0;
long long g_r3N = 0;
namespace {
// Reports the mean of relational feature 3 at exit, under
// OD_AI_REL_TREASURY_STATS -- see the note in buildRelCandidates.
struct R3Report {
    ~R3Report() {
        if (g_r3N) printf("[R3] mean %.4f over %lld reads\n", g_r3Sum / (double)g_r3N, g_r3N);
    }
} g_r3Report;
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

float AISystem::bcSampleWeight(int module, int teacherAction, bool fromBook) const {
    if (module < 0 || module >= MOD_COUNT) return 0.0f;
    if (teacherAction < 0 || teacherAction >= MAX_MODULE_ACTIONS) return 0.0f;

    float base;
    if (fromBook) {
        // The book's own move. Its sample carries no policy gradient, so this
        // is the only thing it can teach; it is therefore not conditional on
        // cloning being switched on, and not annealed -- the book already stops
        // at AI_OPENING_TURNS.
        base = BOOK_CLONE_WEIGHT;
    } else {
        if (s_bcWeight <= 0.0f) return 0.0f;
        // ── ANNEAL ──  A warm start, not a permanent second opinion. The
        // teacher is a fixed script with a fixed ceiling; left running at full
        // weight it spends the whole run arguing with the reward for the right
        // to hold the policy down at its own level.
        const double frac = (double)m_learnBatches / (double)BC_ANNEAL_BATCHES;
        const float anneal = (float)std::max(0.0, 1.0 - frac);
        if (anneal <= 0.0f) return 0.0f;
        base = s_bcWeight * anneal;
    }

    // ── CLASS BALANCE ──  OFF: BC_BALANCE_POWER is zero, and the constant
    // carries the measurement that turned it off. The counters below are still
    // maintained so the term can be switched back on and measured, but at
    // power 0 this whole block resolves to a balance of exactly 1.
    float balance = 1.0f;
    const double total = (BC_BALANCE_POWER > 0.0f) ? m_teacherTotal[module] : 0.0;
    if (total > 0.0) {
        const double share = m_teacherCount[module][teacherAction] / total;
        if (share > 1e-6) {
            // Normalised so an action at its module's uniform share weighs 1:
            // the average sample keeps the weight the caller asked for, and
            // only the imbalance is corrected.
            const double uniform = 1.0 / (double)MAX_MODULE_ACTIONS;
            balance = (float)std::pow(uniform / share, (double)BC_BALANCE_POWER);
            // ── THE UPPER BOUND IS 2, AND IT WAS 4 ──
            //
            // At 4 the clone could weigh 0.30 x 4 = 1.2, which is larger than
            // a typical clamped advantage, so on the rare states where it
            // applied the teacher outvoted the reward. Measured on a 527-map
            // run: the single worst batch of the run was simultaneously its
            // largest clone weight (1.194 against a median of 0.124) and its
            // largest optimiser step (KL 0.2243 against a median of 0.0120,
            // eighteen times over, pinned at the trust region's floor), and it
            // was the batch on which the econ head's marginal fell to 0.164
            // against a floor of 0.373.
            //
            // That is one sample and therefore a correlation, not a proof. But
            // the ceiling on that line was 2.485 = log(12), meaning every one
            // of the econ head's twelve actions was on offer -- which is
            // exactly the state that maximises the balance multiplier, so the
            // mechanism is at least the one the arithmetic predicts. Halving
            // the bound caps the clone at 0.60, below the advantage scale it
            // is meant to inform rather than overrule.
            //
            // The lower bound is left at 0.25: down-weighting an action the
            // teacher takes constantly costs nothing and is not what produced
            // the outlier.
            balance = std::clamp(balance, 0.25f, 2.0f);
        }
    }
    return base * balance;
}

float AISystem::applyStabilityGuards() {
    ++m_learnBatches;

    double klWorst = 0.0;
    for (int m = 0; m < MOD_COUNT; ++m) {
        BatchHead& b = m_batchHead[m];
        if (b.n <= 0) { m_headEntropy[m] = 0.0f; m_headCeiling[m] = 0.0f; continue; }
        const double H       = b.entropySum / (double)b.n;
        const double ceiling = b.ceilingSum / (double)b.n;
        const double kl      = b.klSum / (double)b.n;
        m_headEntropy[m] = (float)H;
        m_headCeiling[m] = (float)ceiling;
        m_headKl[m]      = (float)kl;
        klWorst = std::max(klWorst, kl);

        b = BatchHead{};
    }

    // ── THE COLLAPSE GUARD ──
    //
    // Reads the MARGINAL over chosen actions, not per-state entropy: see
    // ENTROPY_FLOOR_FRAC for the measurement that ruled the latter out. A head
    // that answers the same thing on every turn of every country has a
    // marginal entropy of zero however confident each individual decision was,
    // and that -- not confidence -- is what destroyed the last run.
    //
    // PROPORTIONAL, not a creeping multiplier. The multiplicative version was
    // written first and measured: raising the coefficient 6% a batch takes ~50
    // batches to get from 0.01 to 0.15, and a head under a strong constant push
    // saturates in fewer than that, so the guard arrived after the collapse
    // every time. Responding to the SIZE of the deficit in one batch is the
    // difference between a guard and a witness.
    //
    // The deficit drives two things: the entropy coefficient, and the uniform
    // pull in runLearningWork that does the actual climbing out. See
    // UNIFORM_PULL_K for why the entropy bonus alone cannot.
    for (int m = 0; m < GUARD_HEADS; ++m) {
        double offeredTotal = 0.0, chosenTotal = 0.0;
        for (int a = 0; a < MAX_MODULE_ACTIONS; ++a) {
            m_marginalOffered[m][a] *= MARGINAL_DECAY;
            m_marginalChosen[m][a]  *= MARGINAL_DECAY;
            offeredTotal += m_marginalOffered[m][a];
            chosenTotal  += m_marginalChosen[m][a];
        }
        if (offeredTotal <= 0.0 || chosenTotal <= 0.0) { m_headDeficit[m] = 0.0f; continue; }

        // The ceiling counts only actions the head is REGULARLY offered. A war
        // module that is landlocked and at peace is offered two actions, and
        // taking one of them consistently is not a collapse.
        //
        // ── PER DECISION, NOT PER OFFER-SLOT ──
        //
        // This divided by offeredTotal, which is the sum of the offer counts of
        // ALL actions -- so the question "is this action offered often enough
        // to judge the head on" had an answer that depended on how many OTHER
        // actions there were and how often THEY were offered. Two always-valid
        // actions are enough to dilute everything else below the floor.
        //
        // It did exactly that to the economy head, which is the one head that
        // needs watching: it has twelve actions, `save` and `fund down` are
        // valid on nearly every turn, and every purchase is valid only when it
        // is affordable. The guard's ceiling came out at log(3) -- it was
        // watching THREE of twelve actions, scoring the head 0.793 against
        // 1.099 and reporting it healthy, while fort, port, specialize,
        // destroyer and carrier all sat at exactly 0.0 marginal probability and
        // the head had not built a ship or a fort in two hundred turns.
        //
        // Action 0 is valid unconditionally in all four modules (save, hold,
        // hold, hold), so its offer count IS the number of decisions the module
        // made, and that is the honest denominator: "offered on at least 5% of
        // the turns this module acted".
        const double decisions = m_marginalOffered[m][0];
        if (decisions <= 0.0) { m_headDeficit[m] = 0.0f; m_marginalH[m] = 0.0f; continue; }
        int live = 0;
        for (int a = 0; a < MAX_MODULE_ACTIONS; ++a)
            if (m_marginalOffered[m][a] / decisions >= MARGINAL_OFFER_FLOOR) ++live;
        if (live < 2) { m_headDeficit[m] = 0.0f; m_marginalH[m] = 0.0f; continue; }

        double H = 0.0;
        for (int a = 0; a < MAX_MODULE_ACTIONS; ++a) {
            const double p = m_marginalChosen[m][a] / chosenTotal;
            if (p > 1e-9) H -= p * std::log(p);
        }
        const double ceiling = std::log((double)live);
        const double floorH  = ENTROPY_FLOOR_FRAC * ceiling;
        const double target  = floorH * ENTROPY_GUARD_TARGET_MUL;
        const double deficit = (target > 1e-9)
                             ? std::clamp((target - H) / target, 0.0, 1.0) : 0.0;
        m_marginalH[m]       = (float)H;
        m_marginalCeiling[m] = (float)ceiling;
        m_headDeficit[m]     = (float)deficit;
        // Base is per-head now (entropyFor), so a head can be given a
        // different starting coefficient without touching the others -- the
        // war head's 0.01 was earned by measurement and must not move.
        const float base = entropyFor(m);
        m_entropyCoef[m]     = std::clamp(
            (float)(base + deficit * deficit), base, ENTROPY_COEF_MAX);
        if (H < floorH) ++m_collapseBatches[m];
    }

    // klWorst is REPORTED, not acted on. It is KL(behaviour || policy), which
    // is dominated by the off-policy staleness this design has on purpose --
    // see KLProbe for why using it as a trust region was a learning-rate cut
    // wearing a trust region's name. The step scale comes from
    // measureStepAndSetScale, which measures the step itself.
    (void)klWorst;
    const float scale = m_klStepScale;

    // Reported on a schedule whether or not tracing is on. A guard nobody can
    // see is indistinguishable from a guard that is not running, and the whole
    // reason this exists is that the last collapse was only visible in a bench
    // after fourteen hours of training had already been spent on it.
    if (m_learnBatches % GUARD_LOG_BATCHES == 0) {
        static const char* MN[MOD_COUNT] = {"econ", "politics", "war", "navy"};
        for (int m = 0; m < MOD_COUNT; ++m) {
            printf("[GUARD] %-8s marginal %.3f/%.3f floor %.3f  perState %.3f  "
                   "ent %.3f  step %.4f x%.2f  bc %.3f  under %lld%s\n",
                   MN[m], m_marginalH[m], m_marginalCeiling[m],
                   ENTROPY_FLOOR_FRAC * m_marginalCeiling[m], m_headEntropy[m],
                   m_entropyCoef[m], m_stepKl, scale,
                   bcSampleWeight(m, 0, false), m_collapseBatches[m],
                   // Below the FLOOR is a collapse. Between the floor and the
                   // guard's target the pull is ramping in gently and saying
                   // so would cry wolf on every ordinary run.
                   (m_marginalH[m] < ENTROPY_FLOOR_FRAC * m_marginalCeiling[m])
                       ? "  <-- COLLAPSING"
                       : (m_headDeficit[m] > 0.0f ? "  (guard ramping)" : ""));
        }
        // The diplomacy head, one line per kind it was actually asked about.
        for (int k = 0; k < OFFER_KINDS; ++k) {
            const int m = MOD_COUNT + k;
            if (m_marginalOffered[m][0] <= 0.0) continue;
            printf("[GUARD] diplo/%-14s marginal %.3f/%.3f floor %.3f  ent %.3f  under %lld%s\n",
                   offerKindName(k), m_marginalH[m], m_marginalCeiling[m],
                   ENTROPY_FLOOR_FRAC * m_marginalCeiling[m], m_entropyCoef[m],
                   m_collapseBatches[m],
                   (m_marginalH[m] < ENTROPY_FLOOR_FRAC * m_marginalCeiling[m])
                       ? "  <-- COLLAPSING"
                       : (m_headDeficit[m] > 0.0f ? "  (guard ramping)" : ""));
        }
        fflush(stdout);
    }
    return scale;
}

void AISystem::captureKLProbe() {
    m_klProbe.clear();
    if (m_work.empty()) return;
    // An even spread through the batch rather than the first N: work items are
    // appended country by country, so the head of the list is one country's
    // whole turn and would measure that country's decisions only.
    const size_t stride = std::max<size_t>(1, m_work.size() / (size_t)KL_PROBE_N);
    if (!m_probeReady) {
        m_trunk.initScratch(m_probeTrunk);
        // Every module head has the same shape of scratch; the widest is safe
        // for all of them because forwardInto resizes to the net it is given.
        m_policy[0].initScratch(m_probePolicy);
        m_probeReady = true;
    }
    for (size_t i = 0; i < m_work.size() && (int)m_klProbe.size() < KL_PROBE_N; i += stride) {
        const WorkItem& w = m_work[i];
        if (w.module < 0 || w.module >= MOD_COUNT || w.features.empty()) continue;
        KLProbe p;
        p.module = w.module;
        p.features = w.features;
        p.validMask = w.validMask;
        m_trunk.forwardInto(m_probeTrunk, p.features);
        m_policy[w.module].forwardInto(m_probePolicy, m_probeTrunk.acts.back());
        p.before = maskedProbs(m_probePolicy.acts.back(), p.validMask);
        m_klProbe.push_back(std::move(p));
    }
}

std::vector<float> AISystem::maskedProbs(const std::vector<float>& logits,
                                         const std::vector<uint8_t>& mask) {
    std::vector<float> ml(logits);
    if (!mask.empty())
        for (size_t i = 0; i < ml.size(); ++i)
            if (i >= mask.size() || !mask[i]) ml[i] = -1e9f;
    std::vector<float> p;
    NeuralNet::softmax(ml, 1.0f, p);
    return p;
}

void AISystem::measureStepAndSetScale() {
    if (m_klProbe.empty()) { m_klStepScale = 1.0f; return; }
    double sum = 0.0;
    int n = 0;
    for (const KLProbe& p : m_klProbe) {
        m_trunk.forwardInto(m_probeTrunk, p.features);
        m_policy[p.module].forwardInto(m_probePolicy, m_probeTrunk.acts.back());
        const std::vector<float> after = maskedProbs(m_probePolicy.acts.back(), p.validMask);
        if (after.size() != p.before.size()) continue;
        // KL(before || after): how much of what the policy used to believe the
        // step has thrown away. Asymmetric on purpose and in this direction --
        // a step that deletes an action the policy was relying on is the
        // expensive one, and this is the direction that charges for it.
        double kl = 0.0;
        for (size_t i = 0; i < after.size(); ++i) {
            const double a = p.before[i];
            if (a <= 1e-8) continue;
            kl += a * std::log(a / std::max(1e-8, (double)after[i]));
        }
        sum += std::max(0.0, kl);
        ++n;
    }
    m_stepKl = n ? (float)(sum / (double)n) : 0.0f;
    // Below the target the step is unconstrained. Above it, shrink in
    // proportion to the overshoot, with a floor so a single wild batch cannot
    // stall learning outright.
    m_klStepScale = (m_stepKl > PPO_KL_TARGET)
                  ? (float)std::clamp((double)PPO_KL_TARGET / m_stepKl, 0.10, 1.0)
                  : 1.0f;
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
            if (w.module == MOD_COUNT + 2) {
                // ── THE FORWARD MODEL: a regression, not a policy ──
                //
                // Target is the trunk's OWN embedding of the state that
                // actually followed, taken with a stop-gradient: nothing here
                // backpropagates into the trunk. A trunk trained to be
                // predictable would learn to discard whatever is hard to
                // predict, and what is hard to predict is what planning is for.
                if (w.features.empty() || w.nextFeatures.empty()) continue;
                m_trunk.forwardInto(ws.trunkNext, w.nextFeatures);
                const std::vector<float> target = ws.trunkNext.acts.back();
                m_trunk.forwardInto(ws.trunk, w.features);
                const std::vector<float> in =
                    dynamicsInput(ws.trunk.acts.back(), w.dynModule, w.action);
                const std::vector<float>& pred = m_dynamics.forwardInto(ws.dynamics, in);
                if (pred.size() != target.size()) continue;
                // d/dpred of 1/2 * ||pred - target||^2.
                std::vector<float> g(pred.size());
                for (size_t k = 0; k < pred.size(); ++k) g[k] = pred[k] - target[k];
                m_dynamics.accumulateVectorGradInto(ws.dynamics, g);
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
                // THE SAME MASK THE ANSWER WAS CHOSEN UNDER. decideDiplomacy
                // softmaxes over one kind's pair, so an update that softmaxed
                // over all fourteen would measure the PPO ratio against a
                // distribution nothing ever sampled from, and every diplomatic
                // sample would look off-policy. The kind is read back out of
                // the recorded state rather than stored twice; see
                // offerKindFromFeatures.
                const int dKind = offerKindFromFeatures(w.features);
                // INVARIANT: the recorded action must lie inside the pair the
                // recovered kind selects. If offerKindFromFeatures ever
                // disagreed with the offerKindOf that chose the action, PPO
                // would update rows the behaviour policy never sampled and the
                // ratio would be measured against a distribution that did not
                // exist -- with no visible symptom at all, which is why this is
                // counted rather than trusted.
                if (w.action < dKind * DIPLO_ACTIONS ||
                    w.action >= (dKind + 1) * DIPLO_ACTIONS) {
                    static int warned = 0;
                    if (warned++ < 3)
                        fprintf(stderr, "[AI] BUG: diplo action %d outside kind "
                                        "%d's pair -- offerKindFromFeatures "
                                        "disagrees with offerKindOf\n",
                                w.action, dKind);
                    continue;
                }
                std::vector<uint8_t> dMask(DIPLO_OUTPUTS, 0);
                dMask[dKind * DIPLO_ACTIONS + 0] = 1;
                dMask[dKind * DIPLO_ACTIONS + 1] = 1;
                // THE COLLAPSE GUARD COVERS THIS HEAD PER KIND. Marginals are
                // recorded in the ANSWER space (reject/accept), so `live` is 2
                // and the ceiling is ln2 = 0.693 -- the same quantity the eval
                // reports -- and each kind has its own slot and coefficient,
                // because a head-wide mixture hid a dead non-aggression kind
                // behind a lively alliance one (journal 34c).
                m_diplo.accumulatePPOInto(ws.diplo, w.action, dAdv,
                                          w.oldLogProb, PPO_CLIP,
                                          m_entropyCoef[MOD_COUNT + dKind], &dMask);
                // ── THE UNIFORM PULL, per kind ──
                //
                // The module heads have had this actuator since the guard was
                // built; this path only ever had the coefficient, and with the
                // head answering at p ~ 1e-4 the entropy bonus's gradient is
                // nothing -- N2 sat at marginal 0.000 on every kind with the
                // coefficient pinned at its ceiling for 200 batches (journal
                // 35a). Cross-entropy toward both legal answers of THIS kind,
                // gradient (p - 1/2), only while this kind is under its floor.
                if (m_headDeficit[MOD_COUNT + dKind] > 0.0f) {
                    const float pull = UNIFORM_PULL_K * m_headDeficit[MOD_COUNT + dKind] / 2.0f;
                    for (int a = 0; a < DIPLO_OUTPUTS; ++a)
                        if (dMask[a])
                            m_diplo.accumulateCrossEntropyInto(ws.diplo, a, pull, &dMask);
                }
                // See TrainStats::diploAdvSum. Recorded against the cohort the
                // country belongs to, like every other counter, and under the
                // same lock-free convention as warAdvSum a few hundred lines
                // down: runLearningWork partitions by index, so two threads CAN
                // hit one slot and a few increments are lost. That is fine for
                // a diagnostic whose only question is whether yes and no carry
                // different advantages -- a gap of 1.5 does not become a gap of
                // 0 because a handful of samples went missing -- and it would
                // not be fine for anything used as a ledger.
                //
                // statsFor() reads m_randomCids, which nothing writes during
                // learning, so the cohort lookup itself is safe to share.
                {
                    TrainStats& ds = statsFor(w.cid);
                    const int ans = w.action - dKind * DIPLO_ACTIONS;
                    ds.diploAdvSum[dKind][ans] += dAdv;
                    ds.diploAdvN[dKind][ans]   += 1;
                }
                // ── HOW HARD DIPLOMACY MAY RESHAPE THE SHARED TRUNK ──
                //
                // The seven output pairs are independent; the embedding all
                // seven read is not. Measured on a fresh reset, CEASEFIRE is
                // 315 of 434 diplomatic samples -- 73% of everything this head
                // ever learns from -- so an unscaled gradient lets one question
                // reshape the representation the other six are answered from.
                // That is the mechanism behind the collapse in journal 05 and
                // 06: sensible per-kind answers early, uniform refusal later,
                // even with the readout fully separated.
                //
                // 1.0 is the old behaviour and the default, so this is inert
                // until a sweep asks for it. See s_diploTrunkGrad.
                {
                    // Per-kind first, then the global scale. The head's OWN
                    // pair still trains normally either way -- this touches
                    // only what diplomacy is allowed to write back into the
                    // representation the other six kinds are read from.
                    float scale = s_diploTrunkGrad;
                    if (dKind == OFFER_CEASEFIRE) scale *= s_diploCeasefireTrunkGrad;
                    std::vector<float> dg = NeuralNet::inputGrad(ws.diplo);
                    if (scale != 1.0f) for (float& g : dg) g *= scale;
                    m_trunk.accumulateVectorGradInto(ws.trunk, dg);
                }
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
            // See UpdTrace. Single-threaded path only (OD_UPDATE_TRACE forces
            // it) so these are exact rather than lock-free approximations.
            if (s_updTrace && m >= 0 && m < MOD_COUNT) {
                UpdTrace& t = m_upd[m];
                t.normSum += w.norm;      t.normSq += (double)w.norm * w.norm;
                t.baseSum += baseline;    t.baseSq += (double)baseline * baseline;
                t.advSum  += advantage;   t.advSq  += (double)advantage * advantage;
                t.n++;
                if (std::fabs(target - baseline) >= 3.0f) t.advClipped++;
                if (std::fabs(target) >= 6.0f) t.tgtClipped++;
            }
            // See TrainStats::warAdvSum. Accumulated under a lock-free
            // convention that is safe here only because runRange partitions
            // work by index and each item has one module and one action -- two
            // threads can hit the same slot, so this is a diagnostic accurate
            // to within a few lost increments, not a ledger.
            if (m == MOD_WAR && w.action >= 0 && w.action < WAR_ACTIONS) {
                m_trainStats.warAdvSum[w.action] += advantage;
                m_trainStats.warAdvN[w.action]++;
                // ...and the two halves it is made of. See warImmSum.
                m_trainStats.warImmSum[w.action]  += w.norm;
                m_trainStats.warBootSum[w.action] += (target - w.norm);
                m_trainStats.warBaseSum[w.action] += baseline;
            }
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
            // WHERE the war module pushed, scored by the same advantage as
            // whether to push at all. Identical arithmetic to the block above,
            // and identical reasoning: one forward pass per candidate, softmax
            // across the scores, each candidate given the one derivative that
            // belongs to it.
            if (w.attackChosen >= 0 && w.attackCand.size() > 1) {
                std::vector<float> scores(w.attackCand.size(), 0.0f);
                for (size_t i = 0; i < w.attackCand.size(); ++i) {
                    m_attack.forwardInto(ws.attack, w.attackCand[i]);
                    const auto& o = ws.attack.acts.back();
                    scores[i] = o.empty() || !std::isfinite(o[0]) ? 0.0f : o[0];
                }
                std::vector<float> probs;
                NeuralNet::softmax(scores, 1.0f, probs);
                for (size_t i = 0; i < w.attackCand.size(); ++i) {
                    const float gi = advantage *
                        (probs[i] - (i == (size_t)w.attackChosen ? 1.0f : 0.0f));
                    m_attack.forwardInto(ws.attack, w.attackCand[i]);
                    m_attack.accumulateOutputGradInto(ws.attack, gi);
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
            // The trunk looks redundant here -- it was forwarded on these same
            // features a few lines up for the Q head, and nothing since changes
            // its input or its weights. Removing it was TRIED and reverted on
            // 2026-08-27: it saved nothing measurable (19.7s vs 20.2s on a
            // 200-turn map, inside the noise) and could not be PROVEN safe,
            // because training is not reproducible run to run -- the same
            // binary on the same seed produces different model checksums even
            // single-threaded, since saves and peer syncs are driven by the
            // wall clock. An optimisation that buys nothing and cannot be
            // verified is not worth the risk of carrying.
            m_trunk.forwardInto(ws.trunk, w.features);
            m_policy[m].forwardInto(ws.policy[m], ws.trunk.acts.back());
            const std::vector<uint8_t>* mask =
                w.validMask.empty() ? nullptr : &w.validMask;
            // THE SURROGATE SKIPS BOOKED MOVES. The book is deterministic, so
            // there is no behaviour distribution to measure a ratio against;
            // see Experience::fromBook. Everything else about the sample --
            // its value target, its Q target, and the demonstration below --
            // is still used, which is why it is skipped here rather than
            // dropped when the work item was built.
            NeuralNet::PPOStats ps;
            if (!w.fromBook) {
                m_policy[m].accumulatePPOInto(ws.policy[m], w.action, advantage,
                                              w.oldLogProb, PPO_CLIP,
                                              m_entropyCoef[m], mask,
                                              w.mixScale, w.mixFloor, &ps);
                // ── AND TOWARD THE SEARCH ──
                //
                // The AlphaZero improvement operator, and the reason the search
                // is worth its cost: the visit distribution is a BETTER policy
                // than the prior that produced it, so pulling the network
                // toward it makes the next search start from a better prior.
                // Without this, a search is a re-ranker — journal 16 measured
                // that at 162 -> 149.
                //
                // Cross-entropy against the visits, masked to the legal set, so
                // probability is not spent on actions the mask will delete.
                // Weighted below 1 because PPO is still the primary signal: the
                // visits are an estimate from a LATENT rollout, and the
                // dynamics model drifts.
                if (!w.visits.empty() && MCTS_POLICY_WEIGHT > 0.0f) {
                    m_policy[m].accumulateCrossEntropyTargetInto(
                        ws.policy[m], w.visits, MCTS_POLICY_WEIGHT, mask);
                }
                // Health, per module, merged with the gradients. The entropy is
                // measured on the samples the policy actually chose, which is
                // the distribution a collapse would be hiding in.
                WorkerScratch::HeadStats& hs = ws.head[m];
                hs.entropySum += ps.entropy;
                hs.ceilingSum += std::log((double)std::max(2, ps.support));
                hs.klSum      += ps.kl;
                hs.n++;
            }
            // ── THE UNIFORM PULL, only while this head is under the floor ──
            //
            // Cross-entropy toward every legal action at once, which sums to a
            // pull toward the uniform distribution over the mask. Its gradient
            // is (p_i - 1/k), largest exactly where the entropy bonus's is
            // smallest, which is why this and not more entropy. See
            // UNIFORM_PULL_K. m_headDeficit is 0 for a healthy head and this
            // whole block does not execute.
            if (m_headDeficit[m] > 0.0f) {
                // Counted from the mask, not from ps: a booked sample skips the
                // surrogate, so ps.support would be zero and the pull would
                // quietly not apply on exactly the turns the book plays.
                int support = 0;
                const int nOut = m_policy[m].outputSize();
                for (int a = 0; a < nOut; ++a)
                    if (!mask || (a < (int)mask->size() && (*mask)[a])) ++support;
                if (support > 1) {
                    const float w = UNIFORM_PULL_K * m_headDeficit[m] / (float)support;
                    for (int a = 0; a < nOut; ++a)
                        if (!mask || (a < (int)mask->size() && (*mask)[a]))
                            m_policy[m].accumulateCrossEntropyInto(ws.policy[m], a, w, mask);
                }
            }

            // ── THE CLONE, as one gradient in this batch ──
            //
            // Same scratch, same flush, same learning rate as the policy
            // gradient it sits beside. Weighted down over the run and balanced
            // across the teacher's actions: see bcSampleWeight.
            if (w.teacher >= 0) {
                const float bw = bcSampleWeight(m, w.teacher, w.fromBook);
                if (bw > 0.0f)
                    m_policy[m].accumulateCrossEntropyInto(ws.policy[m], w.teacher,
                                                           bw, mask);
            }
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
            m_attack.initScratch(ws.attack);
            m_trunk.initScratch(ws.trunk);
            m_stanceHead.initScratch(ws.stance);
            m_diploValue.initScratch(ws.diploValue);
            m_diploValue.initScratch(ws.diploValueNext);
            m_dynamics.initScratch(ws.dynamics);
            m_trunk.initScratch(ws.trunkNext);
            m_diplo.initScratch(ws.diplo);
            ws.ready = true;
        }
    }

    // Tracing accumulates into shared counters, so it forces the
    // single-threaded path -- exact numbers matter more than speed here.
    if (threads <= 1 || s_updTrace) {
        runRange(0, m_work.size(), m_scratch[0]);
    if (s_updTrace && ++m_updTraceBatches % 20 == 0) {
        static const char* MN[MOD_COUNT] = {"econ", "politics", "war", "navy"};
        printf("[UPD] batch %lld\n", m_updTraceBatches);
        for (int m = 0; m < MOD_COUNT; ++m) {
            UpdTrace& t = m_upd[m];
            if (!t.n) continue;
            auto mean = [&](double s2) { return s2 / (double)t.n; };
            auto sd = [&](double s1, double s2) {
                const double mu = s1 / (double)t.n;
                return std::sqrt(std::max(0.0, s2 / (double)t.n - mu * mu));
            };
            printf("[UPD]   %-8s n=%-7lld norm %+.3f+-%.3f  V(s) %+.3f+-%.3f  "
                   "ADV %+.4f+-%.3f  advClip %.1f%%  tgtClip %.1f%%\n",
                   MN[m], t.n, mean(t.normSum), sd(t.normSum, t.normSq),
                   mean(t.baseSum), sd(t.baseSum, t.baseSq),
                   mean(t.advSum), sd(t.advSum, t.advSq),
                   100.0 * (double)t.advClipped / (double)t.n,
                   100.0 * (double)t.tgtClipped / (double)t.n);
            t = UpdTrace{};
        }
    }
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
        m_attack.mergeScratch(m_scratch[t].attack);
        m_diploValue.mergeScratch(m_scratch[t].diploValue);
        m_dynamics.mergeScratch(m_scratch[t].dynamics);
        m_diplo.mergeScratch(m_scratch[t].diplo);
        for (int m = 0; m < MOD_COUNT; ++m) {
            WorkerScratch::HeadStats& hs = m_scratch[t].head[m];
            m_batchHead[m].entropySum += hs.entropySum;
            m_batchHead[m].ceilingSum += hs.ceilingSum;
            m_batchHead[m].klSum      += hs.klSum;
            m_batchHead[m].n          += hs.n;
            hs = WorkerScratch::HeadStats{};
        }
    }
    applyStabilityGuards();
    // Last thing before the batch is dropped: the caller flushes immediately
    // after this returns, so "now" is the only moment the pre-step policy can
    // be recorded.
    captureKLProbe();

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

    // ── THE OUTCOME REGRESSION. See VALUE_MC_WEIGHT. ──
    //
    // The map's result is known now, so pull every value head toward what
    // actually happened to that country. A BLEND, not a replacement: the
    // advantage every policy step uses is target - V(s), and a V trained purely
    // on the outcome while `target` stays shaped would put the two in different
    // units and turn every advantage into noise.
    if (s_valueMcWeight > 0.0f && !m_outcomeBuf.empty()) {
        long long applied = 0;
        for (const OutcomeSample& os : m_outcomeBuf) {
            auto fit = m_finalStanding.find(os.cid);
            if (fit == m_finalStanding.end()) continue;   // eliminated: no share
            for (int m = 0; m < MOD_COUNT; ++m) {
                const std::vector<float>& out = m_value[m].forward(os.features);
                if (out.empty()) continue;
                const float cur = out[0];
                const float tgt = (1.0f - s_valueMcWeight) * cur +
                                  s_valueMcWeight * fit->second;
                m_value[m].valueUpdate(tgt, VALUE_MC_LR);
            }
            ++applied;
        }
        printf("[AI] value: regressed %lld state(s) toward the map outcome\n", applied);
        m_outcomeBuf.clear();
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

// ─── The difficulty ladder ───────────────────────────────────────────────
//
// See DifficultyProfile. Easy still samples softly and still makes the odd
// unforced move -- 8% rather than 35%, which is enough to be unpredictable and
// not enough to be incoherent -- but it aims with the old margin rule, consults
// no critic and holds no posture. Insane is everything, at argmax.
const AISystem::DifficultyProfile AISystem::DIFFICULTY[5] = {
    // temp, eps,  critic, aim,   posture, actionScale, coalition, searchDepth
    {1.60f, 0.08f, false,  false, false,   0.0f,        false,     0, true,  true},  // easy
    {0.90f, 0.05f, true,   false, true,    0.0f,        false,     0, true,  true},  // normal
    {0.35f, 0.02f, true,   true,  true,    0.0f,        false,     0, true,  true},  // hard
    // ── INSANE IS NO LONGER ARGMAX ──
    //
    // 0.05 was effectively deterministic, and a deterministic opponent is a
    // puzzle rather than a player: beat the position once and the answer keeps
    // working. 0.18 is still very sharp -- a one-unit logit lead is about 4:1 --
    // but the reply to a given position is drawn rather than fixed, so a human
    // reloading a save does not get the same move back.
    //
    // The rung was meant to earn its difficulty from three faculties instead --
    // the full size-scaled action budget, a coalition against the leader, and
    // two plies of search over its own value function. READ THE ROW: it has
    // none of them. actionScale is 0 and searchDepth is 0 for the reasons given
    // below and at SEARCH_BLEND (depth 2 measured 65.9% -> 62.8% of the land at
    // 8.5x the think time, because the Q head already estimates what the search
    // re-derives), and useCoalition is settled by the measurement below. What
    // actually separates this rung from Hard is the sampling: temperature 0.35
    // -> 0.30 and epsilon 0.02 -> 0.00. Anything stronger has to be earned by a
    // measurement, and every attempt so far has been reverted by one.
    //
    // ── actionScale IS 0 HERE, AND IT IS NOT AN OVERSIGHT ──
    //
    // The wider budget was measured on the shipped model and cost it a third of
    // everything it holds: 1.86x the scripted player's land became 0.91x at the
    // aggressive ramp and 1.26x at the gentle one, with bankruptcy up. The
    // reasoning for the budget is still right -- a flat three decisions is a cap
    // on competence that no training escapes -- but a policy trained under the
    // flat three does not know what to do with nine, and shipping a measured
    // regression because the argument is good is how a difficulty setting stops
    // meaning anything.
    //
    // So self-play trains under it (see the row below) and this flips to 1.0
    // when a model trained that way benches better than its predecessor
    // head-to-head. That is a one-character change gated on one measurement.
    // useCoalition is FALSE, and that is a measurement rather than an omission.
    // See COALITION_SHARE for the whole result.
    {0.30f, 0.00f, true,   true,  true,    0.0f,        false,     0, true,  true},  // insane
    // ── SELF-PLAY. Not reachable from the menu. ──
    // Everything the top rung has, plus the faculty it is not yet allowed to
    // ship: the policy has to experience the wide budget to learn it. The
    // temperature and epsilon here are ignored -- difficultyParams overrides
    // both for self-play with its own annealed schedule.
    {0.18f, 0.00f, true,   true,  true,    1.0f,        true,      2, true,  true},  // self-play
};

// ─── What each posture leans towards ─────────────────────────────────────
//
// Rows: 0 expand, 1 consolidate, 2 defend, 3 develop. See STANCE_BIAS for why
// these are biases rather than masks, and for the honest note about them being
// hand-authored taste.
//
// econ: save industry fort port specialize destroyer carrier fundUp fundDown
//       focusBldg focusArmy focusNavy
const float AISystem::STANCE_ECON[STANCE_COUNT][ECON_ACTIONS] = {
    {-1,  0,  0,  0,  0,  0,  0,  0,  0,  0, +1,  0},  // expand: pay for the army
    { 0,  0, +1,  0,  0,  0,  0,  0,  0,  0,  0,  0},  // consolidate: hold what we took
    {+1,  0, +1,  0,  0,  0,  0,  0,  0,  0, +1,  0},  // defend: forts and men
    { 0, +1,  0,  0, +1,  0,  0, +1, -1, +1,  0,  0},  // develop: build and research
};
// pol: hold enact pacUp pacDown cancel alliance nap guarantee calming
//      conciliate repress
// politics: hold policy pac+ pac- cancel ally nap guarantee calm concil repress TRADE
const float AISystem::STANCE_POL[STANCE_COUNT][POL_ACTIONS] = {
    { 0,  0,  0,  0,  0, +1,  0,  0,  0,  0,  0,  0},  // expand: allies for the war
    { 0,  0,  0,  0,  0,  0, +1,  0, +1, +1, -1, +1},  // consolidate: buy it, don't take it
    { 0,  0,  0,  0,  0, +1,  0, +1, +1,  0,  0,  0},  // defend: guarantees and calm
    { 0,  0,  0,  0,  0,  0, +1,  0,  0, +1,  0, +1},  // develop: money is the cheap lever
};
// war: hold recruit reinforce attack declare artillery ceasefire stage
const float AISystem::STANCE_WAR[STANCE_COUNT][WAR_ACTIONS] = {
    { 0,  0,  0, +1, +1,  0, -1, +1},                  // expand: press it
    { 0,  0, +1,  0, -2, +1, +1,  0},                  // consolidate: end it well
    { 0, +1, +1,  0, -2, +1,  0,  0},                  // defend: hold the line
    { 0, -1,  0, -1, -2,  0, +1,  0},                  // develop: not now
};
// navy: hold move bombard embark disembark scrap
const float AISystem::STANCE_NAVY[STANCE_COUNT][NAVY_ACTIONS] = {
    { 0, +1, +1, +1,  0,  0, +1},                      // expand: put men ashore, escort them
    { 0,  0,  0,  0, +1,  0,  0},                      // consolidate: bring them home
    { 0, +1,  0,  0, +1,  0, +1},                      // defend: keep the fleet near, contest it
    { 0,  0,  0,  0,  0, +1,  0},                      // develop: stop paying for hulls
};

bool AISystem::s_readOnlyModel = false;
bool AISystem::s_updTrace = std::getenv("OD_UPDATE_TRACE") != nullptr;
bool AISystem::s_scriptedControl = false;
int AISystem::s_exploitVariant = -1;
std::unordered_set<int> AISystem::s_exploitCids;
// Cloning weight, read once. See BC_DEFAULT_WEIGHT.
bool AISystem::s_bcObserve = std::getenv("OD_BC_OBSERVE") != nullptr;
// See the note at the diplo head's trunk backprop. 1.0 reproduces the old
// behaviour exactly, so a build with this compiled in measures the same as one
// without until OD_DIPLO_TRUNK_GRAD is set.
float AISystem::s_diploTrunkGrad = [] {
    const char* e = std::getenv("OD_DIPLO_TRUNK_GRAD");
    return e ? (float)atof(e) : 1.0f;
}();
// See s_diploCeasefireTrunkGrad. Also 1.0 by default, so a build carrying both
// knobs measures exactly the same as one carrying neither.
float AISystem::s_diploCeasefireTrunkGrad = [] {
    const char* e = std::getenv("OD_DIPLO_CEASEFIRE_TRUNK");
    return e ? (float)atof(e) : 1.0f;
}();
// See Experience::ceasefireCredit. OD_CEASEFIRE_CREDIT=0 removes the term.
float AISystem::s_ceasefireCredit = [] {
    if (const char* s = std::getenv("OD_CEASEFIRE_CREDIT")) {
        const float f = (float)atof(s);
        if (f >= 0.0f && f <= 4.0f) { printf("[AI] ceasefire credit x%.2f\n", f); return f; }
    }
    return 1.0f;
}();

// See s_diploPactWeight. 1.0 leaves diploReward exactly as written.
float AISystem::s_diploPactWeight = [] {
    const char* e = std::getenv("OD_DIPLO_PACT_WEIGHT");
    return e ? (float)atof(e) : 1.0f;
}();
// See s_warStageBias. All zero leaves every action selection exactly as it was.
//
// Generalised from the single stage knob once the same question came up for
// `artillery`, which carries +0.399 mean advantage — comparable to `declare
// war` — and sits at 0.0/4.3 probability. One array so any dead action can be
// swept without another constant each time.
//
//   OD_WAR_BIAS="0,0,0,0,0,3,0,5"   comma-separated, one per war action, in
//                                   the order hold, recruit, reinforce, attack,
//                                   declare, artillery, ceasefire, stage
//   OD_WAR_STAGE_BIAS=5             still sets index 7 alone, so journal 11's
//                                   sweep reproduces by name
int AISystem::openingTurns() {
    static const int v = [] {
        if (const char* e = std::getenv("OD_OPENING_TURNS")) {
            const int n = atoi(e);
            if (n >= 0 && n <= 200) return n;
        }
        return (int)AI_OPENING_TURNS;
    }();
    return v;
}

float AISystem::entropyFor(int head) {
    static const std::vector<float> v = [] {
        std::vector<float> e((size_t)MOD_COUNT + 1, PPO_ENTROPY);
        if (const char* s = std::getenv("OD_PPO_ENTROPY_HEADS")) {
            size_t i = 0;
            for (const char* p = s; *p && i <= (size_t)MOD_COUNT; ) {
                const float f = (float)atof(p);
                if (f >= 0.0f && f < 1.0f) e[i] = f;
                ++i;
                while (*p && *p != ',') ++p;
                if (*p == ',') ++p;
            }
            printf("[AI] per-head entropy: econ %.3f pol %.3f war %.3f navy %.3f diplo %.3f\n",
                   e[0], e[1], e[2], e[3], e[4]);
        }
        return e;
    }();
    // Every diplomacy kind shares the diplo base (the fifth value); the
    // controller then moves each kind's coefficient on its own.
    if (head >= 0 && head < MOD_COUNT)   return v[(size_t)head];
    if (head >= MOD_COUNT && head < GUARD_HEADS) return v[(size_t)MOD_COUNT];
    return ppoEntropy();
}

// See the note at "declare from strength". ON by default as of 2026-09-04 --
// the user's decision: a peaceful country declaring war on a random neighbour
// because "do I have more than two soldiers" is behaviour a player sees as
// broken, whatever the benchmark preferred. This changes the RULER (the rung
// every seat is scored against), so every rating stored before this date is on
// the old scale; the journal re-baselines. OD_SCRIPT_DECLARE_FIX=0 restores the
// bug for a comparison.
// See the "nobody stronger is standing next to us" note. ON by default.
bool AISystem::s_scriptLoomFix = [] {
    const char* e = std::getenv("OD_SCRIPT_LOOM_FIX");
    return !(e && *e == '0');
}();
bool AISystem::s_scriptDeclareFix = [] {
    const char* e = std::getenv("OD_SCRIPT_DECLARE_FIX");
    return !(e && *e == '0');
}();
// See s_lossAversion. 1.0 leaves the land term exactly symmetric, as shipped.
float AISystem::s_lossAversion = [] {
    const char* e = std::getenv("OD_LOSS_AVERSION");
    if (!e) return 1.0f;
    const float f = (float)atof(e);
    return (f >= 1.0f && f <= 10.0f) ? f : 1.0f;
}();
// See s_leagueExploitCap. Off = the league is past selves only, as shipped.
float AISystem::s_leagueExploitCap = [] {
    const char* e = std::getenv("OD_LEAGUE_EXPLOIT");
    if (!e) return 0.0f;
    const float f = (float)atof(e);
    // A bare "1" means "on", not "always" -- the uncapped reading is what took
    // 7 maps of 8. Anything in (0,1) is taken as the cap it looks like.
    if (f > 0.0f && f < 1.0f) return f;
    return 0.25f;
}();
float AISystem::s_warBias[8] = {0, 0, 0, 0, 0, 0, 0, 0};
float AISystem::s_navyBias[7] = {0, 0, 0, 0, 0, 0, 0};
static const bool s_navyBiasParsed = [] {
    if (const char* e = std::getenv("OD_NAVY_BIAS")) {
        int i = 0;
        for (const char* p = e; *p && i < 7; ) {
            AISystem::s_navyBias[i++] = (float)atof(p);
            while (*p && *p != ',') ++p;
            if (*p == ',') ++p;
        }
        printf("[AI] navy bias: %g %g %g %g %g %g %g\n", AISystem::s_navyBias[0], AISystem::s_navyBias[1],
               AISystem::s_navyBias[2], AISystem::s_navyBias[3], AISystem::s_navyBias[4],
               AISystem::s_navyBias[5], AISystem::s_navyBias[6]);
    }
    return true;
}();
float& AISystem::s_warStageBias = AISystem::s_warBias[7];
namespace {
const bool g_warBiasInit = [] {
    if (const char* e = std::getenv("OD_WAR_BIAS")) {
        int i = 0;
        for (const char* p = e; *p && i < 8; ) {
            AISystem::s_warBias[i++] = (float)atof(p);
            while (*p && *p != ',') ++p;
            if (*p == ',') ++p;
        }
    }
    if (const char* e = std::getenv("OD_WAR_STAGE_BIAS"))
        AISystem::s_warBias[7] = (float)atof(e);
    return true;
}();
}
// Monte-Carlo value blend, read once. See VALUE_MC_WEIGHT.
float AISystem::s_valueMcWeight = [] {
    if (const char* e = std::getenv("OD_VALUE_MC")) {
        const float w = (float)std::atof(e);
        if (w > 0.0f && w <= 1.0f) {
            printf("[AI] value head blended toward the map outcome at %.2f\n", w);
            return w;
        }
        printf("[AI] OD_VALUE_MC=%s ignored (want 0 < w <= 1)\n", e);
    }
    return 0.0f;
}();
bool AISystem::bcCloneModule(int module) {
    static const unsigned mask = [] {
        const char* e = std::getenv("OD_BC_MODULES");
        if (!e || !*e) return 0xFu;               // unset: every module, as shipped
        unsigned m = 0;
        std::string v(e);
        if (v.find("econ") != std::string::npos) m |= 1u << MOD_ECONOMY;
        if (v.find("pol")  != std::string::npos) m |= 1u << MOD_POLITICS;
        if (v.find("war")  != std::string::npos) m |= 1u << MOD_WAR;
        if (v.find("navy") != std::string::npos) m |= 1u << MOD_NAVY;
        printf("[AI] behavioural cloning restricted to module mask 0x%x\n", m);
        return m;
    }();
    return module >= 0 && module < MOD_COUNT && (mask & (1u << module));
}

float AISystem::s_bcWeight = [] {
    if (const char* e = std::getenv("OD_BC_FROM_SCRIPT")) {
        const float w = (float)std::atof(e);
        if (w > 0.0f && w <= 5.0f) {
            printf("[AI] behavioural cloning from the scripted player at weight %.2f\n", w);
            return w;
        }
        printf("[AI] OD_BC_FROM_SCRIPT=%s ignored (want 0 < w <= 5)\n", e);
    }
    return 0.0f;
}();
bool AISystem::s_scriptDuel = false;
bool AISystem::s_tutorialAI = false;
bool AISystem::s_evaluating = false;
std::string AISystem::s_opponentModelPath;

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
    // 7 = the attack head rides along, after the war-target head it mirrors.
    // APPENDED at the end of the net list rather than beside m_target, so a v6
    // file is a strict prefix of a v7 one: every blob a v6 reader wants is
    // still where it was, and loadModel can take a v6 file by reading what is
    // there and leaving the new head fresh. ATTACK_WARMUP_UPDATES then keeps
    // the old margin rule choosing until that head has learned something, so an
    // upgraded model plays exactly as it did until it can do better.
    // 8 = the dynamics head rides along, appended after the attack head for the
    // same reason that one was appended after the war-target head: a v7 file is
    // then a strict PREFIX of a v8 one. Every blob a v7 reader wants is still
    // where it was, and loadModel can take a v7 file by reading what is there
    // and leaving the forward model fresh. DYN_WARMUP_UPDATES then keeps the
    // search out of the way until it has learned something, so an upgraded
    // model plays exactly as it did until it can do better.
    // The AI's own ARCH number; see src/ai/AIVersion.h. static_assert rather
    // than a literal so the version header and the file format cannot drift.
    static_assert(ai::ARCH == 8, "AIVersion ARCH must equal the model format byte");
    out.push_back((uint8_t)ai::ARCH);
    out.push_back(MOD_COUNT * 3 + 9); // ...+ attack, + dynamics
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
    { std::vector<uint8_t> b; m_attack.serialize(b); appendBlob(out, b); }
    { std::vector<uint8_t> b; m_dynamics.serialize(b); appendBlob(out, b); }
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
    // Deinterleave the float bytes and deflate: 9 MB of weights becomes 4, and
    // every one of them comes back bit for bit. See ModelBlob.h for why the
    // deinterleave is what makes deflate work on float arrays at all.
    //
    // The round trip is CHECKED before anything is written, not asserted. This
    // file is the only copy of tens of millions of updates; an encoder that
    // silently altered a weight would be discovered as a slow decline in play
    // strength weeks later, with no way back. Unpacking costs ~20 ms against a
    // save that already serialises 9 MB, and it makes that failure impossible
    // rather than unlikely. If it ever does fail, the plain bytes are written
    // instead -- an older-but-correct file beats no file.
    if (std::vector<uint8_t> packed = modelblob::pack(out); !packed.empty()) {
        // Compared against `out` itself, and the swap only happens after: the
        // plain bytes are still there to check against, so this does not need a
        // nine-megabyte copy of them to hold one.
        std::vector<uint8_t> check = packed;
        if (modelblob::unpack(check) && check == out) {
            out.swap(packed);
        } else {
            printf("[AI] model compression did not round-trip; writing plain\n");
        }
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
    // than all held at once and averaged: one model is ~6 MB, and a run with
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
        const bool ok = blendAllToward(scratch, share);
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
        const bool ok = acc.blendAllToward(peer, share);
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
    // MOD_COUNT means the diplomacy head, which needed this more than any of
    // the four: it was trained for its whole life on samples whose action was
    // always 0 and whose behaviour log-probability was always 0, because
    // inference never reached it (see decideDiplomacy). Those are not weak
    // weights, they are weights fitted to a degenerate dataset through a
    // meaningless PPO ratio, and no amount of further training walks that back.
    // MOD_COUNT+1 means the stance head, which needs it for a third reason: it
    // has CONVERGED, not broken. Measured on the merged model, the trained
    // cohort spent 97% of its country-turns in "develop" while a uniform
    // control spread evenly over the four -- a controller that has stopped
    // controlling. That did not matter while the stance only set a feature bit;
    // now that it steers action selection (see STANCE_BIAS), inheriting a head
    // stuck on the one posture that discourages war would cement the very
    // passivity this change exists to break.
    if (module < 0 || module > MOD_COUNT + 1) {
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
    const bool diplo  = (module == MOD_COUNT);
    const bool stance = (module == MOD_COUNT + 1);
    const long long before =
        (long long)(stance ? a.m_stanceHead.updateCount()
                  : diplo  ? a.m_diplo.updateCount()
                           : a.m_policy[module].updateCount());
    // The same architectures and seeds the constructor uses, so a reset head is
    // indistinguishable from a fresh one.
    //
    // {TRUNK_OUT, N}, NOT {FEATURE_COUNT, 512, 320, N}. This line still built
    // the pre-trunk shape, so resetting any module produced a policy head whose
    // first layer was FEATURE_COUNT wide while the trunk hands it TRUNK_OUT --
    // forward() returns empty on that mismatch and pickAction falls through to
    // action 0. The tool for un-learning a bad reward silently made the module
    // it was pointed at pick "hold" forever, which is the same failure the
    // diplomacy head had, from the same cause.
    static const int ACTIONS[MOD_COUNT] =
        {ECON_ACTIONS, POL_ACTIONS, WAR_ACTIONS, NAVY_ACTIONS};
    if (stance) {
        // The stance shares the four modules' reward statistics (it trains on
        // their mean), so there is nothing of its own to clear but the head.
        a.m_stanceHead = NeuralNet({TRUNK_OUT, STANCE_COUNT}, 105);
    } else if (diplo) {
        a.m_diplo      = NeuralNet({TRUNK_OUT, DIPLO_OUTPUTS}, 300);
        a.m_diploValue = NeuralNet({FEATURE_COUNT, 160, 1}, 600);
        // The reward statistics are POLITICS' -- diplomacy deliberately shares
        // them (see the note where diploReward is normalised) -- so they are
        // left alone. Clearing them here would reset a module nobody asked to
        // reset.
    } else {
        a.m_policy[module] = NeuralNet({TRUNK_OUT, ACTIONS[module]},
                                       (uint32_t)(101 + module));
        a.m_value[module]  = NeuralNet({FEATURE_COUNT, 160, 1}, (uint32_t)(200 + module));
        a.m_rMean[module] = 0.0f;
        a.m_rVar[module]  = 1.0f;
    }
    a.m_modelPath = modelPath;
    s_readOnlyModel = false;
    a.saveModel();
    s_readOnlyModel = true;   // a's own destructor must not write again
    printf("[AI] reset the %s head in %s (discarded %lld updates); "
           "every other module kept\n",
           stance ? "stance" : diplo ? "diplomacy" : MODULE_NAMES[module],
           modelPath.c_str(), before);
    result = true;
    }
    s_readOnlyModel = wasReadOnly;
    return result;
}

// ALL of them, in each of the three. These counted the policy heads, the value
// heads and the diplomacy net, and stopped there -- so the trunk, which is the
// largest net in the model by a wide margin, the four Q heads, the stance, the
// relational pair and the war-target head were all missing from every one.
// "Model loaded (1.2 MB)" was the size of a third of a 6 MB file, and the
// trainer's parameter and update counters understated the model roughly
// fivefold. Same failure as blendAllToward's: a list that has to be extended
// every time a net is added, and was not.
size_t AISystem::serializedSize() const {
    size_t n = 6; // magic + version + net count
    std::vector<uint8_t> b;
    auto add = [&](const NeuralNet& net) {
        b.clear(); const_cast<NeuralNet&>(net).serialize(b); n += 4 + b.size();
    };
    add(m_trunk); add(m_stanceHead); add(m_relEncoder); add(m_relScore);
    for (int m = 0; m < MOD_COUNT; ++m) add(m_policy[m]);
    for (int m = 0; m < MOD_COUNT; ++m) add(m_value[m]);
    for (int m = 0; m < MOD_COUNT; ++m) add(m_q[m]);
    add(m_target); add(m_diplo); add(m_diploValue); add(m_attack);
    return n + 4 + MOD_COUNT * 2 * 4; // + the reward-statistics blob
}

long long AISystem::paramCount() const {
    long long n = (long long)m_trunk.paramCount()
                + (long long)m_stanceHead.paramCount()
                + (long long)m_relEncoder.paramCount()
                + (long long)m_relScore.paramCount()
                + (long long)m_target.paramCount()
                + (long long)m_diplo.paramCount()
                + (long long)m_diploValue.paramCount()
                + (long long)m_attack.paramCount();
    for (int m = 0; m < MOD_COUNT; ++m)
        n += (long long)m_policy[m].paramCount()
           + (long long)m_value[m].paramCount()
           + (long long)m_q[m].paramCount();
    return n;
}

unsigned long long AISystem::totalUpdates() const {
    unsigned long long n = m_trunk.updateCount() + m_stanceHead.updateCount()
                         + m_relEncoder.updateCount() + m_relScore.updateCount()
                         + m_target.updateCount() + m_diplo.updateCount()
                         + m_diploValue.updateCount() + m_attack.updateCount();
    for (int m = 0; m < MOD_COUNT; ++m)
        n += m_policy[m].updateCount() + m_value[m].updateCount()
           + m_q[m].updateCount();
    return n;
}

bool AISystem::blendAllToward(const AISystem& peer, float share) {
    // EVERY net that carries learning. See the declaration for what the two
    // hand-maintained lists this replaces had quietly been leaving out.
    bool ok = true;
    for (int m = 0; m < MOD_COUNT; ++m) {
        ok &= m_policy[m].blendToward(peer.m_policy[m], share);
        ok &= m_value[m].blendToward(peer.m_value[m], share);
        ok &= m_q[m].blendToward(peer.m_q[m], share);
    }
    ok &= m_trunk.blendToward(peer.m_trunk, share);
    ok &= m_stanceHead.blendToward(peer.m_stanceHead, share);
    ok &= m_relEncoder.blendToward(peer.m_relEncoder, share);
    ok &= m_relScore.blendToward(peer.m_relScore, share);
    ok &= m_target.blendToward(peer.m_target, share);
    ok &= m_attack.blendToward(peer.m_attack, share);
    ok &= m_diplo.blendToward(peer.m_diplo, share);
    ok &= m_diploValue.blendToward(peer.m_diploValue, share);
    return ok;
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
int AISystem::s_leagueGames[LEAGUE_CHECKPOINTS + 1] = {0};
int AISystem::s_leagueLosses[LEAGUE_CHECKPOINTS + 1] = {0};

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
    // THE EXPLOITER NEEDS NO FILE, so it must be added BEFORE the empty check —
    // otherwise a fresh model directory (no league-*.bin yet) returns here and
    // the rusher never joins at all. Measured: run H drew it 0 times in 6 maps
    // because of exactly this, and the comment below claimed the opposite.
    if (s_leagueExploitCap > 0.0f) present.push_back(LEAGUE_CHECKPOINTS);
    if (present.empty()) return false;

    // ── THE EXPLOITER IS A LEAGUE MEMBER, NOT A SCREEN ──
    //
    // A virtual slot meaning "the hand-written rusher". It carries the same
    // PFSP bookkeeping as a real checkpoint, so the pool plays it more often
    // exactly while the policy is losing to it — which is the property the
    // merge guard cannot provide, because a guard rejects a bad run after
    // paying for it while this stops the run going bad.
    //
    // Always available: unlike a checkpoint it needs no file, so a young run
    // that has never checkpointed still trains against a rusher from turn one.
    const int EXPLOIT_SLOT = LEAGUE_CHECKPOINTS;   // pushed above, before the
                                                   // empty check

    // PFSP: weight by how badly the slot beats us. See s_leagueGames.
    std::vector<double> weight;
    weight.reserve(present.size());
    for (int slot : present) {
        const int g = s_leagueGames[slot];
        const double lossRate = g > 0 ? (double)s_leagueLosses[slot] / (double)g : 0.5;
        weight.push_back(lossRate * lossRate + 0.05);
    }
    // ── CAP THE EXPLOITER'S SHARE ──
    // PFSP would hand it the draw outright: it weights by loss rate, and a
    // policy that never masters a blitz keeps its loss rate high forever. Cap
    // it so it stays one opponent among several. See s_leagueExploitCap.
    if (s_leagueExploitCap > 0.0f && present.size() > 1 &&
        present.back() == EXPLOIT_SLOT) {
        double others = 0.0;
        for (size_t i = 0; i + 1 < weight.size(); ++i) others += weight[i];
        const double cap = s_leagueExploitCap;
        const double maxW = others * cap / std::max(1e-9, 1.0 - cap);
        if (weight.back() > maxW) weight.back() = maxW;
    }

    std::discrete_distribution<size_t> pick(weight.begin(), weight.end());
    m_leagueSlot = present[pick(m_rng)];
    m_leagueIsExploiter = (m_leagueSlot == EXPLOIT_SLOT);
    if (m_leagueIsExploiter) {
        // No weights to read. m_leagueLoaded must still be set, because it is
        // what gates assignLeagueCountries() and therefore whether any country
        // is handed over at all.
        s_exploitVariant = SCRIPT_BLITZ;
        m_leagueLoaded   = true;
        m_leagueDiploLoaded = false;
        printf("[AI] league slot %d: the RUSHER (SCRIPT_BLITZ), not a past self\n",
               m_leagueSlot);
        return true;
    }
    const std::string path = leagueSlotPath(m_modelPath, m_leagueSlot);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 6) { fclose(f); return false; }
    std::vector<uint8_t> buf((size_t)n);
    const size_t rd = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (rd != buf.size()) return false;
    m_leagueIsExploiter = false;
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

bool AISystem::loadOpponentModel(const std::string& path) {
    m_opponentLoaded = false;
    m_leagueDiploLoaded = false;

    auto fail = [&](const char* why) {
        fprintf(stderr, "[AI] --vs-model: %s (%s)\n", why, path.c_str());
        return false;
    };

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return fail("cannot open");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 10) { fclose(f); return fail("too small to be a model"); }
    std::vector<uint8_t> buf((size_t)n);
    const size_t rd = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (rd != buf.size()) return fail("short read");
    if (!modelblob::unpack(buf)) return fail("compressed model is corrupt");
    if (buf.size() < 10) return fail("too small to be a model");
    if (memcmp(buf.data(), "ODAI", 4) != 0)
        return fail("not a model file (expected ODAI; league-N.bin checkpoints "
                    "are ODLG and are not full models)");
    // The SAME acceptance rule loadModel uses, and for the same reason: a
    // pre-trunk file stores whole nets where this build wants heads, and the
    // shapes differ in their first dimension. deserialize would "migrate" that
    // into nonsense rather than refuse it, and the run would report a confident
    // number produced by an opponent made of noise.
    // v6, v7 or v8. The opponent only ever needs the ACTING nets, and those sit
    // at the front of every layout -- each version appends to the end, so an
    // older file works here unchanged and a newer one is read as far as this
    // build cares about. A v6 opponent simply attacks by the margin rule, which
    // is what a v6 model did anyway.
    //
    // THIS GATE IS SEPARATE FROM loadModel'S AND HAS TO BE UPDATED WITH IT.
    // Adding the dynamics head (v8) without touching this one produced a
    // failure that looked like nothing at all: --vs-model refused the file,
    // the eval printed no `land held` line, and the pool's own regression check
    // reported "bench did not produce a result" and kept an unchecked merge.
    // The version accepted here is the version this build can WRITE.
    if (buf[4] < 6 || buf[4] > 8)
        return fail("pre-trunk model (v<6); this build needs v6, v7 or v8");
    {
        const int want = MOD_COUNT * 3 + (buf[4] >= 8 ? 9 : buf[4] >= 7 ? 8 : 7);
        if (buf[5] != want) return fail("unexpected net count");
    }

    size_t p = 6;
    auto readBlob = [&](NeuralNet* net) -> bool {
        if (p + 4 > buf.size()) return false;
        const uint32_t len = buf[p] | (buf[p+1] << 8) | (buf[p+2] << 16) |
                             ((uint32_t)buf[p+3] << 24);
        p += 4;
        if (p + len > buf.size()) return false;
        const bool ok = !net || net->deserialize(buf.data() + p, len);
        p += len;
        return ok;
    };

    // Blob order is loadModel's, and only the acting parts are taken. The value
    // heads, Q and the reward statistics all describe how to KEEP LEARNING; a
    // frozen opponent does none of that, and reading them would give this
    // object a second set of optimiser state to no purpose.
    if (!readBlob(&m_leagueTrunk)) return fail("truncated trunk");
    m_leagueTrunk.setTanhOutput(true);
    if (!readBlob(&m_leagueStance)) return fail("truncated stance head");
    for (int i = 0; i < 2; ++i)                       // relEncoder, relScore
        if (!readBlob(nullptr)) return fail("truncated header nets");
    for (int m = 0; m < MOD_COUNT; ++m)
        if (!readBlob(&m_leaguePolicy[m])) return fail("truncated policy heads");
    for (int i = 0; i < MOD_COUNT * 2 + 1; ++i)       // value, Q, target
        if (!readBlob(nullptr)) return fail("truncated value/Q heads");
    // The opponent's diplomacy head gets the same widening treatment loadModel
    // gives ours, and for the same reason: --vs-model against a file written
    // before the per-kind layout would otherwise field an opponent that answers
    // every request except a ceasefire from an untrained row. A head-to-head
    // whose two sides are not fed identically is not a match, and this is the
    // half nobody would think to check.
    {
        if (p + 4 > buf.size()) return fail("truncated diplomacy net");
        const uint32_t len = buf[p] | (buf[p+1] << 8) | (buf[p+2] << 16) |
                             ((uint32_t)buf[p+3] << 24);
        const size_t body = p + 4;
        if (body + len > buf.size()) return fail("truncated diplomacy net");
        auto at32 = [&](size_t off) -> uint32_t {
            return buf[off] | (buf[off+1] << 8) | (buf[off+2] << 16) |
                   ((uint32_t)buf[off+3] << 24);
        };
        uint32_t storedOut = 0;
        if (len >= 12) {
            const uint32_t n = at32(body + 8);
            if (n >= 2 && n <= 16 && len >= 12 + n * 4)
                storedOut = at32(body + 12 + (n - 1) * 4);
        }
        bool ok;
        if (storedOut == (uint32_t)DIPLO_ACTIONS && DIPLO_OUTPUTS != DIPLO_ACTIONS) {
            NeuralNet narrow({TRUNK_OUT, DIPLO_ACTIONS}, 300);
            ok = narrow.deserialize(buf.data() + body, len);
            if (ok) {
                m_leagueDiplo = NeuralNet({TRUNK_OUT, DIPLO_OUTPUTS}, 301);
                ok = m_leagueDiplo.replicateOutputBlocks(narrow, DIPLO_ACTIONS,
                                                         OFFER_KINDS);
            }
        } else {
            ok = m_leagueDiplo.deserialize(buf.data() + body, len);
        }
        p = body + len;
        if (!ok) return fail("truncated diplomacy net");
    }

    m_leagueDiploLoaded  = true;
    m_leagueStanceLoaded = true;
    m_leagueLoaded = true;
    m_opponentLoaded = true;
    // m_leagueSlot stays -1: this is not a checkpoint drawn from the rotation,
    // so recordLeagueOutcome must not score it into the PFSP tables that pick
    // training opponents.
    printf("[AI] Opponent model: %s (control cohort plays this, not dice)\n",
           path.c_str());
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
    // ── WHY A REFUSAL HAS TO BE TOLD APART FROM AN ABSENCE ──
    //
    // Every `return false` below used to reach a caller that printed "Fresh
    // model (no file at X)" -- for a file that exists, that somebody had just
    // copied there on purpose, and that the loader had read and rejected. So
    // pointing an evaluation at a model the loader dislikes did not fail: it
    // benched an UNTRAINED network and printed a perfectly ordinary score.
    // Every result taken that way is a measurement of random weights wearing
    // the label of a trained model.
    //
    // m_loadError carries the reason when the bytes were there and unusable,
    // and stays empty when the file simply is not there -- which is a legitimate
    // first run and must stay quiet.
    m_loadError.clear();
    // odFile, not fopen: the shipped model is an APK asset on Android and only
    // AAssetManager can reach it. Writes still use fopen -- they go to internal
    // storage, which the training path wants anyway. See OdFile.h.
    const std::string bytes = odFile::readAll(m_modelPath);
    if (bytes.empty()) return false;                       // no file: not an error
    if (bytes.size() < 10) {
        m_loadError = "file is " + std::to_string(bytes.size()) + " bytes, too small to be a model";
        return false;
    }
    std::vector<uint8_t> buf(bytes.begin(), bytes.end());
    // ODAZ is the compressed container; a plain ODAI file passes through
    // untouched, so a model written by an older build still loads.
    if (!modelblob::unpack(buf)) { m_loadError = "compressed container is corrupt"; return false; }
    if (buf.size() < 10) { m_loadError = "unpacked to fewer than 10 bytes"; return false; }
    if (memcmp(buf.data(), "ODAI", 4) != 0) {
        m_loadError = "not a model file: expected magic ODAI (league-N.bin "
                      "checkpoints are ODLG and are not full models)";
        return false;
    }
    // v1 models load fine — they just carry no reward statistics, so those keep
    // their cold-start values. Refusing them would throw away every hour of
    // training already invested in the file on disk.
    const int fileVersion = buf[4];
    if (fileVersion < 1 || fileVersion > 8) {
        m_loadError = "format byte is " + std::to_string(fileVersion) +
                      "; this build reads 6 to 8";
        return false;
    }
    // A PRE-TRUNK FILE CANNOT BE READ, AND MUST NOT BE GUESSED AT.
    //
    // Versions 1-5 store policy/Q/diplo as whole nets taking the full feature
    // vector; this build wants heads taking a TRUNK_OUT embedding. The shapes
    // differ in their FIRST dimension, which deserialize's input-widening path
    // would happily "migrate" into nonsense. Refusing is the honest failure:
    // the caller prints "Fresh model" and training starts over, which is the
    // price of the architecture change and was decided deliberately.
    if (fileVersion < 6) {
        m_loadError = "pre-trunk model (v" + std::to_string(fileVersion) +
                      "); this build needs v6 or later";
        return false;
    }
    const int count = buf[5];
    // v1/v2 carry no Q heads. Those files are every hour of training done
    // before this existed, so they load and simply leave Q at its initial
    // weights -- which Q_WARMUP_UPDATES then keeps out of the way until it has
    // learned something.
    // v7 appends the attack head; v6 is a strict prefix of it. A v6 file loads
    // in full and simply leaves that head at its initial weights, which
    // ATTACK_WARMUP_UPDATES then keeps out of the way until it has learned
    // something -- the same arrangement Q heads got when they were added.
    const bool hasAttack = (fileVersion >= 7);
    // v8 appends the dynamics head; v7 is a strict prefix of it and loads in
    // full, leaving the forward model at its initial weights.
    const bool hasDynamics = (fileVersion >= 8);
    if (count != MOD_COUNT * 3 + (hasDynamics ? 9 : hasAttack ? 8 : 7)) return false;
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
    // ── THE DIPLOMACY HEAD, WHICH CHANGED SHAPE ──
    //
    // A file written before the per-kind layout carries a {TRUNK_OUT,
    // DIPLO_ACTIONS} blob. deserialize would migrate it through the
    // gained-outputs path, which is right for new ACTIONS and wrong here: the
    // extra outputs are not new choices, they are the SAME accept/reject asked
    // about a different request kind. Left at Xavier, a shipped model would
    // keep only its ceasefire policy and answer every pact, alliance and
    // guarantee from an untrained row -- its behaviour would change the instant
    // it loaded, which is exactly the objection the input-widening path makes.
    //
    // So read the narrow blob into a narrow net and REPEAT it across all seven
    // pairs. The model then plays identically on every kind, and training
    // pulls them apart from there.
    {
        if (p + 4 > buf.size()) return false;
        const uint32_t len = buf[p] | (buf[p+1] << 8) | (buf[p+2] << 16) |
                             ((uint32_t)buf[p+3] << 24);
        const size_t body = p + 4;
        if (body + len > buf.size()) return false;
        auto at32 = [&](size_t off) -> uint32_t {
            return buf[off] | (buf[off+1] << 8) | (buf[off+2] << 16) |
                   ((uint32_t)buf[off+3] << 24);
        };
        // Peek the stored output width: magic, version, layer count, sizes.
        uint32_t storedOut = 0;
        if (len >= 12) {
            const uint32_t n = at32(body + 8);
            if (n >= 2 && n <= 16 && len >= 12 + n * 4)
                storedOut = at32(body + 12 + (n - 1) * 4);
        }
        bool ok;
        if (storedOut == (uint32_t)DIPLO_ACTIONS && DIPLO_OUTPUTS != DIPLO_ACTIONS) {
            NeuralNet narrow({TRUNK_OUT, DIPLO_ACTIONS}, 300);
            ok = narrow.deserialize(buf.data() + body, len);
            if (ok) {
                m_diplo = NeuralNet({TRUNK_OUT, DIPLO_OUTPUTS}, 300);
                ok = m_diplo.replicateOutputBlocks(narrow, DIPLO_ACTIONS, OFFER_KINDS);
                if (ok) printf("[AI] diplomacy head widened to one pair per "
                               "request kind; the trained pair was copied to "
                               "all %d\n", OFFER_KINDS);
            }
        } else {
            ok = m_diplo.deserialize(buf.data() + body, len);
        }
        p = body + len;
        if (!ok) return false;
    }
    if (hasDiploValue && !readBlob(m_diploValue)) return false;
    if (hasAttack && !readBlob(m_attack)) return false;
    if (hasDynamics) {
        if (!readBlob(m_dynamics)) return false;
        m_dynamics.setTanhOutput(true);
    }
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

// Action histogram storage and dump. See OD_ACT_HIST at the module dispatch.
int AISystem::s_actHist[AISystem::MOD_COUNT][AISystem::MAX_MODULE_ACTIONS] = {};
int AISystem::s_offHist[AISystem::MOD_COUNT][AISystem::MAX_MODULE_ACTIONS] = {};
long long AISystem::s_navalPorts = 0;
long long AISystem::s_navalShips = 0;
long long AISystem::s_industryBuys = 0;
long long AISystem::s_austeritySteps = 0;
// Which austerity branch actually fired. The shipped win was a REORDER of this
// list (research moved from first to last, +71/+42 rating), so whether any
// further reorder is worth trying depends entirely on which branches fire at
// all. Trace firings before attributing.
long long AISystem::s_austBranch[6] = {0,0,0,0,0,0};
// WHERE THE MONEY ACTUALLY GOES. The econ head withholds industry for want of
// cash on 90.4% of the turns it wants it, and industry compounds the same way
// research does -- so whatever is consuming the budget is the largest untapped
// lever in the game. Never measured; this measures it.
double AISystem::s_expense[8] = {0,0,0,0,0,0,0,0};
long long AISystem::s_expenseN = 0;
// Mean research allocation per country-turn, under OD_ACT_HIST only.
//
// The research-ratchet finding is +21.9 points of world across three seed
// sets, and its STORY is that two reflexes ratchet this slider down in a game
// where it never comes back up. Every arm measured the outcome; none measured
// the slider. Research NODES per 1k moved only +3.0% between control and
// treatment, which is consistent with the story and nowhere near proof of it --
// nodes completed also move with income, territory and time. This reads the
// quantity the explanation actually names.
double AISystem::s_researchSum = 0.0;
long long AISystem::s_researchN = 0;

long long AISystem::s_gateOffered[ECON_ACTIONS]    = {0};
long long AISystem::s_gateNoCash[ECON_ACTIONS]    = {0};
long long AISystem::s_gateImpossible[ECON_ACTIONS] = {0};

void AISystem::dumpActionHistogram() {
    dumpNoopHistogram();
    {
        bool any = false;
        for (int i = 0; i < ECON_ACTIONS; ++i)
            if (s_gateOffered[i] || s_gateNoCash[i] || s_gateImpossible[i]) any = true;
        if (any) {
            printf("[GATE] econ action:  offered / blocked-by-cash / not-possible\n");
            for (int i = 0; i < ECON_ACTIONS; ++i) {
                const long long tot = s_gateOffered[i] + s_gateNoCash[i] + s_gateImpossible[i];
                if (!tot) continue;
                printf("[GATE]   a%-2d  %9lld  %9lld  %9lld    offered %5.1f%%  cash-blocked %5.1f%%\n",
                       i, s_gateOffered[i], s_gateNoCash[i], s_gateImpossible[i],
                       100.0 * (double)s_gateOffered[i] / (double)tot,
                       100.0 * (double)s_gateNoCash[i] / (double)tot);
            }
        }
    }
    if (s_researchN > 0) {
        printf("[ACTHIST] mean research allocation %.4f over %lld country-turns\n",
               s_researchSum / (double)s_researchN, s_researchN);
        // od_bench runs the binary with capture_output=True and parses only the
        // score, so anything printed here is discarded. OD_ACT_HIST_FILE gets
        // the number out of a benched process, which is the only place these
        // rules fire often enough for the mean to mean anything.
        if (const char* f = std::getenv("OD_ACT_HIST_FILE")) {
            if (FILE* fp = fopen(f, "a")) {
                fprintf(fp, "%.4f %lld\n", s_researchSum / (double)s_researchN, s_researchN);
                fclose(fp);
            }
        }
    }
    static const char* names[] = {"ECON", "POLITICS", "WAR", "NAVY"};
    fprintf(stderr, "[ACTHIST] naval reflex bought: %lld ports, %lld destroyers; industry reflex: %lld\n",
            s_navalPorts, s_navalShips, s_industryBuys);
    fprintf(stderr, "[ACTHIST] research austerity steps: %lld\n", s_austeritySteps);
    if (s_expenseN > 0) {
        static const char* en[8] = {"army","navy","policy","minority",
                                    "research","pacification","indUpkeep","GROSS"};
        fprintf(stderr, "[ACTHIST] mean expenses over %lld country-turns:\n", s_expenseN);
        for (int i = 0; i < 8; ++i)
            fprintf(stderr, "[ACTHIST]   %-14s %8.2f   %5.1f%% of gross\n", en[i],
                    s_expense[i] / (double)s_expenseN,
                    s_expense[7] > 0 ? 100.0 * s_expense[i] / s_expense[7] : 0.0);
    }
    fprintf(stderr, "[ACTHIST] austerity branches: research-first %lld  pacification %lld  "
            "doctrine %lld  minority %lld  scrap-ship %lld  research-last %lld\n",
            s_austBranch[0], s_austBranch[1], s_austBranch[2],
            s_austBranch[3], s_austBranch[4], s_austBranch[5]);
    for (int m = 0; m < MOD_COUNT; ++m) {
        long long tot = 0;
        for (int a = 0; a < MAX_MODULE_ACTIONS; ++a) tot += s_actHist[m][a];
        if (tot == 0) continue;
        fprintf(stderr, "[ACTHIST] %-8s total %lld\n", names[m], tot);
        for (int a = 0; a < MAX_MODULE_ACTIONS; ++a)
            if (s_actHist[m][a] || s_offHist[m][a] || a < 12)
                fprintf(stderr, "[ACTHIST]   %-8s a%-2d picked %7d (%5.2f%%)  offered %8d  taken-when-offered %5.2f%%\n",
                        names[m], a, s_actHist[m][a], 100.0 * s_actHist[m][a] / (double)tot,
                        s_offHist[m][a],
                        s_offHist[m][a] ? 100.0 * s_actHist[m][a] / (double)s_offHist[m][a] : 0.0);
    }
}

// ── NAVAL REFLEX (OD_NAVAL_REFLEX, off by default) ──
//
// Measured with OD_ACT_HIST on the Norway seat: the econ head is offered a
// port 1,914 times and a warship 6,020 times in one game and takes neither,
// ever, while taking "raise research funding" on 95.5% of the turns it is
// legal. The navy MODULE is healthy -- it steams, bombards, embarks, lands
// and engages -- so the AI operates a fleet it can never replace, and the
// bench's floor seat is a coastal country.
//
// validEconomy already records why a rule here is not the obvious answer: a
// savings reserve was built to make ports affordable, it worked (716 offers),
// and the policy took the action four times. A mask cannot make a collapsed
// head choose. So this does not try to tempt the head -- it DECIDES, and then
// calls the head's own executor so that every constraint (the port candidate,
// the siege earmark, the berth checked against projected income, the
// bankruptcy history that motivated all of it) is enforced by the one copy of
// the rule that already exists. If the executor refuses, nothing happens.
//
// The cadence exists because of that bankruptcy history: hulls cost 10 or 25
// a turn for ever, and the reason the head was ever punished for buying them
// was a country ordering several before the first bill arrived.
void AISystem::navalReflex(int cid) {
    static const bool on = std::getenv("OD_NAVAL_REFLEX") &&
                           atoi(std::getenv("OD_NAVAL_REFLEX")) != 0;
    if (!on) return;
    static const int cadence = std::getenv("OD_NAVAL_CADENCE")
                             ? atoi(std::getenv("OD_NAVAL_CADENCE")) : 8;
    int& last = m_lastNavalBuy[cid];
    if (last != 0 && m_turn - last < cadence) return;
    // A harbour first: it is what unlocks the hulls, returns no income, and so
    // loses every comparison the economy makes on payback.
    int pid = -1; float cost = 0.0f;
    if (nextPortBuy(cid, pid, cost)) {
        const std::string r = execEconomy(cid, 3);
        if (r.rfind("port lvl", 0) == 0) { last = m_turn; ++s_navalPorts; return; }
    }
    // Then a destroyer, the cheap hull. The executor decides whether the berth
    // is affordable at the income of the turn it floats.
    const std::string r = execEconomy(cid, 5);
    if (r.find("destroyer") != std::string::npos) { last = m_turn; ++s_navalShips; }
}

// ── INDUSTRY REFLEX (OD_INDUSTRY_REFLEX, off by default) ──
//
// Written from the horizon finding rather than from a trace. The bench runs
// 120 turns, and at 120 turns an investment has not paid for itself: every
// rule this project ever measured was scored before the return arrived. That
// is a BIAS, not noise -- the two rules that survive a 400-turn test
// (campaigns, siege) both change what happens over many turns, and every
// local adjustment tried in one session got worse at length.
//
// Industry is the compounding action. The econ head takes it on 54% of the
// turns it is offered but is only offered it on 6% of decisions, and spends
// 86% of its agency moving the research slider up and down -- a toggle that
// compounds nothing. So this buys industry outside the head, on a cadence,
// through execEconomy so the affordability rule stays in one place.
//
// Explicitly expected to look bad at 120 turns and good at 400. If it looks
// bad at both, the hypothesis is wrong and the horizon was not the reason
// investment rules keep failing here.
void AISystem::industryReflex(int cid) {
    static const bool on = std::getenv("OD_INDUSTRY_REFLEX") &&
                           atoi(std::getenv("OD_INDUSTRY_REFLEX")) != 0;
    if (!on) return;
    static const int cadence = std::getenv("OD_INDUSTRY_CADENCE")
                             ? atoi(std::getenv("OD_INDUSTRY_CADENCE")) : 4;
    int& last = m_lastIndustryBuy[cid];
    if (last != 0 && m_turn - last < cadence) return;
    int pid = -1, lvl = 0; float cost = 0.0f;
    if (!nextIndustryBuy(cid, pid, lvl, cost)) return;
    const std::string r = execEconomy(cid, 1);
    if (r.rfind("industry", 0) == 0 || r.find("lvl") != std::string::npos) {
        last = m_turn;
        ++s_industryBuys;
    }
}

// ── RESEARCH AUSTERITY (OD_RESEARCH_AUSTERITY, off by default) ──
//
// Found by asking why N24 is the only model in the pool that still holds all
// six seats after 400 turns. Against N36 -- 32 rating points behind but with
// a floor of 4 -- on the seat where N36 collapses (France in a rushing
// world), the action distributions differ in one place that matters:
//
//     econ a7 research funding UP     N24 52.1%   N36 41.0%
//     econ a8 research funding DOWN   N24 39.0%   N36  0.00%
//     econ a0 nothing                 N24  2.7%   N36 40.9%
//
// N24 moves the slider both ways; N36 only raises it and then idles. In a
// world where everyone attacks, N36 has locked its income into research and
// has no way to release it for defence. So the research allocation is the
// AI's main economic regulator, and using it in ONE direction is the
// difference between holding a seat and losing it.
//
// This reflex gives a model that cannot regulate the behaviour of one that
// can: when the treasury is low and the country is at war, walk the
// allocation down by the same 0.05 step the head's own action uses. It
// should be inert on N24, which already does this, and should help N36.
// That asymmetry is the test -- a rule derived from a difference between two
// models ought to close the difference and do nothing to the model it was
// derived from.
void AISystem::researchAusterityReflex(int cid) {
    static const bool on = std::getenv("OD_RESEARCH_AUSTERITY") &&
                           atoi(std::getenv("OD_RESEARCH_AUSTERITY")) != 0;
    if (!on) return;
    Game& g = *m_g;
    Country* c = g.m_countries.getCountry(cid);
    if (!c) return;
    auto w = m_warWith.find(cid);
    const bool atWar = (w != m_warWith.end() && !w->second.empty());
    if (!atWar) return;
    static const double floorCash = std::getenv("OD_AUSTERITY_CASH")
                                  ? atof(std::getenv("OD_AUSTERITY_CASH")) : 20.0;
    if (c->treasury >= floorCash) return;
    float& alloc = g.m_countryResearchAllocation[cid];
    if (alloc <= 0.0f) return;
    alloc = std::max(0.0f, alloc - 0.05f);
    ++s_austeritySteps;
}
