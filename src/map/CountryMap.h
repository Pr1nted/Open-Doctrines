#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "json.hpp"
#include "../renderer/FlagRenderer.h"

struct Country {
    int id = 0;
    std::string name;
    std::string isoA3;
    Color color{};
    FlagPattern flagActual;
    FlagPattern flagCensored;
    // double, not float: a 32-bit float cannot represent consecutive integers
    // above 2^24 (~16.7M), so once a treasury grew past that, `treasury += net`
    // silently became a no-op and the economy appeared to freeze.
    double treasury = 0.0;
    float compassEconomic = 0.0f;  // -100 (left) to +100 (right)
    float compassSocial = 0.0f;    // -100 (auth) to +100 (libertarian)
    std::vector<std::string> research; // researched tech node IDs

    // ── What this country was called and flew before politics changed it ──
    //
    // Written once, the first time a government goes far enough for
    // Game::updatePoliticalIdentities() to restyle it, and never again. Every
    // restyle is computed from THESE rather than from the current name and
    // flag, so a country that swings left, further left, and then back to the
    // centre lands on exactly what it started with instead of an approximation
    // that drifted a little at each step.
    //
    // Empty rootName means "never restyled"; it is not a copy of `name`.
    std::string rootName;
    FlagPattern rootFlag;
    // The censored original, kept alongside it. A restyle is computed from the
    // root, and the censored restyle has to be computed from the CENSORED root
    // or a country whose real flag carries a hate symbol would keep carrying it
    // through the censored view: the recolour changes an image's colours, it
    // does not change what is drawn on it.
    FlagPattern rootFlagCensored;
    bool        rootSaved = false;

    // The identity currently expressed, as ints so the save format stays plain
    // JSON numbers. See PoliticalIdentity.h -- these are IdeologyQuadrant and
    // IdeologyIntensity. Persisted because the hysteresis needs to know what
    // was already showing, and `identityTurn` because a change is rate-limited
    // and a reload must not hand out a free one.
    int identityQuadrant  = 0;
    int identityIntensity = 0;
    int identityTurn      = -1000;
};

class CountryMap {
public:
    bool load(const std::string& jsonPath);
    bool loadFromJson(const std::string& jsonStr);

    const Country* getCountry(int id) const;
    Country* getCountry(int id);
    const Country* getCountryByCode(const std::string& isoA3) const;
    const std::unordered_map<int, Country>& getAll() const { return m_countries; }
    std::unordered_map<int, Country>& getAll() { return m_countries; }
    int size() const { return (int)m_countries.size(); }
    void clear() { m_countries.clear(); }

private:
    std::unordered_map<int, Country> m_countries;
};
