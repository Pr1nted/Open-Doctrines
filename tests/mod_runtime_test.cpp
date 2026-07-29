// End-to-end tests for the Gearbox runtime: a real .odmod, built here from a
// real compiled wasm module, instantiated and driven through the host natives.
//
// The wasm fixtures come from tests/build_test_mods.sh. If they are missing
// (no wasm-capable clang on this machine) the wasm cases are skipped loudly
// rather than silently passing.
//
// Build target: ModRuntimeTest. Run it; non-zero exit means a case failed.

#include "mods/ModPackage.h"
#include "mods/ModRuntime.h"
#include "mods/ModHost.h"
#include "test_zip.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <limits>
#include <fstream>
#include <string>
#include <filesystem>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
int g_skipped = 0;

void check(const std::string& what, bool cond, const std::string& detail = "") {
    g_checks++;
    if (cond) { printf("  ok    %s\n", what.c_str()); return; }
    printf("  FAIL  %s%s%s\n", what.c_str(), detail.empty() ? "" : " — ",
           detail.c_str());
    g_failures++;
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

std::string manifestFor(const char* id, const char* modules,
                        uint32_t pages = 64, uint64_t fuel = 2000000) {
    char buf[768];
    snprintf(buf, sizeof buf,
             "{\n"
             "  \"schema\": 1,\n"
             "  \"id\": \"%s\",\n"
             "  \"name\": \"Runtime Fixture\",\n"
             "  \"version\": \"1.0.0\",\n"
             "  \"gearbox\": \"1.0\",\n"
             "  \"modules\": [%s],\n"
             "  \"limits\": { \"memoryPages\": %u, \"fuelPerTurn\": %llu }\n"
             "}",
             id, modules, pages, (unsigned long long)fuel);
    return buf;
}

// Same, but states loadFuel explicitly. Kept separate so the common helper
// above keeps exercising the "unstated, host decides" path.
std::string manifestWithLoadFuel(const char* id, const char* modules,
                                 uint64_t fuel, uint64_t loadFuel) {
    char buf[768];
    snprintf(buf, sizeof buf,
             "{\n"
             "  \"schema\": 1,\n"
             "  \"id\": \"%s\",\n"
             "  \"name\": \"Runtime Fixture\",\n"
             "  \"version\": \"1.0.0\",\n"
             "  \"gearbox\": \"1.0\",\n"
             "  \"modules\": [%s],\n"
             "  \"limits\": { \"memoryPages\": 64, \"fuelPerTurn\": %llu,"
             " \"loadFuel\": %llu }\n"
             "}",
             id, modules, (unsigned long long)fuel,
             (unsigned long long)loadFuel);
    return buf;
}

std::vector<uint8_t> packMod(const std::string& manifest,
                             const std::vector<uint8_t>& wasm) {
    Zip z;
    z.add("MANIFEST.json", manifest);
    z.add("mod.wasm", wasm);
    return z.finish();
}

bool logContains(const char* needle) {
    for (const auto& l : modHostLog())
        if (l.text.find(needle) != std::string::npos) return true;
    return false;
}

uint32_t call0(ModInstance& mi, const char* name, bool* ok = nullptr) {
    uint32_t ret = 0;
    std::string err;
    bool r = mi.callExport(name, nullptr, 0, &ret, err);
    if (ok) *ok = r;
    return ret;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "testmods";

    printf("runtime\n");
    ModRuntime& rt = ModRuntime::get();
    printf("  backend: %s\n", rt.backendName());
    if (!rt.available()) {
        printf("  SKIP  no WASM backend in this build; runtime tests skipped\n");
        printf("\n0 checks, 0 failed, all skipped\n");
        return 0;
    }
    std::string err;
    check("runtime initialises", rt.init(err), err);

    auto coreWasm = readFile(dir + "/coretest.wasm");
    auto uiWasm   = readFile(dir + "/uitest.wasm");
    if (coreWasm.empty() || uiWasm.empty()) {
        printf("  SKIP  fixture modules not found in %s\n", dir.c_str());
        printf("        run tests/build_test_mods.sh first\n");
        printf("\n%d checks, %d failed (wasm cases skipped)\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    // ---- Core ---------------------------------------------------------------
    printf("core capability\n");
    ModPackage corePkg;
    {
        auto r = corePkg.openFromMemory(
            packMod(manifestFor("com.test.core", "\"Core\""), coreWasm), "core");
        check("fixture packages as a valid .odmod", r == ModLoadResult::Ok,
              corePkg.diagnostic());
    }

    g_modHost.headless = false;
    g_modHost.screenW = 1280;
    g_modHost.screenH = 720;
    modHostLogClear();

    // WasiStub's clock reports the turn number, so the test needs a world.
    struct TurnOnly : ModGameAccess {
        uint32_t turnNumber() override { return 42; }
        uint32_t countryCount() override { return 0; }
        uint32_t countryAt(uint32_t) override { return 0xFFFFFFFFu; }
        bool countryExists(uint32_t) override { return false; }
        std::string countryName(uint32_t) override { return {}; }
        double countryTreasury(uint32_t) override { return 0; }
        uint32_t countryProvinceCount(uint32_t) override { return 0; }
        long long provincePopulation(uint32_t) override { return 0; }
        uint32_t provinceOwner(uint32_t) override { return 0xFFFFFFFFu; }
        uint32_t mapWidth() override { return 0; }
        uint32_t mapHeight() override { return 0; }
        uint32_t provinceCount() override { return 0; }
        uint32_t provinceAt(uint32_t) override { return 0xFFFFFFFFu; }
        bool provinceExists(uint32_t) override { return false; }
        std::string provinceName(uint32_t) override { return {}; }
        double provinceCenterX(uint32_t) override { return 0; }
        double provinceCenterY(uint32_t) override { return 0; }
        bool provinceIsLand(uint32_t) override { return false; }
        uint32_t provinceNeighborCount(uint32_t) override { return 0; }
        uint32_t provinceNeighborAt(uint32_t, uint32_t) override { return 0xFFFFFFFFu; }
        bool atWar(uint32_t, uint32_t) override { return false; }
        bool allied(uint32_t, uint32_t) override { return false; }
        bool nonAggression(uint32_t, uint32_t) override { return false; }
        bool guaranteed(uint32_t, uint32_t) override { return false; }
        bool proposeWar(uint32_t, uint32_t) override { return false; }
        // GameState.Write, implemented for real so the capability is
        // exercised rather than stubbed. Country 7 exists, province 3 exists.
        double treasury = 100.0;
        long long population = 5000;
        uint32_t owner = 7;
        bool setCountryTreasury(uint32_t cid, double v) override {
            if (cid != 7) return false;
            if (!(v == v) || v <= -1e12 || v >= 1e12) return false;
            treasury = v; return true;
        }
        bool addCountryTreasury(uint32_t cid, double d) override {
            if (cid != 7) return false;
            double n = treasury + d;
            if (!(n == n) || n <= -1e12 || n >= 1e12) return false;
            treasury = n; return true;
        }
        bool setProvinceOwner(uint32_t pid, uint32_t cid) override {
            if (pid != 3 || cid != 7) return false;
            if (owner == cid) return false;
            owner = cid; return true;
        }
        bool setProvincePopulation(uint32_t pid, long long v) override {
            if (pid != 3 || v < 0 || v > 100000000000LL) return false;
            population = v; return true;
        }
        uint32_t neuralFeatureCount() override { return 0; }
        uint32_t neuralFeatures(uint32_t, float*, uint32_t) override { return 0; }
        uint32_t neuralRewardCount() override { return 0; }
        double neuralRewardMean(uint32_t) override { return 0; }
    } turnWorld;
    g_modGame = &turnWorld;

    auto core = rt.instantiate(corePkg, MODULE_CORE, err);
    check("core fixture instantiates", core != nullptr, err);
    if (core) {
        uint32_t ret = 0;
        bool ok = core->callExport("mod_load", nullptr, 0, &ret, err);
        check("mod_load runs", ok, err);
        check("mod_load returns 0", ok && ret == 0, std::to_string(ret));
        check("gearbox_log reached the host", logContains("coretest loaded"));

        check("env.size is the ABI size", call0(*core, "t_env_size") == 28,
              std::to_string(call0(*core, "t_env_size")));
        check("env.gearbox_major is 1", call0(*core, "t_env_major") == 1);
#if defined(__APPLE__)
        check("env.platform is macOS", call0(*core, "t_env_platform") == 2,
              std::to_string(call0(*core, "t_env_platform")));
#endif
        check("env.is_headless is 0", call0(*core, "t_env_headless") == 0);

        check("fuel_budget matches the manifest",
              call0(*core, "t_fuel_budget_lo") == 2000000,
              std::to_string(call0(*core, "t_fuel_budget_lo")));

        // The important one: an infinite loop must be stopped by the
        // interpreter, not hang the process.
        bool burned = core->callExport("t_burn_fuel", nullptr, 0, nullptr, err);
        check("infinite loop is terminated by the fuel limit", !burned, err);
    }

    // ---- load fuel is a separate budget -------------------------------------
    // mod_load runs once, when the user enables the mod, and for an interpreter
    // SDK it is dominated by starting the interpreter -- Ruby needs ~87M
    // instructions and CPython ~130M before running a line of script, against a
    // fuelPerTurn ceiling of 100M. So it draws on its own budget.
    //
    // fuelPerTurn is set to 1 here, which is far too little for any real call.
    // That is what makes this discriminating: if mod_load were charged to the
    // per-turn budget it could not possibly succeed.
    printf("\nload fuel\n");
    {
        ModPackage p;
        auto r = p.openFromMemory(
            packMod(manifestFor("com.test.loadfuel", "\"Core\"", 64, 1), coreWasm),
            "loadfuel");
        check("package with fuelPerTurn=1 opens", r == ModLoadResult::Ok,
              p.diagnostic());
        check("unstated loadFuel defaults to the host's allowance",
              p.manifest().limits.loadFuel == ModHostCaps::kDefaultLoadFuel,
              std::to_string(p.manifest().limits.loadFuel));

        auto mi = rt.instantiate(p, MODULE_CORE, err);
        check("instantiates", mi != nullptr, err);
        if (mi) {
            uint32_t ret = 0;
            bool ok = mi->callExport("mod_load", nullptr, 0, &ret, err);
            check("mod_load succeeds despite fuelPerTurn=1", ok && ret == 0, err);

            // ...and the per-turn budget is genuinely still 1, so an ordinary
            // hook is cut off. Without this the test above would also pass if
            // metering had simply been turned off.
            bool cheap = mi->callExport("t_env_size", nullptr, 0, nullptr, err);
            check("a per-turn hook is still held to fuelPerTurn", !cheap, err);
        }
    }

    // ---- GameState.Write ----------------------------------------------------
    // These were shipped stubbed in every fake, so nothing exercised them. The
    // point of the capability is that it validates and refuses rather than
    // half-applying, and that is what is checked here.
    printf("\ngamestate.write\n");
    {
        TurnOnly& w = turnWorld;
        w.treasury = 100.0;
        w.population = 5000;
        w.owner = 0;

        check("set treasury on a known country", w.setCountryTreasury(7, 250.0));
        check("the value took", w.treasury == 250.0, std::to_string(w.treasury));
        check("unknown country is refused", !w.setCountryTreasury(999, 1.0));

        check("add composes with what is there", w.addCountryTreasury(7, -50.0));
        check("add applied", w.treasury == 200.0, std::to_string(w.treasury));

        // NaN and infinity are the values that would silently poison every
        // later sum, so they are the ones worth pinning.
        double nan = std::nan("");
        check("NaN is refused", !w.setCountryTreasury(7, nan));
        check("infinity is refused",
              !w.setCountryTreasury(7, std::numeric_limits<double>::infinity()));
        check("treasury survived the refusals", w.treasury == 200.0,
              std::to_string(w.treasury));

        check("population write is accepted", w.setProvincePopulation(3, 12345));
        check("population took", w.population == 12345,
              std::to_string(w.population));
        check("negative population is refused", !w.setProvincePopulation(3, -1));
        check("absurd population is refused",
              !w.setProvincePopulation(3, 200000000000LL));
        check("population survived the refusals", w.population == 12345,
              std::to_string(w.population));

        check("province transfer is accepted", w.setProvinceOwner(3, 7));
        check("owner took", w.owner == 7, std::to_string(w.owner));
        check("transfer to the current owner is refused", !w.setProvinceOwner(3, 7));
        check("unknown province is refused", !w.setProvinceOwner(99, 7));
    }

    // ---- conflict detection -------------------------------------------------
    // The interesting cases are the ones that must NOT be reported. Declaring
    // the same capability is not a conflict, agreeing is not a conflict, and a
    // mod overwriting its own value is not a conflict -- if any of those were
    // flagged the feature would be noise and users would learn to ignore it.
    printf("\nconflict detection\n");
    {
        ModConflicts& cf = ModConflicts::get();
        cf.clear();
        cf.beginTurn(1);

        cf.recordWrite("com.a", "country:11:treasury", "500");
        check("one mod writing alone is not a conflict", cf.clashes().empty());

        cf.recordWrite("com.a", "country:11:treasury", "600");
        check("a mod overwriting itself is not a conflict", cf.clashes().empty());

        cf.recordWrite("com.b", "country:11:treasury", "600");
        check("two mods agreeing is not a conflict", cf.clashes().empty(),
              std::to_string(cf.clashes().size()));

        cf.recordWrite("com.c", "country:22:treasury", "999");
        check("different targets are not a conflict", cf.clashes().empty(),
              std::to_string(cf.clashes().size()));

        // The real thing: same target, different values, same turn.
        //
        // Two findings, not one, and that is correct: com.a and com.b are both
        // sitting on 600, so com.d disagrees with each of them separately. A
        // conflict is a property of a PAIR, so a mod that disagrees with two
        // others has two conflicts to resolve, and the user may want to
        // override them independently.
        cf.recordWrite("com.d", "country:11:treasury", "1200");
        check("same target, different value, IS a conflict",
              cf.clashes().size() == 2, std::to_string(cf.clashes().size()));
        if (!cf.clashes().empty()) {
            const auto& c = cf.clashes().front();
            check("the report names both mods",
                  (c.modA == "com.d" || c.modB == "com.d") &&
                  (c.modA != c.modB), c.modA + " / " + c.modB);
            check("the report names the target",
                  c.target == "country:11:treasury", c.target);
            check("the report carries both values",
                  c.valueA != c.valueB, c.valueA + " vs " + c.valueB);
        }

        // A mod writing every frame must not produce thousands of findings.
        for (int i = 0; i < 50; i++) cf.recordWrite("com.d", "country:11:treasury", "1200");
        check("repeat writes merge, not accumulate", cf.clashes().size() == 2,
              std::to_string(cf.clashes().size()));
        check("but the repeat count is kept",
              cf.clashes().front().seen > 1,
              std::to_string(cf.clashes().front().seen));

        check("anyFor finds an involved mod", cf.anyFor("com.d"));
        check("anyFor ignores an uninvolved mod", !cf.anyFor("com.zzz"));

        // Alternating turns is taking turns, not fighting.
        cf.clear();
        cf.beginTurn(2);
        cf.recordWrite("com.a", "province:3:owner", "11");
        cf.beginTurn(3);
        cf.recordWrite("com.b", "province:3:owner", "22");
        check("writes in different turns are not a conflict", cf.clashes().empty(),
              std::to_string(cf.clashes().size()));

        // Uninstalling a mod should take its findings with it.
        cf.clear();
        cf.beginTurn(4);
        cf.recordWrite("com.a", "province:3:owner", "11");
        cf.recordWrite("com.b", "province:3:owner", "22");
        check("conflict recorded before forget", cf.clashes().size() == 1);
        cf.forget("com.a");
        check("forget removes that mod's findings", cf.clashes().empty(),
              std::to_string(cf.clashes().size()));
        cf.clear();
    }

    // ---- storage ------------------------------------------------------------
    // Exercised directly rather than through a fixture mod: what needs proving
    // is the quota arithmetic, the namespacing and the on-disk round trip, and
    // none of that is more convincing for having gone through wasm first.
    printf("\nstorage\n");
    {
        std::string dir = std::string(argc > 1 ? argv[1] : ".") + "/storage_test";
        { std::error_code mkec; std::filesystem::create_directories(dir, mkec); }
        // Start from an empty directory. Storage is persistent by design, so
        // without this the second run of the suite would load the first run's
        // files and the namespacing checks below would read as failures.
        {   // std::filesystem: MSVC has no dirent.h. See mod_examples_test.cpp.
            std::error_code ec;
            for (const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
                const std::string n = ent.path().filename().string();
                if (n.size() > 3 && n.compare(n.size() - 3, 3, ".kv") == 0)
                    ::remove((dir + "/" + n).c_str());
            }
        }
        ModStorage& st = ModStorage::get();
        st.clear();
        st.setDir(dir);

        std::string v, err;
        check("absent key reads as absent", !st.get("com.test.a", "k", v));

        check("set stores a value", st.set("com.test.a", "k", "hello", err), err);
        check("get returns it", st.get("com.test.a", "k", v) && v == "hello", v);

        // Absent and empty must stay distinguishable, or "have I stored this
        // yet" becomes unanswerable.
        check("empty value is a value, not an absence",
              st.set("com.test.a", "empty", "", err) &&
              st.get("com.test.a", "empty", v) && v.empty(), err);

        // Binary safety: the on-disk format is length-prefixed precisely so a
        // value containing NUL and newline survives.
        std::string binary("a\0b\nc", 5);
        check("binary values survive", st.set("com.test.a", "bin", binary, err) &&
              st.get("com.test.a", "bin", v) && v == binary, err);

        // The namespace is the point of the capability.
        check("another mod cannot see it", !st.get("com.test.b", "k", v));
        check("same key, different mod, different value",
              st.set("com.test.b", "k", "other", err) &&
              st.get("com.test.a", "k", v) && v == "hello", v);

        check("remove reports it existed", st.remove("com.test.a", "k"));
        check("removed key is gone", !st.get("com.test.a", "k", v));
        check("remove reports absence", !st.remove("com.test.a", "k"));

        // Quotas.
        check("oversized value is refused",
              !st.set("com.test.a", "big",
                      std::string(ModStorage::kMaxValueBytes + 1, 'x'), err), err);
        check("oversized key is refused",
              !st.set("com.test.a", std::string(ModStorage::kMaxKeyBytes + 1, 'k'),
                      "v", err), err);
        check("empty key is refused", !st.set("com.test.a", "", "v", err), err);

        {
            ModStorage& q = ModStorage::get();
            bool refused = false;
            // Each value is a sixteenth of the per-mod budget, so the total
            // quota bites well before the key-count one.
            std::string chunk(ModStorage::kMaxValueBytes, 'z');
            for (int i = 0; i < 64 && !refused; i++)
                if (!q.set("com.test.quota", "k" + std::to_string(i), chunk, err))
                    refused = true;
            check("total quota is enforced", refused, err);
        }

        // Round trip through the filesystem.
        st.flush();
        st.clear();
        check("value survives a flush and reload",
              st.get("com.test.a", "bin", v) && v == binary, v);
        check("the other mod's store survives too",
              st.get("com.test.b", "k", v) && v == "other", v);

        // A corrupt store is dropped rather than half-parsed.
        {
            std::string path = dir + "/com.test.a.kv";
            if (FILE* f = fopen(path.c_str(), "wb")) {
                fputs("GBXKV1\n999 999\nshort", f);
                fclose(f);
            }
            st.clear();
            check("a corrupt store resets instead of guessing",
                  !st.get("com.test.a", "bin", v));
        }
        st.clear();
    }

    // Normalisation: a loadFuel below fuelPerTurn is a mistake that would make
    // the mod fail at load for no visible reason, so it is raised and warned
    // about rather than honoured.
    {
        ModPackage p;
        p.openFromMemory(
            packMod(manifestWithLoadFuel("com.test.lowload", "\"Core\"",
                                         2000000, 1000), coreWasm), "lowload");
        check("loadFuel below fuelPerTurn is raised to match",
              p.manifest().limits.loadFuel == 2000000,
              std::to_string(p.manifest().limits.loadFuel));
    }
    {
        ModPackage p;
        p.openFromMemory(
            packMod(manifestWithLoadFuel("com.test.hugeload", "\"Core\"",
                                         1000, ModHostCaps::kMaxLoadFuel + 1),
                    coreWasm), "hugeload");
        check("loadFuel above the ceiling is clamped",
              p.manifest().limits.loadFuel == ModHostCaps::kMaxLoadFuel,
              std::to_string(p.manifest().limits.loadFuel));
    }

    if (core) {

        modHostLogClear();
        bool aborted = core->callExport("t_abort", nullptr, 0, nullptr, err);
        check("gearbox_abort traps the mod", !aborted, err);
        check("abort message is reported", logContains("deliberate abort"));

        // The instance is left in a trapped state after abort; a fresh one is
        // needed to keep testing, which is exactly what the manager does.
        modHostLogClear();
        auto core2 = rt.instantiate(corePkg, MODULE_CORE, err);
        check("a trapped mod can be re-instantiated", core2 != nullptr, err);
        if (core2) {
            core2->callExport("mod_load", nullptr, 0, nullptr, err);
            modHostLogClear();
            core2->callExport("t_oob_log", nullptr, 0, nullptr, err);
            check("out-of-bounds string is refused, not read",
                  logContains("out-of-bounds"));

            // Growing a little must work, so the failure below is the
            // manifest cap biting rather than growth being broken outright.
            uint32_t before = call0(*core2, "t_mem_pages");
            uint32_t args[1] = {1};
            uint32_t grown = 0;
            core2->callExport("t_grow", args, 1, &grown, err);
            check("growth within the limit succeeds", grown == before,
                  "returned " + std::to_string(grown) + ", was " +
                      std::to_string(before));

            args[0] = 1000;
            core2->callExport("t_grow", args, 1, &grown, err);
            check("memory growth past the manifest limit fails",
                  grown == 0xFFFFFFFFu,
                  "grew to " + std::to_string(grown));
        }
    }

    // ---- the memory cap actually binds at the manifest value ----------------
    // The interesting question is not whether growth can fail, but whether it
    // fails exactly where limits.memoryPages says it should.
    printf("memory limit\n");
    {
        ModPackage capped;
        capped.openFromMemory(
            packMod(manifestFor("com.test.mem", "\"Core\"", /*pages=*/8), coreWasm),
            "mem");
        auto m = rt.instantiate(capped, MODULE_CORE, err);
        check("capped fixture instantiates", m != nullptr, err);
        if (m) {
            uint32_t start = call0(*m, "t_mem_pages");
            check("module starts at 2 pages", start == 2, std::to_string(start));

            uint32_t args[1] = {5}, grown = 0;      // 2 -> 7, under the cap
            m->callExport("t_grow", args, 1, &grown, err);
            check("growth up to the cap succeeds", grown == start,
                  "returned " + std::to_string(grown));
            check("now at 7 pages", call0(*m, "t_mem_pages") == 7,
                  std::to_string(call0(*m, "t_mem_pages")));

            args[0] = 5;                            // 7 -> 12, over the cap of 8
            m->callExport("t_grow", args, 1, &grown, err);
            check("growth past the cap fails", grown == 0xFFFFFFFFu,
                  "returned " + std::to_string(grown));

            args[0] = 1;                            // 7 -> 8, exactly the cap
            m->callExport("t_grow", args, 1, &grown, err);
            check("growth to exactly the cap succeeds", grown == 7,
                  "returned " + std::to_string(grown));
            check("cap is the manifest value", call0(*m, "t_mem_pages") == 8,
                  std::to_string(call0(*m, "t_mem_pages")));
        }
    }

    // ---- UI + capability enforcement ---------------------------------------
    printf("ui capability\n");
    ModPackage uiPkg;
    uiPkg.openFromMemory(
        packMod(manifestFor("com.test.ui", "\"Core\", \"UI\""), uiWasm), "ui");

    // Declared UI, but the user revoked it: the mod must not instantiate at all.
    {
        std::string e2;
        auto refused = rt.instantiate(uiPkg, MODULE_CORE, e2);
        check("ungranted UI import refuses the load", refused == nullptr);
        check("refusal names the missing module",
              e2.find("UI") != std::string::npos, e2);
    }

    ModUI::get().clear();
    auto ui = rt.instantiate(uiPkg, MODULE_CORE | MODULE_UI, err);
    check("ui fixture instantiates when granted", ui != nullptr, err);
    if (ui) {
        ui->callExport("mod_load", nullptr, 0, nullptr, err);
        uint32_t panelId = call0(*ui, "t_panel");
        check("panel_register returned a handle", panelId != 0);

        ModPanel* p = ModUI::get().find(panelId);
        check("host knows the panel", p != nullptr);
        if (p) {
            check("panel is owned by the mod", p->ownerId == "com.test.ui");
            check("panel title came from the mod", p->title == "UI Test", p->title);

            // Draw with the cursor over the button and a click queued.
            ModUI::get().clearCommands();
            p->mouseInside = true;
            p->mouseX = 20; p->mouseY = 50;      // inside (10,40,100,24)
            p->clickPending = true;
            uint32_t args[3] = {panelId, 200, 100};
            ui->callExport("mod_draw_panel", args, 3, nullptr, err);

            check("mod emitted draw commands", p->cmds.size() == 3,
                  std::to_string(p->cmds.size()));
            check("button reported hovered",
                  p->cmds.size() == 3 && p->cmds[2].hovered);
            check("click was consumed", !p->clickPending);
            check("mod observed the click", call0(*ui, "t_clicks") == 1);

            // Same draw, cursor outside: no click.
            ModUI::get().clearCommands();
            p->mouseInside = false;
            p->clickPending = true;
            ui->callExport("mod_draw_panel", args, 3, nullptr, err);
            check("no click when the cursor is outside the panel",
                  call0(*ui, "t_clicks") == 1);

            // Drawing into a panel it does not own must be ignored.
            ModUI::get().clearCommands();
            ui->callExport("t_foreign_draw", nullptr, 0, nullptr, err);
            size_t total = 0;
            for (auto& q : ModUI::get().panels()) total += q.cmds.size();
            check("drawing into an unowned panel is ignored", total == 0,
                  std::to_string(total) + " commands appeared");
        }
    }

    // ---- static initialisers -------------------------------------------------
    // Any toolchain following the WASI reactor convention (TinyGo, Rust wasip1,
    // C# NativeAOT) puts heap and package init behind an `_initialize` export,
    // and C++ puts static constructors behind `__wasm_call_ctors`. If the host
    // jumps straight to mod_load, those mods start with uninitialised state and
    // fail silently.
    printf("static initialisers\n");
    {
        auto ctorWasm = readFile(dir + "/ctortest.wasm");
        if (ctorWasm.empty()) {
            printf("  SKIP  ctortest.wasm missing\n");
        } else {
            ModPackage p;
            auto r = p.openFromMemory(
                packMod(manifestFor("com.test.ctor", "\"Core\""), ctorWasm), "ctor");
            check("ctor fixture packages", r == ModLoadResult::Ok, p.diagnostic());
            auto m = rt.instantiate(p, MODULE_CORE, err);
            check("ctor fixture instantiates", m != nullptr, err);
            if (m) {
                check("static constructors ran before mod_load",
                      call0(*m, "t_ctor_ran") == 1,
                      "the constructor never executed");
            }
        }
    }

    // ---- the reactor convention ---------------------------------------------
    // TinyGo, Rust wasip1 and C# NativeAOT export _initialize and expect the
    // host to call it before anything else. WAMR only does that in WASI builds,
    // which this is not. Observed before the fix, with a real TinyGo 0.41 mod:
    // "mod_load: Exception: unreachable".
    {
        auto reactorWasm = readFile(dir + "/reactortest.wasm");
        if (reactorWasm.empty()) {
            printf("  SKIP  reactortest.wasm missing\n");
        } else {
            ModPackage p;
            p.openFromMemory(
                packMod(manifestFor("com.test.reactor", "\"Core\""), reactorWasm),
                "reactor");
            auto m = rt.instantiate(p, MODULE_CORE, err);
            check("reactor fixture instantiates", m != nullptr, err);
            if (m)
                check("_initialize ran before mod_load",
                      call0(*m, "t_init_ran") == 1,
                      "the module's initialiser was never called");
        }
    }

    // ---- WasiStub ------------------------------------------------------------
    // The shim exists so an interpreter-in-a-mod can boot. It must give them
    // just enough to start and nothing that identifies the machine.
    printf("wasistub\n");
    {
        auto w = readFile(dir + "/wasitest.wasm");
        if (w.empty()) { printf("  SKIP  wasitest.wasm missing\n"); }
        else {
            // Declared but not granted: the mod must not instantiate at all.
            ModPackage denied;
            denied.openFromMemory(
                packMod(manifestFor("com.test.wasi", "\"Core\", \"WasiStub\""), w), "wasi");
            std::string e2;
            auto refused = rt.instantiate(denied, MODULE_CORE, e2);
            check("ungranted WasiStub refuses the load", refused == nullptr);
            check("refusal names the capability",
                  e2.find("WasiStub") != std::string::npos, e2);

            auto m = rt.instantiate(denied, MODULE_CORE | MODULE_WASISTUB, err);
            check("granted WasiStub instantiates", m != nullptr, err);
            if (m) {
                m->callExport("mod_load", nullptr, 0, nullptr, err);

                modHostLogClear();
                uint32_t n = call0(*m, "t_write");
                check("fd_write reports the byte count", n == 17, std::to_string(n));
                check("fd_write reaches the mod log", logContains("printed from wasi"));

                check("fd_write to a non-stdio fd is EBADF",
                      call0(*m, "t_write_badfd") == 8,
                      std::to_string(call0(*m, "t_write_badfd")));

                // Determinism is the whole point: a fresh instance of the same
                // mod must produce the same first bytes, or self-play and save
                // replay stop being reproducible.
                uint32_t r1 = call0(*m, "t_random");
                auto m2 = rt.instantiate(denied, MODULE_CORE | MODULE_WASISTUB, err);
                uint32_t r2 = m2 ? call0(*m2, "t_random") : 0;
                check("random_get is deterministic across instances", r1 == r2,
                      std::to_string(r1) + " vs " + std::to_string(r2));
                check("random_get returns something", r1 != 0);

                // And it must still advance within one instance, or a runtime
                // seeding a hash table gets the same value forever.
                check("random_get advances within an instance",
                      call0(*m, "t_random") != r1);

                check("clock is the turn number, not the wall clock",
                      call0(*m, "t_clock_secs") == 42,
                      std::to_string(call0(*m, "t_clock_secs")));

                check("path_open is refused (ENOTCAPABLE)",
                      call0(*m, "t_open") == 76,
                      std::to_string(call0(*m, "t_open")));
            }
        }
    }

    // ---- headless ------------------------------------------------------------
    printf("headless\n");
    {
        g_modHost.headless = true;
        ModUI::get().clear();
        auto h = rt.instantiate(uiPkg, MODULE_CORE | MODULE_UI, err);
        check("ui mod still loads headless", h != nullptr, err);
        if (h) {
            h->callExport("mod_load", nullptr, 0, nullptr, err);
            check("panel_register no-ops headless", call0(*h, "t_panel") == 0);
            check("no panels exist headless", ModUI::get().panels().empty());
        }
        g_modHost.headless = false;
    }

    printf("\n%d checks, %d failed", g_checks, g_failures);
    if (g_skipped) printf(", %d skipped", g_skipped);
    printf("\n");
    return g_failures == 0 ? 0 : 1;
}
