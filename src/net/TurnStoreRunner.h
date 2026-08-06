#pragma once

// Turn-store work, kept off the render thread.
//
// Every `TurnStoreClient` call blocks on the network -- TurnStore.h says so and
// means it -- and a store may be a stranger's machine on the far side of a
// timeout. This game draws at sixty frames a second. So requests are queued
// here and a worker performs them, which is exactly what AccountClient does for
// the account API and for exactly the same reason.
//
// WHAT THIS DELIBERATELY DOES NOT DO
//
// It does not know what a turn is, whether one is due, whose orders are
// missing, or what to do with what it fetched. It moves opaque bytes to and
// from a store and reports what happened. Every decision stays with the caller,
// where the game's rules already live -- this is transport, the same way
// Host.cpp is transport and Lobby.cpp is the rules.
//
// IT ALSO DOES NOT SEAL ANYTHING. Orders arrive here already sealed and turn
// bundles are public by design, so this layer never holds a key and never has
// to be trusted with one. See TurnSeal.h.

#include "TurnStore.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/** One finished piece of work, waiting to be collected. */
struct TurnStoreResult {
    enum class Kind : uint8_t {
        TurnPublished,
        TurnFetched,
        OrdersPublished,
        OrdersFetched,
    } kind = Kind::TurnPublished;

    bool     ok = false;
    uint32_t turnNumber = 0;

    /** Whose orders these are, for the two orders kinds. Empty otherwise. */
    std::string psid;

    /** Where it ended up, for the publish kinds -- a spectator's link. */
    TurnStoreRef ref;

    /** The blob, for the fetch kinds. Still sealed, for orders. */
    std::vector<uint8_t> payload;

    /**
     * A sentence for a player when `ok` is false.
     *
     * Note that "not there yet" is an ordinary answer for a fetch, not a
     * failure worth showing: in long-form, most of the time nobody has
     * submitted anything, and a game that complained about it every few seconds
     * would be unusable. Callers check `ok` and stay quiet.
     */
    std::string error;
};

class TurnStoreRunner {
public:
    TurnStoreRunner();
    ~TurnStoreRunner();
    TurnStoreRunner(const TurnStoreRunner&) = delete;
    TurnStoreRunner& operator=(const TurnStoreRunner&) = delete;

    /** Applies to work queued after it; anything in flight keeps the old one. */
    void configure(const TurnStoreClient::Config& config);
    TurnStoreClient::Config config() const;

    /** False for Manual, where the player is the transport. */
    bool automatic() const;

    void publishTurn(uint32_t turnNumber, const std::vector<uint8_t>& bundle);
    void publishOrders(uint32_t turnNumber, const std::string& psid,
                       const std::vector<uint8_t>& sealed);

    /**
     * Fetches take a ref rather than deriving one, because only some stores
     * can be derived from -- see TurnStoreClient::turnRef. Pass what that
     * returned, or the id a JsonBlob host handed out.
     */
    void fetchTurn(uint32_t turnNumber, const TurnStoreRef& ref);
    void fetchOrders(uint32_t turnNumber, const std::string& psid,
                     const TurnStoreRef& ref);

    /** Take one finished result. False when there are none waiting. */
    bool nextResult(TurnStoreResult& out);

    /**
     * Requests queued or in flight.
     *
     * The caller uses this to avoid asking for the same thing twice while the
     * first ask is still out -- a poll loop that did would queue faster than
     * the network could drain.
     */
    size_t pending() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
