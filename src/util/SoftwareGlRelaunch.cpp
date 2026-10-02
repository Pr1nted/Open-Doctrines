#include "SoftwareGlRelaunch.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#  include <unistd.h>
#endif
#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif
#if defined(__FreeBSD__)
#  include <sys/sysctl.h>
#endif

namespace {
std::vector<char*> g_argv;      // argv, kept NUL-terminated for execv
std::string        g_exePath;   // absolute, resolved before any chdir

// Absolute path to this binary, by whatever each OS offers, falling back to
// argv[0]. Resolved ONCE at startup: /proc and the like are cheap, but cwd is
// not guaranteed to stay put, so a relative argv[0] has to be banked early.
std::string resolveExe(const char* argv0) {
#if defined(__linux__)
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
#elif defined(__sun)
    // illumos/Solaris: /proc/self/path/a.out is the canonical self-link.
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/path/a.out", buf, sizeof buf - 1);
    if (n > 0) { buf[n] = '\0'; return buf; }
#endif
    // OpenBSD and every fallback: argv[0]. execvp below does a PATH search when
    // it has no slash, which is the best available answer there.
    return argv0 ? argv0 : "";
}
} // namespace

void odRememberRelaunch(int argc, char** argv) {
    g_argv.clear();
    for (int i = 0; i < argc; ++i) g_argv.push_back(argv[i]);
    g_argv.push_back(nullptr);
    g_exePath = resolveExe(argc > 0 ? argv[0] : nullptr);
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
