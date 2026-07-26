// Tests for ModManager: the four-state machine, grants, persistence, and the
// rule that a mod is never instantiated into a game already in progress.
//
// Operates on a scratch mods directory built from the fixture wasm modules, so
// it exercises the same import/scan/enable path the mod menu uses.
//
// Build target: ModManagerTest.

#include "mods/ModManager.h"
#include "mods/ModHost.h"
#include "test_zip.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(const std::string& what, bool cond, const std::string& detail = "") {
    g_checks++;
    if (cond) { printf("  ok    %s\n", what.c_str()); return; }
    printf("  FAIL  %s%s%s\n", what.c_str(), detail.empty() ? "" : " — ",
           detail.c_str());
    g_failures++;
}

std::vector<uint8_t> readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    if (n <= 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> v((size_t)n);
    f.read((char*)v.data(), n);
    return v;
}

void writeFile(const std::string& p, const std::vector<uint8_t>& d) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write((const char*)d.data(), (std::streamsize)d.size());
}

std::string readText(const std::string& p) {
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

std::string manifestFor(const char* id, const char* modules) {
    char buf[512];
    snprintf(buf, sizeof buf,
             "{\n  \"schema\": 1,\n  \"id\": \"%s\",\n  \"name\": \"%s\",\n"
             "  \"version\": \"1.0.0\",\n  \"gearbox\": \"1.0\",\n"
             "  \"modules\": [%s],\n"
             "  \"limits\": { \"memoryPages\": 16, \"fuelPerTurn\": 500000 }\n}",
             id, id, modules);
    return buf;
}

std::vector<uint8_t> packMod(const std::string& manifest,
                             const std::vector<uint8_t>& wasm) {
    Zip z;
    z.add("MANIFEST.json", manifest);
    z.add("mod.wasm", wasm);
    return z.finish();
}

ModEntry* byId(const char* id) {
    for (auto& e : ModManager::get().mods())
        if (e.id == id) return &e;
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    std::string fixtures = argc > 1 ? argv[1] : "testmods";
    std::string dir = argc > 2 ? argv[2] : "modmgr_scratch";
    std::string state = dir + "/../modmgr_state.json";

    printf("mod manager\n");
    if (!ModRuntime::get().available()) {
        printf("  SKIP  no WASM backend in this build\n\n0 checks, 0 failed\n");
        return 0;
    }

    auto coreWasm   = readFile(fixtures + "/coretest.wasm");
    auto uiWasm     = readFile(fixtures + "/uitest.wasm");
    auto refuseWasm = readFile(fixtures + "/refusemod.wasm");
    if (coreWasm.empty() || uiWasm.empty() || refuseWasm.empty()) {
        printf("  SKIP  fixtures missing in %s; run tests/build_test_mods.sh\n",
               fixtures.c_str());
        printf("\n0 checks, 0 failed\n");
        return 0;
    }

    // Fresh scratch directory every run.
    mkdir(dir.c_str(), 0755);
    for (const char* f : {"com.test.core.odmod", "com.test.ui.odmod",
                          "com.test.refuse.odmod"})
        ::remove((dir + "/" + f).c_str());
    ::remove((dir + "_staging.odmod").c_str());
    ::remove(state.c_str());

    g_modHost.headless = false;
    g_modHost.screenW = 1280;
    g_modHost.screenH = 720;

    ModManager& mm = ModManager::get();
    mm.init(dir, state);
    check("empty directory yields no mods", mm.mods().empty(),
          std::to_string(mm.mods().size()));

    // ---- import ------------------------------------------------------------
    printf("import\n");
    {
        std::string staged = dir + "_staging.odmod";   // outside the mods dir
        writeFile(staged, packMod(manifestFor("com.test.core", "\"Core\""), coreWasm));
        std::string err;
        check("valid mod imports", mm.importFile(staged, err), err);
        ::remove(staged.c_str());
        mm.rescan();
        check("imported mod appears", byId("com.test.core") != nullptr);
        if (auto* e = byId("com.test.core")) {
            check("starts disabled", e->state == ModState::Disabled && !e->enabled,
                  modStateName(e->state));
            check("named after its id on disk",
                  e->fileName == "com.test.core.odmod", e->fileName);
        }
    }
    {
        // A file that is not a mod must be reported, not stored.
        std::string junk = dir + "/../not_a_mod.odmod";
        writeFile(junk, {'n', 'o', 'p', 'e'});
        std::string err;
        size_t before = mm.mods().size();
        check("junk import is refused", !mm.importFile(junk, err));
        check("refusal has a diagnostic", !err.empty(), err);
        check("nothing was added", mm.mods().size() == before);
        ::remove(junk.c_str());
    }

    {
        // Two files claiming one id: the second must be refused by name.
        std::string dupPath = dir + "/zz-duplicate.odmod";
        writeFile(dupPath, packMod(manifestFor("com.test.core", "\"Core\""), coreWasm));
        mm.rescan();
        ModEntry* dup = nullptr;
        for (auto& e : mm.mods()) if (e.fileName == "zz-duplicate.odmod") dup = &e;
        check("duplicate id is refused", dup && dup->state == ModState::Failed,
              dup ? modStateName(dup->state) : "missing");
        check("duplicate names the file that won",
              dup && dup->diagnostic.find("com.test.core.odmod") != std::string::npos,
              dup ? dup->diagnostic : "");
        ::remove(dupPath.c_str());
        mm.rescan();
    }

    // ---- enable / activate -------------------------------------------------
    printf("enable\n");
    mm.setInGame(false);
    {
        size_t idx = 0;
        for (size_t i = 0; i < mm.mods().size(); i++)
            if (mm.mods()[i].id == "com.test.core") { idx = i; break; }
        mm.setEnabled(idx, true);
        auto* e = byId("com.test.core");
        check("enabling outside a game activates immediately",
              e && e->state == ModState::Active, e ? e->diagnostic : "missing");
        check("anyEnabled reports true", mm.anyEnabled());
        check("anyActive reports true", mm.anyActive());
    }

    // ---- persistence -------------------------------------------------------
    printf("persistence\n");
    {
        std::string j = readText(state);
        check("state file was written", !j.empty());
        check("state records the mod id",
              j.find("com.test.core") != std::string::npos);
        check("state records enabled", j.find("\"enabled\": true") != std::string::npos, j);
    }

    // ---- deferred activation ------------------------------------------------
    printf("deferred activation\n");
    {
        std::string staged = dir + "_staging.odmod";   // outside the mods dir
        writeFile(staged, packMod(manifestFor("com.test.ui", "\"Core\", \"UI\""), uiWasm));
        std::string err;
        mm.importFile(staged, err);
        ::remove(staged.c_str());

        // A game is running: enabling must NOT start the mod.
        mm.setInGame(true);
        size_t idx = 0;
        for (size_t i = 0; i < mm.mods().size(); i++)
            if (mm.mods()[i].id == "com.test.ui") { idx = i; break; }
        mm.setEnabled(idx, true);

        auto* e = byId("com.test.ui");
        check("enabling mid-game defers activation",
              e && e->state == ModState::PendingReload,
              e ? modStateName(e->state) : "missing");
        check("deferral explains itself", e && !e->diagnostic.empty(),
              e ? e->diagnostic : "");
        check("no instance was created", e && e->instance == nullptr);

        mm.reloadAll();
        e = byId("com.test.ui");
        check("reload activates it", e && e->state == ModState::Active,
              e ? e->diagnostic : "missing");
        mm.setInGame(false);
    }

    // ---- grants -------------------------------------------------------------
    printf("grants\n");
    {
        size_t idx = 0;
        for (size_t i = 0; i < mm.mods().size(); i++)
            if (mm.mods()[i].id == "com.test.ui") { idx = i; break; }

        mm.setGrant(idx, MODULE_UI, false);
        auto* e = byId("com.test.ui");
        check("revoking a grant needs a reload",
              e && e->state == ModState::PendingReload,
              e ? modStateName(e->state) : "missing");
        check("grant bit cleared", e && !(e->grants & MODULE_UI));

        // Revoked UI means the import cannot link, so the mod must fail rather
        // than run without the capability it believes it holds.
        mm.reloadAll();
        e = byId("com.test.ui");
        check("reload with a revoked capability fails the mod",
              e && e->state == ModState::Failed, e ? modStateName(e->state) : "missing");
        check("failure names the capability",
              e && e->diagnostic.find("UI") != std::string::npos,
              e ? e->diagnostic : "");

        mm.setGrant(idx, MODULE_UI, true);
        mm.reloadAll();
        e = byId("com.test.ui");
        check("restoring the grant fixes it", e && e->state == ModState::Active,
              e ? e->diagnostic : "missing");

        // Core is not revocable, and nothing the manifest did not ask for can
        // be granted.
        // Precedence trap avoided: assert the state explicitly rather than
        // leaning on && / || binding.
        uint32_t coreBefore = e ? e->grants : 0;
        ModState stateBefore = e ? e->state : ModState::Failed;
        mm.setGrant(idx, MODULE_CORE, false);
        e = byId("com.test.ui");
        check("revoking Core is a no-op",
              e != nullptr && e->grants == coreBefore && e->state == stateBefore,
              e ? modStateName(e->state) : "missing");
        mm.setGrant(idx, MODULE_NEURAL, true);
        e = byId("com.test.ui");
        check("cannot grant what was never requested",
              e && !(e->grants & MODULE_NEURAL));
    }

    // ---- a mod that refuses its own load ------------------------------------
    printf("refusal\n");
    {
        std::string staged = dir + "_staging.odmod";   // outside the mods dir
        writeFile(staged, packMod(manifestFor("com.test.refuse", "\"Core\""), refuseWasm));
        std::string err;
        mm.importFile(staged, err);
        ::remove(staged.c_str());

        size_t idx = 0;
        for (size_t i = 0; i < mm.mods().size(); i++)
            if (mm.mods()[i].id == "com.test.refuse") { idx = i; break; }
        mm.setEnabled(idx, true);

        auto* e = byId("com.test.refuse");
        check("a non-zero mod_load fails the mod",
              e && e->state == ModState::Failed, e ? modStateName(e->state) : "missing");
        check("the returned code is reported",
              e && e->diagnostic.find("7") != std::string::npos,
              e ? e->diagnostic : "");
        check("no instance is left behind", e && e->instance == nullptr);
    }

    // ---- disable and delete --------------------------------------------------
    printf("disable and delete\n");
    {
        size_t idx = 0;
        for (size_t i = 0; i < mm.mods().size(); i++)
            if (mm.mods()[i].id == "com.test.core") { idx = i; break; }
        mm.setEnabled(idx, false);
        auto* e = byId("com.test.core");
        check("disabling tears the instance down",
              e && e->state == ModState::Disabled && e->instance == nullptr,
              e ? modStateName(e->state) : "missing");

        for (size_t i = 0; i < mm.mods().size(); i++)
            if (mm.mods()[i].id == "com.test.refuse") { idx = i; break; }
        std::string err;
        check("delete removes the mod", mm.removeMod(idx, err), err);
        check("it is gone from the list", byId("com.test.refuse") == nullptr);
        struct stat st;
        check("the file is gone from disk",
              stat((dir + "/com.test.refuse.odmod").c_str(), &st) != 0);
    }

    mm.unloadAll();
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
