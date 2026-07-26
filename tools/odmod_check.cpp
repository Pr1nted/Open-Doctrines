// odmod-check — validate a .odmod and dry-run it.
//
// For modders: this is what to run before shipping. It reads the archive under
// the same limits the game uses, validates MANIFEST.json, then instantiates the
// module with the capabilities the manifest requests and calls mod_load.
//
// It lives outside the game deliberately. The game only ever loads a mod from
// the mod menu, so adding a "load this file" flag there would poke a hole in
// the one rule the whole security model rests on.
//
//   odmod-check mymod.odmod [--revoke UI] [--no-run]

#include "mods/ModPackage.h"
#include "mods/ModRuntime.h"
#include "mods/ModHost.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: odmod-check <file.odmod> [--revoke <Module>]... [--no-run]\n");
        return 2;
    }
    std::string path = argv[1];
    std::vector<std::string> revoked;
    bool run = true;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--no-run") == 0) run = false;
        else if (strcmp(argv[i], "--revoke") == 0 && i + 1 < argc) revoked.push_back(argv[++i]);
    }

    ModPackage pkg;
    ModLoadResult r = pkg.open(path);
    if (r != ModLoadResult::Ok) {
        printf("REJECTED (%s)\n  %s\n", modLoadResultName(r), pkg.diagnostic().c_str());
        return 1;
    }

    const ModManifest& m = pkg.manifest();
    printf("%s %s\n", m.name.c_str(), m.version.c_str());
    printf("  id          %s\n", m.id.c_str());
    printf("  gearbox     %d.%d\n", m.gearboxMajor, m.gearboxMinor);
    printf("  modules     %s\n", modModuleMaskToString(m.modules).c_str());
    printf("  limits      %u pages (%u KiB), %llu fuel/turn\n",
           m.limits.memoryPages, m.limits.memoryPages * 64,
           (unsigned long long)m.limits.fuelPerTurn);
    printf("  mod.wasm    %zu bytes\n", pkg.wasm().size());
    if (!pkg.thumbnail().empty())
        printf("  thumbnail   %zu bytes\n", pkg.thumbnail().size());
    if (!pkg.assetNames().empty()) {
        printf("  assets      %zu\n", pkg.assetNames().size());
        for (const auto& a : pkg.assetNames()) printf("                %s\n", a.c_str());
    }
    for (const auto& d : m.dependencies)
        printf("  depends     %s %s%s\n", d.id.c_str(), d.version.c_str(),
               d.optional ? " (optional)" : "");
    for (const auto& w : pkg.warnings()) printf("  warning     %s\n", w.c_str());

    if (!run) return 0;

    ModRuntime& rt = ModRuntime::get();
    if (!rt.available()) {
        printf("\nno WASM backend in this build (%s); archive checks only\n",
               rt.backendName());
        return 0;
    }
    std::string err;
    if (!rt.init(err)) { printf("\nruntime failed: %s\n", err.c_str()); return 1; }

    uint32_t grants = m.modules;
    for (const auto& name : revoked) {
        uint32_t bit = modModuleFromName(name);
        if (!bit) { printf("\nunknown module to revoke: %s\n", name.c_str()); return 2; }
        grants &= ~bit;
        printf("  revoking    %s\n", name.c_str());
    }
    grants |= MODULE_CORE;

    printf("\ninstantiating with %s\n", modModuleMaskToString(grants).c_str());
    auto inst = rt.instantiate(pkg, grants, err);
    if (!inst) { printf("FAILED: %s\n", err.c_str()); return 1; }

    uint32_t ret = 0;
    if (!inst->callExport("mod_load", nullptr, 0, &ret, err)) {
        printf("mod_load FAILED: %s\n", err.c_str());
        return 1;
    }
    if (ret != 0) { printf("mod_load refused the load, code %u\n", ret); return 1; }

    printf("mod_load OK\n");
    for (const char* h : {"mod_unload", "mod_pre_turn", "mod_post_turn", "mod_draw_panel"})
        printf("  %-16s %s\n", h, inst->hasExport(h) ? "yes" : "-");

    if (inst->hasExport("mod_unload"))
        inst->callExport("mod_unload", nullptr, 0, nullptr, err);
    printf("\nOK\n");
    return 0;
}
