// EVERY CAPABILITY, CALLED.
//
// The SDK's static side is covered three times over: ModAbiTest checks the
// host's capability table against sdk/abi.json in both directions,
// gen_bindings.py --check keeps every generated binding in step with it, and
// check_bindings.py verifies each hand-written binding names every import and
// spells its module right.
//
// None of that CALLS anything. A capability can be declared by the host,
// described in abi.json, named correctly in eleven bindings, and still be
// unreachable -- and all three checks pass. That is the gap this closes: for
// each `gearbox:*` module, a generated mod calls EVERY import in it once and
// counts the calls that returned. Anything less than the full count means the
// host did not answer one of them.
//
//   python3 tools/gen_capability_mods.py build/capmods   # generate + describe
//   (compile each cap_*.c to wasm32 -- tests/build_test_mods.sh's toolchain)
//   ModCapabilityTest build/capmods
//
// WHAT IT DOES NOT CLAIM. The arguments are benign placeholders -- zero
// handles, null pointers with zero lengths. So this proves REACHABILITY and
// that the host refuses nonsense without falling over. It does not check the
// answers are right; a function returning the wrong province is
// indistinguishable here from one returning the right one. Meaning is checked
// by the per-subject tests; presence is checked here, and was not checked
// anywhere before.
//
// Build target: ModCapabilityTest.
#include "mods/ModPackage.h"
#include "mods/ModRuntime.h"
#include "mod_world_stub.h"
#include "mods/ModHost.h"
#include "test_zip.h"
#include "json.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <string>
#include <vector>

namespace {

int g_checks = 0, g_failures = 0;

void check(const char* what, bool ok, const std::string& detail = "") {
    ++g_checks;
    printf("  %-5s %s%s\n", ok ? "ok" : "FAIL", what,
           detail.empty() ? "" : ("  --  " + detail).c_str());
    if (!ok) ++g_failures;
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    if (n <= 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> v((size_t)n);
    f.read((char*)v.data(), n);
    return v;
}

// Every capability a generated mod can ask for. Core is always granted: a mod
// without it cannot log, and Core is what abi.json gives a null capability.
uint64_t bitFor(const std::string& cap) {
    // ONE LIST, NOT TWO. This was a hand-kept copy of the table in
    // ModPackage.cpp, so every capability added to the game failed here until
    // somebody remembered to add it twice -- a test that reports "the
    // capability has a module bit: false" about a capability that plainly has
    // one, which reads as a bug in the feature rather than in the test.
    // modModuleFromName() is the thing the game itself resolves names with and
    // it already answers 0 for unknown, which is exactly what this wants.
    return modModuleFromName(cap);
}

std::string manifestFor(const std::string& id, const std::string& cap) {
    // "Core" plus the capability under test, unless the capability IS Core.
    std::string mods = "\"Core\"";
    if (cap != "Core") mods += ", \"" + cap + "\"";
    return "{\n  \"schema\": 1,\n  \"id\": \"" + id +
           "\",\n  \"name\": \"Capability Fixture\",\n  \"version\": \"1.0.0\",\n"
           "  \"gearbox\": \"1.0\",\n  \"modules\": [" + mods + "],\n"
           "  \"limits\": { \"memoryPages\": 64, \"fuelPerTurn\": 20000000,"
           " \"loadFuel\": 20000000 }\n}";
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "build/capmods";
    printf("Every capability, called -- fixtures from %s\n\n", dir.c_str());

    // The grant mask is 64 bits. Map.Write took bit 30 and left exactly one,
    // so it was widened before the last was spent -- and the widening is only
    // real if a bit above 31 survives every hop a grant makes: the mask, the
    // persisted JSON number, and the capability test back. On a uint32_t mask
    // kHigh truncates to zero and all three of these fail.
    {
        // Against the PRODUCTION types, not locals of this test's choosing:
        // a check written with its own uint64_t passes just as well while the
        // real field is still 32 bits, which is the mistake worth avoiding in
        // a test whose whole subject is a field's width.
        static_assert(sizeof(ModManifest::modules) >= 8,
                      "the manifest's module mask must hold a bit above 31");
        static_assert(sizeof(decltype(std::declval<ModInstance&>().granted())) >= 8,
                      "a granted mask must hold a bit above 31");

        constexpr uint64_t kHigh = 1ull << 40;
        decltype(ModManifest::modules) grants = MODULE_CORE | kHigh;
        check("a bit above 31 survives the grant mask", (grants & kHigh) != 0);
        check("and does not disturb Core", (grants & MODULE_CORE) != 0);

        nlohmann::json j;
        j["grants"] = grants;
        const uint64_t back = j["grants"].get<uint64_t>();
        check("and survives the state file", back == grants,
              "read back " + std::to_string(back) + " from " + std::to_string(grants));
    }


    const std::string descPath = dir + "/capabilities.json";
    std::ifstream df(descPath);
    if (!df) {
        printf("  SKIP  no %s\n", descPath.c_str());
        printf("        run: python3 tools/gen_capability_mods.py %s\n", dir.c_str());
        printf("        then compile each cap_*.c to wasm32\n");
        printf("\n%d checks, %d failed (skipped)\n", g_checks, g_failures);
        return 0;
    }
    nlohmann::json desc;
    df >> desc;

    // NO WASM TOOLCHAIN IS A SKIP, NOT A FAILURE -- the rule mod_runtime_test.cpp
    // already states for its own fixtures ("skipped loudly"). When
    // build_capability_mods.sh finds no wasm32-capable clang it builds nothing,
    // which is the macOS CI runners' situation, and every capability then read
    // "the fixture was built  FAIL" -- a red suite about the machine, not the
    // code. So: if NOT ONE fixture exists, the toolchain is absent and the whole
    // test is skipped, loudly. If some exist and one is missing, a fixture
    // failed to build, and that is still the failure it was, below.
    {
        int built = 0;
        for (const auto& m : desc["modules"])
            if (!readFile(dir + "/cap_" + m.value("slug", "") + ".wasm").empty()) ++built;
        if (built == 0) {
            printf("  SKIP  no capability fixture was built in %s\n", dir.c_str());
            printf("        no wasm32-capable clang here -- see tests/build_capability_mods.sh\n");
            printf("\n%d checks, %d failed (skipped)\n", g_checks, g_failures);
            return 0;
        }
    }

    ModRuntime& rt = ModRuntime::get();
    // Headless, and a world that answers "nothing" to everything. The point
    // here is that each call REACHES the host and returns -- an empty world
    // is the cleanest backdrop for that, and it exercises the "no such thing"
    // path that every id lookup has.
    g_modHost.headless = true;
    static StubWorld emptyWorld;
    g_modGame = &emptyWorld;

    int covered = 0, totalCalls = 0;
    for (const auto& m : desc["modules"]) {
        const std::string wire = m.value("module", "");
        const std::string slug = m.value("slug", "");
        const std::string cap  = m.value("capability", "Core");
        const int expected     = m.value("expected_calls", 0);

        printf("%s  (%s, %d import(s))\n", wire.c_str(), cap.c_str(), expected);

        const std::string wasmPath = dir + "/cap_" + slug + ".wasm";
        std::vector<uint8_t> wasm = readFile(wasmPath);
        if (wasm.empty()) {
            check("the fixture was built", false, "missing " + wasmPath);
            continue;
        }

        const uint64_t bit = bitFor(cap);
        if (!bit) { check("the capability has a module bit", false, cap); continue; }

        Zip z;
        z.add("MANIFEST.json", manifestFor("com.test.cap." + slug, cap));
        z.add("mod.wasm", wasm);

        ModPackage pkg;
        std::string err;
        if (pkg.openFromMemory(z.finish(), slug.c_str()) != ModLoadResult::Ok) {
            check("it packages", false, pkg.diagnostic());
            continue;
        }

        auto mi = rt.instantiate(pkg, MODULE_CORE | bit, err);
        // THE IMPORTANT ONE. An unresolved import fails here, naming the
        // function the host does not provide -- which is exactly the failure
        // a modder sees and the one no static check can produce.
        if (!mi) { check("every import resolves", false, err); continue; }
        check("every import resolves", true);

        uint32_t ret = 0xFFFFFFFFu;
        if (!mi->callExport("mod_load", nullptr, 0, &ret, err)) {
            check("mod_load runs", false, err);
            continue;
        }
        check("mod_load runs and returns 0", ret == 0, std::to_string((int32_t)ret));

        uint32_t calls = 0;
        if (!mi->callExport("cap_calls", nullptr, 0, &calls, err)) {
            check("it reports how many calls returned", false, err);
            continue;
        }
        check("every import was called and the host came back",
              (int)calls == expected,
              std::to_string((int)calls) + " of " + std::to_string(expected));
        if ((int)calls == expected) { ++covered; totalCalls += (int)calls; }
    }

    printf("\n%d capability module(s) fully exercised, %d host calls\n",
           covered, totalCalls);
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
