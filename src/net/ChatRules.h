#pragma once

// WHO MAY SAY WHAT, AND HOW OFTEN. Decided once, for everybody.
//
// The transport for lobby chat has existed since the protocol was written --
// a client sends Chat, the host stamps who it came from and broadcasts
// ChatFrom -- and there were no rules in front of it at all. Any joined peer
// could send as fast as it could write, and the host would fan every one of
// those out to every other peer. With eight players in a lobby that is an
// eightfold amplifier driven by whoever is rudest, which is a thing to find
// out about before a tournament rather than during one.
//
// IT IS A PURE HEADER ON PURPOSE, like ReleaseRules.h beside it. The host is
// the only thing that can enforce these, but it is also the hardest place to
// test them: enforcing inside the frame handler means a test needs a socket, a
// relay and eight peers. Here it needs a number and a clock.
//
// See tests/net_chat_test.cpp.

#include "RateLimit.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace netchat {

/** What the host decided about one incoming line. */
enum class Verdict {
    Allowed,
    Disabled,    ///< the host turned chat off for this game
    Empty,       ///< nothing but whitespace
    TooLong,     ///< longer than the protocol's own cap
    TooFast,     ///< this peer is over its budget
};

/** Why a line was refused, for the one person who needs to know. */
inline const char* explain(Verdict v) {
    switch (v) {
        case Verdict::Allowed:  return "";
        case Verdict::Disabled: return "Chat is off in this game.";
        case Verdict::Empty:    return "";
        case Verdict::TooLong:  return "That was too long to send.";
        case Verdict::TooFast:  return "You are sending too quickly.";
    }
    return "";
}

/**
 * The dials. Defaults are for a lobby of people talking, not a chat room.
 *
 * `burst` is what makes this usable: a player who has said nothing for a while
 * can send three lines back to back -- a thought split over three messages is
 * how people actually type -- and then settles to `perSecond`. A flat rate with
 * no burst punishes normal conversation and stops nothing, because a flooder is
 * happy to send at exactly the limit forever.
 */
struct Policy {
    bool   enabled   = true;
    double perSecond = 0.5;    ///< a line every two seconds, sustained
    double burst     = 3.0;    ///< and three in hand to start with
    size_t maxLen    = 512;    ///< matches NetLimits::kChat
};

/**
 * Per-peer budgets, held by the host.
 *
 * A token bucket rather than "time since last message": the latter cannot
 * express "three quickly, then slow down" without a second counter, and it
 * refuses the second line of a two-line thought.
 *
 * NOT thread-safe, and does not need to be -- the host handles frames on one
 * thread. It is stateful, so it is a class rather than a free function; the
 * DECISION is still pure given (bucket, now, text).
 */
class Gate {
public:
    explicit Gate(Policy p = {}) : m_policy(p) {}

    const Policy& policy() const { return m_policy; }
    void configure(const Policy& p) { m_policy = p; }

    /**
     * May `peerId` say `text` at `now` (seconds, monotonic)?
     *
     * A refusal costs no tokens: being told to slow down must not push the
     * budget further away, or a client that retries politely is punished for
     * the retry and never recovers.
     */
    Verdict admit(uint16_t peerId, const std::string& text, double now) {
        if (!m_policy.enabled) return Verdict::Disabled;
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) return Verdict::Empty;
        if (text.size() > m_policy.maxLen) return Verdict::TooLong;

        // The bucket itself lives in RateLimit.h, shared with the host's frame
        // and byte budgets -- one implementation of "how often may this peer",
        // not three.
        if (!m_buckets[peerId].spend(1.0, now, m_policy.perSecond, m_policy.burst))
            return Verdict::TooFast;
        return Verdict::Allowed;
    }

    /// A peer that left takes its budget with it, so ids can be reused.
    void forget(uint16_t peerId) { m_buckets.erase(peerId); }

    void clear() { m_buckets.clear(); }

private:
    Policy m_policy;
    std::unordered_map<uint16_t, netrate::Bucket> m_buckets;
};

}  // namespace netchat
