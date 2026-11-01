#include "platform/Vita.h"

#if defined(__vita__)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include <psp2/power.h>

extern const char* GAME_VERSION;

// ── MEMORY ──
//
// newlib's heap on the Vita is whatever this says, taken from the app's
// memory budget at start (32 MB if it says nothing). A world of 1641
// provinces, its AI, the software renderer's framebuffers and the fonts'
// atlases need a good deal more; 256 MB leaves the rest of the budget to SDL
// and the threads' stacks.
int _newlib_heap_size_user = 256 * 1024 * 1024;

// A turn resolution recurses deeper than the default main-thread stack
// (256 KB) allows; the web build had to be given 4 MB for the same reason.
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;

namespace {

// i18n-ignore: Vita device paths, not words a player reads.
const char* kShipped = "app0:data";
const char* kCard    = "ux0:data/OpenDoctrines";
const char* kData    = "ux0:data/OpenDoctrines/data";

bool makeDir(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(p.c_str(), 0777) == 0;
}

bool copyFile(const std::string& from, const std::string& to) {
    FILE* in = std::fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = std::fopen(to.c_str(), "wb");
    if (!out) { std::fclose(in); return false; }
    static char buf[256 * 1024];
    bool ok = true;
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, in)) > 0)
        if (std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
    std::fclose(in);
    if (std::fclose(out) != 0) ok = false;
    return ok;
}

bool copyTree(const std::string& from, const std::string& to) {
    if (!makeDir(to)) return false;
    DIR* d = opendir(from.c_str());
    if (!d) return false;
    bool ok = true;
    while (struct dirent* e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        const std::string src = from + "/" + e->d_name, dst = to + "/" + e->d_name;
        struct stat st;
        if (stat(src.c_str(), &st) != 0) { ok = false; continue; }
        ok = (S_ISDIR(st.st_mode) ? copyTree(src, dst) : copyFile(src, dst)) && ok;
    }
    closedir(d);
    return ok;
}

std::string readStamp(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return "";
    char buf[64] = {};
    const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    return std::string(buf, n);
}

}  // namespace

std::string odVitaPrepareData() {
    // Full speed for the copy and for the game: homebrew starts at the
    // system's default clocks, well below what the hardware runs games at.
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    const std::string data = kData;
    const std::string stamp = data + "/.vita-copied";
    if (readStamp(stamp) != GAME_VERSION) {
        std::printf("[VITA] copying %s to %s (first start of %s)\n", kShipped, kData, GAME_VERSION);
        if (!makeDir("ux0:data") || !makeDir(kCard) || !copyTree(kShipped, data)) {
            std::printf("[VITA] the copy failed; starting without data\n");
            return "";
        }
        if (FILE* f = std::fopen(stamp.c_str(), "wb")) {
            std::fputs(GAME_VERSION, f);
            std::fclose(f);
        }
    }
    setenv("OD_DATA_DIR", (data + "/").c_str(), 1);
    return data + "/";
}

#else

std::string odVitaPrepareData() { return ""; }

#endif
