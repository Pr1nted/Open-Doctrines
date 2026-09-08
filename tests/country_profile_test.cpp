// What a country publishes about itself, and what that is worth.
//
// The rule under test is the one the migration pass reads: published figures
// pull people toward a country, but only when the figures SAY something good.
// Two properties matter more than any particular number.
//
//   1. Publishing a bad figure is worth exactly nothing. Otherwise every
//      country ticks every box on turn one and the decision is not a decision.
//   2. No single field can reach the ceiling on its own. The first version let
//      the expenses term run to 0.7 against a cap of 0.2, and the consequence
//      was measurable: 374 AI decisions in a 60-turn world, mean appeal 0.197,
//      94% of them pinned at the cap. A pull every country gets in full moves
//      nobody anywhere.
//
// Replicated rather than linked, like the other rule tests: reaching the real
// one means building a world. If the rule changes, change it here in the same
// commit.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
static void ok(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { printf("  ok    %s\n", what.c_str()); return; }
    ++g_fails; printf("  FAIL  %s\n", what.c_str());
}
static void section(const char* t) { printf("\n== %s ==\n", t); }

// --- the rule, as Game::disclosureAppealFor has it ---
enum : unsigned { EXPENSES = 1u, DOCTRINES = 2u, TREASURY = 4u, DISTRICT_LAWS = 8u };
static constexpr float CAP            = 0.20f;
static constexpr float EXPENSES_MAX   = 0.07f;
static constexpr float DOCTRINES_MAX  = 0.05f;
static constexpr float TREASURY_MAX   = 0.05f;
static constexpr float DISTRICTS_MAX  = 0.03f;
static constexpr float HARD_SPEND_BAR = 0.35f;

struct Books {
    float total       = 100.0f;  ///< gross income this turn
    float hardSpend   = 20.0f;   ///< army + navy + pacification
    float calmPoints  = 0.0f;    ///< unrest reduction across active doctrines, in points
    double treasury   = 0.0;     ///< held at the start of last turn
    /// Averaged over its districts: unrest a regional law removes (positive) or
    /// adds (negative), plus the growth it brings, in the law file's own units.
    float lawCalm     = 0.0f;
    float lawGrowth   = 0.0f;
};

static float appealFor(const Books& b, unsigned bits) {
    if (bits == 0) return 0.0f;
    if (b.total <= 0.0f) return 0.0f;
    float appeal = 0.0f;
    if (bits & EXPENSES) {
        const float share = b.hardSpend / std::max(1.0f, b.total);
        if (share < HARD_SPEND_BAR)
            appeal += (HARD_SPEND_BAR - share) / HARD_SPEND_BAR * EXPENSES_MAX;
    }
    if (bits & DOCTRINES)
        appeal += std::min(b.calmPoints * 0.02f, DOCTRINES_MAX);
    if (bits & DISTRICT_LAWS) {
        const float good = std::max(0.0f, b.lawCalm) * 0.002f +
                           std::max(0.0f, b.lawGrowth) * 0.002f -
                           std::max(0.0f, -b.lawCalm) * 0.002f;
        appeal += std::clamp(good, 0.0f, DISTRICTS_MAX);
    }
    if (bits & TREASURY) {
        if (b.treasury > 0.0)
            appeal += (float)std::min((double)TREASURY_MAX,
                                      b.treasury / (double)(b.total * 100.0f));
    }
    return std::clamp(appeal, 0.0f, CAP);
}

/// What an AI country decides to publish: a field goes out if it flatters.
static unsigned aiBits(const Books& b) {
    unsigned bits = 0;
    for (unsigned bit : {EXPENSES, DOCTRINES, TREASURY, DISTRICT_LAWS})
        if (appealFor(b, bit) > 0.0f) bits |= bit;
    return bits;
}

// --- the age formatter, as Game::countryAgeText has it ---
static std::string ageText(int foundedTurn, int now) {
    if (foundedTurn < 0) return "session";
    const int months = std::max(0, now - foundedTurn);
    if (months >= 12000) return "millennia";
    if (months >= 12)    return (months % 12 == 0) ? "years" : "years+months";
    return "months";
}

int main() {
    section("publishing a bad figure is worth nothing");
    {
        Books broke;                       // a garrison state, in debt, ungoverned
        broke.hardSpend = 90.0f;           // 90% of income on soldiers and police
        broke.calmPoints = 0.0f;
        broke.treasury = 0.0;
        ok(appealFor(broke, EXPENSES) == 0.0f,  "expenses above the bar earn nothing");
        ok(appealFor(broke, DOCTRINES) == 0.0f, "no calming doctrines earn nothing");
        ok(appealFor(broke, TREASURY) == 0.0f,  "an empty treasury earns nothing");
        broke.lawCalm = -9.0f;             // martial law: it makes things worse
        ok(appealFor(broke, DISTRICT_LAWS) == 0.0f,
           "and a district under martial law earns nothing for saying so");
        ok(appealFor(broke, EXPENSES|DOCTRINES|TREASURY|DISTRICT_LAWS) == 0.0f,
           "and publishing all four at once still earns nothing");
        ok(aiBits(broke) == 0u, "so the AI publishes nothing at all");
    }
    {
        Books b; b.hardSpend = HARD_SPEND_BAR * b.total;  // exactly at the bar
        ok(appealFor(b, EXPENSES) == 0.0f, "the bar itself is not flattering");
    }

    section("no field reaches the ceiling alone");
    {
        Books best;
        best.hardSpend = 0.0f;        // spends nothing on soldiers
        best.calmPoints = 1000.0f;    // absurdly well governed
        best.treasury = 1e9;          // absurdly rich
        best.lawCalm = 50.0f;         // and absurdly well legislated
        best.lawGrowth = 50.0f;
        ok(appealFor(best, EXPENSES)  <= EXPENSES_MAX  + 1e-6f, "expenses capped at its own share");
        ok(appealFor(best, DOCTRINES) <= DOCTRINES_MAX + 1e-6f, "doctrines capped at its own share");
        ok(appealFor(best, TREASURY)  <= TREASURY_MAX  + 1e-6f, "treasury capped at its own share");
        ok(appealFor(best, DISTRICT_LAWS) <= DISTRICTS_MAX + 1e-6f, "district law capped too");
        ok(EXPENSES_MAX + DOCTRINES_MAX + TREASURY_MAX + DISTRICTS_MAX == CAP,
           "and the four shares sum to exactly the ceiling");
        ok(std::abs(appealFor(best, EXPENSES|DOCTRINES|TREASURY|DISTRICT_LAWS) - CAP) < 1e-6f,
           "so only a frugal, calm, solvent AND well-governed country reaches it");
        ok(appealFor(best, EXPENSES) < CAP,  "being frugal alone does not");
        ok(appealFor(best, TREASURY) < CAP,  "being rich alone does not");
    }

    section("more of a good figure is worth more (ordering, not magnitude)");
    {
        // Monotone in each input, which is the property the migration pass
        // relies on -- improving the country must never lower its pull.
        float prev = -1.0f;
        for (int spend = 90; spend >= 0; spend -= 10) {
            Books b; b.hardSpend = (float)spend;
            const float a = appealFor(b, EXPENSES);
            ok(a >= prev, "spending less on hard power never lowers the pull (" +
                          std::to_string(spend) + "%)");
            prev = a;
        }
        prev = -1.0f;
        for (double t : {0.0, 100.0, 1000.0, 5000.0, 1e6}) {
            Books b; b.treasury = t;
            const float a = appealFor(b, TREASURY);
            ok(a >= prev, "holding more never lowers the pull");
            prev = a;
        }
    }

    section("adding a field is never a loss");
    {
        // The player's tick boxes are free to tick in any order; a worthless
        // field must be inert rather than dilutive.
        Books b; b.hardSpend = 10.0f; b.treasury = 0.0; b.calmPoints = 0.0f;
        const float alone = appealFor(b, EXPENSES);
        ok(appealFor(b, EXPENSES|TREASURY) == alone,
           "publishing an empty treasury beside good expenses changes nothing");
        ok(appealFor(b, EXPENSES|DOCTRINES) == alone,
           "publishing no doctrines beside good expenses changes nothing");
        ok(aiBits(b) == EXPENSES, "and the AI publishes only the field that flatters");
    }

    section("the AI picks exactly the flattering fields");
    {
        Books rich;  rich.hardSpend = 80.0f; rich.treasury = 1e6;
        ok(aiBits(rich) == TREASURY, "a rich garrison state publishes its treasury only");
        Books calm;  calm.hardSpend = 80.0f; calm.calmPoints = 5.0f;
        ok(aiBits(calm) == DOCTRINES, "a well-governed garrison state publishes its doctrines only");
        Books all;   all.hardSpend = 5.0f; all.calmPoints = 5.0f; all.treasury = 1e5;
        all.lawCalm = 5.0f;
        ok(aiBits(all) == (EXPENSES|DOCTRINES|TREASURY|DISTRICT_LAWS),
           "a country with nothing to hide hides nothing");
        Books harsh; harsh.hardSpend = 5.0f; harsh.lawCalm = -9.0f;
        ok((aiBits(harsh) & DISTRICT_LAWS) == 0u,
           "a country governing its districts harshly keeps that to itself");
    }

    section("an age is said in the largest unit that still means something");
    {
        ok(ageText(-1, 500) == "session",       "a country on the map at the start has no age");
        ok(ageText(100, 100) == "months",       "founded this turn");
        ok(ageText(100, 111) == "months",       "eleven months is still months");
        ok(ageText(100, 112) == "years",        "twelve is a year");
        ok(ageText(100, 115) == "years+months", "and thirteen is a year and a month");
        ok(ageText(0, 12000) == "millennia",    "a thousand years is millennia");
        ok(ageText(500, 100) == "months",       "a founding in the future is not negative");
    }

    printf("\n%d check(s), %d failure(s)\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
