// sim::apply -- the one function an Order needs Game for. Kept out of Order.cpp
// so the codec can be tested without linking the game. See Order.h.
#include "Order.h"
#include "../Game.h"

#include <type_traits>

// Game's queues are private; this is the one struct Game names as a friend for
// them. A struct rather than befriending sim::apply itself because a friend
// FUNCTION has to be declared, with the Order variant spelled out, before
// Game.h can name it -- and Game.h should not have to include this header.
struct OrderApplier {
    static void apply(Game& g, const sim::Order& o) {
        std::visit([&g](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, PendingDiplomaticAction>) g.m_pendingDiplomaticActions.push_back(x);
            else if constexpr (std::is_same_v<T, PendingUpgrade>)        g.m_pendingUpgrades.push_back(x);
            else if constexpr (std::is_same_v<T, PendingSpecialization>) g.m_pendingSpecializations.push_back(x);
            else if constexpr (std::is_same_v<T, PendingRecruitment>)    g.m_pendingRecruitments.push_back(x);
            else if constexpr (std::is_same_v<T, PendingMoveOrder>)      g.m_pendingMoveOrders.push_back(x);
            else if constexpr (std::is_same_v<T, PendingDisbandOrder>)   g.m_pendingDisbandOrders.push_back(x);
            else if constexpr (std::is_same_v<T, sim::WithdrawOrder>)    g.m_pendingWithdraws.push_back(x.provinceId);
            else if constexpr (std::is_same_v<T, PendingShipBuild>)      g.m_pendingShipBuilds.push_back(x);
            else if constexpr (std::is_same_v<T, PendingScrapShip>)      g.m_pendingScrapShips.push_back(x);
            else if constexpr (std::is_same_v<T, PendingEmbark>)         g.m_pendingEmbarkations.push_back(x);
            else if constexpr (std::is_same_v<T, PendingArtilleryOrder>) g.m_pendingArtilleryOrders.push_back(x);
            else if constexpr (std::is_same_v<T, PendingShipMoveOrder>)  g.m_pendingShipMoveOrders.push_back(x);
            else if constexpr (std::is_same_v<T, PendingShipEngageOrder>) g.m_pendingShipEngageOrders.push_back(x);
            else if constexpr (std::is_same_v<T, PendingShipBombardOrder>) g.m_pendingShipBombardOrders.push_back(x);
            else { static_assert(std::is_same_v<T, PendingShipDisembark>, "unhandled Order kind");
                   g.m_pendingShipDisembarks.push_back(x); }
        }, o);
    }
};

namespace sim {
void apply(Game& g, const Order& o) { OrderApplier::apply(g, o); }
} // namespace sim
