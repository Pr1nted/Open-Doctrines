// The Greater Diplomacy translation layer, on whatever platform is running it.
//
// Built only when OD_ENABLE_GDTL=ON, because without it every call here is a
// stub and the test would assert that the stubs are stubs.
//
// It translates a real shipped map rather than a fixture: the conversion reads
// a province raster, a nation table and a land/sea mask, and a hand-made
// stand-in for those is a test of the stand-in. The map is the one the game
// ships, found through OD_SOURCE_DIR.
//
// WHAT THIS IS FOR. The desktop flow was checked by hand on macOS. Nothing had
// ever run it on Windows, Linux, Android or the web, and three of those four
// have their own answer to "where does a file go" and "is there a file chooser
// at all". This runs on every platform CI builds, so a change that is fine on
// one and wrong on another is caught by the platform it is wrong on.
#include "Game_Gdtl.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include "miniz.h"
#include "miniz_zip.h"

static int failures = 0;

static void check(bool ok, const char* what) {
    std::printf("  %-4s  %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

static std::string sourceDir() {
#ifdef OD_SOURCE_DIR
    return OD_SOURCE_DIR;
#else
    return ".";
#endif
}

// Every member Game_Loading.cpp insists on. A .odmap missing one of these
// loads as an empty world rather than failing, which is the worst way for a
// translation to be wrong.
static bool archiveHas(const std::string& path, const char* member) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) return false;
    const bool found = mz_zip_reader_locate_file(&zip, member, nullptr, 0) >= 0;
    mz_zip_reader_end(&zip);
    return found;
}

int main() {
    std::printf("=== the translation layer ===\n");

    check(Gdtl::available(), "the library is linked and speaks this ABI");
    if (!Gdtl::available()) {
        std::printf("nothing below can run without it\n");
        return 1;
    }
    std::printf("  note  open-dragoman %s\n", Gdtl::version().c_str());

    // A platform that cannot ask where a file goes must not claim it is about
    // to search anywhere for one. These two answers are shown to the player in
    // the same dialog and disagreeing is how it ends up lying.
    if (!Gdtl::canChooseLocation()) {
        check(Gdtl::searchLocations().empty(),
              "no file chooser, so nothing is offered to search");
        check(Gdtl::findGd5Installations().empty(),
              "no file chooser, so no installation is ever found");
    } else {
        check(!Gdtl::searchLocations().empty(),
              "a desktop offers somewhere to search");
    }

    // Nothing is an installation until it has both marks, and a directory that
    // certainly is not one must not pass.
    check(!Gdtl::looksLikeGd5(""), "an empty path is not an installation");
    check(!Gdtl::looksLikeGd5(sourceDir()), "this repository is not an installation");
    check(Gdtl::mapDestination(sourceDir(), "X").empty(),
          "a non-installation has nowhere to put a map");

    const std::string dataDir = (std::filesystem::path(sourceDir()) / "data").string() + "/";
    const std::string dest = Gdtl::unattendedDestination(dataDir, "Bad/Name:*?");
    check(dest.find('*') == std::string::npos && dest.find('?') == std::string::npos,
          "a destination is made from a name a filesystem will take");

    // ---- the conversion itself ----
    const std::string map = (std::filesystem::path(sourceDir()) / "data" / "STDmaps" /
                             "map.odmap").string();
    if (!std::filesystem::exists(map)) {
        std::printf("  skip  %s is not here\n", map.c_str());
        return failures == 0 ? 0 : 1;
    }

    std::error_code ec;
    const std::filesystem::path work =
        std::filesystem::temp_directory_path(ec) / "od-gdtl-test";
    std::filesystem::remove_all(work, ec);
    std::filesystem::create_directories(work, ec);

    const std::string gd5Dir = (work / "AsGd5").string();
    Gdtl::Result out = Gdtl::toGd5(map, gd5Dir);
    check(out.ok, "the shipped world becomes a Greater Diplomacy 5 map");
    if (!out.ok) std::printf("        %s\n", out.error.c_str());

    if (out.ok) {
        // The four files that game's loader opens by name. A directory of the
        // right shape with the wrong names is not a map.
        for (const char* f : {"map_data.json", "meta.json", "id_map.png"}) {
            check(std::filesystem::exists(std::filesystem::path(gd5Dir) / f),
                  (std::string("it contains ") + f).c_str());
        }
        check(!out.notes.empty(), "it says what did not cross cleanly");

        // The web and Android hand over a zip, so zipping has to work on the
        // platforms that never see a folder picker.
        const std::string zip = (work / "AsGd5.zip").string();
        std::string zipError;
        check(Gdtl::zipDirectory(gd5Dir, zip, zipError), "the map packs into one file");
        if (!zipError.empty()) std::printf("        %s\n", zipError.c_str());
        check(std::filesystem::exists(zip) && std::filesystem::file_size(zip, ec) > 0,
              "the package is not empty");

        // ---- and back ----
        const std::string back = (work / "back.odmap").string();
        Gdtl::Result home = Gdtl::toOdmap(gd5Dir, back);
        check(home.ok, "it comes home as an .odmap");
        if (!home.ok) std::printf("        %s\n", home.error.c_str());
        if (home.ok) {
            for (const char* f : {"land_sea.png", "provinces.png", "provinces.json",
                                  "countries.json"}) {
                check(archiveHas(back, f), (std::string("the archive carries ") + f).c_str());
            }
        }
    }

    // A conversion that failed must not leave half a map behind, and one that
    // worked must not leave the scratch copy either.
    std::filesystem::remove_all(work, ec);

    std::printf("%s\n", failures == 0 ? "all passed" : "SOMETHING FAILED");
    return failures == 0 ? 0 : 1;
}
