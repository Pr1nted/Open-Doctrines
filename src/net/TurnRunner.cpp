#include "TurnRunner.h"

void TurnRunner::beginTurn(uint32_t turnNumber, long long nowMs) {
    m_turnNumber = turnNumber;
    m_running = true;
    // Long-form has no deadline at all. Storing one anyway and then ignoring it
    // would leave a value that looks meaningful to anything reading it.
    m_deadlineMs = m_config.turnSeconds == 0
        ? 0
        : nowMs + static_cast<long long>(m_config.turnSeconds) * 1000;
}

bool TurnRunner::due(long long nowMs) const {
    if (!m_running || m_config.turnSeconds == 0) return false;
    return nowMs >= m_deadlineMs;
}

uint32_t TurnRunner::remainingMs(long long nowMs) const {
    if (!m_running || m_config.turnSeconds == 0) return 0;
    const long long left = m_deadlineMs - nowMs;
    return left <= 0 ? 0 : static_cast<uint32_t>(left);
}

std::vector<TurnResolution> TurnRunner::resolve(const Lobby& lobby,
                                                uint32_t turnNumber) const {
    std::vector<TurnResolution> out;

    for (const auto& m : lobby.members()) {
        // A spectator holds nothing, and a player who never took a country has
        // nothing to resolve. Neither belongs in the output.
        if (m.spectator || m.countryId == 0) continue;

        TurnResolution r;
        r.peerId = m.peerId;
        r.countryId = m.countryId;

        const bool submittedThisTurn = m.submitted && m.submittedTurn == turnNumber;
        const bool malformedThisTurn = m.malformed && m.submittedTurn == turnNumber;

        if (submittedThisTurn) {
            // Note what is NOT consulted here: whether they are connected. They
            // sent their orders; closing the game afterwards is their business.
            r.usePlayerOrders = true;
            r.orders = m.orders;
            out.push_back(std::move(r));
            continue;
        }

        r.substitution = malformedThisTurn ? NetSubstitution::Malformed
                                           : NetSubstitution::NotSubmitted;

        // A malformed submission is ALWAYS played by the AI, whatever the
        // absent policy says. "Idle" is for a player who chose not to act; a
        // player whose orders were mangled did not choose anything, and
        // freezing their country would punish them for a transport failure.
        r.aiPlays = malformedThisTurn || m_config.absent == NetAbsent::Ai;

        const char* why = netSubstitutionReason(r.substitution);
        r.announcement = r.aiPlays
            ? std::string("The server AI played this turn because ") + why + "."
            : std::string("This country did nothing this turn because ") + why + ".";

        out.push_back(std::move(r));
    }
    return out;
}

bool TurnRunner::anySubstituted(const std::vector<TurnResolution>& r) {
    for (const auto& one : r) {
        if (one.substitution != NetSubstitution::None) return true;
    }
    return false;
}
