#pragma once

// When a turn fires, and what happens to each country when it does.
//
// Pure logic over a clock the caller supplies. No sockets, no timers of its
// own, no game state -- it answers "is it time yet" and "whose orders count",
// which are the two decisions that must be right every single turn.
//
// THE RULE THAT MATTERS MOST
//
// A submission that could not be read is discarded WHOLE. Not partly applied,
// not best-effort: the server AI plays that country and everyone is told it
// did, and why. Half a player's intent is worse than none of it, because it is
// not recoverable into anything they would recognise as their plan.
//
// AND THE ONE THAT IS EASIEST TO GET WRONG
//
// Being disconnected is not the same as not having submitted. Someone who sent
// orders and closed the game has done their part; with a long turn interval
// that is ordinary behaviour, not neglect. Substitution is decided by what
// arrived, never by who is currently connected.

#include "Lobby.h"
#include "NetProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

/** What the server will do for one country this turn. */
struct TurnResolution {
    uint16_t peerId = 0;
    uint16_t countryId = 0;

    /** True when the player's own orders are used. */
    bool usePlayerOrders = false;
    std::vector<uint8_t> orders;

    /** True when the server AI plays this country instead. */
    bool aiPlays = false;

    /**
     * Why the player was not played by their own orders. `None` when they
     * were, so this doubles as "was anything substituted".
     */
    NetSubstitution substitution = NetSubstitution::None;

    /** A sentence for the announcement. Empty when nothing was substituted. */
    std::string announcement;
};

class TurnRunner {
public:
    struct Config {
        /** 0 means long-form: no deadline, and due() is never true. */
        uint32_t turnSeconds = 0;
        /** What happens to a country whose player sent nothing readable. */
        NetAbsent absent = NetAbsent::Ai;
    };

    void configure(const Config& c) { m_config = c; }
    const Config& config() const { return m_config; }

    /** Starts the clock for a turn. `nowMs` is any monotonic millisecond. */
    void beginTurn(uint32_t turnNumber, long long nowMs);

    uint32_t turnNumber() const { return m_turnNumber; }
    bool     running() const { return m_running; }

    /** True once the deadline has passed. Always false in long-form. */
    bool due(long long nowMs) const;

    /** Milliseconds left, for the countdown. 0 in long-form or when due. */
    uint32_t remainingMs(long long nowMs) const;

    /** Stops the clock without resolving. */
    void stop() { m_running = false; }

    /**
     * Decide, for every player country, whose orders are used.
     *
     * Reads the lobby and touches nothing: the caller applies the result and
     * then clears submissions. Spectators and countryless members are absent
     * from the output entirely rather than present with a flag.
     */
    std::vector<TurnResolution> resolve(const Lobby& lobby, uint32_t turnNumber) const;

    /** True when any resolution substituted for a player. */
    static bool anySubstituted(const std::vector<TurnResolution>& r);

private:
    Config   m_config;
    uint32_t m_turnNumber = 0;
    long long m_deadlineMs = 0;
    bool     m_running = false;
};
