// Espionage and counter-espionage: fog-of-war worlds only (Phase 6 of
// docs/design/fog-of-war-plan.md).
//
// Two operations a country can run against another:
//   - a NETWORK: agents across their territory. On success the spy sees every
//     province the target holds, as if it were its own, for kGrantTurns.
//   - a DOSSIER: their books. On success the spy reads the target's figures
//     (and, for the AI, its true statistics) for kGrantTurns.
// Each takes a turn or two to come back, costs money up front, and can fail.
// A failure is noticed by the target, and named when its counter-intelligence
// is good enough to say whose agents they were.
//
// THE ROLL IS A HASH, NOT THE TURN RNG. g_simRng is the stream every other
// system draws from, and a draw here would shift every roll after it -- in fog
// worlds only, but a save would then replay differently depending on whether a
// spy had been launched. A hash of (spy, target, kind, launch turn) is as
// unpredictable to a player and moves nothing else.

#include "Game.h"
#include "GameInternals.h"
#include "Audio.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int   kGrantTurns      = 6;
constexpr float kBaseChance      = 0.60f;
constexpr float kNetworkBaseCost = 30.0f;
constexpr float kDossierBaseCost = 20.0f;
constexpr int   kCounterMaxLevel = 3;
constexpr float kCounterPerLevel = 10.0f;    // points off a foreign operation's chance
constexpr float kCounterCostFrac = 0.02f;    // of gross income, per level, per turn
constexpr int   kNamedAtCounter  = 20;       // counter points at which a caught spy is named

uint64_t mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

uint64_t pairKey(int a, int b) { return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b; }

}  // namespace

const char* Game::spyKindName(uint8_t kind) {
    return kind == SPY_NETWORK ? "spy network" : "dossier";
}

int Game::counterIntelLevel(int cid) const {
    auto it = m_counterIntelLevel.find(cid);
    return it == m_counterIntelLevel.end() ? 0 : it->second;
}

float Game::counterIntelPoints(int cid) const {
    return (float)counterIntelLevel(cid) * kCounterPerLevel +
           std::max(0.0f, getResearchEffect("counterIntelPct", cid));
}

float Game::counterIntelCost(int cid) const {
    const int level = counterIntelLevel(cid);
    if (level <= 0) return 0.0f;
    return std::max(1.0f, computeCountryIncome(cid).total * kCounterCostFrac) * (float)level;
}

void Game::setCounterIntelLevel(int cid, int level) {
    level = std::clamp(level, 0, kCounterMaxLevel);
    if (level == 0) m_counterIntelLevel.erase(cid);
    else m_counterIntelLevel[cid] = level;
}

float Game::spyChance(int spy, int target) const {
    const float p = kBaseChance + std::max(0.0f, getResearchEffect("spySuccessPct", spy)) / 100.0f -
                    counterIntelPoints(target) / 100.0f;
    return std::clamp(p, 0.10f, 0.95f);
}

float Game::spyCost(int spy, int target, uint8_t kind) const {
    // Scaled by what there is to cover: a network across a continent costs
    // more than one across an island. The size is what the spy KNOWS of them.
    int known = 0;
    const std::vector<int>& owners = (spy == m_playerCountryId) ? displayOwners() : m_provinceCountryLookup;
    for (size_t pid = 1; pid < owners.size(); ++pid)
        if (owners[pid] == target) ++known;
    const float base = kind == SPY_NETWORK ? kNetworkBaseCost : kDossierBaseCost;
    const float cut = std::clamp(getResearchEffect("spyCostPct", spy), 0.0f, 90.0f) / 100.0f;
    return base * std::sqrt((float)std::max(1, known)) * (1.0f - cut);
}

bool Game::spyOpPending(int spy, int target, uint8_t kind) const {
    for (const SpyOp& op : m_spyOps)
        if (op.spy == spy && op.target == target && op.kind == kind) return true;
    return false;
}

bool Game::fogNetwork(int spy, int target) const {
    auto it = m_intelGrants.find(pairKey(spy, target));
    return it != m_intelGrants.end() && it->second.networkUntil >= m_turnNumber;
}

bool Game::fogDossier(int spy, int target) const {
    auto it = m_intelGrants.find(pairKey(spy, target));
    return it != m_intelGrants.end() && it->second.dossierUntil >= m_turnNumber;
}

std::string Game::spyRefusal(int spy, int target, uint8_t kind) const {
    if (!m_fogOfWar) return T("Spying exists only in fog-of-war worlds.");
    if (spy == target || !fogCountry(spy) || !fogCountry(target)) return T("Not a country you can spy on.");
    if (sharesIntel(spy, target)) return T("You already share intelligence with them.");
    if (spyOpPending(spy, target, kind)) return T("An operation is already under way.");
    if (kind == SPY_NETWORK ? fogNetwork(spy, target) : fogDossier(spy, target))
        return T("Your agents are already in place.");
    const Country* c = m_countries.getCountry(spy);
    if (!c || c->treasury < spyCost(spy, target, kind)) return T("Not enough money.");
    return {};
}

bool Game::launchSpyOp(int spy, int target, uint8_t kind) {
    if (!spyRefusal(spy, target, kind).empty()) return false;
    const float cost = spyCost(spy, target, kind);
    m_countries.getAll()[spy].treasury -= cost;
    SpyOp op;
    op.spy = spy;
    op.target = target;
    op.kind = kind;
    op.launchTurn = m_turnNumber;
    // A network has to be built; a file only has to be found.
    op.resolveTurn = m_turnNumber + (kind == SPY_NETWORK ? 2 : 1);
    m_spyOps.push_back(op);
    return true;
}

void Game::processSpyOps() {
    if (!m_fogOfWar) return;

    // OD_SPY_TEST=SPY,TARGET,KIND (ISO codes, kind 0 network / 1 dossier):
    // launch one operation on the first turn, for a headless --simulate check.
    if (const char* t = OD_ENV("OD_SPY_TEST"); t && !m_spyTestDone) {
        m_spyTestDone = true;
        char a[8] = {}, b[8] = {};
        int kind = 0;
        if (sscanf(t, "%7[^,],%7[^,],%d", a, b, &kind) == 3) {
            int sc = 0, tc = 0;
            for (const auto& [cid, c] : m_countries.getAll()) {
                if (c.isoA3 == a) sc = cid;
                if (c.isoA3 == b) tc = cid;
            }
            m_countries.getAll()[sc].treasury += spyCost(sc, tc, (uint8_t)kind);
            const bool ok = sc > 0 && tc > 0 && launchSpyOp(sc, tc, (uint8_t)kind);
            printf("[SPY] test launch %s -> %s kind %d: %s%s\n", a, b, kind, ok ? "ok" : "refused: ",
                   ok ? "" : spyRefusal(sc, tc, (uint8_t)kind).c_str());
        }
    }

    // The standing bill for counter-intelligence, paid every turn it is kept.
    for (auto& [cid, level] : m_counterIntelLevel) {
        (void)level;
        auto& all = m_countries.getAll();
        auto it = all.find(cid);
        if (it == all.end()) continue;
        it->second.treasury -= counterIntelCost(cid);
    }

    std::vector<SpyOp> still;
    for (const SpyOp& op : m_spyOps) {
        if (op.resolveTurn > m_turnNumber) { still.push_back(op); continue; }
        if (!m_countries.getCountry(op.spy) || !m_countries.getCountry(op.target)) continue;
        const float p = spyChance(op.spy, op.target);
        const uint64_t h = mix(pairKey(op.spy, op.target) ^ mix(((uint64_t)op.kind << 32) |
                                                                  (uint32_t)op.launchTurn));
        const bool success = (double)(h >> 11) * (1.0 / 9007199254740992.0) < (double)p;
        const std::string spyName = od::i18n::properName(m_countries.getCountry(op.spy)->name);
        const std::string targetName = od::i18n::properName(m_countries.getCountry(op.target)->name);
        if (success) {
            IntelGrant& g = m_intelGrants[pairKey(op.spy, op.target)];
            (op.kind == SPY_NETWORK ? g.networkUntil : g.dossierUntil) = m_turnNumber + kGrantTurns;
            if (op.spy == m_playerCountryId) {
                // Two statements, not a wrapped ternary: tools/i18n_extract.py
                // reads one line at a time and loses a ternary's second label.
                const char* fmt = T("Your agents have %s's books: their figures are open to you for %d turns.");
                if (op.kind == SPY_NETWORK)
                    fmt = T("Your spy network in %s is in place: you see their land for %d turns.");
                addNotification(TextFormat(fmt, targetName.c_str(), kGrantTurns),
                                Color{150, 210, 170, 255}, 8.0f);
                Audio::get().playSfx("deal_accepted");
            }
            if (OD_ENV("OD_SPY_TRACE"))
                printf("[SPY] turn %d %d -> %d %s success (p %.2f)\n", m_turnNumber, op.spy,
                       op.target, spyKindName(op.kind), p);
        } else {
            m_spyCaught[pairKey(op.target, op.spy)] = m_turnNumber;
            const bool named = counterIntelPoints(op.target) >= (float)kNamedAtCounter;
            if (op.spy == m_playerCountryId) {
                addNotification(TextFormat(T("Your agents in %s were caught. The operation failed."),
                                           targetName.c_str()),
                                Color{235, 130, 90, 255}, 8.0f);
                Audio::get().playSfx("deal_rejected");
            }
            if (op.target == m_playerCountryId) {
                std::string msg = T("Your counter-intelligence caught foreign spies.");
                if (named) msg = TextFormat(T("Your counter-intelligence caught spies from %s."), spyName.c_str());
                addNotification(msg, Color{235, 200, 110, 255}, 8.0f);
            }
            if (OD_ENV("OD_SPY_TRACE"))
                printf("[SPY] turn %d %d -> %d %s caught%s (p %.2f)\n", m_turnNumber, op.spy,
                       op.target, spyKindName(op.kind), named ? ", named" : "", p);
        }
    }
    m_spyOps.swap(still);

    // Expired grants go, so the map stays small.
    for (auto it = m_intelGrants.begin(); it != m_intelGrants.end();) {
        if (it->second.networkUntil < m_turnNumber && it->second.dossierUntil < m_turnNumber)
            it = m_intelGrants.erase(it);
        else ++it;
    }
}

int Game::spyCaughtTurn(int target, int spy) const {
    auto it = m_spyCaught.find(pairKey(target, spy));
    return it == m_spyCaught.end() ? -1 : it->second;
}

void Game::writeSpyState(nlohmann::json& j) const {
    if (!m_fogOfWar) return;
    if (OD_ENV("OD_SPY_TRACE"))
        printf("[SPY] write turn %d: %zu op(s), %zu grant(s)\n", m_turnNumber, m_spyOps.size(),
               m_intelGrants.size());
    nlohmann::json ops = nlohmann::json::array();
    for (const SpyOp& op : m_spyOps)
        ops.push_back({{"spy", op.spy}, {"target", op.target}, {"kind", op.kind},
                       {"launch", op.launchTurn}, {"resolve", op.resolveTurn}});
    j["spyOps"] = ops;
    nlohmann::json grants = nlohmann::json::array();
    for (const auto& [key, g] : m_intelGrants)
        grants.push_back({{"spy", (int)(key >> 32)}, {"target", (int)(uint32_t)key},
                          {"network", g.networkUntil}, {"dossier", g.dossierUntil}});
    // Sorted: an unordered_map's order is not a property of the world, and a
    // save that differs by iteration order differs for no reason.
    std::sort(grants.begin(), grants.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return std::make_pair(a["spy"].get<int>(), a["target"].get<int>()) <
               std::make_pair(b["spy"].get<int>(), b["target"].get<int>());
    });
    j["intelGrants"] = grants;
    nlohmann::json counter = nlohmann::json::object();
    for (const auto& [cid, level] : m_counterIntelLevel) counter[std::to_string(cid)] = level;
    j["counterIntel"] = counter;
    nlohmann::json caught = nlohmann::json::array();
    for (const auto& [key, turn] : m_spyCaught)
        caught.push_back({(int)(key >> 32), (int)(uint32_t)key, turn});
    std::sort(caught.begin(), caught.end());
    j["spyCaught"] = caught;
}

void Game::readSpyState(const nlohmann::json& j) {
    m_spyOps.clear();
    m_intelGrants.clear();
    m_counterIntelLevel.clear();
    m_spyCaught.clear();
    auto num = [](const nlohmann::json& o, const char* k, int dflt) {
        return (o.contains(k) && o[k].is_number_integer()) ? o[k].get<int>() : dflt;
    };
    if (j.contains("spyOps") && j["spyOps"].is_array())
        for (const auto& o : j["spyOps"]) {
            if (!o.is_object()) continue;
            SpyOp op;
            op.spy = num(o, "spy", 0);
            op.target = num(o, "target", 0);
            op.kind = (uint8_t)std::clamp(num(o, "kind", 0), 0, 1);
            op.launchTurn = num(o, "launch", 0);
            op.resolveTurn = num(o, "resolve", 0);
            if (op.spy > 0 && op.target > 0) m_spyOps.push_back(op);
        }
    if (j.contains("intelGrants") && j["intelGrants"].is_array())
        for (const auto& o : j["intelGrants"]) {
            if (!o.is_object()) continue;
            IntelGrant g;
            g.networkUntil = num(o, "network", -1);
            g.dossierUntil = num(o, "dossier", -1);
            m_intelGrants[pairKey(num(o, "spy", 0), num(o, "target", 0))] = g;
        }
    if (j.contains("counterIntel") && j["counterIntel"].is_object())
        for (const auto& [k, v] : j["counterIntel"].items())
            if (v.is_number_integer()) setCounterIntelLevel(std::atoi(k.c_str()), v.get<int>());
    if (j.contains("spyCaught") && j["spyCaught"].is_array())
        for (const auto& e : j["spyCaught"])
            if (e.is_array() && e.size() == 3 && e[0].is_number_integer() && e[1].is_number_integer() &&
                e[2].is_number_integer())
                m_spyCaught[pairKey(e[0].get<int>(), e[1].get<int>())] = e[2].get<int>();
}

// The Intelligence block on a foreign country's profile: what the player knows
// of them, what is under way, and the two operations. Returns the next y.
int Game::drawSpySection(int cid, int x, int y, int w) {
    if (!m_fogOfWar || !fogCountry(m_playerCountryId)) return y;
    const Vector2 mouse = getMouse();
    const bool click = !m_paused && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    const Color dim{150, 156, 176, 255};
    DrawText(T("Intelligence"), x, y, 16, hexToColor(m_config.accent()));
    y += 24;

    if (cid == m_playerCountryId) {
        // Our own side of it: what we spend keeping theirs out.
        const int level = counterIntelLevel(cid);
        DrawText(TextFormat(T("Counter-intelligence: level %d of %d  (%s per turn)"), level,
                            kCounterMaxLevel, formatBalance(counterIntelCost(cid)).c_str()),
                 x, y + 4, 13, WHITE);
        DrawText(TextFormat(T("Foreign operations against you succeed %.0f points less often."),
                            counterIntelPoints(cid)),
                 x, y + 22, 12, dim);
        auto button = [&](float bx, const char* label, bool enabled) {
            const Rectangle r{bx, (float)y, 30.0f, 24.0f};
            const bool hov = enabled && CheckCollisionPointRec(mouse, r);
            DrawRectangleRounded(r, 0.25f, 5, hov ? Color{40, 46, 62, 230} : Color{24, 27, 36, 210});
            DrawRectangleRoundedLines(r, 0.25f, 5, Color{62, 66, 86, 190});
            DrawText(label, (int)(r.x + 15 - MeasureText(label, 16) / 2), y + 4, 16,
                     enabled ? WHITE : Color{90, 94, 110, 255});
            return hov && click;
        };
        const float bx = (float)(x + w - 72);
        if (button(bx, "-", level > 0)) { setCounterIntelLevel(cid, level - 1); Audio::get().playSfx("click_soft"); }
        if (button(bx + 38, "+", level < kCounterMaxLevel)) { setCounterIntelLevel(cid, level + 1); Audio::get().playSfx("click_soft"); }
        return y + 46;
    }

    if (sharesIntel(m_playerCountryId, cid)) {
        DrawText(T("You share intelligence with them: you see what they see."), x, y, 12, dim);
        return y + 22;
    }
    if (const int caught = spyCaughtTurn(cid, m_playerCountryId); caught >= 0)
        DrawText(TextFormat(T("They caught your agents on turn %d."), caught), x, y, 12,
                 Color{235, 150, 110, 255}), y += 18;

    struct Op { uint8_t kind; const char* label; const char* what; };
    const Op ops[] = {
        {SPY_NETWORK, T("Plant a spy network"), T("See all their land")},
        {SPY_DOSSIER, T("Steal their dossier"), T("Read their figures")},
    };
    for (const Op& op : ops) {
        const bool active = op.kind == SPY_NETWORK ? fogNetwork(m_playerCountryId, cid)
                                                   : fogDossier(m_playerCountryId, cid);
        const bool pending = spyOpPending(m_playerCountryId, cid, op.kind);
        const std::string why = spyRefusal(m_playerCountryId, cid, op.kind);
        const Rectangle r{(float)x, (float)y, (float)w, 28.0f};
        const bool can = why.empty();
        const bool hov = can && CheckCollisionPointRec(mouse, r);
        DrawRectangleRounded(r, 0.18f, 5, active ? Color{28, 46, 36, 220}
                                        : hov ? Color{40, 46, 62, 230} : Color{18, 20, 27, 200});
        DrawRectangleRoundedLines(r, 0.18f, 5, active ? Color{86, 170, 106, 200} : Color{58, 62, 80, 170});
        DrawText(op.label, x + 12, y + 7, 13, can || active ? WHITE : Color{130, 134, 150, 255});
        std::string right;
        if (active) {
            auto g = m_intelGrants.find(pairKey(m_playerCountryId, cid));
            const int until = g == m_intelGrants.end() ? m_turnNumber
                            : (op.kind == SPY_NETWORK ? g->second.networkUntil : g->second.dossierUntil);
            right = TextFormat(T("in place, %d turn(s) left"), std::max(0, until - m_turnNumber));
        } else if (pending) {
            right = T("under way");
        } else {
            right = TextFormat(T("%s -- $%s, %.0f%% chance"), op.what,
                               formatBalance(spyCost(m_playerCountryId, cid, op.kind)).c_str(),
                               spyChance(m_playerCountryId, cid) * 100.0f);
            if (!can) right = why;
        }
        const int rw = MeasureText(right.c_str(), 12);
        DrawText(right.c_str(), x + w - rw - 12, y + 8, 12, active ? Color{180, 215, 190, 255} : dim);
        if (hov && click && launchSpyOp(m_playerCountryId, cid, op.kind)) {
            Audio::get().playSfx("click_heavy");
            addNotification(TextFormat(T("Operation launched against %s."),
                                       od::i18n::properName(m_countries.getCountry(cid)->name).c_str()),
                            Color{200, 200, 220, 255}, 5.0f);
        }
        y += 34;
    }
    return y + 6;
}
