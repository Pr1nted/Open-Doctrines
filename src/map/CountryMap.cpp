#include "CountryMap.h"
#include <fstream>
#include <iostream>

static SymbolType parseSymbolType(const std::string& s) {
    if (s == "star_5" || s == "star") return SymbolType::STAR_5;
    if (s == "star_6") return SymbolType::STAR_6;
    if (s == "star_7") return SymbolType::STAR_7;
    if (s == "stars_circle") return SymbolType::STARS_CIRCLE;
    if (s == "stars_grid") return SymbolType::STARS_GRID;
    if (s == "crescent") return SymbolType::CRESCENT;
    if (s == "crescent_star") return SymbolType::CRESCENT_STAR;
    if (s == "sun") return SymbolType::SUN;
    if (s == "sun_rays") return SymbolType::SUN_RAYS;
    if (s == "cross_latin" || s == "cross") return SymbolType::CROSS_LATIN;
    if (s == "cross_saltir") return SymbolType::CROSS_SALTIR;
    if (s == "cross_maltese") return SymbolType::CROSS_MALTESE;
    if (s == "circle") return SymbolType::CIRCLE;
    if (s == "disc") return SymbolType::DISC;
    if (s == "triangle") return SymbolType::TRIANGLE;
    if (s == "diamond") return SymbolType::DIAMOND;
    if (s == "gear") return SymbolType::GEAR;
    if (s == "hammer_sickle") return SymbolType::HAMMER_SICKLE;
    if (s == "swastika") return SymbolType::SWASTIKA;
    if (s == "sword") return SymbolType::SWORD;
    if (s == "crossed_swords") return SymbolType::CROSSED_SWORDS;
    if (s == "mountain") return SymbolType::MOUNTAIN;
    if (s == "tree") return SymbolType::TREE;
    if (s == "text" || s == "text_block") return SymbolType::TEXT_BLOCK;
    if (s == "coat_arms") return SymbolType::COAT_ARMS;
    if (s == "censor_bar") return SymbolType::CENSOR_BAR;
    return SymbolType::NONE;
}

static FlagPattern parseFlag(const nlohmann::json& j) {
    FlagPattern fp;
    if (j.is_null()) return fp;
    fp.imagePath = j.value("image", "");
    fp.censored = j.value("censored", false);
    if (!fp.imagePath.empty()) return fp; // loaded from file

    std::string type = j.value("type", "solid");
         if (type == "hstripes_2" || type == "hstripes") fp.type = FlagType::HSTRIPES_2;
    else if (type == "hstripes_3")   fp.type = FlagType::HSTRIPES_3;
    else if (type == "hstripes_n")   fp.type = FlagType::HSTRIPES_N;
    else if (type == "vstripes_2" || type == "vstripes") fp.type = FlagType::VSTRIPES_2;
    else if (type == "vstripes_3")   fp.type = FlagType::VSTRIPES_3;
    else if (type == "vstripes_n")   fp.type = FlagType::VSTRIPES_N;
    else if (type == "diagonal_l")   fp.type = FlagType::DIAGONAL_L;
    else if (type == "diagonal_r" || type == "diagonal") fp.type = FlagType::DIAGONAL_R;
    else if (type == "triangle")     fp.type = FlagType::TRIANGLE;
    else if (type == "triangle_double") fp.type = FlagType::TRIANGLE_DOUBLE;
    else if (type == "quartered")    fp.type = FlagType::QUARTERED;
    else if (type == "saltir")       fp.type = FlagType::SALTIR;
    else if (type == "canton")       fp.type = FlagType::CANTON;
    else if (type == "pale")         fp.type = FlagType::PALE;
    else if (type == "fess")         fp.type = FlagType::FESS;
    else if (type == "cross_nordic") fp.type = FlagType::CROSS_NORDIC;
    else if (type == "cross_greek" || type == "cross") fp.type = FlagType::CROSS_GREEK;
    else if (type == "striped_edge") fp.type = FlagType::STRIPED_EDGE;
    else if (type == "sunburst")     fp.type = FlagType::SUNBURST;
    else fp.type = FlagType::SOLID;

    if (j.contains("colors")) {
        for (auto& c : j["colors"]) {
            fp.colors.push_back(hexToColor(c.get<std::string>()));
        }
    }
    fp.starCount = j.value("starCount", 1);

    if (j.contains("symbols")) {
        for (auto& s : j["symbols"]) {
            FlagSymbol fs;
            fs.type = parseSymbolType(s.value("type", ""));
            fs.count = s.value("count", 0);
            fs.text = s.value("text", "");
            fs.x = s.value("x", 0.5);
            fs.y = s.value("y", 0.5);
            fs.size = s.value("size", 0.3);
            fs.rotation = s.value("rotation", 0.0);
            if (s.contains("colors")) {
                for (auto& c : s["colors"]) {
                    fs.colors.push_back(hexToColor(c.get<std::string>()));
                }
            }
            fp.symbols.push_back(fs);
        }
    }
    return fp;
}

bool CountryMap::load(const std::string& jsonPath) {
    std::ifstream f(jsonPath);
    if (!f.is_open()) {
        std::cerr << "Could not open " << jsonPath << std::endl;
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return loadFromJson(content);
}

bool CountryMap::loadFromJson(const std::string& jsonStr) {
    try {
        nlohmann::json j = nlohmann::json::parse(jsonStr);
        for (auto& [key, val] : j.items()) {
            Country c;
            c.id = val["id"];
            c.name = val["name"];
            c.isoA3 = val["iso_a3"];
            if (val.contains("color"))
                c.color = hexToColor(val["color"].get<std::string>());
            if (val.contains("flag_actual"))
                c.flagActual = parseFlag(val["flag_actual"]);
            if (val.contains("flag_censored"))
                c.flagCensored = parseFlag(val["flag_censored"]);
            if (val.contains("treasury"))
                c.treasury = val["treasury"].get<float>();
            if (val.contains("compass_economic"))
                c.compassEconomic = val["compass_economic"].get<float>();
            if (val.contains("compass_social"))
                c.compassSocial = val["compass_social"].get<float>();
            if (val.contains("doctrine"))
                c.doctrine = val["doctrine"].get<std::string>();
            if (val.contains("research")) {
                for (auto& r : val["research"])
                    c.research.push_back(r.get<std::string>());
            }
            m_countries[c.id] = c;
            if (c.id > 65000) {
                std::cout << "  HIGH ID: " << c.id << " (" << c.isoA3 << " - " << c.name << ")" << std::endl;
            }
        }
        return true;
    } catch (std::exception& e) {
        std::cerr << "Country JSON parse error: " << e.what() << std::endl;
        return false;
    }
}

const Country* CountryMap::getCountry(int id) const {
    auto it = m_countries.find(id);
    if (it != m_countries.end()) return &it->second;
    return nullptr;
}

Country* CountryMap::getCountry(int id) {
    auto it = m_countries.find(id);
    if (it != m_countries.end()) return &it->second;
    return nullptr;
}

const Country* CountryMap::getCountryByCode(const std::string& isoA3) const {
    for (auto& [id, c] : m_countries)
        if (c.isoA3 == isoA3) return &c;
    return nullptr;
}
