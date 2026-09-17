#pragma once

#include <string>
#include <vector>

#include "GameStructs.h"

/**
 * Who a government answers to.
 *
 * ── WHY THIS EXISTS ──
 *
 * The government compass was a number that only doctrines moved. A country
 * drifted left because you enacted collectivisation, and that was the whole of
 * its politics: there was nobody inside the country with an opinion, so there
 * was nothing to persuade, nothing to lose an argument with, and no reason the
 * compass should ever move on its own.
 *
 * A party is that somebody. Each one stands somewhere on the same compass the
 * government does, holds a share of support, and the one in power PULLS the
 * government toward itself every turn it holds it. So a compass position stops
 * being a setting and becomes the outcome of who won.
 *
 * ── IT PULLS, IT DOES NOT SET ──
 *
 * The ruling party does not teleport the compass to its own stance. It applies
 * a bounded pull per turn, through Game::shiftCountryCompass -- the one place
 * the compass is allowed to move -- so it composes with doctrines instead of
 * fighting them. A government enacting a doctrine its own ruling party
 * disagrees with is a real position, and the compass should show the argument
 * rather than one side winning it by assignment.
 *
 * That also means the mechanic cannot run away: the pull shrinks to nothing as
 * the government arrives at the party's stance, so a long-ruling party settles
 * the country where it stands and stops.
 *
 * ── HISTORICAL NAMES ARE DATA, AND THEIR ABSENCE IS NOT A BLANK ──
 *
 * On a 1914 map the German Empire's parties were the SPD, the Centre Party and
 * the National Liberals, and inventing plausible-sounding German party names
 * instead would be a worse game and a false history. So real parties come from
 * data/parties.json, per scenario and per country.
 *
 * What that file CANNOT be is complete: the shipped maps carry between 55 and
 * 185 countries across six scenario dates, and a fabricated party for a
 * country nobody checked is exactly the fault tools/check_flag_dates.py exists
 * to catch -- Iran flew the Islamic Republic's flag on a 1962 map for the same
 * reason. So a country with no data gets GENERATED parties, marked as
 * generated, and tools/check_party_coverage.py reports the gap per scenario
 * rather than letting it read as finished. Generated names are deliberately
 * descriptive ("Workers' Party", "National Union") rather than invented
 * proper nouns, so nothing in the game claims to be a real party that is not.
 */
namespace odparty {

/** A party's own compass, its support, and where its name came from. */
struct Party {
    std::string name;            ///< "Social Democratic Party of Germany"
    std::string shortName;       ///< "SPD", for a list that has to fit
    PoliticalCompass stance;     ///< where it stands, same axes as a government
    float support = 0.0f;        ///< share of the country, 0..100
    /**
     * This party existed, under this name, on this map's date.
     *
     * False means the name was generated from the stance. The distinction is
     * reported, never hidden: a screen may say "representative parties" for a
     * generated set, and the coverage tool counts them as the backlog.
     */
    bool historical = false;
};

/** Every party in one country, and which of them governs. */
struct Legislature {
    std::vector<Party> parties;
    int ruling = -1;             ///< index into parties, or -1 for none
    /** Whether any party here came from data rather than the generator. */
    bool anyHistorical() const {
        for (const Party& p : parties) if (p.historical) return true;
        return false;
    }
    const Party* rulingParty() const {
        return (ruling >= 0 && ruling < (int)parties.size()) ? &parties[(size_t)ruling] : nullptr;
    }
};

/**
 * How fast a ruling party drags the government toward itself, per turn, as a
 * fraction of the distance still to go.
 *
 * A FRACTION AND NOT A STEP, so the pull dies out as it arrives -- that is
 * what stops a party from pushing the compass past its own stance and
 * oscillating. At 0.02 a party needs about 35 turns to close two thirds of the
 * gap, which is slower than a doctrine's effect and deliberately so: a
 * doctrine is a decision a player made this turn, and a party is the slow
 * weight of who keeps winning.
 */
constexpr float PULL_PER_TURN = 0.02f;

/**
 * A party with less than this share does not steer anything.
 *
 * Without it, a country whose ruling party holds 3% of the legislature would
 * drag the compass exactly as hard as one holding 60%, and a fringe
 * government would be indistinguishable from a mandate.
 */
constexpr float MIN_STEERING_SUPPORT = 10.0f;

/**
 * The compass delta the ruling party applies this turn.
 *
 * Zero when nobody governs, when the ruling party is too small to steer, or
 * when the government already stands where the party does. Pure: no clamping
 * to the compass bounds happens here, because shiftCountryCompass owns that
 * and a second copy of the bound is a second bound (see makeCompass).
 */
inline PoliticalCompass pull(const Legislature& leg, const PoliticalCompass& government) {
    const Party* p = leg.rulingParty();
    if (!p) return {0.0f, 0.0f};
    if (p->support < MIN_STEERING_SUPPORT) return {0.0f, 0.0f};
    // Scaled by the mandate as well as the distance: a party governing on 55%
    // pulls harder than one governing on 12%, which is the difference between
    // a mandate and a coalition of convenience.
    const float mandate = p->support * 0.01f;
    return {(p->stance.economic - government.economic) * PULL_PER_TURN * mandate,
            (p->stance.social   - government.social)   * PULL_PER_TURN * mandate};
}

/**
 * Normalise support so the shares are a partition of the country.
 *
 * The same rule minorities follow (od::renormaliseShares) and for the same
 * reason: these are shares of ONE electorate, so they have to add to 100 or
 * every number computed from them is answering a different question. A
 * legislature whose shares sum to 140 is not a legislature.
 */
void renormalise(Legislature& leg);

/**
 * Pick the ruling party: the largest share.
 *
 * Ties break toward the LOWER index, which is the order the data file lists
 * them in, so a scenario author can decide a tie by writing the intended
 * governing party first. Deterministic, because a tie broken by iteration
 * order over a hash map is a tie broken differently on two machines.
 */
void chooseRuling(Legislature& leg);

/**
 * A representative legislature for a country with no historical data.
 *
 * Built from where the country's government already stands, so a generated set
 * is at least consistent with the world it is in: a government at hard-left
 * authoritarian gets a dominant left party and a small opposition, not a
 * random spread. `seed` makes it deterministic per country -- the same country
 * on the same map generates the same parties every load, which a save file
 * depends on.
 */
Legislature generate(const PoliticalCompass& government, uint32_t seed);

/**
 * A descriptive name for a party standing at `stance`.
 *
 * Deliberately not a proper noun. "Workers' Party" is honest about being a
 * description; "Freedom Alliance of Bolivia" would be a claim about Bolivia.
 */
const char* describeStance(const PoliticalCompass& stance);

}  // namespace odparty
