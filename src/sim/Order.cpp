// The byte encoding of sim::Order. See Order.h for why it exists and why it is
// not the multiplayer wire. Deliberately free of Game.h, so the codec test can
// link this file alone.
#include "Order.h"

#include <cstring>
#include <type_traits>

namespace sim {

namespace {

// ── Writers: explicit little-endian, never memcpy of a struct ──
void putU8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }
void putU32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back((uint8_t)(v >> (8 * i)));
}
void putI32(std::vector<uint8_t>& o, int v) { putU32(o, (uint32_t)v); }
void putF64(std::vector<uint8_t>& o, double d) {
    uint64_t bits;
    static_assert(sizeof bits == sizeof d, "double is not 64-bit");
    std::memcpy(&bits, &d, sizeof bits);
    for (int i = 0; i < 8; ++i) o.push_back((uint8_t)(bits >> (8 * i)));
}
void putStr(std::vector<uint8_t>& o, const std::string& s) {
    putU32(o, (uint32_t)s.size());
    o.insert(o.end(), s.begin(), s.end());
}

// ── Reader: every get checks the bytes are there first ──
struct Reader {
    const uint8_t* d;
    size_t n;
    size_t& p;
    bool ok = true;
    bool need(size_t k) {
        if (!ok || k > n || p > n - k) ok = false;
        return ok;
    }
    uint8_t u8() { if (!need(1)) return 0; return d[p++]; }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= (uint32_t)d[p + i] << (8 * i);
        p += 4;
        return v;
    }
    int i32() { return (int)u32(); }
    double f64() {
        if (!need(8)) return 0.0;
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) bits |= (uint64_t)d[p + i] << (8 * i);
        p += 8;
        double v;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }
    std::string str() {
        const uint32_t len = u32();
        if (!need(len)) return std::string();
        std::string s((const char*)d + p, (size_t)len);
        p += len;
        return s;
    }
};

} // namespace

OrderKind orderKind(const Order& o) {
    return std::visit([](const auto& x) -> OrderKind {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, PendingDiplomaticAction>) return OrderKind::Diplomatic;
        else if constexpr (std::is_same_v<T, PendingUpgrade>)        return OrderKind::Upgrade;
        else if constexpr (std::is_same_v<T, PendingSpecialization>) return OrderKind::Specialization;
        else if constexpr (std::is_same_v<T, PendingRecruitment>)    return OrderKind::Recruitment;
        else if constexpr (std::is_same_v<T, PendingMoveOrder>)      return OrderKind::Move;
        else if constexpr (std::is_same_v<T, PendingDisbandOrder>)   return OrderKind::Disband;
        else if constexpr (std::is_same_v<T, WithdrawOrder>)         return OrderKind::Withdraw;
        else if constexpr (std::is_same_v<T, PendingShipBuild>)      return OrderKind::ShipBuild;
        else if constexpr (std::is_same_v<T, PendingScrapShip>)      return OrderKind::ScrapShip;
        else if constexpr (std::is_same_v<T, PendingEmbark>)         return OrderKind::Embark;
        else if constexpr (std::is_same_v<T, PendingArtilleryOrder>) return OrderKind::Artillery;
        else if constexpr (std::is_same_v<T, PendingShipMoveOrder>)  return OrderKind::ShipMove;
        else if constexpr (std::is_same_v<T, PendingShipEngageOrder>) return OrderKind::ShipEngage;
        else if constexpr (std::is_same_v<T, PendingShipBombardOrder>) return OrderKind::ShipBombard;
        else { static_assert(std::is_same_v<T, PendingShipDisembark>, "unhandled Order kind");
               return OrderKind::ShipDisembark; }
    }, o);
}

const char* orderKindName(OrderKind k) {
    switch (k) {
        case OrderKind::Diplomatic:     return "pendingDiplomaticActions";
        case OrderKind::Upgrade:        return "pendingUpgrades";
        case OrderKind::Specialization: return "pendingSpecializations";
        case OrderKind::Recruitment:    return "pendingRecruitments";
        case OrderKind::Move:           return "pendingMoveOrders";
        case OrderKind::Disband:        return "pendingDisbandOrders";
        case OrderKind::Withdraw:       return "pendingWithdraws";
        case OrderKind::ShipBuild:      return "pendingShipBuilds";
        case OrderKind::ScrapShip:      return "pendingScrapShips";
        case OrderKind::Embark:         return "pendingEmbarkations";
        case OrderKind::Artillery:      return "pendingArtilleryOrders";
        case OrderKind::ShipMove:       return "pendingShipMoveOrders";
        case OrderKind::ShipEngage:     return "pendingShipEngageOrders";
        case OrderKind::ShipBombard:    return "pendingShipBombardOrders";
        case OrderKind::ShipDisembark:  return "pendingShipDisembarks";
    }
    return "unknown";
}

void encodeOrder(const Order& o, std::vector<uint8_t>& out) {
    putU8(out, (uint8_t)orderKind(o));
    std::visit([&out](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, PendingDiplomaticAction>) {
            putStr(out, x.sourceIso); putStr(out, x.targetIso); putStr(out, x.action);
            putI32(out, x.turnsRemaining); putStr(out, x.subjectIso); putI32(out, x.statedGoal);
        } else if constexpr (std::is_same_v<T, PendingUpgrade>) {
            putI32(out, x.provinceId); putStr(out, x.type);
            putI32(out, x.targetLevel); putI32(out, x.turnsRemaining);
        } else if constexpr (std::is_same_v<T, PendingSpecialization>) {
            putI32(out, x.provinceId); putStr(out, x.specialization); putI32(out, x.turnsRemaining);
        } else if constexpr (std::is_same_v<T, PendingRecruitment>) {
            putI32(out, x.provinceId); putI32(out, x.count);
            putI32(out, x.turnsRemaining); putI32(out, (int)x.type);
        } else if constexpr (std::is_same_v<T, PendingMoveOrder>) {
            putI32(out, x.fromProvince); putI32(out, x.toProvince); putI32(out, x.pct);
            putI32(out, x.countryId); putI32(out, x.troopType);
        } else if constexpr (std::is_same_v<T, PendingDisbandOrder>) {
            putI32(out, x.provinceId); putI32(out, x.count);
        } else if constexpr (std::is_same_v<T, WithdrawOrder>) {
            putI32(out, x.provinceId);
        } else if constexpr (std::is_same_v<T, PendingShipBuild>) {
            putI32(out, x.provinceId); putStr(out, x.type); putI32(out, x.turnsRemaining);
        } else if constexpr (std::is_same_v<T, PendingScrapShip>) {
            putI32(out, x.shipIndex);
        } else if constexpr (std::is_same_v<T, PendingEmbark>) {
            putI32(out, x.provinceId); putI32(out, x.count); putI32(out, x.turnsRemaining);
        } else if constexpr (std::is_same_v<T, PendingArtilleryOrder>) {
            putI32(out, x.fromProvince); putI32(out, x.targetProvince); putStr(out, x.ammoType);
        } else if constexpr (std::is_same_v<T, PendingShipMoveOrder>) {
            // The whole voyage, route and all: a trace of a turn in progress
            // has to be able to say where a fleet was already going.
            putI32(out, x.shipIndex); putF64(out, x.destLon); putF64(out, x.destLat);
            putU32(out, (uint32_t)x.route.size());
            for (const auto& [lon, lat] : x.route) { putF64(out, lon); putF64(out, lat); }
            putU8(out, x.planned ? 1 : 0); putI32(out, x.destProvince);
        } else if constexpr (std::is_same_v<T, PendingShipEngageOrder>) {
            putI32(out, x.shipIndex); putI32(out, x.targetIndex);
        } else if constexpr (std::is_same_v<T, PendingShipBombardOrder>) {
            putI32(out, x.shipIndex); putI32(out, x.targetProvince); putStr(out, x.ammoType);
        } else {
            putI32(out, x.shipIndex); putI32(out, x.targetProvince);
        }
    }, o);
}

bool decodeOrder(const uint8_t* data, size_t size, size_t& pos, Order& out) {
    Reader r{data, size, pos};
    const uint8_t tag = r.u8();
    if (!r.ok) return false;
    switch ((OrderKind)tag) {
        case OrderKind::Diplomatic: {
            PendingDiplomaticAction x;
            x.sourceIso = r.str(); x.targetIso = r.str(); x.action = r.str();
            x.turnsRemaining = r.i32(); x.subjectIso = r.str(); x.statedGoal = r.i32();
            out = std::move(x); break;
        }
        case OrderKind::Upgrade: {
            PendingUpgrade x;
            x.provinceId = r.i32(); x.type = r.str(); x.targetLevel = r.i32(); x.turnsRemaining = r.i32();
            out = std::move(x); break;
        }
        case OrderKind::Specialization: {
            PendingSpecialization x;
            x.provinceId = r.i32(); x.specialization = r.str(); x.turnsRemaining = r.i32();
            out = std::move(x); break;
        }
        case OrderKind::Recruitment: {
            PendingRecruitment x;
            x.provinceId = r.i32(); x.count = r.i32(); x.turnsRemaining = r.i32();
            const int t = r.i32();
            // Bounded, the same as off the wire: an out-of-range kind would
            // index TROOP_TYPES in the resolver.
            if (t < 0 || t >= (int)TROOP_TYPE_COUNT) return false;
            x.type = (TroopType)t;
            out = std::move(x); break;
        }
        case OrderKind::Move: {
            PendingMoveOrder x;
            x.fromProvince = r.i32(); x.toProvince = r.i32(); x.pct = r.i32();
            x.countryId = r.i32(); x.troopType = r.i32();
            out = std::move(x); break;
        }
        case OrderKind::Disband: {
            PendingDisbandOrder x;
            x.provinceId = r.i32(); x.count = r.i32();
            out = std::move(x); break;
        }
        case OrderKind::Withdraw: {
            WithdrawOrder x;
            x.provinceId = r.i32();
            out = x; break;
        }
        case OrderKind::ShipBuild: {
            PendingShipBuild x;
            x.provinceId = r.i32(); x.type = r.str(); x.turnsRemaining = r.i32();
            out = std::move(x); break;
        }
        case OrderKind::ScrapShip: {
            PendingScrapShip x;
            x.shipIndex = r.i32();
            out = x; break;
        }
        case OrderKind::Embark: {
            PendingEmbark x;
            x.provinceId = r.i32(); x.count = r.i32(); x.turnsRemaining = r.i32();
            out = x; break;
        }
        case OrderKind::Artillery: {
            PendingArtilleryOrder x;
            x.fromProvince = r.i32(); x.targetProvince = r.i32(); x.ammoType = r.str();
            out = std::move(x); break;
        }
        case OrderKind::ShipMove: {
            PendingShipMoveOrder x;
            x.shipIndex = r.i32(); x.destLon = r.f64(); x.destLat = r.f64();
            const uint32_t legs = r.u32();
            // Each leg is 16 bytes; refuse a count the buffer cannot hold
            // before reserving anything for it.
            if (!r.ok || legs > (size - pos) / 16) return false;
            x.route.reserve(legs);
            for (uint32_t i = 0; i < legs; ++i) {
                const double lon = r.f64();
                const double lat = r.f64();
                x.route.emplace_back(lon, lat);
            }
            x.planned = r.u8() != 0; x.destProvince = r.i32();
            out = std::move(x); break;
        }
        case OrderKind::ShipEngage: {
            PendingShipEngageOrder x;
            x.shipIndex = r.i32(); x.targetIndex = r.i32();
            out = x; break;
        }
        case OrderKind::ShipBombard: {
            PendingShipBombardOrder x;
            x.shipIndex = r.i32(); x.targetProvince = r.i32(); x.ammoType = r.str();
            out = std::move(x); break;
        }
        case OrderKind::ShipDisembark: {
            PendingShipDisembark x;
            x.shipIndex = r.i32(); x.targetProvince = r.i32();
            out = x; break;
        }
        default:
            return false;
    }
    return r.ok;
}

void encodeOrders(const std::vector<Order>& orders, std::vector<uint8_t>& out) {
    putU32(out, (uint32_t)orders.size());
    for (const Order& o : orders) encodeOrder(o, out);
}

bool decodeOrders(const uint8_t* data, size_t size, std::vector<Order>& out) {
    size_t pos = 0;
    Reader r{data, size, pos};
    const uint32_t n = r.u32();
    // Every order is at least two bytes (a tag and something), so a count
    // larger than that cannot be honest; checked before reserving.
    if (!r.ok || n > (size - pos) / 2) return false;
    out.clear();
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        Order o;
        if (!decodeOrder(data, size, pos, o)) return false;
        out.push_back(std::move(o));
    }
    return pos == size;
}

} // namespace sim
