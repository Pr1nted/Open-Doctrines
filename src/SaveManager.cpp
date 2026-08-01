#include "SaveManager.h"
#include "miniz.h"
#include "miniz_zip.h"
#include "json.hpp"
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <ctime>

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

void SaveManager::writeFloat(std::vector<uint8_t>& buf, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    writeU32(buf, bits);
}

// ─── Pack one turn delta to binary ───────────────────────

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
        writeU16(buf, mask);
        if (p.ownerChanged)           writeU16(buf, (uint16_t)p.newOwner);
        if (p.populationChanged)      writeU32(buf, (uint32_t)p.newPopulation);
        if (p.industryLevelChanged)   buf.push_back((uint8_t)p.newIndustryLevel);
        if (p.fortificationChanged)   buf.push_back((uint8_t)p.newFortification);
        if (p.incomeChanged)          writeFloat(buf, p.newIncome);
        if (p.resourceIncomeChanged)  writeFloat(buf, p.newResourceIncome);
        if (p.popIncomeChanged)       writeFloat(buf, p.newPopIncome);
        if (p.popModifierChanged)     writeFloat(buf, p.newPopModifier);
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

    // Research/pacification state
    buf.push_back(0x01); // extra_flags: bit 0 = has_research_state
    writeFloat(buf, delta.researchAllocation);
    writeFloat(buf, delta.pacificationAllocation);
    writeU16(buf, (uint16_t)(delta.researchActiveNode + 1)); // -1 → 0 offset
    writeU16(buf, (uint16_t)delta.researchPoints);
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

    // Research/pacification state (optional, backward-compatible with old saves)
    if (ptr + 1 <= end) {
        uint8_t extraFlags = *ptr++;
        if ((extraFlags & 1) && ptr + 11 <= end) {
            out.researchAllocation = readFloat(ptr);
            out.pacificationAllocation = readFloat(ptr);
            int activeRaw = (int)readU16(ptr);
            out.researchActiveNode = activeRaw - 1; // undo +1 offset
            out.researchPoints = (int)readU16(ptr);
        }
    }

    return true;
}

// ─── Read entire file into memory ────────────────────────

static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamsize sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> data((size_t)sz);
    if (!f.read((char*)data.data(), sz)) return {};
    return data;
}

// ─── Create initial .odsv ────────────────────────────────

bool SaveManager::createSave(const std::string& odsvPath,
                              const std::string& odmData,
                              const SaveMetadata& meta) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, odsvPath.c_str(), 0))
        return false;

    // Embed the original .odmap
    // Stored, not deflated — see the note in appendTurn(); the embedded map is
    // already-compressed data and re-deflating it is wasted time.
    mz_zip_writer_add_mem(&zip, "map.odmap", odmData.data(), odmData.size(), MZ_NO_COMPRESSION);

    // Write metadata.json with country compasses
    std::string cr = meta.created.empty() ? "unknown" : meta.created;
    std::string lp = meta.lastPlayed.empty() ? cr : meta.lastPlayed;
    std::string ver = meta.version.empty() ? "unknown" : meta.version;
    
    std::string metaJson = "{\n";
    metaJson += "  \"save_name\": \"" + meta.saveName + "\",\n";
    metaJson += "  \"version\": \"" + ver + "\",\n";
    metaJson += "  \"created\": \"" + cr + "\",\n";
    metaJson += "  \"last_played\": \"" + lp + "\",\n";
    metaJson += "  \"turn_count\": " + std::to_string(meta.turnCount) + ",\n";
    metaJson += "  \"province_count\": " + std::to_string(meta.provinceCount) + ",\n";
    metaJson += "  \"ship_count\": " + std::to_string(meta.shipCount) + ",\n";
    metaJson += "  \"player_country_id\": " + std::to_string(meta.playerCountryId) + ",\n";
    metaJson += "  \"country_compasses\": {\n";
    bool first = true;
    for (auto& [cid, pc] : meta.countryCompasses) {
        if (!first) metaJson += ",\n";
        first = false;
        metaJson += "    \"" + std::to_string(cid) + "\": { \"economic\": " + std::to_string(pc.economic) + ", \"social\": " + std::to_string(pc.social) + " }";
    }
    metaJson += "\n  },\n";
    metaJson += "  \"country_treasuries\": {\n";
    first = true;
    for (auto& [cid, tr] : meta.countryTreasuries) {
        if (!first) metaJson += ",\n";
        first = false;
        metaJson += "    \"" + std::to_string(cid) + "\": " + std::to_string(tr);
    }
    metaJson += "\n  }\n";
    metaJson += "}\n";
    
    mz_zip_writer_add_mem(&zip, "metadata.json", metaJson.data(), metaJson.size(), MZ_BEST_COMPRESSION);

    // Write empty index
    const char* idx = "{\"turns\":[]}\n";
    mz_zip_writer_add_mem(&zip, "index.json", idx, strlen(idx), MZ_BEST_COMPRESSION);

mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return true;
}
 
// ─── Append a turn delta ─────────────────────────────────
 
bool SaveManager::appendTurn(const std::string& odsvPath, const TurnDelta& delta,
                             const std::string* stateJson,
                             const std::vector<std::pair<std::string, std::string>>* extraFiles) {
    std::vector<uint8_t> zipData = readFile(odsvPath);
    if (zipData.empty()) return false;

    // Extract map.odmap from original archive
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

    // Re-read metadata
    SaveMetadata meta = readMetadata(odsvPath);

    // Build new archive
    mz_zip_archive newZip{};
    if (!mz_zip_writer_init_file(&newZip, odsvPath.c_str(), 0))
        return false;

    // Stored, not deflated: map.odmap is itself a zip of already-compressed
    // PNGs, so max-level deflate burns hundreds of ms per call and saves
    // essentially nothing. This runs on every save rewrite, so it dominated
    // turn time.
    mz_zip_writer_add_mem(&newZip, "map.odmap", odmData.data(), odmData.size(), MZ_NO_COMPRESSION);

    meta.turnCount++;
    std::string cr = meta.created.empty() ? "unknown" : meta.created;
    std::string lp = meta.lastPlayed.empty() ? cr : meta.lastPlayed;
    std::string ver = meta.version.empty() ? "unknown" : meta.version;
    std::string metaJson =
        "{\n"
        "  \"save_name\": \"" + meta.saveName + "\",\n"
        "  \"version\": \"" + ver + "\",\n"
        "  \"created\": \"" + cr + "\",\n"
        "  \"last_played\": \"" + lp + "\",\n"
        "  \"turn_count\": " + std::to_string(meta.turnCount) + ",\n"
        "  \"province_count\": " + std::to_string(meta.provinceCount) + ",\n"
        "  \"ship_count\": " + std::to_string(meta.shipCount) + ",\n"
        "  \"player_country_id\": " + std::to_string(meta.playerCountryId) + ",\n"
        "  \"country_compasses\": {\n";
    bool first = true;
    for (auto& [cid, pc] : meta.countryCompasses) {
        if (!first) metaJson += ",\n";
        first = false;
        metaJson += "    \"" + std::to_string(cid) + "\": { \"economic\": " + std::to_string(pc.economic) + ", \"social\": " + std::to_string(pc.social) + " }";
    }
    metaJson += "\n  },\n";
    metaJson += "  \"country_treasuries\": {\n";
    first = true;
    for (auto& [cid, tr] : meta.countryTreasuries) {
        if (!first) metaJson += ",\n";
        first = false;
        metaJson += "    \"" + std::to_string(cid) + "\": " + std::to_string(tr);
    }
    metaJson += "\n  }\n";
    metaJson += "}\n";
    mz_zip_writer_add_mem(&newZip, "metadata.json", metaJson.data(), metaJson.size(), MZ_BEST_COMPRESSION);

    // Index
    std::string idx = "{\"turns\":[";
    for (int t = 1; t <= meta.turnCount; ++t) {
        if (t > 1) idx += ",";
        idx += "\"" + std::string(5 - std::to_string(t).size(), '0') + std::to_string(t) + "\"";
    }
    idx += "]}\n";
    mz_zip_writer_add_mem(&newZip, "index.json", idx.data(), idx.size(), MZ_BEST_COMPRESSION);

    // Carry over previous turn files, plus anything else already in the
    // archive (rebellion/*.svg and friends). map.odmap / metadata.json /
    // index.json are regenerated above, so they're skipped here.
    // state.json is skipped when we're writing a fresh one below; entries
    // supplied via extraFiles are skipped too, since those override.
    {
        mz_zip_archive srcZip{};
        if (mz_zip_reader_init_mem(&srcZip, zipData.data(), zipData.size(), 0)) {
            int fc = (int)mz_zip_reader_get_num_files(&srcZip);
            for (int i = 0; i < fc; ++i) {
                mz_zip_archive_file_stat st{};
                if (!mz_zip_reader_file_stat(&srcZip, i, &st)) continue;
                if (strcmp(st.m_filename, "map.odmap") == 0 ||
                    strcmp(st.m_filename, "metadata.json") == 0 ||
                    strcmp(st.m_filename, "index.json") == 0) continue;
                if (stateJson && strcmp(st.m_filename, "state.json") == 0) continue;
                bool overridden = false;
                if (extraFiles)
                    for (auto& [n, c] : *extraFiles)
                        if (n == st.m_filename) { overridden = true; break; }
                if (overridden) continue;

                size_t sz = 0;
                void* d = mz_zip_reader_extract_to_heap(&srcZip, i, &sz, 0);
                if (d) {
                    mz_zip_writer_add_mem(&newZip, st.m_filename, d, sz, MZ_BEST_COMPRESSION);
                    free(d);
                }
            }
            mz_zip_reader_end(&srcZip);
        }
    }

    // Add new turn
    auto packed = packTurn(delta);
    char turnPath[32];
    snprintf(turnPath, sizeof(turnPath), "turns/t_%05d.dat", delta.turnNumber);
    mz_zip_writer_add_mem(&newZip, turnPath, packed.data(), packed.size(), MZ_BEST_COMPRESSION);

    // Fold the state snapshot into this same rewrite when supplied.
    // It is written twice: state.json is the "resume where I left off" copy,
    // and turns/s_NNNNN.json pins the state as it was at the END of this turn.
    // The binary .dat only carries province/ship/army numbers, so without this
    // per-turn copy the pending orders, policies and research could not be
    // reconstructed for any turn but the latest — which is what rewinding to
    // an earlier turn needs.
    if (stateJson) {
        mz_zip_writer_add_mem(&newZip, "state.json", stateJson->data(), stateJson->size(), MZ_BEST_COMPRESSION);
        char statePath[40];
        snprintf(statePath, sizeof(statePath), "turns/s_%05d.json", delta.turnNumber);
        mz_zip_writer_add_mem(&newZip, statePath, stateJson->data(), stateJson->size(), MZ_BEST_COMPRESSION);
    }
    if (extraFiles)
        for (auto& [name, content] : *extraFiles)
            mz_zip_writer_add_mem(&newZip, name.c_str(), content.data(), content.size(), MZ_BEST_COMPRESSION);

    mz_zip_writer_finalize_archive(&newZip);
    mz_zip_writer_end(&newZip);
    return true;
}

// ─── Update last_played ──────────────────────────────────

bool SaveManager::updateLastPlayed(const std::string& odsvPath, const SaveMetadata* updatedMeta) {
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

    // Build new archive
    mz_zip_archive newZip{};
    if (!mz_zip_writer_init_file(&newZip, odsvPath.c_str(), 0))
        return false;

    // Stored, not deflated: map.odmap is itself a zip of already-compressed
    // PNGs, so max-level deflate burns hundreds of ms per call and saves
    // essentially nothing. This runs on every save rewrite, so it dominated
    // turn time.
    mz_zip_writer_add_mem(&newZip, "map.odmap", odmData.data(), odmData.size(), MZ_NO_COMPRESSION);

    std::string cr = meta.created.empty() ? "unknown" : meta.created;
    std::string lp = meta.lastPlayed.empty() ? cr : meta.lastPlayed;
    std::string ver = meta.version.empty() ? "unknown" : meta.version;
    
    std::string metaJson = "{\n";
    metaJson += "  \"save_name\": \"" + meta.saveName + "\",\n";
    metaJson += "  \"version\": \"" + ver + "\",\n";
    metaJson += "  \"created\": \"" + cr + "\",\n";
    metaJson += "  \"last_played\": \"" + lp + "\",\n";
    metaJson += "  \"turn_count\": " + std::to_string(meta.turnCount) + ",\n";
    metaJson += "  \"province_count\": " + std::to_string(meta.provinceCount) + ",\n";
    metaJson += "  \"ship_count\": " + std::to_string(meta.shipCount) + ",\n";
    metaJson += "  \"player_country_id\": " + std::to_string(meta.playerCountryId) + ",\n";
    metaJson += "  \"country_compasses\": {\n";
    bool first = true;
    for (auto& [cid, pc] : meta.countryCompasses) {
        if (!first) metaJson += ",\n";
        first = false;
        metaJson += "    \"" + std::to_string(cid) + "\": { \"economic\": " + std::to_string(pc.economic) + ", \"social\": " + std::to_string(pc.social) + " }";
    }
    metaJson += "\n  },\n";
    metaJson += "  \"country_treasuries\": {\n";
    first = true;
    for (auto& [cid, tr] : meta.countryTreasuries) {
        if (!first) metaJson += ",\n";
        first = false;
        metaJson += "    \"" + std::to_string(cid) + "\": " + std::to_string(tr);
    }
    metaJson += "\n  }\n";
    metaJson += "}\n";
    mz_zip_writer_add_mem(&newZip, "metadata.json", metaJson.data(), metaJson.size(), MZ_BEST_COMPRESSION);

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
                    strcmp(st.m_filename, "index.json") == 0) continue;
                size_t sz = 0;
                void* d = mz_zip_reader_extract_to_heap(&srcZip, i, &sz, 0);
                if (d) {
                    mz_zip_writer_add_mem(&newZip, st.m_filename, d, sz, MZ_BEST_COMPRESSION);
                    free(d);
                }
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

    mz_zip_writer_finalize_archive(&newZip);
    mz_zip_writer_end(&newZip);
    return true;
}

// ─── Read metadata ───────────────────────────────────────

// ─── Parse SaveMetadata from an in-memory ZIP buffer ─────

static SaveMetadata parseMetadataFromZip(const std::vector<uint8_t>& zipData) {
    SaveMetadata meta;
    if (zipData.empty()) return meta;

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0))
        return meta;

    int idx = mz_zip_reader_locate_file(&zip, "metadata.json", nullptr, 0);
    if (idx < 0) { mz_zip_reader_end(&zip); return meta; }

    size_t sz = 0;
    void* d = mz_zip_reader_extract_to_heap(&zip, idx, &sz, 0);
    if (!d) { mz_zip_reader_end(&zip); return meta; }

    std::string js((char*)d, sz);
    free(d);
    mz_zip_reader_end(&zip);

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

        if (j.contains("country_compasses")) {
            for (auto& [cidStr, compass] : j["country_compasses"].items()) {
                int cid = std::stoi(cidStr);
                float econ = compass.value("economic", 0.0f);
                float soc = compass.value("social", 0.0f);
                meta.countryCompasses[cid] = {econ, soc};
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

SaveMetadata SaveManager::readMetadata(const std::string& odsvPath) {
    auto zipData = readFile(odsvPath);
    return parseMetadataFromZip(zipData);
}

// ─── Read a specific turn ────────────────────────────────

TurnDelta SaveManager::readTurn(const std::string& odsvPath, int turn) {
    TurnDelta delta;
    auto zipData = readFile(odsvPath);
    if (zipData.empty()) return delta;

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0))
        return delta;

    char turnPath[32];
    snprintf(turnPath, sizeof(turnPath), "turns/t_%05d.dat", turn);
    int idx = mz_zip_reader_locate_file(&zip, turnPath, nullptr, 0);
    if (idx < 0) { mz_zip_reader_end(&zip); return delta; }

    size_t sz = 0;
    void* d = mz_zip_reader_extract_to_heap(&zip, idx, &sz, 0);
    if (!d) { mz_zip_reader_end(&zip); return delta; }

    unpackTurn((const uint8_t*)d, sz, delta);
    free(d);
    mz_zip_reader_end(&zip);
    return delta;
}

// ─── Extract .odmap from .odsv ───────────────────────────

std::vector<uint8_t> SaveManager::extractODM(const std::string& odsvPath) {
    auto zipData = readFile(odsvPath);
    if (zipData.empty()) return {};

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0))
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
    s += 12; // research/pacification: extra_flags(1) + alloc(4) + pacAlloc(4) + activeNode(2) + points(2) + endMarker(1)
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
    auto meta = readMetadata(odsvPath);
    if (meta.saveName.empty()) return false;
    meta.playerCountryId = playerCountryId;

    auto zipData = readFile(odsvPath);
    if (zipData.empty()) return false;

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

    mz_zip_archive newZip{};
    if (!mz_zip_writer_init_file(&newZip, odsvPath.c_str(), 0))
        return false;

    // Stored, not deflated: map.odmap is itself a zip of already-compressed
    // PNGs, so max-level deflate burns hundreds of ms per call and saves
    // essentially nothing. This runs on every save rewrite, so it dominated
    // turn time.
    mz_zip_writer_add_mem(&newZip, "map.odmap", odmData.data(), odmData.size(), MZ_NO_COMPRESSION);

    std::string cr = meta.created.empty() ? "unknown" : meta.created;
    std::string lp = meta.lastPlayed.empty() ? cr : meta.lastPlayed;
    std::string ver = meta.version.empty() ? "unknown" : meta.version;
    std::string metaJson =
        "{\n"
        "  \"save_name\": \"" + meta.saveName + "\",\n"
        "  \"version\": \"" + ver + "\",\n"
        "  \"created\": \"" + cr + "\",\n"
        "  \"last_played\": \"" + lp + "\",\n"
        "  \"turn_count\": " + std::to_string(meta.turnCount) + ",\n"
        "  \"province_count\": " + std::to_string(meta.provinceCount) + ",\n"
        "  \"ship_count\": " + std::to_string(meta.shipCount) + ",\n"
        "  \"player_country_id\": " + std::to_string(meta.playerCountryId) + ",\n"
        "  \"country_compasses\": {\n";
    bool first = true;
    for (auto& [cid, pc] : meta.countryCompasses) {
        if (!first) metaJson += ",\n";
        first = false;
        metaJson += "    \"" + std::to_string(cid) + "\": { \"economic\": " + std::to_string(pc.economic) + ", \"social\": " + std::to_string(pc.social) + " }";
    }
    metaJson += "\n  },\n";
    metaJson += "  \"country_treasuries\": {\n";
    first = true;
    for (auto& [cid, tr] : meta.countryTreasuries) {
        if (!first) metaJson += ",\n";
        first = false;
        metaJson += "    \"" + std::to_string(cid) + "\": " + std::to_string(tr);
    }
    metaJson += "\n  }\n";
    metaJson += "}\n";
    mz_zip_writer_add_mem(&newZip, "metadata.json", metaJson.data(), metaJson.size(), MZ_BEST_COMPRESSION);

    // Copy turn files + state.json from original archive
    {
        mz_zip_archive srcZip{};
        if (mz_zip_reader_init_mem(&srcZip, zipData.data(), zipData.size(), 0)) {
            int fc = (int)mz_zip_reader_get_num_files(&srcZip);
            for (int i = 0; i < fc; ++i) {
                mz_zip_archive_file_stat st{};
                if (!mz_zip_reader_file_stat(&srcZip, i, &st)) continue;
                if (strstr(st.m_filename, "turns/") == st.m_filename ||
                    strcmp(st.m_filename, "state.json") == 0 ||
                    strcmp(st.m_filename, "index.json") == 0) {
                    size_t sz = 0;
                    void* d = mz_zip_reader_extract_to_heap(&srcZip, i, &sz, 0);
                    if (d) {
                        mz_zip_writer_add_mem(&newZip, st.m_filename, d, sz, MZ_BEST_COMPRESSION);
                        free(d);
                    }
                }
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
    auto zipData = readFile(odsvPath);
    if (zipData.empty()) return false;

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

    mz_zip_archive newZip{};
    if (!mz_zip_writer_init_file(&newZip, odsvPath.c_str(), 0))
        return false;

    // Stored, not deflated: map.odmap is itself a zip of already-compressed
    // PNGs, so max-level deflate burns hundreds of ms per call and saves
    // essentially nothing. This runs on every save rewrite, so it dominated
    // turn time.
    mz_zip_writer_add_mem(&newZip, "map.odmap", odmData.data(), odmData.size(), MZ_NO_COMPRESSION);
    // Parse metadata from the in-memory buffer (NOT from the truncated file on disk)
    meta = parseMetadataFromZip(zipData);
    std::string cr = meta.created.empty() ? "unknown" : meta.created;
    std::string lp = meta.lastPlayed.empty() ? cr : meta.lastPlayed;
    std::string ver = meta.version.empty() ? "unknown" : meta.version;
    std::string metaJson =
        "{\n"
        "  \"save_name\": \"" + meta.saveName + "\",\n"
        "  \"version\": \"" + ver + "\",\n"
        "  \"created\": \"" + cr + "\",\n"
        "  \"last_played\": \"" + lp + "\",\n"
        "  \"turn_count\": " + std::to_string(meta.turnCount) + ",\n"
        "  \"province_count\": " + std::to_string(meta.provinceCount) + ",\n"
        "  \"ship_count\": " + std::to_string(meta.shipCount) + ",\n"
        "  \"player_country_id\": " + std::to_string(meta.playerCountryId) + ",\n"
        "  \"country_compasses\": {\n";
    bool first = true;
    for (auto& [cid, pc] : meta.countryCompasses) {
        if (!first) metaJson += ",\n";
        first = false;
        metaJson += "    \"" + std::to_string(cid) + "\": { \"economic\": " + std::to_string(pc.economic) + ", \"social\": " + std::to_string(pc.social) + " }";
    }
    metaJson += "\n  },\n";
    metaJson += "  \"country_treasuries\": {\n";
    first = true;
    for (auto& [cid, tr] : meta.countryTreasuries) {
        if (!first) metaJson += ",\n";
        first = false;
        metaJson += "    \"" + std::to_string(cid) + "\": " + std::to_string(tr);
    }
    metaJson += "\n  }\n";
    metaJson += "}\n";
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
                    size_t sz = 0;
                    void* d = mz_zip_reader_extract_to_heap(&srcZip, i, &sz, 0);
                    if (d) {
                        mz_zip_writer_add_mem(&newZip, st.m_filename, d, sz, MZ_BEST_COMPRESSION);
                        free(d);
                    }
                }
            }
            mz_zip_reader_end(&srcZip);
        }
    }

    mz_zip_writer_add_mem(&newZip, "state.json", stateJson.data(), stateJson.size(), MZ_BEST_COMPRESSION);

    for (auto& [name, data] : existingExtra)
        mz_zip_writer_add_mem(&newZip, name.c_str(), data.data(), data.size(), MZ_BEST_COMPRESSION);

    // Write new extra files (rebel SVGs, etc.) from caller
    for (auto& [name, content] : extraFiles)
        mz_zip_writer_add_mem(&newZip, name.c_str(), content.data(), content.size(), MZ_BEST_COMPRESSION);

    mz_zip_writer_finalize_archive(&newZip);
    mz_zip_writer_end(&newZip);
    return true;
}

// ─── Read state.json from .odsv ──────────────────────────

std::string SaveManager::readEntry(const std::string& odsvPath, const std::string& entryName) {
    auto zipData = readFile(odsvPath);
    if (zipData.empty()) return {};

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0))
        return {};

    int idx = mz_zip_reader_locate_file(&zip, entryName.c_str(), nullptr, 0);
    if (idx < 0) { mz_zip_reader_end(&zip); return {}; }

    size_t sz = 0;
    void* d = mz_zip_reader_extract_to_heap(&zip, idx, &sz, 0);
    mz_zip_reader_end(&zip);

    if (!d) return {};
    std::string result((char*)d, sz);
    free(d);
    return result;
}

std::string SaveManager::readState(const std::string& odsvPath) {
    auto zipData = readFile(odsvPath);
    if (zipData.empty()) return {};

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0))
        return {};

    int idx = mz_zip_reader_locate_file(&zip, "state.json", nullptr, 0);
    if (idx < 0) { mz_zip_reader_end(&zip); return {}; }

    size_t sz = 0;
    void* d = mz_zip_reader_extract_to_heap(&zip, idx, &sz, 0);
    mz_zip_reader_end(&zip);

    if (!d) return {};
    std::string result((char*)d, sz);
    free(d);
    return result;
}
