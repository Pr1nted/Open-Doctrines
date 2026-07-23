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
    float treasury = 0.0f;
    float compassEconomic = 0.0f;  // -100 (left) to +100 (right)
    float compassSocial = 0.0f;    // -100 (auth) to +100 (libertarian)
    std::string doctrine;          // doctrine ID, empty = none
    std::vector<std::string> research; // researched tech node IDs
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
