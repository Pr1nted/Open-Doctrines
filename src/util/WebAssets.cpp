#include "WebAssets.h"
#include "util/LoadLog.h"

#include <iostream>
#include <unordered_set>

#include "raylib.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#ifdef __EMSCRIPTEN__
// Function-local rather than a file-scope object so the set is built on first
// use; this file is linked into the map tools as well as the game.
static std::unordered_set<std::string>& unavailable() {
    static std::unordered_set<std::string> s;
    return s;
}
#endif

void odForgetAsset(const std::string& path) {
#ifdef __EMSCRIPTEN__
    unavailable().erase(path);
#else
    (void)path;
#endif
}

bool odEnsureAsset(const std::string& path) {
    if (path.empty()) return false;
    if (FileExists(path.c_str())) return true;

#ifndef __EMSCRIPTEN__
    // Every other platform ships these files in the install. A missing one is a
    // broken install, and inventing a download for it would hide that.
    return false;
#else
    // Asked once, answered no. Without this a caller in a per-frame path -- the
    // flag renderer retrying a country whose art will never arrive -- turns one
    // 404 into one request per frame, forever. Audio.cpp learned this the hard
    // way with streamed music; the note there is worth reading.
    //
    // A caller that knows its request was worth retrying clears its own entry;
    // see odForgetAsset().
    if (unavailable().count(path)) return false;

    // The deployed copy mirrors the VFS layout exactly, so the URL is the VFS
    // path minus its leading slash and neither side has to translate. See the
    // POST_BUILD copy in CMakeLists.txt -- a deployment that uploads only
    // OpenDoctrines.{html,js,wasm,data} lands here for every scenario.
    const std::string url = path[0] == '/' ? path.substr(1) : path;

    // The directory has to exist before anything can be written into it, and it
    // may not: the file packager creates a directory only for files it actually
    // packs, so excluding the single file under data/ai/ leaves no data/ai at
    // all. FS is a symbol of the generated JS rather than an exported runtime
    // method, which is exactly the scope EM_ASM bodies are emitted into.
    const std::string::size_type slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        const std::string dir = path.substr(0, slash);
        EM_ASM({ try { FS.mkdirTree(UTF8ToString($0)); } catch (e) {} }, dir.c_str());
    }

    emscripten_wget(url.c_str(), path.c_str());

    if (FileExists(path.c_str())) return true;
    unavailable().insert(path);
    LoadLog() << "[web] could not download " << url << std::endl;
    return false;
#endif
}
