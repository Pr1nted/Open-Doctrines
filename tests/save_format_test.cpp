// The per-turn state diffs and the in-place append, held to what they promise.
//
// WHAT IS CHECKED
//
//  - savedelta round-trips any input exactly, edits and all.
//  - A long run of turns stores most states as diffs, a full snapshot every
//    32nd, and every turn's state reads back byte for byte.
//  - A save in the old layout -- a full turns/s_NNNNN.json every turn -- reads
//    exactly as before. Saves already on players' disks must keep working.
//  - Re-appending a turn (a rewind, a replay) changes that turn and nothing
//    else: the turn after it, stored as a diff of the old one, still reads as
//    it was.
//  - An append cut short at ANY byte reads as the save before it, and the next
//    append repairs the file. That is the guarantee the write-then-rename gave.
//  - The archive a plain zip reader sees -- an older build's view -- holds the
//    turn deltas and state.json, with no duplicate names.
//  - The dead bytes appends leave behind are compacted away.

#include "SaveManager.h"

#include "miniz.h"
#include "miniz_zip.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

int g_checks = 0, g_failed = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failed; std::printf("FAIL  %s\n", what); }
    else std::printf("ok    %s\n", what);
}

std::string readAll(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
void writeAll(const std::string& p, const std::string& d) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(d.data(), (std::streamsize)d.size());
}

// A state that changes a little each turn, the way the game's does: numbers
// move, an entry appears now and then, the rest stays put.
std::string stateFor(int turn, std::mt19937& rng) {
    std::string s = "{\"turn\":" + std::to_string(turn) + ",\"countries\":[";
    for (int c = 0; c < 120; ++c) {
        if (c) s += ",";
        const int drift = (int)(rng() % 7 == 0) * (int)(rng() % 1000);
        s += "{\"id\":" + std::to_string(c) + ",\"name\":\"Country number " + std::to_string(c) +
             "\",\"treasury\":" + std::to_string(c * 1000 + turn * 3 + drift) +
             ",\"research\":\"node_" + std::to_string((c + turn / 10) % 40) + "\"}";
    }
    s += "],\"log\":[";
    for (int i = 0; i < turn % 25; ++i) s += (i ? ",\"" : "\"") + std::string("event ") + std::to_string(i) + "\"";
    return s + "]}";
}

TurnDelta deltaFor(int turn) {
    TurnDelta d;
    d.turnNumber = turn;
    ProvinceDelta p;
    p.provinceId = turn % 500;
    p.ownerChanged = true;
    p.newOwner = turn % 30;
    d.provinces.push_back(p);
    return d;
}

std::vector<std::string> names(const std::string& path) {
    std::vector<std::string> out;
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, path.c_str(), 0)) return out;
    char buf[256];
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); ++i)
        if (mz_zip_reader_get_filename(&z, i, buf, sizeof buf)) out.push_back(buf);
    mz_zip_reader_end(&z);
    return out;
}

bool fresh(const std::string& path) {
    std::filesystem::remove(path);
    SaveMetadata m;
    m.saveName = "format test";
    return SaveManager::createSave(path, std::string(4096, 'M'), m);
}

}  // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "od_save_format_test";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "w.odsv").string();
    std::mt19937 rng(20261101);

    // ── the codec alone ──
    {
        bool all = true;
        for (int i = 0; i < 300 && all; ++i) {
            std::string base(rng() % 5000, '\0'), target;
            for (auto& c : base) c = (char)(rng() % 4 + 'a');
            target = base;
            for (int e = (int)(rng() % 20); e-- > 0;) {
                const size_t at = target.empty() ? 0 : rng() % target.size();
                switch (rng() % 3) {
                    case 0: target.insert(at, std::string(rng() % 50, (char)('A' + rng() % 26))); break;
                    case 1: if (!target.empty()) target.erase(at, rng() % 40); break;
                    default: if (!target.empty()) target[at] = (char)rng(); break;
                }
            }
            if (rng() % 10 == 0) target.swap(base);
            std::string back;
            all = savedelta::decode(base, savedelta::encode(base, target), back) && back == target;
        }
        check(all, "savedelta: 300 random edits all decode to exactly their target");
        std::string out;
        check(savedelta::decode("", savedelta::encode("", ""), out) && out.empty(), "savedelta: empty to empty");
        check(!savedelta::decode("abc", std::string("\x03\x40", 2), out), "savedelta: a copy past the base is refused");
    }

    // ── a long run of turns ──
    std::vector<std::string> states(101);
    check(fresh(path), "a new save is created");
    for (int t = 1; t <= 100; ++t) {
        states[t] = stateFor(t, rng);
        SaveManager::appendTurn(path, deltaFor(t), &states[t]);
    }
    {
        int exact = 0;
        for (int t = 1; t <= 100; ++t) exact += SaveManager::readTurnState(path, t) == states[t];
        check(exact == 100, "every one of 100 turns reads back exactly");
        const auto n = names(path);
        const std::set<std::string> s(n.begin(), n.end());
        check(s.size() == n.size(), "no entry name appears twice");
        check(s.count("turns/s_00001.json") && s.count("turns/s_00032.json") && s.count("turns/s_00064.json") &&
              s.count("turns/s_00096.json"), "a full snapshot on the first turn and every 32nd");
        check(s.count("turns/sd_00002.bin") && s.count("turns/sd_00050.bin") && !s.count("turns/s_00050.json"),
              "the turns between are diffs");
        check(SaveManager::readState(path) == states[100], "state.json is whole and current");
        check(SaveManager::readMetadata(path).turnCount == 100, "the metadata counts 100 turns");
        SaveReader r(path);
        check(r.hasTurnState(50) && r.hasTurnState(64) && !r.hasTurnState(101), "hasTurnState sees both forms");
        check(r.readTurn(77).turnNumber == 77, "turn deltas read as before");
    }

    // ── the old layout ──
    {
        const std::string old = (dir / "old.odsv").string();
        std::filesystem::remove(old);
        mz_zip_archive z{};
        mz_zip_writer_init_file(&z, old.c_str(), 0);
        const std::string meta = "{\"save_name\":\"old\",\"turn_count\":3}";
        mz_zip_writer_add_mem(&z, "map.odmap", "MAP", 3, MZ_NO_COMPRESSION);
        mz_zip_writer_add_mem(&z, "metadata.json", meta.data(), meta.size(), MZ_BEST_COMPRESSION);
        for (int t = 1; t <= 3; ++t) {
            char n[40];
            std::snprintf(n, sizeof n, "turns/s_%05d.json", t);
            mz_zip_writer_add_mem(&z, n, states[t].data(), states[t].size(), MZ_BEST_COMPRESSION);
            const auto packed = SaveManager::packTurn(deltaFor(t));
            std::snprintf(n, sizeof n, "turns/t_%05d.dat", t);
            mz_zip_writer_add_mem(&z, n, packed.data(), packed.size(), MZ_BEST_COMPRESSION);
        }
        mz_zip_writer_finalize_archive(&z);
        mz_zip_writer_end(&z);
        check(SaveManager::readTurnState(old, 2) == states[2], "an old-layout save's turn reads as before");
        // ...and carrying on playing it diffs against the old full snapshots.
        SaveManager::appendTurn(old, deltaFor(4), &states[4]);
        check(SaveManager::readTurnState(old, 4) == states[4] && SaveManager::readTurnState(old, 3) == states[3],
              "playing on from an old save keeps every turn exact");
    }

    // ── a rewind re-appends a turn ──
    {
        const std::string changed = stateFor(20, rng) + " (played differently)";
        SaveManager::appendTurn(path, deltaFor(20), &changed);
        check(SaveManager::readTurnState(path, 20) == changed, "the re-appended turn reads as the new state");
        int exact = 0;
        for (int t = 21; t <= 100; ++t) exact += SaveManager::readTurnState(path, t) == states[t];
        check(exact == 80, "turns after it still read exactly (the next one was re-anchored)");
        check(SaveManager::readTurnState(path, 19) == states[19], "turns before it are untouched");
        states[20] = changed;
    }

    // ── an append cut short anywhere ──
    {
        const std::string before = readAll(path);
        // The count before this append. (Re-appending turn 20 above already
        // raised it: appendTurn counts every append, a long-standing rule this
        // test does not change.)
        const int countBefore = SaveManager::readMetadata(path).turnCount;
        const std::string next = stateFor(101, rng);
        SaveManager::appendTurn(path, deltaFor(101), &next);
        const std::string after = readAll(path);
        check(after.size() > before.size() && after.compare(0, before.size(), before) == 0,
              "an append leaves every existing byte where it was");
        const std::string cut = (dir / "cut.odsv").string();
        int good = 0, tried = 0;
        for (size_t at = before.size() + 1; at < after.size(); at += std::max<size_t>(1, (after.size() - before.size()) / 97)) {
            ++tried;
            writeAll(cut, after.substr(0, at));
            const int tc = SaveManager::readMetadata(cut).turnCount;
            const bool s100 = SaveManager::readTurnState(cut, 100) == states[100];
            const bool no101 = SaveManager::readTurnState(cut, 101).empty();
            if (tc == countBefore && s100 && no101) ++good;
            else if (std::getenv("OD_FORMAT_TEST_VERBOSE"))
                std::printf("      cut at +%zu of %zu: turn_count=%d state100=%d no101=%d\n",
                            at - before.size(), after.size() - before.size(), tc, s100, no101);
        }
        check(tried > 50 && good == tried, "cut at any of ~100 points, it reads as the save before the append");
        writeAll(cut, after.substr(0, before.size() + (after.size() - before.size()) / 2));
        const std::string again = stateFor(101, rng);
        check(SaveManager::appendTurn(cut, deltaFor(101), &again) && SaveManager::readTurnState(cut, 101) == again &&
              SaveManager::readTurnState(cut, 50) == states[50],
              "the next append over a cut file repairs it and keeps the history");
        mz_zip_archive z{};
        check(mz_zip_reader_init_file(&z, cut.c_str(), 0) && mz_zip_validate_archive(&z, 0),
              "...and leaves an archive any zip reader validates");
        mz_zip_reader_end(&z);
    }

    // ── what an older build sees ──
    {
        mz_zip_archive z{};
        const bool open = mz_zip_reader_init_file(&z, path.c_str(), 0);
        bool deltas = open;
        char n[40];
        for (int t = 1; open && t <= 101; ++t) {
            std::snprintf(n, sizeof n, "turns/t_%05d.dat", t);
            deltas = deltas && mz_zip_reader_locate_file(&z, n, nullptr, 0) >= 0;
        }
        check(open && deltas && mz_zip_reader_locate_file(&z, "state.json", nullptr, 0) >= 0,
              "a plain zip reader finds every turn delta and state.json");
        check(open && mz_zip_validate_archive(&z, 0), "...and validates the archive");
        mz_zip_reader_end(&z);
    }

    // ── dead bytes are compacted ──
    {
        std::vector<std::string> later(401);
        for (int t = 102; t <= 400; ++t) {
            later[t] = stateFor(t, rng);
            SaveManager::appendTurn(path, deltaFor(t), &later[t]);
        }
        mz_zip_archive z{};
        mz_zip_reader_init_file(&z, path.c_str(), 0);
        uint64_t live = 0;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); ++i) {
            mz_zip_archive_file_stat st{};
            mz_zip_reader_file_stat(&z, i, &st);
            live += st.m_comp_size + 30 + std::strlen(st.m_filename);
        }
        mz_zip_reader_end(&z);
        const uint64_t size = std::filesystem::file_size(path);
        check(size - live <= std::max<uint64_t>(1u << 20, size / 4 + 100000),
              "dead bytes stay under the compaction bound (1 MB, or a quarter of the file)");
        int exact = 0;
        for (int t = 102; t <= 400; ++t) exact += SaveManager::readTurnState(path, t) == later[t];
        check(exact == 299, "and every one of turns 102-400 still reads exactly");
    }

    // ── compaction, on a save big enough to need it ──
    {
        const std::string big = (dir / "big.odsv").string();
        fresh(big);
        // 48 KB that never changes and does not compress, so every turn's
        // replaced state.json is 48 KB of dead bytes and the file passes the
        // 1 MB floor below which nothing is compacted.
        std::string filler(48 * 1024, '\0');
        for (auto& c : filler) c = (char)('!' + rng() % 90);
        std::vector<std::string> st(121);
        uint64_t largest = 0;
        for (int t = 1; t <= 120; ++t) {
            st[t] = stateFor(t, rng) + filler;
            SaveManager::appendTurn(big, deltaFor(t), &st[t]);
            largest = std::max<uint64_t>(largest, std::filesystem::file_size(big));
        }
        int exact = 0;
        for (int t = 1; t <= 120; ++t) exact += SaveManager::readTurnState(big, t) == st[t];
        check(exact == 120, "a save that was compacted along the way reads every turn exactly");
        // Uncompacted it would hold 120 dead copies of state.json, ~6 MB.
        check(largest < 4u << 20, "and it never grew past 4 MB (compaction ran)");

        // An append longer than the 64 KB a zip reader searches for the end
        // record -- a late-game turn writes over a megabyte -- cut short. Only
        // openSaveFile's own search finds the archive inside it.
        std::string wide(200 * 1024, '\0');
        for (auto& c : wide) c = (char)('!' + rng() % 90);
        // The first such append that went in place -- a big one may instead be
        // the compacting rewrite, which has nothing to cut.
        std::string before, after, prev = st[120];
        int countBefore = 0, turn = 0;
        for (int t = 121; t <= 130 && !turn; ++t) {
            before = readAll(big);
            countBefore = SaveManager::readMetadata(big).turnCount;
            const std::string next = stateFor(t, rng) + wide;
            SaveManager::appendTurn(big, deltaFor(t), &next);
            after = readAll(big);
            if (after.size() > before.size() + 70 * 1024 && after.compare(0, before.size(), before) == 0) turn = t;
            else prev = next;
        }
        const std::string cut = (dir / "bigcut.odsv").string();
        int good = 0, tried = 0;
        bool far = turn != 0;
        if (far)
            for (size_t at = before.size() + 70 * 1024; at < after.size(); at += 9973) {
                ++tried;
                writeAll(cut, after.substr(0, at));
                good += SaveManager::readMetadata(cut).turnCount == countBefore &&
                        SaveManager::readTurnState(cut, turn - 1) == prev &&
                        SaveManager::readTurnState(cut, turn).empty();
            }
        if (std::getenv("OD_FORMAT_TEST_VERBOSE"))
            std::printf("      in place at turn %d: before %zu after %zu tried %d good %d\n", turn, before.size(),
                        after.size(), tried, good);
        check(far && tried >= 10 && good == tried,
              "an append of over 64 KB cut short still reads as the save before it");
    }

    std::filesystem::remove_all(dir);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
