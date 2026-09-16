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
//   odmod-check mymod.odmod [--revoke UI] [--no-run] [--decide N]
//
// --decide drives mod_ai_choose against a stub world for N turns. It exists
// because mod_load proves only that a mod STARTS: for a Neural.Decide mod the
// whole point is the deciding, and until this flag there was no way to run that
// outside a real game -- and the mod menu is the only way into one. A decision
// path nobody can exercise is a decision path nobody has exercised.

#include "mods/ModPackage.h"
#include "mods/ModRuntime.h"
#include "mods/ModHost.h"
#include "mod_world_stub.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

/* A world that MOVES, so a decision has something to be about. Everything not
 * overridden here answers "nothing" -- see tests/mod_world_stub.h. */
struct DecideWorld : StubWorld {
    uint32_t turn = 1;
    uint32_t owned = 9;
    double   gross = 100.0, net = 12.0, treasury = 240.0;
    bool     warWith2 = false;

    uint32_t turnNumber() override { return turn; }
    uint32_t countryCount() override { return 4; }
    uint32_t countryAt(uint32_t i) override { return i; }
    bool     countryExists(uint32_t c) override { return c < 4; }
    double   countryTreasury(uint32_t) override { return treasury; }
    uint32_t countryProvinceCount(uint32_t) override { return owned; }
    uint32_t provinceCount() override { return 900; }
    double   countryIncomeGross(uint32_t) override { return gross; }
    double   countryIncomeNet(uint32_t) override { return net; }
    bool     atWar(uint32_t, uint32_t b) override { return warWith2 && b == 2; }
    bool     neuralCountryIsAI(uint32_t) override { return true; }
    uint32_t neuralModuleCount() override { return 4; }
    uint32_t neuralActionCount(uint32_t) override { return 12; }
    std::string neuralModuleName(uint32_t m) override {
        const char* n[] = {"economy", "politics", "war", "navy"};
        return m < 4 ? n[m] : "";
    }
};

void decide(ModInstance* inst, int turns) {
    if (!inst->hasExport("mod_ai_choose")) {
        printf("\n--decide: this mod does not export mod_ai_choose\n");
        return;
    }
    DecideWorld world;
    g_modGame = &world;
    std::vector<uint8_t> mask(12, 1);

    printf("\ndriving mod_ai_choose, %d turns, 12 actions all legal\n", turns);
    printf("  turn  provinces   net  war    e    p    w    n\n");
    for (int t = 1; t <= turns; t++) {
        world.turn = (uint32_t)t;
        world.owned = (uint32_t)(9 + (t % 5) * 4);
        world.net = (t % 2) ? 40.0 : -25.0;
        world.warWith2 = (t % 3) == 0;

        printf("  %4d  %9u  %4.0f  %3s ", t, world.owned, world.net,
               world.warWith2 ? "yes" : "no");
        for (uint32_t m = 0; m < 4; m++) {
            /* Set the same way ModManager::aiChoose sets it, because that is
             * what neural.decide.action_valid reads. */
            g_modHost.decideMask = mask.data();
            g_modHost.decideMaskLen = (uint32_t)mask.size();
            g_modHost.decideModule = (int32_t)m;

            uint32_t args[2] = {0u, m};
            uint32_t got = 0xFFFFFFFFu;
            std::string e;
            const bool ok = inst->callExport("mod_ai_choose", args, 2, &got, e);

            g_modHost.decideMask = nullptr;
            g_modHost.decideMaskLen = 0;
            g_modHost.decideModule = -1;

            if (!ok) { printf("  TRAP: %s", e.c_str()); break; }
            if (got == 0xFFFFFFFFu) printf("    -");
            else printf(" %4u", got);
        }
        printf("\n");
    }
    g_modGame = nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: odmod-check <file.odmod> [--revoke <Module>]... [--no-run]\n");
        return 2;
    }
    std::string path = argv[1];
    std::vector<std::string> revoked;
    bool run = true;
    int decideTurns = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--no-run") == 0) run = false;
        else if (strcmp(argv[i], "--revoke") == 0 && i + 1 < argc) revoked.push_back(argv[++i]);
        else if (strcmp(argv[i], "--decide") == 0)
            decideTurns = (i + 1 < argc && argv[i + 1][0] != '-') ? atoi(argv[++i]) : 8;
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
    for (const char* h : {"mod_unload", "mod_pre_turn", "mod_post_turn", "mod_draw_panel",
                          "mod_ai_choose"})
        printf("  %-16s %s\n", h, inst->hasExport(h) ? "yes" : "-");

    if (decideTurns > 0) decide(inst.get(), decideTurns);

    if (inst->hasExport("mod_unload"))
        inst->callExport("mod_unload", nullptr, 0, nullptr, err);
    printf("\nOK\n");
    return 0;
}
