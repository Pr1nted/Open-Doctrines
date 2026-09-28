// A fight that does not finish: reinforce, withdraw, and the men in between.
//
// An assault used to be settled the instant it was ordered -- it carried, or it
// was thrown back and the men who never reached the line marched home. So a war
// was a sequence of instants: no province was ever contested at the end of a
// turn, and there was nothing for a player to DO inside a war, only decisions
// before one. A battle is what an unfinished assault becomes.
//
// WHAT THIS PINS, and it is mostly bookkeeping rather than balance, because the
// bookkeeping is where this feature can quietly go wrong: men committed to a
// battle are held in the battle record and NOT in m_provinceArmies, which means
// every existing thing that counts a country's soldiers stops seeing them. The
// first version of this shipped four such holes at once -- fuel demand, the
// munitions reserve, per-country upkeep and the one-pass upkeep table all walk
// province armies -- so an army parked in a standing battle ate no fuel, drew
// no munitions and cost no upkeep. A war that stops being paid for while it is
// being fought is not a subtle bug; it is a strategy.
//
// It replicates the accounting rather than linking the resolver, the way
// tests/army_split_test.cpp replicates the split arithmetic: the real one needs
// a Game, a map and a world. If the rule changes, change it here in the same
// commit -- that is what makes this test worth having.

#include "GameStructs.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;
static void ok(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    ++g_failed;
    printf("  FAIL  %s\n", what.c_str());
}
static void section(const char* n) { printf("\n== %s ==\n", n); }

// The parts of Game the rules below touch.
struct Prov { int owner = 0; long long garrison = 0; };
struct Btl  { int pid = 0; int cid = 0; long long men = 0; int from = -1; int rounds = 0; };

struct World {
    std::vector<Prov> provs;
    std::vector<Btl>  battles;

    // Game::countryTroops: garrisons AND men in battles.
    long long countryTroops(int cid) const {
        long long n = 0;
        for (size_t p = 1; p < provs.size(); ++p)
            if (provs[p].owner == cid) n += provs[p].garrison;
        for (const auto& b : battles) if (b.cid == cid) n += b.men;
        return n;
    }
    // What the OLD accounting saw: garrisons only.
    long long garrisonsOnly(int cid) const {
        long long n = 0;
        for (size_t p = 1; p < provs.size(); ++p)
            if (provs[p].owner == cid) n += provs[p].garrison;
        return n;
    }
    Btl* battleAt(int pid, int cid) {
        for (auto& b : battles) if (b.pid == pid && b.cid == cid) return &b;
        return nullptr;
    }
    // Game::withdrawFromBattle.
    void withdraw(int pid, int cid) {
        for (size_t i = 0; i < battles.size(); ++i) {
            if (battles[i].pid != pid || battles[i].cid != cid) continue;
            const int home = battles[i].from;
            // Home only if we still hold it: ground lost while the battle was
            // fought is not somewhere to retreat to, and men with nowhere to go
            // are lost -- the rule a repulsed landing already follows.
            if (home > 0 && home < (int)provs.size() && provs[home].owner == cid)
                provs[home].garrison += battles[i].men;
            battles.erase(battles.begin() + i);
            return;
        }
    }
};

int main() {
    section("men in a battle are still the country's men");
    {
        World w;
        w.provs.resize(4);
        w.provs[1] = {7, 100000};      // ours, with a garrison
        w.provs[2] = {9, 80000};       // theirs
        ok(w.countryTroops(7) == 100000, "before the assault, all at home");

        // 60,000 march out and end up in a standing battle over province 2.
        w.provs[1].garrison -= 60000;
        w.battles.push_back({2, 7, 60000, 1, 0});

        ok(w.countryTroops(7) == 100000,
           "committed to a battle, the army is the same size");
        ok(w.garrisonsOnly(7) == 40000,
           "but garrisons alone see only 40,000 -- the hole this closes");
        ok(w.countryTroops(7) - w.garrisonsOnly(7) == 60000,
           "and the difference is exactly the men in the fight");
    }

    section("a war is paid for while it is being fought");
    {
        // The four callers that count soldiers: fuel, munitions, and upkeep
        // twice. All of them charge per 10,000 men, so one property covers all
        // four -- what matters is WHICH total they are given.
        World w;
        w.provs.resize(3);
        w.provs[1] = {7, 20000};
        w.battles.push_back({2, 7, 180000, 1, 0});
        const double perTenK = 0.01;
        const double charged = (double)w.countryTroops(7) / 10000.0 * perTenK;
        const double wouldHaveBeen = (double)w.garrisonsOnly(7) / 10000.0 * perTenK;
        ok(charged > wouldHaveBeen * 9.0,
           "an army mostly in the field costs ~10x what garrisons alone suggest");
        ok(charged == 0.2, "200,000 men cost 0.20 whether they are fighting or not");
    }

    section("withdrawing brings the men home");
    {
        World w;
        w.provs.resize(3);
        w.provs[1] = {7, 10000};
        w.battles.push_back({2, 7, 50000, 1, 3});
        const long long before = w.countryTroops(7);
        w.withdraw(2, 7);
        ok(w.battles.empty(), "the battle ends");
        ok(w.provs[1].garrison == 60000, "and the men are back where they came from");
        ok(w.countryTroops(7) == before, "no soldier is created or destroyed by leaving");
    }

    section("...unless there is no home left to go to");
    {
        World w;
        w.provs.resize(3);
        w.provs[1] = {9, 10000};       // the province we came from has been TAKEN
        w.battles.push_back({2, 7, 50000, 1, 3});
        w.withdraw(2, 7);
        ok(w.battles.empty(), "the battle still ends");
        ok(w.countryTroops(7) == 0, "but men with nowhere to retreat to are lost");
        ok(w.provs[1].garrison == 10000, "and they do not reinforce the new owner");
    }

    section("a landing has nowhere to withdraw to, by construction");
    {
        World w;
        w.provs.resize(3);
        w.provs[1] = {7, 10000};
        w.battles.push_back({2, 7, 50000, /*from=*/-1, 1});   // fromProvince -1 = from the sea
        w.withdraw(2, 7);
        ok(w.countryTroops(7) == 10000, "the landing force is lost, as a failed landing drowns");
    }

    section("reinforcing adds to the fight, it does not start a second one");
    {
        // Two assaults on one province in one turn would each be weighed
        // against the frontage separately, which is a way of bringing more men
        // to bear than the ground holds -- exactly what a frontage prevents.
        World w;
        w.provs.resize(3);
        w.provs[1] = {7, 90000};
        w.battles.push_back({2, 7, 30000, 1, 2});
        const long long before = w.countryTroops(7);
        w.provs[1].garrison -= 40000;
        w.battleAt(2, 7)->men += 40000;
        ok(w.battles.size() == 1, "still one battle, not two");
        ok(w.battleAt(2, 7)->men == 70000, "and it is 70,000 strong");
        ok(w.countryTroops(7) == before, "reinforcing creates nobody");
        ok(w.battleAt(2, 7)->rounds == 2, "and does not reset how long it has run");
    }

    // ── WHICH WAY IS IT GOING, over the whole fight ──
    //
    // The panel used to answer this from ONE round -- lastDefPower >
    // lastAtkPower -- and coloured its headline red whenever that was true.
    // The record's own note says that is true of almost every round of a
    // battle being won by persistence: the province 824 trace is four repulses
    // in a row and the fourth carries, and a withdraw reflex written on that
    // signal measured 7 to 38 rating points WORSE than never withdrawing.
    // So the panel was telling a player their winning sieges were lost.
    {
        auto fight = [](int rounds, double openDef, double lastDef,
                        long long atkLost, long long defLost) {
            Battle b;
            b.rounds          = rounds;
            b.openingDefPower = openDef;
            b.lastDefPower    = lastDef;
            b.totalAtkLosses  = atkLost;
            b.totalDefLosses  = defLost;
            // The single-round signal the old rule read, set so that every case
            // below LOOKS like a round we lost. That is the whole point.
            b.lastAtkPower = 100.0;
            b.lastDefPower = lastDef;
            return b;
        };

        // The 824 case: four repulses, the defence worn to half, the exchange
        // ours. Persistence is winning this and the old rule called it losing.
        ok(battleTrend(fight(4, 1000.0, 500.0, 8000, 9000)) == BattleTrend::Grinding,
           "a grind that is wearing the defence down reads as winning");

        // A wall: the defence is exactly as strong as it was and we are paying
        // more than they are. This is the one worth leaving.
        ok(battleTrend(fight(5, 1000.0, 1000.0, 9000, 4000)) == BattleTrend::Failing,
           "a defence that has not moved while we bleed reads as failing");

        // Mixed, and early: the expensive mistake is withdrawing here.
        ok(battleTrend(fight(1, 1000.0, 1000.0, 5000, 9000)) == BattleTrend::TooEarly,
           "one round of mixed signals is too early to call");

        // Mixed and no longer early.
        ok(battleTrend(fight(6, 1000.0, 1000.0, 5000, 9000)) == BattleTrend::Stalled,
           "the same signals after six rounds are a stalemate");

        // Nothing has happened yet.
        ok(battleTrend(fight(0, 0.0, 0.0, 0, 0)) == BattleTrend::TooEarly,
           "a battle that has not fought a round says so");

        // A defence worn down but an exchange going against us is NOT a win:
        // taking the province is not worth the army if the army is gone.
        ok(battleTrend(fight(4, 1000.0, 400.0, 20000, 3000)) != BattleTrend::Grinding,
           "wearing them down while losing the exchange is not winning");

        // AND THE PROPERTY THAT MATTERS: the verdict must not be readable off
        // the last round alone, which is what it was.
        const Battle won  = fight(4, 1000.0, 500.0, 8000, 9000);
        const Battle lost = fight(5, 1000.0, 1000.0, 9000, 4000);
        ok(won.lastDefPower > 0 && lost.lastDefPower > 0 &&
               battleTrend(won) != battleTrend(lost),
           "two fights whose last round looks identical are told apart");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
