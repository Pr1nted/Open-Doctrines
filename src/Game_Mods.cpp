// Mod menu, mod panel rendering, and the bridge from the game world to the
// GameState.Read capability.
//
// The menu is the only place a mod is ever loaded (docs/modding.md, "Lifecycle
// rules"), so everything that can start or stop a mod lives in this file.

#include "Game.h"
#include "Audio.h"
#include "GameInternals.h"
#include "mods/ModManager.h"
#include "mods/ModUpdates.h"
#include "mods/ModHost.h"
#include "net/Host.h"
#include "net/Session.h"
#include "net/Lobby.h"

#include <algorithm>
#include "ai/AISystem.h"
#include <cstdio>
#include <cstring>
#include <unordered_map>

// ----------------------------------------------------- world access -------

namespace {

// Bridges the game's containers to the mod-facing getters. Deliberately narrow:
// a mod sees exactly these fields and nothing else about the world.
class GameModAccess : public ModGameAccess {
public:
    explicit GameModAccess(Game* g) : m_game(g) {}

    uint32_t turnNumber() override { return (uint32_t)m_game->modTurnNumber(); }
    uint32_t countryCount() override { return (uint32_t)m_game->modCountryIds().size(); }

    uint32_t countryAt(uint32_t index) override {
        const auto& ids = m_game->modCountryIds();
        if (index >= ids.size()) return 0xFFFFFFFFu;
        return (uint32_t)ids[index];
    }
    bool countryExists(uint32_t cid) override { return m_game->modCountryExists((int)cid); }
    std::string countryName(uint32_t cid) override { return m_game->modCountryName((int)cid); }
    double countryTreasury(uint32_t cid) override { return m_game->modCountryTreasury((int)cid); }
    uint32_t countryProvinceCount(uint32_t cid) override {
        return (uint32_t)m_game->modCountryProvinceCount((int)cid);
    }
    long long provincePopulation(uint32_t pid) override {
        return m_game->modProvincePopulation((int)pid);
    }
    uint32_t provinceOwner(uint32_t pid) override {
        int o = m_game->modProvinceOwner((int)pid);
        return o <= 0 ? 0xFFFFFFFFu : (uint32_t)o;
    }

private:
    Game* m_game;
    uint32_t mapWidth() override  { return (uint32_t)m_game->modMapWidth(); }
    uint32_t mapHeight() override { return (uint32_t)m_game->modMapHeight(); }
    uint32_t provinceCount() override {
        return (uint32_t)m_game->modProvinceIds().size();
    }
    uint32_t provinceAt(uint32_t index) override {
        const auto& ids = m_game->modProvinceIds();
        return index < ids.size() ? (uint32_t)ids[index] : 0xFFFFFFFFu;
    }
    bool provinceExists(uint32_t pid) override {
        return m_game->modProvinceExists((int)pid);
    }
    std::string provinceName(uint32_t pid) override {
        return m_game->modProvinceName((int)pid);
    }
    double provinceCenterX(uint32_t pid) override {
        return m_game->modProvinceCenterX((int)pid);
    }
    double provinceCenterY(uint32_t pid) override {
        return m_game->modProvinceCenterY((int)pid);
    }
    bool provinceIsLand(uint32_t pid) override {
        return m_game->modProvinceIsLand((int)pid);
    }
    uint32_t provinceNeighborCount(uint32_t pid) override {
        return (uint32_t)m_game->modProvinceNeighborCount((int)pid);
    }
    uint32_t provinceNeighborAt(uint32_t pid, uint32_t index) override {
        int n = m_game->modProvinceNeighborAt((int)pid, (int)index);
        return n < 0 ? 0xFFFFFFFFu : (uint32_t)n;
    }
    bool atWar(uint32_t a, uint32_t b) override {
        return m_game->modAtWar((int)a, (int)b);
    }
    bool allied(uint32_t a, uint32_t b) override {
        return m_game->modAllied((int)a, (int)b);
    }
    bool nonAggression(uint32_t a, uint32_t b) override {
        return m_game->modNonAggression((int)a, (int)b);
    }
    bool guaranteed(uint32_t a, uint32_t b) override {
        return m_game->modGuaranteed((int)a, (int)b);
    }
    bool proposeWar(uint32_t a, uint32_t b) override {
        return m_game->modProposeWar((int)a, (int)b);
    }
    bool setCountryTreasury(uint32_t cid, double v) override {
        return m_game->modSetCountryTreasury((int)cid, v);
    }
    bool addCountryTreasury(uint32_t cid, double d) override {
        return m_game->modAddCountryTreasury((int)cid, d);
    }
    bool setProvinceOwner(uint32_t pid, uint32_t cid) override {
        return m_game->modSetProvinceOwner((int)pid, (int)cid);
    }
    bool setProvincePopulation(uint32_t pid, long long v) override {
        return m_game->modSetProvincePopulation((int)pid, v);
    }
    // ── Gearbox 1.1 ──────────────────────────────────────────────────────────
    // Forwards, nothing more. Every bound, clamp and validity test lives either
    // in the Game accessor or, for the four order calls, in the turn resolver.
    uint32_t shipCount() override { return (uint32_t)m_game->modShipCount(); }
    uint32_t shipAt(uint32_t i) override {
        return i < (uint32_t)m_game->modShipCount() ? i : 0xFFFFFFFFu;
    }
    bool shipExists(uint32_t s) override { return m_game->modShipExists((int)s); }
    uint32_t shipOwner(uint32_t s) override { return (uint32_t)m_game->modShipOwner((int)s); }
    std::string shipType(uint32_t s) override { return m_game->modShipType((int)s); }
    double shipLon(uint32_t s) override { return m_game->modShipLon((int)s); }
    double shipLat(uint32_t s) override { return m_game->modShipLat((int)s); }
    int32_t shipHealth(uint32_t s) override { return m_game->modShipHealth((int)s); }
    int32_t shipCrew(uint32_t s) override { return m_game->modShipCrew((int)s); }
    double shipRange(uint32_t s) override { return m_game->modShipRange((int)s); }
    uint32_t armyStackCount(uint32_t p) override { return (uint32_t)m_game->modArmyStackCount((int)p); }
    uint32_t armyStackOwner(uint32_t p, uint32_t i) override { return (uint32_t)m_game->modArmyStackOwner((int)p, (int)i); }
    long long armyStackSize(uint32_t p, uint32_t i) override { return m_game->modArmyStackSize((int)p, (int)i); }
    long long countryArmy(uint32_t c) override { return m_game->modCountryArmy((int)c); }
    int32_t provinceFortification(uint32_t p) override { return m_game->modProvinceFortification((int)p); }
    int32_t provincePortLevel(uint32_t p) override { return m_game->modProvincePortLevel((int)p); }

    bool orderArmyMove(uint32_t f, uint32_t t, uint32_t pct) override {
        return m_game->modOrderArmyMove((int)f, (int)t, (int)pct);
    }
    bool orderShipMove(uint32_t s, double lon, double lat) override {
        return m_game->modOrderShipMove((int)s, lon, lat);
    }
    bool orderShipEngage(uint32_t s, uint32_t t) override {
        return m_game->modOrderShipEngage((int)s, (int)t);
    }
    bool orderShipBombard(uint32_t s, uint32_t p, const std::string& ammo) override {
        return m_game->modOrderShipBombard((int)s, (int)p, ammo);
    }

    uint32_t researchNodeCount() override { return (uint32_t)m_game->modResearchNodeCount(); }
    std::string researchNodeId(uint32_t i) override { return m_game->modResearchNodeId((int)i); }
    std::string researchNodeName(uint32_t i) override { return m_game->modResearchNodeName((int)i); }
    std::string researchNodeCategory(uint32_t i) override { return m_game->modResearchNodeCategory((int)i); }
    int32_t researchNodeCost(uint32_t i) override { return m_game->modResearchNodeCost((int)i); }
    bool countryHasResearched(uint32_t c, const std::string& n) override { return m_game->modCountryHasResearched((int)c, n); }
    double countryResearchFunding(uint32_t c) override { return m_game->modCountryResearchFunding((int)c); }
    bool setCountryResearchFunding(uint32_t c, double v) override { return m_game->modSetCountryResearchFunding((int)c, v); }

    double countryCompassEcon(uint32_t c) override { return m_game->modCountryCompassEcon((int)c); }
    double countryCompassSocial(uint32_t c) override { return m_game->modCountryCompassSocial((int)c); }
    double provinceUnrest(uint32_t p) override { return m_game->modProvinceUnrest((int)p); }
    uint32_t policyCount() override { return (uint32_t)m_game->modPolicyCount(); }
    std::string policyId(uint32_t i) override { return m_game->modPolicyId((int)i); }
    std::string policyName(uint32_t i) override { return m_game->modPolicyName((int)i); }
    bool countryHasPolicy(uint32_t c, const std::string& p) override { return m_game->modCountryHasPolicy((int)c, p); }
    bool setCountryPolicy(uint32_t c, const std::string& p, bool on) override { return m_game->modSetCountryPolicy((int)c, p, on); }
    uint32_t provinceMinorityCount(uint32_t p) override { return (uint32_t)m_game->modProvinceMinorityCount((int)p); }
    std::string provinceMinorityName(uint32_t p, uint32_t i) override { return m_game->modProvinceMinorityName((int)p, (int)i); }
    double provinceMinorityShare(uint32_t p, uint32_t i) override { return m_game->modProvinceMinorityShare((int)p, (int)i); }

    double countryIncomeGross(uint32_t c) override { return m_game->modCountryIncomeGross((int)c); }
    double countryIncomeNet(uint32_t c) override { return m_game->modCountryIncomeNet((int)c); }
    double countryArmyUpkeep(uint32_t c) override { return m_game->modCountryArmyUpkeep((int)c); }
    double countryNavyUpkeep(uint32_t c) override { return m_game->modCountryNavyUpkeep((int)c); }
    bool countryIsBankrupt(uint32_t c) override { return m_game->modCountryIsBankrupt((int)c); }
    int32_t provinceIndustryLevel(uint32_t p) override { return m_game->modProvinceIndustryLevel((int)p); }
    std::string provinceIndustrySpecialization(uint32_t p) override { return m_game->modProvinceIndustrySpecialization((int)p); }
    double provinceResource(uint32_t p, const std::string& w) override { return m_game->modProvinceResource((int)p, w); }
    bool setProvinceIndustryLevel(uint32_t p, int32_t l) override { return m_game->modSetProvinceIndustryLevel((int)p, l); }

    bool provinceIsCoastal(uint32_t p) override { return m_game->modProvinceIsCoastal((int)p); }
    bool seaRouteExists(double a, double b, double c, double d) override { return m_game->modSeaRouteExists(a, b, c, d); }
    bool pointIsLand(double lon, double lat) override { return m_game->modPointIsLand(lon, lat); }


    // ── mapeditor (ABI 1.1) ──
    bool editorActive() override { return m_game->modEditorActive(); }
    uint32_t editorProvinceCount() override { return (uint32_t)m_game->modEditorProvinceCount(); }
    uint32_t editorProvinceAt(uint32_t i) override {
        int p = m_game->modEditorProvinceAt((int)i);
        return p < 0 ? 0xFFFFFFFFu : (uint32_t)p;
    }
    long long editorProvincePopulation(uint32_t p) override { return m_game->modEditorProvincePopulation((int)p); }
    int32_t editorProvinceIndustryLevel(uint32_t p) override { return m_game->modEditorProvinceIndustryLevel((int)p); }
    int32_t editorProvinceFortification(uint32_t p) override { return m_game->modEditorProvinceFortification((int)p); }
    int32_t editorProvincePortLevel(uint32_t p) override { return m_game->modEditorProvincePortLevel((int)p); }
    double editorProvinceResource(uint32_t p, const std::string& w) override { return m_game->modEditorProvinceResource((int)p, w); }
    double editorProvinceCompassEcon(uint32_t p) override { return m_game->modEditorProvinceCompassEcon((int)p); }
    double editorProvinceCompassSocial(uint32_t p) override { return m_game->modEditorProvinceCompassSocial((int)p); }
    bool editorSetProvincePopulation(uint32_t p, long long v) override { return m_game->modEditorSetProvincePopulation((int)p, v); }
    bool editorSetProvinceIndustryLevel(uint32_t p, int32_t v) override { return m_game->modEditorSetProvinceIndustryLevel((int)p, v); }
    bool editorSetProvinceFortification(uint32_t p, int32_t v) override { return m_game->modEditorSetProvinceFortification((int)p, v); }
    bool editorSetProvincePortLevel(uint32_t p, int32_t v) override { return m_game->modEditorSetProvincePortLevel((int)p, v); }
    bool editorSetProvinceResource(uint32_t p, const std::string& w, double v) override { return m_game->modEditorSetProvinceResource((int)p, w, v); }
    bool editorSetProvinceCompass(uint32_t p, double e2, double so) override { return m_game->modEditorSetProvinceCompass((int)p, e2, so); }
    std::string editorMapName() override { return m_game->modEditorMapName(); }
    bool editorSetMapName(const std::string& n) override { return m_game->modEditorSetMapName(n); }
    bool editorSetAuthor(const std::string& a) override { return m_game->modEditorSetAuthor(a); }
    bool editorSetLicense(const std::string& l) override { return m_game->modEditorSetLicense(l); }

    // ── net (ABI 1.1) ──
    uint32_t netPeerAt(uint32_t i) override {
        int p = m_game->modNetPeerAt((int)i);
        return p < 0 ? 0xFFFFFFFFu : (uint32_t)p;
    }
    std::string netPeerName(uint32_t i) override { return m_game->modNetPeerName((int)i); }
    uint32_t netMaxMessageBytes() override { return (uint32_t)m_game->modNetMaxMessageBytes(); }

    // ── neural (ABI 1.1) ──
    uint32_t neuralModuleCount() override { return (uint32_t)m_game->modNeuralModuleCount(); }
    std::string neuralModuleName(uint32_t m) override { return m_game->modNeuralModuleName((int)m); }
    uint32_t neuralActionCount(uint32_t m) override { return (uint32_t)m_game->modNeuralActionCount((int)m); }
    std::string neuralActionName(uint32_t m, uint32_t a) override { return m_game->modNeuralActionName((int)m, (int)a); }
    bool neuralCountryIsAI(uint32_t c) override { return m_game->modNeuralCountryIsAI((int)c); }
    long long neuralUpdateCount() override { return m_game->modNeuralUpdateCount(); }
    bool neuralModelLoaded() override { return m_game->modNeuralModelLoaded(); }

    uint32_t neuralFeatureCount() override {
        return (uint32_t)m_game->modNeuralFeatureCount();
    }
    uint32_t neuralFeatures(uint32_t cid, float* out, uint32_t cap) override {
        return (uint32_t)m_game->modNeuralFeatures((int)cid, out, (int)cap);
    }
    uint32_t neuralRewardCount() override {
        return (uint32_t)m_game->modNeuralRewardCount();
    }
    double neuralRewardMean(uint32_t i) override {
        return m_game->modNeuralRewardMean((int)i);
    }
};

GameModAccess* g_access = nullptr;

// macOS-only native picker, mirroring the one in Game_Browsers.cpp (which is
// static to that translation unit). Everywhere else, drag-and-drop is the path
// -- and on web it is the only one.
std::string pickOdmodFile() {
#ifdef __APPLE__
    std::string script =
        "osascript -e 'POSIX path of (choose file with prompt \"Select a .odmod file\" "
        "of type {\"odmod\"})' 2>/dev/null";
    FILE* pipe = popen(script.c_str(), "r");
    if (!pipe) return "";
    char buf[1024];
    std::string out;
    while (fgets(buf, sizeof buf, pipe)) out += buf;
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
#else
    return "";
#endif
}

// A mod's own art, decoded once and kept.
//
// KEYED BY (mod id, asset name), NOT BY NAME. Two mods that both ship
// "bg.png" are two different pictures, and a shared cache would hand one mod
// the other's -- a way to read another mod's package that the assets module
// carefully does not give.
//
// Decoding happens HERE rather than in ModHost because the host does not link
// raylib. Bytes come out of the mod's package by the same readAsset the assets
// capability uses, so a mod still cannot name a file outside its own .odmod.
//
// A name that fails to decode is remembered as a null entry, so a mod drawing a
// broken image every frame costs one failed decode, not sixty a second.
std::map<std::pair<std::string, std::string>, Texture2D> g_modImages;

const Texture2D* modImage(const std::string& modId, const std::string& name) {
    auto key = std::make_pair(modId, name);
    auto it = g_modImages.find(key);
    if (it != g_modImages.end())
        return it->second.id > 0 ? &it->second : nullptr;

    Texture2D tex{};
    const ModPackage* pkg = nullptr;
    for (const auto& m : ModManager::get().mods())
        if (m.id == modId) { pkg = m.package.get(); break; }
    std::vector<uint8_t> bytes;
    if (pkg && pkg->readAsset(name, bytes) && !bytes.empty() && bytes.size() < 32u * 1024 * 1024) {
        // The extension decides the decoder; raylib needs it and the name a mod
        // passed is the only hint available.
        std::string ext = ".png";
        size_t dot = name.rfind('.');
        if (dot != std::string::npos && name.size() - dot <= 5) ext = name.substr(dot);
        Image img = LoadImageFromMemory(ext.c_str(), bytes.data(), (int)bytes.size());
        if (img.data) {
            tex = LoadTextureFromImage(img);
            UnloadImage(img);
        }
    }
    g_modImages[key] = tex;                 // null too, so failures cost once
    return tex.id > 0 ? &g_modImages[key] : nullptr;
}

Color colorFromRGBA(uint32_t rgba) {
    return Color{(unsigned char)((rgba >> 24) & 0xFF), (unsigned char)((rgba >> 16) & 0xFF),
                 (unsigned char)((rgba >> 8) & 0xFF), (unsigned char)(rgba & 0xFF)};
}

}  // namespace

// -------------------------------------------------- world accessors -------

int Game::modTurnNumber() const { return m_turnNumber; }

const std::vector<int>& Game::modCountryIds() const {
    // Rebuilt when the turn changes; a mod walking every country would
    // otherwise pay for a full map rebuild on each call.
    if (m_modCountryIdsTurn != m_turnNumber || m_modCountryIds.empty()) {
        m_modCountryIds.clear();
        for (const auto& kv : m_countries.getAll()) {
            if (kv.first <= 0 || kv.first >= REBEL_CID_MIN) continue;
            m_modCountryIds.push_back(kv.first);
        }
        std::sort(m_modCountryIds.begin(), m_modCountryIds.end());
        m_modCountryIdsTurn = m_turnNumber;
        m_modProvCounts.clear();
    }
    return m_modCountryIds;
}

bool Game::modCountryExists(int cid) const {
    return m_countries.getCountry(cid) != nullptr;
}

std::string Game::modCountryName(int cid) const {
    const Country* c = m_countries.getCountry(cid);
    return c ? c->name : std::string();
}

double Game::modCountryTreasury(int cid) const {
    const Country* c = m_countries.getCountry(cid);
    return c ? c->treasury : 0.0;
}

int Game::modCountryProvinceCount(int cid) const {
    if (m_modProvCounts.empty()) {
        for (size_t pid = 0; pid < m_provinceCountryLookup.size(); ++pid) {
            int owner = m_provinceCountryLookup[pid];
            if (owner > 0) m_modProvCounts[owner]++;
        }
    }
    auto it = m_modProvCounts.find(cid);
    return it == m_modProvCounts.end() ? 0 : it->second;
}

long long Game::modProvincePopulation(int pid) const {
    auto it = m_provincePopulations.find(pid);
    return it == m_provincePopulations.end() ? 0 : it->second;
}

int Game::modProvinceOwner(int pid) const {
    if (pid < 0 || (size_t)pid >= m_provinceCountryLookup.size()) return 0;
    return m_provinceCountryLookup[pid];
}

// ---------------------------------------------------------------- Map -----
//
// Geometry only. Adjacency and centres are computed once at load, so every one
// of these is a lookup rather than a scan -- which matters because a mod may
// call them from a per-frame draw hook.

int Game::modMapWidth() const  { return m_provinces.getWidth(); }
int Game::modMapHeight() const { return m_provinces.getHeight(); }

const std::vector<int>& Game::modProvinceIds() const {
    // m_provinces is an unordered_map, so its iteration order is not stable
    // across runs. A mod indexing provinces needs an order that is, hence the
    // sorted cache. Provinces do not appear or vanish mid-game, so unlike the
    // country list this is built once.
    if (m_modProvinceIds.empty()) {
        for (const auto& kv : m_provinces.getAllProvinces())
            m_modProvinceIds.push_back(kv.first);
        std::sort(m_modProvinceIds.begin(), m_modProvinceIds.end());
    }
    return m_modProvinceIds;
}

bool Game::modProvinceExists(int pid) const {
    return m_provinces.getAllProvinces().count(pid) != 0;
}

std::string Game::modProvinceName(int pid) const {
    const auto& all = m_provinces.getAllProvinces();
    auto it = all.find(pid);
    return it == all.end() ? std::string() : it->second.name;
}

float Game::modProvinceCenterX(int pid) const {
    auto it = m_provinceCenters.find(pid);
    return it == m_provinceCenters.end() ? 0.0f : it->second.x;
}

float Game::modProvinceCenterY(int pid) const {
    auto it = m_provinceCenters.find(pid);
    return it == m_provinceCenters.end() ? 0.0f : it->second.y;
}

bool Game::modProvinceIsLand(int pid) const {
    // Sampled at the province centre. A province is land or sea as a whole in
    // this game, so one sample is the answer rather than an approximation of it.
    auto it = m_provinceCenters.find(pid);
    if (it == m_provinceCenters.end()) return false;
    return m_landSea.isLand((int)it->second.x, (int)it->second.y);
}

int Game::modProvinceNeighborCount(int pid) const {
    auto it = m_provinceNeighbors.find(pid);
    return it == m_provinceNeighbors.end() ? 0 : (int)it->second.size();
}

int Game::modProvinceNeighborAt(int pid, int index) const {
    auto it = m_provinceNeighbors.find(pid);
    if (it == m_provinceNeighbors.end()) return -1;
    if (index < 0 || (size_t)index >= it->second.size()) return -1;
    return it->second[index];
}

// --------------------------------------------------------- Diplomacy -----
//
// m_relations is keyed by isoA3 and the relation is stored on both sides, so
// each read resolves two ids to codes and looks at one direction.

const std::string* Game::modIsoFor(int cid) const {
    const Country* c = m_countries.getCountry(cid);
    return c ? &c->isoA3 : nullptr;
}

bool Game::modRelationFlag(int a, int b, int which) const {
    const std::string* ia = modIsoFor(a);
    const std::string* ib = modIsoFor(b);
    if (!ia || !ib || ia->empty() || ib->empty() || *ia == *ib) return false;
    auto row = m_relations.find(*ia);
    if (row == m_relations.end()) return false;
    auto cell = row->second.find(*ib);
    if (cell == row->second.end()) return false;
    switch (which) {
        case 0: return cell->second.war;
        case 1: return cell->second.alliance;
        case 2: return cell->second.nonAggression;
        case 3: return cell->second.guarantee;
    }
    return false;
}

bool Game::modAtWar(int a, int b) const          { return modRelationFlag(a, b, 0); }
bool Game::modAllied(int a, int b) const         { return modRelationFlag(a, b, 1); }
bool Game::modNonAggression(int a, int b) const  { return modRelationFlag(a, b, 2); }
bool Game::modGuaranteed(int a, int b) const     { return modRelationFlag(a, b, 3); }

bool Game::modProposeWar(int attacker, int defender) {
    const std::string* ia = modIsoFor(attacker);
    const std::string* ib = modIsoFor(defender);
    if (!ia || !ib || ia->empty() || ib->empty()) return false;
    if (*ia == *ib) return false;                 // no civil war by this route
    if (modAtWar(attacker, defender)) return false;   // already at war

    // declareWar owns the consequences -- guarantee chains, kin penalties, the
    // lot. Reimplementing any of that here would let a mod produce a state the
    // game itself could never reach.
    declareWar(*ia, *ib, true);
    return true;
}

// ----------------------------------------------------- GameState.Write ----
//
// The only capability that can change the world, so every entry point here
// validates first and refuses rather than half-applying. Values are clamped to
// a range the game's own economy stays inside; a mod handing us an infinity
// would otherwise poison every later calculation.

namespace {
// Treasuries are doubles but the economy is not meaningful past this, and
// letting NaN or infinity in would silently corrupt every downstream sum.
constexpr double kTreasuryLimit = 1e12;
// A province holding more than this is not a state the game's economy or
// rebellion maths stays sane in; refuse rather than store it.
constexpr long long kMaxProvincePopulation = 100000000000LL;   // 100 billion
bool finiteAndSane(double v) {
    return v == v && v > -kTreasuryLimit && v < kTreasuryLimit;
}
}  // namespace

bool Game::modSetCountryTreasury(int cid, double value) {
    if (!finiteAndSane(value)) return false;
    Country* c = m_countries.getCountry(cid);
    if (!c) return false;
    c->treasury = value;
    return true;
}

bool Game::modAddCountryTreasury(int cid, double delta) {
    if (!finiteAndSane(delta)) return false;
    Country* c = m_countries.getCountry(cid);
    if (!c) return false;
    double next = c->treasury + delta;
    if (!finiteAndSane(next)) return false;
    c->treasury = next;
    return true;
}

bool Game::modSetProvincePopulation(int pid, long long value) {
    // Population lives in TWO structures: m_provincePopulations (the map the
    // rest of the game reads) and m_provincePopArray (a dense mirror built at
    // load for the population texture and the hot loops in Game_Loading).
    // Writing one and not the other makes them disagree silently, and the
    // symptom surfaces turns later as a province whose population depends on
    // which code path asked. Both are updated here, together.
    if (value < 0) return false;
    if (value > kMaxProvincePopulation) return false;
    if (m_provincePopulations.find(pid) == m_provincePopulations.end())
        return false;                        // unknown province

    m_provincePopulations[pid] = value;
    if (pid >= 0 && (size_t)pid < m_provincePopArray.size())
        m_provincePopArray[pid] = value;
    return true;
}

bool Game::modSetProvinceOwner(int pid, int toCid) {
    const Province* pp = m_provinces.getProvinceById(pid);
    if (!pp) return false;
    if (!m_countries.getCountry(toCid)) return false;
    int fromCid = pp->countryId;
    if (fromCid == toCid) return false;          // nothing to do

    // The shared implementation, so a mod's transfer and a ceasefire's are the
    // same operation and cannot drift apart.
    transferProvinceOwnership(pid, fromCid, toCid);

    // Every existing caller also disbands the previous owner's troops left in
    // the province; a mod-driven transfer that skipped it would leave an army
    // sitting inside territory its country no longer owns.
    auto it = m_provinceArmies.find(pid);
    if (it != m_provinceArmies.end()) {
        it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
            [fromCid](const ArmyUnit& u) { return u.countryId == fromCid; }),
            it->second.end());
    }
    return true;
}

// -------------------------------------------------------------- Neural ----
//
// OBSERVE ONLY, deliberately. The capability is documented as being able to
// corrupt a trained model, and data/ai/model.bin represents hours of training,
// so v1 exposes no path that can write to the model, the optimiser state, or
// the reward history. A mod can see what the AI sees; it cannot change it.
//
// If an override is ever added it should be a separate capability, not a
// widening of this one, so that granting "let this mod read the AI" never
// silently becomes "let this mod retrain it".

int Game::modNeuralFeatureCount() const {
    return AISystem::FEATURE_COUNT;
}

int Game::modNeuralFeatures(int cid, float* out, int cap) const {
    if (!m_ai) return 0;
    if (!modCountryExists(cid)) return 0;
    std::vector<float> f;
    m_ai->modObserveFeatures(cid, f);
    int n = (int)f.size();
    if (out && cap > 0) {
        int take = n < cap ? n : cap;
        for (int i = 0; i < take; i++) out[i] = f[(size_t)i];
    }
    return n;                       // full length, per two-call sizing
}

int Game::modNeuralRewardCount() const {
    return m_ai ? (int)AISystem::MOD_COUNT : 0;
}

double Game::modNeuralRewardMean(int index) const {
    if (!m_ai || index < 0 || index >= (int)AISystem::MOD_COUNT) return 0.0;
    return (double)m_ai->rewardMeans()[index];
}


// ─────────────────────────────────── mapeditor (ABI 1.1) ──────────────────
//
// GATED ON THE SCREEN, NOT ON THE CAPABILITY ALONE. A mod holding MapEditor
// still gets nothing unless the player is actually in the editor: the data it
// reaches is an editor PROJECT, and there is no such thing while a game is
// running. Every call below therefore starts at editor(), which is null
// anywhere but SCREEN_MAP_EDITOR.
//
// The write calls do not create provinces. A pid the project does not have
// fails rather than inventing one, because a province with data and no shape on
// the province bitmap exports an .odmap the game cannot load.

MapEditor* Game::modEditorOrNull() const {
    if (m_currentScreen != SCREEN_MAP_EDITOR || !m_mapEditor) return nullptr;
    // A project dialog is open: the project underneath is not in a state a mod
    // should be editing, and may be about to be replaced.
    if (m_mapEditor->isInProjectDialog()) return nullptr;
    return m_mapEditor;
}

bool Game::modEditorActive() const { return modEditorOrNull() != nullptr; }

int Game::modEditorProvinceCount() const {
    MapEditor* ed = modEditorOrNull();
    return ed ? (int)ed->modProvinceIds().size() : 0;
}
int Game::modEditorProvinceAt(int index) const {
    MapEditor* ed = modEditorOrNull();
    if (!ed) return -1;
    auto ids = ed->modProvinceIds();
    return (index >= 0 && (size_t)index < ids.size()) ? ids[(size_t)index] : -1;
}

// One accessor per field would be one guard per field. This is the guard.
const MapEditor::EditorProvinceData* Game::modEditorProv(int pid) const {
    MapEditor* ed = modEditorOrNull();
    return ed ? ed->modProvince(pid) : nullptr;
}
MapEditor::EditorProvinceData* Game::modEditorProvMut(int pid) {
    MapEditor* ed = modEditorOrNull();
    return ed ? ed->modProvinceMut(pid) : nullptr;
}

long long Game::modEditorProvincePopulation(int pid) const {
    const auto* d = modEditorProv(pid); return d ? d->population : 0;
}
int Game::modEditorProvinceIndustryLevel(int pid) const {
    const auto* d = modEditorProv(pid); return d ? d->industryLevel : 0;
}
int Game::modEditorProvinceFortification(int pid) const {
    const auto* d = modEditorProv(pid); return d ? d->fortification : 0;
}
int Game::modEditorProvincePortLevel(int pid) const {
    const auto* d = modEditorProv(pid); return d ? d->portLevel : 0;
}
double Game::modEditorProvinceCompassEcon(int pid) const {
    const auto* d = modEditorProv(pid); return d ? (double)d->compassEconomic : 0.0;
}
double Game::modEditorProvinceCompassSocial(int pid) const {
    const auto* d = modEditorProv(pid); return d ? (double)d->compassSocial : 0.0;
}

// The five the editor itself knows about. An unknown name reads 0 and writes
// nothing rather than being silently mapped onto oil.
namespace {
float* editorResourceField(MapEditor::EditorProvinceData& d, const std::string& w) {
    if (w == "oil")       return &d.oil;
    if (w == "gold")      return &d.gold;
    if (w == "rubber")    return &d.rubber;
    if (w == "gemstones") return &d.gemstones;
    if (w == "metal")     return &d.metal;
    return nullptr;
}
}  // namespace

double Game::modEditorProvinceResource(int pid, const std::string& which) const {
    const auto* d = modEditorProv(pid);
    if (!d) return 0.0;
    auto copy = *d;                       // the lookup wants a mutable ref
    const float* f = editorResourceField(copy, which);
    return f ? (double)*f : 0.0;
}
bool Game::modEditorSetProvinceResource(int pid, const std::string& which, double v) {
    auto* d = modEditorProvMut(pid);
    if (!d) return false;
    float* f = editorResourceField(*d, which);
    if (!f) return false;
    *f = (float)std::clamp(v, 0.0, 100.0);
    return true;
}

bool Game::modEditorSetProvincePopulation(int pid, long long v) {
    auto* d = modEditorProvMut(pid);
    if (!d) return false;
    d->population = std::clamp<long long>(v, 0, 2000000000LL);
    return true;
}
bool Game::modEditorSetProvinceIndustryLevel(int pid, int v) {
    auto* d = modEditorProvMut(pid);
    if (!d) return false;
    d->industryLevel = std::clamp(v, 0, 10);       // the editor's own range
    return true;
}
bool Game::modEditorSetProvinceFortification(int pid, int v) {
    auto* d = modEditorProvMut(pid);
    if (!d) return false;
    d->fortification = std::clamp(v, 0, 5);
    return true;
}
bool Game::modEditorSetProvincePortLevel(int pid, int v) {
    auto* d = modEditorProvMut(pid);
    if (!d) return false;
    d->portLevel = std::clamp(v, 0, 3);
    return true;
}
bool Game::modEditorSetProvinceCompass(int pid, double econ, double social) {
    auto* d = modEditorProvMut(pid);
    if (!d) return false;
    d->compassEconomic = (float)std::clamp(econ, -100.0, 100.0);
    d->compassSocial   = (float)std::clamp(social, -100.0, 100.0);
    return true;
}

std::string Game::modEditorMapName() const {
    MapEditor* ed = modEditorOrNull();
    return ed ? ed->getMapName() : std::string();
}
bool Game::modEditorSetMapName(const std::string& n) {
    MapEditor* ed = modEditorOrNull();
    if (!ed || n.empty() || n.size() > 96) return false;
    ed->setMapName(n);
    return true;
}
bool Game::modEditorSetAuthor(const std::string& a) {
    MapEditor* ed = modEditorOrNull();
    if (!ed || a.size() > 96) return false;
    ed->modSetAuthor(a);
    return true;
}
bool Game::modEditorSetLicense(const std::string& l) {
    MapEditor* ed = modEditorOrNull();
    if (!ed || l.size() > 96) return false;
    ed->modSetLicense(l);
    return true;
}

// ─────────────────────────────────────── net (ABI 1.1) ────────────────────

// WHAT THE MOD SYSTEM IS TOLD ABOUT THE NETWORK.
//
// ModHostContext::netRole was never assigned by anything. It sat at Standalone
// for the life of the process, which meant gearbox_is_multiplayer() and
// gearbox_is_server() -- both shipped in Gearbox 1.0, both documented, both the
// obvious way for a mod to ask -- always answered "single player, and you are
// the authority". A mod that did the correct thing and checked before mutating
// got the wrong answer every time.
//
// ModManager::setNetContext was equally dead, and that one is worse than a
// wrong answer: it feeds modSideGrantMask, so a mod declaring "side": "server"
// was never masked off on a client, and a client-side mod was never masked off
// on a dedicated host. The side field did nothing at all.
//
// Called every frame the mod system is live and again before any reload, since
// the role changes when a session opens or closes rather than on a schedule.
void Game::syncModNetContext() {
    const bool mp   = modNetIsMultiplayer();
    const bool auth = modNetIsAuthoritative();

    ModNetRole role = ModNetRole::Standalone;
    if (mp) {
        // HostPlayer rather than Server: this build has no dedicated server
        // mode, so an authoritative copy always has somebody playing on it.
        role = auth ? ModNetRole::HostPlayer : ModNetRole::Client;
    }
    g_modHost.netRole = role;
    ModManager::get().setNetContext(mp, auth);
}

bool Game::modNetIsMultiplayer() const { return m_netHost || m_netSession; }

// THE ONE A MULTIPLAYER MOD MOST NEEDS. A mod that mutates the world wherever
// it runs desynchronises the game the moment two clients disagree; the correct
// shape is "decide on the authority, broadcast the result". Until now a mod had
// no way to tell which side it was on -- is_host answers a different question,
// because a listen-server client is not the host but IS authoritative for
// nothing, and a single-player game has no host at all yet is authoritative for
// everything.
bool Game::modNetIsAuthoritative() const {
    if (!modNetIsMultiplayer()) return true;   // single player: always
    return m_netHost != nullptr;
}

int Game::modNetPeerAt(int index) const {
    if (index < 0) return -1;
    if (m_netHost) {
        const auto& r = m_netHost->lobby().members();
        return (size_t)index < r.size() ? (int)r[(size_t)index].peerId : -1;
    }
    if (m_netSession) {
        const auto& r = m_netSession->roster();
        return (size_t)index < r.size() ? (int)r[(size_t)index].peerId : -1;
    }
    return -1;
}

// The display name, which is the only peer identity a mod gets. Notably NOT the
// psid or the account issuer: a mod has no business correlating players across
// sessions, and handing it a stable identifier would let it.
std::string Game::modNetPeerName(int index) const {
    if (index < 0) return {};
    if (m_netHost) {
        const auto& r = m_netHost->lobby().members();
        return (size_t)index < r.size() ? r[(size_t)index].name : std::string();
    }
    if (m_netSession) {
        const auto& r = m_netSession->roster();
        return (size_t)index < r.size() ? r[(size_t)index].name : std::string();
    }
    return {};
}

// Matches the cap net_send enforces. Published so a mod can chunk its own
// payloads rather than discovering the limit by having a message dropped.
int Game::modNetMaxMessageBytes() const { return 8192; }

// ──────────────────────────────────── neural (ABI 1.1) ────────────────────
//
// The decision space, by name. A mod reading neural_features gets 143 floats
// with no idea what any of them mean; what it can usefully reason about is
// WHAT THE AI CAN DO, and that is what this exposes. Enough to build an
// advisor, a decision log or an alternative AI that speaks the same vocabulary.
//
// The feature vector itself is deliberately left unnamed. The 143 slots are an
// implementation detail of buildFeatures that has changed several times and
// will change again; naming them here would publish a layout the game does not
// promise to keep, and a mod written against those names would break silently
// rather than loudly.

namespace {
const char* const kAiModuleNames[] = {"economy", "politics", "war", "navy"};

// These mirror the comments above ECON_ACTIONS and friends in AISystem.h. If an
// action is added there, add it here -- the count assertion below fails the
// build if the two lists drift apart.
const char* const kEconActionNames[] = {
    "save", "industry", "fortification", "port", "specialize",
    "build_destroyer", "build_carrier", "research_fund_up", "research_fund_down",
    "research_focus_buildings", "research_focus_army", "research_focus_navy"};
const char* const kPolActionNames[] = {
    "hold", "enact_policy", "pacify_up", "pacify_down", "cancel_policy",
    "propose_alliance", "propose_nap", "propose_guarantee",
    "enact_calming_policy", "conciliate_minority", "repress_minority",
    "propose_trade"};
const char* const kWarActionNames[] = {
    "hold", "recruit", "reinforce", "attack", "declare_war", "artillery",
    "offer_ceasefire", "stage_troops"};
const char* const kNavyActionNames[] = {
    "hold", "move_fleet", "bombard", "embark", "disembark", "scrap", "engage"};

static_assert(sizeof(kEconActionNames) / sizeof(char*) == AISystem::ECON_ACTIONS,
              "econ action names drifted from ECON_ACTIONS");
static_assert(sizeof(kPolActionNames) / sizeof(char*) == AISystem::POL_ACTIONS,
              "politics action names drifted from POL_ACTIONS");
static_assert(sizeof(kWarActionNames) / sizeof(char*) == AISystem::WAR_ACTIONS,
              "war action names drifted from WAR_ACTIONS");
static_assert(sizeof(kNavyActionNames) / sizeof(char*) == AISystem::NAVY_ACTIONS,
              "navy action names drifted from NAVY_ACTIONS");
}  // namespace

int Game::modNeuralModuleCount() const { return m_ai ? (int)AISystem::MOD_COUNT : 0; }

std::string Game::modNeuralModuleName(int m) const {
    if (!m_ai || m < 0 || m >= (int)AISystem::MOD_COUNT) return {};
    return kAiModuleNames[m];
}

int Game::modNeuralActionCount(int m) const {
    if (!m_ai) return 0;
    switch (m) {
        case AISystem::MOD_ECONOMY:  return AISystem::ECON_ACTIONS;
        case AISystem::MOD_POLITICS: return AISystem::POL_ACTIONS;
        case AISystem::MOD_WAR:      return AISystem::WAR_ACTIONS;
        case AISystem::MOD_NAVY:     return AISystem::NAVY_ACTIONS;
        default: return 0;
    }
}

std::string Game::modNeuralActionName(int m, int a) const {
    if (a < 0 || a >= modNeuralActionCount(m)) return {};
    switch (m) {
        case AISystem::MOD_ECONOMY:  return kEconActionNames[a];
        case AISystem::MOD_POLITICS: return kPolActionNames[a];
        case AISystem::MOD_WAR:      return kWarActionNames[a];
        case AISystem::MOD_NAVY:     return kNavyActionNames[a];
        default: return {};
    }
}

bool Game::modNeuralCountryIsAI(int cid) const {
    return modCountryExists(cid) && cid != m_playerCountryId;
}

long long Game::modNeuralUpdateCount() const {
    return m_ai ? (long long)m_ai->totalUpdates() : 0;
}

bool Game::modNeuralModelLoaded() const { return m_ai != nullptr; }

// ------------------------------------------------------- mod speakers ------

namespace {

// A sound a mod started. The mod id rides along so one mod cannot stop, mute or
// query another's -- the handle alone would be forgeable by counting.
struct ModSound {
    std::string owner;
    Wave        wave{};
    Sound       sound{};
};

std::unordered_map<uint32_t, ModSound> g_modSounds;
uint32_t                               g_nextModSound = 1;

/** Reap finished one-shots, so a mod that fires and forgets does not leak. */
void modSoundsCollect() {
    for (auto it = g_modSounds.begin(); it != g_modSounds.end();) {
        if (IsSoundPlaying(it->second.sound)) { ++it; continue; }
        UnloadSound(it->second.sound);
        UnloadWave(it->second.wave);
        it = g_modSounds.erase(it);
    }
}

}  // namespace

void Game::unloadModSounds(const std::string& modId) {
    for (auto it = g_modSounds.begin(); it != g_modSounds.end();) {
        if (!modId.empty() && it->second.owner != modId) { ++it; continue; }
        StopSound(it->second.sound);
        UnloadSound(it->second.sound);
        UnloadWave(it->second.wave);
        it = g_modSounds.erase(it);
    }
}

void Game::installModBridges() {
    ModAudioBridge audio;

    audio.play = [](const std::string& owner, const std::string& ext,
                    const std::vector<uint8_t>& bytes, float volume) -> uint32_t {
        // Silence is a valid outcome everywhere in this game: no device, or a
        // file raylib cannot decode, leaves the mod running and quiet.
        if (Audio::s_disabled || !IsAudioDeviceReady()) return 0;

        // A mod that never stops its sounds must not grow the table forever.
        modSoundsCollect();
        if (g_modSounds.size() >= 64) return 0;

        ModSound entry;
        entry.owner = owner;
        entry.wave  = LoadWaveFromMemory(ext.c_str(), bytes.data(), (int)bytes.size());
        if (!entry.wave.data) return 0;

        entry.sound = LoadSoundFromWave(entry.wave);
        if (!entry.sound.frameCount) {
            UnloadWave(entry.wave);
            return 0;
        }

        // Through the player's sfx slider, not around it: a mod is not entitled
        // to be louder than the game the player turned down.
        Audio& a = Audio::get();
        SetSoundVolume(entry.sound, volume * a.sfxVolume() * a.masterVolume());
        PlaySound(entry.sound);

        const uint32_t handle = g_nextModSound++;
        g_modSounds[handle] = entry;
        return handle;
    };

    audio.stop = [](const std::string& owner, uint32_t handle) {
        auto it = g_modSounds.find(handle);
        if (it == g_modSounds.end() || it->second.owner != owner) return;
        StopSound(it->second.sound);
        UnloadSound(it->second.sound);
        UnloadWave(it->second.wave);
        g_modSounds.erase(it);
    };

    audio.setVolume = [](const std::string& owner, uint32_t handle, float volume) {
        auto it = g_modSounds.find(handle);
        if (it == g_modSounds.end() || it->second.owner != owner) return;
        Audio& a = Audio::get();
        SetSoundVolume(it->second.sound, volume * a.sfxVolume() * a.masterVolume());
    };

    audio.isPlaying = [](const std::string& owner, uint32_t handle) -> bool {
        auto it = g_modSounds.find(handle);
        if (it == g_modSounds.end() || it->second.owner != owner) return false;
        return IsSoundPlaying(it->second.sound);
    };

    audio.stopAll = [this](const std::string& owner) { unloadModSounds(owner); };

    modSetAudioBridge(audio);

    ModNetBridge net;

    net.send = [this](const std::string& modId, int32_t peer,
                      const std::vector<uint8_t>& payload) -> bool {
        if (m_netHost)    { m_netHost->sendModMessage(modId, peer, payload);    return true; }
        if (m_netSession) { m_netSession->sendModMessage(modId, peer, payload); return true; }
        return false;     // singleplayer: there is nobody to send to
    };

    net.recv = [this](const std::string& modId, std::vector<uint8_t>& out,
                      int32_t& from) -> bool {
        // Only this mod's messages, and in order. A mod cannot read another's,
        // which is why the queue is scanned rather than simply popped.
        for (auto it = m_mpModInbox.begin(); it != m_mpModInbox.end(); ++it) {
            if (it->modId != modId) continue;
            out.assign(it->payload.begin(), it->payload.end());
            from = (int32_t)it->peerId;
            m_mpModInbox.erase(it);
            return true;
        }
        return false;
    };

    net.peerCount = [this]() -> uint32_t {
        if (m_netHost)    return (uint32_t)m_netHost->lobby().roster().size();
        if (m_netSession) return (uint32_t)m_netSession->roster().size();
        return 0;
    };

    net.selfPeer = [this]() -> uint32_t {
        if (m_netHost)    return m_netHost->lobby().hostPeerId();
        if (m_netSession) return m_netSession->welcome().peerId;
        return 0;
    };

    net.isHost = [this]() { return m_netHost != nullptr; };

    modSetNetBridge(net);

    // ── UI ── the three things the mod host cannot do without raylib ──────────
    ModUiBridge ui;

    // Measured with drawHybridText's own font, so a mod centring a string gets
    // the width the game will actually draw and not an estimate.
    ui.measureText = [](const std::string& t, int size) -> uint32_t {
        return (uint32_t)MeasureText(t.c_str(), size);
    };

    // 0x00RRGGBB. Reports the PLAYER's colour, not another mod's override, so a
    // mod building a palette around it harmonises with what the player chose
    // rather than with whatever mod happened to load first.
    ui.themeAccent = [this]() -> uint32_t {
        return (uint32_t)(m_config.accentColor & 0x00FFFFFF);
    };

    // Into accentOverride, which Config::save() does not write. See the note
    // there: the accent is read at over a hundred sites, so this restyles the
    // whole interface, and it must not be able to outlive the mod that set it.
    ui.setThemeAccent = [this](uint32_t rgb) {
        m_config.accentOverride = (int)(rgb & 0x00FFFFFF);
    };

    modSetUiBridge(ui);
}

// ------------------------------------------------------------- setup ------

void Game::initModSystem() {
    if (!g_access) g_access = new GameModAccess(this);
    g_modGame = g_access;
    g_modHost.game = this;
    g_modHost.headless = m_aiTraining;
    g_modHost.screenW = (uint32_t)m_screenW;
    g_modHost.screenH = (uint32_t)m_screenH;

    syncModNetContext();

    // Headless training never opens the mod menu, which is the only load path,
    // so it never scans for mods either.
    if (m_aiTraining) return;
    installModBridges();
    ModManager::get().init(m_dataDir + "mods", m_dataDir + "mods.json");
}

void Game::clearModThumbnails() {
    for (auto& kv : m_modThumbs)
        if (kv.second.id) UnloadTexture(kv.second);
    m_modThumbs.clear();
}

// ------------------------------------------------------- mod panels -------

void Game::drawModPanels() {
    if (m_aiTraining) return;
    ModUI& ui = ModUI::get();
    if (ui.panels().empty()) return;

    g_modHost.screenW = (uint32_t)m_screenW;
    g_modHost.screenH = (uint32_t)m_screenH;

    // Panels live in a column down the right edge. The host owns the geometry;
    // the mod only ever sees its own width and height.
    const float panelW = 300.0f;
    float x = (float)m_screenW - panelW - 16.0f;
    float y = 110.0f;

    Vector2 mouse = getMouse();
    bool clicked = IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    ui.clearCommands();
    for (auto& p : ui.panels()) {
        if (!p.visible) continue;
        float h = (float)std::max(60u, p.minH);
        if (y + h > (float)m_screenH - 16.0f) { p.visible = false; continue; }
        p.x = x; p.y = y; p.w = panelW; p.h = h;

        p.mouseInside = mouse.x >= x && mouse.x < x + panelW &&
                        mouse.y >= y && mouse.y < y + h;
        p.mouseX = mouse.x - x;
        p.mouseY = mouse.y - y;
        p.clickPending = p.mouseInside && clicked;

        y += h + 10.0f;
    }

    syncModNetContext();

    // A mod's accent override lasts exactly as long as a mod is running. Nothing
    // else takes it back: the mod that set it may have been disabled, replaced
    // or failed to load since, and none of those paths knows about Config.
    // Checked here because this runs every frame the mod system is live.
    if (m_config.accentOverride >= 0 && !ModManager::get().anyActive())
        m_config.accentOverride = -1;

    // Mods emit their draw commands here.
    ModManager::get().drawPanels();

    Color accent = hexToColor(m_config.accent());
    for (auto& p : ui.panels()) {
        if (!p.visible) continue;
        Rectangle r{p.x, p.y, p.w, p.h};
        DrawRectangleRounded(r, 0.06f, 8, Color{15, 15, 25, 230});
        DrawRectangleRoundedLines(r, 0.06f, 8, Color{60, 60, 90, 200});
        DrawText(p.title.c_str(), (int)p.x + 10, (int)p.y + 6, 14, accent);

        // Everything the mod drew is clipped to its own rectangle. Coordinates
        // are translated here, so a negative or huge coordinate from a mod
        // cannot land outside this box.
        int cy = (int)p.y + 26;
        BeginScissorMode((int)p.x + 2, cy, (int)p.w - 4, (int)(p.h - 28));
        for (const auto& c : p.cmds) {
            float cx = p.x + (float)c.x;
            float cyy = (float)cy + (float)c.y;
            if (c.kind == ModDrawCmd::Rect) {
                DrawRectangle((int)cx, (int)cyy, c.w, c.h, colorFromRGBA(c.rgba));
            } else if (c.kind == ModDrawCmd::Text) {
                drawHybridText((int)cx, (int)cyy, c.fontSize, c.text.c_str(),
                               colorFromRGBA(c.rgba));
            } else if (c.kind == ModDrawCmd::Line) {
                DrawLineEx({cx, cyy}, {p.x + (float)c.x2, (float)cy + (float)c.y2},
                           c.thickness, colorFromRGBA(c.rgba));
            } else if (c.kind == ModDrawCmd::Circle) {
                DrawCircle((int)cx, (int)cyy, c.radius, colorFromRGBA(c.rgba));
            } else if (c.kind == ModDrawCmd::Image) {
                const Texture2D* t = modImage(p.ownerId, c.text);
                if (t) {
                    Rectangle src{0, 0, (float)t->width, (float)t->height};
                    Rectangle dst{cx, cyy,
                                  c.w > 0 ? (float)c.w : (float)t->width,
                                  c.h > 0 ? (float)c.h : (float)t->height};
                    DrawTexturePro(*t, src, dst, {0, 0}, 0.0f, colorFromRGBA(c.rgba));
                }
            } else {
                Rectangle b{cx, cyy, (float)c.w, (float)c.h};
                DrawRectangleRounded(b, 0.2f, 6,
                                     c.hovered ? Color{70, 80, 110, 255}
                                               : Color{40, 45, 65, 255});
                DrawRectangleRoundedLines(b, 0.2f, 6, Color{90, 95, 125, 255});
                int tw = MeasureText(c.text.c_str(), 13);
                drawHybridText((int)(cx + (c.w - tw) / 2), (int)(cyy + (c.h - 13) / 2),
                               13, c.text.c_str(), WHITE);
            }
        }
        EndScissorMode();
    }
}

// --------------------------------------------------------- mod menu ------

void Game::drawModsMenu() {
    drawMenuBackground(true);
    Color accent = hexToColor(m_config.accent());
    ModManager& mm = ModManager::get();
    auto& mods = mm.mods();
    Vector2 mouse = getMouse();

    const char* title = "Mods";
    DrawText(title, m_screenW / 2 - MeasureText(title, 48) / 2, 40, 48, accent);

    // Runtime line: says plainly when this build cannot run mods at all.
    {
        std::string line = std::string("Runtime: ") + ModRuntime::get().backendName();
        Color c = ModRuntime::get().available() ? Color{150, 150, 165, 255}
                                                : Color{220, 120, 100, 255};
        if (!ModRuntime::get().available())
            line += "  —  mods cannot run in this build";
        DrawText(line.c_str(), m_screenW / 2 - MeasureText(line.c_str(), 16) / 2, 96, 16, c);
    }

    const int startY = 130;
    const int itemH = 92;
    const int bottomBar = 70;
    int maxVisible = std::max(1, (m_screenH - startY - bottomBar) / itemH);
    int maxScroll = std::max(0, (int)mods.size() - maxVisible);
    m_modScroll = std::clamp(m_modScroll, 0, maxScroll);

    if (mods.empty()) {
        const char* none = "No mods installed";
        DrawText(none, m_screenW / 2 - MeasureText(none, 22) / 2, startY + 40, 22,
                 Color{140, 140, 150, 255});
        const char* hint = "Drag a .odmod file onto this window, or use Add from computer";
        DrawText(hint, m_screenW / 2 - MeasureText(hint, 15) / 2, startY + 76, 15,
                 Color{110, 110, 120, 255});
    }

    for (int i = 0; i < (int)mods.size(); ++i) {
        int y = startY + (i - m_modScroll) * itemH;
        if (y + itemH < 0 || y > m_screenH - bottomBar) continue;
        ModEntry& e = mods[i];

        bool selected = (i == m_modIndex);
        Rectangle row{40.0f, (float)y, (float)m_screenW - 80.0f, (float)itemH - 8};
        bool hovered = CheckCollisionPointRec(mouse, row);

        Color bg{255, 255, 255, 8};
        if (e.state == ModState::PendingReload) bg = ColorAlpha(accent, 0.16f);
        else if (e.state == ModState::Failed)   bg = Color{120, 40, 40, 60};
        else if (selected)                      bg = Color{60, 70, 100, 60};
        else if (hovered)                       bg = Color{255, 255, 255, 18};
        DrawRectangleRounded(row, 0.08f, 8, bg);
        if (e.state == ModState::PendingReload)
            DrawRectangleRoundedLines(row, 0.08f, 8, accent);

        // Thumbnail, decoded lazily and cached until the list is rescanned.
        float tx = row.x + 10, ty = row.y + 10;
        float thumb = (float)itemH - 28;
        if (!e.thumbnailPng.empty()) {
            auto it = m_modThumbs.find(e.id);
            if (it == m_modThumbs.end()) {
                Image img = LoadImageFromMemory(".png", e.thumbnailPng.data(),
                                                (int)e.thumbnailPng.size());
                Texture2D tex{};
                if (img.data) { tex = LoadTextureFromImage(img); UnloadImage(img); }
                it = m_modThumbs.emplace(e.id, tex).first;
            }
            if (it->second.id)
                DrawTexturePro(it->second,
                               Rectangle{0, 0, (float)it->second.width, (float)it->second.height},
                               Rectangle{tx, ty, thumb, thumb}, Vector2{0, 0}, 0.0f, WHITE);
            else
                DrawRectangleRounded({tx, ty, thumb, thumb}, 0.1f, 6, Color{40, 40, 55, 255});
        } else {
            DrawRectangleRounded({tx, ty, thumb, thumb}, 0.1f, 6, Color{40, 40, 55, 255});
        }

        float textX = tx + thumb + 14;
        std::string name = e.manifestValid ? e.manifest.name : e.fileName;
        drawHybridText((int)textX, (int)row.y + 10, 22, name.c_str(),
                       e.state == ModState::Failed ? Color{230, 150, 140, 255} : WHITE);

        std::string sub;
        if (e.manifestValid) {
            sub = "v" + e.manifest.version;
            if (!e.manifest.authors.empty()) sub += "  ·  " + e.manifest.authors[0];
            sub += "  ·  " + modModuleMaskToString(e.grants);
        } else {
            sub = e.fileName;
        }
        drawHybridText((int)textX, (int)row.y + 38, 13, sub.c_str(), Color{155, 155, 168, 255});

        std::string status = modStateName(e.state);
        if (e.state == ModState::PendingReload)
            status = "Needs reload to take effect";
        if (!e.diagnostic.empty()) status += " — " + e.diagnostic;
        else if (!e.warnings.empty()) status += " — " + e.warnings[0];
        Color sc = Color{130, 200, 140, 255};
        if (e.state == ModState::PendingReload) sc = accent;
        else if (e.state == ModState::Failed)   sc = Color{225, 110, 100, 255};
        else if (e.state == ModState::Disabled) sc = Color{130, 130, 140, 255};
        if (status.size() > 96) status.resize(96);
        drawHybridText((int)textX, (int)row.y + 58, 13, status.c_str(), sc);

        // Badges, left to right after the status line. A conflict the host
        // OBSERVED is shown even while the mod runs: it is not grounds to
        // disable a mod mid-game, but the player should still be told which two
        // mods are fighting and over what.
        int badgeX = (int)textX + MeasureText(status.c_str(), 13) + 12;
        auto badge = [&](const char* label, Color fg, Color bg) {
            int w = MeasureText(label, 12) + 14;
            Rectangle b{(float)badgeX, (float)row.y + 55, (float)w, 19.0f};
            DrawRectangleRounded(b, 0.4f, 6, bg);
            DrawText(label, badgeX + 7, (int)row.y + 58, 12, fg);
            badgeX += w + 6;
            return b;
        };

        if (e.manifestValid && ModConflicts::get().anyFor(e.id))
            badge("conflict", Color{255, 200, 130, 255}, Color{90, 60, 25, 220});

        // "Can be updated" only ever appears when the player opted in AND the
        // author's page reported a genuinely higher version.
        if (e.manifestValid) {
            if (const auto* up = ModUpdates::get().infoFor(e.id)) {
                if (up->newer) {
                    Rectangle b = badge("Can be updated",
                                        Color{140, 220, 255, 255},
                                        Color{25, 60, 90, 230});
                    if (CheckCollisionPointRec(mouse, b)) {
                        DrawRectangleRoundedLines(b, 0.4f, 6, Color{140, 220, 255, 200});
                        // The click is handled in updateModsMenu; drawing only
                        // marks it. Opening a browser is as far as this goes --
                        // the game never downloads or installs a mod.
                    }
                }
            }
        }

        // Right-anchored controls, laid out right to left.
        const int btn = 30;
        int bx = (int)(row.x + row.width) - 12 - btn;
        int by = (int)row.y + (itemH - 8 - btn) / 2;
        auto drawBtn = [&](const char* glyph, Color tint) {
            Rectangle b{(float)bx, (float)by, (float)btn, (float)btn};
            bool bh = CheckCollisionPointRec(mouse, b);
            DrawRectangleRounded(b, 0.25f, 6, bh ? Color{70, 75, 100, 255}
                                                 : Color{35, 38, 55, 220});
            int gw = MeasureText(glyph, 15);
            DrawText(glyph, bx + (btn - gw) / 2, by + (btn - 15) / 2, 15, tint);
            bx -= btn + 8;
        };
        drawBtn("X", Color{225, 120, 110, 255});      // delete
        drawBtn("R", Color{200, 200, 210, 255});      // reload this mod
        drawBtn("A", Color{200, 200, 210, 255});      // advanced
        // Enable toggle
        {
            Rectangle b{(float)bx - 34, (float)by, 64.0f, (float)btn};
            bool bh = CheckCollisionPointRec(mouse, b);
            bool on = e.enabled;
            DrawRectangleRounded(b, 0.35f, 8,
                                 on ? ColorAlpha(accent, bh ? 0.9f : 0.7f)
                                    : (bh ? Color{70, 75, 100, 255} : Color{35, 38, 55, 220}));
            const char* t = on ? "ON" : "OFF";
            int tw = MeasureText(t, 14);
            DrawText(t, (int)b.x + (int)(b.width - tw) / 2, (int)b.y + 8, 14,
                     on ? Color{20, 20, 25, 255} : Color{190, 190, 200, 255});
        }
    }

    // Bottom bar
    int by = m_screenH - 52;
    auto bar = [&](const char* label, int x, int w, Color tint) {
        Rectangle b{(float)x, (float)by, (float)w, 36.0f};
        bool bh = CheckCollisionPointRec(mouse, b);
        DrawRectangleRounded(b, 0.25f, 8, bh ? Color{70, 75, 100, 255} : Color{30, 33, 48, 220});
        DrawRectangleRoundedLines(b, 0.25f, 8, Color{70, 70, 100, 180});
        int tw = MeasureText(label, 16);
        DrawText(label, x + (w - tw) / 2, by + 10, 16, tint);
    };
    bar("Add from computer", 40, 200, WHITE);
    bar("Reload modloader", 250, 190, WHITE);
    bar("Back", m_screenW - 140, 100, WHITE);

    if (m_modFeedbackTimer > 0.0f) {
        int tw = MeasureText(m_modFeedback.c_str(), 16);
        DrawText(m_modFeedback.c_str(), m_screenW / 2 - tw / 2, by - 28, 16,
                 ColorAlpha(accent, std::min(1.0f, m_modFeedbackTimer)));
    }

    if (m_modAdvancedFor >= 0) drawModAdvanced();
    if (m_modDeleteFor >= 0)   drawModDeleteConfirm();
    if (m_modAiWarnFor >= 0)   drawModAiWarning();
    if (m_modReloading)        drawModReloadingOverlay();
}

// Permissions rows, laid out once for both the draw and the hit test.
//
// TWO COLUMNS BECAUSE THERE ARE TWENTY-TWO OF THEM. One column at 34px was fine
// for the ten this panel used to hardcode; Gearbox 1.1 took the list to
// twenty-two, which is 748px of rows in a 600px box -- the last capabilities
// would have drawn straight through the conflicts section and off the bottom.
//
// Returns the toggle's rectangle for row `i`. The label sits to its left, and
// the two columns split the panel down the middle.
struct PermLayout {
    int x, y, w;
    static constexpr int kRowH = 30;
    static constexpr int kTop = 80;
    int rows() const { return ((int)modAllModuleBits().size() + 1) / 2; }
    int colW() const { return (w - 40) / 2; }
    Rectangle toggle(int i) const {
        const int col = i / rows(), row = i % rows();
        const int cx = x + 20 + col * colW();
        return {(float)(cx + colW() - 78), (float)(y + kTop + row * kRowH), 60.0f, 24.0f};
    }
    int labelX(int i) const { return x + 24 + (i / rows()) * colW(); }
    int height() const { return kTop + rows() * kRowH; }
};

void Game::drawModAdvanced() {
    auto& mods = ModManager::get().mods();
    if (m_modAdvancedFor >= (int)mods.size()) { m_modAdvancedFor = -1; return; }
    ModEntry& e = mods[m_modAdvancedFor];
    Color accent = hexToColor(m_config.accent());
    Vector2 mouse = getMouse();

    DrawRectangle(0, 0, m_screenW, m_screenH, Color{0, 0, 0, 170});
    // Taller than it was: the conflicts section lives below the permissions.
    int w = 520, h = 600;
    int x = m_screenW / 2 - w / 2, y = m_screenH / 2 - h / 2;
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.05f, 10,
                         Color{20, 20, 32, 245});
    DrawRectangleRoundedLines({(float)x, (float)y, (float)w, (float)h}, 0.05f, 10,
                              Color{70, 70, 100, 220});

    std::string t = "Permissions — " + (e.manifestValid ? e.manifest.name : e.fileName);
    drawHybridText(x + 24, y + 20, 22, t.c_str(), accent);
    DrawText("Only what the mod requested can be granted.", x + 24, y + 50, 13,
             Color{150, 150, 160, 255});

    // Every capability the build knows about, so one added to kModules cannot
    // end up granted-but-unrevocable because an array here was not updated.
    const PermLayout pl{x, y, w};
    const auto& bits = modAllModuleBits();
    for (int i = 0; i < (int)bits.size(); i++) {
        const uint32_t bit = bits[(size_t)i];
        bool requested = (e.manifest.modules & bit) != 0;
        bool granted = (e.grants & bit) != 0 || bit == MODULE_CORE;
        std::string label = modModuleMaskToString(bit);

        Rectangle b = pl.toggle(i);
        Color labelCol = requested ? WHITE : Color{95, 95, 105, 255};
        drawHybridText(pl.labelX(i), (int)b.y + 5, 14, label.c_str(), labelCol);

        bool locked = !requested || bit == MODULE_CORE;
        bool bh = !locked && CheckCollisionPointRec(mouse, b);
        const char* st = locked ? (requested ? "ALWAYS" : "—") : (granted ? "ON" : "OFF");
        Color bc = locked ? Color{40, 42, 55, 200}
                          : (granted ? ColorAlpha(accent, bh ? 0.9f : 0.7f)
                                     : (bh ? Color{70, 75, 100, 255} : Color{35, 38, 55, 220}));
        DrawRectangleRounded(b, 0.35f, 8, bc);
        int tw = MeasureText(st, 12);
        DrawText(st, (int)b.x + (int)(b.width - tw) / 2, (int)b.y + 6, 12,
                 (granted && !locked) ? Color{20, 20, 25, 255} : Color{180, 180, 190, 255});
    }
    int ry = y + pl.height();

    // --- conflicts ------------------------------------------------------------
    // Listed per PAIR, because that is what a conflict is and what an override
    // applies to. Declared conflicts (the author knew) and observed ones (the
    // host watched two mods fight over the same value) are shown together --
    // the user cares that these two disagree, not how we found out.
    ry += 6;
    DrawLine(x + 24, ry, x + w - 24, ry, Color{60, 60, 80, 200});
    ry += 12;
    drawHybridText(x + 28, ry, 16, "Conflicts", accent);
    ry += 26;

    int shown = 0;
    if (e.manifestValid) {
        auto& mmRef = ModManager::get();
        for (auto& other : mmRef.mods()) {
            if (!other.manifestValid || other.id == e.id) continue;

            bool declared = false;
            for (const auto& c : e.manifest.conflicts)
                if (c.id == other.id) declared = true;
            for (const auto& c : other.manifest.conflicts)
                if (c.id == e.id) declared = true;

            bool observed = false;
            for (const auto& cl : ModConflicts::get().clashes())
                if ((cl.modA == e.id && cl.modB == other.id) ||
                    (cl.modB == e.id && cl.modA == other.id)) observed = true;

            if (!declared && !observed) continue;
            if (shown >= 3) break;              // the panel is not a log viewer

            bool bridgedPair = mmRef.bridged(e.id, other.id);
            bool overridden = e.overridesConflictWith(other.id) ||
                              other.overridesConflictWith(e.id);

            std::string label = other.manifest.name;
            if (label.size() > 26) label.resize(26);
            label += declared ? "  (declared)" : "  (observed)";
            drawHybridText(x + 28, ry + 5, 14, label.c_str(),
                           bridgedPair || overridden ? Color{150, 150, 160, 255}
                                                     : Color{255, 200, 130, 255});

            Rectangle b{(float)(x + w - 150), (float)ry, 112.0f, 24.0f};
            const char* st = bridgedPair ? "BRIDGED"
                                         : (overridden ? "ALLOWED" : "BLOCKED");
            bool bh = !bridgedPair && CheckCollisionPointRec(mouse, b);
            DrawRectangleRounded(b, 0.35f, 8,
                bridgedPair ? Color{40, 42, 55, 200}
                            : (overridden ? ColorAlpha(accent, bh ? 0.9f : 0.7f)
                                          : (bh ? Color{70, 75, 100, 255}
                                                : Color{35, 38, 55, 220})));
            int tw2 = MeasureText(st, 12);
            DrawText(st, (int)b.x + (int)(b.width - tw2) / 2, (int)b.y + 6, 12,
                     (overridden && !bridgedPair) ? Color{20, 20, 25, 255}
                                                  : Color{180, 180, 190, 255});
            ry += 30;
            shown++;
        }
    }
    if (shown == 0) {
        DrawText("None with the mods you have enabled.", x + 28, ry, 13,
                 Color{130, 130, 140, 255});
    }

    const char* note = "Changing permissions requires a reload.";
    DrawText(note, x + 24, y + h - 66, 13, Color{150, 150, 160, 255});

    Rectangle close{(float)(x + w - 120), (float)(y + h - 46), 96.0f, 32.0f};
    bool ch = CheckCollisionPointRec(mouse, close);
    DrawRectangleRounded(close, 0.25f, 8, ch ? Color{70, 75, 100, 255} : Color{35, 38, 55, 220});
    DrawText("Close", (int)close.x + 24, (int)close.y + 8, 16, WHITE);
}

void Game::drawModDeleteConfirm() {
    auto& mods = ModManager::get().mods();
    if (m_modDeleteFor >= (int)mods.size()) { m_modDeleteFor = -1; return; }
    ModEntry& e = mods[m_modDeleteFor];
    Vector2 mouse = getMouse();

    DrawRectangle(0, 0, m_screenW, m_screenH, Color{0, 0, 0, 170});
    int w = 460, h = 180;
    int x = m_screenW / 2 - w / 2, y = m_screenH / 2 - h / 2;
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.06f, 10,
                         Color{20, 20, 32, 245});
    DrawRectangleRoundedLines({(float)x, (float)y, (float)w, (float)h}, 0.06f, 10,
                              Color{70, 70, 100, 220});
    DrawText("Delete this mod?", x + 24, y + 22, 22, WHITE);
    drawHybridText(x + 24, y + 58, 15,
                   (e.manifestValid ? e.manifest.name : e.fileName).c_str(),
                   Color{190, 190, 200, 255});
    DrawText("The .odmod file is removed from disk.", x + 24, y + 82, 13,
             Color{150, 150, 160, 255});

    Rectangle del{(float)(x + w - 220), (float)(y + h - 52), 96.0f, 34.0f};
    Rectangle can{(float)(x + w - 114), (float)(y + h - 52), 96.0f, 34.0f};
    DrawRectangleRounded(del, 0.25f, 8,
                         CheckCollisionPointRec(mouse, del) ? Color{200, 60, 60, 255}
                                                            : Color{150, 45, 45, 230});
    DrawRectangleRounded(can, 0.25f, 8,
                         CheckCollisionPointRec(mouse, can) ? Color{70, 75, 100, 255}
                                                            : Color{35, 38, 55, 220});
    DrawText("Delete", (int)del.x + 20, (int)del.y + 9, 16, WHITE);
    DrawText("Cancel", (int)can.x + 20, (int)can.y + 9, 16, WHITE);
}

void Game::drawModAiWarning() {
    Vector2 mouse = getMouse();
    DrawRectangle(0, 0, m_screenW, m_screenH, Color{0, 0, 0, 180});
    int w = 560, h = 230;
    int x = m_screenW / 2 - w / 2, y = m_screenH / 2 - h / 2;
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.06f, 10,
                         Color{26, 20, 20, 248});
    DrawRectangleRoundedLines({(float)x, (float)y, (float)w, (float)h}, 0.06f, 10,
                              Color{170, 90, 80, 220});

    DrawText("AI Learning is on", x + 24, y + 22, 24, Color{240, 170, 150, 255});
    DrawText("Mods can change what the AI sees and how turns resolve.", x + 24, y + 62, 15,
             Color{205, 205, 215, 255});
    DrawText("Training with a mod loaded silently corrupts the model in", x + 24, y + 84, 15,
             Color{205, 205, 215, 255});
    std::string f = "data/ai/model.bin — and the damage is invisible until it is done.";
    drawHybridText(x + 24, y + 106, 15, f.c_str(), Color{205, 205, 215, 255});

    Rectangle off{(float)(x + 24), (float)(y + h - 58), 250.0f, 36.0f};
    Rectangle can{(float)(x + w - 130), (float)(y + h - 58), 106.0f, 36.0f};
    DrawRectangleRounded(off, 0.25f, 8,
                         CheckCollisionPointRec(mouse, off) ? Color{70, 90, 70, 255}
                                                            : Color{40, 60, 45, 230});
    DrawRectangleRounded(can, 0.25f, 8,
                         CheckCollisionPointRec(mouse, can) ? Color{70, 75, 100, 255}
                                                            : Color{35, 38, 55, 220});
    DrawText("Turn off AI Learning and enable", (int)off.x + 14, (int)off.y + 10, 15, WHITE);
    DrawText("Cancel", (int)can.x + 24, (int)can.y + 10, 16, WHITE);
}

void Game::drawModReloadingOverlay() {
    DrawRectangle(0, 0, m_screenW, m_screenH, Color{0, 0, 0, 200});
    const char* t = "Reloading mods...";
    DrawText(t, m_screenW / 2 - MeasureText(t, 28) / 2, m_screenH / 2 - 14, 28,
             hexToColor(m_config.accent()));
}

// ------------------------------------------------------- mod menu input ---

void Game::updateModsMenu() {
    // One round of update checks per visit to this menu, and only ever with the
    // player's opt-in. checkAll() is a no-op when the setting is off and when a
    // round is already in flight, so this cannot turn into per-frame traffic.
    if (!m_modUpdatesAsked) {
        m_modUpdatesAsked = true;
        ModUpdates::get().checkAll(ModManager::get(), m_config.modUpdateChecks);
    }

    ModManager& mm = ModManager::get();
    // A world being loaded is what makes activation deferred: joining a game
    // already in progress would make turn order depend on when the user
    // happened to click.
    mm.setInGame(m_renderer != nullptr && m_provinces.getImage().data != nullptr);

    // mod_load runs from this screen, and it is the first thing a mod calls
    // gearbox_env from, so the reported screen size has to be current here --
    // initModSystem runs before the window size is known.
    g_modHost.screenW = (uint32_t)m_screenW;
    g_modHost.screenH = (uint32_t)m_screenH;

    auto& mods = mm.mods();
    Vector2 mouse = getMouse();
    bool click = IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    if (m_modFeedbackTimer > 0.0f) m_modFeedbackTimer -= GetFrameTime();

    // A reload draws one frame of its overlay before doing the work, so the
    // user sees the loading screen rather than a frozen frame.
    if (m_modReloading) {
        if (m_modReloadFrames++ > 0) {
            syncModNetContext();   // setNetContext must precede reloadAll
            mm.reloadAll();
            clearModThumbnails();
            m_modReloading = false;
            m_modFeedback = "Modloader reloaded";
            m_modFeedbackTimer = 2.0f;
        }
        return;
    }

    // Drag-and-drop is the portable import path, and the only one on web.
    if (IsFileDropped()) {
        FilePathList dropped = LoadDroppedFiles();
        for (unsigned i = 0; i < dropped.count; ++i) {
            std::string p = dropped.paths[i];
            if (p.size() < 7 || p.compare(p.size() - 6, 6, ".odmod") != 0) continue;
            std::string err;
            if (mm.importFile(p, err)) {
                clearModThumbnails();
                m_modFeedback = "Added " + p.substr(p.find_last_of('/') + 1);
            } else {
                m_modFeedback = "Rejected: " + err;
            }
            m_modFeedbackTimer = 4.0f;
        }
        UnloadDroppedFiles(dropped);
    }

    // --- modal overlays swallow everything beneath them -----------------------
    if (m_modAiWarnFor >= 0) {
        int w = 560, h = 230;
        int x = m_screenW / 2 - w / 2, y = m_screenH / 2 - h / 2;
        Rectangle off{(float)(x + 24), (float)(y + h - 58), 250.0f, 36.0f};
        Rectangle can{(float)(x + w - 130), (float)(y + h - 58), 106.0f, 36.0f};
        if (click && CheckCollisionPointRec(mouse, off)) {
            Audio::get().playSfx("toggle_off");
            m_config.aiLearning = false;
            m_config.save(m_configPath);
            mm.setEnabled((size_t)m_modAiWarnFor, true);
            m_modAiWarnFor = -1;
        } else if ((click && CheckCollisionPointRec(mouse, can)) || IsKeyPressed(KEY_ESCAPE)) {
            Audio::get().playSfx("back");
            m_modAiWarnFor = -1;
        }
        return;
    }

    if (m_modDeleteFor >= 0) {
        int w = 460, h = 180;
        int x = m_screenW / 2 - w / 2, y = m_screenH / 2 - h / 2;
        Rectangle del{(float)(x + w - 220), (float)(y + h - 52), 96.0f, 34.0f};
        Rectangle can{(float)(x + w - 114), (float)(y + h - 52), 96.0f, 34.0f};
        if (click && CheckCollisionPointRec(mouse, del)) {
            Audio::get().playSfx("confirm");
            std::string err;
            if (!mm.removeMod((size_t)m_modDeleteFor, err)) {
                m_modFeedback = err;
                m_modFeedbackTimer = 3.0f;
            } else {
                clearModThumbnails();
            }
            m_modDeleteFor = -1;
            m_modIndex = std::clamp(m_modIndex, 0, std::max(0, (int)mods.size() - 1));
        } else if ((click && CheckCollisionPointRec(mouse, can)) || IsKeyPressed(KEY_ESCAPE)) {
            Audio::get().playSfx("back");
            m_modDeleteFor = -1;
        }
        return;
    }

    if (m_modAdvancedFor >= 0) {
        if (m_modAdvancedFor >= (int)mods.size()) { m_modAdvancedFor = -1; return; }
        ModEntry& e = mods[m_modAdvancedFor];
        int w = 520, h = 600;              // must match drawModAdvanced
        int x = m_screenW / 2 - w / 2, y = m_screenH / 2 - h / 2;

        // The same layout the draw uses, so the hit boxes cannot drift from it.
        const PermLayout pl{x, y, w};
        const auto& bits = modAllModuleBits();
        for (int i = 0; i < (int)bits.size(); i++) {
            const uint32_t bit = bits[(size_t)i];
            bool locked = !(e.manifest.modules & bit) || bit == MODULE_CORE;
            if (!locked && click && CheckCollisionPointRec(mouse, pl.toggle(i)) &&
                (Audio::get().playSfx("click_light"), true))
                mm.setGrant((size_t)m_modAdvancedFor, bit, !(e.grants & bit));
        }
        int ry = y + pl.height();

        // Conflict overrides. The same walk as the draw, so the hit boxes line
        // up; a bridged pair has no toggle because nothing needs deciding.
        ry += 6 + 12 + 26;
        int shown = 0;
        if (e.manifestValid) {
            for (auto& other : mm.mods()) {
                if (!other.manifestValid || other.id == e.id) continue;

                bool declared = false;
                for (const auto& c : e.manifest.conflicts)
                    if (c.id == other.id) declared = true;
                for (const auto& c : other.manifest.conflicts)
                    if (c.id == e.id) declared = true;

                bool observed = false;
                for (const auto& cl : ModConflicts::get().clashes())
                    if ((cl.modA == e.id && cl.modB == other.id) ||
                        (cl.modB == e.id && cl.modA == other.id)) observed = true;

                if (!declared && !observed) continue;
                if (shown >= 3) break;

                if (!mm.bridged(e.id, other.id)) {
                    Rectangle b{(float)(x + w - 150), (float)ry, 112.0f, 24.0f};
                    if (click && CheckCollisionPointRec(mouse, b)) {
                        Audio::get().playSfx("click_light");
                        bool on = e.overridesConflictWith(other.id);
                        mm.setConflictOverride((size_t)m_modAdvancedFor, other.id, !on);
                        m_modFeedback = on ? "Conflict blocked again"
                                           : "Allowed — reload for it to take effect";
                        m_modFeedbackTimer = 3.0f;
                        return;
                    }
                }
                ry += 30;
                shown++;
            }
        }
        Rectangle close{(float)(x + w - 120), (float)(y + h - 46), 96.0f, 32.0f};
        if (((click && CheckCollisionPointRec(mouse, close)) || IsKeyPressed(KEY_ESCAPE)) &&
            (Audio::get().playSfx("back"), true))
            m_modAdvancedFor = -1;
        return;
    }

    // --- list navigation ------------------------------------------------------
    const int startY = 130, itemH = 92, bottomBar = 70;
    int maxVisible = std::max(1, (m_screenH - startY - bottomBar) / itemH);
    int maxScroll = std::max(0, (int)mods.size() - maxVisible);

    if (IsKeyPressed(KEY_ESCAPE)) { m_currentScreen = SCREEN_MENU; return; }
    if (IsKeyPressed(KEY_DOWN) && !mods.empty())
        m_modIndex = std::min(m_modIndex + 1, (int)mods.size() - 1);
    if (IsKeyPressed(KEY_UP)) m_modIndex = std::max(m_modIndex - 1, 0);
    if (m_modIndex < m_modScroll) m_modScroll = m_modIndex;
    if (m_modIndex >= m_modScroll + maxVisible) m_modScroll = m_modIndex - maxVisible + 1;
    m_modScroll = std::clamp(m_modScroll - (int)GetMouseWheelMove(), 0, maxScroll);

    // --- bottom bar -----------------------------------------------------------
    int by = m_screenH - 52;
    Rectangle addB{40.0f, (float)by, 200.0f, 36.0f};
    Rectangle relB{250.0f, (float)by, 190.0f, 36.0f};
    Rectangle backB{(float)(m_screenW - 140), (float)by, 100.0f, 36.0f};

    if (click && CheckCollisionPointRec(mouse, backB)) { Audio::get().playSfx("back"); m_currentScreen = SCREEN_MENU; return; }
    if (click && CheckCollisionPointRec(mouse, relB)) {
        Audio::get().playSfx("click_light");
        m_modReloading = true;
        m_modReloadFrames = 0;
        return;
    }
    if (click && CheckCollisionPointRec(mouse, addB)) {
        Audio::get().playSfx("click_light");
        std::string p = pickOdmodFile();
        if (p.empty()) {
            m_modFeedback = "Drag a .odmod file onto the window to add it";
            m_modFeedbackTimer = 3.0f;
        } else {
            std::string err;
            if (mm.importFile(p, err)) {
                clearModThumbnails();
                m_modFeedback = "Added " + p.substr(p.find_last_of('/') + 1);
            } else {
                m_modFeedback = "Rejected: " + err;
            }
            m_modFeedbackTimer = 4.0f;
        }
        return;
    }

    // --- rows -----------------------------------------------------------------
    for (int i = 0; i < (int)mods.size(); ++i) {
        int y = startY + (i - m_modScroll) * itemH;
        if (y + itemH < 0 || y > m_screenH - bottomBar) continue;
        Rectangle row{40.0f, (float)y, (float)m_screenW - 80.0f, (float)itemH - 8};

        const int btn = 30;
        int bx = (int)(row.x + row.width) - 12 - btn;
        int bY = (int)row.y + (itemH - 8 - btn) / 2;
        Rectangle delB{(float)bx, (float)bY, (float)btn, (float)btn};
        Rectangle rldB{(float)(bx - btn - 8), (float)bY, (float)btn, (float)btn};
        Rectangle advB{(float)(bx - 2 * (btn + 8)), (float)bY, (float)btn, (float)btn};
        Rectangle togB{(float)(bx - 3 * (btn + 8) - 34), (float)bY, 64.0f, (float)btn};

        if (!click) continue;

        // "Can be updated" opens the author's page in a browser and does
        // nothing else. The game never downloads or installs a mod: that would
        // mean fetching and running code chosen by a third party, which is the
        // one thing the whole capability sandbox exists to prevent.
        const auto& me = mods[i];
        if (me.manifestValid) {
            if (const auto* up = ModUpdates::get().infoFor(me.id)) {
                if (up->newer && !up->page.empty()) {
                    std::string status = modStateName(me.state);
                    if (me.state == ModState::PendingReload)
                        status = "Needs reload to take effect";
                    if (!me.diagnostic.empty()) status += " — " + me.diagnostic;
                    else if (!me.warnings.empty()) status += " — " + me.warnings[0];
                    if (status.size() > 96) status.resize(96);

                    float thumb = (float)itemH - 28;
                    int badgeX = (int)(row.x + 10 + thumb + 14)
                               + MeasureText(status.c_str(), 13) + 12;
                    if (ModConflicts::get().anyFor(me.id))
                        badgeX += MeasureText("conflict", 12) + 14 + 6;
                    Rectangle upB{(float)badgeX, row.y + 55,
                                  (float)(MeasureText("Can be updated", 12) + 14), 19.0f};
                    if (CheckCollisionPointRec(mouse, upB)) {
                        Audio::get().playSfx("click_light");
                        OpenURL(up->page.c_str());
                        m_modFeedback = "Opened the mod's page in your browser";
                        m_modFeedbackTimer = 3.0f;
                        return;
                    }
                }
            }
        }

        if (CheckCollisionPointRec(mouse, delB))  { Audio::get().playSfx("click_light"); m_modDeleteFor = i; return; }
        if (CheckCollisionPointRec(mouse, advB))  { Audio::get().playSfx("click_light"); m_modAdvancedFor = i; return; }
        if (CheckCollisionPointRec(mouse, rldB))  {
            Audio::get().playSfx("click_light");
            mm.reloadOne((size_t)i);
            clearModThumbnails();
            m_modFeedback = "Reloaded";
            m_modFeedbackTimer = 2.0f;
            return;
        }
        if (CheckCollisionPointRec(mouse, togB)) {
            ModEntry& e = mods[i];
            // The interlock: never let a mod and AI learning both be live.
            // Blocked by the interlock: the click did something, but not the
            // thing it looks like, so it must not sound like a toggle.
            if (!e.enabled && m_config.aiLearning) {
                Audio::get().playSfx("deny");
                m_modAiWarnFor = i;
                return;
            }
            Audio::get().playSfx(!e.enabled ? "toggle_on" : "toggle_off");
            mm.setEnabled((size_t)i, !e.enabled);
            if (mods[i].state == ModState::PendingReload) {
                m_modFeedback = "Enabled — reload to apply while a game is running";
                m_modFeedbackTimer = 4.0f;
            }
            return;
        }
        if (CheckCollisionPointRec(mouse, row)) { Audio::get().playSfx("click_light"); m_modIndex = i; return; }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Gearbox 1.1 backing
// ─────────────────────────────────────────────────────────────────────────────
//
// Every getter tolerates a missing world and an out-of-range handle by
// returning a neutral value rather than trapping: a mod polling during a map
// change must not take the game down with it.
//
// The four order functions are the only writes, and each is a THIN WRAPPER over
// the same pending-order queue the player's clicks and the AI's decisions use.
// They deliberately do not validate anything themselves -- adjacency, range,
// at-war and the percentage bounds are enforced in Game_TurnLogic's resolvers,
// which is the one place every order source converges. Validating here as well
// would be a second copy of the rules to drift out of step with the first.

int Game::modShipCount() const { return (int)m_ships.size(); }
bool Game::modShipExists(int sid) const {
    return sid >= 0 && (size_t)sid < m_ships.size() && m_ships[sid].countryId > 0;
}
int Game::modShipOwner(int sid) const {
    return modShipExists(sid) ? m_ships[sid].countryId : 0;
}
std::string Game::modShipType(int sid) const {
    return modShipExists(sid) ? m_ships[sid].type : std::string();
}
double Game::modShipLon(int sid) const { return modShipExists(sid) ? m_ships[sid].lon : 0.0; }
double Game::modShipLat(int sid) const { return modShipExists(sid) ? m_ships[sid].lat : 0.0; }
int Game::modShipHealth(int sid) const { return modShipExists(sid) ? m_ships[sid].health : 0; }
int Game::modShipCrew(int sid) const { return modShipExists(sid) ? m_ships[sid].crew : 0; }
double Game::modShipRange(int sid) const {
    return modShipExists(sid) ? (double)shipMaxRangePx(m_ships[sid]) : 0.0;
}

int Game::modArmyStackCount(int pid) const {
    auto it = m_provinceArmies.find(pid);
    return it == m_provinceArmies.end() ? 0 : (int)it->second.size();
}
int Game::modArmyStackOwner(int pid, int index) const {
    auto it = m_provinceArmies.find(pid);
    if (it == m_provinceArmies.end() || index < 0 || (size_t)index >= it->second.size()) return 0;
    return it->second[index].countryId;
}
long long Game::modArmyStackSize(int pid, int index) const {
    auto it = m_provinceArmies.find(pid);
    if (it == m_provinceArmies.end() || index < 0 || (size_t)index >= it->second.size()) return 0;
    return it->second[index].count;
}
long long Game::modCountryArmy(int cid) const {
    long long n = 0;
    for (const auto& [pid, units] : m_provinceArmies) {
        (void)pid;
        for (const auto& u : units) if (u.countryId == cid) n += u.count;
    }
    return n;
}
int Game::modProvinceFortification(int pid) const {
    auto it = m_provinceIndustry.find(pid);
    return it == m_provinceIndustry.end() ? 0 : it->second.fortification;
}
int Game::modProvincePortLevel(int pid) const {
    auto it = m_provincePorts.find(pid);
    return it == m_provincePorts.end() ? 0 : it->second.level;
}

bool Game::modOrderArmyMove(int fromPid, int toPid, int pct) {
    const Province* f = m_provinces.getProvinceById(fromPid);
    const Province* t = m_provinces.getProvinceById(toPid);
    if (!f || !t || f->countryId <= 0) return false;
    // Queued for the owner of the source province, so a mod cannot move a
    // country's army on behalf of somebody else.
    m_pendingMoveOrders.push_back({fromPid, toPid, std::clamp(pct, 0, 100), f->countryId});
    return true;
}
bool Game::modOrderShipMove(int sid, double lon, double lat) {
    if (!modShipExists(sid)) return false;
    m_pendingShipMoveOrders.push_back({sid, lon, lat});
    return true;
}
bool Game::modOrderShipEngage(int sid, int targetSid) {
    if (!modShipExists(sid) || !modShipExists(targetSid) || sid == targetSid) return false;
    m_pendingShipEngageOrders.push_back({sid, targetSid});
    return true;
}
bool Game::modOrderShipBombard(int sid, int pid, const std::string& ammo) {
    if (!modShipExists(sid) || !m_provinces.getProvinceById(pid) || ammo.empty()) return false;
    m_pendingShipBombardOrders.push_back({sid, pid, ammo});
    return true;
}

int Game::modResearchNodeCount() const { return (int)m_researchNodes.size(); }
static const ResearchNode* modNodeAt(const std::vector<ResearchNode>& v, int i) {
    return (i >= 0 && (size_t)i < v.size()) ? &v[i] : nullptr;
}
std::string Game::modResearchNodeId(int i) const {
    const ResearchNode* n = modNodeAt(m_researchNodes, i); return n ? n->id : std::string();
}
std::string Game::modResearchNodeName(int i) const {
    const ResearchNode* n = modNodeAt(m_researchNodes, i); return n ? n->name : std::string();
}
std::string Game::modResearchNodeCategory(int i) const {
    const ResearchNode* n = modNodeAt(m_researchNodes, i); return n ? n->category : std::string();
}
int Game::modResearchNodeCost(int i) const {
    const ResearchNode* n = modNodeAt(m_researchNodes, i); return n ? n->cost : 0;
}
bool Game::modCountryHasResearched(int cid, const std::string& nodeId) const {
    auto it = m_countryResearched.find(cid);
    return it != m_countryResearched.end() && it->second.count(nodeId) > 0;
}
// A SHARE OF INCOME, 0..1 -- not an absolute sum. That is how the game stores
// it and how the economy screen presents it, and a mod handed "points per turn"
// would be describing a quantity that does not exist.
double Game::modCountryResearchFunding(int cid) const {
    auto it = m_countryResearchAllocation.find(cid);
    return it == m_countryResearchAllocation.end() ? 0.0 : (double)it->second;
}
bool Game::modSetCountryResearchFunding(int cid, double value) {
    if (!m_countries.getCountry(cid)) return false;
    m_countryResearchAllocation[cid] = (float)std::clamp(value, 0.0, 1.0);
    return true;
}

double Game::modCountryCompassEcon(int cid) const {
    auto it = m_countryCompass.find(cid);
    return it == m_countryCompass.end() ? 0.0 : (double)it->second.economic;
}
double Game::modCountryCompassSocial(int cid) const {
    auto it = m_countryCompass.find(cid);
    return it == m_countryCompass.end() ? 0.0 : (double)it->second.social;
}
double Game::modProvinceUnrest(int pid) const {
    return (double)const_cast<Game*>(this)->getProvinceRebellionChance(pid);
}
int Game::modPolicyCount() const { return (int)m_allPolicies.size(); }
std::string Game::modPolicyId(int i) const {
    return (i >= 0 && (size_t)i < m_allPolicies.size()) ? m_allPolicies[i].id : std::string();
}
std::string Game::modPolicyName(int i) const {
    return (i >= 0 && (size_t)i < m_allPolicies.size()) ? m_allPolicies[i].name : std::string();
}
bool Game::modCountryHasPolicy(int cid, const std::string& policyId) const {
    auto it = m_countryActivePolicyIndices.find(cid);
    if (it == m_countryActivePolicyIndices.end()) return false;
    for (int idx : it->second)
        if (idx >= 0 && (size_t)idx < m_activePolicies.size() &&
            m_activePolicies[idx].policyId == policyId) return true;
    return false;
}
// ROUTED THROUGH enactPolicy/cancelPolicy, NOT written into m_activePolicies.
// A mod that appended the struct itself would skip canCountryEnactPolicy -- the
// cost, the prerequisites, the per-turn enactment cap -- and produce a country
// running policies it could never have afforded. The same rule as every other
// write in this ABI: use the path the player's own click uses.
bool Game::modSetCountryPolicy(int cid, const std::string& policyId, bool on) {
    if (!m_countries.getCountry(cid)) return false;
    const Policy* def = nullptr;
    for (const auto& p : m_allPolicies)
        if (p.id == policyId) { def = &p; break; }
    if (!def) return false;

    const bool has = modCountryHasPolicy(cid, policyId);
    if (on == has) return true;  // already in the asked-for state

    if (on) {
        if (!canCountryEnactPolicy(cid, *def)) return false;
        enactPolicy(cid, policyId);
        return modCountryHasPolicy(cid, policyId);
    }

    auto it = m_countryActivePolicyIndices.find(cid);
    if (it == m_countryActivePolicyIndices.end()) return false;
    for (int idx : it->second) {
        if (idx >= 0 && (size_t)idx < m_activePolicies.size() &&
            m_activePolicies[idx].policyId == policyId) {
            cancelPolicy(idx);
            return true;
        }
    }
    return false;
}

int Game::modProvinceMinorityCount(int pid) const {
    auto it = m_provinceMinorities.find(pid);
    return it == m_provinceMinorities.end() ? 0 : (int)it->second.size();
}
std::string Game::modProvinceMinorityName(int pid, int index) const {
    auto it = m_provinceMinorities.find(pid);
    if (it == m_provinceMinorities.end() || index < 0 || (size_t)index >= it->second.size())
        return {};
    return it->second[index].name;
}
double Game::modProvinceMinorityShare(int pid, int index) const {
    auto it = m_provinceMinorities.find(pid);
    if (it == m_provinceMinorities.end() || index < 0 || (size_t)index >= it->second.size())
        return 0.0;
    return (double)it->second[index].pct;
}

double Game::modCountryIncomeGross(int cid) const {
    if (!m_countries.getCountry(cid)) return 0.0;
    return (double)const_cast<Game*>(this)->computeCountryIncome(cid).total;
}
double Game::modCountryIncomeNet(int cid) const {
    if (!m_countries.getCountry(cid)) return 0.0;
    return (double)const_cast<Game*>(this)->computeCountryIncome(cid).net;
}
double Game::modCountryArmyUpkeep(int cid) const {
    if (!m_countries.getCountry(cid)) return 0.0;
    return (double)const_cast<Game*>(this)->computeCountryIncome(cid).armyExpenses;
}
double Game::modCountryNavyUpkeep(int cid) const {
    if (!m_countries.getCountry(cid)) return 0.0;
    return (double)const_cast<Game*>(this)->computeCountryIncome(cid).navyExpenses;
}
bool Game::modCountryIsBankrupt(int cid) const {
    return const_cast<Game*>(this)->isBankrupt(cid);
}
int Game::modProvinceIndustryLevel(int pid) const {
    auto it = m_provinceIndustry.find(pid);
    return it == m_provinceIndustry.end() ? 0 : it->second.level;
}
std::string Game::modProvinceIndustrySpecialization(int pid) const {
    auto it = m_provinceIndustry.find(pid);
    return it == m_provinceIndustry.end() ? std::string() : it->second.specialization;
}
double Game::modProvinceResource(int pid, const std::string& which) const {
    auto it = m_provinceResources.find(pid);
    if (it == m_provinceResources.end()) return 0.0;
    const auto& r = it->second;
    if (which == "oil") return r.oil.amount;
    if (which == "gold") return r.gold.amount;
    if (which == "metal") return r.metal.amount;
    if (which == "rubber") return r.rubber.amount;
    if (which == "gemstones") return r.gemstones.amount;
    return 0.0;
}
bool Game::modSetProvinceIndustryLevel(int pid, int level) {
    if (!m_provinces.getProvinceById(pid)) return false;
    auto& ind = m_provinceIndustry[pid];      // indexed, not found: see processUpgrades
    ind.level = std::clamp(level, 0, 10);
    ind.income = ind.level * 2.0f;
    return true;
}

bool Game::modProvinceIsCoastal(int pid) const {
    return const_cast<Game*>(this)->isProvinceCoastal(pid);
}
bool Game::modSeaRouteExists(double lon1, double lat1, double lon2, double lat2) const {
    return navReachable(lon1, lat1, lon2, lat2);
}
bool Game::modPointIsLand(double lon, double lat) const {
    return m_landSea.isLand((float)lon, (float)lat);
}
