// The monument rules: the catalogue, what a slot costs, and what one gives.
//
// Everything here is pure arithmetic over src/Monuments.h, so it runs without a
// window, a map or a save. The half that needs a world -- which province a
// monument stands in, whether it may be built there, and whether its effect
// reaches the next province -- belongs to the game layer and is tested with it.
//
// Build target: MonumentsTest. Non-zero exit means a case failed.

#include "Monuments.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

namespace {

int g_checks = 0, g_failed = 0;

void ok(bool cond, const std::string& what, const std::string& detail = {}) {
    ++g_checks;
    printf("  %-66s %s", what.c_str(), cond ? "ok" : "FAILED");
    if (!cond && !detail.empty()) printf("  --  %s", detail.c_str());
    printf("\n");
    if (!cond) ++g_failed;
}

void section(const char* name) { printf("\n== %s ==\n", name); }

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

}  // namespace

int main() {
    printf("Monuments\n");

    section("the catalogue is complete and nothing in it collides");
    {
        std::set<std::string> keys, names, nodes;
        bool allKeyed = true, allNamed = true, allBlurbed = true;
        for (int i = 0; i < odmon::kKindCount; ++i) {
            const odmon::Kind k = (odmon::Kind)i;
            const std::string key = odmon::kindKey(k);
            if (key.empty()) allKeyed = false;
            if (std::string(odmon::kindName(k)).empty()) allNamed = false;
            if (std::string(odmon::kindBlurb(k)).empty()) allBlurbed = false;
            keys.insert(key);
            names.insert(odmon::kindName(k));
            nodes.insert(odmon::unlockNode(k));
            // A row that was never filled in would come back as the fallback,
            // whose maxLevel is the default rather than this kind's.
            ok(odmon::spec(k).kind == k,
               std::string("the catalogue row for ") + key + " is its own");
        }
        ok(allKeyed, "every kind has a save key");
        ok(allNamed, "and a name");
        ok(allBlurbed, "and a line saying what it does");
        ok((int)keys.size() == odmon::kKindCount, "no two kinds share a save key");
        ok((int)names.size() == odmon::kKindCount, "nor a name");
        ok((int)nodes.size() == odmon::kKindCount, "nor a research node");

        // Out of range must not read past the array. A save from a newer build
        // is exactly how a bad Kind arrives.
        ok(odmon::spec((odmon::Kind)99).maxLevel > 0, "an unknown kind gives a safe row");
        ok(std::string(odmon::kindKey((odmon::Kind)99)).empty(), "and no key");
        ok(odmon::scaledEffect((odmon::Kind)99, 0.5f, 3) == 0.0f, "and no effect");
    }

    section("what a slot costs");
    {
        // The series that was asked for, verbatim. A table would stop wherever
        // somebody stopped typing; this is the formula, so the eighth slot has
        // a price too.
        ok(near(odmon::slotCost(1), 50.0f),  "the first slot is 50");
        ok(near(odmon::slotCost(2), 75.0f),  "the second is 75");
        ok(near(odmon::slotCost(3), 125.0f), "the third is 125");
        ok(near(odmon::slotCost(4), 200.0f), "the fourth is 200");
        ok(near(odmon::slotCost(5), 300.0f), "the fifth is 300");
        ok(near(odmon::slotCost(6), 425.0f), "and the sixth is 425");
        ok(odmon::slotCost(0) == 0.0f && odmon::slotCost(-3) == 0.0f,
           "a slot that does not exist costs nothing");

        // Rising by more each time is the whole economy: a flat or linear
        // series makes the eleventh monument as easy as the second.
        bool accelerating = true;
        for (int n = 2; n < 12; ++n) {
            const float d1 = odmon::slotCost(n) - odmon::slotCost(n - 1);
            const float d2 = odmon::slotCost(n + 1) - odmon::slotCost(n);
            if (!(d2 > d1)) accelerating = false;
        }
        ok(accelerating, "each slot costs more MORE than the one before it");

        ok(near(odmon::slotUpkeep(4), 450.0f), "running four costs 450 a turn");
        ok(odmon::slotUpkeep(0) == 0.0f, "and running none costs nothing");
    }

    section("what a monument costs to put up and to take down");
    {
        const odmon::Kind uni = odmon::Kind::University;
        ok(odmon::levelCost(uni, 0) == odmon::spec(uni).buildCost,
           "level 0 is the build price");
        ok(odmon::levelCost(uni, 1) > 0.0f, "level 1 buys level 2");
        ok(odmon::levelCost(uni, odmon::spec(uni).maxLevel) == 0.0f,
           "and there is nothing to buy at the top");

        // The silo is the expensive one, by request: "around 200 to upgrade
        // from level 1 to level 2".
        ok(near(odmon::levelCost(odmon::Kind::MissileSilo, 1), 200.0f),
           "a silo's second level costs 200");
        ok(odmon::levelCost(odmon::Kind::MissileSilo, 2) >
               odmon::levelCost(odmon::Kind::MissileSilo, 1),
           "and its third costs more than its second");

        ok(odmon::kDismantleCost == 50.0f, "taking one down costs 50");
    }

    section("only two of them move, and moving one is a decision");
    {
        int movable = 0;
        for (int i = 0; i < odmon::kKindCount; ++i)
            if (odmon::spec((odmon::Kind)i).movable) ++movable;
        ok(movable == 2, "exactly two kinds can be moved",
           "got " + std::to_string(movable));

        // Both of them are destroyed by heavy ordnance. That is the trade:
        // they go where they are needed and they do not survive being found.
        bool bothFragile = true;
        for (int i = 0; i < odmon::kKindCount; ++i) {
            const odmon::Spec& s = odmon::spec((odmon::Kind)i);
            if (s.movable && !s.fragile) bothFragile = false;
        }
        ok(bothFragile, "and both of the movable ones are destroyed by ordnance");

        const odmon::Kind dc = odmon::Kind::DefenceCorporation;
        ok(odmon::moveCost(dc, 1) > 0.0f, "moving one costs money");
        ok(odmon::moveCost(dc, 3) > odmon::moveCost(dc, 1),
           "and moving a big one costs more than moving a small one");
        ok(odmon::moveCost(odmon::Kind::University, 3) == 0.0f,
           "a university cannot be moved at any price");
    }

    section("what one gives, and why the tenth is not worth ten");
    {
        const odmon::Kind uni = odmon::Kind::University;
        const float small = odmon::scaledEffect(uni, 0.01f, 1);
        const float big   = odmon::scaledEffect(uni, 0.30f, 1);
        ok(near(small, odmon::spec(uni).effectMin, 0.02f),
           "in an empty province it is worth about the low end");
        ok(big > small, "and in the country's biggest one, much more");
        ok(big <= odmon::spec(uni).effectMax + 1e-4f,
           "never more than the high end at level 1");

        // Saturates, or the rule would only be rewarding maps where one
        // province holds everything.
        ok(near(odmon::scaledEffect(uni, 0.34f, 1), odmon::scaledEffect(uni, 0.99f, 1)),
           "a third of the country and all of it are worth the same");

        ok(odmon::scaledEffect(uni, 0.30f, 2) > big, "a level raises it");
        ok(odmon::scaledEffect(uni, 0.30f, 0) == 0.0f, "and level 0 gives nothing");

        // THE ANSWER TO "ridiculous amounts of RP". Five universities are
        // worth about 2.28 of one, not five, so building more stops being the
        // answer to everything.
        float stack = 0.0f;
        for (int rank = 1; rank <= 5; ++rank)
            stack += odmon::stackedEffect(uni, 0.30f, 1, rank);
        ok(stack < 3.0f * big, "five of them are worth less than three of one",
           "stack " + std::to_string(stack) + " vs one " + std::to_string(big));
        ok(near(odmon::stackedEffect(uni, 0.30f, 1, 2), big / 2.0f),
           "the second is worth half");
        ok(near(odmon::stackedEffect(uni, 0.30f, 1, 3), big / 3.0f),
           "the third a third");
        ok(odmon::stackedEffect(uni, 0.30f, 1, 0) == 0.0f,
           "and a rank that does not exist is worth nothing");
    }

    section("how far one reaches");
    {
        ok(odmon::radius(odmon::Kind::University, 1) == 0,
           "a university is felt nationally, so it has no radius of its own");
        ok(odmon::radius(odmon::Kind::AirDefence, 1) == 1,
           "air defence covers its neighbours at level 1");
        ok(odmon::radius(odmon::Kind::AirDefence, 3) == 3, "and further at level 3");
        ok(odmon::radius(odmon::Kind::AirDefence, 0) == 0, "nothing reaches at level 0");

        // The silo is the one measured in kilometres, because
        // "intercontinental" is not a number of borders.
        ok(odmon::siloRangeKm(1) > 0.0f, "a silo reaches a theatre at level 1");
        ok(odmon::siloRangeKm(2) > odmon::siloRangeKm(1), "an ocean at level 2");
        ok(odmon::siloRangeKm(3) > 20015.0f,
           "and at level 3 further than half way round the planet, so the "
           "curve stops mattering");
        ok(odmon::siloRangeKm(0) == 0.0f, "an unbuilt silo reaches nowhere");
    }

    section("which monument pays the cheap slot");
    {
        std::vector<odmon::Holding> all = {
            {10, odmon::Kind::University,          1, true},
            {20, odmon::Kind::FactoryConglomerate, 3, true},
            {30, odmon::Kind::Megacity,            2, false},   // switched off
            {40, odmon::Kind::AirDefence,          3, true},
        };
        ok(odmon::activeCount(all) == 3, "three of the four are running");

        const std::vector<odmon::Holding> order = odmon::chargeOrder(all);
        ok(order.size() == all.size(), "every one is in the order");
        ok(order.back().provinceId == 30,
           "the inactive one is last, because it is not paying for a slot");
        ok(order.front().level == 3,
           "and the cheapest slot goes to the biggest investment");

        // Determinism: two level-3s, and the order must not depend on what the
        // map handed over. Same input reversed, same answer.
        std::vector<odmon::Holding> reversed(all.rbegin(), all.rend());
        const std::vector<odmon::Holding> again = odmon::chargeOrder(reversed);
        bool same = again.size() == order.size();
        for (size_t i = 0; same && i < order.size(); ++i)
            if (again[i].provinceId != order[i].provinceId) same = false;
        ok(same, "the same holdings in another order charge the same way");

        ok(odmon::chargeOrder({}).empty() && odmon::activeCount({}) == 0,
           "a country with none of them is not a special case");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
