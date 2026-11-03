#pragma once
// ─── sim::WorldState ─────────────────────────────────────────────────────────
//
// REWORK_PLAN.md item 4. The gameplay state, extracted out of Game into a plain,
// copyable struct a search can afford to copy (the target size is a few MB).
// Game keeps rendering, UI and the raster, and holds one of these as
// Game::m_world.
//
// FILLED ONE FIELD GROUP AT A TIME. Until a group is moved here Game still owns
// it; a group that HAS moved stays reachable from Game through a reference-alias
// member -- e.g.
//
//     sim::WorldState m_world;            // declared first
//     int& m_turnNumber = m_world.turnNumber;
//
// so the hundreds of existing `m_turnNumber` call sites are untouched and every
// step is behaviour- and save-neutral. The alias is safe because Game is already
// non-copyable (it owns unique_ptrs), and the referent is a sibling member with
// the same lifetime. Each move is guarded by the save round-trip test
// (tests/save_roundtrip_test.cpp) and the decision hash (OD_DECISION_HASH); both
// must be unchanged. Once every group has moved, the aliases can be retired by
// migrating call sites to m_world.* , but that is cleanup, not a blocker.
//
// Groups still to move: provinces' gameplay fields (not the raster), and the
// turn RNG.
#include <string>
#include <vector>
#include <unordered_map>
#include "../GameStructs.h"       // CountryRelation, NavyShip, ArmyUnit
#include "../map/CountryMap.h"    // CountryMap

namespace sim {

struct WorldState {
    int turnNumber = 0;   ///< the game turn counter (Game::m_turnNumber aliases this)

    /// The countries and their per-country state (Game::m_countries).
    CountryMap countries;

    /// Diplomacy: relations[a][b] is a's stance toward b (Game::m_relations).
    std::unordered_map<std::string, std::unordered_map<std::string, CountryRelation>> relations;

    /// Land forces, by the province they stand in (Game::m_provinceArmies).
    std::unordered_map<int, std::vector<ArmyUnit>> provinceArmies;

    /// Every hull afloat (Game::m_ships).
    std::vector<NavyShip> ships;

    // ── Pending orders ──────────────────────────────────────────────────────
    // The orders issued this turn, awaiting resolution. The same set sim::Order
    // is a variant over (REWORK_PLAN item 1): the 14 Pending* queues plus the
    // withdraw list. Game reaches each through a reference-alias member.
    std::vector<int>                     pendingWithdraws;      ///< province ids
    std::vector<PendingDiplomaticAction> pendingDiplomaticActions;
    std::vector<PendingUpgrade>          pendingUpgrades;
    std::vector<PendingSpecialization>   pendingSpecializations;
    std::vector<PendingRecruitment>      pendingRecruitments;
    std::vector<PendingMoveOrder>        pendingMoveOrders;
    std::vector<PendingDisbandOrder>     pendingDisbandOrders;
    std::vector<PendingShipBuild>        pendingShipBuilds;
    std::vector<PendingScrapShip>        pendingScrapShips;
    std::vector<PendingEmbark>           pendingEmbarkations;
    std::vector<PendingArtilleryOrder>   pendingArtilleryOrders;
    std::vector<PendingShipMoveOrder>    pendingShipMoveOrders;
    std::vector<PendingShipEngageOrder>  pendingShipEngageOrders;
    std::vector<PendingShipBombardOrder> pendingShipBombardOrders;
    std::vector<PendingShipDisembark>    pendingShipDisembarks;
};

}  // namespace sim
