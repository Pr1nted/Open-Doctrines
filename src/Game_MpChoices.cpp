// ─────────────────────────────────────────────────────────────────────────────
// What a remote player decides about their COUNTRY, and how it reaches the host.
//
// An order is a thing done to the map: build here, march there, sail that.
// Those always travelled (mpSerializeOrders / mpApplyOrders). Everything else a
// player decides -- what research to fund and how much, which doctrines to
// adopt or repeal, which diplomacy to open, what to publish about themselves,
// how to treat their minorities, how to divide their country, what to claim --
// was set on the client's screen and stopped there. The host never heard of
// it, and the client's own screen was overwritten by the next TurnState.
//
// For a long time that was invisible, because the AI played every seat on a
// dedicated server and made all of those choices itself. Commit ac4cbda
// stopped it acting for seats a human holds (m_mpHumanCids), which was right,
// and which meant those choices were then made by nobody at all: a player's
// research budget sat wherever the AI had left it on the day they joined.
//
// The same JSON carries them now, under new keys:
//
//   budgets     {"research": f, "pacification": f}
//   research    [{"node": id, "share": n, "auto": b}] -- one per group
//   policies    [{"op": "enact"|"cancel", "id", "province", "minority"}]
//   diplomacy   [{"action", "target", "subject", "goal", "terms"}]
//   disclosure  bits
//   ethnic      {minority: [option per category]}
//   districts   [district...] -- the whole partition
//   claims      {"add": [...], "drop": [...]}
//   answers     [{"seq", "accept", "reason"}] -- to requests the host forwarded
//
// TWO RULES, and both are older than this file:
//
//   * A KEY THAT IS ABSENT CHANGES NOTHING. An older client sends none of
//     these, and must find its country exactly as the host left it -- never
//     cleared, never reset to a default.
//   * EVERYTHING IS HOSTILE UNTIL CHECKED. The client is whatever the person on
//     the other end is running. Each value goes through the same rule the
//     screen would have applied, on the host, against the host's world.
//
// And the other direction: diplomacy aimed AT a remote player used to be
// answered by the AI, since every "is this the player" test in the turn loop
// named m_playerCountryId and a remote seat is never that. Those requests now
// wait in m_mpInbox, go out with that seat's TurnState, and are settled by the
// answer in its next orders.
// ─────────────────────────────────────────────────────────────────────────────

#include "Game.h"
#include "GameInternals.h"
#include "ai/AISystem.h"
#include "json.hpp"
#include "net/Host.h"
#include "net/Lobby.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

// Game_TurnLogic.cpp: "an alliance", "a non-aggression pact" -- the words the
// local player's notifications use, so a remote answer reads the same.
const char* diploRequestPhrase(const std::string& action);

namespace {

using nlohmann::json;

/// A province id as the wire may carry one: Province::colorToId is 24-bit.
constexpr long long kMaxPid = 0xFFFFFF;

/// An integer field: `missing` when it is absent, -1 when it is anything but
/// an integer (every caller reads a negative as out of range). json::value()
/// THROWS on a key of the wrong type, and a throw out of the middle of a
/// hostile payload would leave half of it applied.
long long intOr(const json& e, const char* k, long long missing) {
    if (!e.is_object() || !e.contains(k)) return missing;
    const json& v = e[k];
    return v.is_number_integer() ? v.get<long long>() : -1;
}

json intList(const std::vector<int>& v) {
    json a = json::array();
    for (int x : v) a.push_back(x);
    return a;
}

json termsToJson(const CeasefireTerms& t) {
    json j;
    j["ourMoney"] = t.ourMoney;
    j["theirMoney"] = t.theirMoney;
    j["ourProvs"] = intList(t.ourProvs);
    j["theirProvs"] = intList(t.theirProvs);
    j["ourDropClaims"] = intList(t.ourDropClaims);
    j["theirDropClaims"] = intList(t.theirDropClaims);
    j["ourReleaseTag"] = t.ourReleaseTag;
    j["ourReleaseProvs"] = intList(t.ourReleaseProvs);
    j["theirReleaseTag"] = t.theirReleaseTag;
    j["theirReleaseProvs"] = intList(t.theirReleaseProvs);
    j["ourGoods"] = json::array();
    j["theirGoods"] = json::array();
    for (int g = 0; g < 4; ++g) {
        j["ourGoods"].push_back(t.ourGoods[g]);
        j["theirGoods"].push_back(t.theirGoods[g]);
    }
    return j;
}

/**
 * Terms off the wire -- or out of a save, which is the same reader with a
 * longer leash.
 *
 * STRICT: anything of the wrong type or out of range fails the whole set
 * rather than being dropped from it. A deal is read by the other side as a
 * whole, and a version of it with one province quietly missing is a deal
 * nobody offered.
 */
bool termsFromJson(const json& j, CeasefireTerms& out, size_t maxList) {
    if (!j.is_object()) return false;
    CeasefireTerms t;
    auto money = [&](const char* k, int& v) {
        if (!j.contains(k)) return true;
        if (!j[k].is_number_integer()) return false;
        const long long n = j[k].get<long long>();
        if (n < 0 || n > 1000000000LL) return false;
        v = (int)n;
        return true;
    };
    auto list = [&](const char* k, std::vector<int>& v) {
        if (!j.contains(k)) return true;
        if (!j[k].is_array() || j[k].size() > maxList) return false;
        for (const auto& e : j[k]) {
            if (!e.is_number_integer()) return false;
            const long long n = e.get<long long>();
            if (n < 1 || n > kMaxPid) return false;
            if (std::find(v.begin(), v.end(), (int)n) == v.end()) v.push_back((int)n);
        }
        return true;
    };
    auto tag = [&](const char* k, std::string& v) {
        if (!j.contains(k)) return true;
        if (!j[k].is_string()) return false;
        v = j[k].get<std::string>();
        return v.size() <= 64;
    };
    auto goods = [&](const char* k, float* v) {
        if (!j.contains(k)) return true;
        if (!j[k].is_array() || j[k].size() > 4) return false;
        for (size_t g = 0; g < j[k].size(); ++g) {
            if (!j[k][g].is_number()) return false;
            const double d = j[k][g].get<double>();
            // NaN fails both halves of this, which is the point of writing it so.
            if (!(d >= 0.0 && d <= 1e9)) return false;
            v[g] = (float)d;
        }
        return true;
    };
    if (!money("ourMoney", t.ourMoney) || !money("theirMoney", t.theirMoney) ||
        !list("ourProvs", t.ourProvs) || !list("theirProvs", t.theirProvs) ||
        !list("ourDropClaims", t.ourDropClaims) || !list("theirDropClaims", t.theirDropClaims) ||
        !tag("ourReleaseTag", t.ourReleaseTag) || !list("ourReleaseProvs", t.ourReleaseProvs) ||
        !tag("theirReleaseTag", t.theirReleaseTag) || !list("theirReleaseProvs", t.theirReleaseProvs) ||
        !goods("ourGoods", t.ourGoods) || !goods("theirGoods", t.theirGoods))
        return false;
    // A release with no nation, or a nation with no ground, is not a release.
    if (t.ourReleaseTag.empty() != t.ourReleaseProvs.empty()) return false;
    if (t.theirReleaseTag.empty() != t.theirReleaseProvs.empty()) return false;
    out = std::move(t);
    return true;
}

/// Shares that add to 100 over the first `unlocked` groups, largest
/// remainder -- the same promise normaliseResearchShares makes the player.
std::array<int, 3> normaliseShares(std::array<int, 3> in, int unlocked) {
    std::array<int, 3> out{{0, 0, 0}};
    int sum = 0;
    for (int g = 0; g < unlocked; ++g) sum += std::max(0, in[(size_t)g]);
    if (sum <= 0) {
        for (int g = 0; g < unlocked; ++g) in[(size_t)g] = 1;
        sum = unlocked;
    }
    int handed = 0;
    std::array<double, 3> rem{{0, 0, 0}};
    for (int g = 0; g < unlocked; ++g) {
        const double exact = 100.0 * std::max(0, in[(size_t)g]) / sum;
        out[(size_t)g] = (int)exact;
        rem[(size_t)g] = exact - out[(size_t)g];
        handed += out[(size_t)g];
    }
    while (handed < 100) {
        int best = 0;
        for (int g = 1; g < unlocked; ++g) if (rem[(size_t)g] > rem[(size_t)best]) best = g;
        out[(size_t)best]++;
        rem[(size_t)best] = -1.0;
        handed++;
    }
    return out;
}

}  // namespace

// One request, as the wire and the save both carry it.
nlohmann::json Game::mpInboxItemJson(const MpInboxItem& it) {
    json e;
    e["seq"] = it.seq;
    e["action"] = it.action;
    e["source"] = it.sourceIso;
    e["target"] = it.targetIso;
    if (!it.subjectIso.empty()) e["subject"] = it.subjectIso;
    if (it.goal != WAR_GOAL_NONE) e["goal"] = it.goal;
    if (it.hasTerms) e["terms"] = termsToJson(it.terms);
    e["turn"] = it.turn;
    return e;
}

bool Game::mpInboxItemFromJson(const nlohmann::json& e, MpInboxItem& it) {
    try {
    if (!e.is_object() || !e.contains("seq") || !e["seq"].is_number_unsigned()) return false;
    it.seq = e["seq"].get<uint32_t>();
    it.action = e.value("action", std::string());
    it.sourceIso = e.value("source", std::string());
    it.targetIso = e.value("target", std::string());
    it.subjectIso = e.value("subject", std::string());
    it.goal = e.value("goal", (int)WAR_GOAL_NONE);
    if (it.goal < 0 || it.goal >= WAR_GOAL_COUNT) it.goal = WAR_GOAL_NONE;
    it.turn = e.value("turn", 0);
    it.hasTerms = e.contains("terms") && termsFromJson(e["terms"], it.terms, 1u << 20);
    return !it.action.empty() && !it.sourceIso.empty() && !it.targetIso.empty();
    } catch (const std::exception&) {
        return false;      // a field of the wrong type: not a request we can show
    }
}

// ───────────────────────────────────────────────────────── the client side ──

void Game::mpSerializeChoices(int countryId, nlohmann::json& j) const {
    // Only ever this machine's own country: the globals below ARE the player's
    // screen, and they mean nothing for anybody else.
    if (countryId <= 0 || countryId != m_playerCountryId) return;
    const Country* me = m_countries.getCountry(countryId);
    if (!me) return;

    j["budgets"] = {{"research", m_researchAllocation},
                    {"pacification", m_pacificationAllocation}};

    // By node ID, never by index: the index is a position in this build's
    // tree, and a host on a build that added one node in the middle would
    // read every choice as the one beside it.
    json groups = json::array();
    for (int g = 0; g < RESEARCH_GROUPS_MAX; ++g) {
        const ResearchGroup& grp = m_researchGroups[g];
        json e;
        e["node"] = (grp.activeNode >= 0 && grp.activeNode < (int)m_researchNodes.size())
                        ? m_researchNodes[grp.activeNode].id : std::string();
        e["share"] = std::clamp(grp.sharePct, 0, 100);
        e["auto"] = grp.autoAdvance;
        groups.push_back(e);
    }
    j["research"] = groups;

    for (const MpPolicyOp& op : m_mpPolicyOps) {
        json e;
        e["op"] = op.enact ? "enact" : "cancel";
        e["id"] = op.id;
        if (op.province > 0) e["province"] = op.province;
        if (!op.minority.empty()) e["minority"] = op.minority;
        j["policies"].push_back(e);
    }

    // Our own overtures, from the queue the panel writes to. The bookkeeping
    // halves of an answer (apply_*, cancel) are not overtures and are never
    // the host's to take from a client.
    for (const PendingDiplomaticAction& da : m_pendingDiplomaticActions) {
        if (da.sourceIso != me->isoA3) continue;
        if (da.action == "apply_ceasefire" || da.action == "apply_trade" || da.action == "cancel")
            continue;
        json e;
        e["action"] = da.action;
        e["target"] = da.targetIso;
        if (!da.subjectIso.empty()) e["subject"] = da.subjectIso;
        if (da.action == "declare_war") e["goal"] = da.statedGoal;
        if (da.action == "request_ceasefire" || da.action == "propose_trade") {
            auto t = m_pendingCeasefireTerms.find(da.sourceIso + "|" + da.targetIso);
            if (t != m_pendingCeasefireTerms.end()) e["terms"] = termsToJson(t->second);
        }
        j["diplomacy"].push_back(e);
    }

    {
        auto d = m_countryDisclosure.find(countryId);
        j["disclosure"] = d == m_countryDisclosure.end() ? 0u : d->second;
    }
    if (auto e = m_ethnicPolicies.find(countryId); e != m_ethnicPolicies.end() && !e->second.empty()) {
        json rows = json::object();
        for (const auto& [name, opts] : e->second) rows[name] = opts;
        j["ethnic"] = rows;
    }
    // Only a country that HAS districts sends them. One that never opened the
    // tab has none on either side, and sending an empty list would read as
    // "undivide me" to a host holding the map author's division.
    if (auto d = m_districts.find(countryId); d != m_districts.end() && !d->second.empty()) {
        json arr = json::array();
        for (const District& dd : d->second) {
            json e;
            e["id"] = dd.id;
            e["name"] = dd.name;
            if (dd.customName) e["custom_name"] = true;
            e["r"] = (int)dd.r; e["g"] = (int)dd.g; e["b"] = (int)dd.b;
            e["share"] = dd.sharePct;
            e["provinces"] = dd.provinces;
            if (!dd.policies.empty()) e["policies"] = dd.policies;
            arr.push_back(e);
        }
        j["districts"] = arr;
    }
    if (!m_claimsPendingAdd.empty() || !m_claimsPendingDrop.empty())
        j["claims"] = {{"add", m_claimsPendingAdd}, {"drop", m_claimsPendingDrop}};
    for (const MpDiploAnswer& a : m_mpDiploAnswers)
        j["answers"].push_back({{"seq", a.seq}, {"accept", a.accept}, {"reason", a.reason}});
}

void Game::mpClearSentChoices() {
    // The turn these were for has resolved; the host's verdict on each of them
    // is in the delta just applied and the TurnState behind it.
    m_mpPolicyOps.clear();
    m_mpDiploAnswers.clear();
    // The three-a-turn limit on enacting doctrines is counted here and reset
    // by updatePolicies(), which runs inside processTurn -- and a client never
    // runs that. Without this a player could enact three doctrines for the
    // whole game, and then the Enact button said "No actions" for ever.
    m_policiesEnactedThisTurn = 0;
    m_claimsPendingAdd.clear();
    m_claimsPendingDrop.clear();
    // ALL of the diplomatic queue, not only our own overtures. A client never
    // resolves diplomacy, so nothing in its queue is ever consumed: what it
    // sent is settled now, an accepted offer's apply_* half was the host's to
    // queue, and the rest came with the snapshot it joined on -- every other
    // country's pending business, frozen at the moment of joining, which made
    // the panel refuse a request "already in talks" for the rest of the game.
    m_pendingDiplomaticActions.clear();
    m_pendingCeasefireTerms.clear();
    m_acceptedCeasefireTerms.clear();
}

void Game::mpMirrorOwnCountryRows(const nlohmann::json& st) {
    const int me = m_playerCountryId;
    if (me <= 0 || !st.is_object()) return;
    const std::string key = std::to_string(me);
    // ABSENT IS ZERO. saveStateJson writes these rows only when they are
    // non-zero, so a budget the host cut to nothing arrives as a missing key
    // -- and reading "missing" as "unchanged" would leave the slider showing
    // money the country is no longer spending.
    auto row = [&](const char* k) -> const json* {
        if (!st.contains(k) || !st[k].is_object() || !st[k].contains(key)) return nullptr;
        return &st[k][key];
    };
    auto number = [&](const char* k) -> double {
        const json* v = row(k);
        return v && v->is_number() ? v->get<double>() : 0.0;
    };
    auto nodeIndex = [&](const std::string& id) {
        for (int i = 0; i < (int)m_researchNodes.size(); ++i)
            if (m_researchNodes[i].id == id) return i;
        return -1;
    };

    m_researchAllocation = std::clamp((float)number("countryResearchAlloc"), 0.0f, 1.0f);
    m_pacificationAllocation = std::clamp((float)number("pacification"), 0.0f, 1.0f);
    m_researchPoints = (int)number("countryResearchPoints");

    int active[RESEARCH_GROUPS_MAX] = {-1, -1, -1};
    int invested[RESEARCH_GROUPS_MAX] = {0, 0, 0};
    if (const json* a = row("countryResearchActive"); a && a->is_string())
        active[0] = nodeIndex(a->get<std::string>());
    invested[0] = (int)number("countryResearchInvested");
    if (const json* x = row("countryResearchExtra"); x && x->is_array())
        for (size_t g = 0; g + 1 < RESEARCH_GROUPS_MAX && g < x->size(); ++g) {
            const json& e = (*x)[g];
            if (!e.is_object()) continue;
            active[g + 1] = nodeIndex(e.value("node", std::string()));
            invested[g + 1] = e.value("invested", 0);
        }

    for (auto& n : m_researchNodes) {
        n.researched = hasResearched(n.id, me);
        n.inProgress = false;
        n.invested = 0;
    }
    for (int g = 0; g < RESEARCH_GROUPS_MAX; ++g) {
        m_researchGroups[g].activeNode = active[g];
        if (active[g] >= 0) {
            m_researchNodes[active[g]].inProgress = true;
            m_researchNodes[active[g]].invested = invested[g];
        }
    }
    if (const json* p = row("countryResearchPlan"); p && p->is_object()) {
        for (int g = 0; g < RESEARCH_GROUPS_MAX; ++g) {
            if (p->contains("share") && (*p)["share"].is_array() && (size_t)g < (*p)["share"].size() &&
                (*p)["share"][g].is_number_integer())
                m_researchGroups[g].sharePct = std::clamp((*p)["share"][g].get<int>(), 0, 100);
            if (p->contains("auto") && (*p)["auto"].is_array() && (size_t)g < (*p)["auto"].size() &&
                (*p)["auto"][g].is_boolean())
                m_researchGroups[g].autoAdvance = (*p)["auto"][g].get<bool>();
            if (p->contains("last") && (*p)["last"].is_array() && (size_t)g < (*p)["last"].size() &&
                (*p)["last"][g].is_string())
                m_researchGroups[g].lastNode = nodeIndex((*p)["last"][g].get<std::string>());
        }
    }
    m_mpMirroredTurn = m_turnNumber;
}

void Game::mpTakeInbox(const nlohmann::json& inbox) {
    const Country* me = m_countries.getCountry(m_playerCountryId);
    if (!me) return;
    // ── WHAT IS NO LONGER BEING ASKED ──
    //
    // The host sends a seat everything it is still waiting on, every turn. A
    // forwarded request missing from that list has been settled -- answered,
    // or declined for want of an answer -- and a popup for it still on screen
    // would collect an answer the host will never read.
    std::unordered_set<uint32_t> live;
    if (inbox.is_array())
        for (const auto& e : inbox)
            if (e.is_object() && e.contains("seq") && e["seq"].is_number_unsigned())
                live.insert(e["seq"].get<uint32_t>());
    m_popupQueue.erase(std::remove_if(m_popupQueue.begin(), m_popupQueue.end(),
                           [&](const PopupEntry& p) { return p.mpSeq != 0 && !live.count(p.mpSeq); }),
                       m_popupQueue.end());
    if (!inbox.is_array()) return;
    for (const auto& e : inbox) {
        MpInboxItem it;
        if (!mpInboxItemFromJson(e, it)) continue;
        if (it.targetIso != me->isoA3) continue;          // not ours to be asked
        if (!m_mpInboxShown.insert(it.seq).second) continue;   // already on screen
        PendingDiplomaticAction da;
        da.sourceIso = it.sourceIso;
        da.targetIso = it.targetIso;
        da.action = it.action;
        da.subjectIso = it.subjectIso;
        da.statedGoal = it.goal;
        pushDiplomaticRequestPopup(da, it.terms, it.seq);
    }
}

// ─────────────────────────────────────────────────────────── the host side ──

void Game::mpApplyChoices(int countryId, const nlohmann::json& j, size_t maxEntries) {
    const Country* meC = m_countries.getCountry(countryId);
    if (!meC) return;
    // NEVER A SOURCE THE CLIENT NAMED. Every overture below is sent from this
    // ISO, which is the seat's country on this host; a payload that says it
    // comes from somebody else has said nothing anybody reads.
    const std::string myIso = meC->isoA3;
    auto refused = [&](const char* what, const std::string& why) {
        printf("  Refused %s from country %d: %s\n", what, countryId, why.c_str());
    };
    auto owns = [&](int pid) {
        const Province* p = m_provinces.getProvinceById(pid);
        return p && p->countryId == countryId;
    };
    // From the provinces themselves rather than the per-country index, which
    // is rebuilt on ownership passes this function does not sit behind.
    // Computed once, on first use: most payloads never ask.
    std::vector<int> ownedCache;
    bool ownedKnown = false;
    auto ownedPids = [&]() -> const std::vector<int>& {
        if (!ownedKnown) {
            for (const auto& [pid, p] : m_provinces.getAllProvinces())
                if (p.countryId == countryId) ownedCache.push_back(pid);
            std::sort(ownedCache.begin(), ownedCache.end());
            ownedKnown = true;
        }
        return ownedCache;
    };

    // ── BUDGETS ──
    //
    // Out of 0..1 or not finite is refused outright -- a NaN allocation would
    // poison every economy sum it touched. Within it, the ceiling the sliders
    // draw is applied the way the sliders apply it, by clamping: a player
    // whose income fell since they set 40% gets what the screen would now show
    // them, not a refusal of the whole budget.
    if (j.contains("budgets") && j["budgets"].is_object()) {
        const json& b = j["budgets"];
        auto read = [&](const char* k, float& out) {
            if (!b.contains(k)) return false;
            if (!b[k].is_number()) { refused("a budget", std::string(k) + " is not a number"); return false; }
            const double d = b[k].get<double>();
            if (!(d >= 0.0 && d <= 1.0)) { refused("a budget", std::string(k) + " out of range"); return false; }
            out = (float)d;
            return true;
        };
        auto ra = m_countryResearchAllocation.find(countryId);
        auto pa = m_countryPacification.find(countryId);
        float research = ra != m_countryResearchAllocation.end() ? ra->second : 0.0f;
        float pacify = pa != m_countryPacification.end() ? pa->second : 0.0f;
        const bool haveR = read("research", research);
        const bool haveP = read("pacification", pacify);
        if (haveR || haveP) {
            const CountryIncomeSnapshot cs = computeCountryIncome(countryId);
            // Research against the pacification it will sit beside, then
            // pacification against what research leaves -- each slider's cap
            // reads the other's value, exactly as on the client's screen.
            if (haveR) research = std::min(research, researchAllocationCap(cs, pacify));
            if (haveP) pacify = std::min(pacify, pacificationAllocationCap(cs, research));
            if (haveR) modSetCountryResearchFunding(countryId, research);
            if (haveP) m_countryPacification[countryId] = pacify;
            m_countryIncomeCache.erase(countryId);
            invalidateIncomeCache();
        }
    }

    // ── RESEARCH ──
    //
    // The whole plan: what each group works on, its share, whether it walks.
    // Each node has to be one THIS host agrees the country could start, and
    // the check is made against the plan being installed rather than the one
    // it replaces -- otherwise moving a group from one side of a fork to the
    // other is refused because the side being left is "in progress".
    if (j.contains("research") && j["research"].is_array()) {
        const json& r = j["research"];
        const int unlocked = std::clamp(researchGroupsUnlocked(countryId), 1, RESEARCH_GROUPS_MAX);
        auto& extra = m_countryResearchExtra[countryId];
        if (!m_countryResearchActive.count(countryId)) m_countryResearchActive[countryId] = -1;
        int& slot0 = m_countryResearchActive[countryId];
        int& inv0 = m_countryResearchInvested[countryId];
        auto slot = [&](int g) -> int& { return g == 0 ? slot0 : extra[(size_t)g - 1].activeNode; };
        auto invOf = [&](int g) -> int& { return g == 0 ? inv0 : extra[(size_t)g - 1].invested; };
        MpResearchPlan plan;
        if (auto it = m_countryResearchPlan.find(countryId); it != m_countryResearchPlan.end())
            plan = it->second;
        else
            plan.share = {{1, 1, 1}};

        int cur[RESEARCH_GROUPS_MAX], curInv[RESEARCH_GROUPS_MAX], want[RESEARCH_GROUPS_MAX];
        for (int g = 0; g < RESEARCH_GROUPS_MAX; ++g) {
            cur[g] = slot(g); curInv[g] = invOf(g); want[g] = cur[g];
        }
        for (size_t g = 0; g < r.size() && g < (size_t)RESEARCH_GROUPS_MAX; ++g) {
            const json& e = r[g];
            if (!e.is_object()) continue;
            if ((int)g >= unlocked) {
                // A group this country cannot run. Said only when it asks for
                // something, since an idle one is what every client sends.
                if (e.contains("node") && e["node"].is_string() && !e["node"].get<std::string>().empty())
                    refused("research", "group " + std::to_string(g + 1) + " is not unlocked");
                continue;
            }
            if (e.contains("node")) {
                if (e["node"].is_null() || (e["node"].is_string() && e["node"].get<std::string>().empty())) {
                    want[g] = -1;
                } else if (e["node"].is_string() && e["node"].get<std::string>().size() <= 64) {
                    const std::string id = e["node"].get<std::string>();
                    int idx = -1;
                    for (int i = 0; i < (int)m_researchNodes.size(); ++i)
                        if (m_researchNodes[i].id == id) { idx = i; break; }
                    if (idx < 0) refused("research", "no node " + id);
                    else want[g] = idx;
                } else {
                    refused("research", "a node that is not a name");
                }
            }
            if (e.contains("share")) {
                if (e["share"].is_number_integer() && e["share"].get<long long>() >= 0 &&
                    e["share"].get<long long>() <= 100)
                    plan.share[g] = e["share"].get<int>();
                else
                    refused("research", "a share outside 0-100");
            }
            if (e.contains("auto") && e["auto"].is_boolean()) plan.autoAdvance[g] = e["auto"].get<bool>();
        }

        // Install, one group at a time, against what is already installed.
        for (int g = 0; g < RESEARCH_GROUPS_MAX; ++g) slot(g) = -1;
        auto& done = m_countryResearched[countryId];
        auto acceptable = [&](int idx, int g) {
            if (idx < 0 || idx >= (int)m_researchNodes.size()) return false;
            for (int k = 0; k < g; ++k) if (slot(k) == idx) return false;   // two groups, one node
            const ResearchNode& n = m_researchNodes[idx];
            return !done.count(n.id) && isNodeAvailableFor(n, countryId);
        };
        for (int g = 0; g < RESEARCH_GROUPS_MAX; ++g) {
            int n = -1;
            if (g < unlocked && want[g] >= 0) {
                if (acceptable(want[g], g)) n = want[g];
                else {
                    refused("research", m_researchNodes[want[g]].id + " is not available to it");
                    // The refusal keeps what the group had, if that still stands.
                    if (cur[g] >= 0 && acceptable(cur[g], g)) n = cur[g];
                }
            } else if (g >= unlocked) {
                n = cur[g];      // untouched: not this payload's to change
            }
            slot(g) = n;
            // Points sunk into a node stay with the node when it moves between
            // groups, as they do for the local player (ResearchNode::invested);
            // a node newly started starts from nothing.
            int keep = 0;
            for (int k = 0; k < RESEARCH_GROUPS_MAX; ++k) if (n >= 0 && cur[k] == n) keep = curInv[k];
            invOf(g) = keep;
        }
        plan.share = normaliseShares(plan.share, unlocked);
        m_countryResearchPlan[countryId] = plan;
    }

    // ── DOCTRINES ──
    //
    // Through enactPolicy / cancelPolicy, the path the Doctrines screen takes,
    // so the compass gate, the cost, the conflicts and the implementation time
    // are the host's. Three enactments a payload, which is the screen's
    // per-turn cap, counted on what is ASKED so a list of a thousand is three
    // checks rather than a thousand.
    if (j.contains("policies") && j["policies"].is_array()) {
        int enacts = 0;
        size_t n = 0;
        for (const auto& e : j["policies"]) {
            if (++n > 32) { refused("doctrines", "more than 32 in one turn"); break; }
            if (!e.is_object() || !e.contains("op") || !e["op"].is_string() ||
                !e.contains("id") || !e["id"].is_string())
                continue;
            const std::string op = e["op"].get<std::string>();
            const std::string id = e["id"].get<std::string>();
            const Policy* def = nullptr;
            for (const Policy& p : m_allPolicies) if (p.id == id) { def = &p; break; }
            if (!def) { refused("a doctrine", "no doctrine " + id.substr(0, 64)); continue; }
            int province = -1;
            if (e.contains("province")) {
                if (!e["province"].is_number_integer()) { refused("a doctrine", "province"); continue; }
                const long long pv = e["province"].get<long long>();
                if (pv > 0 && !(pv <= kMaxPid && owns((int)pv))) {
                    refused("a doctrine", "aimed at a province it does not own");
                    continue;
                }
                if (pv > 0) province = (int)pv;
            }
            std::string minority;
            if (e.contains("minority")) {
                if (!e["minority"].is_string() || e["minority"].get<std::string>().size() > 64) continue;
                minority = e["minority"].get<std::string>();
            }
            if (op == "enact") {
                if (++enacts > 3) { refused("a doctrine", "a fourth enactment this turn"); continue; }
                if (!minority.empty()) {
                    bool present = false;
                    for (int pid : ownedPids()) {
                        auto mit = m_provinceMinorities.find(pid);
                        if (mit == m_provinceMinorities.end()) continue;
                        for (const MinorityGroup& mg : mit->second)
                            if (mg.name == minority) { present = true; break; }
                        if (present) break;
                    }
                    if (!present) { refused("a doctrine", "aimed at a people it does not have"); continue; }
                }
                if (!canCountryEnactPolicy(countryId, *def)) {
                    refused("a doctrine", id + ": " + policyBlockReason(countryId, *def));
                    continue;
                }
                enactPolicy(countryId, id, province, minority);
            } else if (op == "cancel") {
                auto ix = m_countryActivePolicyIndices.find(countryId);
                if (ix == m_countryActivePolicyIndices.end()) continue;
                for (int idx : ix->second) {
                    if (idx < 0 || idx >= (int)m_activePolicies.size()) continue;
                    const ActivePolicy& ap = m_activePolicies[idx];
                    if (ap.policyId != id || ap.turnsRemaining == -1) continue;
                    if (e.contains("province") && ap.targetProvince != province) continue;
                    if (e.contains("minority") && ap.targetMinority != minority) continue;
                    cancelPolicy(idx);
                    break;
                }
            }
        }
    }

    // ── DIPLOMACY THEY START ──
    //
    // A short list of words, each checked against the same table the province
    // panel draws its buttons from (diploActionAllowed), then queued through
    // the doors the panel uses. The bookkeeping halves of an answer --
    // apply_ceasefire, apply_trade, cancel -- are not on the list and never
    // will be: they settle a deal somebody else agreed to, and accepting one
    // from a client would let it end a war, or take provinces, by saying so.
    if (j.contains("diplomacy") && j["diplomacy"].is_array()) {
        static const std::unordered_set<std::string> kWords = {
            "declare_war", "request_alliance", "request_guarantee", "request_nap",
            "break_alliance", "break_guarantee", "break_nap", "add_guarantee",
            "call_to_arms", "request_ceasefire", "propose_trade"};
        size_t n = 0;
        for (const auto& e : j["diplomacy"]) {
            if (++n > 64) { refused("diplomacy", "more than 64 in one turn"); break; }
            if (!e.is_object() || !e.contains("action") || !e["action"].is_string() ||
                !e.contains("target") || !e["target"].is_string())
                continue;
            const std::string action = e["action"].get<std::string>();
            const std::string target = e["target"].get<std::string>();
            if (!kWords.count(action)) { refused("diplomacy", "\"" + action.substr(0, 32) + "\""); continue; }
            const int tgtCid = target.size() <= 8 ? cidForIso(target) : -1;
            if (tgtCid <= 0 || tgtCid == countryId) { refused("diplomacy", "no such country"); continue; }
            std::string why;
            if (!diploActionAllowed(countryId, tgtCid, action, &why)) {
                refused("diplomacy", action + " with " + target + ": " + why);
                continue;
            }
            // Asked once. An overture still waiting in a remote player's inbox
            // is not in the queue any more, so the queue cannot say so.
            bool waiting = false;
            if (auto box = m_mpInbox.find(tgtCid); box != m_mpInbox.end())
                for (const MpInboxItem& it : box->second)
                    if (it.sourceIso == myIso && it.action == action) waiting = true;
            if (waiting) continue;

            if (action == "call_to_arms") {
                if (!requestAllyJoinWar(countryId, target, why)) refused("a call to arms", why);
            } else if (action == "request_ceasefire" || action == "propose_trade") {
                CeasefireTerms terms;
                if (e.contains("terms") &&
                    !termsFromJson(e["terms"], terms, m_provinces.getAllProvinces().size())) {
                    refused("a deal", "terms that do not read");
                    continue;
                }
                // Each side gives only what is its own to give.
                const Country* tc = m_countries.getCountry(tgtCid);
                auto ownedBy = [&](const std::vector<int>& v, int cid) {
                    for (int pid : v) {
                        const Province* p = m_provinces.getProvinceById(pid);
                        if (!p || p->countryId != cid) return false;
                    }
                    return true;
                };
                auto claimedBy = [&](const std::vector<int>& v, const std::string& iso) {
                    auto c = m_claims.find(iso);
                    for (int pid : v)
                        if (c == m_claims.end() ||
                            std::find(c->second.begin(), c->second.end(), pid) == c->second.end())
                            return false;
                    return true;
                };
                if (!ownedBy(terms.ourProvs, countryId) || !ownedBy(terms.ourReleaseProvs, countryId) ||
                    !ownedBy(terms.theirProvs, tgtCid) || !ownedBy(terms.theirReleaseProvs, tgtCid) ||
                    !claimedBy(terms.ourDropClaims, myIso) ||
                    !(tc && claimedBy(terms.theirDropClaims, tc->isoA3))) {
                    refused("a deal", "terms trading what one side does not hold");
                    continue;
                }
                // The same shape check the deal screen makes before it lets a
                // release through: big enough, and in one piece.
                std::string whyNot;
                if ((!terms.ourReleaseProvs.empty() &&
                     !releaseSubsetOk(countryId, terms.ourReleaseProvs, whyNot)) ||
                    (!terms.theirReleaseProvs.empty() &&
                     !releaseSubsetOk(tgtCid, terms.theirReleaseProvs, whyNot))) {
                    refused("a deal", "a release that is not a nation: " + whyNot);
                    continue;
                }
                if (!submitDealOffer(countryId, tgtCid, action == "propose_trade", terms))
                    refused("a deal", "already in talks with " + target);
            } else {
                PendingDiplomaticAction da;
                da.sourceIso = myIso;
                da.targetIso = target;
                da.action = action;
                da.turnsRemaining = 1;
                if (action == "declare_war" && e.contains("goal")) {
                    if (!e["goal"].is_number_integer() || e["goal"].get<long long>() < 0 ||
                        e["goal"].get<long long>() >= WAR_GOAL_COUNT) {
                        refused("a declaration", "a war goal that does not exist");
                        continue;
                    }
                    da.statedGoal = e["goal"].get<int>();
                }
                if (!queueDiplomaticAction(std::move(da)))
                    refused("diplomacy", action + " with " + target + ": the queue said no");
            }
        }
    }

    // ── WHAT IT PUBLISHES ──
    //
    // Masked to the bits this world offers -- the population, income and army
    // rows exist only under fog, where publishing them tells anybody anything.
    if (j.contains("disclosure")) {
        if (j["disclosure"].is_number_unsigned()) {
            unsigned mask = DISCLOSE_EXPENSES | DISCLOSE_DOCTRINES | DISCLOSE_TREASURY |
                            DISCLOSE_DISTRICT_LAWS;
            if (m_fogOfWar) mask |= DISCLOSE_POPULATION | DISCLOSE_INCOME | DISCLOSE_ARMY;
            const unsigned bits = (unsigned)(j["disclosure"].get<unsigned long long>() & mask);
            if (bits) m_countryDisclosure[countryId] = bits;
            else m_countryDisclosure.erase(countryId);
        } else {
            refused("disclosure", "not a set of bits");
        }
    }

    // ── MINORITY LAW ──
    //
    // Only for peoples who actually live in this country, so a client cannot
    // fill the host's table with rows for groups that are not there. The
    // option indices go through setEthnicPolicyOption, which ignores one that
    // is out of range for its category.
    if (j.contains("ethnic") && j["ethnic"].is_object()) {
        std::unordered_set<std::string> present;
        for (int pid : ownedPids()) {
            if (!owns(pid)) continue;
            auto mit = m_provinceMinorities.find(pid);
            if (mit == m_provinceMinorities.end()) continue;
            for (const MinorityGroup& mg : mit->second) present.insert(mg.name);
        }
        size_t rows = 0;
        for (const auto& [name, opts] : j["ethnic"].items()) {
            if (++rows > maxEntries) break;
            if (!present.count(name)) { refused("minority law", "for a people it does not have"); continue; }
            if (!opts.is_array() || opts.size() > m_ethnicPolicyCategories.size()) continue;
            for (size_t ci = 0; ci < opts.size(); ++ci)
                if (opts[ci].is_number_integer())
                    setEthnicPolicyOption(countryId, name, ci, opts[ci].get<int>());
        }
    }

    // ── DISTRICTS: A PARTITION OR NOTHING ──
    //
    // Every province the country owns in exactly one district, nothing it does
    // not own in any. Anything else is refused WHOLE rather than repaired: a
    // repaired division is one the player did not draw, and a half-applied one
    // polices ground with nobody's budget.
    if (j.contains("districts") && j["districts"].is_array()) {
        const json& arr = j["districts"];
        std::unordered_set<int> owned;
        for (int pid : ownedPids()) if (owns(pid)) owned.insert(pid);
        std::vector<District> list;
        std::unordered_set<int> seen, ids;
        std::string bad;
        if (arr.size() > owned.size() || arr.size() > 256) bad = "more districts than provinces";
        for (const auto& e : arr) {
            if (!bad.empty()) break;
            if (!e.is_object()) { bad = "a district that is not an object"; break; }
            District d;
            const long long id = intOr(e, "id", 0);
            if (id < 1 || id > 1000000 || !ids.insert((int)id).second) { bad = "district ids"; break; }
            d.id = (int)id;
            if (!e.contains("name") || !e["name"].is_string() || e["name"].get<std::string>().size() > 64) {
                bad = "a district name"; break;
            }
            d.name = e["name"].get<std::string>();
            d.customName = e.contains("custom_name") && e["custom_name"].is_boolean() &&
                           e["custom_name"].get<bool>();
            auto channel = [&](const char* k, unsigned char& c) {
                const long long v = intOr(e, k, 128);
                if (v < 0 || v > 255) return false;
                c = (unsigned char)v;
                return true;
            };
            if (!channel("r", d.r) || !channel("g", d.g) || !channel("b", d.b)) { bad = "a colour"; break; }
            const long long share = intOr(e, "share", 0);
            if (share < 0 || share > 100) { bad = "a share"; break; }
            d.sharePct = (int)share;
            if (!e.contains("provinces") || !e["provinces"].is_array()) { bad = "no provinces"; break; }
            for (const auto& pv : e["provinces"]) {
                if (!pv.is_number_integer()) { bad = "a province"; break; }
                const long long pid = pv.get<long long>();
                if (pid < 1 || pid > kMaxPid || !owned.count((int)pid)) { bad = "a province it does not own"; break; }
                if (!seen.insert((int)pid).second) { bad = "a province in two districts"; break; }
                d.provinces.push_back((int)pid);
            }
            if (!bad.empty()) break;
            std::sort(d.provinces.begin(), d.provinces.end());
            if (e.contains("policies")) {
                if (!e["policies"].is_array() || e["policies"].size() > 32) { bad = "district laws"; break; }
                for (const auto& lw : e["policies"]) {
                    if (!lw.is_string() || !districtLawById(lw.get<std::string>())) { bad = "a district law"; break; }
                    const std::string law = lw.get<std::string>();
                    if (std::find(d.policies.begin(), d.policies.end(), law) == d.policies.end())
                        d.policies.push_back(law);
                }
                if (!bad.empty()) break;
            }
            list.push_back(std::move(d));
        }
        if (bad.empty() && seen.size() != owned.size()) bad = "provinces left out of every district";
        if (!bad.empty()) {
            refused("districts", bad);
        } else {
            if (list.empty()) m_districts.erase(countryId);
            else m_districts[countryId] = std::move(list);
            normaliseDistrictShares(countryId, -1);
            ensureDefaultDistrict(countryId);
        }
    }

    // ── CLAIMS ──
    //
    // grantClaim refuses ground the claimant already holds, and is the same
    // call updatePolicies makes for the local player's Claims tab. Applied
    // here, before the turn, where the player's are applied late in it; the
    // difference is a province taken this same turn, which grantClaim then
    // refuses for being already ours -- the same answer either way.
    if (j.contains("claims") && j["claims"].is_object()) {
        auto pids = [&](const char* k) {
            std::vector<int> out;
            const json& c = j["claims"];
            if (!c.contains(k) || !c[k].is_array()) return out;
            for (const auto& v : c[k]) {
                if (out.size() >= maxEntries) break;
                if (!v.is_number_integer()) continue;
                const long long pid = v.get<long long>();
                if (pid >= 1 && pid <= kMaxPid && m_provinces.getProvinceById((int)pid))
                    out.push_back((int)pid);
            }
            return out;
        };
        for (int pid : pids("drop")) revokeClaim(myIso, pid);
        for (int pid : pids("add")) if (!owns(pid)) grantClaim(myIso, pid);
    }

    // ── ANSWERS TO WHAT IT WAS ASKED ──
    //
    // Matched by the host's own number against the host's own list for THIS
    // seat, so a client can only answer what it was actually asked, and only
    // once. An unknown number is a request already settled, not an attack.
    if (j.contains("answers") && j["answers"].is_array()) {
        auto box = m_mpInbox.find(countryId);
        size_t n = 0;
        for (const auto& e : j["answers"]) {
            if (box == m_mpInbox.end() || box->second.empty()) break;
            if (++n > 64) break;
            if (!e.is_object() || !e.contains("seq") || !e["seq"].is_number_unsigned()) continue;
            const uint32_t seq = e["seq"].get<uint32_t>();
            const bool accept = e.contains("accept") && e["accept"].is_boolean() &&
                                e["accept"].get<bool>();
            int reason = REFUSE_NONE;
            if (e.contains("reason") && e["reason"].is_number_integer()) {
                const long long rr = e["reason"].get<long long>();
                if (rr >= 0 && rr < REFUSE_COUNT) reason = (int)rr;
            }
            auto it = std::find_if(box->second.begin(), box->second.end(),
                                   [&](const MpInboxItem& x) { return x.seq == seq; });
            if (it == box->second.end()) continue;
            const MpInboxItem item = *it;
            box->second.erase(it);
            applyDiplomaticAnswer(countryId, item, accept, reason);
        }
    }
}

void Game::applyDiplomaticAnswer(int answerer, const MpInboxItem& item, bool accept,
                                 int statedReason, bool timedOut) {
    const std::string& src = item.sourceIso;
    const std::string& tgt = item.targetIso;
    std::string hostIso;
    if (const Country* pc = m_countries.getCountry(m_playerCountryId)) hostIso = pc->isoA3;
    const bool toHostPlayer = !hostIso.empty() && src == hostIso;
    if (item.action == "declare_war") return;       // told, not asked
    printf("[MPANSWER] %s %s %s from %s seq=%u\n", tgt.c_str(),
           timedOut ? "did not answer" : (accept ? "accepts" : "refuses"),
           item.action.c_str(), src.c_str(), item.seq);

    if (item.action == "request_alliance" || item.action == "request_guarantee" ||
        item.action == "request_nap") {
        if (accept) {
            // The rule the local popup applies: a treaty does not land on a
            // pair that went to war while the offer was waiting.
            if (hasRelation(src, tgt, &CountryRelation::war)) {
                if (toHostPlayer)
                    addNotification(TextFormat(T("Your offer to %s is void — you are at war"),
                                               diploDisplayName(tgt).c_str()),
                                    Color{235, 130, 90, 255}, 7.0f);
                return;
            }
            applyTreatyAction(src, tgt, item.action);
            if (toHostPlayer)
                if (const char* what = diploRequestPhrase(item.action))
                    addNotification(TextFormat(T("%s accepted your offer of %s"),
                                               diploDisplayName(tgt).c_str(), what),
                                    Color{140, 220, 150, 255}, 7.0f);
        } else {
            if (!timedOut) tellRefusal(answerer, src, statedReason);
            // So an AI proposer backs off as it does from an AI refusal.
            if (m_ai) m_ai->noteDiploRejected(cidForIso(src), answerer);
            if (toHostPlayer)
                if (const char* what = diploRequestPhrase(item.action))
                    addNotification(TextFormat(T("%s declined your offer of %s"),
                                               diploDisplayName(tgt).c_str(), what),
                                    Color{235, 130, 90, 255}, 7.0f);
        }
        return;
    }

    if (item.action == "call_to_arms") {
        // Still an alliance, and the caller still at war with the aggressor:
        // a call answered a turn late must not become a war nobody asked for,
        // outside the war-declaration limit the panel enforces.
        const bool stillLive = m_relations[src][tgt].alliance &&
                               m_relations[src][item.subjectIso].war;
        if (accept && !stillLive) return;
        if (accept) {
            declareWar(tgt, item.subjectIso, false);
            addWarWeariness(answerer, CALL_TO_ARMS_UNREST);
            if (m_ai) m_ai->noteCallAnswered(answerer);
            if (toHostPlayer)
                addNotification(TextFormat(T("%s answers your call and joins the war against %s"),
                                           diploDisplayName(tgt).c_str(),
                                           diploDisplayName(item.subjectIso).c_str()),
                                Color{140, 220, 150, 255}, 8.0f);
        } else if (!timedOut) {
            // Refusing is free at home and costs the alliance -- the popup's rule.
            m_relations[src][tgt].alliance = false;
            m_relations[tgt][src].alliance = false;
            tellRefusal(answerer, src, statedReason);
            if (m_ai) m_ai->noteCallRefused(answerer);
        }
        // ── SILENCE IS NOT A REFUSAL ──
        //
        // A player who was away for a turn did not decline anything. Ending
        // their alliance for it would punish a closed laptop as though it were
        // a betrayal, so an unanswered call simply lapses: they are not in the
        // war, and the alliance stands.
        return;
    }

    if (item.action == "request_ceasefire" || item.action == "propose_trade") {
        const std::string key = src + "|" + tgt;
        auto pending = m_pendingCeasefireTerms.find(key);
        CeasefireTerms terms = item.hasTerms ? item.terms
                             : (pending != m_pendingCeasefireTerms.end() ? pending->second : CeasefireTerms{});
        // The host's own player paid what they offered when they offered it
        // (submitDealOffer). apply_* charges at resolution, so the prepayment
        // comes back either way -- once, here.
        if (toHostPlayer && terms.ourMoney > 0)
            if (Country* sc = m_countries.getCountry(cidForIso(src))) sc->treasury += terms.ourMoney;
        if (pending != m_pendingCeasefireTerms.end()) m_pendingCeasefireTerms.erase(pending);
        if (accept) {
            PendingDiplomaticAction da;
            da.sourceIso = src;
            da.targetIso = tgt;
            da.action = item.action == "propose_trade" ? "apply_trade" : "apply_ceasefire";
            da.turnsRemaining = 1;
            queueDiplomaticAction(da);
            m_acceptedCeasefireTerms[key] = terms;
        } else if (!timedOut) {
            tellRefusal(answerer, src, statedReason);
        }
        return;
    }
}

nlohmann::json Game::mpInboxJsonFor(int countryId) const {
    json out = json::array();
    auto box = m_mpInbox.find(countryId);
    if (box == m_mpInbox.end()) return out;
    for (const MpInboxItem& it : box->second) out.push_back(mpInboxItemJson(it));
    return out;
}

void Game::mpExpireInbox(const std::unordered_map<int, bool>& aiPlays) {
    // Everything here was raised by an earlier resolution and has had a turn
    // to be answered; the answers that came are already applied, because this
    // runs after every seat's orders.
    std::vector<std::pair<int, MpInboxItem>> due;
    for (auto& [cid, items] : m_mpInbox)
        for (const MpInboxItem& it : items) due.emplace_back(cid, it);
    m_mpInbox.clear();
    // In the order they were raised, so the outcome does not depend on how an
    // unordered map happened to lay the seats out.
    std::sort(due.begin(), due.end(),
              [](const auto& a, const auto& b) { return a.second.seq < b.second.seq; });
    for (const auto& [cid, it] : due) {
        auto ai = aiPlays.find(cid);
        // A seat the AI is playing THIS turn -- the player left, or missed it
        // under "absent: AI" -- has its business decided the way any AI
        // country's is. Anyone else's lapses, without prejudice.
        const bool aiDecides = ai == aiPlays.end() || ai->second;
        if (aiDecides && m_ai && it.action != "declare_war") {
            int stated = REFUSE_NONE;
            const bool yes = m_ai->decideDiplomacy(cid, it.action, it.sourceIso,
                                                   it.subjectIso, &stated);
            applyDiplomaticAnswer(cid, it, yes, stated);
        } else {
            applyDiplomaticAnswer(cid, it, false, REFUSE_NONE, true);
        }
    }
}

// ─────────────────────────────────────────────────────────────── the save ──

void Game::mpWriteChoiceState(nlohmann::json& j) const {
    for (const auto& [cid, plan] : m_countryResearchPlan) {
        json p;
        p["share"] = plan.share;
        p["auto"] = plan.autoAdvance;
        p["last"] = json::array();
        for (int idx : plan.lastNode)
            p["last"].push_back(idx >= 0 && idx < (int)m_researchNodes.size()
                                    ? m_researchNodes[idx].id : std::string());
        j["countryResearchPlan"][std::to_string(cid)] = p;
        // The extra groups too, for these countries only. The AI's are not
        // saved and never were -- its groups refill themselves -- and writing
        // them now would change how every single-player save reloads.
        auto ex = m_countryResearchExtra.find(cid);
        if (ex == m_countryResearchExtra.end()) continue;
        json arr = json::array();
        for (const ExtraResearchSlot& sl : ex->second)
            arr.push_back({{"node", sl.activeNode >= 0 && sl.activeNode < (int)m_researchNodes.size()
                                        ? m_researchNodes[sl.activeNode].id : std::string()},
                           {"invested", sl.invested}});
        j["countryResearchExtra"][std::to_string(cid)] = arr;
    }
    // Unanswered requests survive a restart: a server stopped overnight must
    // not have quietly declined everything everyone was asked.
    for (const auto& [cid, items] : m_mpInbox)
        for (const MpInboxItem& it : items) {
            json e = mpInboxItemJson(it);
            e["cid"] = cid;
            j["mpInbox"].push_back(e);
        }
    if (m_mpInboxSeq) j["mpInboxSeq"] = m_mpInboxSeq;
}

void Game::mpReadChoiceState(const nlohmann::json& j) {
    auto nodeIndex = [&](const std::string& id) {
        for (int i = 0; i < (int)m_researchNodes.size(); ++i)
            if (m_researchNodes[i].id == id) return i;
        return -1;
    };
    if (j.contains("countryResearchPlan") && j["countryResearchPlan"].is_object()) {
        m_countryResearchPlan.clear();
        for (const auto& [key, p] : j["countryResearchPlan"].items()) {
            if (!p.is_object()) continue;
            MpResearchPlan plan;
            for (int g = 0; g < RESEARCH_GROUPS_MAX; ++g) {
                if (p.contains("share") && p["share"].is_array() && (size_t)g < p["share"].size() &&
                    p["share"][g].is_number_integer())
                    plan.share[(size_t)g] = std::clamp(p["share"][g].get<int>(), 0, 100);
                if (p.contains("auto") && p["auto"].is_array() && (size_t)g < p["auto"].size() &&
                    p["auto"][g].is_boolean())
                    plan.autoAdvance[(size_t)g] = p["auto"][g].get<bool>();
                if (p.contains("last") && p["last"].is_array() && (size_t)g < p["last"].size() &&
                    p["last"][g].is_string())
                    plan.lastNode[(size_t)g] = nodeIndex(p["last"][g].get<std::string>());
            }
            m_countryResearchPlan[std::atoi(key.c_str())] = plan;
        }
    }
    if (j.contains("countryResearchExtra") && j["countryResearchExtra"].is_object())
        for (const auto& [key, arr] : j["countryResearchExtra"].items()) {
            if (!arr.is_array()) continue;
            auto& ex = m_countryResearchExtra[std::atoi(key.c_str())];
            for (size_t g = 0; g < ex.size() && g < arr.size(); ++g) {
                if (!arr[g].is_object()) continue;
                ex[g].activeNode = nodeIndex(arr[g].value("node", std::string()));
                ex[g].invested = std::max(0, arr[g].value("invested", 0));
            }
        }
    if (j.contains("mpInbox") && j["mpInbox"].is_array()) {
        m_mpInbox.clear();
        for (const auto& e : j["mpInbox"]) {
            MpInboxItem it;
            if (!mpInboxItemFromJson(e, it)) continue;
            const int cid = e.value("cid", 0);
            if (cid > 0) m_mpInbox[cid].push_back(it);
        }
    }
    if (j.contains("mpInboxSeq") && j["mpInboxSeq"].is_number_unsigned())
        m_mpInboxSeq = std::max(m_mpInboxSeq, j["mpInboxSeq"].get<uint32_t>());
}
