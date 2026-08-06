// Conformance test: the host's capability table must match sdk/abi.json exactly.
//
// This is what makes multi-language SDKs maintainable. Every binding under sdk/
// is a transcription of abi.json, and none of them can be compiled or run here.
// Checking each binding against the host is therefore impossible; checking that
// abi.json still describes the host is easy, and it is the same guarantee one
// step removed. If someone adds a host function, changes a signature, or moves a
// function to a different capability without updating abi.json, this fails and
// names the discrepancy.
//
// The check runs in both directions, because either half alone is a trap: a
// host-only function would be missing from every SDK, and an abi.json-only
// function would appear in every SDK and fail to link for the modder.
//
// Build target: ModAbiTest.

#include "mods/ModPackage.h"
#include "mods/ModRuntime.h"
#include "json.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>

using json = nlohmann::json;

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

std::string key(const std::string& module, const std::string& name) {
    return module + "." + name;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : "../sdk/abi.json";

    printf("abi conformance (%s)\n", path.c_str());

    std::ifstream f(path);
    if (!f) {
        printf("  FAIL  cannot open %s\n", path.c_str());
        printf("        pass the path as argv[1] if running from elsewhere\n");
        return 1;
    }
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        printf("  FAIL  %s is not valid JSON\n", path.c_str());
        return 1;
    }

    // ---- the declared ABI version must match what the host advertises -------
    //
    // Against the host's OWN constants, not a literal. Comparing to "1.0" meant
    // bumping kHostGearboxMinor and forgetting abi.json still passed, which is
    // precisely the drift this file exists to catch.
    {
        const std::string hostVer = std::to_string(kHostGearboxMajor) + "." +
                                    std::to_string(kHostGearboxMinor);
        check("abi.json declares the host's gearbox version",
              j.value("gearbox", std::string()) == hostVer,
              "abi.json says " + j.value("gearbox", std::string("(missing)")) +
                  ", host says " + hostVer);
    }

    // ---- abi.json must agree with ITSELF --------------------------------------
    //
    // The version appears twice in this file: as the "gearbox" string and as the
    // GEARBOX_MAJOR/MINOR constants the C header is generated from. Nothing
    // compared them, and they drifted -- "gearbox" was hand-edited to 1.1 while
    // the constants stayed at 1.0, so sdk/gearbox_generated.h shipped
    // GEARBOX_MINOR 0 to every modder while the host announced 1.
    {
        auto consts = j.find("constants");
        int cmaj = -1, cmin = -1;
        if (consts != j.end()) {
            cmaj = consts->value("GEARBOX_MAJOR", -1);
            cmin = consts->value("GEARBOX_MINOR", -1);
        }
        const std::string fromConsts = std::to_string(cmaj) + "." + std::to_string(cmin);
        check("abi.json constants match its own gearbox string",
              fromConsts == j.value("gearbox", std::string()),
              "constants say " + fromConsts + ", \"gearbox\" says " +
                  j.value("gearbox", std::string("(missing)")));
    }

    // ---- env struct layout ---------------------------------------------------
    // ModHost.cpp static_asserts its own struct at 28 bytes; this pins the
    // number the SDKs are written against to the same value.
    {
        auto e = j.find("env_struct");
        check("env_struct is described", e != j.end() && e->is_object());
        if (e != j.end() && e->is_object()) {
            check("env_struct is 28 bytes", e->value("size_bytes", 0) == 28,
                  std::to_string(e->value("size_bytes", 0)));
            auto flds = e->find("fields");
            check("env_struct lists 10 fields",
                  flds != e->end() && flds->is_array() && flds->size() == 10,
                  flds != e->end() && flds->is_array()
                      ? std::to_string(flds->size()) : "missing");
            // Offsets must be consistent with the declared sizes, or an SDK
            // author transcribing them writes into the wrong words.
            uint32_t at = 0;
            bool offsetsOk = true;
            if (flds != e->end() && flds->is_array()) {
                for (const auto& fl : *flds) {
                    std::string t = fl.value("type", std::string());
                    uint32_t want = fl.value("offset", 0xFFFFFFFFu);
                    if (want != at) { offsetsOk = false; break; }
                    at += (t == "u8") ? 1u : 4u;
                }
            }
            check("env_struct offsets are contiguous and correct", offsetsOk,
                  "diverges at byte " + std::to_string(at));
        }
    }

    // ---- imports: abi.json vs the host table, both directions ---------------
    size_t hostCount = 0;
    const ModHostFn* host = modHostFunctions(hostCount);

    std::set<std::string> inAbi, inHost;
    for (size_t i = 0; i < hostCount; i++)
        inHost.insert(key(host[i].module, host[i].name));

    auto imports = j.find("imports");
    check("abi.json lists imports", imports != j.end() && imports->is_array());
    if (imports == j.end() || !imports->is_array()) {
        printf("\n%d checks, %d failed\n", g_checks, g_failures);
        return 1;
    }

    for (const auto& imp : *imports) {
        std::string module = imp.value("module", std::string());
        std::string name = imp.value("name", std::string());
        std::string k = key(module, name);
        inAbi.insert(k);

        // present in the host at all?
        const ModHostFn* fn = modFindHostFunction(module.c_str(), name.c_str());
        if (!fn) {
            check(k + " exists in the host", false,
                  "declared in abi.json but the host does not provide it — every "
                  "SDK would export a binding that fails to link");
            continue;
        }

        // wire signature
        std::string want = imp.value("signature", std::string());
        check(k + " signature", want == fn->signature,
              "abi.json says " + want + ", host says " + fn->signature);

        // capability
        std::string capName;
        if (imp.contains("capability") && !imp["capability"].is_null())
            capName = imp["capability"].get<std::string>();
        uint32_t wantCap = capName.empty() ? 0u : modModuleFromName(capName);
        if (!capName.empty() && wantCap == 0) {
            check(k + " capability is a known module", false, capName);
        } else {
            check(k + " capability", wantCap == fn->capability,
                  "abi.json says " + (capName.empty() ? "none" : capName) +
                      ", host says " +
                      (fn->capability ? modModuleMaskToString(fn->capability)
                                      : "none"));
        }
    }

    // The other direction: nothing the host provides may be undocumented.
    for (size_t i = 0; i < hostCount; i++) {
        std::string k = key(host[i].module, host[i].name);
        if (inAbi.count(k)) continue;
        check(k + " is documented in abi.json", false,
              "the host provides it but abi.json does not list it — no SDK "
              "would expose it and no modder would know it exists");
    }

    check("import counts agree", inAbi.size() == inHost.size(),
          std::to_string(inAbi.size()) + " in abi.json, " +
              std::to_string(inHost.size()) + " in the host");

    // ---- modules -------------------------------------------------------------
    auto modules = j.find("modules");
    check("abi.json lists modules", modules != j.end() && modules->is_array());
    if (modules != j.end() && modules->is_array()) {
        for (const auto& m : *modules) {
            std::string n = m.value("name", std::string());
            check("module " + n + " is a capability the host knows",
                  modModuleFromName(n) != 0, n);
        }
        // Every capability bit the host defines must be described, or the
        // Advanced permissions panel would show a module the docs never mention.
        for (uint32_t bit = 1; bit; bit <<= 1) {
            std::string nm = modModuleMaskToString(bit);
            if (nm.empty()) continue;
            bool found = false;
            for (const auto& m : *modules)
                if (m.value("name", std::string()) == nm) { found = true; break; }
            check("module " + nm + " is described in abi.json", found);
        }
    }

    // ---- exports -------------------------------------------------------------
    // The host calls these by name; a typo here ships broken SDKs everywhere.
    auto exports = j.find("exports");
    check("abi.json lists exports", exports != j.end() && exports->is_array());
    if (exports != j.end() && exports->is_array()) {
        std::set<std::string> names;
        for (const auto& e : *exports) names.insert(e.value("name", std::string()));
        for (const char* required : {"mod_load", "mod_unload", "mod_pre_turn",
                                     "mod_post_turn", "mod_draw_panel"})
            check(std::string("export ") + required + " is described",
                  names.count(required) == 1);
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
