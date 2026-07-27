// Tests for ModManager: the four-state machine, grants, persistence, and the
// rule that a mod is never instantiated into a game already in progress.
//
// Operates on a scratch mods directory built from the fixture wasm modules, so
// it exercises the same import/scan/enable path the mod menu uses.
//
// Build target: ModManagerTest.

#include "mods/ModManager.h"
#include "mods/ModUpdates.h"
#include "mods/ModHost.h"
#include "test_zip.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <dirent.h>
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

// A manifest with a raw extra block, for dependency/conflict/bridge cases.
std::string manifestWith(const char* id, const char* version, const char* extra) {
    char buf[1024];
    snprintf(buf, sizeof buf,
             "{\n  \"schema\": 1,\n  \"id\": \"%s\",\n  \"name\": \"%s\",\n"
             "  \"version\": \"%s\",\n  \"gearbox\": \"1.0\",\n"
             "  \"modules\": [\"Core\"],\n%s"
             "  \"limits\": { \"memoryPages\": 16, \"fuelPerTurn\": 500000 }\n}",
             id, id, version, extra);
    return buf;
}

ModEntry* byId(const char* id) {
    for (auto& e : ModManager::get().mods())
        if (e.id == id) return &e;
    return nullptr;
}

// Dereferencing a missing entry turns a readable test failure into a SIGBUS,
// which is how a wrong fixture path cost a debugging cycle. Fail loudly instead.
const ModEntry& need(const char* id) {
    static ModEntry empty;
    ModEntry* e = byId(id);
    if (e) return *e;
    check(std::string("fixture present: ") + id, false, "not found after rescan");
    return empty;
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
    // Start from an empty scratch directory. The suite installs mods into it as
    // it goes, so without this the second run finds the first run's leftovers
    // and the "empty directory" precondition below fails -- a stale-state
    // failure that looks like a real one.
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* ent = readdir(d)) {
            std::string n = ent->d_name;
            if (n.size() > 6 && n.compare(n.size() - 6, 6, ".odmod") == 0)
                ::remove((dir + "/" + n).c_str());
        }
        closedir(d);
    }
    ::remove(state.c_str());
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

    // ---- dependencies and conflicts -----------------------------------------
    // These were "parsed, not resolved in Phase 1". The point of resolving them
    // is that a mod refuses BEFORE running, with a message naming what is
    // wrong, rather than failing somewhere inside itself later.
    printf("\ndependencies and conflicts\n");
    {
        const auto& wasm = coreWasm;   // fixtures live in `fixtures`, not the scratch dir

        // lib 2.0.0; app needs >=2.0.0 <3.0.0; old needs >=3.0.0 (unmet).
        writeFile(dir + "/com.test.lib.odmod",
                  packMod(manifestWith("com.test.lib", "2.0.0", ""), wasm));
        writeFile(dir + "/com.test.app.odmod",
                  packMod(manifestWith("com.test.app", "1.0.0",
                      "  \"dependencies\": [{\"id\": \"com.test.lib\","
                      " \"version\": \">=2.0.0 <3.0.0\"}],\n"), wasm));
        writeFile(dir + "/com.test.needsnew.odmod",
                  packMod(manifestWith("com.test.needsnew", "1.0.0",
                      "  \"dependencies\": [{\"id\": \"com.test.lib\","
                      " \"version\": \">=3.0.0\"}],\n"), wasm));
        writeFile(dir + "/com.test.needsmissing.odmod",
                  packMod(manifestWith("com.test.needsmissing", "1.0.0",
                      "  \"dependencies\": [{\"id\": \"com.test.absent\"}],\n"), wasm));
        writeFile(dir + "/com.test.optional.odmod",
                  packMod(manifestWith("com.test.optional", "1.0.0",
                      "  \"dependencies\": [{\"id\": \"com.test.absent\","
                      " \"optional\": true}],\n"), wasm));
        // Two mods that declare a conflict, and a bridge that reconciles them.
        writeFile(dir + "/com.test.left.odmod",
                  packMod(manifestWith("com.test.left", "1.0.0",
                      "  \"conflicts\": [{\"id\": \"com.test.right\","
                      " \"reason\": \"both rewrite the economy\"}],\n"), wasm));
        writeFile(dir + "/com.test.right.odmod",
                  packMod(manifestWith("com.test.right", "1.0.0", ""), wasm));
        writeFile(dir + "/com.test.bridge.odmod",
                  packMod(manifestWith("com.test.bridge", "1.0.0",
                      "  \"bridges\": [{\"between\": [\"com.test.left\","
                      " \"com.test.right\"], \"reason\": \"merges both\"}],\n"), wasm));
        mm.rescan();

        auto idxOf = [&](const char* id) -> size_t {
            const auto& v = mm.mods();
            for (size_t i = 0; i < v.size(); i++) if (v[i].id == id) return i;
            return (size_t)-1;
        };

        // Resolution only considers ENABLED mods, deliberately: an installed
        // but switched-off dependency is not going to run, so treating it as
        // satisfied would let the dependent fail later for no visible reason.
        mm.setEnabled(idxOf("com.test.lib"), true);

        check("satisfied dependency is met",
              mm.unmetDependency(need("com.test.app")).empty(),
              mm.unmetDependency(need("com.test.app")));
        check("missing dependency is reported",
              !mm.unmetDependency(need("com.test.needsmissing")).empty());
        check("out-of-range version is reported",
              !mm.unmetDependency(need("com.test.needsnew")).empty(),
              mm.unmetDependency(need("com.test.needsnew")));
        check("the message names the installed version",
              mm.unmetDependency(need("com.test.needsnew")).find("2.0.0")
                  != std::string::npos,
              mm.unmetDependency(need("com.test.needsnew")));
        check("an optional missing dependency is not a blocker",
              mm.unmetDependency(need("com.test.optional")).empty(),
              mm.unmetDependency(need("com.test.optional")));

        // Present but switched off is a different message from absent.
        mm.setEnabled(idxOf("com.test.lib"), false);
        std::string off = mm.unmetDependency(need("com.test.app"));
        check("a disabled dependency is reported as disabled, not missing",
              off.find("not enabled") != std::string::npos, off);
        mm.setEnabled(idxOf("com.test.lib"), true);

        // Conflicts.
        check("no conflict while the other side is disabled",
              mm.blockingConflict(need("com.test.left")).empty());
        mm.setEnabled(idxOf("com.test.left"), true);
        mm.setEnabled(idxOf("com.test.right"), true);
        check("declared conflict blocks once both are enabled",
              !mm.blockingConflict(need("com.test.left")).empty(),
              mm.blockingConflict(need("com.test.left")));
        check("the conflict is mutual, not one-directional",
              !mm.blockingConflict(need("com.test.right")).empty(),
              "right should see it too");
        check("the message carries the author's reason",
              mm.blockingConflict(need("com.test.left")).find("economy")
                  != std::string::npos);

        // Override on ONE side is enough to let the pair run.
        mm.setConflictOverride(idxOf("com.test.left"), "com.test.right", true);
        check("an override on one side clears it for both",
              mm.blockingConflict(need("com.test.left")).empty() &&
              mm.blockingConflict(need("com.test.right")).empty());
        mm.setConflictOverride(idxOf("com.test.left"), "com.test.right", false);
        check("removing the override restores the conflict",
              !mm.blockingConflict(need("com.test.left")).empty());

        // A bridge mod does the same thing, without the user deciding anything.
        mm.setEnabled(idxOf("com.test.bridge"), true);
        check("an enabled bridge suppresses the conflict",
              mm.blockingConflict(need("com.test.left")).empty(),
              mm.blockingConflict(need("com.test.left")));
        mm.setEnabled(idxOf("com.test.bridge"), false);
        check("disabling the bridge brings the conflict back",
              !mm.blockingConflict(need("com.test.left")).empty());
    }

    // ---- update checks ------------------------------------------------------
    // The response comes from a server a mod author controls, so the parser is
    // the security boundary. It is tested directly; the network fetch is not,
    // because a test that needs the internet is a test that fails on a train.
    printf("\nupdate checks\n");
    {
        std::string v, page;

        check("a well-formed response parses",
              ModUpdates::parseResponse(
                  "{\"version\": \"1.4.0\", \"url\": \"https://example.com/m\"}",
                  v, page) && v == "1.4.0" && page == "https://example.com/m",
              v + " / " + page);

        check("url is optional",
              ModUpdates::parseResponse("{\"version\": \"2.0.0\"}", v, page)
                  && v == "2.0.0" && page.empty(), v);

        // A page that merely contains the word must not read as an update.
        check("a response with no version is rejected",
              !ModUpdates::parseResponse("<html>version</html>", v, page));
        check("a non-numeric version is rejected",
              !ModUpdates::parseResponse("{\"version\": \"latest\"}", v, page));
        check("an over-long version is rejected",
              !ModUpdates::parseResponse(
                  "{\"version\": \"" + std::string(64, '1') + "\"}", v, page));

        // A plain-http or javascript: link must never reach a browser-open.
        ModUpdates::parseResponse(
            "{\"version\": \"1.1.0\", \"url\": \"http://insecure.example\"}", v, page);
        check("a non-https url is dropped", page.empty(), page);
        ModUpdates::parseResponse(
            "{\"version\": \"1.1.0\", \"url\": \"javascript:alert(1)\"}", v, page);
        check("a javascript: url is dropped", page.empty(), page);

        // Version comparison.
        check("newer patch is newer",  ModUpdates::isNewer("1.0.0", "1.0.1"));
        check("newer minor is newer",  ModUpdates::isNewer("1.0.9", "1.1.0"));
        check("newer major is newer",  ModUpdates::isNewer("1.9.9", "2.0.0"));
        check("equal is not newer",   !ModUpdates::isNewer("1.2.3", "1.2.3"));
        check("older is not newer",   !ModUpdates::isNewer("2.0.0", "1.9.9"));
        check("missing version is not newer", !ModUpdates::isNewer("1.0.0", ""));

        // The gate. With the setting off, nothing is requested at all.
        ModUpdates::get().clear();
        ModUpdates::get().checkAll(mm, false);
        check("no check runs while the setting is off",
              ModUpdates::get().infoFor("com.test.lib") == nullptr &&
              !ModUpdates::get().busy());
    }

    mm.unloadAll();
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
