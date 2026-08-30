// Drives every shipped example mod through its full draw path and compares the
// results across languages.
//
// Why this exists: odmod-check only calls mod_load, and for these examples
// mod_load does little more than register a panel. That proves a module links
// and starts. It does NOT prove the binding is correct -- a language binding
// that declared draw_text's parameters in the wrong order, or got two-call
// sizing backwards, would pass every other test in this suite and then draw
// nonsense in the game.
//
// So: install a synthetic world, run each mod's mod_draw_panel, and capture the
// draw commands it emits. Every hello-panel example is written to do the same
// thing, so the command lists must agree. Where a language disagrees, its
// binding is wrong -- and the diff says how.
//
// Build target: ModExamplesTest. Takes the sdk directory as argv[1].

#include "mods/ModManager.h"
#include "mod_world_stub.h"
#include "mods/ModHost.h"
#include "mods/ModPackage.h"
#include "mods/ModRuntime.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// Headless: Locale calls odText::setComplexArabic, which lives in Text.cpp
// behind raylib. Same stub, same reason, as locale_test.cpp.
namespace odText { void setComplexArabic(bool) {} }

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

// A fixed, deliberately awkward world. The name is long enough to exercise
// two-call sizing, and the treasury has a fractional part so a binding that
// mishandles the f64 return shows up.
class FakeWorld : public StubWorld {
public:
    uint32_t turnNumber() override { return 42; }
    uint32_t countryCount() override { return 3; }
    uint32_t countryAt(uint32_t i) override {
        static const uint32_t ids[3] = {11, 22, 33};
        return i < 3 ? ids[i] : 0xFFFFFFFFu;
    }
    bool countryExists(uint32_t c) override { return c == 11 || c == 22 || c == 33; }
    std::string countryName(uint32_t c) override {
        if (c == 11) return "Grand Duchy of Testphalia";
        if (c == 22) return "Bortania";
        if (c == 33) return "Qet";
        return {};
    }
    double countryTreasury(uint32_t c) override {
        if (c == 11) return 1234.75;
        if (c == 22) return -50.25;
        return 0.0;
    }
    uint32_t countryProvinceCount(uint32_t c) override {
        if (c == 11) return 7;
        if (c == 22) return 19;
        return 0;
    }
    long long provincePopulation(uint32_t) override { return 999; }
    uint32_t provinceOwner(uint32_t) override { return 11; }

    // A tiny three-province map: 101 - 102 - 103 in a line, the middle one sea.
    // Small enough to assert on exactly, and asymmetric enough that a binding
    // reading adjacency backwards would show up.
    uint32_t mapWidth() override { return 640; }
    uint32_t mapHeight() override { return 480; }
    uint32_t provinceCount() override { return 3; }
    uint32_t provinceAt(uint32_t i) override {
        static const uint32_t ids[3] = {101, 102, 103};
        return i < 3 ? ids[i] : 0xFFFFFFFFu;
    }
    bool provinceExists(uint32_t p) override { return p >= 101 && p <= 103; }
    std::string provinceName(uint32_t p) override {
        if (p == 101) return "Westmarch";
        if (p == 102) return "The Narrow Sea";
        if (p == 103) return "Eastmarch";
        return {};
    }
    double provinceCenterX(uint32_t p) override { return 100.0 * (double)(p - 100); }
    double provinceCenterY(uint32_t p) override { return 50.5; }
    bool provinceIsLand(uint32_t p) override { return p != 102; }
    uint32_t provinceNeighborCount(uint32_t p) override {
        return p == 102 ? 2u : (p == 101 || p == 103 ? 1u : 0u);
    }
    uint32_t provinceNeighborAt(uint32_t p, uint32_t i) override {
        if (p == 101) return i == 0 ? 102u : 0xFFFFFFFFu;
        if (p == 103) return i == 0 ? 102u : 0xFFFFFFFFu;
        if (p == 102) return i == 0 ? 101u : (i == 1 ? 103u : 0xFFFFFFFFu);
        return 0xFFFFFFFFu;
    }

    // 11 and 22 are at war; 11 guarantees 33. Deliberately not symmetric in
    // the guarantee, since that is the one relation where direction matters.
    bool atWar(uint32_t a, uint32_t b) override {
        return (a == 11 && b == 22) || (a == 22 && b == 11);
    }
    bool allied(uint32_t, uint32_t) override { return false; }
    bool nonAggression(uint32_t, uint32_t) override { return false; }
    bool guaranteed(uint32_t a, uint32_t b) override { return a == 11 && b == 33; }
    bool proposeWar(uint32_t, uint32_t) override { return false; }
    bool setCountryTreasury(uint32_t, double) override { return false; }
    bool addCountryTreasury(uint32_t, double) override { return false; }
    bool setProvinceOwner(uint32_t, uint32_t) override { return false; }
    bool setProvincePopulation(uint32_t, long long) override { return false; }
    uint32_t neuralFeatureCount() override { return 0; }
    uint32_t neuralFeatures(uint32_t, float*, uint32_t) override { return 0; }
    uint32_t neuralRewardCount() override { return 0; }
    double neuralRewardMean(uint32_t) override { return 0; }
};

struct Capture {
    std::string label;
    std::vector<std::string> texts;      // Text and Button commands, in order
    std::vector<std::string> afterClick; // same, after one click on the button
    size_t rects = 0;
    bool   loaded = false;
    // Not a failure: this mod cannot run on THIS build, and says so precisely.
    // CPython trips WAMR's fast-interpreter INT16_MAX operand-stack limit, so
    // the Python example only loads under -DOD_MODS_FAST_INTERP=OFF. Reporting
    // that as a failure in the default build would train everyone to ignore a
    // red suite.
    bool   skipped = false;
    std::string note;
};

// The loader's exact words when a module is too big for the fast interpreter.
// Matching on the message rather than on the mod's name keeps this honest: any
// mod that hits the limit is skipped, and a Python mod that fails for some
// OTHER reason still fails loudly.
bool isFastInterpLimit(const std::string& err) {
    return err.find("fast interpreter offset overflow") != std::string::npos;
}

void findOdmods(const std::string& dir, std::vector<std::string>& out) {
    // std::filesystem, not dirent.h: MSVC has no such header, so this file
    // could not compile on Windows at all. "." and ".." are never yielded, so
    // the skips readdir needed are gone rather than merely unnecessary.
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n == "node_modules" || n == "target" || n == ".build") continue;
        const std::string p = dir + "/" + n;
        if (e.is_directory()) findOdmods(p, out);
        else if (n.size() > 6 && n.compare(n.size() - 6, 6, ".odmod") == 0)
            out.push_back(p);
    }
}

std::string shortLabel(const std::string& path, const std::string& sdk) {
    std::string r = path.substr(path.rfind('/') + 1);
    if (r.size() > 6) r.resize(r.size() - 6);
    (void)sdk;
    return r;
}

Capture drive(const std::string& path, const std::string& sdk) {
    Capture cap;
    cap.label = shortLabel(path, sdk);

    static std::vector<std::unique_ptr<ModPackage>> keep;   // instances point in
    keep.push_back(std::make_unique<ModPackage>());
    ModPackage& pkg = *keep.back();

    if (pkg.open(path) != ModLoadResult::Ok) {
        cap.note = pkg.diagnostic();
        return cap;
    }

    std::string err;
    // Grant exactly what the manifest asks for, which is what the mod menu
    // defaults to.
    auto inst = ModRuntime::get().instantiate(pkg, pkg.manifest().modules, err);
    if (!inst) {
        cap.note = err;
        cap.skipped = isFastInterpLimit(err);
        return cap;
    }

    uint32_t ret = 0;
    if (!inst->callExport("mod_load", nullptr, 0, &ret, err)) { cap.note = err; return cap; }
    if (ret != 0) { cap.note = "mod_load returned " + std::to_string(ret); return cap; }
    cap.loaded = true;

    // Find the panel the mod registered for itself.
    ModPanel* panel = nullptr;
    for (auto& p : ModUI::get().panels())
        if (p.ownerId == pkg.manifest().id) { panel = &p; break; }
    if (!panel) { cap.note = "registered no panel"; return cap; }

    if (!inst->hasExport("mod_draw_panel")) { cap.note = "no mod_draw_panel"; return cap; }

    ModUI::get().clearCommands();
    panel->x = 100; panel->y = 100; panel->w = 300; panel->h = 200;
    panel->mouseInside = false;          // no click: keeps the cursor at index 0
    panel->clickPending = false;

    uint32_t args[3] = {panel->id, 300, 200};
    if (!inst->callExport("mod_draw_panel", args, 3, nullptr, err)) {
        cap.note = "mod_draw_panel: " + err;
        return cap;
    }

    for (const auto& c : panel->cmds) {
        if (c.kind == ModDrawCmd::Rect) cap.rects++;
        else cap.texts.push_back(c.text);
    }

    // Second pass with a click inside the button. This is the only thing that
    // exercises gearbox_button's *return value* -- everything above would pass
    // even if a binding always returned 0 from it.
    //
    // The examples put their button at (8, 116, 120, 24) in panel-relative
    // coordinates, so (20, 128) is inside it.
    ModUI::get().clearCommands();
    panel->mouseInside = true;
    panel->mouseX = 20;
    panel->mouseY = 128;
    panel->clickPending = true;
    if (inst->callExport("mod_draw_panel", args, 3, nullptr, err)) {
        // The click advances the mod's cursor; a third draw shows the new state.
        ModUI::get().clearCommands();
        panel->clickPending = false;
        panel->mouseInside = false;
        if (inst->callExport("mod_draw_panel", args, 3, nullptr, err))
            for (const auto& c : panel->cmds)
                if (c.kind != ModDrawCmd::Rect) cap.afterClick.push_back(c.text);
    }
    return cap;
}

}  // namespace

int main(int argc, char** argv) {
    std::string sdk = argc > 1 ? argv[1] : "../sdk";

    printf("example mods, full draw path\n");
    if (!ModRuntime::get().available()) {
        printf("  SKIP  no WASM backend in this build\n\n0 checks, 0 failed\n");
        return 0;
    }
    std::string err;
    if (!ModRuntime::get().init(err)) {
        printf("  FAIL  runtime init: %s\n", err.c_str());
        return 1;
    }

    FakeWorld world;
    g_modGame = &world;
    g_modHost.headless = false;
    g_modHost.screenW = 1600;
    g_modHost.screenH = 900;

    std::vector<std::string> paths;
    findOdmods(sdk, paths);
    std::sort(paths.begin(), paths.end());

    if (paths.empty()) {
        printf("  SKIP  no .odmod files under %s\n", sdk.c_str());
        printf("        build the examples first (sdk/*/build.sh)\n");
        printf("\n0 checks, 0 failed\n");
        return 0;
    }

    std::vector<Capture> caps;
    for (const auto& p : paths) {
        ModUI::get().clear();
        caps.push_back(drive(p, sdk));
    }

    // --- what each mod actually drew ----------------------------------------
    printf("\n");
    for (const auto& c : caps) {
        printf("  %-24s ", c.label.c_str());
        if (c.skipped) {
            printf("SKIP  needs -DOD_MODS_FAST_INTERP=OFF\n");
            continue;
        }
        if (!c.loaded) { printf("did not load: %s\n", c.note.c_str()); continue; }
        if (!c.note.empty()) { printf("%s\n", c.note.c_str()); continue; }
        printf("%zu rect(s), %zu text(s)\n", c.rects, c.texts.size());
        for (const auto& t : c.texts) printf("      | %s\n", t.c_str());
    }
    printf("\n");

    for (const auto& c : caps) {
        if (c.skipped) {
            printf("  skip  %s (not runnable on this interpreter)\n", c.label.c_str());
            continue;
        }
        check(c.label + " loads and draws",
              c.loaded && c.note.empty() && !c.texts.empty(),
              c.note.empty() ? "drew nothing" : c.note);
    }

    // --- cross-language agreement -------------------------------------------
    // The full hello-panel examples all render the same five strings. WAT is a
    // deliberately minimal module and is not expected to match.
    printf("\ncross-language agreement\n");
    std::vector<const Capture*> full;
    for (const auto& c : caps) {
        if (!c.loaded || !c.note.empty()) continue;
        if (c.label.find("wat") != std::string::npos) continue;
        full.push_back(&c);
    }

    if (full.size() < 2) {
        printf("  SKIP  need at least two full examples built\n");
    } else {
        const Capture* ref = full.front();
        for (size_t i = 1; i < full.size(); i++) {
            const Capture* c = full[i];
            bool same = c->texts.size() == ref->texts.size();
            std::string detail;
            if (same) {
                for (size_t k = 0; k < c->texts.size(); k++) {
                    if (c->texts[k] == ref->texts[k]) continue;
                    same = false;
                    detail = "line " + std::to_string(k) + ": " + ref->label +
                             " drew \"" + ref->texts[k] + "\", " + c->label +
                             " drew \"" + c->texts[k] + "\"";
                    break;
                }
            } else {
                detail = ref->label + " drew " + std::to_string(ref->texts.size()) +
                         " strings, " + c->label + " drew " +
                         std::to_string(c->texts.size());
            }
            check(c->label + " agrees with " + ref->label, same, detail);
        }

        // The values must actually come from the world, not be hardcoded.
        bool sawTurn = false, sawCount = false, sawName = false, sawTreasury = false;
        for (const auto& t : ref->texts) {
            if (t.find("42") != std::string::npos) sawTurn = true;
            if (t.find("3") != std::string::npos) sawCount = true;
            if (t.find("Testphalia") != std::string::npos) sawName = true;
            if (t.find("1234") != std::string::npos) sawTreasury = true;
        }
        printf("\nvalues reach the mod\n");
        check("turn number (42) was read", sawTurn);
        check("country count (3) was read", sawCount);
        check("country name survived two-call sizing", sawName);
        check("treasury (1234.75) was read", sawTreasury);

        // A click must move every mod to the second country. If a binding gets
        // gearbox_button's return value wrong, its state never advances.
        printf("\nbutton return value\n");
        for (const auto* c : full) {
            bool advanced = false;
            for (const auto& t : c->afterClick)
                if (t.find("Bortania") != std::string::npos) advanced = true;
            std::string drew;
            for (const auto& t : c->afterClick) drew += "\"" + t + "\" ";
            check(c->label + " advanced on click", advanced,
                  c->afterClick.empty() ? "drew nothing after the click" : drew);
        }
    }

    g_modGame = nullptr;
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
