#include "OdFile.h"
#include "raylib.h"
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#if defined(PLATFORM_ANDROID)
#include <android_native_app_glue.h>
// Defined in raylib's rcore_android.c but not declared in raylib.h. The app
// carries activity->internalDataPath, which is the only writable location an
// Android app has and where first-run extraction puts everything.
extern "C" struct android_app *GetAndroidApp(void);
#endif

namespace odFile {

std::string readAll(const std::string& path) {
    if (path.empty()) return {};
    int size = 0;
    // LoadFileData rather than ifstream: on Android this reaches
    // AAssetManager through raylib's fopen redirection, which is the only way
    // to read anything shipped inside the APK. See OdFile.h.
    unsigned char* data = LoadFileData(path.c_str(), &size);
    if (!data) return {};
    std::string out(reinterpret_cast<const char*>(data), size > 0 ? (size_t)size : 0u);
    UnloadFileData(data);
    return out;
}

bool exists(const std::string& path) {
    if (path.empty()) return false;
    // FileExists consults the same layer LoadFileData does, so an asset inside
    // the APK answers true here even though it has no filesystem path.
    return FileExists(path.c_str());
}

bool writeAll(const std::string& path, const std::string& data) {
    if (path.empty()) return false;
    // SaveFileData goes through the same redirection in the other direction:
    // on Android a "w" open is rewritten to the app's internal storage.
    return SaveFileData(path.c_str(), const_cast<char*>(data.data()), (int)data.size());
}

namespace {

#if defined(PLATFORM_ANDROID)
std::string androidInternalRoot() {
    struct android_app* app = GetAndroidApp();
    if (app && app->activity && app->activity->internalDataPath)
        return std::string(app->activity->internalDataPath) + "/";
    return {};
}

// mkdir -p. Assets arrive as "flags/POL.png", so the directory has to exist
// before the file can be written into it.
void makeDirs(const std::string& full) {
    for (size_t i = 1; i < full.size(); ++i) {
        if (full[i] != '/') continue;
        const std::string part = full.substr(0, i);
        ::mkdir(part.c_str(), 0770);
    }
}
#endif

}  // namespace

std::string writableRoot(const std::string& desktopDataDir) {
#if defined(PLATFORM_ANDROID)
    const std::string r = androidInternalRoot();
    if (!r.empty()) return r;
#endif
    return desktopDataDir;
}

int extractAssetsOnce(const std::string& stamp) {
#if !defined(PLATFORM_ANDROID)
    (void)stamp;
    return 0;   // every other platform already has a real filesystem
#else
    const std::string root = androidInternalRoot();
    if (root.empty()) return 0;

    // Already done for THIS build? The stamp carries a version so an upgrade
    // re-extracts rather than leaving the player on the previous release's
    // maps and model.
    const std::string stampPath = root + ".assets_stamp";
    {
        FILE* f = ::fopen(stampPath.c_str(), "rb");
        if (f) {
            char buf[256] = {0};
            const size_t n = ::fread(buf, 1, sizeof(buf) - 1, f);
            ::fclose(f);
            if (std::string(buf, n) == stamp) return 0;
        }
    }

    // The manifest is itself an asset, read through AAssetManager.
    const std::string list = readAll("assets_manifest.txt");
    if (list.empty()) {
        TraceLog(LOG_ERROR, "assets_manifest.txt missing from the APK; "
                            "nothing can be extracted");
        return 0;
    }

    int written = 0, failedRead = 0, failedWrite = 0;
    size_t pos = 0;
    while (pos < list.size()) {
        size_t nl = list.find('\n', pos);
        if (nl == std::string::npos) nl = list.size();
        std::string rel = list.substr(pos, nl - pos);
        pos = nl + 1;
        while (!rel.empty() && (rel.back() == '\r' || rel.back() == ' ')) rel.pop_back();
        if (rel.empty()) continue;

        // readAll goes to AAssetManager; the plain fopen below deliberately
        // does NOT, because it must land on the real filesystem.
        const std::string bytes = readAll(rel);
        if (bytes.empty()) {
            // Named, not counted. A silent skip here is how 61 files went
            // missing and only turned up as "failed to open map.odmap" much
            // later, with nothing pointing at the extraction that dropped them.
            TraceLog(LOG_WARNING, "extract: could not READ asset %s", rel.c_str());
            failedRead++;
            continue;
        }

        const std::string dest = root + rel;
        makeDirs(dest);
        FILE* out = ::fopen(dest.c_str(), "wb");
        if (!out) {
            TraceLog(LOG_WARNING, "extract: could not WRITE %s", dest.c_str());
            failedWrite++;
            continue;
        }
        const size_t wrote = ::fwrite(bytes.data(), 1, bytes.size(), out);
        ::fclose(out);
        if (wrote != bytes.size()) {
            TraceLog(LOG_WARNING, "extract: short write %s (%zu of %zu)",
                     dest.c_str(), wrote, bytes.size());
            failedWrite++;
            continue;
        }
        written++;
    }

    if (FILE* f = ::fopen(stampPath.c_str(), "wb")) {
        ::fwrite(stamp.data(), 1, stamp.size(), f);
        ::fclose(f);
    }
    TraceLog(LOG_INFO, "extracted %d asset(s) to %s (%d unreadable, %d unwritable)",
             written, root.c_str(), failedRead, failedWrite);
    return written;
#endif
}

}  // namespace odFile
