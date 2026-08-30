#include "CountryMap.h"
#include "util/LoadLog.h"
#include <cstdio>
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
    if (s == "wreath") return SymbolType::WREATH;
    if (s == "hammer") return SymbolType::HAMMER;
    if (s == "lightning") return SymbolType::LIGHTNING;
    if (s == "sun_splendour") return SymbolType::SUN_SPLENDOUR;
    if (s == "anchor") return SymbolType::ANCHOR;
    if (s == "torch") return SymbolType::TORCH;
    if (s == "rose") return SymbolType::ROSE;
    if (s == "fasces") return SymbolType::FASCES;
    if (s == "cross_pattee") return SymbolType::CROSS_PATTEE;
    if (s == "star_4") return SymbolType::STAR_4;
    if (s == "star_of_david") return SymbolType::STAR_OF_DAVID;
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
    // stdio, NOT std::ifstream.
    //
    // This runs inside the map load, and the map load yields to the browser
    // through Asyncify every few thousand provinces so the audio does not
    // stutter. Coming back in, libc++'s iostream machinery traps the whole
    // module -- constructing a basic_filebuf was enough:
    //
    //     RuntimeError: null function
    //       at basic_filebuf<char>::basic_filebuf()
    //       at CountryMap::load()
    //       at Object.doRewind
    //
    // and the load simply stopped, with no message and no world. fopen has
    // none of that machinery behind it. See src/util/LoadLog.h for the same
    // reasoning applied to the logging.
    std::FILE* f = std::fopen(jsonPath.c_str(), "rb");
    if (!f) {
        LoadLog() << "Could not open " << jsonPath << std::endl;
        return false;
    }
    std::string content;
    char buf[16384];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) content.append(buf, n);
    std::fclose(f);
    return loadFromJson(content);
}

bool CountryMap::loadFromJson(const std::string& jsonStr) {
    printf("[countries] loadFromJson %zu bytes\n", jsonStr.size()); fflush(stdout);
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
            // "doctrine" may still appear in older countries.json files. It was
            // a per-country string that nothing ever read -- no combat, economy
            // or unrest maths touched it -- so it is ignored rather than loaded.
            if (val.contains("research")) {
                for (auto& r : val["research"])
                    c.research.push_back(r.get<std::string>());
            }
            m_countries[c.id] = c;
            if (c.id > 65000) {
                printf("  HIGH ID: %d (%s - %s)\n", c.id, c.isoA3.c_str(), c.name.c_str());
            }
        }
        return true;
    } catch (std::exception& e) {
        // printf, NOT LoadLog().
        //
        // Under Asyncify this handler is reached on a REWIND, and the ostream
        // call trapped there with "null function" -- taking the whole load
        // with it and telling nobody why. printf has no virtual dispatch and
        // no locale machinery behind it, so it survives the same path.
        printf("Country JSON parse error: %s\n", e.what());
        fflush(stdout);
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
