// A real campaign's save, written again turn by turn -- timed, measured, and
// checked byte for byte.
//
//   SaveStreamBench <source.odsv> <out.odsv> [turns]
//
// Reads every turn the source holds -- its turns/t_NNNNN.dat delta and its
// turns/s_NNNNN.json snapshot -- and feeds them through SaveManager::appendTurn
// into a fresh save made from the source's own map, in order, exactly as a game
// would have written them. Prints the archive size and the append time as it
// grows, then reads every turn's snapshot back out of the new save and fails
// unless each is the source's, byte for byte.
//
// WHY A REAL SAVE. The cost being measured -- how big a turn makes the save and
// how long appending it takes -- depends on how much consecutive snapshots have
// in common, which a synthetic state cannot stand in for. data/saves/Quick
// Start.odsv is 820 turns of a real campaign.

#include "SaveManager.h"

#include "miniz.h"
#include "miniz_zip.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::string entry(mz_zip_archive& z, const std::string& name) {
    size_t n = 0;
    void* p = mz_zip_reader_extract_file_to_heap(&z, name.c_str(), &n, 0);
    if (!p) return {};
    std::string s((const char*)p, n);
    mz_free(p);
    return s;
}

std::string tag(int t) {
    char b[8];
    std::snprintf(b, sizeof b, "%05d", t);
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: SaveStreamBench <source.odsv> <out.odsv> [turns]\n");
        return 2;
    }
    const std::string src = argv[1], out = argv[2];
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, src.c_str(), 0)) {
        std::fprintf(stderr, "cannot open %s\n", src.c_str());
        return 2;
    }
    SaveMetadata meta = SaveManager::readMetadata(src);
    int turns = meta.turnCount;
    if (argc > 3) turns = std::min(turns, std::atoi(argv[3]));
    const std::string odm = entry(z, "map.odmap");

    std::filesystem::remove(out);
    SaveMetadata fresh = meta;
    fresh.turnCount = 0;
    if (!SaveManager::createSave(out, odm, fresh)) {
        std::fprintf(stderr, "createSave failed\n");
        return 1;
    }

    std::vector<std::string> states(turns + 1);
    double total = 0.0, window = 0.0;
    int written = 0;
    std::printf("turn  archive_MB  append_ms(avg of last 50)\n");
    for (int t = 1; t <= turns; ++t) {
        TurnDelta d = SaveManager::readTurn(src, t);
        if (d.turnNumber != t) continue;
        states[t] = entry(z, "turns/s_" + tag(t) + ".json");
        const std::string* st = states[t].empty() ? nullptr : &states[t];
        const auto a = std::chrono::steady_clock::now();
        if (!SaveManager::appendTurn(out, d, st)) {
            std::fprintf(stderr, "appendTurn failed at turn %d\n", t);
            return 1;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        total += ms;
        window += ms;
        ++written;
        if (t % 50 == 0 || t == turns) {
            std::printf("%4d  %10.2f  %8.2f\n", t, std::filesystem::file_size(out) / 1e6, window / 50.0);
            window = 0.0;
        }
    }
    mz_zip_reader_end(&z);
    std::printf("total: %d turns appended in %.2f s, final archive %.2f MB\n",
                written, total / 1000.0, std::filesystem::file_size(out) / 1e6);

    int checked = 0, bad = 0;
    const auto a = std::chrono::steady_clock::now();
    for (int t = 1; t <= turns; ++t) {
        if (states[t].empty()) continue;
        ++checked;
        if (SaveManager::readTurnState(out, t) != states[t]) {
            if (++bad <= 5) std::fprintf(stderr, "FAIL: turn %d's snapshot does not read back exactly\n", t);
        }
    }
    const double readMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
    std::printf("read back %d snapshots in %.2f s (%.2f ms each): %d wrong\n",
                checked, readMs / 1000.0, checked ? readMs / checked : 0.0, bad);
    return bad == 0 ? 0 : 1;
}
