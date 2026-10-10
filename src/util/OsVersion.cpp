#include "OsVersion.h"

#include <cstdio>
#include <cstring>
#include <sstream>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#elif defined(__ANDROID__)
#include <sys/system_properties.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/utsname.h>
#elif defined(__unix__)
#include <sys/utsname.h>
#endif

namespace od {

std::string osVersion() {
#if defined(__EMSCRIPTEN__)
    const char* ua = emscripten_run_script_string(
        "(function(){try{return navigator.userAgent||'';}catch(e){return '';}})()");
    return (ua && *ua) ? std::string(ua) : std::string("browser (unknown)");
#elif defined(__ANDROID__)
    char rel[PROP_VALUE_MAX] = {0}, sdk[PROP_VALUE_MAX] = {0};
    char maker[PROP_VALUE_MAX] = {0}, model[PROP_VALUE_MAX] = {0};
    __system_property_get("ro.build.version.release", rel);
    __system_property_get("ro.build.version.sdk", sdk);
    __system_property_get("ro.product.manufacturer", maker);
    __system_property_get("ro.product.model", model);
    std::ostringstream o;
    o << "Android " << rel << " (API " << sdk << "), " << maker << " " << model;
    return o.str();
#elif defined(_WIN32)
    // RtlGetVersion, because GetVersionEx lies to any binary without a
    // compatibility manifest and reports Windows 8.
    using RtlGetVersionFn = LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion"))))
            fn(&vi);
    std::ostringstream o;
    o << "Windows " << vi.dwMajorVersion << "." << vi.dwMinorVersion
      << " build " << vi.dwBuildNumber;
#if defined(_M_ARM64) || defined(__aarch64__)
    o << " arm64";
#elif defined(_M_X64) || defined(__x86_64__)
    o << " x64";
#else
    o << " x86";
#endif
    return o.str();
#elif defined(__APPLE__)
    char ver[64] = {0};
    size_t n = sizeof(ver);
    sysctlbyname("kern.osproductversion", ver, &n, nullptr, 0);
    struct utsname u{};
    uname(&u);
    return std::string("macOS ") + (ver[0] ? ver : "?") + " " + u.machine;
#elif defined(__unix__)
    std::string pretty;
    if (FILE* f = std::fopen("/etc/os-release", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            if (std::strncmp(line, "PRETTY_NAME=", 12) == 0) {
                pretty = line + 12;
                while (!pretty.empty() && (pretty.back() == '\n' || pretty.back() == '"'))
                    pretty.pop_back();
                if (!pretty.empty() && pretty.front() == '"') pretty.erase(0, 1);
                break;
            }
        }
        std::fclose(f);
    }
    struct utsname u{};
    uname(&u);
    std::ostringstream o;
    o << (pretty.empty() ? std::string(u.sysname) : pretty)
      << ", kernel " << u.release << " " << u.machine;
    return o.str();
#else
    return "unknown";
#endif
}

}  // namespace od
