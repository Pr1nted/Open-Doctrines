#include "SoftwareGlRelaunch.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#  include <climits>
#  include <unistd.h>
#endif
#include "raylib.h"
#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif
#if defined(__FreeBSD__) || defined(__NetBSD__)
#  include <sys/sysctl.h>
#endif
#if defined(__HAIKU__)
#  include <FindDirectory.h>
#endif

namespace {
std::vector<char*> g_argv;      // argv, kept NUL-terminated for execv
std::string        g_exePath;   // absolute, resolved before any chdir

// Absolute path to this binary, by whatever each OS offers, falling back to
// argv[0]. Resolved ONCE at startup: /proc and the like are cheap, but cwd is
// not guaranteed to stay put, so a relative argv[0] has to be banked early.
std::string resolveExe(const char* argv0) {
#if defined(__linux__) || defined(__serenity__)   // SerenityOS's ProcFS has the same link
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) { buf[n] = '\0'; return buf; }
#elif defined(__APPLE__)
    char buf[4096]; uint32_t sz = sizeof buf;
    if (_NSGetExecutablePath(buf, &sz) == 0) return buf;
#elif defined(__FreeBSD__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    char buf[4096]; size_t sz = sizeof buf;
    if (sysctl(mib, 4, buf, &sz, nullptr, 0) == 0) return buf;
#elif defined(__NetBSD__)
    // The same question in NetBSD's words; its /proc is not mounted by default.
    int mib[4] = {CTL_KERN, KERN_PROC_ARGS, -1, KERN_PROC_PATHNAME};
    char buf[4096]; size_t sz = sizeof buf;
    if (sysctl(mib, 4, buf, &sz, nullptr, 0) == 0) return buf;
#elif defined(__sun)
    // illumos/Solaris: /proc/self/path/a.out is the canonical self-link.
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/path/a.out", buf, sizeof buf - 1);
    if (n > 0) { buf[n] = '\0'; return buf; }
#elif defined(__HAIKU__)
    // Haiku has no /proc: the loader is asked for the image this symbol
    // lives in, which for the application's own symbol is the executable.
    char buf[B_PATH_NAME_LENGTH];
    if (find_path(B_APP_IMAGE_SYMBOL, B_FIND_PATH_IMAGE_PATH, nullptr, buf, sizeof buf) == B_OK)
        return buf;
#endif
    // OpenBSD and every fallback: argv[0], made absolute. OpenBSD has no
    // /proc and no sysctl that names the executable, so argv[0] is all there
    // is -- a path when started by path, a bare name when found on PATH, in
    // which case the same PATH search the shell did finds it again. Done now,
    // before anything can chdir, because a relative path means nothing later.
    if (!argv0 || !*argv0) return "";
#if !defined(_WIN32)
    char real[PATH_MAX];
    if (std::strchr(argv0, '/')) {
        if (::realpath(argv0, real)) return real;
        return argv0;
    }
    if (const char* path = std::getenv("PATH")) {
        std::string dirs = path;
        size_t start = 0;
        while (start <= dirs.size()) {
            size_t end = dirs.find(':', start);
            if (end == std::string::npos) end = dirs.size();
            std::string cand = dirs.substr(start, end - start);
            if (cand.empty()) cand = ".";
            cand += '/';
            cand += argv0;
            if (::access(cand.c_str(), X_OK) == 0 && ::realpath(cand.c_str(), real)) return real;
            start = end + 1;
        }
    }
#endif
    return argv0;
}
} // namespace

void odRememberRelaunch(int argc, char** argv) {
    g_argv.clear();
    for (int i = 0; i < argc; ++i) g_argv.push_back(argv[i]);
    g_argv.push_back(nullptr);
    g_exePath = resolveExe(argc > 0 ? argv[0] : nullptr);
}

const std::string& odExecutablePath() { return g_exePath; }

std::string odApplicationDirectory() {
    // raylib's own answer wherever it has one -- Windows, Linux, macOS and
    // FreeBSD. Everywhere else (OpenBSD, NetBSD) raylib returns "" (5.5 and 6.2 alike), and
    // the game then looked for data/ relative to wherever it was STARTED
    // from: fine from its own folder, nothing at all from a desktop launcher
    // or a full path typed elsewhere. The path resolved at startup answers
    // instead.
    std::string dir = GetApplicationDirectory();
    if (dir.empty() && !g_exePath.empty()) {
        const size_t slash = g_exePath.find_last_of("/\\");
        if (slash != std::string::npos) dir = g_exePath.substr(0, slash + 1);
    }
    return dir;
}

bool odSoftwareGlAlreadyTried() {
    return std::getenv("OD_GL_SOFTWARE_TRIED") != nullptr;
}

bool odTrySoftwareGlRelaunch() {
#if defined(_WIN32) || defined(__APPLE__)
    // No llvmpipe to fall back to. The caller keeps its existing message.
    return false;
#else
    if (odSoftwareGlAlreadyTried()) return false;   // already the retry: give up
    if (g_exePath.empty() || g_argv.size() < 2) return false;

    // setenv, not putenv: these outlive this scope into execv, and the guard
    // is what stops the retry from retrying forever.
    ::setenv("OD_GL_SOFTWARE_TRIED", "1", 1);
    ::setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    // GALLIUM_DRIVER names the software rasteriser directly, for a Mesa whose
    // auto-selection still reaches for a GPU that just failed.
    ::setenv("GALLIUM_DRIVER", "llvmpipe", 1);

    ::execv(g_exePath.c_str(), g_argv.data());
    // execv only returns on failure -- e.g. g_exePath was a bare name. One more
    // try through PATH before conceding.
    ::execvp(g_argv[0], g_argv.data());
    return false;
#endif
}
