// What the browser build writes to IndexedDB, and how much work that costs.
//
// A .odstate is a player's whole mutable state in one zip. On the desktop it is
// a file they ask for; on the web it is written on a timer, on the frame
// thread, and everything it does is time the game is not drawing. A player
// reported the browser version freezing for minutes at a time, repeatedly, and
// this is the only periodic work in that build big enough to do it -- so the
// three things that decide its cost are checked here rather than left to be
// noticed by whoever reads the profile next:
//
//   1. the shipped content is EXCLUDED, so the build's own megabytes are not
//      re-archived every time a player's file changes,
//   2. files that are already compressed are STORED, because deflating a save
//      -- itself a zip, and the biggest thing in data/ -- is pure cost,
//   3. an unchanged tree has an unchanged FINGERPRINT, which is what lets the
//      write be skipped altogether.
//
// Round-tripping is checked too: all three are optimisations, and an
// optimisation that loses a save is worse than the freeze.

#include "OdState.h"

#include "miniz.h"
#include "miniz_zip.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_checks = 0, g_failed = 0;

void ok(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    ++g_failed;
    printf("  FAIL  %s\n", what.c_str());
}

void section(const char* name) { printf("\n== %s ==\n", name); }

void write(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(body.data(), (std::streamsize)body.size());
}

std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

/** Text that deflates well, so "was it compressed?" is answerable. */
std::string squashable(size_t n) {
    std::string s;
    while (s.size() < n) s += "the same sentence over and over, which deflate loves. ";
    s.resize(n);
    return s;
}

struct Entry {
    std::string name;
    mz_uint64 uncompressed = 0;
    mz_uint64 compressed = 0;
    bool found = false;
};

Entry entryOf(const std::string& archive, const std::string& name) {
    Entry e;
    e.name = name;
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, archive.c_str(), 0)) return e;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (name != st.m_filename) continue;
        e.found = true;
        e.uncompressed = st.m_uncomp_size;
        e.compressed = st.m_comp_size;
        break;
    }
    mz_zip_reader_end(&zip);
    return e;
}

bool hasEntry(const std::string& archive, const std::string& name) {
    return entryOf(archive, name).found;
}

}  // namespace

int main(int argc, char** argv) {
    printf("The archive the web build writes on a timer\n");

    const fs::path tmp = fs::temp_directory_path() /
                         ("odstate-test-" + std::to_string(
                              (unsigned long long)std::hash<std::string>{}(
                                  argc > 0 ? argv[0] : "x")));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    const fs::path data = tmp / "data";
    const std::string archive = (tmp / "state.odstate").string();

    // A data directory shaped like a played install: some of the build's
    // content, some of the player's.
    write(data / "config.json", "{\"minutesPlayed\":12}");
    write(data / "saves" / "Quick Start.odsv", squashable(200000));
    write(data / "custom_maps" / "mine.odmap", squashable(50000));
    write(data / "mods" / "thing.odmod", squashable(30000));
    // Shipped: every one of these is in OD_SHIPPED_DATA, and three of them
    // (lang, dialog, comms) were missing from the exclusion list, which is the
    // bug this file was written for.
    write(data / "lang" / "de.json", squashable(400000));
    write(data / "dialog" / "en" / "intro.oddlg", squashable(100000));
    write(data / "comms" / "cast.json", squashable(20000));
    write(data / "policies.json", squashable(10000));
    write(data / "district_laws.json", squashable(10000));
    write(data / "parties.json", squashable(10000));
    write(data / "menu_bg.png", squashable(300000));
    write(data / "STDmaps" / "map.odmap", squashable(500000));
    write(data / "fonts" / "unifont.ttf", squashable(500000));

    std::string err;
    int count = 0;
    const bool saved = OdState::save(data.string(), archive, err, &count);

    section("what goes in");
    {
        ok(saved, "the state is archived: " + err);
        ok(hasEntry(archive, "config.json"), "the config is the player's");
        ok(hasEntry(archive, "saves/Quick Start.odsv"), "and so are their saves");
        ok(hasEntry(archive, "custom_maps/mine.odmap"), "and their maps");
        ok(hasEntry(archive, "mods/thing.odmod"), "and their mods");
    }

    section("what stays out, because the build already shipped it");
    {
        // Each of these is a directory or file in OD_SHIPPED_DATA. On the web
        // this list decides what gets re-compressed and pushed to IndexedDB
        // every time anything changes, so a name missing from it is not a
        // fatter file -- it is a frame the game did not draw.
        for (const char* name : {"lang/de.json", "dialog/en/intro.oddlg",
                                 "comms/cast.json", "policies.json",
                                 "district_laws.json", "parties.json",
                                 "menu_bg.png", "STDmaps/map.odmap",
                                 "fonts/unifont.ttf"}) {
            ok(!hasEntry(archive, name),
               std::string("shipped, so not archived: ") + name);
        }
    }

    section("already compressed means stored, not deflated again");
    {
        // A .odsv is a zip. Deflating one costs the most CPU of anything in
        // data/ -- the saves are the biggest files there -- and saves nothing,
        // because there is nothing left to find. The test content is text, so
        // if it HAD been deflated the entry would be a fraction of its size:
        // this fails loudly rather than subtly.
        const Entry sv = entryOf(archive, "saves/Quick Start.odsv");
        ok(sv.found && sv.compressed >= sv.uncompressed,
           "a save is stored whole, not re-deflated");
        const Entry md = entryOf(archive, "custom_maps/mine.odmap");
        ok(md.found && md.compressed >= md.uncompressed, "and so is a map");
        const Entry mo = entryOf(archive, "mods/thing.odmod");
        ok(mo.found && mo.compressed >= mo.uncompressed, "and a mod");

        // The other direction, or the check above would pass on an archiver
        // that had simply stopped compressing anything.
        const Entry cfg = entryOf(archive, "config.json");
        ok(cfg.found, "the config is there to compare against");
        write(data / "notes.txt", squashable(100000));
        std::string e2;
        ok(OdState::save(data.string(), archive, e2), "re-archived with a text file");
        const Entry txt = entryOf(archive, "notes.txt");
        ok(txt.found && txt.compressed < txt.uncompressed / 4,
           "and something that is not already compressed still is");
    }

    section("the fingerprint says whether a write is needed at all");
    {
        const std::string a = OdState::fingerprint(data.string());
        ok(!a.empty(), "an install has a fingerprint");
        ok(a == OdState::fingerprint(data.string()),
           "walking an unchanged tree twice gives the same answer");

        // The web build writes the state on a timer and the timer fires
        // whether or not anything happened. This is what tells it not to.
        write(data / "saves" / "another.odsv", squashable(1000));
        const std::string b = OdState::fingerprint(data.string());
        ok(a != b, "a new save changes it");

        fs::remove(data / "saves" / "another.odsv", ec);
        ok(OdState::fingerprint(data.string()) == a, "removing it changes it back");

        // A shipped file is not the player's state, so touching one must not
        // provoke a write of everything they own.
        write(data / "lang" / "fr.json", squashable(5000));
        ok(OdState::fingerprint(data.string()) == a,
           "but a shipped file does not, because it is not in the archive");

        write(data / "config.json", "{\"minutesPlayed\":13}");
        ok(OdState::fingerprint(data.string()) != a, "and an edit does");
    }

    section("and it all comes back");
    {
        std::string e3;
        int stored = 0;
        ok(OdState::save(data.string(), archive, e3, &stored), "archived: " + e3);
        const fs::path fresh = tmp / "restored";
        fs::create_directories(fresh);
        int restored = 0;
        ok(OdState::load(fresh.string(), archive, e3, &restored), "restored: " + e3);
        ok(restored == stored, "every file that went in came back");
        ok(read(fresh / "saves" / "Quick Start.odsv") == squashable(200000),
           "a stored save is byte for byte what it was");
        ok(read(fresh / "config.json") == "{\"minutesPlayed\":13}",
           "and so is a deflated config");
    }

    fs::remove_all(tmp, ec);
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
