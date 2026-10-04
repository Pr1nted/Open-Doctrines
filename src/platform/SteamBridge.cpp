#include "platform/SteamBridge.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
#  include <dlfcn.h>
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#  endif
#  include <climits>
#  include <unistd.h>
#endif

namespace odsteam {
namespace {

// The flat API, declared from its documented C signatures rather than from
// the SDK headers we may not ship. Every one of these has been stable across
// SDK releases; the user-stats ACCESSOR is versioned in its name, so several
// versions are tried, newest first.
using FnInitFlat   = int  (*)(char* errMsg1024);   // ESteamAPIInitResult; 0 = OK
using FnInit       = bool (*)();
using FnVoid       = void (*)();
using FnAccessor   = void* (*)();
using FnSetAch     = bool (*)(void* self, const char* name);
using FnStore      = bool (*)(void* self);

std::mutex g_mutex;
std::atomic<bool> g_active{false};
bool g_tried = false;
void* g_stats = nullptr;
FnVoid g_run = nullptr, g_shutdown = nullptr;
FnSetAch g_set = nullptr;
FnStore g_store = nullptr;
std::set<std::string> g_done;    // set this run, so a re-sync costs nothing
// Grants arrive on the tracker's worker thread; Steam is called from the game
// thread only, so they wait here until the next runCallbacks().
std::vector<std::string> g_queue;

#if defined(_WIN32)
using Lib = HMODULE;
Lib openLib() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
#  if defined(_WIN64)
    const wchar_t* name = L"steam_api64.dll";
#  else
    const wchar_t* name = L"steam_api.dll";
#  endif
    return LoadLibraryW((dir + name).c_str());
}
void* sym(Lib l, const char* n) { return (void*)GetProcAddress(l, n); }
#elif !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
using Lib = void*;
std::string exeDir() {
    char buf[PATH_MAX] = {0};
#  if defined(__APPLE__)
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
#  else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    buf[n] = 0;
#  endif
    std::string s(buf);
    return s.substr(0, s.find_last_of('/') + 1);
}
Lib openLib() {
#  if defined(__APPLE__)
    const char* name = "libsteam_api.dylib";
#  else
    const char* name = "libsteam_api.so";
#  endif
    // Beside the executable ONLY. Searching the system path would pick up
    // whatever some other program left there.
    return dlopen((exeDir() + name).c_str(), RTLD_NOW | RTLD_LOCAL);
}
void* sym(Lib l, const char* n) { return dlsym(l, n); }
#endif

}  // namespace

bool init() {
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    return false;
#else
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_tried) return g_active.load();
    g_tried = true;
    Lib lib = openLib();
    if (!lib) return false;

    bool ok = false;
    if (auto initFlat = (FnInitFlat)sym(lib, "SteamAPI_InitFlat")) {
        char err[1024] = {0};
        ok = initFlat(err) == 0;
        if (!ok && err[0]) std::fprintf(stderr, "Steam: %s\n", err);
    } else if (auto initOld = (FnInit)sym(lib, "SteamAPI_Init")) {
        ok = initOld();
    }
    if (!ok) return false;

    for (const char* acc : {"SteamAPI_SteamUserStats_v013", "SteamAPI_SteamUserStats_v012",
                            "SteamAPI_SteamUserStats_v011"}) {
        if (auto f = (FnAccessor)sym(lib, acc)) { g_stats = f(); if (g_stats) break; }
    }
    g_run = (FnVoid)sym(lib, "SteamAPI_RunCallbacks");
    g_shutdown = (FnVoid)sym(lib, "SteamAPI_Shutdown");
    g_set = (FnSetAch)sym(lib, "SteamAPI_ISteamUserStats_SetAchievement");
    g_store = (FnStore)sym(lib, "SteamAPI_ISteamUserStats_StoreStats");
    // Older SDKs need the current stats requested before a set is accepted;
    // 1.61 removed the call because the client now does it at startup.
    if (auto req = (FnStore)sym(lib, "SteamAPI_ISteamUserStats_RequestCurrentStats"))
        if (g_stats) req(g_stats);
    g_active = g_stats && g_set && g_store;
    std::fprintf(stderr, "Steam: %s\n", g_active ? "achievements available" : "running, but no user stats");
    return g_active.load();
#endif
}

bool active() { return g_active.load(); }

void setAchievement(const char* apiName) {
    if (!g_active.load() || !apiName) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_done.insert(apiName).second) return;
    g_queue.push_back(apiName);
}

void runCallbacks() {
    if (!g_active.load()) return;
    std::vector<std::string> todo;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        todo.swap(g_queue);
    }
    bool any = false;
    for (const auto& n : todo) any = g_set(g_stats, n.c_str()) || any;
    if (any) g_store(g_stats);
    if (g_run) g_run();
}

void shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active.load() && g_shutdown) g_shutdown();
    g_active = false;
}

}  // namespace odsteam
