// The .odsv binary turn delta, through pack and unpack.
//
// This codec is two things at once: what a save is made of, and what one
// multiplayer peer hands another. So it has to survive both a round trip and a
// stranger -- a turn arriving off a socket is attacker-shaped input, and every
// truncation of it must fail rather than read past its end.
//
// The case that brought this file into existence: a province may hold up to
// Game::MAX_PROVINCE_POP (1e10) and the population field is 32 bits, so a
// 60-turn simulation of 1939 recorded a province going from 4,249,772,357 to
// 171,303,012 in one turn -- 2^32 subtracted, and nothing to say so.
//
// Build target: SaveDeltaTest. Run it; non-zero exit means a case failed.

#include "SaveManager.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(const char* what, bool ok, const std::string& got = {}) {
    g_checks++;
    if (ok) { printf("  ok    %s\n", what); return; }
    g_failures++;
    printf("  FAIL  %s%s%s\n", what, got.empty() ? "" : "  --  ", got.c_str());
}

void checkPop(const char* what, long long want, long long got) {
    check(what, want == got,
          "wanted " + std::to_string(want) + ", got " + std::to_string(got));
}

TurnDelta oneProvince(int pid, long long pop) {
    TurnDelta d;
    d.turnNumber = 57;
    d.researchAllocation = 0.25f;
    d.pacificationAllocation = 0.1f;
    d.researchActiveNode = 4;
    d.researchPoints = 900;
    ProvinceDelta p;
    p.provinceId = pid;
    p.populationChanged = true;
    p.newPopulation = pop;
    d.provinces.push_back(p);
    return d;
}

long long roundTripPop(long long pop) {
    auto packed = SaveManager::packTurn(oneProvince(583, pop));
    TurnDelta out;
    if (!SaveManager::unpackTurn(packed.data(), packed.size(), out)) return -1;
    if (out.provinces.size() != 1) return -1;
    return out.provinces[0].newPopulation;
}

// The bytes an old build wrote: no wide table, and the field taken modulo 2^32.
// Reconstructed by hand rather than kept as a fixture, so this stays readable
// next to the format comment in SaveManager.h.
std::vector<uint8_t> legacyPacked(int pid, uint32_t population) {
    std::vector<uint8_t> b;
    auto u16 = [&](uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); };
    auto u32 = [&](uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); };
    u16(57); u16(1); u16(0); u16(0);        // turn, provinces, ships, armies
    u16((uint16_t)pid);
    u16(1 << 1);                            // POPULATION only
    u32(population);
    b.push_back(0x01);                      // extra_flags: research state
    u32(0x3E800000); u32(0x3DCCCCCD);       // allocation 0.25, pacification 0.1
    u16(5); u16(900);                       // active node 4 (+1), points
    b.push_back(0xFF);
    return b;
}

}  // namespace

int main() {
    printf("odsv turn delta\n");

    printf("\npopulations the 32-bit field can hold\n");
    checkPop("zero", 0, roundTripPop(0));
    checkPop("an ordinary province", 4'812'000, roundTripPop(4'812'000));
    checkPop("one below the field's ceiling", 4294967294LL, roundTripPop(4294967294LL));
    checkPop("exactly the field's ceiling", 4294967295LL, roundTripPop(4294967295LL));

    printf("\npopulations wider than the field\n");
    // The turn the reproduction wrapped on, and what it came back as before.
    checkPop("the turn that used to lose 2^32", 4466270308LL, roundTripPop(4466270308LL));
    checkPop("one past the field's ceiling", 4294967296LL, roundTripPop(4294967296LL));
    checkPop("MAX_PROVINCE_POP", 10000000000LL, roundTripPop(10000000000LL));
    // Not a population the game produces, but the field is unsigned and a
    // negative cast into it is 4.29 billion, which would read as plausible.
    checkPop("a negative, kept negative", -5, roundTripPop(-5));

    printf("\nsaves that predate the wide table\n");
    {
        // What the pre-fix build wrote for a province of 4,466,270,308.
        auto packed = legacyPacked(583, 171303012u);
        TurnDelta out;
        check("an old turn still decodes",
              SaveManager::unpackTurn(packed.data(), packed.size(), out));
        check("its province survives", out.provinces.size() == 1);
        if (out.provinces.size() == 1)
            checkPop("its population is what the old build stored",
                     171303012LL, out.provinces[0].newPopulation);
        check("its research state survives",
              out.researchActiveNode == 4 && out.researchPoints == 900 &&
              out.researchAllocation == 0.25f,
              "node " + std::to_string(out.researchActiveNode) +
              ", points " + std::to_string(out.researchPoints) +
              ", allocation " + std::to_string(out.researchAllocation));
    }
    {
        // And a save old enough to have no trailer at all.
        auto packed = legacyPacked(583, 900000u);
        packed.resize(packed.size() - 14);   // drop extra_flags..end marker
        TurnDelta out;
        check("a turn with no trailer decodes",
              SaveManager::unpackTurn(packed.data(), packed.size(), out));
        if (out.provinces.size() == 1)
            checkPop("its population survives", 900000LL,
                     out.provinces[0].newPopulation);
    }

    printf("\nbytes a peer could send\n");
    {
        // Nothing below is a save; it is what arrives on a socket when a peer
        // is malicious, or merely truncated by a dropped connection.
        auto full = SaveManager::packTurn(oneProvince(583, 6'000'000'000LL));
        bool overran = false;
        for (size_t n = 0; n < full.size(); ++n) {
            TurnDelta out;
            // A short read may fail or succeed; what it may not do is read
            // past `n`. Under ASan an overrun is a crash, which is the point.
            std::vector<uint8_t> cut(full.begin(), full.begin() + n);
            if (SaveManager::unpackTurn(cut.data(), cut.size(), out)) {
                for (auto& p : out.provinces)
                    if (p.newPopulation < 0 || p.newPopulation > 10000000000LL)
                        overran = true;
            }
        }
        check("every truncation stops inside the buffer", !overran);

        // A wide table claiming more entries than it carries. Counting back
        // from the end marker: count(2) + one entry of id(2) + population(8).
        auto lying = full;
        lying[lying.size() - 13] = 0xFF;     // the count's low byte
        lying[lying.size() - 12] = 0xFF;
        TurnDelta out;
        check("a wide table that overruns is refused",
              !SaveManager::unpackTurn(lying.data(), lying.size(), out));
    }

    printf("\nthe whole delta, unchanged by the new field\n");
    {
        TurnDelta d = oneProvince(288, 1'181'712'752LL);
        d.provinces[0].ownerChanged = true;      d.provinces[0].newOwner = 12;
        d.provinces[0].industryLevelChanged = true; d.provinces[0].newIndustryLevel = 7;
        d.provinces[0].incomeChanged = true;     d.provinces[0].newIncome = 3.5f;
        ProvinceDelta big;                        // and one that needs the table
        big.provinceId = 583;
        big.populationChanged = true;
        big.newPopulation = 9'999'999'999LL;
        big.fortificationChanged = true;
        big.newFortification = 3;
        d.provinces.push_back(big);
        ShipDelta s; s.shipIndex = 2; s.crewChanged = true; s.newCrew = 640;
        d.ships.push_back(s);
        ArmyDelta a; a.provinceId = 288; a.units.push_back({12, 250000});
        d.armies.push_back(a);

        auto packed = SaveManager::packTurn(d);
        TurnDelta out;
        check("a mixed turn decodes",
              SaveManager::unpackTurn(packed.data(), packed.size(), out));
        check("both provinces survive", out.provinces.size() == 2);
        if (out.provinces.size() == 2) {
            checkPop("the ordinary population", 1'181'712'752LL,
                     out.provinces[0].newPopulation);
            checkPop("the wide population", 9'999'999'999LL,
                     out.provinces[1].newPopulation);
            check("the owner beside it", out.provinces[0].newOwner == 12);
            check("the industry level beside it",
                  out.provinces[0].newIndustryLevel == 7);
            check("the fortification beside the wide one",
                  out.provinces[1].newFortification == 3);
        }
        check("the ship", out.ships.size() == 1 && out.ships[0].newCrew == 640);
        check("the army", out.armies.size() == 1 &&
                          out.armies[0].units.size() == 1 &&
                          out.armies[0].units[0].count == 250000);
        check("the research state", out.researchActiveNode == 4 &&
                                    out.researchPoints == 900);
        check("the size estimate matches what was written",
              SaveManager::estimateDeltaSize(d) == packed.size(),
              "estimated " + std::to_string(SaveManager::estimateDeltaSize(d)) +
              ", wrote " + std::to_string(packed.size()));
    }

    printf("\na save that needs no wide table is byte-identical to before\n");
    {
        auto packed = SaveManager::packTurn(oneProvince(583, 171303012LL));
        auto legacy = legacyPacked(583, 171303012u);
        check("same bytes as the previous build wrote",
              packed == legacy,
              std::to_string(packed.size()) + " bytes vs " +
              std::to_string(legacy.size()));
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
