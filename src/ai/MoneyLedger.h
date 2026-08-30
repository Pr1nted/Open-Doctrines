#pragma once

// Where the money actually goes.
//
// WHY THIS EXISTS
//
// Measured 2026-08-20, after sixty-six maps of self-play: every worker in the
// pool converged on one of two behaviours, and both lose. Two of the three
// spent themselves insolvent -- 63 and 100 turns bankrupt per run against a
// random control's 24 -- and the third stayed perfectly solvent by doing
// nothing at all (recruit 1.3%, attack 0.0%, declare war 0.2%). All three
// finished at an ADVANTAGE of 0.02 against a starting model's 1.96.
//
// That is not a war-module problem, which is what the previous week of work
// assumed. It says there is no policy in the reachable space that both acts and
// pays for itself. Before changing a single constant to fix that, it is worth
// knowing WHICH act is the one that cannot be paid for -- and nothing in the
// game could answer that, because the recurring costs are itemised in
// CountryIncomeSnapshot while every one-off purchase just decrements a treasury
// at nine different call sites and leaves no trace.
//
// So: one line per channel, income and outgoings, totalled over a run and
// divided by country-turns so runs of different lengths compare.
//
// OFF BY DEFAULT AND FREE WHEN OFF. `g_on` is read once from OD_MONEY_LEDGER at
// static-init; every add() behind it is a predictable branch on a hot path that
// already does far more work than that. It is a diagnostic, not telemetry: it
// aggregates across all countries and keeps no per-country state, so it says
// nothing about who went broke, only about what the money was spent on.

#include <cstdio>
#include <cstdlib>

namespace money {

enum Channel {
    // ── in ──
    INCOME_INDUSTRY = 0,
    INCOME_RESOURCE,
    INCOME_POP,
    // ── out, every turn, whether or not anybody decided anything ──
    LOST_BLOCKADE,
    UPKEEP_ARMY,
    UPKEEP_NAVY,
    UPKEEP_POLICY,
    UPKEEP_INDUSTRY,
    UPKEEP_MINORITY,
    BUDGET_RESEARCH,
    BUDGET_PACIFICATION,
    // ── out, because a decision was taken ──
    BUY_TROOPS,
    BUY_SHIP,
    BUY_INDUSTRY,
    BUY_FORT,
    BUY_PORT,
    BUY_SPECIALIZE,
    BUY_ARTILLERY,
    BUY_BOMBARD,
    PAY_CEASEFIRE,
    // ── the shortfall that is written off when a treasury goes below zero.
    //     Not a payment: money the country did not have and never paid. It is
    //     here because it is the size of the hole, which is the number the
    //     bankruptcy cascade is trying to close. ──
    WRITTEN_OFF,
    CHANNEL_COUNT
};

inline const char* channelName(int c) {
    switch (c) {
        case INCOME_INDUSTRY:     return "income: industry";
        case INCOME_RESOURCE:     return "income: resources";
        case INCOME_POP:          return "income: population";
        case LOST_BLOCKADE:       return "lost: blockaded trade";
        case UPKEEP_ARMY:         return "upkeep: army";
        case UPKEEP_NAVY:         return "upkeep: navy";
        case UPKEEP_POLICY:       return "upkeep: doctrines";
        case UPKEEP_INDUSTRY:     return "upkeep: industry";
        case UPKEEP_MINORITY:     return "upkeep: minority settlements";
        case BUDGET_RESEARCH:     return "budget: research";
        case BUDGET_PACIFICATION: return "budget: pacification";
        case BUY_TROOPS:          return "bought: troops";
        case BUY_SHIP:            return "bought: ships";
        case BUY_INDUSTRY:        return "bought: industry";
        case BUY_FORT:            return "bought: fortification";
        case BUY_PORT:            return "bought: ports";
        case BUY_SPECIALIZE:      return "bought: specialisation";
        case BUY_ARTILLERY:       return "bought: artillery fire";
        case BUY_BOMBARD:         return "bought: naval bombardment";
        case PAY_CEASEFIRE:       return "paid: ceasefire indemnities";
        case WRITTEN_OFF:         return "WRITTEN OFF (bankrupt shortfall)";
        default:                  return "?";
    }
}

struct Entry {
    double total = 0;      ///< signed: income positive, spending negative
    long long hits = 0;    ///< how many times this channel fired
};

inline Entry     g_entry[CHANNEL_COUNT];
inline long long g_countryTurns = 0;
inline bool      g_on = std::getenv("OD_MONEY_LEDGER") != nullptr;

/// `amount` is SIGNED. Income is positive, everything spent is negative, so the
/// column sums to the change in treasury and a mis-signed channel shows up as
/// an arithmetic error rather than hiding.
inline void add(Channel c, double amount) {
    if (!g_on) return;
    g_entry[c].total += amount;
    ++g_entry[c].hits;
}

/// One country resolved one turn. The denominator for every per-turn figure.
inline void tick() {
    if (g_on) ++g_countryTurns;
}

inline void dump() {
    if (!g_on) return;
    const double ct = (g_countryTurns > 0) ? (double)g_countryTurns : 1.0;
    double in = 0, out = 0;
    for (int c = 0; c < CHANNEL_COUNT; ++c) {
        if (c == WRITTEN_OFF) continue;         // not a payment; see the enum
        (g_entry[c].total >= 0 ? in : out) += g_entry[c].total;
    }
    printf("\n[MONEY] %lld country-turns. Per country-turn, and as a share of all\n"
           "[MONEY] spending -- so a channel's share is what cutting it would free.\n",
           g_countryTurns);
    printf("[MONEY] %-34s %12s %10s %12s\n", "channel", "per c-turn", "share", "fired");
    for (int c = 0; c < CHANNEL_COUNT; ++c) {
        const Entry& e = g_entry[c];
        if (e.hits == 0) continue;
        const bool spend = (e.total < 0);
        const double share = (spend && out < 0) ? 100.0 * e.total / out : 0.0;
        printf("[MONEY] %-34s %12.4f %9.1f%% %12lld\n",
               channelName(c), e.total / ct, share, e.hits);
    }
    printf("[MONEY] %-34s %12.4f\n", "TOTAL IN", in / ct);
    printf("[MONEY] %-34s %12.4f\n", "TOTAL OUT", out / ct);
    printf("[MONEY] %-34s %12.4f\n", "NET", (in + out) / ct);
    printf("[MONEY] %-34s %12.4f %9s %12lld\n", channelName(WRITTEN_OFF),
           g_entry[WRITTEN_OFF].total / ct, "-", g_entry[WRITTEN_OFF].hits);
}

}  // namespace money
