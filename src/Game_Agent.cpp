#include "Game.h"
#include "Audio.h"
#include "GameInternals.h"
#include "ai/AISystem.h"
#include "json.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

// ────────────────────────────────────────────────────────────────────────────
// ONE SEAT, PLAYED FROM OUTSIDE, A TURN AT A TIME
//
// An outside player -- a person at a terminal, a script, a simulated fly brain
// -- plays a benchmark seat under exactly the rules the policy plays under: the
// same legal menus, the same executor, the model's per-module budget, and a
// module that ends the moment it picks 0. Those rules used to live inside
// runBenchAgent's FIFO loop. They live here now, once, and there are two ways
// in: runBenchAgent speaks them as text over a FIFO (the protocol Open Fly's
// Python driver parses), and the browser build calls them directly
// (src/web/AgentWeb.cpp). A second copy of the budget rule in either caller
// would be the copy that drifts.
// ────────────────────────────────────────────────────────────────────────────

namespace {

// The action names the agent protocol prints and an outside player reads back
// -- a script, a fly -- not text drawn on screen. i18n-ignore keeps the
// extractor from asking translators for "fund up" and "pacify dn".
// i18n-ignore
const char* const ECON_N[] = {"save", "industry", "fort", "port", "specialize",
    "destroyer", "carrier", "fund up", "fund down", "focus bldg", "focus army", "focus navy"};
// i18n-ignore
const char* const POL_N[] = {"hold", "enact", "pacify up", "pacify dn", "cancel",
    "alliance", "nap", "guarantee", "calming", "conciliate", "repress", "trade"};
// i18n-ignore
const char* const WAR_N[] = {"hold", "recruit", "reinforce", "attack", "declare war",
    "artillery", "ceasefire", "stage"};
// i18n-ignore
const char* const NAVY_N[] = {"hold", "move", "bombard", "embark", "land", "scrap", "engage"};

struct ModInfo { const char* letter; const char* label; const char* const* names; int count; };
const ModInfo MODS[] = {
    {"e", "economy",  ECON_N, 12}, {"p", "politics", POL_N, 12},
    {"w", "war",      WAR_N,  8},  {"n", "navy",     NAVY_N, 7},
};

const char* actionName(int mod, int act) {
    return (act >= 0 && act < MODS[mod].count) ? MODS[mod].names[act] : "?";
}

std::string base64(const std::string& raw) {
    static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string enc;
    enc.reserve((raw.size() + 2) / 3 * 4);
    for (size_t k = 0; k < raw.size(); k += 3) {
        uint32_t n = (uint32_t)(uint8_t)raw[k] << 16;
        if (k + 1 < raw.size()) n |= (uint32_t)(uint8_t)raw[k + 1] << 8;
        if (k + 2 < raw.size()) n |= (uint32_t)(uint8_t)raw[k + 2];
        enc.push_back(B64[(n >> 18) & 63]);
        enc.push_back(B64[(n >> 12) & 63]);
        enc.push_back(k + 1 < raw.size() ? B64[(n >> 6) & 63] : '=');
        enc.push_back(k + 2 < raw.size() ? B64[n & 63] : '=');
    }
    return enc;
}

} // namespace

struct Game::AgentPosition {
    int turn = 0, until = 0;
    std::string name, iso;
    long long mine = 0, owned = 0, army = 0;
    double treasury = 0.0;
    CountryIncomeSnapshot inc{};
    std::vector<std::string> wars;
    std::vector<int> legal[4];
    int budget[4] = {0, 0, 0, 0};
};

bool Game::agentUseDataDir(const std::string& dir) {
    // The browser build has no executable to look beside and no server config
    // to read: its data is wherever the page mounted it. srvResolveDataDir's
    // probing lives with the server loop, which that build does not link.
    m_dataDir = dir;
    if (!m_dataDir.empty() && m_dataDir.back() != '/' && m_dataDir.back() != '\\') m_dataDir += '/';
    std::error_code ec;
    return std::filesystem::is_directory(m_dataDir, ec);
}

bool Game::agentBegin(const std::string& seatSpec, unsigned int seed, int untilTurn) {
    // SAFE BY CONSTRUCTION, not by the caller remembering. This builds a
    // real AISystem and ~AISystem() saves unconditionally, so without this
    // the run's exit overwrites <data>/ai/model.bin. ServerMain used to set
    // it at the call site; journal 311 wired a new caller and did not, which
    // is exactly the failure a call-site guard invites. Journal 314.
    AISystem::s_readOnlyModel = true;
    applyFpsTarget(-1);
    Audio::s_disabled = true;
    m_agentLoad = true;                // see Game.h: load nothing that is only drawn

    // ── ONE SEED, ONE WORLD ──
    //
    // The map seed is derived here, BEFORE the load, because the load is where a
    // fresh world draws its seed: with m_worldSeed unset, chooseWorldSeed() took
    // one from random_device and jitterStartingPolitics() spent it, so two runs
    // of the same seat and seed started from different politics and parted by
    // turn three to five. Pinned the way --simulate pins it; OD_WORLD_SEED still
    // wins. Derivation and reseeding below are unchanged.
    std::mt19937 seatRng(seed);
    m_agentMapSeed = (unsigned int)(seatRng() & 0x7FFFFFFF);
    if (m_worldSeed == 0) m_worldSeed = m_agentMapSeed;

    startBenchSeat(seatSpec, untilTurn);
    while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE) {
        if (WindowShouldClose()) return false;
        updateLoading();
    }
    if (m_loadingFailed || m_provinceCountryLookup.empty()) {
        fprintf(stderr, "[AGENT] load failed\n");
        return false;
    }
    hideLoadingScreen();
    m_currentScreen = SCREEN_PLAYING;
    m_benchPlayUntilTurn = untilTurn;
    m_agentUntilTurn = untilTurn;

    // ── THE SAME WORLD THE MODEL PLAYED ──
    //
    // Turn logic calls rand() directly (combat rolls, rebellion chances) and
    // raylib's InitWindow seeds that from the wall clock, so without this every
    // agent run is a DIFFERENT game from the one the model was scored on and
    // the comparison is unpaired. That is not a small caveat here: the model's
    // three seeds on 1914:FRA:rush came out 0.3, 9.3 and 9.8 -- a spread far
    // wider than any plausible difference between two players. Same two lines,
    // same reason, as the eval loop; see the note at "SEED THE C PRNG".
    //
    // DERIVED THE WAY THE EVAL DERIVES IT, not used raw. runAIEvaluation seeds
    // an mt19937 with the run's base seed and draws a per-map seed from it --
    // `p.seed = rng() & 0x7FFFFFFF` -- so seeding rand() with the base seed
    // here would produce a DIFFERENT world from the one the model was scored
    // on, while looking for all the world like the same one. The first draw is
    // the first map's seed, which is the map a one-map seat run plays.
    srand(m_agentMapSeed);
    seedSimRng(m_agentMapSeed);

    if (!m_countries.getCountry(m_playerCountryId)) {
        fprintf(stderr, "[AGENT] no seat country\n");
        return false;
    }
    if (!m_ai)
        m_ai = new AISystem(this, m_evalModelOverride.empty()
                                      ? m_dataDir + m_aiModelPath
                                      : m_evalModelOverride);
    return true;
}

Game::AgentPosition Game::agentPosition() {
    AgentPosition p;
    const int cid = m_playerCountryId;
    const Country* me = m_countries.getCountry(cid);
    m_ai->agentRefresh();
    p.turn = m_turnNumber;
    p.until = m_agentUntilTurn;
    if (me) {
        p.name = me->name;
        p.iso = me->isoA3;
        p.treasury = me->treasury;
    }
    p.inc = computeCountryIncome(cid);
    for (int owner : m_provinceCountryLookup) {
        if (owner <= 0 || owner >= REBEL_CID_MIN) continue;
        ++p.owned;
        if (owner == cid) ++p.mine;
    }
    for (const auto& [pid, units] : m_provinceArmies)
        for (const auto& u : units) if (u.countryId == cid) p.army += u.count;
    if (me)
        if (auto rIt = m_relations.find(me->isoA3); rIt != m_relations.end())
            for (const auto& [iso, rel] : rIt->second)
                if (rel.war) p.wars.push_back(iso);
    for (int mod = 0; mod < 4; ++mod) {
        std::vector<bool> legal;
        m_ai->agentLegal(cid, mod, legal);
        for (size_t a = 0; a < legal.size(); ++a)
            if (legal[a]) p.legal[mod].push_back((int)a);
        p.budget[mod] = m_ai->agentBudget(cid, mod);
    }
    return p;
}

std::string Game::agentOwners(std::vector<AgentColor>& colors) {
    // Who owns each province, indexed by province id: uint16 little-endian,
    // base64. With each owner's colour, a viewer can colour the map's own
    // provinces.png the way the timelapse export does.
    std::string raw;
    raw.reserve(m_provinceCountryLookup.size() * 2);
    std::vector<int> seen;
    for (int owner : m_provinceCountryLookup) {
        const int v = owner < 0 ? 0 : (owner > 65535 ? 65535 : owner);
        raw.push_back((char)(v & 0xFF));
        raw.push_back((char)((v >> 8) & 0xFF));
        if (owner > 0) seen.push_back(owner);
    }
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    colors.clear();
    for (int ocid : seen) {
        const Country* oc = m_countries.getCountry(ocid);
        if (!oc) continue;
        colors.push_back({ocid, oc->color.r, oc->color.g, oc->color.b, oc->isoA3});
    }
    return base64(raw);
}

std::string Game::agentPositionText() {
    const AgentPosition p = agentPosition();
    std::string s;
    char buf[512];
    snprintf(buf, sizeof(buf), "\n[AGENT] ===== turn %d/%d  %s (%s) =====\n", p.turn, p.until,
             p.name.c_str(), p.iso.c_str());
    s += buf;
    snprintf(buf, sizeof(buf), "[AGENT] land %lld/%lld (%.2f%% of the world)  army %lld  treasury %.1f\n",
             p.mine, p.owned, p.owned ? 100.0 * (double)p.mine / (double)p.owned : 0.0,
             p.army, p.treasury);
    s += buf;
    snprintf(buf, sizeof(buf), "[AGENT] gross %.1f net %.1f  (army %.1f navy %.1f industry %.1f "
             "research %.1f minorities %.1f)\n", p.inc.total, p.inc.net,
             p.inc.armyExpenses, p.inc.navyExpenses, p.inc.industryUpkeep,
             p.inc.researchCost, p.inc.minorityCosts);
    s += buf;
    std::string wars;
    for (const std::string& w : p.wars) { if (!wars.empty()) wars += " "; wars += w; }
    s += "[AGENT] at war with: " + (wars.empty() ? std::string("(nobody)") : wars) + "\n";
    for (int mod = 0; mod < 4; ++mod) {
        std::string line;
        for (int a : p.legal[mod]) {
            if (!line.empty()) line += "  ";
            line += MODS[mod].letter + std::string(":") + std::to_string(a) + " " + actionName(mod, a);
        }
        std::string label = MODS[mod].label;
        if (label.size() < 8) label.resize(8, ' ');
        s += "[AGENT] " + label + " " + line + "\n";
    }
    // ── THE MODEL'S BUDGET, STATED ──
    //
    // A difference in score is only a difference in judgement if both
    // players get the same number of moves. The policy makes up to
    // agentBudget() picks per module each turn and stops a module when it
    // picks 0; before this, an agent could send any number of actions and
    // the comparison measured the budget rather than the player.
    snprintf(buf, sizeof(buf), "[AGENT] budget e:%d p:%d w:%d n:%d  (a module also ends when it picks 0)\n",
             p.budget[0], p.budget[1], p.budget[2], p.budget[3]);
    s += buf;
    // The whole map, for a live viewer. Off unless asked for: ~3 KB a turn
    // that no other agent wants.
    if (std::getenv("OD_AGENT_OWNERS")) {
        std::vector<AgentColor> colors;
        s += "[AGENT] owners " + agentOwners(colors) + "\n";
        std::string cols;
        for (const AgentColor& c : colors) {
            snprintf(buf, sizeof(buf), "%s%d:%02x%02x%02x:%s", cols.empty() ? "" : " ", c.cid,
                     c.r, c.g, c.b, c.iso.c_str());
            cols += buf;
        }
        s += "[AGENT] colors " + cols + "\n";
    }
    s += "[AGENT] waiting\n";
    return s;
}

std::string Game::agentPositionJson(bool withMap) {
    const AgentPosition p = agentPosition();
    nlohmann::json j;
    j["turn"] = p.turn;
    j["until"] = p.until;
    j["over"] = agentOver();
    j["name"] = p.name;
    j["iso"] = p.iso;
    j["cid"] = m_playerCountryId;
    j["mine"] = p.mine;
    j["owned"] = p.owned;
    j["share"] = p.owned ? 100.0 * (double)p.mine / (double)p.owned : 0.0;
    j["army"] = p.army;
    j["treasury"] = p.treasury;
    j["gross"] = p.inc.total;
    j["net"] = p.inc.net;
    j["war"] = p.wars;
    j["benchShare"] = m_benchScoreShare;
    nlohmann::json legal = nlohmann::json::object(), budget = nlohmann::json::object();
    for (int mod = 0; mod < 4; ++mod) {
        nlohmann::json list = nlohmann::json::array();
        for (int a : p.legal[mod]) list.push_back({{"a", a}, {"name", actionName(mod, a)}});
        legal[MODS[mod].letter] = list;
        budget[MODS[mod].letter] = p.budget[mod];
    }
    j["legal"] = legal;
    j["budget"] = budget;
    if (withMap) {
        std::vector<AgentColor> colors;
        j["owners"] = agentOwners(colors);
        nlohmann::json cj = nlohmann::json::object();
        char hex[8];
        for (const AgentColor& c : colors) {
            snprintf(hex, sizeof(hex), "%02x%02x%02x", c.r, c.g, c.b);
            cj[std::to_string(c.cid)] = {{"hex", hex}, {"iso", c.iso}};
        }
        j["colors"] = cj;
    }
    return j.dump();
}

std::vector<Game::AgentMove> Game::agentPlay(const std::string& cmds) {
    std::vector<AgentMove> out;
    const int cid = m_playerCountryId;
    int used[4] = {0, 0, 0, 0};
    bool passed[4] = {false, false, false, false};
    size_t at = 0;
    while (at < cmds.size()) {
        const size_t comma = cmds.find(',', at);
        std::string tok = cmds.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        at = (comma == std::string::npos) ? cmds.size() : comma + 1;
        while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
        if (tok.size() < 3 || tok[1] != ':') continue;
        const char* found = strchr("epwn", tok[0]);
        if (!found) { out.push_back({tok, "unknown", ""}); continue; }
        const int mod = (int)(found - "epwn");
        const int act = atoi(tok.c_str() + 2);
        std::vector<bool> legal;
        m_ai->agentLegal(cid, mod, legal);
        if (act < 0 || act >= (int)legal.size() || !legal[act]) {
            out.push_back({tok, "refused", "not legal this turn"});
            continue;
        }
        // After the legality check, so a refused token costs nothing -- the
        // policy only ever picks from the legal set, and would not have
        // spent a move on it either.
        if (passed[mod]) {
            out.push_back({tok, "skipped", "this module already picked 0 this turn"});
            continue;
        }
        const int budget = m_ai->agentBudget(cid, mod);
        if (used[mod] >= budget) {
            out.push_back({tok, "over_budget", std::to_string(used[mod]) + " of " + std::to_string(budget)});
            continue;
        }
        ++used[mod];
        if (act == 0) passed[mod] = true;
        out.push_back({tok, "did", m_ai->agentExec(cid, mod, act)});
        m_ai->agentRefresh();
    }
    return out;
}

bool Game::agentEndTurn() {
    processTurn();
    PollInputEvents();
    return !agentOver();
}
