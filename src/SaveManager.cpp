#include "SaveManager.h"
#include "util/WebPersist.h"
#include "miniz.h"
#include "miniz_zip.h"
#include "libdeflate.h"
#include "json.hpp"
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdint>
#include <functional>
#include <vector>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <random>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#ifdef OD_HAS_P2
#include "odseal.h"
#endif

// ─── Binary pack helpers ─────────────────────────────────

void SaveManager::writeU16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back((uint8_t)(v & 0xFF));
    buf.push_back((uint8_t)((v >> 8) & 0xFF));
}

void SaveManager::writeU32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back((uint8_t)(v & 0xFF));
    buf.push_back((uint8_t)((v >> 8) & 0xFF));
    buf.push_back((uint8_t)((v >> 16) & 0xFF));
    buf.push_back((uint8_t)((v >> 24) & 0xFF));
}

void SaveManager::writeU64(std::vector<uint8_t>& buf, uint64_t v) {
    writeU32(buf, (uint32_t)(v & 0xFFFFFFFFu));
    writeU32(buf, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

void SaveManager::writeFloat(std::vector<uint8_t>& buf, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    writeU32(buf, bits);
}

// A population the 32-bit province field cannot hold, and so has to be named
// again in the trailer. Negative is not a population the game produces, but
// (uint32_t)(-1) is 4.29 billion, so it is worth being exact about rather than
// trusting: anything outside the field's range goes in the table.
static bool popNeedsWideField(long long pop) {
    return pop < 0 || pop > (long long)UINT32_MAX;
}

static uint32_t popSaturated(long long pop) {
    if (pop < 0) return 0;
    if (pop > (long long)UINT32_MAX) return UINT32_MAX;
    return (uint32_t)pop;
}

// ─── Pack one turn delta to binary ───────────────────────

// ── A NUMBER JSON CAN ACTUALLY HOLD ──
//
// std::to_string(NaN) is the bare token "nan", and infinity is "inf". Neither
// is JSON, and a parser does not skip the number it cannot read -- it abandons
// the WHOLE document. metadata.json carries one treasury per country and two
// compass axes per country, so a single non-finite value among two hundred
// would make turn_count unreadable and take the save's entire history with it.
//
// No save in data/saves/ (439 of them) contains one, so this is a guard and
// not a cure: it exists so that a number going bad upstream costs a wrong
// number rather than a lost world. If it ever fires, the save survives to say
// so.
static std::string jnum(double v) {
    if (!std::isfinite(v)) return "0";
    return std::to_string(v);
}

// ── ONE WRITER FOR metadata.json ──
//
// This document used to be written by hand in five places -- createSave,
// appendTurn, updateLastPlayed, updatePlayerCountry and writeState -- each a
// copy of the others. A key added to four of them is silently dropped the next
// time the fifth rewrites the file, which is exactly the shape of change fog of
// war needed. So there is one writer now.
//
// Strings go through the JSON encoder: the save name used to be pasted in raw,
// so a world named with a quote or a backslash wrote a file the parser threw
// on, and the save then listed with no name, no turn count and no country.
static std::string metadataJson(const SaveMetadata& meta) {
    const std::string cr = meta.created.empty() ? "unknown" : meta.created;
    const std::string lp = meta.lastPlayed.empty() ? cr : meta.lastPlayed;
    const std::string ver = meta.version.empty() ? "unknown" : meta.version;
    auto str = [](const std::string& s) { return nlohmann::json(s).dump(); };

    std::string o = "{\n";
    o += "  \"save_name\": " + str(meta.saveName) + ",\n";
    o += "  \"version\": " + str(ver) + ",\n";
    o += "  \"created\": " + str(cr) + ",\n";
    o += "  \"last_played\": " + str(lp) + ",\n";
    o += "  \"turn_count\": " + std::to_string(meta.turnCount) + ",\n";
    o += "  \"province_count\": " + std::to_string(meta.provinceCount) + ",\n";
    o += "  \"ship_count\": " + std::to_string(meta.shipCount) + ",\n";
    o += "  \"player_country_id\": " + std::to_string(meta.playerCountryId) + ",\n";
    o += "  \"format\": " + std::to_string(kSaveFormat) + ",\n";
    o += std::string("  \"fog_of_war\": ") + (meta.fogOfWar ? "true" : "false") + ",\n";
    if (!meta.sealSalt.empty())
        o += "  \"seal\": { \"v\": 1, \"salt\": " + str(meta.sealSalt) + " },\n";
    o += "  \"compass_convention\": 2,\n  \"country_compasses\": {\n";
    bool first = true;
    for (auto& [cid, pc] : meta.countryCompasses) {
        if (!first) o += ",\n";
        first = false;
        o += "    \"" + std::to_string(cid) + "\": { \"economic\": " + jnum(pc.economic) +
             ", \"social\": " + jnum(pc.social) + " }";
    }
    o += "\n  },\n";
    o += "  \"country_treasuries\": {\n";
    first = true;
    // A sealed save keeps its treasuries in fog/treasuries.json, sealed: what
    // every country holds is exactly what fog hides. See sealedTreasuries().
    if (meta.sealSalt.empty())
    for (auto& [cid, tr] : meta.countryTreasuries) {
        if (!first) o += ",\n";
        first = false;
        o += "    \"" + std::to_string(cid) + "\": " + jnum(tr);
    }
    o += "\n  }\n";
    o += "}\n";
    return o;
}

// ── THE ARCHIVE IS THE TRUTH; turn_count IS A CACHE OF IT ──
//
// The turns are in the file. turn_count is a separate number, written by five
// rewrite paths, and when the two disagree the LOW one used to win in silence:
// the replay walks 1..turn_count, so a count that reads short does not report
// a damaged save, it reports a shorter game. Every turn after it is still in
// the archive, and the world simply comes back younger -- with state.json, and
// so the politics and research, entirely intact, because those are extracted
// raw and never pass through this counter.
//
// That is the shape of the "saving resets my world" report. Deriving the
// count from the entries makes it unrepresentable rather than merely unlikely,
// and it repairs a save already in that state the next time one is read.
static int highestTurnInBuffer(const std::vector<uint8_t>& zipData);
static bool openSaveMem(mz_zip_archive& zip, const std::vector<uint8_t>& data);

static int highestTurnIn(mz_zip_archive& zip) {
    int highest = 0;
    const int fc = (int)mz_zip_reader_get_num_files(&zip);
    // The name alone. mz_zip_reader_file_stat also turns each entry's DOS
    // timestamp into a time_t through mktime(), which is slow, and this runs
    // over every entry of the save on every turn.
    char name[64];
    for (int i = 0; i < fc; ++i) {
        if (!mz_zip_reader_get_filename(&zip, (mz_uint)i, name, sizeof(name))) continue;
        int n = 0;
        if (std::sscanf(name, "turns/t_%d.dat", &n) == 1 && n > highest)
            highest = n;
    }
    return highest;
}

static int highestTurnInBuffer(const std::vector<uint8_t>& zipData) {
    if (zipData.empty()) return 0;
    mz_zip_archive zip{};
    if (!openSaveMem(zip, zipData)) return 0;
    const int n = highestTurnIn(zip);
    mz_zip_reader_end(&zip);
    return n;
}

std::vector<uint8_t> SaveManager::packTurn(const TurnDelta& delta) {
    std::vector<uint8_t> buf;
    // Header
    writeU16(buf, (uint16_t)delta.turnNumber);
    writeU16(buf, (uint16_t)delta.provinces.size());
    writeU16(buf, (uint16_t)delta.ships.size());
    writeU16(buf, (uint16_t)delta.armies.size());

    // Province entries
    for (auto& p : delta.provinces) {
        writeU16(buf, (uint16_t)p.provinceId);
        uint16_t mask = 0;
        if (p.ownerChanged)           mask |= 1 << 0;
        if (p.populationChanged)      mask |= 1 << 1;
        if (p.industryLevelChanged)   mask |= 1 << 2;
        if (p.fortificationChanged)   mask |= 1 << 3;
        if (p.incomeChanged)          mask |= 1 << 4;
        if (p.resourceIncomeChanged)  mask |= 1 << 5;
        if (p.popIncomeChanged)       mask |= 1 << 6;
        if (p.popModifierChanged)     mask |= 1 << 7;
        if (p.outputChanged)          mask |= 1 << 8;
        writeU16(buf, mask);
        if (p.ownerChanged)           writeU16(buf, (uint16_t)p.newOwner);
        if (p.populationChanged)      writeU32(buf, popSaturated(p.newPopulation));
        if (p.industryLevelChanged)   buf.push_back((uint8_t)p.newIndustryLevel);
        if (p.fortificationChanged)   buf.push_back((uint8_t)p.newFortification);
        if (p.incomeChanged)          writeFloat(buf, p.newIncome);
        if (p.resourceIncomeChanged)  writeFloat(buf, p.newResourceIncome);
        if (p.popIncomeChanged)       writeFloat(buf, p.newPopIncome);
        if (p.popModifierChanged)     writeFloat(buf, p.newPopModifier);
        // int8, not uint8: -1 means undirected and is the default.
        if (p.outputChanged)          buf.push_back((uint8_t)(int8_t)p.newOutput);
    }

    // Ship entries
    for (auto& s : delta.ships) {
        writeU16(buf, (uint16_t)s.shipIndex);
        uint8_t mask = 0;
        if (s.latChanged)    mask |= 1 << 0;
        if (s.lonChanged)    mask |= 1 << 1;
        if (s.healthChanged) mask |= 1 << 2;
        if (s.crewChanged)   mask |= 1 << 3;
        if (s.countryIdChanged) mask |= 1 << 4;
        buf.push_back(mask);
        if (s.latChanged)    writeFloat(buf, (float)s.newLat);
        if (s.lonChanged)    writeFloat(buf, (float)s.newLon);
        if (s.healthChanged) buf.push_back((uint8_t)s.newHealth);
        if (s.crewChanged)   writeU16(buf, (uint16_t)s.newCrew);
        if (s.countryIdChanged)  writeU16(buf, (uint16_t)s.newCountryId);
    }

    // Army entries
    for (auto& a : delta.armies) {
        writeU16(buf, (uint16_t)a.provinceId);
        buf.push_back((uint8_t)a.units.size());
        for (auto& u : a.units) {
            writeU16(buf, (uint16_t)u.countryId);
            writeU32(buf, (uint32_t)u.count);
        }
    }

    // Populations too large for the province field above. Written here, in the
    // trailer, rather than beside the field they correct: a build that has
    // never heard of them stops reading at the end of the research block and
    // still walks the province stream at the right offsets. Inside the stream
    // the same four bytes would shift every entry after them.
    std::vector<const ProvinceDelta*> widePops;
    for (auto& p : delta.provinces)
        if (p.populationChanged && popNeedsWideField(p.newPopulation))
            widePops.push_back(&p);

    // ── WHAT THE ARMIES ARE MADE OF ──
    //
    // Only provinces holding something other than plain line infantry are
    // written. A world that has not researched another type writes NOTHING
    // here, so the save is byte-for-byte what the previous build produced --
    // which is the acceptance test for introducing types at all.
    //
    // In the trailer for the same reason the wide populations are: the army
    // block above is fixed-width with no version field, and a type byte inside
    // it would shift every entry after it. Here, a build that has never heard
    // of troop types stops at the end of the block it knows and still reads the
    // right totals -- it sees an army of the right size, made of the only kind
    // of soldier it can imagine.
    std::vector<const ArmyDelta*> typedArmies;
    for (auto& a : delta.armies) {
        for (auto& u : a.units)
            if (u.type != 0) { typedArmies.push_back(&a); break; }
    }

    uint8_t extraFlags = 0x01;                          // bit 0 = research state
    if (!widePops.empty()) extraFlags |= 0x02;          // bit 1 = wide populations
    if (!typedArmies.empty()) extraFlags |= 0x04;       // bit 2 = troop types
    // ONLY WHEN IT SAYS SOMETHING. A country running one programme on the
    // default share, which is every country in every save that predates groups
    // and most turns of every one that does not, writes nothing here -- so the
    // file stays byte-for-byte what the previous build produced. That is the
    // acceptance test the troop-type trailer set, and it is the right one: a
    // feature nobody has used must not change a single byte.
    bool anyGroups = false;
    for (int g = 1; g < 3; ++g)
        anyGroups |= (delta.researchGroups[g].activeNode >= 0);
    for (int g = 0; g < 3; ++g)
        anyGroups |= delta.researchGroups[g].autoAdvance ||
                     delta.researchGroups[g].sharePct != 50 ||
                     delta.researchGroups[g].lastNode >= 0;
    if (anyGroups) extraFlags |= 0x08;                  // bit 3 = research groups
    if (!delta.mapEdits.empty()) extraFlags |= 0x10;    // bit 4 = map edits (morph/terrain)
    buf.push_back(extraFlags);
    writeFloat(buf, delta.researchAllocation);
    writeFloat(buf, delta.pacificationAllocation);
    writeU16(buf, (uint16_t)(delta.researchActiveNode + 1)); // -1 → 0 offset
    writeU16(buf, (uint16_t)delta.researchPoints);
    if (!widePops.empty()) {
        writeU16(buf, (uint16_t)widePops.size());
        for (const ProvinceDelta* p : widePops) {
            writeU16(buf, (uint16_t)p->provinceId);
            writeU64(buf, (uint64_t)p->newPopulation);
        }
    }
    if (!typedArmies.empty()) {
        writeU16(buf, (uint16_t)typedArmies.size());
        for (const ArmyDelta* a : typedArmies) {
            writeU16(buf, (uint16_t)a->provinceId);
            buf.push_back((uint8_t)a->units.size());
            for (auto& u : a->units) {
                writeU16(buf, (uint16_t)u.countryId);
                buf.push_back(u.type);
                writeU32(buf, (uint32_t)u.count);
            }
        }
    }
    // ── RESEARCH GROUPS ──
    //
    // Written last so every field before it keeps its offset, and behind its
    // own flag bit so a save from before groups existed simply does not carry
    // it. Group 1 is NOT repeated here -- it is researchActiveNode, up in the
    // fixed part, and duplicating it would be two homes for one fact.
    if (anyGroups) {
        for (int g = 0; g < 3; ++g) {
            writeU16(buf, (uint16_t)(delta.researchGroups[g].activeNode + 1));
            writeU16(buf, (uint16_t)(delta.researchGroups[g].lastNode + 1));
            buf.push_back((uint8_t)std::clamp(delta.researchGroups[g].sharePct, 0, 100));
            buf.push_back(delta.researchGroups[g].autoAdvance ? 1 : 0);
        }
    }

    // ── MAP EDITS (live morph / terrain) ──
    //
    // Last of the trailer blocks, behind bit 4, so every fixed offset before it
    // is untouched and an older build stops at the end marker without ever
    // seeing it. Runs are row-major pixel spans; see MapEditDelta.
    if (!delta.mapEdits.empty()) {
        writeU16(buf, (uint16_t)std::min<size_t>(delta.mapEdits.size(), 0xFFFF));
        size_t emitted = 0;
        for (const auto& e : delta.mapEdits) {
            if (emitted++ >= 0xFFFF) break;
            buf.push_back(e.kind);
            writeU16(buf, (uint16_t)e.target);
            writeU32(buf, (uint32_t)e.runs.size());
            for (const auto& r : e.runs) { writeU32(buf, r.first); writeU32(buf, r.second); }
        }
    }

    buf.push_back(0xFF); // end marker

    return buf;
}

// ─── Unpack binary to TurnDelta ──────────────────────────

static uint16_t readU16(const uint8_t*& ptr) {
    uint16_t v = (uint16_t)(ptr[0] | ((uint16_t)ptr[1] << 8));
    ptr += 2; return v;
}
static uint32_t readU32(const uint8_t*& ptr) {
    uint32_t v = (uint32_t)(ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16) | ((uint32_t)ptr[3] << 24));
    ptr += 4; return v;
}
static uint64_t readU64(const uint8_t*& ptr) {
    uint64_t lo = readU32(ptr);
    uint64_t hi = readU32(ptr);
    return lo | (hi << 32);
}
static float readFloat(const uint8_t*& ptr) {
    uint32_t bits = readU32(ptr);
    float v; memcpy(&v, &bits, sizeof(v)); return v;
}

bool SaveManager::unpackTurn(const uint8_t* data, size_t size, TurnDelta& out) {
    const uint8_t* end = data + size;
    const uint8_t* ptr = data;
    if (size < 8) return false;

    out.turnNumber = readU16(ptr);
    uint16_t provCount = readU16(ptr);
    uint16_t shipCount = readU16(ptr);
    uint16_t armyCount = readU16(ptr);

    out.provinces.resize(provCount);
    for (int i = 0; i < provCount; ++i) {
        auto& p = out.provinces[i];
        if (ptr + 4 > end) return false;
        p.provinceId = readU16(ptr);
        uint16_t mask = readU16(ptr);
        p.ownerChanged          = (mask >> 0) & 1; if (p.ownerChanged)          { if (ptr + 2 > end) return false; p.newOwner = readU16(ptr); }
        p.populationChanged     = (mask >> 1) & 1; if (p.populationChanged)     { if (ptr + 4 > end) return false; p.newPopulation = readU32(ptr); }
        p.industryLevelChanged  = (mask >> 2) & 1; if (p.industryLevelChanged)  { if (ptr + 1 > end) return false; p.newIndustryLevel = *ptr++; }
        p.fortificationChanged  = (mask >> 3) & 1; if (p.fortificationChanged)  { if (ptr + 1 > end) return false; p.newFortification = *ptr++; }
        p.incomeChanged         = (mask >> 4) & 1; if (p.incomeChanged)         { if (ptr + 4 > end) return false; p.newIncome = readFloat(ptr); }
        p.resourceIncomeChanged = (mask >> 5) & 1; if (p.resourceIncomeChanged) { if (ptr + 4 > end) return false; p.newResourceIncome = readFloat(ptr); }
        p.popIncomeChanged      = (mask >> 6) & 1; if (p.popIncomeChanged)      { if (ptr + 4 > end) return false; p.newPopIncome = readFloat(ptr); }
        p.popModifierChanged    = (mask >> 7) & 1; if (p.popModifierChanged)    { if (ptr + 4 > end) return false; p.newPopModifier = readFloat(ptr); }
        p.outputChanged         = (mask >> 8) & 1; if (p.outputChanged)         { if (ptr + 1 > end) return false; p.newOutput = (int)(int8_t)*ptr++; }
    }

    out.ships.resize(shipCount);
    for (int i = 0; i < shipCount; ++i) {
        auto& s = out.ships[i];
        if (ptr + 3 > end) return false;
        s.shipIndex = readU16(ptr);
        uint8_t mask = *ptr++;
        s.latChanged    = (mask >> 0) & 1; if (s.latChanged)    { if (ptr + 4 > end) return false; s.newLat = readFloat(ptr); }
        s.lonChanged    = (mask >> 1) & 1; if (s.lonChanged)    { if (ptr + 4 > end) return false; s.newLon = readFloat(ptr); }
        s.healthChanged = (mask >> 2) & 1; if (s.healthChanged) { if (ptr + 1 > end) return false; s.newHealth = *ptr++; }
        s.crewChanged   = (mask >> 3) & 1; if (s.crewChanged)   { if (ptr + 2 > end) return false; s.newCrew = readU16(ptr); }
        s.countryIdChanged = (mask >> 4) & 1; if (s.countryIdChanged) { if (ptr + 2 > end) return false; s.newCountryId = readU16(ptr); }
    }

    out.armies.resize(armyCount);
    for (int i = 0; i < armyCount; ++i) {
        auto& a = out.armies[i];
        if (ptr + 3 > end) return false;
        a.provinceId = readU16(ptr);
        uint8_t unitCount = *ptr++;
        a.units.resize(unitCount);
        for (int j = 0; j < unitCount; ++j) {
            if (ptr + 6 > end) return false;
            a.units[j].countryId = readU16(ptr);
            a.units[j].count = (int)readU32(ptr);
        }
    }

    // Trailer (optional, backward-compatible with old saves)
    if (ptr + 1 <= end) {
        uint8_t extraFlags = *ptr++;
        bool trailerIntact = true;
        // Research/pacification state.
        //
        // 12, not the 11 this used to check: the four fields below are
        // 4+4+2+2 bytes. Well-formed data always had a thirteenth byte (the end
        // marker) so the short guard never showed up on a real save -- but this
        // same decoder parses turns arriving off a socket, where a packet cut
        // to exactly 11 trailing bytes read one past its end.
        if (extraFlags & 1) {
            if (ptr + 12 <= end) {
                out.researchAllocation = readFloat(ptr);
                out.pacificationAllocation = readFloat(ptr);
                int activeRaw = (int)readU16(ptr);
                out.researchActiveNode = activeRaw - 1; // undo +1 offset
                out.researchPoints = (int)readU16(ptr);
            } else {
                // Nothing after this is at a known offset any more.
                trailerIntact = false;
            }
        }
        // Populations that did not fit the province field, at full width.
        if (trailerIntact && (extraFlags & 2) && ptr + 2 <= end) {
            uint16_t wideCount = readU16(ptr);
            for (uint16_t i = 0; i < wideCount; ++i) {
                if (ptr + 10 > end) return false;
                int pid = (int)readU16(ptr);
                long long pop = (long long)readU64(ptr);
                for (auto& p : out.provinces)
                    if (p.provinceId == pid && p.populationChanged) {
                        p.newPopulation = pop;
                        break;
                    }
            }
        }
        // What the armies are made of. REPLACES that province's unit list,
        // because the fixed-width block above could only carry one entry per
        // country and a mixed army needs one per (country, type) -- the totals
        // up there are the same army seen by a build that cannot tell the
        // difference. Absent for any world that is all line infantry, and
        // absent from every save written before types existed, both of which
        // load as an army of line infantry, correctly.
        if (trailerIntact && (extraFlags & 4) && ptr + 2 <= end) {
            uint16_t typedCount = readU16(ptr);
            for (uint16_t i = 0; i < typedCount; ++i) {
                if (ptr + 3 > end) return false;
                const int pid = (int)readU16(ptr);
                const uint8_t unitCount = *ptr++;
                std::vector<ArmyDelta::Unit> units;
                units.reserve(unitCount);
                for (uint8_t j = 0; j < unitCount; ++j) {
                    if (ptr + 7 > end) return false;
                    ArmyDelta::Unit u;
                    u.countryId = (int)readU16(ptr);
                    u.type = *ptr++;
                    u.count = (int)readU32(ptr);
                    units.push_back(u);
                }
                for (auto& a : out.armies)
                    if (a.provinceId == pid) { a.units = std::move(units); break; }
            }
        }
        if (trailerIntact && (extraFlags & 8)) {
            for (int g = 0; g < 3; ++g) {
                if (ptr + 6 > end) return false;   // refused, not half-read
                out.researchGroups[g].activeNode  = (int)readU16(ptr) - 1;
                out.researchGroups[g].lastNode    = (int)readU16(ptr) - 1;
                out.researchGroups[g].sharePct    = (int)*ptr++;
                out.researchGroups[g].autoAdvance = (*ptr++ != 0);
            }
        }
        // Live geometry edits (morph / terrain), as row-major pixel runs.
        if (trailerIntact && (extraFlags & 0x10) && ptr + 2 <= end) {
            const uint16_t editCount = readU16(ptr);
            for (uint16_t i = 0; i < editCount; ++i) {
                if (ptr + 7 > end) return false;        // kind(1)+target(2)+runs(4)
                MapEditDelta e;
                e.kind = *ptr++;
                e.target = (int)readU16(ptr);
                const uint32_t runCount = readU32(ptr);
                e.runs.reserve(runCount);
                for (uint32_t r = 0; r < runCount; ++r) {
                    if (ptr + 8 > end) return false;
                    const uint32_t start = readU32(ptr);
                    const uint32_t len = readU32(ptr);
                    e.runs.emplace_back(start, len);
                }
                out.mapEdits.push_back(std::move(e));
            }
        }
    }

    return true;
}

// ─── Read entire file into memory ────────────────────────

static std::vector<uint8_t> readFile(const std::string& path) {
    // stdio, NOT std::ifstream. Saves are written during the map load, which
    // yields to the browser through Asyncify; coming back in, constructing a
    // basic_filebuf traps the module with a bare "null function" and the world
    // never finishes loading. See src/util/LoadLog.h for the same reasoning.
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    if (sz < 0) { std::fclose(f); return {}; }
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data((size_t)sz);
    const size_t got = sz ? std::fread(data.data(), 1, (size_t)sz, f) : 0;
    std::fclose(f);
    if (got != (size_t)sz) return {};
    return data;
}

// ─── Open a save without reading it all ─────────────────
//
// A save grows by a state snapshot every turn -- 100 MB is an ordinary size
// after a long campaign -- and every reader used to pull the whole file into
// memory to find one entry in it. appendTurn did that twice a turn (once
// itself, once through readMetadata): 213 MB allocated and freed per turn,
// which macOS then kept charged to the game as freed-but-held memory, 1.7 GB
// of it by turn ten on a 100 MB save.
//
// miniz can read the archive from the file instead: the central directory,
// then only the entries asked for, in chunks. Still stdio underneath (see
// readFile for why that matters on the web).
// THE INTACT PART OF A SAVE WHOSE LAST APPEND DID NOT FINISH. A turn is
// appended after the end of the archive (see appendInPlace), so a process that
// dies mid-append leaves the previous archive whole and some bytes after it.
// This finds where that archive ends: just past the last end-of-directory
// record whose directory sits directly before it. 0 when there is none.
static size_t intactArchiveEnd(const unsigned char* d, size_t n) {
    if (n < 22) return 0;
    auto u16 = [&](size_t at) { return (uint32_t)d[at] | (uint32_t)d[at + 1] << 8; };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    for (size_t p = n - 22 + 1; p-- > 0;) {
        if (d[p] != 0x50 || d[p + 1] != 0x4b || d[p + 2] != 0x05 || d[p + 3] != 0x06) continue;
        const uint64_t cdSize = u32(p + 12), cdOfs = u32(p + 16), entries = u16(p + 10);
        const size_t end = p + 22 + u16(p + 20);
        if (cdOfs + cdSize != p || end > n) continue;
        if (entries > 0 && (cdOfs + 4 > n || u32((size_t)cdOfs) != 0x02014b50)) continue;
        return end;
    }
    return 0;
}

static bool openSaveFile(mz_zip_archive& zip, const std::string& path) {
    zip = mz_zip_archive{};
    if (mz_zip_reader_init_file(&zip, path.c_str(), 0)) return true;
    // Only on failure: read it whole and look for the archive inside. Opened
    // as that prefix, never truncated here -- reading a save does not write it.
    const std::vector<uint8_t> data = readFile(path);
    const size_t end = intactArchiveEnd(data.data(), data.size());
    if (end == 0 || end >= data.size()) return false;
    zip = mz_zip_archive{};
    if (!mz_zip_reader_init_file_v2(&zip, path.c_str(), 0, 0, end)) return false;
    fprintf(stderr, "[SAVE] %s: the last write did not finish; reading the save as it was before it "
                    "(%zu stray bytes ignored)\n", path.c_str(), data.size() - end);
    return true;
}

// The same, for a save already read into memory.
static bool openSaveMem(mz_zip_archive& zip, const std::vector<uint8_t>& data) {
    zip = mz_zip_archive{};
    if (mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) return true;
    const size_t end = intactArchiveEnd(data.data(), data.size());
    zip = mz_zip_archive{};
    return end > 0 && end < data.size() && mz_zip_reader_init_mem(&zip, data.data(), end, 0);
}

// ─── Sealed entries (fog-of-war saves) ──────────────────
//
// A fog-of-war save holds the whole truth -- every army the player could not
// see -- and a .odsv is a zip. So in a world with fog the entries that carry
// that truth are sealed with odseal's od_p2 (ChaCha20-Poly1305, keyed from a
// key inside the sealed archive and the save's own salt):
//
//     "ODSL" | u8 version | 12-byte nonce | ciphertext | 16-byte tag
//
// Version 2 deflates the plaintext before sealing it (ciphertext does not
// compress, so sealing first made a fog save four times the size); version 1
// is the same without the deflate and is still read.
//
// Readers never see this: extractEntry() opens a sealed entry on the way out,
// so every reader in the game gets plaintext and none of them was changed.
// Writers seal through sealFor(), one choke point.
//
// The nonce is a hash of the entry name and its plaintext, so a save written
// twice from the same state is byte-identical (the determinism checks compare
// saves), and two different plaintexts never share a nonce under one key.
//
// What it is worth: unzip, a text editor and tools/read_odsv.py no longer read
// the truth. A program that calls od_p3, a debugger or a rebuilt client still
// can -- see docs/design/fog-of-war-plan.md. Real secrecy is multiplayer's.
namespace {

constexpr char kSealMagic[4] = {'O', 'D', 'S', 'L'};
constexpr size_t kSealHeader = 4 + 1 + 12;

bool sealedName(const char* name) {
    return strcmp(name, "state.json") == 0 || strcmp(name, "rebels.json") == 0 ||
           strncmp(name, "turns/", 6) == 0 || strncmp(name, "rebellion/", 10) == 0 ||
           strncmp(name, "fog/", 4) == 0;
}

bool isSealed(const std::string& data) {
    return data.size() >= kSealHeader + 16 && memcmp(data.data(), kSealMagic, 4) == 0;
}

void sealNonce(const char* name, const void* data, size_t n, unsigned char out[12]) {
    uint64_t a = 1469598103934665603ull, b = 0x9E3779B97F4A7C15ull;
    auto eat = [&](unsigned char c) {
        a ^= c; a *= 1099511628211ull;
        b ^= c; b = (b << 13 | b >> 51) * 0xBF58476D1CE4E5B9ull;
    };
    for (const char* p = name; *p; ++p) eat((unsigned char)*p);
    eat(0);
    const unsigned char* d = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) eat(d[i]);
    for (int i = 0; i < 8; ++i) out[i] = (unsigned char)(a >> (8 * i));
    for (int i = 0; i < 4; ++i) out[8 + i] = (unsigned char)(b >> (8 * i));
}

}  // namespace

bool SaveManager::canSeal() {
#ifdef OD_HAS_P2
    return true;
#else
    return false;
#endif
}

std::string SaveManager::newSealSalt() {
    if (!canSeal()) return {};
    std::random_device rd;
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 32; ++i) s += hex[rd() & 15];
    return s;
}

// ─── Compression ─────────────────────────────────────────
//
// libdeflate compresses and inflates; miniz only lays out the zip around it. On
// a late-game state.json libdeflate is 2.8x faster than miniz at level 6 and 3%
// smaller, inflates 3.4x faster, and its CRC-32 runs on the CPU's instructions
// rather than a table. What it writes is ordinary deflate, so a save written
// here opens in every older build, and an older save opens here.

// One compressor per level per thread: they are not thread-safe, and building
// one costs more than compressing a small entry.
static libdeflate_compressor* compressorFor(int level) {
    struct Cache {
        libdeflate_compressor* c[13] = {};
        ~Cache() { for (auto* p : c) if (p) libdeflate_free_compressor(p); }
    };
    thread_local Cache cache;
    level = level < 1 ? 1 : level > 12 ? 12 : level;
    if (!cache.c[level]) cache.c[level] = libdeflate_alloc_compressor(level);
    return cache.c[level];
}

static libdeflate_decompressor* decompressor() {
    struct Holder {
        libdeflate_decompressor* d = libdeflate_alloc_decompressor();
        ~Holder() { if (d) libdeflate_free_decompressor(d); }
    };
    thread_local Holder h;
    return h.d;
}

static uint32_t crc32Of(const void* data, size_t n) {
    return libdeflate_crc32(0, data, n);
}

// An entry deflated once, to be added under one name or several.
struct Deflated {
    std::string bytes;      // raw deflate; empty when it did not pay
    uint32_t crc = 0;
};

static Deflated deflateEntry(const void* data, size_t n, mz_uint level) {
    Deflated out;
    // Tiny or incompressible entries are left to miniz, which stores them.
    if (level == 0 || n < 64) return out;
    libdeflate_compressor* c = compressorFor((int)level);
    if (!c) return out;
    // Room for one byte less than the input: an entry that does not shrink
    // fails to fit, and is stored.
    out.bytes.resize(n - 1);
    const size_t got = libdeflate_deflate_compress(c, data, n, &out.bytes[0], out.bytes.size());
    if (got == 0) { out.bytes.clear(); return out; }
    out.bytes.resize(got);
    out.crc = crc32Of(data, n);
    return out;
}

static mz_bool zipAddDeflated(mz_zip_archive* z, const char* name, const Deflated& d,
                              const void* data, size_t n, mz_uint level) {
    if (d.bytes.empty()) return mz_zip_writer_add_mem(z, name, data, n, level);
    return mz_zip_writer_add_mem_ex(z, name, d.bytes.data(), d.bytes.size(), nullptr, 0,
                                    level | MZ_ZIP_FLAG_COMPRESSED_DATA, n, d.crc);
}

// mz_zip_writer_add_mem, compressed by libdeflate.
static mz_bool zipAdd(mz_zip_archive* z, const char* name, const void* data, size_t n, mz_uint level) {
    return zipAddDeflated(z, name, deflateEntry(data, n, level), data, n, level);
}

// mz_zip_reader_extract_to_heap, inflated by libdeflate and checked against the
// entry's CRC. Anything that is not plain deflate goes to miniz.
static std::string zipExtract(mz_zip_archive& zip, mz_uint idx, bool& ok) {
    ok = false;
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zip, idx, &st)) return {};
    if (st.m_method != MZ_DEFLATED || st.m_uncomp_size > ((mz_uint64)1 << 31) || st.m_is_encrypted) {
        size_t sz = 0;
        void* d = mz_zip_reader_extract_to_heap(&zip, idx, &sz, 0);
        if (!d) return {};
        std::string out((const char*)d, sz);
        mz_free(d);
        ok = true;
        return out;
    }
    size_t rawLen = 0;
    void* raw = mz_zip_reader_extract_to_heap(&zip, idx, &rawLen, MZ_ZIP_FLAG_COMPRESSED_DATA);
    if (!raw) return {};
    std::string out((size_t)st.m_uncomp_size, '\0');
    size_t got = 0;
    libdeflate_decompressor* dd = decompressor();
    const bool inflated = dd && libdeflate_deflate_decompress(dd, raw, rawLen, out.empty() ? nullptr : &out[0],
                                                              out.size(), &got) == LIBDEFLATE_SUCCESS;
    mz_free(raw);
    if (!inflated || got != out.size() || crc32Of(out.data(), out.size()) != st.m_crc32) return {};
    ok = true;
    return out;
}

// The bytes to write for entry `name`: sealed when the save is, plain otherwise.
static std::string sealFor(const SaveMetadata& meta, const char* name, const void* data, size_t n) {
    std::string plain((const char*)data, n);
#ifdef OD_HAS_P2
    if (meta.sealSalt.empty() || !sealedName(name)) return plain;
    // u64 plain length, then the deflated bytes.
    libdeflate_compressor* zc = compressorFor(MZ_BEST_COMPRESSION);
    if (!zc) return plain;
    std::string packed(8 + libdeflate_zlib_compress_bound(zc, n), '\0');
    for (int i = 0; i < 8; ++i) packed[(size_t)i] = (char)((uint64_t)n >> (8 * i));
    const size_t zlen = libdeflate_zlib_compress(zc, data, n, &packed[8], packed.size() - 8);
    if (zlen == 0) return plain;
    packed.resize(8 + zlen);
    std::string out(kSealHeader + packed.size() + 16, '\0');
    memcpy(&out[0], kSealMagic, 4);
    out[4] = 2;
    unsigned char nonce[12];
    sealNonce(name, data, n, nonce);
    memcpy(&out[5], nonce, 12);
    if (!od_p2((const unsigned char*)meta.sealSalt.data(), meta.sealSalt.size(), nonce,
               (const unsigned char*)packed.data(), packed.size(), (unsigned char*)&out[kSealHeader]))
        return plain;
    return out;
#else
    (void)meta; (void)name;
    return plain;
#endif
}

static SaveMetadata parseMetadataJson(const std::string& js);

// One entry of an open archive, exactly as stored.
static std::string extractRaw(mz_zip_archive& zip, const char* name) {
    const int idx = mz_zip_reader_locate_file(&zip, name, nullptr, 0);
    if (idx < 0) return {};
    bool ok = false;
    return zipExtract(zip, (mz_uint)idx, ok);
}

// One entry of an open archive, or empty when it is absent or unreadable --
// opened if it is sealed. A sealed entry this build cannot open (no seal in
// the linked odseal, a wrong salt, a tampered byte) reads as absent, and says
// so once on stderr: a tampered save is a damaged save, not a readable one.
static std::string extractEntry(mz_zip_archive& zip, const char* name) {
    std::string data = extractRaw(zip, name);
    if (!isSealed(data)) return data;
#ifdef OD_HAS_P2
    const SaveMetadata meta = parseMetadataJson(extractRaw(zip, "metadata.json"));
    if (!meta.sealSalt.empty() && (data[4] == 1 || data[4] == 2)) {
        const unsigned char* nonce = (const unsigned char*)&data[5];
        std::string out(data.size() - kSealHeader - 16, '\0');
        if (od_p3((const unsigned char*)meta.sealSalt.data(), meta.sealSalt.size(), nonce,
                  (const unsigned char*)&data[kSealHeader], data.size() - kSealHeader,
                  out.empty() ? nullptr : (unsigned char*)&out[0])) {
            if (data[4] == 1) return out;
            // Version 2: the length, then deflate. A length past what the
            // archive could hold is refused rather than allocated.
            if (out.size() >= 8) {
                uint64_t n = 0;
                for (int i = 0; i < 8; ++i) n |= (uint64_t)(unsigned char)out[(size_t)i] << (8 * i);
                if (n <= (uint64_t)1 << 31) {
                    std::string plain((size_t)n, '\0');
                    size_t got = 0;
                    if (libdeflate_zlib_decompress(decompressor(), &out[8], out.size() - 8,
                                                   plain.empty() ? nullptr : &plain[0], plain.size(),
                                                   &got) == LIBDEFLATE_SUCCESS &&
                        got == (size_t)n)
                        return plain;
                }
            }
        }
    }
    fprintf(stderr, "[SAVE] %s is sealed and does not open (damaged or edited)\n", name);
#else
    fprintf(stderr, "[SAVE] %s is sealed; this build cannot open fog-of-war saves\n", name);
#endif
    return {};
}

// The treasuries of a sealed save, which its metadata no longer carries.
static void readSealedTreasuries(mz_zip_archive& zip, SaveMetadata& meta) {
    if (meta.sealSalt.empty()) return;
    const std::string js = extractEntry(zip, "fog/treasuries.json");
    if (js.empty()) return;
    try {
        auto j = nlohmann::json::parse(js);
        for (auto& [cidStr, tr] : j.items())
            if (tr.is_number()) meta.countryTreasuries[std::stoi(cidStr)] = tr.get<double>();
    } catch (...) {}
}

static void writeSealedTreasuries(mz_zip_archive& zip, const SaveMetadata& meta) {
    if (meta.sealSalt.empty()) return;
    nlohmann::json j = nlohmann::json::object();
    for (auto& [cid, tr] : meta.countryTreasuries) j[std::to_string(cid)] = tr;
    const std::string plain = j.dump();
    const std::string data = sealFor(meta, "fog/treasuries.json", plain.data(), plain.size());
    zipAdd(&zip, "fog/treasuries.json", data.data(), data.size(), MZ_BEST_COMPRESSION);
}

// ─── Per-turn state as a diff ───────────────────────────
//
// EVERY TURN KEPT A WHOLE COPY OF THE GAME STATE. turns/s_NNNNN.json pins the
// state at the end of a turn so that rewinding to it can restore orders,
// policies and research, which the binary delta does not carry. Each copy is
// ~750 KB of JSON and ~115 KB stored, and consecutive copies are nearly the
// same bytes: in an 820-turn campaign they were 94 MB of a 102 MB save, and
// because the save is carried across whole every turn, also most of what a
// turn's save cost.
//
// So most turns now store turns/sd_NNNNN.bin, the state as a diff against the
// turn before, with a full s_NNNNN.json every kStateKeyframe turns so that
// rebuilding any turn reads at most that many entries:
//
//     "ODSD" | u8 version 1 | u32 base turn | u32 base length | u32 base crc32
//            | u32 length   | u32 crc32     | ops (savedelta::encode)
//
// The checksums are of the base and of the result, so a chain that does not
// rebuild EXACTLY reads as no snapshot -- the answer an older save already
// gets -- never as a different state. state.json, which loading resumes from,
// stays whole. Old saves keep their full snapshots and read as before.
namespace savedelta {

namespace {

constexpr size_t kBlock = 32;    // shortest copy worth finding by hash
constexpr size_t kStride = 8;    // base positions indexed: a copy of kBlock + kStride is always found
constexpr uint64_t kMul = 0x100000001B3ull;

uint64_t blockHash(const unsigned char* p) {
    uint64_t h = 0;
    for (size_t i = 0; i < kBlock; ++i) h = h * kMul + p[i];
    return h;
}

void putVarint(std::string& out, uint64_t v) {
    while (v >= 0x80) { out += (char)(0x80 | (v & 0x7F)); v >>= 7; }
    out += (char)v;
}

bool getVarint(const unsigned char*& p, const unsigned char* end, uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (p >= end) return false;
        const unsigned char c = *p++;
        v |= (uint64_t)(c & 0x7F) << shift;
        if (!(c & 0x80)) return true;
    }
    return false;
}

}  // namespace

// Ops, each a varint of (length << 1 | kind):
//   kind 0, ADD:  `length` literal bytes follow
//   kind 1, COPY: a zigzag varint follows -- where in the base the copy starts,
//                 relative to where the previous copy ended (0 when the two
//                 snapshots simply line up, which is most of them)
std::string encode(const std::string& baseStr, const std::string& targetStr) {
    const unsigned char* base = (const unsigned char*)baseStr.data();
    const unsigned char* tgt = (const unsigned char*)targetStr.data();
    const size_t bn = baseStr.size(), tn = targetStr.size();
    std::string out;

    size_t tableSize = 1024;
    while (tableSize < 2 * (bn / kStride + 1)) tableSize <<= 1;
    std::vector<int64_t> table(tableSize, -1);
    int shift = 64;   // the top log2(tableSize) bits of a hash pick its slot
    for (size_t n = tableSize; n > 1; n >>= 1) --shift;
    if (bn >= kBlock)
        for (size_t p = 0; p + kBlock <= bn; p += kStride) table[blockHash(base + p) >> shift] = (int64_t)p;

    uint64_t pow = 1;   // kMul^(kBlock-1), to roll a byte out of the hash
    for (size_t i = 1; i < kBlock; ++i) pow *= kMul;

    size_t lit = 0;          // first target byte not yet emitted
    size_t copyEnd = 0;      // base position after the last copy
    auto emitAdd = [&](size_t upto) {
        if (upto <= lit) return;
        putVarint(out, (uint64_t)(upto - lit) << 1);
        out.append((const char*)tgt + lit, upto - lit);
        lit = upto;
    };
    auto emitCopy = [&](size_t from, size_t len) {
        putVarint(out, ((uint64_t)len << 1) | 1);
        const int64_t rel = (int64_t)from - (int64_t)copyEnd;
        putVarint(out, (uint64_t)((rel << 1) ^ (rel >> 63)));
        copyEnd = from + len;
    };

    size_t j = 0;
    uint64_t h = 0;
    bool hashValid = false;
    while (j < tn) {
        // Where the snapshots line up -- carry on from the last copy, past
        // any bytes that were replaced one for one since.
        size_t from = SIZE_MAX, len = 0;
        const size_t expect = copyEnd + (j - lit);
        if (expect < bn) {
            while (expect + len < bn && j + len < tn && base[expect + len] == tgt[j + len]) ++len;
            if (len >= 16) from = expect; else len = 0;
        }
        if (from == SIZE_MAX && j + kBlock <= tn && bn >= kBlock) {
            if (!hashValid) { h = blockHash(tgt + j); hashValid = true; }
            const int64_t cand = table[h >> shift];
            if (cand >= 0 && memcmp(base + cand, tgt + j, kBlock) == 0) {
                size_t b = (size_t)cand, t = j;
                while (b > 0 && t > lit && base[b - 1] == tgt[t - 1]) { --b; --t; }
                len = j - t + kBlock;
                while (b + len < bn && t + len < tn && base[b + len] == tgt[t + len]) ++len;
                from = b;
                j = t;
            }
        }
        if (from != SIZE_MAX) {
            emitAdd(j);
            emitCopy(from, len);
            j += len;
            lit = j;
            hashValid = false;
            continue;
        }
        // No copy here: this byte is literal. Roll the hash one byte on.
        if (hashValid && j + kBlock < tn) h = (h - tgt[j] * pow) * kMul + tgt[j + kBlock];
        else hashValid = false;
        ++j;
    }
    emitAdd(tn);
    return out;
}

bool decode(const std::string& baseStr, const std::string& ops, std::string& out) {
    const unsigned char* p = (const unsigned char*)ops.data();
    const unsigned char* end = p + ops.size();
    out.clear();
    size_t copyEnd = 0;
    while (p < end) {
        uint64_t op = 0;
        if (!getVarint(p, end, op)) return false;
        const uint64_t len = op >> 1;
        if (op & 1) {
            uint64_t z = 0;
            if (!getVarint(p, end, z)) return false;
            const int64_t rel = (int64_t)(z >> 1) ^ -(int64_t)(z & 1);
            const int64_t from = (int64_t)copyEnd + rel;
            if (from < 0 || (uint64_t)from > baseStr.size() || len > baseStr.size() - (uint64_t)from)
                return false;
            out.append(baseStr, (size_t)from, (size_t)len);
            copyEnd = (size_t)(from + (int64_t)len);
        } else {
            if (len > (uint64_t)(end - p)) return false;
            out.append((const char*)p, (size_t)len);
            p += len;
        }
    }
    return true;
}

}  // namespace savedelta

namespace {

// A full snapshot every this many turns, so a chain is at most this long.
// Measured on an 820-turn campaign: see the PR that introduced it.
constexpr int kStateKeyframe = 32;

constexpr char kDiffMagic[4] = {'O', 'D', 'S', 'D'};
constexpr size_t kDiffHeader = 4 + 1 + 5 * 4;

void putU32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s += (char)(v >> (8 * i)); }
uint32_t getU32(const std::string& s, size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)(unsigned char)s[at + (size_t)i] << (8 * i);
    return v;
}
uint32_t crcOf(const std::string& s) {
    return crc32Of(s.data(), s.size());
}

std::string fullStateName(int turn) {
    char b[40];
    std::snprintf(b, sizeof(b), "turns/s_%05d.json", turn);
    return b;
}
std::string diffStateName(int turn) {
    char b[40];
    std::snprintf(b, sizeof(b), "turns/sd_%05d.bin", turn);
    return b;
}

std::string makeDiff(int baseTurn, const std::string& base, const std::string& target) {
    std::string d(kDiffMagic, 4);
    d += (char)1;
    putU32(d, (uint32_t)baseTurn);
    putU32(d, (uint32_t)base.size());
    putU32(d, crcOf(base));
    putU32(d, (uint32_t)target.size());
    putU32(d, crcOf(target));
    d += savedelta::encode(base, target);
    return d;
}

bool diffHeader(const std::string& d, int& baseTurn) {
    if (d.size() < kDiffHeader || memcmp(d.data(), kDiffMagic, 4) != 0 || d[4] != 1) return false;
    baseTurn = (int)getU32(d, 5);
    return true;
}

}  // namespace

// The state at the end of `turn`, rebuilt from whichever form the archive holds
// it in, and how many diffs that took (`depth`). "" when there is none, or when
// a diff does not rebuild exactly.
static std::string turnStateFrom(mz_zip_archive& zip, int turn, int* depth = nullptr) {
    std::vector<std::string> diffs;
    int t = turn;
    std::string state;
    for (;;) {
        state = extractEntry(zip, fullStateName(t).c_str());
        if (!state.empty()) break;
        std::string d = extractEntry(zip, diffStateName(t).c_str());
        int baseTurn = 0;
        // Each step must go back: a chain that loops or runs forward is damage.
        if (!diffHeader(d, baseTurn) || baseTurn >= t || diffs.size() > 4096) return {};
        diffs.push_back(std::move(d));
        t = baseTurn;
    }
    if (depth) *depth = (int)diffs.size();
    // Lengths at every step, the checksum of the result once. The keyframe is
    // checked by the zip's own CRC on the way out, and a step that went wrong
    // anywhere in the chain changes the bytes it ends in -- so one CRC of the
    // turn asked for catches it, where one per step cost 62 passes over the
    // whole state on a 31-diff chain and was most of what reading one cost.
    for (auto it = diffs.rbegin(); it != diffs.rend(); ++it) {
        const std::string& d = *it;
        if (getU32(d, 9) != state.size()) return {};
        std::string next;
        const bool last = (it + 1 == diffs.rend());
        if (!savedelta::decode(state, d.substr(kDiffHeader), next) ||
            getU32(d, 17) != next.size() || (last && getU32(d, 21) != crcOf(next))) {
            fprintf(stderr, "[SAVE] the state of turn %d does not rebuild exactly; treating it as absent\n", turn);
            return {};
        }
        state.swap(next);
    }
    return state;
}

// ─── Appending a turn in place ──────────────────────────
//
// THE WHOLE SAVE WAS REWRITTEN TO ADD ONE TURN. Carrying every entry across,
// even as raw compressed bytes, costs per entry, and a campaign holds two or
// three entries a turn: measured on an 820-turn save, appending turn 700 took
// 296 ms, and it grew with every turn played -- so a long game's total save
// time grew with the square of its length.
//
// A zip can be added to without being rewritten. The new entries go after the
// end of the archive, followed by a new central directory listing the old
// entries that are still current and the new ones, and a new end record:
//
//     [entries][old directory][old end] [new entries][new directory][new end]
//
// NOTHING ALREADY IN THE FILE IS OVERWRITTEN, which is what keeps this as safe
// as the write-then-rename it replaces. Until the new end record is complete
// the file still holds the previous save whole, plus some bytes after it, and
// openSaveFile() reads it as exactly that save (see intactArchiveEnd). A
// process killed mid-append loses the turn it was writing, and nothing else.
//
// What is replaced -- last turn's state.json, metadata.json, index.json -- and
// each old directory stay in the file as dead bytes. When they pass a quarter
// of it, the next save is a full rewrite instead, which drops them; so is any
// save this cannot append to (one that does not end in a plain end record,
// one large enough to need zip64). Old builds read the result as any zip.
enum class AppendResult { Done, Rewrite, Failed };

static AppendResult appendInPlace(const std::string& path,
                                  const std::function<bool(mz_zip_archive&)>& addNew,
                                  const std::function<bool(const char*)>& replaced) {
    auto u16 = [](const unsigned char* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; };
    auto u32 = [&](const unsigned char* p) { return u16(p) | u16(p + 2) << 16; };
    auto put16 = [](std::string& s, uint32_t v) { s += (char)v; s += (char)(v >> 8); };
    auto put32 = [&](std::string& s, uint32_t v) { put16(s, v & 0xFFFF); put16(s, v >> 16); };

    std::FILE* f = std::fopen(path.c_str(), "r+b");
    if (!f) return AppendResult::Rewrite;
    auto rewrite = [&]() { std::fclose(f); return AppendResult::Rewrite; };
    if (std::fseek(f, 0, SEEK_END) != 0) return rewrite();
    const long sizeL = std::ftell(f);
    // Well inside 4 GB, past which offsets need zip64, which this does not write.
    if (sizeL < 22 || sizeL > 0x60000000L) return rewrite();
    const uint32_t size = (uint32_t)sizeL;

    unsigned char end[22];
    if (std::fseek(f, (long)size - 22, SEEK_SET) != 0 || std::fread(end, 1, 22, f) != 22) return rewrite();
    if (u32(end) != 0x06054b50 || u16(end + 4) != 0 || u16(end + 6) != 0 || u16(end + 20) != 0) return rewrite();
    const uint32_t entries = u16(end + 10), cdSize = u32(end + 12), cdOfs = u32(end + 16);
    if (entries == 0xFFFF || u16(end + 8) != entries || (uint64_t)cdOfs + cdSize != size - 22) return rewrite();

    std::string cd(cdSize, '\0');
    if (cdSize && (std::fseek(f, (long)cdOfs, SEEK_SET) != 0 || std::fread(&cd[0], 1, cdSize, f) != cdSize))
        return rewrite();

    // The old directory, less what this turn replaces.
    std::string kept;
    uint32_t keptCount = 0;
    uint64_t live = 0;   // bytes of entry data still in use, to know when to compact
    for (size_t pos = 0, i = 0; i < entries; ++i) {
        const unsigned char* r = (const unsigned char*)cd.data() + pos;
        if (pos + 46 > cd.size() || u32(r) != 0x02014b50) return rewrite();
        const uint32_t n = u16(r + 28), e = u16(r + 30), c = u16(r + 32), comp = u32(r + 20);
        const size_t rec = 46 + n + e + c;
        if (pos + rec > cd.size() || comp == 0xFFFFFFFF || u32(r + 42) == 0xFFFFFFFF) return rewrite();
        const std::string name((const char*)r + 46, n);
        if (!replaced(name.c_str())) {
            kept.append((const char*)r, rec);
            ++keptCount;
            live += 30 + n + e + comp;
        }
        pos += rec;
    }

    // This turn's entries, built in memory as an archive of their own.
    mz_zip_archive hz{};
    if (!mz_zip_writer_init_heap(&hz, 0, 0)) return rewrite();
    void* buf = nullptr;
    size_t bufSize = 0;
    const bool built = addNew(hz) && mz_zip_writer_finalize_heap_archive(&hz, &buf, &bufSize);
    mz_zip_writer_end(&hz);
    std::string added;
    if (buf) { added.assign((const char*)buf, bufSize); mz_free(buf); }
    if (!built || added.size() < 22) return rewrite();
    const unsigned char* a = (const unsigned char*)added.data();
    const unsigned char* ae = a + added.size() - 22;
    if (u32(ae) != 0x06054b50) return rewrite();
    const uint32_t aEntries = u16(ae + 10), aCdSize = u32(ae + 12), aCdOfs = u32(ae + 16);
    if ((uint64_t)aCdOfs + aCdSize + 22 != added.size()) return rewrite();

    // Its directory, pointed at where its entries will sit in the save.
    std::string newDir = kept;
    for (size_t pos = aCdOfs, i = 0; i < aEntries; ++i) {
        if (pos + 46 > (size_t)aCdOfs + aCdSize || u32(a + pos) != 0x02014b50) return rewrite();
        const size_t rec = 46 + u16(a + pos + 28) + u16(a + pos + 30) + u16(a + pos + 32);
        std::string r(added, pos, rec);
        const uint32_t local = u32(a + pos + 42) + size;
        for (int b = 0; b < 4; ++b) r[42 + (size_t)b] = (char)(local >> (8 * b));
        newDir += r;
        pos += rec;
    }
    const uint64_t newEntries = (uint64_t)keptCount + aEntries;
    const uint64_t newCdOfs = (uint64_t)size + aCdOfs;
    const uint64_t after = newCdOfs + newDir.size() + 22;
    if (newEntries >= 0xFFFF || after > 0x60000000ull) return rewrite();
    const uint64_t dead = after - (live + aCdOfs) - newDir.size() - 22;
    if (after > (1u << 20) && dead * 4 > after) return rewrite();   // time to compact

    std::string tail;
    put32(tail, 0x06054b50);
    put16(tail, 0); put16(tail, 0);
    put16(tail, (uint32_t)newEntries); put16(tail, (uint32_t)newEntries);
    put32(tail, (uint32_t)newDir.size());
    put32(tail, (uint32_t)newCdOfs);
    put16(tail, 0);

    // Every write checked: a short write is a full disk, and a save that
    // reports success after one is the save that will not open tomorrow.
    bool ok = std::fseek(f, (long)size, SEEK_SET) == 0 &&
              std::fwrite(added.data(), 1, aCdOfs, f) == aCdOfs &&
              std::fwrite(newDir.data(), 1, newDir.size(), f) == newDir.size() &&
              std::fwrite(tail.data(), 1, tail.size(), f) == tail.size() &&
              std::fflush(f) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(f)) == 0;
#elif !defined(__EMSCRIPTEN__)
    ok = ok && fsync(fileno(f)) == 0;
#endif
    if (!ok) {
        // Put the file back as it was. If even that fails, what follows the old
        // archive is ignored by openSaveFile() all the same.
        std::fflush(f);
#ifdef _WIN32
        (void)_chsize_s(_fileno(f), (__int64)size);
#else
        (void)ftruncate(fileno(f), (off_t)size);
#endif
        std::fclose(f);
        return AppendResult::Failed;
    }
    return std::fclose(f) == 0 ? AppendResult::Done : AppendResult::Failed;
}

// ─── Create initial .odsv ────────────────────────────────

bool SaveManager::createSave(const std::string& odsvPath,
                              const std::string& odmData,
                              const SaveMetadata& meta) {
    // A save was written. On the web that write went to a filesystem that
    // dies with the tab, so note it for util/WebPersist. No-op elsewhere.
    odPersistMark();

    // ATOMIC, THE WAY appendTurn ALWAYS WAS -- AND FOR A REPORTED REASON.
    //
    // This wrote the archive straight to its final path, so a process that
    // died mid-write left a half-finished .odsv sitting exactly where a real
    // save belongs. A truncated zip is the tidy outcome; the nasty one is an
    // archive that still opens and holds a truncated state.json, which is the
    // file behind "close the app while it autosaves, reopen, load, crash".
    // See the note over Game::loadStateJson.
    // CREATE MEANS CREATE. This writes a NEW archive -- the map, the metadata
    // and an empty index -- and carries nothing across. Called on a path that
    // already holds a played world, it therefore deletes every turn delta and
    // state.json with them, leaving metadata.json still claiming the turns.
    // The loader then walks 1..turn_count, misses every entry, applies nothing
    // and reports nothing: the world opens at day one, silently.
    //
    // That is not hypothetical. Renaming a world did exactly this, in two
    // copies of the same handler, and a 120-turn save came back as an empty
    // one. Refusing here is what makes it a caught mistake rather than a lost
    // evening -- a rename wants updateLastPlayed(), which rewrites the
    // metadata and keeps everything else.
    if (highestTurnInBuffer(readFile(odsvPath)) > 0) {
        fprintf(stderr, "[SAVE] refusing to create over a save that holds turns: %s\n",
                odsvPath.c_str());
        return false;
    }

    const std::string tmpPath = odsvPath + ".part";
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, tmpPath.c_str(), 0))
        return false;

    // Embed the original .odmap
    // Stored, not deflated — see the note in appendTurn(); the embedded map is
    // already-compressed data and re-deflating it is wasted time.
    zipAdd(&zip, "map.odmap", odmData.data(), odmData.size(), MZ_NO_COMPRESSION);

    // Write metadata.json with country compasses
    const std::string metaJson = metadataJson(meta);
    
    zipAdd(&zip, "metadata.json", metaJson.data(), metaJson.size(), MZ_BEST_COMPRESSION);
    writeSealedTreasuries(zip, meta);

    // Write empty index
    const char* idx = "{\"turns\":[]}\n";
    zipAdd(&zip, "index.json", idx, strlen(idx), MZ_BEST_COMPRESSION);

    // CHECKED, not called and forgotten. A finalize that failed still returned
    // true from here, so a save that was never written reported success and
    // was discovered later, by the loader, as a damaged file.
    if (!mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        std::remove(tmpPath.c_str());
        return false;
    }
    mz_zip_writer_end(&zip);

    // The same rename as appendTurn, for the same reason: MSVC's rename fails
    // if the target exists, so the old file goes first on Windows.
#ifdef _WIN32
    std::remove(odsvPath.c_str());
#endif
    if (std::rename(tmpPath.c_str(), odsvPath.c_str()) != 0) {
        std::remove(tmpPath.c_str());
        return false;
    }
    return true;
}
 
// ─── Append a turn delta ─────────────────────────────────
 
bool SaveManager::appendTurn(const std::string& odsvPath, const TurnDelta& delta,
                             const std::string* stateJson,
                             const std::vector<std::pair<std::string, std::string>>* extraFiles) {
    // A save was written. On the web that write went to a filesystem that
    // dies with the tab, so note it for util/WebPersist. No-op elsewhere.
    odPersistMark();

    // THE SAVE IS REWRITTEN WHOLE EVERY TURN, so what it costs to carry the
    // old contents across IS what Process Turn costs.
    //
    // It used to cost everything. Each carried entry was inflated to the heap
    // and deflated again at MZ_BEST_COMPRESSION -- and the archive holds two
    // files per turn played, one of them the full state snapshot, which is
    // ~700 KB by turn 200. So turn N re-compressed N snapshots, and the bill
    // grew with every turn: 8.7 seconds of it at turn 220 on this machine,
    // and roughly twice that by turn 400.
    //
    // Nothing is drawn or polled for the whole of it -- the last frame goes up
    // at "Saving turn data..." and the next one after the save is finished --
    // so the window stops answering the window server and the player gets a
    // beachball, or "Not Responding" on Windows. That is the freeze on Process
    // Turn, and it is invisible to the [TURN] timing line above, which stops
    // measuring before the save begins.
    //
    // The bytes were already compressed. mz_zip_writer_add_from_zip_reader
    // copies them across as they stand, so the per-turn cost is now the I/O
    // and nothing else, and it no longer grows with the length of the game.
    //
    // And it is read from the file, not from a copy of it in memory: see
    // openSaveFile().
    mz_zip_archive srcZip{};
    if (!openSaveFile(srcZip, odsvPath))
        return false;
    // A save without a map is not a save; refuse it rather than write a
    // half-formed archive over it. (Located, not extracted: the point of this
    // pass is that map.odmap is never unpacked.)
    if (mz_zip_reader_locate_file(&srcZip, "map.odmap", nullptr, 0) < 0) {
        mz_zip_reader_end(&srcZip);
        return false;
    }

    // Re-read metadata (from the file as it still stands: neither way of
    // writing below changes it before the new turn is complete).
    SaveMetadata meta = readMetadata(odsvPath);

    meta.turnCount++;
    // ...and never below what the archive already holds, nor below the turn
    // being written. See highestTurnIn(): the count is a cache of the entries,
    // so a stale cache must lose to them, not the other way round.
    const int heldTurns = highestTurnIn(srcZip);
    if (heldTurns > meta.turnCount)        meta.turnCount = heldTurns;
    if (delta.turnNumber > meta.turnCount) meta.turnCount = delta.turnNumber;
    const std::string metaJson = metadataJson(meta);

    // Index
    std::string idx = "{\"turns\":[";
    for (int t = 1; t <= meta.turnCount; ++t) {
        if (t > 1) idx += ",";
        idx += "\"" + std::string(5 - std::to_string(t).size(), '0') + std::to_string(t) + "\"";
    }
    idx += "]}\n";

    // Carry over map.odmap, the previous turn files, and anything else already
    // in the archive (rebellion/*.svg and friends), byte for byte.
    // metadata.json / index.json are regenerated above, so they're skipped
    // here. state.json is skipped when we're writing a fresh one below;
    // entries supplied via extraFiles are skipped too, since those override.
    // THE TURN BEING WRITTEN IS NOT CARRIED OVER. Re-appending a turn number
    // the archive already has -- which a rewind, a replay, or a host resolving
    // the same turn twice all do -- used to leave TWO entries with the same
    // name. `data/saves/Quick Start.odsv` has 160 such pairs. miniz finds an
    // entry by binary search over the sorted directory, so which of the two
    // any later read returns is unspecified: in multiplayer that means the
    // host can broadcast a different turn from the one it resolved, and the
    // game desyncs permanently with nothing to see.
    char turnPath[32];
    snprintf(turnPath, sizeof(turnPath), "turns/t_%05d.dat", delta.turnNumber);
    char turnStatePath[40];
    snprintf(turnStatePath, sizeof(turnStatePath), "turns/s_%05d.json", delta.turnNumber);
    const std::string turnDiffPath = diffStateName(delta.turnNumber);

    // THIS TURN'S STATE, AS A DIFF WHEN IT CAN BE (see "Per-turn state as a
    // diff"). Against the turn before, rebuilt from this archive -- never from
    // a copy held in memory, which could be of some other version of the file.
    // A full snapshot instead on every kStateKeyframe-th turn, when the chain
    // behind the base is already that long, or when there is no base to diff
    // against (the first turn, a turn after a gap, a base that does not rebuild).
    std::string stateDiff;
    if (stateJson && delta.turnNumber > 1 && delta.turnNumber % kStateKeyframe != 0) {
        int depth = 0;
        const std::string base = turnStateFrom(srcZip, delta.turnNumber - 1, &depth);
        if (!base.empty() && depth + 1 < kStateKeyframe)
            stateDiff = makeDiff(delta.turnNumber - 1, base, *stateJson);
    }

    // REWRITING A TURN THE NEXT ONE IS A DIFF OF. A rewind, a replay or a host
    // resolving the same turn twice re-appends turn N while N+1 is still in the
    // archive -- and if N+1 is stored as a diff of N, replacing N leaves it a
    // diff of bytes that no longer exist. So N+1 is rebuilt now, from the old
    // N, and kept as a full snapshot; nothing after it changes.
    std::string reanchoredName, reanchoredState;
    if (stateJson) {
        const std::string nextDiff = diffStateName(delta.turnNumber + 1);
        if (mz_zip_reader_locate_file(&srcZip, nextDiff.c_str(), nullptr, 0) >= 0) {
            reanchoredName = nextDiff;
            reanchoredState = turnStateFrom(srcZip, delta.turnNumber + 1);
        }
    }
    // The old entries this turn replaces. Everything else is carried over.
    auto replaced = [&](const char* name) {
        if (strcmp(name, "metadata.json") == 0 || strcmp(name, "index.json") == 0) return true;
        if (strcmp(name, turnPath) == 0) return true;
        if (stateJson && strcmp(name, turnStatePath) == 0) return true;
        if (stateJson && turnDiffPath == name) return true;
        if (!reanchoredName.empty() && reanchoredName == name) return true;
        if (!reanchoredState.empty() && fullStateName(delta.turnNumber + 1) == name) return true;
        if (stateJson && strcmp(name, "state.json") == 0) return true;
        if (extraFiles)
            for (auto& [n, c] : *extraFiles)
                if (n == name) return true;
        return false;
    };

    // Everything this turn writes, into whichever archive is being built.
    // Level 6, not the best: on a late-game state.json (1.2 MB) level 9 took
    // five times as long for 4.6% less, and it is written every turn.
    constexpr mz_uint kTurnLevel = 6;
    auto addNew = [&](mz_zip_archive& z) -> bool {
        bool ok = zipAdd(&z, "metadata.json", metaJson.data(), metaJson.size(), kTurnLevel);
        ok = zipAdd(&z, "index.json", idx.data(), idx.size(), kTurnLevel) && ok;
        // Add new turn
        auto packedPlain = packTurn(delta);
        const std::string packed = sealFor(meta, turnPath, packedPlain.data(), packedPlain.size());
        ok = ok && zipAdd(&z, turnPath, packed.data(), packed.size(), kTurnLevel);

        // Fold the state snapshot into this same rewrite when supplied.
        // It is written twice: state.json is the "resume where I left off" copy,
        // and turns/s_NNNNN.json pins the state as it was at the END of this turn.
        // The binary .dat only carries province/ship/army numbers, so without this
        // per-turn copy the pending orders, policies and research could not be
        // reconstructed for any turn but the latest — which is what rewinding to
        // an earlier turn needs.
        if (!reanchoredState.empty()) {
            const std::string name = fullStateName(delta.turnNumber + 1);
            const std::string c = sealFor(meta, name.c_str(), reanchoredState.data(), reanchoredState.size());
            ok = ok && zipAdd(&z, name.c_str(), c.data(), c.size(), kTurnLevel);
        }
        if (stateJson && !stateDiff.empty()) {
            // state.json stays whole: it is what a load resumes from, in this build
            // and in every older one.
            const std::string st = sealFor(meta, "state.json", stateJson->data(), stateJson->size());
            const std::string sd = sealFor(meta, turnDiffPath.c_str(), stateDiff.data(), stateDiff.size());
            ok = ok && zipAdd(&z, "state.json", st.data(), st.size(), kTurnLevel);
            ok = ok && zipAdd(&z, turnDiffPath.c_str(), sd.data(), sd.size(), kTurnLevel);
        } else if (stateJson) {
            const std::string st = sealFor(meta, "state.json", stateJson->data(), stateJson->size());
            const std::string ts = sealFor(meta, turnStatePath, stateJson->data(), stateJson->size());
            // DEFLATED ONCE WHEN THE TWO ARE THE SAME BYTES, which they are unless
            // the save is sealed (the seal is keyed by entry name). The snapshot
            // is the largest thing written each turn and compressing it twice at
            // best compression was a fifth of a turn's whole cost. The archive is
            // byte-for-byte what two separate zipAdd calls produce.
            if (st == ts) {
                const Deflated d = deflateEntry(st.data(), st.size(), kTurnLevel);
                ok = ok && zipAddDeflated(&z, "state.json", d, st.data(), st.size(), kTurnLevel);
                ok = ok && zipAddDeflated(&z, turnStatePath, d, st.data(), st.size(), kTurnLevel);
            } else {
                ok = ok && zipAdd(&z, "state.json", st.data(), st.size(), kTurnLevel);
                ok = ok && zipAdd(&z, turnStatePath, ts.data(), ts.size(), kTurnLevel);
            }
        }
        if (extraFiles)
            for (auto& [name, content] : *extraFiles) {
                const std::string c = sealFor(meta, name.c_str(), content.data(), content.size());
                ok = ok && zipAdd(&z, name.c_str(), c.data(), c.size(), kTurnLevel);
            }
        return ok;
    };

    // APPENDED IN PLACE when it can be: see appendInPlace(). The archive is
    // closed first -- everything needed from it has been read above.
    mz_zip_reader_end(&srcZip);
    switch (appendInPlace(odsvPath, addNew, replaced)) {
        case AppendResult::Done:   return true;
        case AppendResult::Failed: return false;
        case AppendResult::Rewrite: break;
    }
    if (!openSaveFile(srcZip, odsvPath)) return false;

    // THE FULL REWRITE, which also drops whatever earlier appends left behind.
    // WRITTEN BESIDE THE SAVE, THEN MOVED OVER IT.
    //
    // mz_zip_writer_init_file truncates its target, so writing straight to
    // odsvPath left the player's only copy destroyed for the length of the
    // rewrite. During a rewrite that took eight seconds and looked like a
    // hang, force-quitting -- which is what one does to a hung game -- ate the
    // save. The rename at the end is atomic on every platform the game ships
    // on, so the file on disk is either last turn's save or this turn's.
    const std::string tmpPath = odsvPath + ".part";
    mz_zip_archive newZip{};
    if (!mz_zip_writer_init_file(&newZip, tmpPath.c_str(), 0)) {
        mz_zip_reader_end(&srcZip);
        return false;
    }
    auto fail = [&]() {
        mz_zip_writer_end(&newZip);
        mz_zip_reader_end(&srcZip);
        std::remove(tmpPath.c_str());
        return false;
    };
    {
        const int fc = (int)mz_zip_reader_get_num_files(&srcZip);
        for (int i = 0; i < fc; ++i) {
            mz_zip_archive_file_stat st{};
            if (!mz_zip_reader_file_stat(&srcZip, i, &st)) continue;
            if (replaced(st.m_filename)) continue;

            // Compressed bytes straight across: no inflate, no deflate. This
            // is the whole of the fix -- see the note at the top.
            if (!mz_zip_writer_add_from_zip_reader(&newZip, &srcZip, (mz_uint)i))
                return fail();
        }
    }

    if (!addNew(newZip)) return fail();
    if (!mz_zip_writer_finalize_archive(&newZip)) return fail();
    mz_zip_writer_end(&newZip);
    mz_zip_reader_end(&srcZip);

    // Only now does the save on disk change. std::rename replaces the target
    // atomically on POSIX; on Windows MSVC's rename fails if the target
    // exists, so the old file goes first and the small window that opens is
    // still far shorter than the whole rewrite it replaces.
#ifdef _WIN32
    std::remove(odsvPath.c_str());
#endif
    if (std::rename(tmpPath.c_str(), odsvPath.c_str()) != 0) {
        std::remove(tmpPath.c_str());
        return false;
    }
    return true;
}

// ─── Update last_played ──────────────────────────────────

bool SaveManager::updateLastPlayed(const std::string& odsvPath, const SaveMetadata* updatedMeta) {
    // A save was written. On the web that write went to a filesystem that
    // dies with the tab, so note it for util/WebPersist. No-op elsewhere.
    odPersistMark();
    auto meta = updatedMeta ? *updatedMeta : readMetadata(odsvPath);
    if (meta.saveName.empty()) return false;

    auto now = std::time(nullptr);
    char buf[32];
    struct tm* tmInfo = std::gmtime(&now);
    if (tmInfo == nullptr) return false;
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tmInfo);
    meta.lastPlayed = buf;

    // Rebuild the archive with updated metadata
    auto zipData = readFile(odsvPath);
    if (zipData.empty()) return false;

    // A REWRITE NEVER CLAIMS FEWER TURNS THAN IT CARRIES ACROSS. The metadata
    // here can come from the caller (trySaveGame builds it) or from a parse
    // that failed; either way a short count used to be copied onto the file
    // verbatim, and the turns below were carried over regardless -- a save
    // holding a full game and claiming an empty one.
    if (const int heldTurns = highestTurnInBuffer(zipData); heldTurns > meta.turnCount)
        meta.turnCount = heldTurns;

    std::vector<uint8_t> odmData;
    {
        mz_zip_archive tmpZip{};
        if (!openSaveMem(tmpZip, zipData))
            return false;
        int idx = mz_zip_reader_locate_file(&tmpZip, "map.odmap", nullptr, 0);
        if (idx < 0) { mz_zip_reader_end(&tmpZip); return false; }
        size_t odmSz = 0;
        void* odm = mz_zip_reader_extract_to_heap(&tmpZip, idx, &odmSz, 0);
        if (odm) { odmData.assign((uint8_t*)odm, (uint8_t*)odm + odmSz); free(odm); }
        mz_zip_reader_end(&tmpZip);
    }
    if (odmData.empty()) return false;

    // Build new archive
    mz_zip_archive newZip{};
    if (!mz_zip_writer_init_file(&newZip, odsvPath.c_str(), 0))
        return false;

    // Stored, not deflated: map.odmap is itself a zip of already-compressed
    // PNGs, so max-level deflate burns hundreds of ms per call and saves
    // essentially nothing. This runs on every save rewrite, so it dominated
    // turn time.
    zipAdd(&newZip, "map.odmap", odmData.data(), odmData.size(), MZ_NO_COMPRESSION);

    const std::string metaJson = metadataJson(meta);
    zipAdd(&newZip, "metadata.json", metaJson.data(), metaJson.size(), MZ_BEST_COMPRESSION);
    writeSealedTreasuries(newZip, meta);

    // Everything the archive already held, except the three entries this
    // function regenerates.
    //
    // This used to copy ONLY turns/* and state.json, which made a
    // "touch the timestamp" call quietly destructive: rebels.json and every
    // rebellion/<cid>.svg were dropped on the floor. Clicking a save in the load
    // browser calls this before loading it, so a breakaway state's name, colour
    // and flag were deleted by the act of opening the game they belonged to.
    // What came back was whatever synthesizeMissingRebels() could invent from
    // the province ids alone -- "Rebel State 84", a hashed colour, no flag.
    //
    // Listing what to KEEP is what made that possible. This lists what to
    // REPLACE instead, so an entry nothing here knows about survives by
    // default rather than by being remembered.
    {
        mz_zip_archive srcZip{};
        if (openSaveMem(srcZip, zipData)) {
            int fc = (int)mz_zip_reader_get_num_files(&srcZip);
            for (int i = 0; i < fc; ++i) {
                mz_zip_archive_file_stat st{};
                if (!mz_zip_reader_file_stat(&srcZip, i, &st)) continue;
                if (strcmp(st.m_filename, "map.odmap") == 0 ||
                    strcmp(st.m_filename, "metadata.json") == 0 ||
                    strcmp(st.m_filename, "index.json") == 0 ||
                    strcmp(st.m_filename, "fog/treasuries.json") == 0) continue;
                // Verbatim, for the reason given in appendTurn: inflating and
                // re-deflating every turn file in the archive is seconds of
                // work on a long save, and it buys nothing -- the bytes are
                // already compressed.
                mz_zip_writer_add_from_zip_reader(&newZip, &srcZip, (mz_uint)i);
            }
            mz_zip_reader_end(&srcZip);
        }
    }

    // Index
    std::string idxStr = "{\"turns\":[";
    for (int t = 1; t <= meta.turnCount; ++t) {
        if (t > 1) idxStr += ",";
        idxStr += "\"" + std::string(5 - std::to_string(t).size(), '0') + std::to_string(t) + "\"";
    }
    idxStr += "]}\n";
    zipAdd(&newZip, "index.json", idxStr.data(), idxStr.size(), MZ_BEST_COMPRESSION);

    mz_zip_writer_finalize_archive(&newZip);
    mz_zip_writer_end(&newZip);
    return true;
}

// ─── Read metadata ───────────────────────────────────────

// ─── Parse SaveMetadata from an in-memory ZIP buffer ─────

static SaveMetadata parseMetadataJson(const std::string& js) {
    SaveMetadata meta;
    if (js.empty()) return meta;
    try {
        auto j = nlohmann::json::parse(js);

        meta.saveName = j.value("save_name", "");
        meta.version = j.value("version", "");
        meta.created = j.value("created", "");
        meta.lastPlayed = j.value("last_played", "");
        meta.turnCount = j.value("turn_count", 0);
        meta.provinceCount = j.value("province_count", 0);
        meta.shipCount = j.value("ship_count", 0);
        meta.playerCountryId = j.value("player_country_id", 0);
        meta.format = j.value("format", 0);
        meta.fogOfWar = j.value("fog_of_war", false);
        if (j.contains("seal") && j["seal"].is_object())
            meta.sealSalt = j["seal"].value("salt", "");

        // Convention 1 stored the map file's own numbers: positive meant more
        // LEFT and more AUTHORITARIAN, the opposite of what PoliticalCompass
        // means on both axes. Every save written before that was fixed carries
        // those, and reading them as-is is the bug itself -- the Soviet Union
        // comes back as hard right and cannot enact a single left doctrine.
        //
        // Absent means 1, because that is what every save that lacks the field
        // was written by. Saves from here on say 2 and are taken at face value.
        const int compassConvention = j.value("compass_convention", 1);
        const float compassSign = (compassConvention < 2) ? -1.0f : 1.0f;
        if (j.contains("country_compasses")) {
            for (auto& [cidStr, compass] : j["country_compasses"].items()) {
                int cid = std::stoi(cidStr);
                float econ = compass.value("economic", 0.0f);
                float soc = compass.value("social", 0.0f);
                meta.countryCompasses[cid] = {econ * compassSign, soc * compassSign};
            }
        }
        if (j.contains("country_treasuries")) {
            for (auto& [cidStr, tr] : j["country_treasuries"].items()) {
                int cid = std::stoi(cidStr);
                meta.countryTreasuries[cid] = tr.get<double>();
            }
        }
    } catch (...) {
        // Fall back to manual parsing if JSON fails
    }

    return meta;
}

static SaveMetadata parseMetadataFromZip(const std::vector<uint8_t>& zipData) {
    if (zipData.empty()) return {};
    mz_zip_archive zip{};
    if (!openSaveMem(zip, zipData)) return {};
    const std::string js = extractEntry(zip, "metadata.json");
    SaveMetadata meta = parseMetadataJson(js);
    readSealedTreasuries(zip, meta);
    const int held = highestTurnIn(zip);
    if (held > meta.turnCount) meta.turnCount = held;
    mz_zip_reader_end(&zip);
    return meta;
}

SaveMetadata SaveManager::readMetadata(const std::string& odsvPath) {
    mz_zip_archive zip{};
    if (!openSaveFile(zip, odsvPath)) return {};
    const std::string js = extractEntry(zip, "metadata.json");
    SaveMetadata meta = parseMetadataJson(js);
    readSealedTreasuries(zip, meta);
    // Every caller of this gets the repair, including the two that go on to
    // WRITE the number back -- trySaveGame through updateLastPlayed, and
    // appendTurn. Putting it here rather than at those call sites is the
    // point: a sixth rewrite path cannot forget it.
    const int held = highestTurnIn(zip);
    if (held > meta.turnCount) meta.turnCount = held;
    mz_zip_reader_end(&zip);
    return meta;
}

// ─── Read a specific turn ────────────────────────────────

TurnDelta SaveManager::readTurn(const std::string& odsvPath, int turn) {
    TurnDelta delta;
    mz_zip_archive zip{};
    if (!openSaveFile(zip, odsvPath))
        return delta;

    char turnPath[32];
    snprintf(turnPath, sizeof(turnPath), "turns/t_%05d.dat", turn);
    const std::string packed = extractEntry(zip, turnPath);   // opens a sealed turn
    if (!packed.empty()) unpackTurn((const uint8_t*)packed.data(), packed.size(), delta);
    mz_zip_reader_end(&zip);
    return delta;
}

// ─── SaveReader ──────────────────────────────────────────

SaveReader::SaveReader(const std::string& odsvPath) {
    auto* zip = new mz_zip_archive{};
    m_ok = openSaveFile(*zip, odsvPath);
    if (!m_ok) { delete zip; zip = nullptr; }
    m_zip = zip;
}

SaveReader::~SaveReader() {
    if (!m_zip) return;
    auto* zip = static_cast<mz_zip_archive*>(m_zip);
    mz_zip_reader_end(zip);
    delete zip;
}

TurnDelta SaveReader::readTurn(int turn) {
    TurnDelta delta;
    if (!m_ok) return delta;
    char turnPath[32];
    snprintf(turnPath, sizeof(turnPath), "turns/t_%05d.dat", turn);
    const std::string packed = extractEntry(*static_cast<mz_zip_archive*>(m_zip), turnPath);
    if (!packed.empty())
        SaveManager::unpackTurn((const uint8_t*)packed.data(), packed.size(), delta);
    return delta;
}

bool SaveReader::hasEntry(const std::string& name) {
    return m_ok && mz_zip_reader_locate_file(static_cast<mz_zip_archive*>(m_zip), name.c_str(), nullptr, 0) >= 0;
}

bool SaveReader::hasTurnState(int turn) {
    return hasEntry(fullStateName(turn)) || hasEntry(diffStateName(turn));
}

// ─── Extract .odmap from .odsv ───────────────────────────

std::vector<uint8_t> SaveManager::extractODM(const std::string& odsvPath) {
    mz_zip_archive zip{};
    if (!openSaveFile(zip, odsvPath))
        return {};

    int idx = mz_zip_reader_locate_file(&zip, "map.odmap", nullptr, 0);
    if (idx < 0) { mz_zip_reader_end(&zip); return {}; }

    size_t sz = 0;
    void* d = mz_zip_reader_extract_to_heap(&zip, idx, &sz, 0);
    mz_zip_reader_end(&zip);

    if (!d) return {};
    std::vector<uint8_t> result((uint8_t*)d, (uint8_t*)d + sz);
    free(d);
    return result;
}

// ─── Size estimation ─────────────────────────────────────

size_t SaveManager::estimateDeltaSize(const TurnDelta& delta) {
    size_t s = 8; // header
    for (auto& p : delta.provinces) {
        s += 4; // id + mask
        if (p.ownerChanged)          s += 2;
        if (p.populationChanged)     s += 4;
        if (p.industryLevelChanged)  s += 1;
        if (p.fortificationChanged)  s += 1;
        if (p.incomeChanged)         s += 4;
        if (p.resourceIncomeChanged) s += 4;
        if (p.popIncomeChanged)      s += 4;
        if (p.popModifierChanged)    s += 4;
        if (p.outputChanged)         s += 1;
    }
    for (auto& s_ : delta.ships) {
        s += 3; // index + mask
        if (s_.latChanged)    s += 4;
        if (s_.lonChanged)    s += 4;
        if (s_.healthChanged) s += 1;
        if (s_.crewChanged)   s += 2;
        if (s_.countryIdChanged) s += 2;
    }
    for (auto& a : delta.armies) {
        s += 3; // provinceId + unitCount
        s += a.units.size() * 6; // each unit: countryId(2) + count(4)
    }
    s += 14; // research/pacification: extra_flags(1) + alloc(4) + pacAlloc(4) + activeNode(2) + points(2) + endMarker(1)
    // Wide populations, when any province outgrew the 32-bit field.
    size_t wide = 0;
    for (auto& p : delta.provinces)
        if (p.populationChanged && popNeedsWideField(p.newPopulation)) wide++;
    if (wide) s += 2 + wide * 10; // count(2) + each: provinceId(2) + population(8)
    // Research groups, on the same terms: counted only when they are written.
    bool anyGroups = false;
    for (int g = 1; g < 3; ++g) anyGroups |= (delta.researchGroups[g].activeNode >= 0);
    for (int g = 0; g < 3; ++g)
        anyGroups |= delta.researchGroups[g].autoAdvance ||
                     delta.researchGroups[g].sharePct != 50 ||
                     delta.researchGroups[g].lastNode >= 0;
    if (anyGroups) s += 3 * 6;   // each: activeNode(2) + lastNode(2) + share(1) + auto(1)
    return s;
}

std::string SaveManager::estimateGrowth(const TurnDelta& avgDelta, int turns) {
    size_t perTurn = estimateDeltaSize(avgDelta);
    size_t compressed = perTurn / 3; // rough ZIP compression ratio ~3:1 for binary data

    std::string out;
    out += "Estimated per-turn delta (uncompressed): " +
           std::to_string(perTurn / 1024) + " KB (" +
           std::to_string(perTurn) + " bytes)\n";
    out += "Estimated per-turn delta (compressed): ~" +
           std::to_string(std::max((size_t)1, compressed / 1024)) + " KB\n";
    out += "Estimated base .odmap: ~12 MB (static)\n";
    out += "Estimated .odsv after " + std::to_string(turns) + " turns: ~12 MB + " +
           std::to_string(compressed * turns / 1024 / 1024) + " MB\n";
    out += "Estimated .odsv after 100 turns: ~12 MB + " +
           std::to_string(compressed * 100 / 1024 / 1024) + " MB\n";
    out += "Estimated .odsv after 1000 turns: ~12 MB + " +
           std::to_string(compressed * 1000 / 1024 / 1024) + " MB\n";

    // Breakdown
    size_t provSize = 0;
    for (auto& p : avgDelta.provinces) {
        provSize += 4;
        if (p.populationChanged)     provSize += 4;
        if (p.ownerChanged)          provSize += 2;
        if (p.industryLevelChanged)  provSize += 1;
        if (p.fortificationChanged)  provSize += 1;
        if (p.incomeChanged)         provSize += 4;
        if (p.resourceIncomeChanged) provSize += 4;
        if (p.popIncomeChanged)      provSize += 4;
        if (p.popModifierChanged)    provSize += 4;
    }
    out += "Breakdown:\n";
    out += "  Province entries: " + std::to_string(avgDelta.provinces.size()) +
           " provinces × ~" + (avgDelta.provinces.empty() ? std::string("0") :
           std::to_string(provSize / avgDelta.provinces.size())) + " bytes avg\n";
    out += "  Ship entries: " + std::to_string(avgDelta.ships.size()) + "\n";
    out += "  Army entries: " + std::to_string(avgDelta.armies.size()) + "\n";
    return out;
}

bool SaveManager::updatePlayerCountry(const std::string& odsvPath, int playerCountryId) {
    // A save was written. On the web that write went to a filesystem that
    // dies with the tab, so note it for util/WebPersist. No-op elsewhere.
    odPersistMark();
    auto meta = readMetadata(odsvPath);
    if (meta.saveName.empty()) return false;
    meta.playerCountryId = playerCountryId;

    auto zipData = readFile(odsvPath);
    if (zipData.empty()) return false;

    // A REWRITE NEVER CLAIMS FEWER TURNS THAN IT CARRIES ACROSS. The metadata
    // here can come from the caller (trySaveGame builds it) or from a parse
    // that failed; either way a short count used to be copied onto the file
    // verbatim, and the turns below were carried over regardless -- a save
    // holding a full game and claiming an empty one.
    if (const int heldTurns = highestTurnInBuffer(zipData); heldTurns > meta.turnCount)
        meta.turnCount = heldTurns;

    std::vector<uint8_t> odmData;
    {
        mz_zip_archive tmpZip{};
        if (!openSaveMem(tmpZip, zipData))
            return false;
        int idx = mz_zip_reader_locate_file(&tmpZip, "map.odmap", nullptr, 0);
        if (idx < 0) { mz_zip_reader_end(&tmpZip); return false; }
        size_t odmSz = 0;
        void* odm = mz_zip_reader_extract_to_heap(&tmpZip, idx, &odmSz, 0);
        if (odm) { odmData.assign((uint8_t*)odm, (uint8_t*)odm + odmSz); free(odm); }
        mz_zip_reader_end(&tmpZip);
    }
    if (odmData.empty()) return false;

    mz_zip_archive newZip{};
    if (!mz_zip_writer_init_file(&newZip, odsvPath.c_str(), 0))
        return false;

    // Stored, not deflated: map.odmap is itself a zip of already-compressed
    // PNGs, so max-level deflate burns hundreds of ms per call and saves
    // essentially nothing. This runs on every save rewrite, so it dominated
    // turn time.
    zipAdd(&newZip, "map.odmap", odmData.data(), odmData.size(), MZ_NO_COMPRESSION);

    const std::string metaJson = metadataJson(meta);
    zipAdd(&newZip, "metadata.json", metaJson.data(), metaJson.size(), MZ_BEST_COMPRESSION);
    writeSealedTreasuries(newZip, meta);

    // Everything else the archive held, verbatim -- see appendTurn. This used
    // to copy only turns/*, state.json and index.json, the same keep-list bug
    // updateLastPlayed had: picking a country dropped rebels.json, every
    // rebellion/<cid>.svg and, in a fog world, what every country had seen.
    {
        mz_zip_archive srcZip{};
        if (openSaveMem(srcZip, zipData)) {
            int fc = (int)mz_zip_reader_get_num_files(&srcZip);
            for (int i = 0; i < fc; ++i) {
                mz_zip_archive_file_stat st{};
                if (!mz_zip_reader_file_stat(&srcZip, i, &st)) continue;
                if (strcmp(st.m_filename, "map.odmap") == 0 ||
                    strcmp(st.m_filename, "metadata.json") == 0 ||
                    strcmp(st.m_filename, "fog/treasuries.json") == 0) continue;
                mz_zip_writer_add_from_zip_reader(&newZip, &srcZip, (mz_uint)i);
            }
            mz_zip_reader_end(&srcZip);
        }
    }

    mz_zip_writer_finalize_archive(&newZip);
    mz_zip_writer_end(&newZip);
    return true;
}

// ─── Write state.json into .odsv ─────────────────────────

bool SaveManager::writeState(const std::string& odsvPath, const std::string& stateJson,
                              const std::vector<std::pair<std::string, std::string>>& extraFiles) {
    // A save was written. On the web that write went to a filesystem that
    // dies with the tab, so note it for util/WebPersist. No-op elsewhere.
    odPersistMark();
    auto zipData = readFile(odsvPath);
    if (zipData.empty()) return false;

    // turn_count here comes from parseMetadataFromZip() below, which already
    // reconciles it against the turns/ entries -- so there is no clamp in this
    // function. See highestTurnIn().
    std::vector<uint8_t> odmData;
    SaveMetadata meta;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> existingExtra;
    {
        mz_zip_archive tmpZip{};
        if (!openSaveMem(tmpZip, zipData))
            return false;
        int odmIdx = mz_zip_reader_locate_file(&tmpZip, "map.odmap", nullptr, 0);
        if (odmIdx >= 0) {
            size_t sz = 0;
            void* d = mz_zip_reader_extract_to_heap(&tmpZip, odmIdx, &sz, 0);
            if (d) { odmData.assign((uint8_t*)d, (uint8_t*)d + sz); free(d); }
        }
        if (odmData.empty()) { mz_zip_reader_end(&tmpZip); return false; }

        int fc = (int)mz_zip_reader_get_num_files(&tmpZip);
        for (int i = 0; i < fc; ++i) {
            mz_zip_archive_file_stat st{};
            if (!mz_zip_reader_file_stat(&tmpZip, i, &st)) continue;
            if (strstr(st.m_filename, "turns/") == st.m_filename ||
                strcmp(st.m_filename, "metadata.json") == 0 ||
                strcmp(st.m_filename, "index.json") == 0 ||
                strcmp(st.m_filename, "map.odmap") == 0 ||
                strcmp(st.m_filename, "state.json") == 0) {
                continue;
            }
            // Anything the caller is supplying is a REPLACEMENT, not a second
            // copy. Without this, saving a game that already had rebels.json in
            // its archive wrote both the old entry and the new one under the
            // same name -- and a zip lookup answers with the FIRST match, so
            // every later load read the stale copy. Rebel states created after
            // a save were therefore never seen again: their provinces came back
            // owned by a country that was not in the file, and got placeholder
            // "Rebel State N" identities. appendTurn already skipped overrides;
            // this path did not, and it also grew the archive by a full set of
            // duplicates every time it ran.
            bool overridden = false;
            for (auto& [n, c] : extraFiles)
                if (n == st.m_filename) { overridden = true; break; }
            if (overridden) continue;

            size_t sz = 0;
            void* d = mz_zip_reader_extract_to_heap(&tmpZip, i, &sz, 0);
            if (d) {
                existingExtra.push_back({st.m_filename, std::vector<uint8_t>((uint8_t*)d, (uint8_t*)d + sz)});
                free(d);
            }
        }
        mz_zip_reader_end(&tmpZip);
    }

    mz_zip_archive newZip{};
    if (!mz_zip_writer_init_file(&newZip, odsvPath.c_str(), 0))
        return false;

    // Stored, not deflated: map.odmap is itself a zip of already-compressed
    // PNGs, so max-level deflate burns hundreds of ms per call and saves
    // essentially nothing. This runs on every save rewrite, so it dominated
    // turn time.
    zipAdd(&newZip, "map.odmap", odmData.data(), odmData.size(), MZ_NO_COMPRESSION);
    // Parse metadata from the in-memory buffer (NOT from the truncated file on disk)
    meta = parseMetadataFromZip(zipData);
    const std::string metaJson = metadataJson(meta);
    zipAdd(&newZip, "metadata.json", metaJson.data(), metaJson.size(), MZ_BEST_COMPRESSION);

    std::string idxStr = "{\"turns\":[";
    for (int t = 1; t <= meta.turnCount; ++t) {
        if (t > 1) idxStr += ",";
        idxStr += "\"" + std::string(5 - std::to_string(t).size(), '0') + std::to_string(t) + "\"";
    }
    idxStr += "]}\n";
    zipAdd(&newZip, "index.json", idxStr.data(), idxStr.size(), MZ_BEST_COMPRESSION);

    {
        mz_zip_archive srcZip{};
        if (openSaveMem(srcZip, zipData)) {
            int fc = (int)mz_zip_reader_get_num_files(&srcZip);
            for (int i = 0; i < fc; ++i) {
                mz_zip_archive_file_stat st{};
                if (!mz_zip_reader_file_stat(&srcZip, i, &st)) continue;
                if (strstr(st.m_filename, "turns/") == st.m_filename) {
                    // Verbatim -- see appendTurn. This is the whole turn
                    // history, two files per turn played, so it is the one
                    // that made saving a long game take seconds.
                    mz_zip_writer_add_from_zip_reader(&newZip, &srcZip, (mz_uint)i);
                }
            }
            mz_zip_reader_end(&srcZip);
        }
    }

    {
        const std::string st = sealFor(meta, "state.json", stateJson.data(), stateJson.size());
        zipAdd(&newZip, "state.json", st.data(), st.size(), MZ_BEST_COMPRESSION);
    }

    // Already sealed, if the save is: carried as stored.
    for (auto& [name, data] : existingExtra)
        zipAdd(&newZip, name.c_str(), data.data(), data.size(), MZ_BEST_COMPRESSION);

    // Write new extra files (rebel SVGs, etc.) from caller
    for (auto& [name, content] : extraFiles) {
        const std::string c = sealFor(meta, name.c_str(), content.data(), content.size());
        zipAdd(&newZip, name.c_str(), c.data(), c.size(), MZ_BEST_COMPRESSION);
    }

    mz_zip_writer_finalize_archive(&newZip);
    mz_zip_writer_end(&newZip);
    return true;
}

// ─── Read state.json from .odsv ──────────────────────────

std::string SaveManager::readEntry(const std::string& odsvPath, const std::string& entryName) {
    mz_zip_archive zip{};
    if (!openSaveFile(zip, odsvPath)) return {};
    std::string result = extractEntry(zip, entryName.c_str());
    mz_zip_reader_end(&zip);
    return result;
}

std::string SaveManager::readState(const std::string& odsvPath) {
    return readEntry(odsvPath, "state.json");
}

std::string SaveManager::readTurnState(const std::string& odsvPath, int turn) {
    mz_zip_archive zip{};
    if (!openSaveFile(zip, odsvPath)) return {};
    std::string state = turnStateFrom(zip, turn);
    mz_zip_reader_end(&zip);
    return state;
}
