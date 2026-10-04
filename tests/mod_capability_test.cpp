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
uint32_t bitFor(const std::string& cap) {
    static const struct { const char* name; uint32_t bit; } kMap[] = {
        {"Core", MODULE_CORE},
        {"Core.Protected", MODULE_CORE_PROTECTED},
        {"GameState.Read", MODULE_GAMESTATE_READ},
        {"GameState.Write", MODULE_GAMESTATE_WRITE},
        {"GameProcess", MODULE_GAMEPROCESS},
        {"Neural", MODULE_NEURAL},
        {"Neural.Decide", MODULE_NEURAL_DECIDE},
        {"UI", MODULE_UI},
        {"Map", MODULE_MAP},
        {"MapEditor", MODULE_MAPEDITOR},
        {"Diplomacy", MODULE_DIPLOMACY},
        {"Assets", MODULE_ASSETS},
        {"Storage", MODULE_STORAGE},
        {"Audio", MODULE_AUDIO},
        {"Net", MODULE_NET},
        {"Military.Read", MODULE_MILITARY_READ},
        {"Military.Write", MODULE_MILITARY_WRITE},
        {"Research.Read", MODULE_RESEARCH_READ},
        {"Research.Write", MODULE_RESEARCH_WRITE},
        {"Politics.Read", MODULE_POLITICS_READ},
        {"Politics.Write", MODULE_POLITICS_WRITE},
        {"Economy.Read", MODULE_ECONOMY_READ},
        {"Economy.Write", MODULE_ECONOMY_WRITE},
        {"Country", MODULE_COUNTRY},
        {"Scripts", MODULE_SCRIPTS},
        {"Render", MODULE_RENDER},
        {"Content", MODULE_CONTENT},
    };
    for (const auto& e : kMap)
        if (cap == e.name) return e.bit;
    return 0;
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

        const uint32_t bit = bitFor(cap);
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
