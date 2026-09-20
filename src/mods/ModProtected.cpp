#include "ModProtected.h"

#include <algorithm>
#include <map>
#include <mutex>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
// emscripten_get_heap_size is declared HERE, not in emscripten.h. Without
// this the web build fails with "use of undeclared identifier" -- on a
// target no developer machine compiles, so it was only ever going to be
// found in CI.
#include <emscripten/heap.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#include <sys/stat.h>

namespace odprotected {
namespace {

// A mod runs on the game thread today, but the host functions are reachable
// from the async pump and the mod tests drive them directly, so the counters
// take a lock. They are touched once per call and never in a loop the game
// depends on.
std::mutex g_lock;
std::map<std::string, Usage> g_usage;

}  // namespace

void record(const std::string& modId, Call which) {
    std::lock_guard<std::mutex> g(g_lock);
    Usage& u = g_usage[modId];
    u.modId = modId;
    switch (which) {
        case Call::ProcessBytes: ++u.processBytes; break;
        case Call::ImageBytes:   ++u.imageBytes;   break;
        case Call::ModCount:     ++u.modCount;     break;
        case Call::ModId:        ++u.modId_;       break;
        case Call::ModName:      ++u.modName;      break;
    }
}

std::vector<Usage> usage() {
    std::lock_guard<std::mutex> g(g_lock);
    std::vector<Usage> out;
    out.reserve(g_usage.size());
    for (const auto& [id, u] : g_usage) out.push_back(u);
    // std::map already orders by id; stated rather than assumed, because a
    // console that lists mods in a different order each time it opens reads
    // as a different report each time.
    std::sort(out.begin(), out.end(),
              [](const Usage& a, const Usage& b) { return a.modId < b.modId; });
    return out;
}

void reset() {
    std::lock_guard<std::mutex> g(g_lock);
    g_usage.clear();
}

uint64_t processBytes() {
#if defined(__EMSCRIPTEN__)
    // The browser heap, which is the only figure the web build can see. It is
    // not resident memory and does not pretend to be.
    return (uint64_t)emscripten_get_heap_size();
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (uint64_t)pmc.WorkingSetSize;
    return 0;
#else
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    // ru_maxrss is BYTES on macOS and KILOBYTES on Linux. Getting this wrong
    // is a factor of 1024 in a number a mod may show a player -- the same
    // distinction Game_Loading.cpp's [MEM] line already has to make.
#if defined(__APPLE__)
    return (uint64_t)ru.ru_maxrss;
#else
    return (uint64_t)ru.ru_maxrss * 1024ull;
#endif
#endif
}

uint64_t imageBytes() {
#if defined(__EMSCRIPTEN__)
    // There is no executable on disk to measure. 0 means unknown, which is
    // honest; inventing the wasm blob's size would answer a question nobody
    // asked.
    return 0;
#else
    std::string path;
#if defined(_WIN32)
    char buf[MAX_PATH] = {0};
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    path.assign(buf, n);
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t n = sizeof(buf);
    if (_NSGetExecutablePath(buf, &n) != 0) return 0;
    path = buf;
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = '\0';
    path = buf;
#endif
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return 0;
    return (uint64_t)st.st_size;
#endif
}

}  // namespace odprotected
