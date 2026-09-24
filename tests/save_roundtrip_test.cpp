// A whole .odsv, written and read back -- and written on one platform and read
// on another.
//
// WHY THIS EXISTS
//
// save_delta_test.cpp covers the turn-delta codec in memory. Nothing covered
// the ARCHIVE: create it, write state into it, append turns, close it, open it
// again and get the same world back. So the one failure a player actually
// reports -- "I load my save and the game crashes" -- had no test at all, and
// the first report of it came from a Windows user on a world written by the
// same version, on a platform the author does not own.
//
// It links SaveManager.cpp and nothing else: no window, no GL, no map. That is
// deliberate and it is what makes this the cross-platform vehicle. The one
// existing per-platform save producer, qualify.sh's --simulate, needs a display
// and is SKIPPED on the hosted macOS runners -- so it cannot be what proves a
// macOS-written save loads on Windows.
//
//   SaveRoundTripTest                 round trip in a temp directory
//   SaveRoundTripTest --emit   <path> write the canonical save and stop
//   SaveRoundTripTest --verify <path> read a canonical save and check it
//
// The emit/verify pair is the cross-platform half: every platform emits one,
// every platform verifies every other platform's. What is compared is MEANING,
// not bytes -- two correct zips of the same content differ in their headers,
// so a byte comparison would fail on a difference nobody can play.

#include "SaveManager.h"

#include "miniz.h"
#include "miniz_zip.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_failures = 0, g_checks = 0;

void check(const char* what, bool ok, const std::string& detail = {}) {
    ++g_checks;
    if (ok) { printf("  ok    %s\n", what); return; }
    ++g_failures;
    printf("  FAIL  %s%s%s\n", what, detail.empty() ? "" : "  --  ", detail.c_str());
}

template <typename T>
void checkEq(const char* what, T want, T got) {
    check(what, want == got,
          "wanted " + std::to_string(want) + ", got " + std::to_string(got));
}

// ── the canonical world ──────────────────────────────────────────────
//
// Fixed values, chosen so nothing about the machine leaks in: no timestamps,
// no locale, and every float an exact binary fraction so a comparison is a
// comparison and not a tolerance.

const char* kStateJson =
    "{\"schema\":1,\"turn\":3,\"note\":\"canonical state for the round trip\"}";
const char* kOdmBytes = "PK\003\004 not a real odmap, but bytes that must survive";

SaveMetadata canonicalMeta() {
    SaveMetadata m;
    m.saveName = "RoundTrip";
    m.version  = "canonical";
    m.created  = "2000-01-01 00:00:00";
    m.lastPlayed = "2000-01-01 00:00:00";
    // ZERO, not three: appendTurn maintains this itself (meta.turnCount++ per
    // call, re-read from the file each time), so seeding it and then appending
    // three turns records six. Asserting 3 after three appends is asserting
    // the contract; asserting what was passed in would be asserting a guess.
    m.turnCount = 0;
    m.provinceCount = 1642;
    m.shipCount = 7;
    m.playerCountryId = 42;
    m.countryTreasuries[1] = 1234.5;
    m.countryTreasuries[7] = -80.25;
    m.countryCompasses[1] = PoliticalCompassSave{ -37.5f, 62.25f };
    return m;
}

TurnDelta canonicalTurn(int n) {
    TurnDelta d;
    d.turnNumber = n;
    d.researchAllocation = 0.25f;
    d.pacificationAllocation = 0.5f;
    d.researchActiveNode = 11;
    d.researchPoints = 900 + n;
    d.researchGroups[1] = TurnDelta::ResearchGroupDelta{ 5, 4, 75, true };
    d.researchGroups[2] = TurnDelta::ResearchGroupDelta{ -1, 9, 25, false };

    ProvinceDelta p;
    p.provinceId = 300 + n;
    p.ownerChanged = true;          p.newOwner = 9;
    // ABOVE 2^32 ON PURPOSE. A 32-bit population field is what made a province
    // lose 4,294,967,296 people in one turn; the wide value rides in the
    // trailer and this is what proves it still does.
    p.populationChanged = true;     p.newPopulation = 5'000'000'123LL;
    p.industryLevelChanged = true;  p.newIndustryLevel = 4;
    p.fortificationChanged = true;  p.newFortification = 2;
    p.incomeChanged = true;         p.newIncome = 1.5f;
    p.resourceIncomeChanged = true; p.newResourceIncome = 0.75f;
    p.popIncomeChanged = true;      p.newPopIncome = 0.125f;
    p.popModifierChanged = true;    p.newPopModifier = 1.25f;
    // -1 is a real value and the commonest one, so it is the one to carry.
    p.outputChanged = true;         p.newOutput = -1;
    d.provinces.push_back(p);

    ShipDelta s;
    s.shipIndex = 2;
    s.latChanged = true;  s.newLat = -33.5;
    s.lonChanged = true;  s.newLon = 151.25;
    s.healthChanged = true; s.newHealth = 61;
    s.crewChanged = true;   s.newCrew = 1320;
    s.countryIdChanged = true; s.newCountryId = 3;
    d.ships.push_back(s);

    ArmyDelta a;
    a.provinceId = 500;
    a.units.push_back(ArmyDelta::Unit{ 3, 250000, 0 });
    a.units.push_back(ArmyDelta::Unit{ 4, 90000, 2 });   // type rides in the trailer
    d.armies.push_back(a);
    return d;
}

void writeCanonical(const std::string& path) {
    SaveManager::createSave(path, std::string(kOdmBytes), canonicalMeta());
    SaveManager::writeState(path, kStateJson,
                            {{"rebellion/9.svg", "<svg/>"}});
    for (int n = 1; n <= 3; ++n) SaveManager::appendTurn(path, canonicalTurn(n));
}

void verifyCanonical(const std::string& path, const char* origin) {
    printf("== %s ==\n", origin);

    const SaveMetadata m = SaveManager::readMetadata(path);
    check("the save opens and names itself", m.saveName == "RoundTrip", m.saveName);
    checkEq("turn count survives", 3, m.turnCount);
    checkEq("province count survives", 1642, m.provinceCount);
    checkEq("player country survives", 42, m.playerCountryId);
    check("a treasury survives to the cent",
          m.countryTreasuries.count(1) && m.countryTreasuries.at(1) == 1234.5);
    check("a negative treasury survives",
          m.countryTreasuries.count(7) && m.countryTreasuries.at(7) == -80.25);
    check("a compass survives",
          m.countryCompasses.count(1) &&
          m.countryCompasses.at(1).economic == -37.5f &&
          m.countryCompasses.at(1).social == 62.25f);

    check("state.json comes back byte for byte", SaveManager::readState(path) == kStateJson);
    check("an extra entry comes back too",
          SaveManager::readEntry(path, "rebellion/9.svg") == "<svg/>");
    const std::vector<uint8_t> odm = SaveManager::extractODM(path);
    check("the embedded map comes back byte for byte",
          std::string(odm.begin(), odm.end()) == kOdmBytes);

    for (int n = 1; n <= 3; ++n) {
        const TurnDelta want = canonicalTurn(n);
        const TurnDelta got  = SaveManager::readTurn(path, n);
        const std::string t = "turn " + std::to_string(n) + ": ";
        checkEq((t + "number").c_str(), want.turnNumber, got.turnNumber);
        check((t + "research allocation").c_str(),
              want.researchAllocation == got.researchAllocation);
        check((t + "pacification allocation").c_str(),
              want.pacificationAllocation == got.pacificationAllocation);
        checkEq((t + "research points").c_str(), want.researchPoints, got.researchPoints);
        checkEq((t + "research group 2 node").c_str(),
                want.researchGroups[1].activeNode, got.researchGroups[1].activeNode);
        check((t + "research group 2 auto-advance").c_str(),
              want.researchGroups[1].autoAdvance == got.researchGroups[1].autoAdvance);

        if (got.provinces.size() != 1) {
            check((t + "one province").c_str(), false,
                  std::to_string(got.provinces.size()) + " provinces");
            continue;
        }
        const ProvinceDelta& a = want.provinces[0];
        const ProvinceDelta& b = got.provinces[0];
        checkEq((t + "province id").c_str(), a.provinceId, b.provinceId);
        checkEq((t + "new owner").c_str(), a.newOwner, b.newOwner);
        checkEq((t + "population above 2^32").c_str(), a.newPopulation, b.newPopulation);
        checkEq((t + "industry level").c_str(), a.newIndustryLevel, b.newIndustryLevel);
        checkEq((t + "fortification").c_str(), a.newFortification, b.newFortification);
        check((t + "income").c_str(), a.newIncome == b.newIncome);
        check((t + "pop modifier").c_str(), a.newPopModifier == b.newPopModifier);
        checkEq((t + "industry output (-1)").c_str(), a.newOutput, b.newOutput);

        if (got.ships.size() == 1) {
            check((t + "ship position").c_str(),
                  got.ships[0].newLat == -33.5 && got.ships[0].newLon == 151.25);
            checkEq((t + "ship crew").c_str(), 1320, got.ships[0].newCrew);
        } else {
            check((t + "one ship").c_str(), false);
        }
        if (got.armies.size() == 1 && got.armies[0].units.size() == 2) {
            checkEq((t + "army count").c_str(), 250000, got.armies[0].units[0].count);
            checkEq((t + "troop type in the trailer").c_str(),
                    2, (int)got.armies[0].units[1].type);
        } else {
            check((t + "one army of two units").c_str(), false);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "--emit") == 0) {
        writeCanonical(argv[2]);
        printf("wrote %s\n", argv[2]);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--verify") == 0) {
        verifyCanonical(argv[2], argv[2]);
        printf("\n%s\n", g_failures ? "FAILURES" : "all ok");
        return g_failures ? 1 : 0;
    }

    printf("A whole .odsv, written and read back\n");
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(ec) / "od-save-roundtrip";
    std::filesystem::create_directories(dir, ec);
    const std::string path = (dir / "RoundTrip.odsv").string();
    std::filesystem::remove(path, ec);

    writeCanonical(path);
    check("the file exists after writing", std::filesystem::exists(path, ec));
    verifyCanonical(path, "written and read on this platform");

    // ── A TURN WRITTEN TWICE IS ONE TURN ──
    //
    // A rewind, a replay, or a host resolving the same turn twice re-appends a
    // turn number the archive already has. That used to leave two entries with
    // the same name, and miniz's binary search picks between them arbitrarily:
    // in multiplayer the host could broadcast a delta other than the one it
    // resolved. `Quick Start.odsv` carries 160 such pairs.
    printf("== the same turn, written twice ==\n");
    {
        TurnDelta again = canonicalTurn(2);
        again.researchAllocation = 0.75f;          // distinguishable from the first
        SaveManager::appendTurn(path, again);

        const TurnDelta got = SaveManager::readTurn(path, 2);
        check("re-writing a turn replaces it rather than adding a second copy",
              got.researchAllocation == 0.75f,
              "read back " + std::to_string(got.researchAllocation));

        // Counted in the archive itself, because "the read returned the new
        // one" would also pass if the search happened to land on it.
        size_t copies = 0;
        mz_zip_archive z{};
        if (mz_zip_reader_init_file(&z, path.c_str(), 0)) {
            const int fc = (int)mz_zip_reader_get_num_files(&z);
            for (int i = 0; i < fc; ++i) {
                mz_zip_archive_file_stat st{};
                if (mz_zip_reader_file_stat(&z, i, &st) &&
                    strcmp(st.m_filename, "turns/t_00002.dat") == 0) copies++;
            }
            mz_zip_reader_end(&z);
        }
        checkEq("and the archive holds exactly one entry for that turn", 1, (int)copies);
    }

    // ── what a broken save must do ──
    printf("== a save that is not one ==\n");
    const SaveMetadata missing = SaveManager::readMetadata((dir / "no-such.odsv").string());
    check("a missing save reports nothing rather than inventing it",
          missing.saveName.empty() && missing.turnCount == 0);
    const std::string junkPath = (dir / "junk.odsv").string();
    if (FILE* f = fopen(junkPath.c_str(), "wb")) {
        fputs("this is not a zip", f); fclose(f);
    }
    const SaveMetadata junk = SaveManager::readMetadata(junkPath);
    check("a save that is not an archive reports nothing", junk.saveName.empty());
    const TurnDelta absent = SaveManager::readTurn(path, 99);
    checkEq("a turn that was never played reads as no turn", 0, absent.turnNumber);

    std::filesystem::remove(path, ec);
    std::filesystem::remove(junkPath, ec);
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
