// Fixture-driven tests for the .odmod reader.
//
// Every rejection reason in docs/modding.md gets an archive built here on the
// fly to trigger it, including the zip bombs. Fixtures are generated rather
// than committed so that a hostile archive never sits in the repo, and so the
// test stays readable as a description of what is rejected and why.
//
// Build target: ModArchiveTest. Run it; non-zero exit means a case failed.

#include "mods/ModPackage.h"

#include "test_zip.h"

#include "miniz.h"
#include "miniz_zip.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

// A module the reader will accept: magic plus version, which is all it checks.
std::vector<uint8_t> minimalWasm() {
    return {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
}

std::string goodManifest() {
    return R"({
  "schema": 1,
  "id": "com.example.test-mod",
  "name": "Test Mod",
  "version": "1.2.3",
  "description": "Fixture.",
  "authors": ["Fixture Author"],
  "gearbox": "1.0",
  "modules": ["Core", "UI"],
  "limits": { "memoryPages": 64, "fuelPerTurn": 100000 }
})";
}

// Deterministic pseudo-random bytes: incompressible, so they pin the
// compressed size and therefore the archive's expansion ratio.
std::vector<uint8_t> noise(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t s = seed | 1u;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        v[i] = (uint8_t)(s >> 24);
    }
    return v;
}

// ----------------------------------------------------------------- checks --

void expect(const std::string& what, std::vector<uint8_t> archive,
            ModLoadResult want, const char* wantDiag = nullptr) {
    g_checks++;
    if (archive.empty()) {
        printf("  FAIL  %-28s fixture archive could not be built\n", what.c_str());
        g_failures++;
        return;
    }
    ModPackage p;
    ModLoadResult got = p.openFromMemory(std::move(archive), "fixture");
    if (got == want) {
        printf("  ok    %-28s %s\n", what.c_str(), modLoadResultName(got));
        if (got != ModLoadResult::Ok && p.diagnostic().empty()) {
            printf("  FAIL  %-28s rejected with an empty diagnostic\n", what.c_str());
            g_failures++;
        } else if (wantDiag &&
                   p.diagnostic().find(wantDiag) == std::string::npos) {
            printf("  FAIL  %-28s diagnostic should mention \"%s\": %s\n",
                   what.c_str(), wantDiag, p.diagnostic().c_str());
            g_failures++;
        }
        return;
    }
    printf("  FAIL  %-28s want %s, got %s (%s)\n", what.c_str(),
           modLoadResultName(want), modLoadResultName(got),
           p.diagnostic().empty() ? "no diagnostic" : p.diagnostic().c_str());
    g_failures++;
}

void check(const std::string& what, bool cond, const std::string& detail = "") {
    g_checks++;
    if (cond) { printf("  ok    %s\n", what.c_str()); return; }
    printf("  FAIL  %s%s%s\n", what.c_str(), detail.empty() ? "" : " — ",
           detail.c_str());
    g_failures++;
}

// Builds a well-formed archive, letting the caller substitute pieces.
std::vector<uint8_t> buildMod(const std::string& manifest,
                              const std::vector<uint8_t>& wasm,
                              bool includeManifest = true,
                              bool includeWasm = true) {
    Zip z;
    if (includeManifest) z.add("MANIFEST.json", manifest);
    if (includeWasm) z.add("mod.wasm", wasm);
    return z.finish();
}

// Replaces one field's value. Splits on the line end, not on the first comma:
// array and object values contain commas of their own.
std::string manifestWith(const std::string& field, const std::string& value) {
    std::string m = goodManifest();
    size_t at = m.find("\"" + field + "\"");
    if (at == std::string::npos) return m;
    size_t colon = m.find(':', at);
    size_t eol = m.find('\n', colon);
    if (eol == std::string::npos) eol = m.size();
    size_t lastCh = m.find_last_not_of(" \t\r", eol - 1);
    bool trailingComma = lastCh != std::string::npos && m[lastCh] == ',';
    return m.substr(0, colon + 1) + " " + value + (trailingComma ? "," : "") +
           m.substr(eol);
}

// Adds a field the good manifest does not have. manifestWith() only rewrites
// an existing one and silently returns the manifest untouched otherwise, which
// would make a test for an optional field pass without testing anything.
std::string manifestPlus(const std::string& field, const std::string& value) {
    std::string m = goodManifest();
    size_t at = m.find("\"schema\"");
    size_t eol = m.find('\n', at);
    return m.substr(0, eol + 1) + "  \"" + field + "\": " + value + ",\n" +
           m.substr(eol + 1);
}

// Rewrites an entry name in place, in both the local header and the central
// directory. Needed because miniz's writer sanitises names it considers
// invalid — it silently drops a leading '/' — so an archive containing a
// genuinely absolute path cannot be produced by a well-behaved writer. An
// attacker is not using a well-behaved writer, so neither do we. The name
// length is unchanged, and the CRC covers the data rather than the name, so
// the archive stays structurally valid.
void renameEntry(std::vector<uint8_t>& zip, const std::string& from,
                 const std::string& to) {
    if (from.size() != to.size()) { printf("  FAIL  renameEntry length\n"); g_failures++; return; }
    int hits = 0;
    for (size_t i = 0; i + from.size() <= zip.size(); i++) {
        if (memcmp(zip.data() + i, from.data(), from.size()) != 0) continue;
        memcpy(zip.data() + i, to.data(), to.size());
        hits++;
    }
    if (hits < 2) {   // local header + central directory
        printf("  FAIL  renameEntry patched %d site(s) for %s\n", hits, from.c_str());
        g_failures++;
    }
}

}  // namespace

int main() {
    printf("odmod reader\n");

    // ---- accepts a well-formed mod ----------------------------------------
    {
        ModPackage p;
        auto r = p.openFromMemory(buildMod(goodManifest(), minimalWasm()), "good");
        g_checks++;
        if (r != ModLoadResult::Ok) {
            printf("  FAIL  valid mod rejected: %s\n", p.diagnostic().c_str());
            g_failures++;
        } else {
            printf("  ok    valid mod accepted\n");
            check("  id parsed", p.manifest().id == "com.example.test-mod",
                  p.manifest().id);
            check("  version parsed", p.manifest().version == "1.2.3");
            check("  api version parsed",
                  p.manifest().gearboxMajor == 1 && p.manifest().gearboxMinor == 0);
            check("  modules parsed",
                  p.manifest().modules == (MODULE_CORE | MODULE_UI),
                  modModuleMaskToString(p.manifest().modules));
            check("  wasm extracted", p.wasm().size() == 8);
            check("  no warnings", p.warnings().empty(),
                  p.warnings().empty() ? "" : p.warnings()[0]);
        }
    }

    // ---- container structure ----------------------------------------------
    printf("container\n");
    expect("not a zip", std::vector<uint8_t>{'n', 'o', 'p', 'e'},
           ModLoadResult::NotAnArchive);
    expect("no manifest", buildMod(goodManifest(), minimalWasm(), false, true),
           ModLoadResult::ManifestMissing);
    expect("no wasm", buildMod(goodManifest(), minimalWasm(), true, false),
           ModLoadResult::WasmMissing);
    {
        Zip z;
        z.add("mod.wasm", minimalWasm());          // manifest second
        z.add("MANIFEST.json", goodManifest());
        expect("manifest not first", z.finish(), ModLoadResult::ManifestNotFirst);
    }
    {
        Zip z;
        z.add("MANIFEST.json", goodManifest());
        z.add("mod.wasm", std::string("not wasm at all"));
        expect("wasm bad magic", z.finish(), ModLoadResult::WasmInvalid);
    }

    // ---- entry name safety -------------------------------------------------
    printf("entry names\n");
    struct NameCase { const char* name; const char* why; };
    for (const NameCase& c : {NameCase{"../escape.txt",        "'..' path component"},
                              NameCase{"data/../../escape.txt", "'..' path component"},
                              NameCase{"data\\win.txt",         "backslash"},
                              NameCase{"./here.txt",            "'.' path component"}}) {
        Zip z;
        z.add("MANIFEST.json", goodManifest());
        z.add("mod.wasm", minimalWasm());
        z.add(c.name, std::string("x"));
        expect(std::string("unsafe: ") + c.name, z.finish(),
               ModLoadResult::EntryNameUnsafe, c.why);
    }
    for (auto pair : {std::pair<const char*, const char*>{"Xetc/passwd", "/etc/passwd"},
                      std::pair<const char*, const char*>{"XX/abs.txt",  "C:/abs.txt"}}) {
        Zip z;
        z.add("MANIFEST.json", goodManifest());
        z.add("mod.wasm", minimalWasm());
        z.add(pair.first, std::string("x"));
        auto a = z.finish();
        renameEntry(a, pair.first, pair.second);
        expect(std::string("unsafe: ") + pair.second, std::move(a),
               ModLoadResult::EntryNameUnsafe);
    }
    {
        Zip z;
        z.add("MANIFEST.json", goodManifest());
        z.add("mod.wasm", minimalWasm());
        std::string deep = "data";
        for (int i = 0; i < 20; i++) deep += "/d";
        z.add(deep + "/f.txt", std::string("x"));
        expect("path too deep", z.finish(), ModLoadResult::PathTooDeep);
    }
    {
        // Overlong UTF-8 for '/': two spellings of one name is how a traversal
        // sneaks past a byte comparison, so the encoding itself is rejected.
        Zip z;
        z.add("MANIFEST.json", goodManifest());
        z.add("mod.wasm", minimalWasm());
        z.add(std::string("data/bad\xC0\xAF" "name.txt"), std::string("x"));
        expect("overlong utf-8", z.finish(), ModLoadResult::EntryNameUnsafe);
    }

    // ---- archive limits ----------------------------------------------------
    printf("archive limits\n");
    {
        Zip z;
        z.add("MANIFEST.json", goodManifest());
        z.add("mod.wasm", minimalWasm());
        for (int i = 0; i < 5000; i++) {
            char n[64];
            snprintf(n, sizeof n, "data/f%04d.bin", i);
            z.add(n, std::string("x"));
        }
        expect("too many entries", z.finish(), ModLoadResult::TooManyEntries);
    }
    {
        // 16 MiB of zeros deflates to a few KiB: well past 200:1.
        Zip z;
        z.add("MANIFEST.json", goodManifest());
        z.add("mod.wasm", minimalWasm());
        z.add("data/bomb.bin", std::string(16 * 1024 * 1024, '\0'));
        expect("entry bomb", z.finish(), ModLoadResult::EntryRatioExceeded);
    }
    {
        // Each entry sits under the 200:1 per-entry limit but the archive as a
        // whole expands past 100:1 — the case a per-entry check alone misses.
        Zip z;
        z.add("MANIFEST.json", goodManifest());
        z.add("mod.wasm", minimalWasm());
        const size_t kPad = 1024 * 1024;         // zeros: ~free
        const size_t kSeed = kPad / 150;         // noise: pins the ratio ~150:1
        for (int i = 0; i < 8; i++) {
            std::vector<uint8_t> e(kPad, 0);
            auto n = noise(kSeed, 1234u + i);
            memcpy(e.data(), n.data(), n.size());
            char nm[64];
            snprintf(nm, sizeof nm, "data/mix%d.bin", i);
            z.add(nm, e);
        }
        expect("whole-archive ratio", z.finish(),
               ModLoadResult::ArchiveRatioExceeded);
    }

    // ---- manifest validation ----------------------------------------------
    printf("manifest\n");
    expect("not json", buildMod("{ this is not json", minimalWasm()),
           ModLoadResult::ManifestNotJson);
    expect("json but not object", buildMod("[1,2,3]", minimalWasm()),
           ModLoadResult::ManifestNotJson);
    expect("bad schema", buildMod(manifestWith("schema", "99"), minimalWasm()),
           ModLoadResult::ManifestInvalid);
    expect("uppercase id",
           buildMod(manifestWith("id", "\"com.example.MyMod\""), minimalWasm()),
           ModLoadResult::ManifestInvalid);
    expect("id without dot",
           buildMod(manifestWith("id", "\"mymod\""), minimalWasm()),
           ModLoadResult::ManifestInvalid);
    expect("id with slash",
           buildMod(manifestWith("id", "\"com/example/mod\""), minimalWasm()),
           ModLoadResult::ManifestInvalid);
    expect("bad semver",
           buildMod(manifestWith("version", "\"1.2\""), minimalWasm()),
           ModLoadResult::ManifestInvalid);
    expect("unknown module",
           buildMod(manifestWith("modules", "[\"Core\", \"Filesystem\"]"),
                    minimalWasm()),
           ModLoadResult::UnknownModule);
    expect("wrong api major",
           buildMod(manifestWith("gearbox", "\"2.0\""), minimalWasm()),
           ModLoadResult::IncompatibleApiVersion);
    expect("bad public key",
           buildMod(goodManifest().insert(goodManifest().find("\"limits\""),
                                          "\"publicKey\": \"rsa:AAAA\",\n  "),
                    minimalWasm()),
           ModLoadResult::ManifestInvalid);

    // ---- semantics ---------------------------------------------------------
    printf("semantics\n");
    {
        ModPackage p;
        p.openFromMemory(buildMod(manifestWith("modules",
                                               "[\"Core\", \"GameState.Write\"]"),
                                  minimalWasm()), "implies");
        check("Write implies Read",
              (p.manifest().modules & MODULE_GAMESTATE_READ) != 0,
              modModuleMaskToString(p.manifest().modules));
    }
    {
        ModPackage p;
        p.openFromMemory(buildMod(manifestWith("modules", "[\"UI\"]"),
                                  minimalWasm()), "core");
        check("Core always granted",
              (p.manifest().modules & MODULE_CORE) != 0);
    }

    // ---- side --------------------------------------------------------------
    {
        ModPackage p;
        auto r = p.openFromMemory(buildMod(goodManifest(), minimalWasm()), "noside");
        check("a manifest with no \"side\" defaults to both",
              r == ModLoadResult::Ok && p.manifest().side == ModSide::Both,
              modSideName(p.manifest().side));
    }
    for (const char* want : {"client", "server", "both"}) {
        ModPackage p;
        p.openFromMemory(buildMod(manifestPlus("side", std::string("\"") + want + "\""),
                                  minimalWasm()), "side");
        check((std::string("\"side\": \"") + want + "\" parses").c_str(),
              std::string(modSideName(p.manifest().side)) == want,
              modSideName(p.manifest().side));
    }
    {
        // A future release adding a side must not stop today's game loading a
        // mod. Warn, fall back to "both", keep going.
        ModPackage p;
        auto r = p.openFromMemory(buildMod(manifestPlus("side", "\"proxy\""),
                                           minimalWasm()), "unknownside");
        check("an unknown side loads anyway, as both",
              r == ModLoadResult::Ok && p.manifest().side == ModSide::Both);
        check("and warns about it", !p.warnings().empty(),
              p.warnings().empty() ? "(no warnings)" : p.warnings()[0]);
    }
    {
        ModPackage p;
        auto r = p.openFromMemory(buildMod(manifestPlus("side", "3"), minimalWasm()),
                                  "numericside");
        check("a non-string side is rejected", r == ModLoadResult::ManifestInvalid,
              modLoadResultName(r));
    }
    {
        // Not an error -- it may be running singleplayer, where side means
        // nothing -- but the usual cause is a mod that meant to say "both".
        std::string m = manifestPlus("side", "\"client\"");
        // Replace the module list in the already-extended manifest.
        size_t at = m.find("\"modules\"");
        size_t eol = m.find('\n', at);
        m = m.substr(0, at) + "\"modules\": [\"Core\", \"GameState.Write\"]," +
            m.substr(eol);
        ModPackage p;
        auto r = p.openFromMemory(buildMod(m, minimalWasm()), "clientwrite");
        check("a client-side mod asking for Write loads", r == ModLoadResult::Ok,
              p.diagnostic());
        check("but is warned that the grant is masked in multiplayer",
              !p.warnings().empty(),
              p.warnings().empty() ? "(no warnings)" : p.warnings()[0]);
    }

    // ---- archive digest ----------------------------------------------------
    {
        ModPackage a, b;
        auto bytes = buildMod(goodManifest(), minimalWasm());
        a.openFromMemory(bytes, "a");
        b.openFromMemory(bytes, "b");
        check("the same archive digests the same", a.sha256() == b.sha256(),
              a.sha256());
        check("and it is a lowercase hex sha256", a.sha256().size() == 64 &&
              a.sha256().find_first_not_of("0123456789abcdef") == std::string::npos,
              a.sha256());

        ModPackage c;
        c.openFromMemory(buildMod(manifestWith("version", "\"9.9.9\""), minimalWasm()), "c");
        check("a different archive digests differently", c.sha256() != a.sha256());
    }
    {
        std::string m = goodManifest();
        size_t at = m.find("\"limits\"");
        m = m.substr(0, at) + "\"limits\": { \"memoryPages\": 999999, "
                              "\"fuelPerTurn\": 999999999999 }\n}";
        ModPackage p;
        auto r = p.openFromMemory(buildMod(m, minimalWasm()), "clamp");
        check("over-large limits still load", r == ModLoadResult::Ok,
              p.diagnostic());
        check("memoryPages clamped",
              p.manifest().limits.memoryPages == ModHostCaps::kMaxMemoryPages,
              std::to_string(p.manifest().limits.memoryPages));
        check("fuel clamped",
              p.manifest().limits.fuelPerTurn == ModHostCaps::kMaxFuelPerTurn);
        check("clamping warned about", p.warnings().size() == 2,
              std::to_string(p.warnings().size()) + " warnings");
    }
    {
        // Newer minor loads with a warning rather than being refused.
        ModPackage p;
        auto r = p.openFromMemory(
            buildMod(manifestWith("gearbox", "\"1.9\""), minimalWasm()), "minor");
        check("newer minor loads", r == ModLoadResult::Ok, p.diagnostic());
        check("newer minor warns", !p.warnings().empty());
    }
    {
        Zip z;
        z.add("MANIFEST.json", manifestWith("modules", "[\"Core\", \"Assets\"]"));
        z.add("mod.wasm", minimalWasm());
        z.add("data/hello.txt", std::string("hello assets"));
        z.add("data/nested/deep.txt", std::string("deeper"));
        ModPackage p;
        auto r = p.openFromMemory(z.finish(), "assets");
        check("assets archive loads", r == ModLoadResult::Ok, p.diagnostic());
        check("asset names stripped of data/",
              p.assetNames().size() == 2 && p.assetNames()[0] == "hello.txt" &&
                  p.assetNames()[1] == "nested/deep.txt");
        std::vector<uint8_t> out;
        check("asset inflates on demand",
              p.readAsset("hello.txt", out) &&
                  std::string(out.begin(), out.end()) == "hello assets");
        check("unknown asset refused", !p.readAsset("nope.txt", out));
        check("asset path escape refused", !p.readAsset("../MANIFEST.json", out));
    }
    {
        // data/ without the Assets module is a mod bug worth surfacing, not a
        // load failure.
        Zip z;
        z.add("MANIFEST.json", goodManifest());          // Core, UI only
        z.add("mod.wasm", minimalWasm());
        z.add("data/unused.txt", std::string("x"));
        ModPackage p;
        auto r = p.openFromMemory(z.finish(), "noassets");
        check("data without Assets loads", r == ModLoadResult::Ok, p.diagnostic());
        check("data without Assets warns", !p.warnings().empty());
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
