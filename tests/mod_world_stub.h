#pragma once
#include "mods/ModHost.h"

/**
 * A ModGameAccess that answers "nothing" to everything.
 *
 * ModGameAccess is deliberately all-pure-virtual: forgetting to implement a new
 * method in the game's real GameModAccess must be a BUILD ERROR, not a call
 * that silently returns 0 and looks like an empty world. The cost is that every
 * test fixture has to stub all 114 methods whether it exercises them or not, and
 * Gearbox 1.1 alone added ninety of them.
 *
 * So the neutral defaults live here, once, in the test tree only. A fixture
 * derives from this and overrides the handful it actually cares about. When the
 * interface grows, this is the single file the tests have to follow it to --
 * and the game's own implementation still has to implement the new method for
 * real, because this class is not in its inheritance chain.
 *
 * Neutral means 0, 0.0, false or "" for a value, and 0xFFFFFFFF for the id
 * lookups, which is what the host treats as "no such thing" throughout the ABI.
 */
struct StubWorld : ModGameAccess {
    uint32_t turnNumber() override { return 0; }
    uint32_t countryCount() override { return 0; }
    uint32_t countryAt(uint32_t) override { return 0xFFFFFFFFu; }
    bool countryExists(uint32_t) override { return false; }
    std::string countryName(uint32_t) override { return {}; }
    double countryTreasury(uint32_t) override { return 0.0; }
    uint32_t countryProvinceCount(uint32_t) override { return 0; }
    long long provincePopulation(uint32_t) override { return 0; }
    uint32_t provinceOwner(uint32_t) override { return 0xFFFFFFFFu; }
    uint32_t mapWidth() override { return 0; }
    uint32_t mapHeight() override { return 0; }
    uint32_t provinceCount() override { return 0; }
    uint32_t provinceAt(uint32_t) override { return 0xFFFFFFFFu; }
    bool provinceExists(uint32_t) override { return false; }
    std::string provinceName(uint32_t) override { return {}; }
    double provinceCenterX(uint32_t) override { return 0.0; }
    double provinceCenterY(uint32_t) override { return 0.0; }
    bool provinceIsLand(uint32_t) override { return false; }
    uint32_t provinceNeighborCount(uint32_t) override { return 0; }
    uint32_t provinceNeighborAt(uint32_t, uint32_t) override { return 0xFFFFFFFFu; }
    bool atWar(uint32_t, uint32_t) override { return false; }
    bool allied(uint32_t, uint32_t) override { return false; }
    bool nonAggression(uint32_t, uint32_t) override { return false; }
    bool guaranteed(uint32_t, uint32_t) override { return false; }
    bool proposeWar(uint32_t, uint32_t) override { return false; }
    bool setCountryTreasury(uint32_t, double) override { return false; }
    bool addCountryTreasury(uint32_t, double) override { return false; }
    bool setProvinceOwner(uint32_t, uint32_t) override { return false; }
    bool setProvincePopulation(uint32_t, long long) override { return false; }
    uint32_t shipCount() override { return 0; }
    uint32_t shipAt(uint32_t) override { return 0xFFFFFFFFu; }
    bool shipExists(uint32_t) override { return false; }
    uint32_t shipOwner(uint32_t) override { return 0xFFFFFFFFu; }
    std::string shipType(uint32_t) override { return {}; }
    double shipLon(uint32_t) override { return 0.0; }
    double shipLat(uint32_t) override { return 0.0; }
    int32_t shipHealth(uint32_t) override { return 0; }
    int32_t shipCrew(uint32_t) override { return 0; }
    double shipRange(uint32_t) override { return 0.0; }
    uint32_t armyStackCount(uint32_t) override { return 0; }
    uint32_t armyStackOwner(uint32_t, uint32_t) override { return 0xFFFFFFFFu; }
    long long armyStackSize(uint32_t, uint32_t) override { return 0; }
    long long countryArmy(uint32_t) override { return 0; }
    int32_t provinceFortification(uint32_t) override { return 0; }
    int32_t provincePortLevel(uint32_t) override { return 0; }
    bool orderArmyMove(uint32_t, uint32_t, uint32_t) override { return false; }
    bool orderShipMove(uint32_t, double, double) override { return false; }
    bool orderShipEngage(uint32_t, uint32_t) override { return false; }
    bool orderShipBombard(uint32_t, uint32_t, const std::string&) override { return false; }
    uint32_t researchNodeCount() override { return 0; }
    std::string researchNodeId(uint32_t) override { return {}; }
    std::string researchNodeName(uint32_t) override { return {}; }
    std::string researchNodeCategory(uint32_t) override { return {}; }
    int32_t researchNodeCost(uint32_t) override { return 0; }
    bool countryHasResearched(uint32_t, const std::string&) override { return false; }
    double countryResearchFunding(uint32_t) override { return 0.0; }
    bool setCountryResearchFunding(uint32_t, double) override { return false; }
    double countryCompassEcon(uint32_t) override { return 0.0; }
    double countryCompassSocial(uint32_t) override { return 0.0; }
    double provinceUnrest(uint32_t) override { return 0.0; }
    uint32_t policyCount() override { return 0; }
    std::string policyId(uint32_t) override { return {}; }
    std::string policyName(uint32_t) override { return {}; }
    bool countryHasPolicy(uint32_t, const std::string&) override { return false; }
    bool setCountryPolicy(uint32_t, const std::string&, bool) override { return false; }
    uint32_t provinceMinorityCount(uint32_t) override { return 0; }
    std::string provinceMinorityName(uint32_t, uint32_t) override { return {}; }
    double provinceMinorityShare(uint32_t, uint32_t) override { return 0.0; }
    double countryIncomeGross(uint32_t) override { return 0.0; }
    double countryIncomeNet(uint32_t) override { return 0.0; }
    double countryArmyUpkeep(uint32_t) override { return 0.0; }
    double countryNavyUpkeep(uint32_t) override { return 0.0; }
    bool countryIsBankrupt(uint32_t) override { return false; }
    int32_t provinceIndustryLevel(uint32_t) override { return 0; }
    std::string provinceIndustrySpecialization(uint32_t) override { return {}; }
    double provinceResource(uint32_t, const std::string&) override { return 0.0; }
    bool setProvinceIndustryLevel(uint32_t, int32_t) override { return false; }
    bool provinceIsCoastal(uint32_t) override { return false; }
    bool seaRouteExists(double, double, double, double) override { return false; }
    bool pointIsLand(double, double) override { return false; }
    bool editorActive() override { return false; }
    uint32_t editorProvinceCount() override { return 0; }
    uint32_t editorProvinceAt(uint32_t) override { return 0xFFFFFFFFu; }
    long long editorProvincePopulation(uint32_t) override { return 0; }
    int32_t editorProvinceIndustryLevel(uint32_t) override { return 0; }
    int32_t editorProvinceFortification(uint32_t) override { return 0; }
    int32_t editorProvincePortLevel(uint32_t) override { return 0; }
    double editorProvinceResource(uint32_t, const std::string&) override { return 0.0; }
    double editorProvinceCompassEcon(uint32_t) override { return 0.0; }
    double editorProvinceCompassSocial(uint32_t) override { return 0.0; }
    bool editorSetProvincePopulation(uint32_t, long long) override { return false; }
    bool editorSetProvinceIndustryLevel(uint32_t, int32_t) override { return false; }
    bool editorSetProvinceFortification(uint32_t, int32_t) override { return false; }
    bool editorSetProvincePortLevel(uint32_t, int32_t) override { return false; }
    bool editorSetProvinceResource(uint32_t, const std::string&, double) override { return false; }
    bool editorSetProvinceCompass(uint32_t, double, double) override { return false; }
    std::string editorMapName() override { return {}; }
    bool editorSetMapName(const std::string&) override { return false; }
    bool editorSetAuthor(const std::string&) override { return false; }
    bool editorSetLicense(const std::string&) override { return false; }
    uint32_t netPeerAt(uint32_t) override { return 0xFFFFFFFFu; }
    std::string netPeerName(uint32_t) override { return {}; }
    uint32_t netMaxMessageBytes() override { return 0; }
    uint32_t neuralModuleCount() override { return 0; }
    std::string neuralModuleName(uint32_t) override { return {}; }
    uint32_t neuralActionCount(uint32_t) override { return 0; }
    std::string neuralActionName(uint32_t, uint32_t) override { return {}; }
    bool neuralCountryIsAI(uint32_t) override { return false; }
    long long neuralUpdateCount() override { return 0; }
    bool neuralModelLoaded() override { return false; }
    uint32_t neuralFeatureCount() override { return 0; }
    uint32_t neuralFeatures(uint32_t, float*, uint32_t) override { return 0; }
    uint32_t neuralRewardCount() override { return 0; }
    double neuralRewardMean(uint32_t) override { return 0.0; }
};
