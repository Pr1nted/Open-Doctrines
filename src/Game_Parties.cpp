// Who governs, inside the game.
//
// ── WHAT THIS FILE OWNS ──
//
// Reading data/parties.json for this scenario, filling in the countries it
// does not cover, and applying one turn of the ruling party's pull. What a
// party IS, how hard it pulls and what a generated one looks like all live in
// src/Parties.h, which is pure and tested on its own.
//
// ── OFF BY DEFAULT, AND EXACTLY OFF ──
//
// Behind OD_PARTIES, like every other politics rule here. With it off no
// legislature is built and applyPartyPull returns before touching a compass,
// so shiftCountryCompass is called exactly as often as it was and the decision
// hash is unchanged -- which matters because the compass feeds rebellion
// chance, and rebellion chance is rolled against simRand.

#include "Game.h"

#include "GameInternals.h"

#include <cstdlib>
#include <fstream>

bool Game::partiesOn() const {
    static const bool on = std::getenv("OD_PARTIES") &&
                           atoi(std::getenv("OD_PARTIES")) != 0;
    return on;
}

const odparty::Party* Game::rulingParty(int countryId) const {
    auto it = m_countryParties.find(countryId);
    return it == m_countryParties.end() ? nullptr : it->second.rulingParty();
}

void Game::loadParties() {
    m_countryParties.clear();
    if (!partiesOn()) return;

    // ── the historical file ──
    //
    // A missing or unreadable file is NOT an error: every country simply
    // generates, which is the same outcome as a scenario the file does not
    // cover. A mod shipping its own map gets working politics without having
    // to author a party table first.
    nlohmann::json scenario;
    {
        const std::string path = m_dataDir + "parties.json";
        std::ifstream f(path);
        if (f) {
            try {
                nlohmann::json doc = nlohmann::json::parse(f, nullptr, true, true);
                if (doc.contains("scenarios") && doc["scenarios"].is_object()) {
                    const auto& all = doc["scenarios"];
                    if (all.contains(m_scenarioKey) && all[m_scenarioKey].is_object())
                        scenario = all[m_scenarioKey];
                }
            } catch (const std::exception& e) {
                // Reported, never fatal -- and reported rather than swallowed,
                // because a typo in the file otherwise reads as "this scenario
                // has no historical parties", which is the one thing the
                // coverage tool cannot distinguish from an honest gap.
                printf("[PARTIES] %s could not be read (%s); every country will generate\n",
                       path.c_str(), e.what());
            }
        }
    }

    int historical = 0, generated = 0;
    for (const auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID || cid == SPC_CID) continue;
        if (cid >= REBEL_CID_MIN) continue;   // a rebel state has no legislature yet

        auto compassIt = m_countryCompass.find(cid);
        const PoliticalCompass gov =
            compassIt != m_countryCompass.end() ? compassIt->second : PoliticalCompass{};

        odparty::Legislature leg;
        bool fromData = false;
        if (!scenario.is_null() && scenario.contains(c.isoA3) &&
            scenario[c.isoA3].is_object() && scenario[c.isoA3].contains("parties")) {
            for (const auto& e : scenario[c.isoA3]["parties"]) {
                if (!e.is_object() || !e.contains("name")) continue;
                odparty::Party p;
                p.name      = e["name"].get<std::string>();
                p.shortName = e.value("short", std::string());
                p.stance    = makeCompass(e.value("econ", 0.0f), e.value("soc", 0.0f));
                p.support   = e.value("support", 0.0f);
                p.historical = true;       // it is in the file, so it is a record
                leg.parties.push_back(p);
            }
            fromData = !leg.parties.empty();
        }

        if (fromData) {
            odparty::renormalise(leg);
            odparty::chooseRuling(leg);
            ++historical;
        } else {
            // Seeded on the country id so the same country generates the same
            // parties on every load of the same world. A save stores the result
            // anyway, but a world loaded twice before its first save must not
            // be two different worlds.
            leg = odparty::generate(gov, (uint32_t)(cid * 2654435761u + 1u));
            ++generated;
        }
        m_countryParties[cid] = std::move(leg);
    }

    // The gap, said out loud. tools/check_party_coverage.py reports the same
    // thing per scenario without running the game; this is so a play session
    // is honest about which of the two kinds of politics it is showing.
    printf("[PARTIES] scenario '%s': %d countries from the record, %d generated\n",
           m_scenarioKey.c_str(), historical, generated);
}

void Game::applyPartyPull() {
    if (!partiesOn()) return;
    for (const auto& [cid, leg] : m_countryParties) {
        auto compassIt = m_countryCompass.find(cid);
        if (compassIt == m_countryCompass.end()) continue;
        const PoliticalCompass d = odparty::pull(leg, compassIt->second);
        if (d.economic == 0.0f && d.social == 0.0f) continue;
        // Through shiftCountryCompass, which is the only thing allowed to move
        // a compass: it owns the bound, and a second copy of a bound is a
        // second bound that drifts from the first.
        shiftCountryCompass(cid, d.economic, d.social);
    }
}
