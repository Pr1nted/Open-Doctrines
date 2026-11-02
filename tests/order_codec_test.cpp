// sim::Order's byte encoding, every kind, both ways.
//
//   OrderCodecTest
//
// WHAT IT PINS
//
// sim::Order (src/sim/Order.h) is the one type the rework's move generation,
// trace and search will pass orders around as, and its bytes are what a trace
// stores and a search will hash. So:
//
//   - every kind survives encode -> decode with every field intact, set to
//     values that are NOT the struct's defaults (a field the codec forgot
//     would otherwise come back as its default and look right);
//   - the tags are the pinned OrderKind numbers, not the variant index, so a
//     reordering of the variant cannot silently change what was written;
//   - a list round-trips, and decodeOrders insists on consuming every byte;
//   - every truncation of a valid encoding is refused rather than read past,
//     and so is an unknown tag or a recruitment of a kind that does not exist.
//
// Links only Order.cpp. If a Pending* struct gains a field, this test does not
// know -- add it to the encoding AND to the value below in the same commit.

#include "sim/Order.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

static int g_checks = 0, g_failed = 0;
static void ok(bool c, const std::string& w) {
    ++g_checks; if (c) return; ++g_failed; printf("  FAIL  %s\n", w.c_str());
}
static void section(const char* n) { printf("\n== %s ==\n", n); }

// Field-by-field equality: the Pending* structs have no operator==, and giving
// them one in GameStructs.h for a test's sake would be a change to the game.
static bool same(const sim::Order& a, const sim::Order& b) {
    if (a.index() != b.index()) return false;
    return std::visit([&b](const auto& x) -> bool {
        using T = std::decay_t<decltype(x)>;
        const T& y = std::get<T>(b);
        if constexpr (std::is_same_v<T, PendingDiplomaticAction>)
            return x.sourceIso == y.sourceIso && x.targetIso == y.targetIso && x.action == y.action &&
                   x.turnsRemaining == y.turnsRemaining && x.subjectIso == y.subjectIso &&
                   x.statedGoal == y.statedGoal;
        else if constexpr (std::is_same_v<T, PendingUpgrade>)
            return x.provinceId == y.provinceId && x.type == y.type &&
                   x.targetLevel == y.targetLevel && x.turnsRemaining == y.turnsRemaining;
        else if constexpr (std::is_same_v<T, PendingSpecialization>)
            return x.provinceId == y.provinceId && x.specialization == y.specialization &&
                   x.turnsRemaining == y.turnsRemaining;
        else if constexpr (std::is_same_v<T, PendingRecruitment>)
            return x.provinceId == y.provinceId && x.count == y.count &&
                   x.turnsRemaining == y.turnsRemaining && x.type == y.type;
        else if constexpr (std::is_same_v<T, PendingMoveOrder>)
            return x.fromProvince == y.fromProvince && x.toProvince == y.toProvince &&
                   x.pct == y.pct && x.countryId == y.countryId && x.troopType == y.troopType;
        else if constexpr (std::is_same_v<T, PendingDisbandOrder>)
            return x.provinceId == y.provinceId && x.count == y.count;
        else if constexpr (std::is_same_v<T, sim::WithdrawOrder>)
            return x.provinceId == y.provinceId;
        else if constexpr (std::is_same_v<T, PendingShipBuild>)
            return x.provinceId == y.provinceId && x.type == y.type && x.turnsRemaining == y.turnsRemaining;
        else if constexpr (std::is_same_v<T, PendingScrapShip>)
            return x.shipIndex == y.shipIndex;
        else if constexpr (std::is_same_v<T, PendingEmbark>)
            return x.provinceId == y.provinceId && x.count == y.count && x.turnsRemaining == y.turnsRemaining;
        else if constexpr (std::is_same_v<T, PendingArtilleryOrder>)
            return x.fromProvince == y.fromProvince && x.targetProvince == y.targetProvince &&
                   x.ammoType == y.ammoType;
        else if constexpr (std::is_same_v<T, PendingShipMoveOrder>)
            return x.shipIndex == y.shipIndex && x.destLon == y.destLon && x.destLat == y.destLat &&
                   x.route == y.route && x.planned == y.planned && x.destProvince == y.destProvince;
        else if constexpr (std::is_same_v<T, PendingShipEngageOrder>)
            return x.shipIndex == y.shipIndex && x.targetIndex == y.targetIndex;
        else if constexpr (std::is_same_v<T, PendingShipBombardOrder>)
            return x.shipIndex == y.shipIndex && x.targetProvince == y.targetProvince &&
                   x.ammoType == y.ammoType;
        else
            return x.shipIndex == y.shipIndex && x.targetProvince == y.targetProvince;
    }, a);
}

// One of every kind, every field away from its default.
static std::vector<sim::Order> everyKind() {
    std::vector<sim::Order> v;
    { PendingDiplomaticAction x; x.sourceIso = "FRA"; x.targetIso = "GER"; x.action = "call_to_arms";
      x.turnsRemaining = 3; x.subjectIso = "AUT"; x.statedGoal = 2; v.push_back(x); }
    { PendingUpgrade x; x.provinceId = 812; x.type = "fortification"; x.targetLevel = 4; x.turnsRemaining = 2; v.push_back(x); }
    { PendingSpecialization x; x.provinceId = 77; x.specialization = "steel"; x.turnsRemaining = 5; v.push_back(x); }
    { PendingRecruitment x; x.provinceId = 9; x.count = 120000; x.turnsRemaining = 2;
      x.type = (TroopType)((int)TROOP_TYPE_COUNT - 1); v.push_back(x); }
    { PendingMoveOrder x; x.fromProvince = 10; x.toProvince = 11; x.pct = 73; x.countryId = 44;
      x.troopType = 1; v.push_back(x); }
    { PendingDisbandOrder x; x.provinceId = 5; x.count = 999; v.push_back(x); }
    { sim::WithdrawOrder x; x.provinceId = 1234; v.push_back(x); }
    { PendingShipBuild x; x.provinceId = 300; x.type = "carrier"; x.turnsRemaining = 1; v.push_back(x); }
    { PendingScrapShip x; x.shipIndex = 17; v.push_back(x); }
    { PendingEmbark x; x.provinceId = 301; x.count = 4500; x.turnsRemaining = 2; v.push_back(x); }
    { PendingArtilleryOrder x; x.fromProvince = 1; x.targetProvince = 2; x.ammoType = "gas"; v.push_back(x); }
    { PendingShipMoveOrder x; x.shipIndex = 3; x.destLon = -12.345678901; x.destLat = 51.0000001;
      x.route = {{-1.5, 50.25}, {-7.0, 49.0}, {-12.345678901, 51.0000001}};
      x.planned = true; x.destProvince = 640; v.push_back(x); }
    { PendingShipEngageOrder x; x.shipIndex = 4; x.targetIndex = 8; v.push_back(x); }
    { PendingShipBombardOrder x; x.shipIndex = 6; x.targetProvince = 90; x.ammoType = "he"; v.push_back(x); }
    { PendingShipDisembark x; x.shipIndex = 2; x.targetProvince = 91; v.push_back(x); }
    // Negative and extreme values, which an encoding that treats ints as
    // unsigned or narrows them would mangle.
    { PendingMoveOrder x; x.fromProvince = -1; x.toProvince = 2147483647; x.pct = 0;
      x.countryId = -2147483647 - 1; x.troopType = -1; v.push_back(x); }
    // An empty voyage and empty strings.
    { PendingShipMoveOrder x; v.push_back(x); }
    { PendingDiplomaticAction x; v.push_back(x); }
    return v;
}

int main() {
    const std::vector<sim::Order> all = everyKind();

    section("every kind is covered");
    {
        bool seen[sim::kOrderKindCount + 1] = {};
        for (const auto& o : all) seen[(int)sim::orderKind(o)] = true;
        for (int k = 1; k <= sim::kOrderKindCount; ++k)
            ok(seen[k], std::string("kind ") + std::to_string(k) + " (" +
                        sim::orderKindName((sim::OrderKind)k) + ") has a case here");
        ok(std::variant_size_v<sim::Order> == (size_t)sim::kOrderKindCount,
           "kOrderKindCount matches the variant");
    }

    section("one order at a time");
    for (const auto& o : all) {
        std::vector<uint8_t> b;
        sim::encodeOrder(o, b);
        const std::string name = sim::orderKindName(sim::orderKind(o));
        ok(!b.empty() && b[0] == (uint8_t)sim::orderKind(o), name + ": first byte is the pinned tag");
        sim::Order back;
        size_t pos = 0;
        ok(sim::decodeOrder(b.data(), b.size(), pos, back), name + ": decodes");
        ok(pos == b.size(), name + ": consumes exactly its bytes");
        ok(same(o, back), name + ": every field survives");

        // Every proper prefix is a truncated order and must be refused.
        bool allRefused = true;
        for (size_t cut = 0; cut < b.size(); ++cut) {
            sim::Order junk;
            size_t p = 0;
            if (sim::decodeOrder(b.data(), cut, p, junk)) allRefused = false;
        }
        ok(allRefused, name + ": every truncation is refused");
    }

    section("pinned tags");
    {
        // A few by number, so renumbering OrderKind cannot pass unnoticed.
        std::vector<uint8_t> b;
        sim::encodeOrder(sim::Order(PendingMoveOrder{}), b);
        ok(b[0] == 5, "move orders are tag 5");
        b.clear();
        sim::encodeOrder(sim::Order(PendingShipDisembark{}), b);
        ok(b[0] == 15, "disembarks are tag 15");
        b.clear();
        sim::encodeOrder(sim::Order(PendingDiplomaticAction{}), b);
        ok(b[0] == 1, "diplomatic actions are tag 1");
    }

    section("a whole list");
    {
        std::vector<uint8_t> b;
        sim::encodeOrders(all, b);
        std::vector<sim::Order> back;
        ok(sim::decodeOrders(b.data(), b.size(), back), "list decodes");
        ok(back.size() == all.size(), "same count");
        bool allSame = back.size() == all.size();
        for (size_t i = 0; allSame && i < all.size(); ++i) allSame = same(all[i], back[i]);
        ok(allSame, "same orders, same order");

        std::vector<uint8_t> extra = b;
        extra.push_back(0);
        ok(!sim::decodeOrders(extra.data(), extra.size(), back), "trailing bytes are refused");

        std::vector<uint8_t> empty;
        sim::encodeOrders({}, empty);
        ok(sim::decodeOrders(empty.data(), empty.size(), back) && back.empty(), "empty list");

        // A count far beyond what the bytes can hold is refused up front.
        std::vector<uint8_t> liar = {0xff, 0xff, 0xff, 0x7f, 9, 0, 0, 0, 0};
        ok(!sim::decodeOrders(liar.data(), liar.size(), back), "impossible count is refused");
    }

    section("hostile bytes");
    {
        sim::Order junk;
        size_t p = 0;
        const uint8_t zero[] = {0, 0, 0, 0, 0};
        ok(!sim::decodeOrder(zero, sizeof zero, p, junk), "tag 0 is refused");
        p = 0;
        const uint8_t high[] = {200, 0, 0, 0, 0};
        ok(!sim::decodeOrder(high, sizeof high, p, junk), "an unknown tag is refused");

        // A recruitment of a troop kind that does not exist.
        PendingRecruitment r; r.provinceId = 1; r.count = 1;
        std::vector<uint8_t> b;
        sim::encodeOrder(r, b);
        b[b.size() - 4] = 0x7f;              // the type's low byte
        p = 0;
        ok(!sim::decodeOrder(b.data(), b.size(), p, junk), "an out-of-range troop type is refused");

        // A string claiming more bytes than there are.
        PendingShipBuild sb; sb.type = "destroyer";
        b.clear();
        sim::encodeOrder(sb, b);
        b[5] = 0xff; b[6] = 0xff;            // the type string's length
        p = 0;
        ok(!sim::decodeOrder(b.data(), b.size(), p, junk), "an over-long string is refused");

        // A voyage claiming more legs than there are bytes.
        PendingShipMoveOrder sm;
        b.clear();
        sim::encodeOrder(sm, b);
        b[21] = 0xff; b[22] = 0xff;          // tag 1 + ship 4 + lon 8 + lat 8 = leg count at 21
        p = 0;
        ok(!sim::decodeOrder(b.data(), b.size(), p, junk), "an impossible route length is refused");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
