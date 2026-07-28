#pragma once

// Verifying a join ticket, on the host, with nobody else involved.
//
// WHAT CHANGED, AND WHY THIS FILE EXISTS
//
// In the relay design the account service checked the ticket and told the host
// who it had. Game traffic no longer goes anywhere near the account service, so
// that check has to happen on the host's own machine, from the ticket alone.
// This is the file that decides whether a stranger connecting to a home
// computer is who they say they are.
//
// The host needs no secret to do it. It verifies an Ed25519 signature with the
// account service's PUBLIC key, published at
// `<issuer>/.well-known/od-keys.json` and cached. So:
//
//   - a busy server never calls home, and a game keeps running if the account
//     service is down or gone;
//   - a host cannot mint a ticket, or a badge, for anyone -- verifying takes
//     only the public key, and issuing takes a private key the host has never
//     seen.
//
// WHAT MAKES A CAPTURED TICKET USELESS
//
// Four independent bindings, each of which alone would be enough:
//
//   - `aud` is `od-relay:<sessionId>` -- exact match, never a prefix. It cannot
//     call the account API, which demands `od-api`, and it cannot be presented
//     at a different session.
//   - `nonce` is a challenge THIS host generated for THIS connection, so a
//     ticket cannot be minted in advance or replayed onto another socket.
//   - `exp` is 120 seconds after issue.
//   - `jti` is burned on first use; see NetTicketReplayGuard.
//
// The audience string still says "od-relay" for a design that no longer has a
// relay. It is an opaque identifier that the Worker and the game must agree on
// byte for byte, so it is left alone deliberately: renaming it would buy
// nothing and would break every already-deployed component at once.
//
// WHAT A TICKET DOES NOT CONTAIN
//
// No account id, no provider identity, no email. A pairwise pseudonym, a
// display name, and whichever badges the PLAYER chose to present. See
// net/PRIVACY.md and net/src/auth/ticket.ts.

#include <cstdint>
#include <string>
#include <vector>

/** An Ed25519 public key from the issuer's published key set. */
struct NetIssuerKey {
    uint8_t bytes[32]{};
};

/**
 * Read `/.well-known/od-keys.json`.
 *
 * Returns every usable Ed25519 key; more than one means a rotation is in
 * progress and a ticket signed by either is genuine. Empty if the document is
 * malformed, which callers must treat as "cannot verify anyone" rather than as
 * "everyone is fine".
 */
std::vector<NetIssuerKey> netParseIssuerKeys(const std::string& json);

/** What the account service said about a player, once the signature held. */
struct NetJoinTicket {
    std::string issuer;
    std::string audience;
    std::string psid;      // pairwise pseudonym: stable here, unrelated elsewhere
    std::string name;      // account nickname, or a per-server alias
    std::vector<std::string> badges;
    std::string nonce;
    std::string jti;
    long long   issuedAt = 0;
    long long   expires  = 0;
};

/** What the host requires of a ticket before it will believe any of it. */
struct NetTicketCheck {
    std::string issuer;      // exact match against the ticket's `iss`
    std::string audience;    // exact match: "od-relay:<sessionId>"
    std::string nonce;       // the challenge this host sent on this connection
    long long   now = 0;     // unix seconds

    /**
     * Residual tolerance for the host's clock, on top of NetIssuerClock.
     *
     * Deliberately small, because it is the wrong tool. Widening it trades
     * security for connectivity; learning the issuer's real time removes the
     * problem instead, so that is what the host does and this is only here to
     * absorb the second or two of round-trip that remains.
     */
    long long skewSeconds = 30;
};

/**
 * The issuer's clock, learned from its HTTP `Date` header.
 *
 * WHY THIS EXISTS
 *
 * A join ticket lives 120 seconds. A host whose machine clock is a few minutes
 * out therefore rejects every player alive, and the only symptom anyone sees is
 * that nobody can join -- no error a player can act on, nothing that looks like
 * a clock problem. Home machines with wrong clocks are common; a host who has
 * never noticed theirs is wrong is exactly the person who would be stuck.
 *
 * The alternative was to allow minutes of skew on the ticket, which buys
 * connectivity by weakening the freshness guarantee for everyone. This removes
 * the cause instead: the host already talks to the issuer over HTTPS to fetch
 * its key, and every reply carries the issuer's own time.
 *
 * NOT A TRUSTED TIME SOURCE, and it does not need to be. It moves a deadline on
 * a token whose authenticity rests entirely on a signature. The worst a
 * manipulated `Date` could do is shift a 120-second window -- and to manipulate
 * it, someone would already have to have broken the TLS connection to the
 * issuer, at which point the window is not what is protecting anybody.
 */
class NetIssuerClock {
public:
    /**
     * Record the issuer's time alongside our own at the moment of the reply.
     * Ignored when `serverTime` is 0, so a reply without a usable `Date`
     * simply leaves the previous estimate in place.
     */
    void observe(long long serverTime, long long localTime);

    /** Our best estimate of the issuer's clock, given ours. */
    long long now(long long localTime) const { return localTime + m_offset; }

    /** Seconds the local clock is behind (negative: ahead of) the issuer. */
    long long offset() const { return m_offset; }

    /** True once an issuer reply has been seen. */
    bool known() const { return m_known; }

private:
    long long m_offset = 0;
    bool      m_known = false;
};

/**
 * Verify a join ticket completely: signature, issuer, audience, freshness and
 * the host's own challenge.
 *
 * Returns false for every kind of failure and never says which one. Reporting
 * which check failed would let someone probe a host for valid session ids,
 * live nonces, or whether a key had rotated. `out` is untouched unless the
 * whole thing passed.
 */
bool netVerifyJoinTicket(const std::string& token,
                         const std::vector<NetIssuerKey>& keys,
                         const NetTicketCheck& expect,
                         NetJoinTicket& out);

/**
 * Burns a ticket's `jti` so it works exactly once.
 *
 * Nothing else stops the same valid ticket being presented twice inside its
 * 120-second life -- on two sockets at once, for instance, to occupy two seats
 * as one player. Entries are dropped once the ticket they refer to has expired,
 * so this stays small without ever forgetting one that still matters.
 */
class NetTicketReplayGuard {
public:
    /** True the first time a `jti` is seen; false every time after. */
    bool useOnce(const std::string& jti, long long expires, long long now);

    /** Drop entries whose tickets have expired. Cheap; call when convenient. */
    void sweep(long long now);

    size_t size() const { return m_seen.size(); }
    void clear() { m_seen.clear(); }

private:
    struct Used {
        std::string jti;
        long long   until = 0;
    };
    std::vector<Used> m_seen;
};
