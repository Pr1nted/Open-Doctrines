#pragma once

#include "LoadLog.h"
#include <cstddef>
#include <cstdint>
#include <climits>

/**
 * A one-shot platform / ABI report for the logs.
 *
 * WHY. When a bug depends on how wide a type is -- a save field that assumed
 * 8-byte long, an overflow that only bites where int is 16-bit, a hash that
 * differs by endianness -- the first question is always "what did this machine
 * actually compile with?", and until now the only answer was to ask the person
 * to run a one-off probe. This prints the whole table once at startup: every
 * fundamental type's size in bytes AND bits, plus enough compiler / arch / OS /
 * data-model detail to tell WHICH machine built the binary without asking.
 *
 * It goes through LoadLog, so it reaches stdout (the captured log) and, once the
 * game installs its console sink, the in-game console too. Every line is tagged
 * [PLATFORM] so it greps out of a long load log in one line. Called once from
 * each entry point's main(); cheap, no allocations beyond LoadLog's own line
 * buffer, and nothing downstream depends on it -- it is pure diagnostics.
 */
inline void logPlatformInfo() {
    auto bits = [](std::size_t bytes) -> unsigned {
        return (unsigned)(bytes * (std::size_t)CHAR_BIT);
    };

    LoadLog() << "[PLATFORM] OpenDoctrines build ABI report" << "\n";

    // --- who built this, and targeting what ---
#if defined(__clang__)
    LoadLog() << "[PLATFORM] compiler: clang " << __clang_major__ << "."
              << __clang_minor__ << "." << __clang_patchlevel__ << "\n";
#elif defined(_MSC_VER)
    LoadLog() << "[PLATFORM] compiler: MSVC " << (int)_MSC_VER << "\n";
#elif defined(__GNUC__)
    LoadLog() << "[PLATFORM] compiler: gcc " << __GNUC__ << "."
              << __GNUC_MINOR__ << "." << __GNUC_PATCHLEVEL__ << "\n";
#else
    LoadLog() << "[PLATFORM] compiler: unknown" << "\n";
#endif
    LoadLog() << "[PLATFORM] C++ standard (__cplusplus): " << (long)__cplusplus << "\n";

#if defined(__EMSCRIPTEN__)
    LoadLog() << "[PLATFORM] os: Emscripten / wasm" << "\n";
#elif defined(_WIN32)
    LoadLog() << "[PLATFORM] os: Windows" << "\n";
#elif defined(__APPLE__)
    LoadLog() << "[PLATFORM] os: Apple / macOS" << "\n";
#elif defined(__linux__)
    LoadLog() << "[PLATFORM] os: Linux" << "\n";
#else
    LoadLog() << "[PLATFORM] os: unknown" << "\n";
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    LoadLog() << "[PLATFORM] arch: arm64 (aarch64)" << "\n";
#elif defined(__x86_64__) || defined(_M_X64)
    LoadLog() << "[PLATFORM] arch: x86-64" << "\n";
#elif defined(__arm__) || defined(_M_ARM)
    LoadLog() << "[PLATFORM] arch: arm (32-bit)" << "\n";
#elif defined(__i386__) || defined(_M_IX86)
    LoadLog() << "[PLATFORM] arch: x86 (32-bit)" << "\n";
#elif defined(__wasm__)
    LoadLog() << "[PLATFORM] arch: wasm" << "\n";
#else
    LoadLog() << "[PLATFORM] arch: unknown" << "\n";
#endif

    // data model: pointer width tells 32- vs 64-bit; endianness is read, not assumed.
    LoadLog() << "[PLATFORM] pointer width: " << (unsigned)sizeof(void*)
              << " bytes (" << bits(sizeof(void*)) << "-bit)" << "\n";
    {
        const std::uint32_t one = 1u;
        const bool little = (*reinterpret_cast<const unsigned char*>(&one) == 1u);
        LoadLog() << "[PLATFORM] endianness: " << (little ? "little" : "big") << "\n";
    }
    LoadLog() << "[PLATFORM] CHAR_BIT: " << (int)CHAR_BIT << " bits per byte" << "\n";

    // --- the fundamental type sizes this binary actually uses ---
    // One row each: bytes and bits. signed/unsigned listed where the game relies
    // on the distinction, so a width assumption in the save/wire code is visible.
#define OD_SZ(label, T)                                                      \
    LoadLog() << "[PLATFORM] sizeof " label ": " << (unsigned)sizeof(T)      \
              << " bytes (" << bits(sizeof(T)) << "-bit)" << "\n"
    OD_SZ("bool",                bool);
    OD_SZ("char",                char);
    OD_SZ("signed char",         signed char);
    OD_SZ("unsigned char",       unsigned char);
    OD_SZ("short",               short);
    OD_SZ("unsigned short",      unsigned short);
    OD_SZ("int",                 int);
    OD_SZ("unsigned int",        unsigned int);
    OD_SZ("long",                long);
    OD_SZ("unsigned long",       unsigned long);
    OD_SZ("long long",           long long);
    OD_SZ("unsigned long long",  unsigned long long);
    OD_SZ("float",               float);
    OD_SZ("double",              double);
    OD_SZ("long double",         long double);
    OD_SZ("void*",               void*);
    OD_SZ("size_t",              std::size_t);
    OD_SZ("ptrdiff_t",           std::ptrdiff_t);
    OD_SZ("wchar_t",             wchar_t);
#undef OD_SZ
}
