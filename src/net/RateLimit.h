#pragma once

// A budget, per peer, for anything a client can do as often as it likes.
//
// WHY THIS IS SEPARATE FROM ChatRules.h. That header answers a question about
// CHAT -- is it switched on, is the line empty, is it too long -- and a token
// bucket happened to be part of the answer. The bucket itself has nothing to do
// with chat: the host needs the same shape for frames and for bytes, and this
// codebase has been bitten repeatedly by one rule living in two places.
//
// THE BURST IS THE PART THAT MAKES IT USABLE. A flat rate with no burst refuses
// the ordinary shape of real traffic -- three messages typed quickly, a client
// reconnecting and catching up -- while stopping nothing, because a flooder is
// perfectly happy to send at exactly the limit for ever. Tokens accumulate up
// to a ceiling and are spent in lumps.

#include <cstdint>
#include <unordered_map>

namespace netrate {

/** One peer's budget. `rate` is units per second, `burst` the ceiling. */
struct Bucket {
    double tokens = 0.0;
    double at = 0.0;
    bool   seen = false;

    /**
     * Spend `cost` if it is there. Returns false without spending if it is not.
     *
     * A refusal must cost nothing, or a client that is told to slow down and
     * politely retries is pushed further from recovery by the retry itself and
     * never gets back.
     *
     * The clock is guarded against going backwards: on a host left running for
     * a tournament that is a certainty rather than a hypothetical, and the
     * naive refill would hand out negative time -- silencing the peer for the
     * length of the step rather than for the length of its overspend.
     */
    bool spend(double cost, double now, double rate, double burst) {
        if (!seen) { seen = true; tokens = burst; at = now; }
        if (now > at) {
            tokens += (now - at) * rate;
            if (tokens > burst) tokens = burst;
        }
        at = now;
        if (tokens < cost) return false;
        tokens -= cost;
        return true;
    }
};

/** Buckets keyed by peer, with one rate for all of them. */
class PerPeer {
public:
    PerPeer(double rate, double burst) : m_rate(rate), m_burst(burst) {}

    bool allow(uint16_t peerId, double cost, double now) {
        return m_buckets[peerId].spend(cost, now, m_rate, m_burst);
    }
    /// A peer that left takes its budget with it: relay handles are reused, and
    /// a new player must not inherit the last one's exhausted bucket.
    void forget(uint16_t peerId) { m_buckets.erase(peerId); }
    void clear() { m_buckets.clear(); }

    double rate() const { return m_rate; }
    double burst() const { return m_burst; }

private:
    double m_rate, m_burst;
    std::unordered_map<uint16_t, Bucket> m_buckets;
};

}  // namespace netrate
