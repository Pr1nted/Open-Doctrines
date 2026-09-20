// Industry in state hands: the game's half. The rule itself is in
// src/Nationalisation.h, which has no world in it and is tested on its own.
//
// What lives here is everything that needs a map: which province a holding
// touches, where the compass comes from, and the one-per-turn step.
//
// OFF BY DEFAULT. It moves build costs, upkeep, resource income and unrest --
// four numbers the AI reads on almost every decision it makes -- so it changes
// play and every bench baseline. With OD_NATIONALISATION unset m_nationalised
// is never written, every multiplier below returns 1.0 and the extra unrest is
// 0, which is what makes the decision hash identical with it off.

#include "Game.h"

#include <algorithm>
#include <cstdlib>

bool Game::nationalisationOn() {
    static const bool on = std::getenv("OD_NATIONALISATION") &&
                           atoi(std::getenv("OD_NATIONALISATION")) != 0;
    return on;
}

int Game::nationalisationCap(int countryId) const {
    if (!nationalisationOn()) return 0;
    auto it = m_countryCompass.find(countryId);
    if (it == m_countryCompass.end()) return 0;
    return odnat::capFor(it->second.economic);
}

float Game::nationalisationRamp(int countryId, const std::string& resource) const {
    if (!nationalisationOn()) return 0.0f;
    auto it = m_nationalised.find(countryId);
    if (it == m_nationalised.end()) return 0.0f;
    for (const odnat::Holding& h : it->second)
        if (h.resource == resource) return h.ramp;
    return 0.0f;
}

bool Game::nationalise(int countryId, const std::string& resource) {
    if (!nationalisationOn()) return false;

    // A resource the game does not have is not a speciality. Checked against
    // the same list the specialisation screen offers, so the two cannot drift.
    bool known = false;
    for (const char* r : SPEC_RESOURCES) if (resource == r) known = true;
    if (!known) return false;

    auto& list = m_nationalised[countryId];
    int held = 0;
    for (const odnat::Holding& h : list) if (h.held) ++held;
    if (held >= nationalisationCap(countryId)) return false;

    for (odnat::Holding& h : list)
        if (h.resource == resource) {
            if (h.held) return false;            // already ours
            // Taking back one that is still decaying RESUMES it rather than
            // starting again. The investment did not evaporate the turn it was
            // released, and pretending it did would make a brief privatisation
            // strictly worse than never having held it at all.
            h.held = true;
            return true;
        }

    list.push_back({resource, 0.0f, true});
    return true;
}

bool Game::releaseNationalised(int countryId, const std::string& resource) {
    if (!nationalisationOn()) return false;
    auto it = m_nationalised.find(countryId);
    if (it == m_nationalised.end()) return false;
    for (odnat::Holding& h : it->second)
        if (h.resource == resource && h.held) {
            // NOT ERASED. The entry stays so its ramp can decay; deleting it
            // would hand back the full output the turn it was privatised,
            // which is the flip this design exists to refuse.
            h.held = false;
            return true;
        }
    return false;
}

void Game::stepNationalisation() {
    if (!nationalisationOn()) return;

    for (auto& [cid, list] : m_nationalised) {
        // The compass may have moved right since last turn. Anything over the
        // new cap is released -- which unwinds over twenty turns like any other
        // release, rather than vanishing.
        for (const std::string& r : odnat::overCap(list, nationalisationCap(cid)))
            for (odnat::Holding& h : list)
                if (h.resource == r) h.held = false;

        for (odnat::Holding& h : list) h.ramp = odnat::step(h.ramp, h.held);

        // A released holding that has finished decaying is no longer anything.
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [](const odnat::Holding& h) {
                                      return !h.held && h.ramp <= 0.0f;
                                  }),
                   list.end());
    }
}

float Game::provinceNationalisationRamp(int provinceId) const {
    if (!nationalisationOn()) return 0.0f;
    auto indIt = m_provinceIndustry.find(provinceId);
    if (indIt == m_provinceIndustry.end() || indIt->second.specialization.empty())
        return 0.0f;
    const Province* p = m_provinces.getProvinceById(provinceId);
    if (!p || p->countryId <= 0) return 0.0f;
    return nationalisationRamp(p->countryId, indIt->second.specialization);
}

/**
 * The country's industry, weighted by how much of it is in state hands.
 *
 * Upkeep is a COUNTRY-level number -- industryUpkeep() takes the total level
 * count and the gross industry income, not a province -- so the per-province
 * ramp has to be reduced to one figure before it can move it. This is the
 * level-weighted mean: a country with half its factories in a speciality held
 * at full ramp pays half of kUpkeepMax, which is the same answer as charging
 * each province separately and adding them up.
 *
 * Weighted by LEVEL and not by province count, because a level-8 province is
 * eight times the industry of a level-1 one and the bill should say so.
 */
float Game::nationalisedIndustryShare(int countryId) const {
    if (!nationalisationOn()) return 0.0f;
    float weighted = 0.0f, total = 0.0f;
    for (int pid : provincesOf(countryId)) {
        auto it = m_provinceIndustry.find(pid);
        if (it == m_provinceIndustry.end() || it->second.level <= 0) continue;
        const float w = (float)it->second.level;
        total += w;
        weighted += w * provinceNationalisationRamp(pid);
    }
    return total > 0.0f ? weighted / total : 0.0f;
}
