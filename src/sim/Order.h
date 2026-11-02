#pragma once
// ─── ONE ORDER TYPE ─────────────────────────────────────────
//
// Groundwork item 1 of docs/ai/REWORK_PLAN.md.
//
// Everything a country can tell the world to do between two turns has always
// been a push into one of about fifteen `Game::m_pending*` vectors, each with
// its own struct in GameStructs.h. That is fine for a resolver -- it walks each
// queue in a fixed order -- and hopeless for anything that wants to talk ABOUT
// orders: a search that generates moves, a visualiser that shows what the AI
// did, a second AI engine that hands back a list instead of reaching into Game.
// None of those can be written against fifteen unrelated vectors.
//
// So `Order` is a variant over the existing structs. NOT new structs: the
// resolver keeps reading exactly the queues it reads today, and an Order is
// only a way to carry one entry of one of them around. `apply(game, order)` is
// the push it stands for -- no validation, no price, no re-attribution -- so
// replacing a push_back with an apply is behaviour-neutral by construction.
// The validation the multiplayer host does (Game::mpApplyOrders) stays where it
// is: it is a property of an UNTRUSTED source, and the AI is not one.
//
// THE BYTE ENCODING is for traces, tests and, later, a search that has to hash
// or store a move. It is NOT the multiplayer wire and does not replace it: the
// wire is JSON (Game::mpSerializeOrders) and its format is a compatibility
// promise to older clients. What is aligned with it is the vocabulary -- the
// kind tags below are numbered in the order the wire lists its keys, and
// orderKindName() returns the wire's own key, so a trace and a wire dump name
// the same order the same way.
//
// This header and Order.cpp do not include Game.h, so a unit test can link the
// encoding alone (tests/order_codec_test.cpp). apply() lives in OrderApply.cpp,
// which does.

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "../GameStructs.h"

class Game;

namespace sim {

/**
 * A withdrawal from a standing battle.
 *
 * The one queue without a struct of its own: Game::m_pendingWithdraws is a
 * vector of province ids. It gets a one-field struct here only so the variant
 * can tell it apart from every other order that is "one int".
 */
struct WithdrawOrder {
    int provinceId = 0;
};

/**
 * Every kind of order, one alternative per `m_pending*` queue.
 *
 * THE ALTERNATIVES' ORDER IS NOT THE ENCODING. The tag written to bytes is
 * OrderKind, below, which is pinned by number; the variant index is free to
 * move if an alternative is ever added in the middle. Adding one means: a new
 * OrderKind at the END, a case in encode/decode, a case in apply, and a case
 * in the round-trip test, which fails until all four agree.
 */
using Order = std::variant<
    PendingDiplomaticAction,
    PendingUpgrade,
    PendingSpecialization,
    PendingRecruitment,
    PendingMoveOrder,
    PendingDisbandOrder,
    WithdrawOrder,
    PendingShipBuild,
    PendingScrapShip,
    PendingEmbark,
    PendingArtilleryOrder,
    PendingShipMoveOrder,
    PendingShipEngageOrder,
    PendingShipBombardOrder,
    PendingShipDisembark>;

/// The stable tag of each kind in the byte encoding. Never renumber.
enum class OrderKind : uint8_t {
    Diplomatic     = 1,
    Upgrade        = 2,
    Specialization = 3,
    Recruitment    = 4,
    Move           = 5,
    Disband        = 6,
    Withdraw       = 7,
    ShipBuild      = 8,
    ScrapShip      = 9,
    Embark         = 10,
    Artillery      = 11,
    ShipMove       = 12,
    ShipEngage     = 13,
    ShipBombard    = 14,
    ShipDisembark  = 15,
};
constexpr int kOrderKindCount = 15;

OrderKind orderKind(const Order& o);
/// The multiplayer wire's key for this kind ("pendingMoveOrders", ...).
/// Diplomatic actions do not travel on the order wire -- they are answered by
/// the host through their own path -- so that one is named
/// "pendingDiplomaticActions" after its Game queue instead.
const char* orderKindName(OrderKind k);

/**
 * Append one order's bytes to `out`.
 *
 * Layout: a one-byte OrderKind, then the struct's fields in declaration order.
 * Integers are 32-bit little-endian two's complement, doubles are their IEEE-754
 * bits as 64-bit little-endian, bools one byte, strings a 32-bit length then
 * that many bytes. Explicit byte order rather than memcpy of the struct, so the
 * bytes are the same on every platform the game builds for, and a struct that
 * gains padding or a field does not silently change what was written.
 */
void encodeOrder(const Order& o, std::vector<uint8_t>& out);
/**
 * Read one order starting at `pos`, advancing `pos` past it.
 *
 * Returns false -- and leaves `out` unspecified -- on an unknown tag, a
 * truncated buffer, or a string or route longer than the bytes that are left.
 * Never reads past `size`: these bytes may come from a file.
 */
bool decodeOrder(const uint8_t* data, size_t size, size_t& pos, Order& out);

/// A whole list: a 32-bit count, then each order.
void encodeOrders(const std::vector<Order>& orders, std::vector<uint8_t>& out);
bool decodeOrders(const uint8_t* data, size_t size, std::vector<Order>& out);

/**
 * Do what pushing this order into its `m_pending*` queue does today.
 *
 * Exactly that and nothing else: no ownership check, no price, no dedup. The
 * callers that need those (mpApplyOrders, queueDiplomaticAction, orderWithdraw)
 * still do them first, and the AI's executors did theirs before they pushed.
 */
void apply(Game& g, const Order& o);

/**
 * Where an AI's orders go.
 *
 * The seam groundwork item 2 is about. AISystem's executors used to push into
 * Game's queues directly; they now hand each order to a sink. The default sink,
 * ImmediateOrderSink, applies it on the spot -- which is what the push did, in
 * the same order, so nothing an executor reads afterwards (its own dedup scans
 * of the queues, most of all) sees anything different.
 *
 * A recording sink, a deferred one for a search, or one that also logs for the
 * visualiser can be dropped in without touching an executor. A deferring sink
 * is NOT behaviour-neutral, though: the executors read the queues back to avoid
 * issuing an order twice, and that only works while their writes land at once.
 */
class OrderSink {
public:
    virtual ~OrderSink() = default;
    virtual void issue(Order o) = 0;
};

/// The default: apply at once, exactly as the direct push did.
class ImmediateOrderSink final : public OrderSink {
public:
    explicit ImmediateOrderSink(Game* g = nullptr) : m_g(g) {}
    void setGame(Game* g) { m_g = g; }
    void issue(Order o) override { if (m_g) sim::apply(*m_g, o); }   // qualified: ADL on the variant would find std::apply
private:
    Game* m_g;
};

} // namespace sim
