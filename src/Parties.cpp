#include "Parties.h"

#include <algorithm>
#include <cmath>

namespace odparty {
namespace {

/**
 * The same generator the rest of the game uses for reproducible streams.
 *
 * Not simRand: that is the SIMULATION's stream, and drawing from it here would
 * make the number of parties generated at load time shift every later roll in
 * the game (memory: early-return-skips-rng). This has its own state, seeded
 * per country, so generation is deterministic and invisible to the sim.
 */
struct Mulberry32 {
    uint32_t s;
    explicit Mulberry32(uint32_t seed) : s(seed) {}
    float next() {
        s += 0x6D2B79F5u;
        uint32_t z = s;
        z = (z ^ (z >> 15)) * (z | 1u);
        z ^= z + (z ^ (z >> 7)) * (z | 61u);
        return (float)((z ^ (z >> 14)) >> 8) / (float)(1u << 24);
    }
    /** In [lo, hi). */
    float range(float lo, float hi) { return lo + next() * (hi - lo); }
};

}  // namespace

const char* describeStance(const PoliticalCompass& stance) {
    const float e = stance.economic, s = stance.social;
    const bool left = e < -20.0f, right = e > 20.0f;
    const bool auth = s < -20.0f, lib = s > 20.0f;

    if (left && auth)  return "Workers' Party";
    if (left && lib)   return "Social Democratic Party";
    if (right && auth) return "National Union";
    if (right && lib)  return "Liberal Party";
    if (left)          return "Labour Party";
    if (right)         return "Conservative Party";
    if (auth)          return "Order and Unity Party";
    if (lib)           return "Reform Party";
    return "Centre Party";
}

void renormalise(Legislature& leg) {
    float total = 0.0f;
    for (const Party& p : leg.parties) total += std::max(0.0f, p.support);
    if (total <= 0.0f) {
        // Nobody has any support at all. An equal split is the only answer
        // that keeps the shares a partition; leaving them at zero would make
        // every share-weighted number downstream divide by nothing.
        if (!leg.parties.empty()) {
            const float each = 100.0f / (float)leg.parties.size();
            for (Party& p : leg.parties) p.support = each;
        }
        return;
    }
    for (Party& p : leg.parties) p.support = std::max(0.0f, p.support) / total * 100.0f;
}

void chooseRuling(Legislature& leg) {
    leg.ruling = -1;
    float best = -1.0f;
    for (size_t i = 0; i < leg.parties.size(); ++i) {
        // STRICTLY greater, so a tie keeps the earlier index -- the order the
        // data file lists them in. See the declaration.
        if (leg.parties[i].support > best) {
            best = leg.parties[i].support;
            leg.ruling = (int)i;
        }
    }
}

Legislature generate(const PoliticalCompass& government, uint32_t seed) {
    Mulberry32 rng(seed ? seed : 1u);
    Legislature leg;

    // THREE PARTIES, and the shape is the point rather than the count: the
    // government's own position, an opposition across the centre from it, and
    // a centre party between them. That is enough for a ruling party to lose
    // power to somebody recognisable, which is the only thing generation has
    // to support.
    const float jitterE = rng.range(-12.0f, 12.0f);
    const float jitterS = rng.range(-12.0f, 12.0f);

    Party incumbent;
    incumbent.stance = makeCompass(government.economic + jitterE,
                                   government.social + jitterS);
    incumbent.name = describeStance(incumbent.stance);
    incumbent.support = rng.range(38.0f, 52.0f);

    Party opposition;
    // Mirrored through the centre, not merely offset: the opposition to a
    // left-authoritarian government is right-libertarian, and an offset would
    // have produced two parties on the same side arguing about degree.
    opposition.stance = makeCompass(-government.economic + rng.range(-15.0f, 15.0f),
                                    -government.social + rng.range(-15.0f, 15.0f));
    opposition.name = describeStance(opposition.stance);
    opposition.support = rng.range(25.0f, 38.0f);

    Party centre;
    centre.stance = makeCompass(rng.range(-10.0f, 10.0f), rng.range(-10.0f, 10.0f));
    centre.name = describeStance(centre.stance);
    centre.support = rng.range(10.0f, 22.0f);

    leg.parties = {incumbent, opposition, centre};
    // Short names are the first letters of the description, which is honest
    // about being a description. A generated party has no real abbreviation
    // and inventing one ("GVP", "PNL") would read as a real party's initials.
    for (Party& p : leg.parties) {
        p.shortName.clear();
        bool wordStart = true;
        for (char c : p.name) {
            if (c == ' ') { wordStart = true; continue; }
            if (wordStart && c >= 'A' && c <= 'Z') p.shortName += c;
            wordStart = false;
        }
        p.historical = false;
    }
    renormalise(leg);
    chooseRuling(leg);
    return leg;
}

}  // namespace odparty
