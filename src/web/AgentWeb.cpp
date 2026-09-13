// ────────────────────────────────────────────────────────────────────────────
// THE GAME AS A MODULE A WEB PAGE DRIVES
//
// The OpenDoctrinesAgent target: the headless game compiled to WebAssembly,
// exporting one seat played from outside, a turn at a time. A page calls
//
//   od_agent_begin("1914:SWE:rung", seed, 120)   -> 1 when the world loaded
//   od_agent_position(1)                          -> JSON: the position, the
//                                                    legal menus, the budget,
//                                                    and (1) every province's
//                                                    owner and colour
//   od_agent_play("e:1,w:2")                      -> JSON: what each choice did
//   od_agent_end_turn()                           -> 1 while turns remain
//
// Everything underneath is Game_Agent.cpp -- the same session the FIFO door in
// OpenDoctrinesServer speaks, so a player in a browser plays under exactly the
// rules a player at a terminal does. Maps come from the preloaded /data/, or
// from any .odmap the page writes into the module's filesystem and names by
// path ("/maps/custom.odmap:ISO").
//
// Strings returned are valid until the next call. One game at a time: begin
// replaces the previous world.
// ────────────────────────────────────────────────────────────────────────────

#include <emscripten/emscripten.h>

#include "Game.h"
#include "json.hpp"

#include <memory>
#include <string>

namespace {
std::unique_ptr<Game> g_game;
std::string g_out;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE int od_agent_begin(const char* seat, unsigned int seed, int turns) {
    g_game.reset();
    g_game = std::make_unique<Game>();
    if (!g_game->agentUseDataDir("/data/")) return 0;
    return g_game->agentBegin(seat ? seat : "", seed, turns) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE const char* od_agent_position(int withMap) {
    g_out = g_game ? g_game->agentPositionJson(withMap != 0) : std::string("null");
    return g_out.c_str();
}

EMSCRIPTEN_KEEPALIVE const char* od_agent_play(const char* tokens) {
    nlohmann::json moves = nlohmann::json::array();
    if (g_game) {
        for (const Game::AgentMove& m : g_game->agentPlay(tokens ? tokens : ""))
            moves.push_back({{"token", m.token}, {"outcome", m.outcome}, {"detail", m.detail}});
    }
    g_out = moves.dump();
    return g_out.c_str();
}

EMSCRIPTEN_KEEPALIVE int od_agent_end_turn() {
    return (g_game && g_game->agentEndTurn()) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE unsigned int od_agent_map_seed() {
    return g_game ? g_game->agentMapSeed() : 0;
}

}  // extern "C"
