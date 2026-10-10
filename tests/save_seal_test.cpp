// Sealed fog-of-war saves, through SaveManager.
//
// A fog-of-war save holds every army the player could not see, and a .odsv is
// a zip. So its truth-bearing entries are sealed (see "Sealed entries" in
// src/SaveManager.cpp): this checks that they are unreadable as stored, read
// back exactly through the game's own readers, refuse to open when edited or
// under the wrong salt, and that a save without fog is written as before.
//
// Needs an odseal that carries the seal (OD_HAS_P2). Built against one that
// does not, there is nothing to test and it says so -- a skip, not a pass.
//
// Build target: SaveSealTest. Non-zero exit means a case failed.

#include "SaveManager.h"
#include "miniz.h"
#include "miniz_zip.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_failures = 0, g_checks = 0;

void check(const char* what, bool ok, const std::string& got = {}) {
    g_checks++;
    if (ok) { printf("  ok    %s\n", what); return; }
    g_failures++;
    printf("  FAIL  %s%s%s\n", what, got.empty() ? "" : "  --  ", got.c_str());
}

// An entry exactly as stored in the zip, bypassing SaveManager.
std::string rawEntry(const std::string& path, const char* name) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, path.c_str(), 0)) return {};
    std::string out;
    const int idx = mz_zip_reader_locate_file(&z, name, nullptr, 0);
    if (idx >= 0) {
        size_t sz = 0;
        if (void* d = mz_zip_reader_extract_to_heap(&z, idx, &sz, 0)) {
            out.assign((const char*)d, sz);
            free(d);
        }
    }
    mz_zip_reader_end(&z);
    return out;
}

// Rewrite one entry's stored bytes, keeping the rest.
bool replaceEntry(const std::string& path, const char* name, const std::string& bytes) {
    mz_zip_archive src{};
    if (!mz_zip_reader_init_file(&src, path.c_str(), 0)) return false;
    const std::string tmp = path + ".edit";
    mz_zip_archive dst{};
    if (!mz_zip_writer_init_file(&dst, tmp.c_str(), 0)) { mz_zip_reader_end(&src); return false; }
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&src); ++i) {
        mz_zip_archive_file_stat st{};
        mz_zip_reader_file_stat(&src, i, &st);
        if (strcmp(st.m_filename, name) == 0) continue;
        mz_zip_writer_add_from_zip_reader(&dst, &src, i);
    }
    mz_zip_writer_add_mem(&dst, name, bytes.data(), bytes.size(), MZ_BEST_COMPRESSION);
    mz_zip_writer_finalize_archive(&dst);
    mz_zip_writer_end(&dst);
    mz_zip_reader_end(&src);
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// A map archive is all createSave needs to be handed; it is stored, not read.
std::string tinyMap() {
    mz_zip_archive z{};
    mz_zip_writer_init_heap(&z, 0, 0);
    const char* m = "{}";
    mz_zip_writer_add_mem(&z, "metadata.json", m, 2, MZ_BEST_COMPRESSION);
    void* buf = nullptr;
    size_t sz = 0;
    mz_zip_writer_finalize_heap_archive(&z, &buf, &sz);
    std::string out((const char*)buf, sz);
    mz_zip_writer_end(&z);
    return out;
}

TurnDelta oneTurn(int turn) {
    TurnDelta d;
    d.turnNumber = turn;
    ProvinceDelta p;
    p.provinceId = 7;
    p.ownerChanged = true;      p.newOwner = 3;
    p.populationChanged = true; p.newPopulation = 123456;
    d.provinces.push_back(p);
    return d;
}

SaveMetadata baseMeta(const char* name, bool fog) {
    SaveMetadata m;
    m.saveName = name;
    m.version = "test";
    m.created = m.lastPlayed = "2026-10-10T00:00:00Z";
    m.fogOfWar = fog;
    if (fog) m.sealSalt = SaveManager::newSealSalt();
    m.countryTreasuries[3] = 4210.5;
    m.countryTreasuries[9] = 17.25;
    return m;
}

}  // namespace

int main() {
    printf("SaveSealTest\n");
    if (!SaveManager::canSeal()) {
        printf("  SKIP  this build's odseal has no seal (OD_HAS_P2); nothing to test\n");
        return 0;
    }
    const std::string dir = "save_seal_test_tmp";
    std::remove((dir + "_fog.odsv").c_str());
    std::remove((dir + "_plain.odsv").c_str());
    const std::string fog = dir + "_fog.odsv", plain = dir + "_plain.odsv";
    const std::string state = "{\"secret\":\"every army the player could not see\"}";
    const std::vector<std::pair<std::string, std::string>> extra = {{"fog/memory.bin", std::string("FOGM\x01\x02\x03", 7)},
                                                                    {"rebels.json", "[]"}};

    printf("\n== a fog world ==\n");
    check("a fog save is created", SaveManager::createSave(fog, tinyMap(), baseMeta("Fog", true)));
    check("a turn is appended with state and extras", SaveManager::appendTurn(fog, oneTurn(1), &state, &extra));
    for (const char* n : {"state.json", "turns/s_00001.json", "turns/t_00001.dat", "fog/memory.bin",
                          "rebels.json", "fog/treasuries.json"}) {
        const std::string raw = rawEntry(fog, n);
        check((std::string("stored sealed: ") + n).c_str(), raw.size() >= 4 && raw.compare(0, 4, "ODSL") == 0,
              raw.substr(0, 24));
    }
    check("the secret is not in the stored state", rawEntry(fog, "state.json").find("player could not see") == std::string::npos);
    const std::string meta = rawEntry(fog, "metadata.json");
    check("metadata names the world and is plain", meta.find("\"save_name\": \"Fog\"") != std::string::npos, meta);
    check("metadata carries no treasury", meta.find("4210") == std::string::npos);
    check("state reads back exactly", SaveManager::readState(fog) == state);
    check("an extra reads back exactly", SaveManager::readEntry(fog, "fog/memory.bin") == extra[0].second);
    {
        const TurnDelta t = SaveManager::readTurn(fog, 1);
        check("a sealed turn unpacks", t.provinces.size() == 1 && t.provinces[0].newPopulation == 123456);
        SaveReader r(fog);
        const TurnDelta t2 = r.readTurn(1);
        check("...through SaveReader too", t2.provinces.size() == 1 && t2.provinces[0].newOwner == 3);
    }
    {
        const SaveMetadata m = SaveManager::readMetadata(fog);
        check("treasuries come back from the sealed entry",
              m.countryTreasuries.count(3) && m.countryTreasuries.at(3) == 4210.5 &&
              m.countryTreasuries.count(9) && m.countryTreasuries.at(9) == 17.25);
        check("the salt survives", !m.sealSalt.empty() && m.fogOfWar);
    }
    check("updatePlayerCountry keeps every entry", SaveManager::updatePlayerCountry(fog, 9) &&
              SaveManager::readEntry(fog, "fog/memory.bin") == extra[0].second &&
              SaveManager::readEntry(fog, "rebels.json") == "[]" &&
              SaveManager::readMetadata(fog).countryTreasuries.count(3) == 1);
    {
        SaveMetadata m = SaveManager::readMetadata(fog);
        m.countryTreasuries[3] = 99.0;
        check("updateLastPlayed rewrites the sealed treasuries",
              SaveManager::updateLastPlayed(fog, &m) &&
              SaveManager::readMetadata(fog).countryTreasuries.at(3) == 99.0);
        check("...and keeps the state", SaveManager::readState(fog) == state);
    }

    printf("\n== refused ==\n");
    {
        std::string raw = rawEntry(fog, "state.json");
        raw[raw.size() / 2] ^= 0x01;
        replaceEntry(fog, "state.json", raw);
        check("an edited byte reads as nothing, not as garbage", SaveManager::readState(fog).empty());
    }
    {
        std::string m = rawEntry(fog, "metadata.json");
        const size_t at = m.find("\"salt\": \"");
        if (at != std::string::npos) m[at + 9] = m[at + 9] == 'a' ? 'b' : 'a';
        replaceEntry(fog, "metadata.json", m);
        check("a changed salt opens nothing", SaveManager::readEntry(fog, "fog/memory.bin").empty());
    }

    printf("\n== a world without fog ==\n");
    check("a plain save is created", SaveManager::createSave(plain, tinyMap(), baseMeta("Plain", false)));
    check("a plain turn is appended", SaveManager::appendTurn(plain, oneTurn(1), &state, &extra));
    check("its state is stored as it was", rawEntry(plain, "state.json") == state);
    check("its metadata still carries the treasuries", rawEntry(plain, "metadata.json").find("4210") != std::string::npos);
    check("it has no sealed treasuries entry", rawEntry(plain, "fog/treasuries.json").empty());

    if (!getenv("SEAL_KEEP")) { std::remove(fog.c_str()); std::remove(plain.c_str()); }
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
