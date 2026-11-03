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
// Groups still to move: provinces' gameplay fields (not the raster), countries,
// armies, ships, relations, pending orders, and the turn RNG.
namespace sim {

struct WorldState {
    int turnNumber = 0;   ///< the game turn counter (Game::m_turnNumber aliases this)
};

}  // namespace sim
