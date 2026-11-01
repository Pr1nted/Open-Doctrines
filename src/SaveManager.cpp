// Before any header that might pull in <windows.h>: its min/max macros would
// otherwise mangle std::max/std::min (MSVC C2589) later in this file.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "SaveManager.h"
#include "util/WebPersist.h"
#include "miniz.h"
#include "miniz_zip.h"
#include "json.hpp"
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <random>
#include <set>
#ifdef OD_HAS_P2
#include "odseal.h"
#endif

#if defined(__EMSCRIPTEN__)
// No durable disk here: the save lands in MEMFS/IDBFS and odPersistMark() is
// what carries it across a tab close. fsync would be a no-op at best.
#elif defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

// ─── Atomic, durable file replacement ────────────────────
//
// A save that survives the PROGRAM being killed is not the same as one that
// survives the MACHINE being killed. The temp-file-then-rename below already
// gives the first: a crash mid-write leaves the half-written .part and the real
// save untouched. The second needs fsync, because a rename can reach the disk
// while the bytes it points at are still in a write-back cache -- after a power
// cut the directory then holds a save-sized file full of nothing. fsync orders
// the two: data first, then the rename over the old file.
//
// Best-effort by design. A platform that cannot fsync (or the web, which has no
// durable disk to fsync to) still gets the atomic rename, which is what stops a
// torn write from being read as a valid save.
namespace {

void fsyncFile(const std::string& path) {
#if defined(__EMSCRIPTEN__)
    (void)path;
#elif defined(_WIN32)
    // GENERIC_WRITE, not READ: FlushFileBuffers requires write access on the
    // handle, and the .part was just written and closed, so reopening it for
    // write to flush is exactly right.
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { FlushFileBuffers(h); CloseHandle(h); }
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) { ::fsync(fd); ::close(fd); }
#endif
}

// The rename itself is only durable once the DIRECTORY entry is flushed. POSIX
// only: on Windows a completed rename is already durable, and the web has no
// directory to sync.
void fsyncDir(const std::string& path) {
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
    const size_t slash = path.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? std::string(".")
                          : (slash == 0)                 ? std::string("/")
                          :                                path.substr(0, slash);
    int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd >= 0) { ::fsync(fd); ::close(fd); }
#else
    (void)path;
#endif
}

// Swap a freshly finalized `.part` over the real save, atomically and durably.
// The caller has already finalized and closed the writer on `tmpPath`. On any
// failure the .part is removed and the existing save is left exactly as it was,
// so a failed save never costs the player the last good one.
bool commitReplace(const std::string& tmpPath, const std::string& finalPath) {
    fsyncFile(tmpPath);
#ifdef _WIN32
    // MSVC's rename refuses an existing target, so the old file goes first. The
    // window this opens is far shorter than the rewrite that preceded it, and
    // the .part is intact throughout.
    std::remove(finalPath.c_str());
#endif
    if (std::rename(tmpPath.c_str(), finalPath.c_str()) != 0) {
        std::remove(tmpPath.c_str());
        return false;
    }
    fsyncDir(finalPath);
    return true;
}

}  // namespace

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

static int highestTurnIn(mz_zip_archive& zip) {
    int highest = 0;
    const int fc = (int)mz_zip_reader_get_num_files(&zip);
    for (int i = 0; i < fc; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        int n = 0;
        if (std::sscanf(st.m_filename, "turns/t_%d.dat", &n) == 1 && n > highest)
            highest = n;
    }
    return highest;
}

static int highestTurnInBuffer(const std::vector<uint8_t>& zipData) {
    if (zipData.empty()) return 0;
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0)) return 0;
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
static bool openSaveFile(mz_zip_archive& zip, const std::string& path) {
    zip = mz_zip_archive{};
    return mz_zip_reader_init_file(&zip, path.c_str(), 0);
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

// The bytes to write for entry `name`: sealed when the save is, plain otherwise.
static std::string sealFor(const SaveMetadata& meta, const char* name, const void* data, size_t n) {
    std::string plain((const char*)data, n);
#ifdef OD_HAS_P2
    if (meta.sealSalt.empty() || !sealedName(name)) return plain;
    // u64 plain length, then the deflated bytes.
    mz_ulong zlen = mz_compressBound((mz_ulong)n);
    std::string packed(8 + zlen, '\0');
    for (int i = 0; i < 8; ++i) packed[(size_t)i] = (char)((uint64_t)n >> (8 * i));
    if (mz_compress2((unsigned char*)&packed[8], &zlen, (const unsigned char*)data, (mz_ulong)n,
                     MZ_BEST_COMPRESSION) != MZ_OK)
        return plain;
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
    size_t sz = 0;
    void* d = mz_zip_reader_extract_to_heap(&zip, idx, &sz, 0);
    if (!d) return {};
    std::string out((const char*)d, sz);
    free(d);
    return out;
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
                    mz_ulong got = (mz_ulong)n;
                    if (mz_uncompress(plain.empty() ? nullptr : (unsigned char*)&plain[0], &got,
                                      (const unsigned char*)&out[8], (mz_ulong)(out.size() - 8)) == MZ_OK &&
                        got == (mz_ulong)n)
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
    mz_zip_writer_add_mem(&zip, "fog/treasuries.json", data.data(), data.size(), MZ_BEST_COMPRESSION);
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
    mz_zip_writer_add_mem(&zip, "map.odmap", odmData.data(), odmData.size(), MZ_NO_COMPRESSION);

    // Write metadata.json with country compasses
    const std::string metaJson = metadataJson(meta);
    
    mz_zip_writer_add_mem(&zip, "metadata.json", metaJson.data(), metaJson.size(), MZ_BEST_COMPRESSION);
    writeSealedTreasuries(zip, meta);

    // Write empty index
    const char* idx = "{\"turns\":[]}\n";
    mz_zip_writer_add_mem(&zip, "index.json", idx, strlen(idx), MZ_BEST_COMPRESSION);

    // CHECKED, not called and forgotten. A finalize that failed still returned
    // true from here, so a save that was never written reported success and
    // was discovered later, by the loader, as a damaged file.
    if (!mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        std::remove(tmpPath.c_str());
        return false;
    }
    mz_zip_writer_end(&zip);

    // Flushed, then swapped in atomically -- see commitReplace.
    return commitReplace(tmpPath, odsvPath);
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

    // Re-read metadata (from the file as it still stands -- see the temp path
    // below, which is why this is safe).
    SaveMetadata meta = readMetadata(odsvPath);

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

    meta.turnCount++;
    // ...and never below what the archive already holds, nor below the turn
    // being written. See highestTurnIn(): the count is a cache of the entries,
    // so a stale cache must lose to them, not the other way round.
    const int heldTurns = highestTurnIn(srcZip);
    if (heldTurns > meta.turnCount)        meta.turnCount = heldTurns;
    if (delta.turnNumber > meta.turnCount) meta.turnCount = delta.turnNumber;
    const std::string metaJson = metadataJson(meta);
    mz_zip_writer_add_mem(&newZip, "metadata.json", metaJson.data(), metaJson.size(), MZ_BEST_COMPRESSION);

    // Index
    std::string idx = "{\"turns\":[";
    for (int t = 1; t <= meta.turnCount; ++t) {
        if (t > 1) idx += ",";
        idx += "\"" + std::string(5 - std::to_string(t).size(), '0') + std::to_string(t) + "\"";
    }
    idx += "]}\n";
    mz_zip_writer_add_mem(&newZip, "index.json", idx.data(), idx.size(), MZ_BEST_COMPRESSION);

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
    {
        const int fc = (int)mz_zip_reader_get_num_files(&srcZip);
        for (int i = 0; i < fc; ++i) {
            mz_zip_archive_file_stat st{};
            if (!mz_zip_reader_file_stat(&srcZip, i, &st)) continue;
            if (strcmp(st.m_filename, "metadata.json") == 0 ||
                strcmp(st.m_filename, "index.json") == 0) continue;
            if (strcmp(st.m_filename, turnPath) == 0) continue;
            if (stateJson && strcmp(st.m_filename, turnStatePath) == 0) continue;
            if (stateJson && strcmp(st.m_filename, "state.json") == 0) continue;
            bool overridden = false;
            if (extraFiles)
                for (auto& [n, c] : *extraFiles)
                    if (n == st.m_filename) { overridden = true; break; }
            if (overridden) continue;

            // Compressed bytes straight across: no inflate, no deflate. This
            // is the whole of the fix -- see the note at the top.
            if (!mz_zip_writer_add_from_zip_reader(&newZip, &srcZip, (mz_uint)i))
                return fail();
        }
    }

    // Add new turn
    auto packedPlain = packTurn(delta);
    const std::string packed = sealFor(meta, turnPath, packedPlain.data(), packedPlain.size());
    mz_zip_writer_add_mem(&newZip, turnPath, packed.data(), packed.size(), MZ_BEST_COMPRESSION);

    // Fold the state snapshot into this same rewrite when supplied.
    // It is written twice: state.json is the "resume where I left off" copy,
    // and turns/s_NNNNN.json pins the state as it was at the END of this turn.
    // The binary .dat only carries province/ship/army numbers, so without this
    // per-turn copy the pending orders, policies and research could not be
    // reconstructed for any turn but the latest — which is what rewinding to
    // an earlier turn needs.
    if (stateJson) {
        const std::string st = sealFor(meta, "state.json", stateJson->data(), stateJson->size());
        mz_zip_writer_add_mem(&newZip, "state.json", st.data(), st.size(), MZ_BEST_COMPRESSION);
        const std::string ts = sealFor(meta, turnStatePath, stateJson->data(), stateJson->size());
        mz_zip_writer_add_mem(&newZip, turnStatePath, ts.data(), ts.size(), MZ_BEST_COMPRESSION);
    }
    if (extraFiles)
        for (auto& [name, content] : *extraFiles) {
            const std::string c = sealFor(meta, name.c_str(), content.data(), content.size());
            mz_zip_writer_add_mem(&newZip, name.c_str(), c.data(), c.size(), MZ_BEST_COMPRESSION);
        }

    if (!mz_zip_writer_finalize_archive(&newZip)) return fail();
    mz_zip_writer_end(&newZip);
    mz_zip_reader_end(&srcZip);

    // Only now does the save on disk change: the new archive is flushed and
    // renamed over the old one in one durable step. See commitReplace.
    return commitReplace(tmpPath, odsvPath);
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
        if (!mz_zip_reader_init_mem(&tmpZip, zipData.data(), zipData.size(), 0))
            return false;
        int idx = mz_zip_reader_locate_file(&tmpZip, "map.odmap", nullptr, 0);
        if (idx < 0) { mz_zip_reader_end(&tmpZip); return false; }
        size_t odmSz = 0;
        void* odm = mz_zip_reader_extract_to_heap(&tmpZip, idx, &odmSz, 0);
        if (odm) { odmData.assign((uint8_t*)odm, (uint8_t*)odm + odmSz); free(odm); }
        mz_zip_reader_end(&tmpZip);
    }
    if (odmData.empty()) return false;

    // Build new archive. To a .part, never straight to the save: init_file
    // truncates its target the instant it opens, so writing in place turned a
    // crash during a timestamp touch into a destroyed world. See commitReplace.
    const std::string tmpPath = odsvPath + ".part";
    mz_zip_archive newZip{};
    if (!mz_zip_writer_init_file(&newZip, tmpPath.c_str(), 0))
        return false;

    // Stored, not deflated: map.odmap is itself a zip of already-compressed
    // PNGs, so max-level deflate burns hundreds of ms per call and saves
    // essentially nothing. This runs on every save rewrite, so it dominated
    // turn time.
    mz_zip_writer_add_mem(&newZip, "map.odmap", odmData.data(), odmData.size(), MZ_NO_COMPRESSION);

    const std::string metaJson = metadataJson(meta);
    mz_zip_writer_add_mem(&newZip, "metadata.json", metaJson.data(), metaJson.size(), MZ_BEST_COMPRESSION);
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
        if (mz_zip_reader_init_mem(&srcZip, zipData.data(), zipData.size(), 0)) {
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
    mz_zip_writer_add_mem(&newZip, "index.json", idxStr.data(), idxStr.size(), MZ_BEST_COMPRESSION);

    if (!mz_zip_writer_finalize_archive(&newZip)) {
        mz_zip_writer_end(&newZip);
        std::remove(tmpPath.c_str());
        return false;
    }
    mz_zip_writer_end(&newZip);
    return commitReplace(tmpPath, odsvPath);
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
    if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0)) return {};
    const std::string js = extractEntry(zip, "metadata.json");
    SaveMetadata meta = parseMetadataJson(js);
    readSealedTreasuries(zip, meta);
    const int held = highestTurnIn(zip);
    if (held > meta.turnCount) meta.turnCount = held;
    mz_zip_reader_end(&zip);
    return meta;
}

bool SaveManager::turnSequenceOk(const std::string& odsvPath, int* highestOut) {
    if (highestOut) *highestOut = 0;
    const std::vector<uint8_t> zipData = readFile(odsvPath);
    if (zipData.empty()) return true;     // no file, no turns: trivially in order
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0)) return false;

    int highest = 0;
    bool duplicate = false;
    std::set<int> seen;
    const int fc = (int)mz_zip_reader_get_num_files(&zip);
    for (int i = 0; i < fc; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        int n = 0;
        if (std::sscanf(st.m_filename, "turns/t_%d.dat", &n) == 1 && n >= 1) {
            if (!seen.insert(n).second) duplicate = true;
            if (n > highest) highest = n;
        }
    }
    mz_zip_reader_end(&zip);
    if (highestOut) *highestOut = highest;

    // Distinct turn ids, all in [1, highest], as many of them as `highest`:
    // that can only be the full run 1..highest with nothing missing and nothing
    // repeated. A gap (12 turns, t_00007 absent) fails here, which is what the
    // forward-recovery replay cannot see -- it stops at the first hole and drops
    // everything after it in silence.
    return !duplicate && (int)seen.size() == highest;
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
        if (!mz_zip_reader_init_mem(&tmpZip, zipData.data(), zipData.size(), 0))
            return false;
        int idx = mz_zip_reader_locate_file(&tmpZip, "map.odmap", nullptr, 0);
        if (idx < 0) { mz_zip_reader_end(&tmpZip); return false; }
        size_t odmSz = 0;
        void* odm = mz_zip_reader_extract_to_heap(&tmpZip, idx, &odmSz, 0);
        if (odm) { odmData.assign((uint8_t*)odm, (uint8_t*)odm + odmSz); free(odm); }
        mz_zip_reader_end(&tmpZip);
    }
    if (odmData.empty()) return false;

    // A .part, not the save itself: init_file truncates on open. See commitReplace.
    const std::string tmpPath = odsvPath + ".part";
    mz_zip_archive newZip{};
    if (!mz_zip_writer_init_file(&newZip, tmpPath.c_str(), 0))
        return false;

    // Stored, not deflated: map.odmap is itself a zip of already-compressed
    // PNGs, so max-level deflate burns hundreds of ms per call and saves
    // essentially nothing. This runs on every save rewrite, so it dominated
    // turn time.
    mz_zip_writer_add_mem(&newZip, "map.odmap", odmData.data(), odmData.size(), MZ_NO_COMPRESSION);

    const std::string metaJson = metadataJson(meta);
    mz_zip_writer_add_mem(&newZip, "metadata.json", metaJson.data(), metaJson.size(), MZ_BEST_COMPRESSION);
    writeSealedTreasuries(newZip, meta);

    // Everything else the archive held, verbatim -- see appendTurn. This used
    // to copy only turns/*, state.json and index.json, the same keep-list bug
    // updateLastPlayed had: picking a country dropped rebels.json, every
    // rebellion/<cid>.svg and, in a fog world, what every country had seen.
    {
        mz_zip_archive srcZip{};
        if (mz_zip_reader_init_mem(&srcZip, zipData.data(), zipData.size(), 0)) {
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

    if (!mz_zip_writer_finalize_archive(&newZip)) {
        mz_zip_writer_end(&newZip);
        std::remove(tmpPath.c_str());
        return false;
    }
    mz_zip_writer_end(&newZip);
    return commitReplace(tmpPath, odsvPath);
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
        if (!mz_zip_reader_init_mem(&tmpZip, zipData.data(), zipData.size(), 0))
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

    // A .part, not the save itself: init_file truncates on open. See commitReplace.
    const std::string tmpPath = odsvPath + ".part";
    mz_zip_archive newZip{};
    if (!mz_zip_writer_init_file(&newZip, tmpPath.c_str(), 0))
        return false;

    // Stored, not deflated: map.odmap is itself a zip of already-compressed
    // PNGs, so max-level deflate burns hundreds of ms per call and saves
    // essentially nothing. This runs on every save rewrite, so it dominated
    // turn time.
    mz_zip_writer_add_mem(&newZip, "map.odmap", odmData.data(), odmData.size(), MZ_NO_COMPRESSION);
    // Parse metadata from the in-memory buffer (NOT from the truncated file on disk)
    meta = parseMetadataFromZip(zipData);
    const std::string metaJson = metadataJson(meta);
    mz_zip_writer_add_mem(&newZip, "metadata.json", metaJson.data(), metaJson.size(), MZ_BEST_COMPRESSION);

    std::string idxStr = "{\"turns\":[";
    for (int t = 1; t <= meta.turnCount; ++t) {
        if (t > 1) idxStr += ",";
        idxStr += "\"" + std::string(5 - std::to_string(t).size(), '0') + std::to_string(t) + "\"";
    }
    idxStr += "]}\n";
    mz_zip_writer_add_mem(&newZip, "index.json", idxStr.data(), idxStr.size(), MZ_BEST_COMPRESSION);

    {
        mz_zip_archive srcZip{};
        if (mz_zip_reader_init_mem(&srcZip, zipData.data(), zipData.size(), 0)) {
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
        mz_zip_writer_add_mem(&newZip, "state.json", st.data(), st.size(), MZ_BEST_COMPRESSION);
    }

    // Already sealed, if the save is: carried as stored.
    for (auto& [name, data] : existingExtra)
        mz_zip_writer_add_mem(&newZip, name.c_str(), data.data(), data.size(), MZ_BEST_COMPRESSION);

    // Write new extra files (rebel SVGs, etc.) from caller
    for (auto& [name, content] : extraFiles) {
        const std::string c = sealFor(meta, name.c_str(), content.data(), content.size());
        mz_zip_writer_add_mem(&newZip, name.c_str(), c.data(), c.size(), MZ_BEST_COMPRESSION);
    }

    if (!mz_zip_writer_finalize_archive(&newZip)) {
        mz_zip_writer_end(&newZip);
        std::remove(tmpPath.c_str());
        return false;
    }
    mz_zip_writer_end(&newZip);
    return commitReplace(tmpPath, odsvPath);
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
