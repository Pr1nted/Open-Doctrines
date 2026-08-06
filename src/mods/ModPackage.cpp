#include "ModPackage.h"

#include "miniz.h"
#include "miniz_zip.h"
#include "json.hpp"
#include "../util/Sha256.h"

#include <cstdio>
#include <cstring>
#include <fstream>

using json = nlohmann::json;

// ---------------------------------------------------------------- modules --

namespace {

struct ModuleEntry { const char* name; uint32_t bit; };

// Order is display order in the Advanced panel.
const ModuleEntry kModules[] = {
    {"Core",            MODULE_CORE},
    {"GameState.Read",  MODULE_GAMESTATE_READ},
    {"Military.Read",   MODULE_MILITARY_READ},
    {"Military.Write",  MODULE_MILITARY_WRITE},
    {"Research.Read",   MODULE_RESEARCH_READ},
    {"Research.Write",  MODULE_RESEARCH_WRITE},
    {"Politics.Read",   MODULE_POLITICS_READ},
    {"Politics.Write",  MODULE_POLITICS_WRITE},
    {"Economy.Read",    MODULE_ECONOMY_READ},
    {"Economy.Write",   MODULE_ECONOMY_WRITE},
    {"MapEditor",       MODULE_MAPEDITOR},
    {"GameState.Write", MODULE_GAMESTATE_WRITE},
    {"GameProcess",     MODULE_GAMEPROCESS},
    {"Neural",          MODULE_NEURAL},
    {"UI",              MODULE_UI},
    {"Map",             MODULE_MAP},
    {"Diplomacy",       MODULE_DIPLOMACY},
    {"Assets",          MODULE_ASSETS},
    {"Storage",         MODULE_STORAGE},
    {"Audio",           MODULE_AUDIO},
    {"Net",             MODULE_NET},
    {"WasiStub",        MODULE_WASISTUB},
};

}  // namespace

// Every capability, in the order the permissions screen lists them.
//
// FROM kModules, NOT A SECOND ARRAY. The permissions screen used to carry its
// own hardcoded list, and it had already fallen behind: Audio and Net were
// grantable in a manifest but invisible in the UI, so a player could neither
// see them nor revoke them. Gearbox 1.1 would have added nine more of the same.
// A capability the player cannot see is not a capability, it is a permission
// granted by default, and the whole model rests on that not being true.
const std::vector<uint32_t>& modAllModuleBits() {
    static const std::vector<uint32_t> bits = [] {
        std::vector<uint32_t> v;
        for (const auto& m : kModules) v.push_back(m.bit);
        return v;
    }();
    return bits;
}

uint32_t modModuleFromName(const std::string& name) {
    for (const auto& m : kModules)
        if (name == m.name) return m.bit;
    return 0;
}

// ------------------------------------------------------------------ side --

const char* modSideName(ModSide s) {
    switch (s) {
        case ModSide::Client: return "client";
        case ModSide::Server: return "server";
        case ModSide::Both:   return "both";
    }
    return "both";
}

ModSide modSideFromName(const std::string& name, bool& known) {
    known = true;
    if (name == "client") return ModSide::Client;
    if (name == "server") return ModSide::Server;
    if (name == "both")   return ModSide::Both;
    known = false;
    return ModSide::Both;
}

uint32_t modSideGrantMask(ModSide side, bool multiplayer) {
    // Outside a multiplayer session `side` means nothing: there is one process
    // and it is authoritative, so a "client" mod is simply a mod.
    if (!multiplayer || side != ModSide::Client) return ~0u;

    // A client-side mod cannot write game state or run turn hooks while a
    // server is authoritative. Not because we distrust it -- because those
    // writes would be silently discarded the moment the next turn delta
    // arrived, and a capability that appears to work but does nothing is worse
    // than one that was never granted.
    //
    // Expressed as a mask over the existing grant word so there is exactly one
    // place in the codebase that decides what a mod may touch.
    return ~(MODULE_GAMESTATE_WRITE | MODULE_GAMEPROCESS);
}

std::string modModuleMaskToString(uint32_t mask) {
    std::string out;
    for (const auto& m : kModules) {
        if (!(mask & m.bit)) continue;
        if (!out.empty()) out += ", ";
        out += m.name;
    }
    return out;
}

const char* modLoadResultName(ModLoadResult r) {
    switch (r) {
        case ModLoadResult::Ok:                     return "Ok";
        case ModLoadResult::FileUnreadable:         return "FileUnreadable";
        case ModLoadResult::NotAnArchive:           return "NotAnArchive";
        case ModLoadResult::TooManyEntries:         return "TooManyEntries";
        case ModLoadResult::EntryNameUnsafe:        return "EntryNameUnsafe";
        case ModLoadResult::PathTooDeep:            return "PathTooDeep";
        case ModLoadResult::TotalSizeExceeded:      return "TotalSizeExceeded";
        case ModLoadResult::ArchiveRatioExceeded:   return "ArchiveRatioExceeded";
        case ModLoadResult::EntryRatioExceeded:     return "EntryRatioExceeded";
        case ModLoadResult::ManifestMissing:        return "ManifestMissing";
        case ModLoadResult::ManifestNotFirst:       return "ManifestNotFirst";
        case ModLoadResult::ManifestTooLarge:       return "ManifestTooLarge";
        case ModLoadResult::ManifestNotJson:        return "ManifestNotJson";
        case ModLoadResult::ManifestInvalid:        return "ManifestInvalid";
        case ModLoadResult::UnknownModule:          return "UnknownModule";
        case ModLoadResult::IncompatibleApiVersion: return "IncompatibleApiVersion";
        case ModLoadResult::WasmMissing:            return "WasmMissing";
        case ModLoadResult::WasmInvalid:            return "WasmInvalid";
        case ModLoadResult::ExtractFailed:          return "ExtractFailed";
    }
    return "Unknown";
}

// ------------------------------------------------------------ name safety --

namespace {

bool isValidUtf8(const char* s, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t extra;
        uint32_t cp;
        if (c < 0x80)              { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else return false;                          // continuation or 5+ byte

        if (i + extra >= len) return false;
        for (size_t k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Reject overlong encodings and surrogates: two spellings of the same
        // name would let an archive smuggle "../" past a byte comparison.
        if (extra == 1 && cp < 0x80)    return false;
        if (extra == 2 && cp < 0x800)   return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10FFFF)              return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += extra + 1;
    }
    return true;
}

// Returns nullptr when safe, else a reason fragment for the diagnostic.
const char* unsafeEntryName(const std::string& n) {
    if (n.empty())                          return "empty name";
    if (!isValidUtf8(n.data(), n.size()))   return "name is not valid UTF-8";
    if (n[0] == '/')                        return "absolute path";
    if (n.find('\\') != std::string::npos)  return "backslash in path";
    if (n.size() >= 2 && n[1] == ':')       return "drive-letter path";

    for (unsigned char c : n)
        if (c < 0x20 || c == 0x7F)          return "control character in name";

    size_t start = 0;
    while (start <= n.size()) {
        size_t slash = n.find('/', start);
        std::string comp = n.substr(start, slash == std::string::npos
                                               ? std::string::npos
                                               : slash - start);
        if (comp == "..")                   return "'..' path component";
        if (comp == ".")                    return "'.' path component";
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return nullptr;
}

uint32_t pathDepth(const std::string& n) {
    uint32_t d = 1;
    for (size_t i = 0; i + 1 < n.size(); i++)   // trailing '/' is a dir marker
        if (n[i] == '/') d++;
    return d;
}

bool isDirEntry(const std::string& n) {
    return !n.empty() && n.back() == '/';
}

bool startsWith(const std::string& s, const char* p) {
    size_t n = strlen(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}

}  // namespace

// ----------------------------------------------------------- manifest ------

namespace {

bool validModId(const std::string& id) {
    // Lowercase only: the id is the Storage namespace and the trust-pinning
    // key, and on macOS/Windows a case-insensitive filesystem would make
    // "com.a.Mod" and "com.a.mod" collide as two identities sharing one store.
    if (id.size() < 3 || id.size() > 128) return false;
    if (id.front() == '.' || id.back() == '.') return false;
    if (id.find("..") != std::string::npos) return false;
    if (id.find('.') == std::string::npos) return false;
    for (char c : id) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

bool validSemver(const std::string& v) {
    // MAJOR.MINOR.PATCH, optionally followed by -prerelease or +build.
    size_t end = v.find_first_of("-+");
    std::string core = v.substr(0, end);
    int parts = 0;
    size_t start = 0;
    while (true) {
        size_t dot = core.find('.', start);
        std::string p = core.substr(start, dot == std::string::npos
                                               ? std::string::npos
                                               : dot - start);
        if (p.empty() || p.size() > 9) return false;
        for (char c : p) if (c < '0' || c > '9') return false;
        parts++;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return parts == 3;
}

bool parseApiVersion(const std::string& s, int& major, int& minor) {
    size_t dot = s.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= s.size()) return false;
    for (size_t i = 0; i < s.size(); i++)
        if (i != dot && (s[i] < '0' || s[i] > '9')) return false;
    major = std::stoi(s.substr(0, dot));
    minor = std::stoi(s.substr(dot + 1));
    return true;
}

std::string jsonString(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

}  // namespace


ModLoadResult parseModManifest(const std::string& text,
                               ModManifest& out,
                               std::vector<std::string>& warnings,
                               std::string& diagnostic) {
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        diagnostic = "MANIFEST.json is not a JSON object";
        return ModLoadResult::ManifestNotJson;
    }

    auto bad = [&](const std::string& m) {
        diagnostic = "MANIFEST.json: " + m;
        return ModLoadResult::ManifestInvalid;
    };

    // schema
    auto sit = j.find("schema");
    if (sit == j.end() || !sit->is_number_integer())
        return bad("missing integer field \"schema\"");
    out.schema = sit->get<int>();
    if (out.schema != 1)
        return bad("unsupported schema " + std::to_string(out.schema) +
                   " (this build understands schema 1)");

    out.id = jsonString(j, "id");
    if (out.id.empty()) return bad("missing field \"id\"");
    if (!validModId(out.id))
        return bad("\"id\" must be lowercase reverse-DNS using [a-z0-9._-] and "
                   "contain a dot, e.g. \"com.example.my-mod\" (got \"" +
                   out.id + "\")");

    out.name = jsonString(j, "name");
    if (out.name.empty()) return bad("missing field \"name\"");
    if (out.name.size() > 96) return bad("\"name\" is longer than 96 bytes");

    out.version = jsonString(j, "version");
    if (out.version.empty()) return bad("missing field \"version\"");
    if (!validSemver(out.version))
        return bad("\"version\" must be semver MAJOR.MINOR.PATCH (got \"" +
                   out.version + "\")");

    out.description = jsonString(j, "description");
    if (out.description.size() > 1024) out.description.resize(1024);

    auto ait = j.find("authors");
    if (ait != j.end() && ait->is_array())
        for (const auto& a : *ait)
            if (a.is_string()) out.authors.push_back(a.get<std::string>());

    // gearbox API version
    std::string gb = jsonString(j, "gearbox");
    if (gb.empty()) return bad("missing field \"gearbox\" (e.g. \"1.0\")");
    if (!parseApiVersion(gb, out.gearboxMajor, out.gearboxMinor))
        return bad("\"gearbox\" must be MAJOR.MINOR (got \"" + gb + "\")");
    if (out.gearboxMajor != kHostGearboxMajor) {
        diagnostic = "targets Gearbox v" + gb + ", this build provides v" +
                     std::to_string(kHostGearboxMajor) + "." +
                     std::to_string(kHostGearboxMinor) +
                     " — different major versions are not compatible";
        return ModLoadResult::IncompatibleApiVersion;
    }
    if (out.gearboxMinor > kHostGearboxMinor) {
        // A WARNING, NOT A REFUSAL. The mod may well only use the older minor
        // and have declared the newer one out of habit, and refusing that would
        // strand working mods on every host that had not updated yet.
        //
        // If it does import something newer, ModRuntime::instantiate refuses it
        // there -- an unresolved import means the module cannot be linked at
        // all, so the failure is loud, at load, and names the missing symbol.
        // (This used to say "traps on first call", which was wrong: there is no
        // first call, because there is no instance.)
        warnings.push_back("targets Gearbox v" + gb + " but this build provides v" +
                           std::to_string(kHostGearboxMajor) + "." +
                           std::to_string(kHostGearboxMinor) +
                           "; newer APIs will be missing");
    }

    // modules
    auto mit = j.find("modules");
    if (mit == j.end() || !mit->is_array())
        return bad("missing array field \"modules\"");
    for (const auto& m : *mit) {
        if (!m.is_string()) return bad("\"modules\" must contain only strings");
        std::string mn = m.get<std::string>();
        uint32_t bit = modModuleFromName(mn);
        if (!bit) {
            // Hard error by design: silently dropping a capability the mod
            // believes it holds is worse than refusing to load.
            diagnostic = "MANIFEST.json requests unknown module \"" + mn +
                         "\". Known modules: " +
                         modModuleMaskToString(0xFFFFFFFFu);
            return ModLoadResult::UnknownModule;
        }
        out.modules |= bit;
    }
    out.modules |= MODULE_CORE;                    // always granted
    if (out.modules & MODULE_GAMESTATE_WRITE)
        out.modules |= MODULE_GAMESTATE_READ;      // Write implies Read
    // Same rule for every 1.1 domain: you cannot sensibly command what you
    // cannot see, and a mod asking only for Write would otherwise be unable to
    // check its own effect.
    if (out.modules & MODULE_MILITARY_WRITE)  out.modules |= MODULE_MILITARY_READ;
    if (out.modules & MODULE_RESEARCH_WRITE)  out.modules |= MODULE_RESEARCH_READ;
    if (out.modules & MODULE_POLITICS_WRITE)  out.modules |= MODULE_POLITICS_READ;
    if (out.modules & MODULE_ECONOMY_WRITE)   out.modules |= MODULE_ECONOMY_READ;
    if (out.modules & MODULE_MAPEDITOR)
        out.modules |= MODULE_GAMESTATE_READ | MODULE_MAPEDITOR;

    // side: optional, defaulting to "both". Not a schema bump -- an existing
    // mod that says nothing keeps working, and "both" is what it already was.
    //
    // An UNKNOWN value is a warning, not a rejection, and it falls back to
    // "both". A future release adding a fourth side should not stop today's
    // game loading a mod; treating it as "both" is the answer that cannot
    // silently drop a mod the server needed.
    auto sideIt = j.find("side");
    if (sideIt != j.end()) {
        if (!sideIt->is_string()) return bad("\"side\" must be a string");
        bool known = false;
        out.side = modSideFromName(sideIt->get<std::string>(), known);
        if (!known) {
            warnings.push_back("unknown \"side\" value \"" + sideIt->get<std::string>() +
                               "\", treated as \"both\"");
        }
    }

    // A client-side mod declaring capabilities it can never use in the mode it
    // named. Allowed -- it may be running singleplayer, where side means
    // nothing -- but worth saying out loud, because the usual cause is a mod
    // that should have declared "both".
    if (out.side == ModSide::Client &&
        (out.modules & (MODULE_GAMESTATE_WRITE | MODULE_GAMEPROCESS))) {
        warnings.push_back(
            "declares \"side\": \"client\" but asks for GameState.Write or "
            "GameProcess; those are masked off in multiplayer, where the "
            "server owns the world");
    }

    // dependencies -- resolved by ModManager at enable time, not here: this
    // layer sees one package and cannot know what else is installed.
    auto dit = j.find("dependencies");
    if (dit != j.end()) {
        if (!dit->is_array()) return bad("\"dependencies\" must be an array");
        for (const auto& d : *dit) {
            if (!d.is_object()) return bad("each dependency must be an object");
            ModDependency dep;
            dep.id = jsonString(d, "id");
            if (dep.id.empty()) return bad("dependency is missing \"id\"");
            if (!validModId(dep.id))
                return bad("dependency id \"" + dep.id + "\" is not a valid mod id");
            dep.version = jsonString(d, "version");
            auto oit = d.find("optional");
            if (oit != d.end() && oit->is_boolean()) dep.optional = oit->get<bool>();
            out.dependencies.push_back(dep);
        }
    }

    // conflicts: the ones the author already knows about. A reason is required
    // -- "conflicts with X" that does not say why leaves the user with no way
    // to decide whether to override it.
    auto cit = j.find("conflicts");
    if (cit != j.end()) {
        if (!cit->is_array()) return bad("\"conflicts\" must be an array");
        for (const auto& c : *cit) {
            if (!c.is_object()) return bad("each conflict must be an object");
            ModConflict cf;
            cf.id = jsonString(c, "id");
            if (cf.id.empty()) return bad("conflict is missing \"id\"");
            if (!validModId(cf.id))
                return bad("conflict id \"" + cf.id + "\" is not a valid mod id");
            if (cf.id == out.id) return bad("a mod cannot conflict with itself");
            cf.reason = jsonString(c, "reason");
            if (cf.reason.empty())
                return bad("conflict with \"" + cf.id + "\" needs a \"reason\"");
            out.conflicts.push_back(cf);
        }
    }

    // bridges: this mod exists (in part) to reconcile two others.
    auto bit = j.find("bridges");
    if (bit != j.end()) {
        if (!bit->is_array()) return bad("\"bridges\" must be an array");
        for (const auto& b : *bit) {
            if (!b.is_object()) return bad("each bridge must be an object");
            auto pit = b.find("between");
            if (pit == b.end() || !pit->is_array() || pit->size() != 2)
                return bad("a bridge needs \"between\": [modA, modB]");
            ModBridge br;
            br.a = (*pit)[0].is_string() ? (*pit)[0].get<std::string>() : "";
            br.b = (*pit)[1].is_string() ? (*pit)[1].get<std::string>() : "";
            if (!validModId(br.a) || !validModId(br.b))
                return bad("bridge \"between\" entries must be valid mod ids");
            if (br.a == br.b) return bad("a bridge must name two different mods");
            br.reason = jsonString(b, "reason");
            out.bridges.push_back(br);
        }
    }

    // updateUrl: only ever shown to the user. The game never fetches a mod by
    // itself, so this is a link, not a download instruction -- and it must be
    // https, because a plaintext update pointer is worse than none.
    out.updateUrl = jsonString(j, "updateUrl");
    if (!out.updateUrl.empty() && !startsWith(out.updateUrl, "https://"))
        return bad("\"updateUrl\" must be an https:// URL");

    out.publicKey = jsonString(j, "publicKey");
    if (!out.publicKey.empty() && !startsWith(out.publicKey, "ed25519:"))
        return bad("\"publicKey\" must be prefixed \"ed25519:\"");

    // limits: clamped, not rejected — a mod asking for more than we allow is
    // being optimistic, not hostile, and should still get to run.
    auto lit = j.find("limits");
    if (lit != j.end() && lit->is_object()) {
        auto mp = lit->find("memoryPages");
        if (mp != lit->end() && mp->is_number_unsigned())
            out.limits.memoryPages = mp->get<uint32_t>();
        auto fp = lit->find("fuelPerTurn");
        if (fp != lit->end() && fp->is_number_unsigned())
            out.limits.fuelPerTurn = fp->get<uint64_t>();
        auto lf = lit->find("loadFuel");
        if (lf != lit->end() && lf->is_number_unsigned())
            out.limits.loadFuel = lf->get<uint64_t>();
    }
    if (out.limits.memoryPages == 0) out.limits.memoryPages = 1;
    if (out.limits.memoryPages > ModHostCaps::kMaxMemoryPages) {
        warnings.push_back("requested " + std::to_string(out.limits.memoryPages) +
                           " memory pages, clamped to " +
                           std::to_string(ModHostCaps::kMaxMemoryPages));
        out.limits.memoryPages = ModHostCaps::kMaxMemoryPages;
    }
    if (out.limits.fuelPerTurn > ModHostCaps::kMaxFuelPerTurn) {
        warnings.push_back("requested " + std::to_string(out.limits.fuelPerTurn) +
                           " fuel/turn, clamped to " +
                           std::to_string(ModHostCaps::kMaxFuelPerTurn));
        out.limits.fuelPerTurn = ModHostCaps::kMaxFuelPerTurn;
    }
    // 0 is not "no budget", it is "unstated" -- the host picks. Starting an
    // interpreter costs far more than any single turn, and a mod author should
    // not have to know what their SDK costs to start.
    if (out.limits.loadFuel == 0) {
        out.limits.loadFuel = ModHostCaps::kDefaultLoadFuel;
    } else if (out.limits.loadFuel > ModHostCaps::kMaxLoadFuel) {
        warnings.push_back("requested " + std::to_string(out.limits.loadFuel) +
                           " load fuel, clamped to " +
                           std::to_string(ModHostCaps::kMaxLoadFuel));
        out.limits.loadFuel = ModHostCaps::kMaxLoadFuel;
    }
    // A load budget below the per-turn budget is almost certainly a mistake,
    // and silently honouring it would make a mod fail at load for no visible
    // reason. Raise it and say so.
    if (out.limits.loadFuel < out.limits.fuelPerTurn) {
        warnings.push_back("loadFuel (" + std::to_string(out.limits.loadFuel) +
                           ") is below fuelPerTurn (" +
                           std::to_string(out.limits.fuelPerTurn) +
                           "); raised to match");
        out.limits.loadFuel = out.limits.fuelPerTurn;
    }

    return ModLoadResult::Ok;
}

// -------------------------------------------------------------- package ----

ModLoadResult ModPackage::fail(ModLoadResult r, const std::string& msg) {
    m_diagnostic = msg;
    // A rejected package must be inert, not half-populated: the scan pass fills
    // in the asset list before the manifest is even parsed, and a caller that
    // ignored the return value could otherwise read entries out of an archive
    // we just refused. Warnings survive because they explain the rejection.
    m_archive.clear();
    m_archive.shrink_to_fit();
    m_wasm.clear();
    m_thumbnail.clear();
    m_assetNames.clear();
    m_assetIndices.clear();
    return r;
}

ModLoadResult ModPackage::open(const std::string& path) {
    m_path = path;
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return fail(ModLoadResult::FileUnreadable, "cannot open " + path);

    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    if (sz <= 0)
        return fail(ModLoadResult::FileUnreadable, path + " is empty");
    // A .odmod larger than the decompressed cap cannot possibly be within it.
    if ((uint64_t)sz > ModArchiveLimits::kMaxTotalUncompressed)
        return fail(ModLoadResult::TotalSizeExceeded,
                    "archive is " + std::to_string((uint64_t)sz / (1024 * 1024)) +
                    " MiB, over the " +
                    std::to_string(ModArchiveLimits::kMaxTotalUncompressed /
                                   (1024 * 1024)) + " MiB limit");
    f.seekg(0, std::ios::beg);

    std::vector<uint8_t> bytes((size_t)sz);
    if (!f.read((char*)bytes.data(), sz))
        return fail(ModLoadResult::FileUnreadable, "short read on " + path);

    return openFromMemory(std::move(bytes), path);
}

ModLoadResult ModPackage::openFromMemory(std::vector<uint8_t> bytes,
                                         const std::string& label) {
    m_path = label;
    m_warnings.clear();
    m_assetNames.clear();
    m_assetIndices.clear();
    m_wasm.clear();
    m_thumbnail.clear();
    m_sha256.clear();
    m_manifest = ModManifest{};
    m_archive = std::move(bytes);

    // Digest the archive before anything reads it, so what we report is what
    // arrived rather than what we made of it.
    m_sha256 = ::sha256Hex(m_archive.data(), m_archive.size());

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, m_archive.data(), m_archive.size(), 0))
        return fail(ModLoadResult::NotAnArchive,
                    label + " is not a readable ZIP archive");

    struct ZipCloser {
        mz_zip_archive* z;
        ~ZipCloser() { mz_zip_reader_end(z); }
    } closer{&zip};

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    if (numFiles == 0)
        return fail(ModLoadResult::ManifestMissing, "archive is empty");
    if (numFiles > ModArchiveLimits::kMaxEntries)
        return fail(ModLoadResult::TooManyEntries,
                    "archive has " + std::to_string(numFiles) + " entries, over "
                    "the limit of " +
                    std::to_string(ModArchiveLimits::kMaxEntries));

    // Pass one: the central directory only. Nothing is inflated here, so a
    // bomb is rejected on its declared sizes before it costs us any memory.
    //
    // The declared sizes are attacker-controlled, which is fine: they are the
    // *upper* bound we later allocate against. If a stream inflates to more
    // than its entry declared, miniz fails the extraction rather than growing
    // the buffer; if it declares more than it produces, we already rejected it
    // here. Either way we never inflate more than we screened.
    uint64_t totalUncomp = 0, totalComp = 0;
    int manifestIdx = -1, wasmIdx = -1, thumbIdx = -1;
    int firstFileIdx = -1;
    std::string firstFileName;

    for (mz_uint i = 0; i < numFiles; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st))
            return fail(ModLoadResult::NotAnArchive,
                        "corrupt central directory at entry " + std::to_string(i));

        std::string name = st.m_filename;
        if (const char* why = unsafeEntryName(name))
            return fail(ModLoadResult::EntryNameUnsafe,
                        std::string("rejected entry \"") + name + "\": " + why);
        if (pathDepth(name) > ModArchiveLimits::kMaxPathDepth)
            return fail(ModLoadResult::PathTooDeep,
                        "entry \"" + name + "\" nests deeper than " +
                        std::to_string(ModArchiveLimits::kMaxPathDepth) +
                        " directories");

        if (isDirEntry(name) || st.m_is_directory) continue;

        totalUncomp += st.m_uncomp_size;
        totalComp   += st.m_comp_size;

        if (totalUncomp > ModArchiveLimits::kMaxTotalUncompressed)
            return fail(ModLoadResult::TotalSizeExceeded,
                        "archive expands to more than " +
                        std::to_string(ModArchiveLimits::kMaxTotalUncompressed /
                                       (1024 * 1024)) +
                        " MiB (reached at \"" + name + "\")");

        if (st.m_comp_size > 0 &&
            st.m_uncomp_size / st.m_comp_size > ModArchiveLimits::kMaxEntryRatio)
            return fail(ModLoadResult::EntryRatioExceeded,
                        "entry \"" + name + "\" expands " +
                        std::to_string(st.m_uncomp_size / st.m_comp_size) +
                        ":1, over the " +
                        std::to_string(ModArchiveLimits::kMaxEntryRatio) +
                        ":1 per-entry limit");

        if (firstFileIdx < 0) { firstFileIdx = (int)i; firstFileName = name; }

        if (name == "MANIFEST.json")      manifestIdx = (int)i;
        else if (name == "mod.wasm")      wasmIdx = (int)i;
        else if (name == "thumbnail.png") thumbIdx = (int)i;
        else if (startsWith(name, "data/")) {
            m_assetNames.push_back(name.substr(5));
            m_assetIndices.push_back(i);
        } else if (name != "signature.bin") {
            m_warnings.push_back("ignoring unrecognised entry \"" + name + "\"");
        }
    }

    if (totalComp > 0 &&
        totalUncomp / totalComp > ModArchiveLimits::kMaxArchiveRatio)
        return fail(ModLoadResult::ArchiveRatioExceeded,
                    "archive expands " + std::to_string(totalUncomp / totalComp) +
                    ":1 overall, over the " +
                    std::to_string(ModArchiveLimits::kMaxArchiveRatio) +
                    ":1 limit");

    if (manifestIdx < 0)
        return fail(ModLoadResult::ManifestMissing,
                    "archive has no MANIFEST.json");
    if (manifestIdx != firstFileIdx)
        return fail(ModLoadResult::ManifestNotFirst,
                    "MANIFEST.json must be the first entry in the archive so it "
                    "can be validated before anything else is decompressed; "
                    "found \"" + firstFileName + "\" first");

    mz_zip_archive_file_stat mst;
    mz_zip_reader_file_stat(&zip, (mz_uint)manifestIdx, &mst);
    if (mst.m_uncomp_size > ModArchiveLimits::kMaxManifestBytes)
        return fail(ModLoadResult::ManifestTooLarge,
                    "MANIFEST.json is " + std::to_string(mst.m_uncomp_size) +
                    " bytes, over the " +
                    std::to_string(ModArchiveLimits::kMaxManifestBytes) +
                    " byte limit");

    size_t msz = 0;
    void* mdata = mz_zip_reader_extract_to_heap(&zip, (mz_uint)manifestIdx, &msz, 0);
    if (!mdata)
        return fail(ModLoadResult::ExtractFailed,
                    "MANIFEST.json failed to decompress (corrupt or bad CRC)");
    std::string manifestText((char*)mdata, msz);
    mz_free(mdata);

    std::string diag;
    ModLoadResult r = parseModManifest(manifestText, m_manifest, m_warnings, diag);
    if (r != ModLoadResult::Ok) return fail(r, diag);

    // mod.wasm
    if (wasmIdx < 0)
        return fail(ModLoadResult::WasmMissing, "archive has no mod.wasm");
    mz_zip_archive_file_stat wst;
    mz_zip_reader_file_stat(&zip, (mz_uint)wasmIdx, &wst);
    if (wst.m_uncomp_size > ModArchiveLimits::kMaxWasmBytes)
        return fail(ModLoadResult::WasmInvalid,
                    "mod.wasm is " + std::to_string(wst.m_uncomp_size / (1024 * 1024)) +
                    " MiB, over the " +
                    std::to_string(ModArchiveLimits::kMaxWasmBytes / (1024 * 1024)) +
                    " MiB limit");

    size_t wsz = 0;
    void* wdata = mz_zip_reader_extract_to_heap(&zip, (mz_uint)wasmIdx, &wsz, 0);
    if (!wdata)
        return fail(ModLoadResult::ExtractFailed,
                    "mod.wasm failed to decompress (corrupt or bad CRC)");
    m_wasm.assign((uint8_t*)wdata, (uint8_t*)wdata + wsz);
    mz_free(wdata);

    static const uint8_t kWasmMagic[8] = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
    if (m_wasm.size() < 8 || memcmp(m_wasm.data(), kWasmMagic, 8) != 0) {
        m_wasm.clear();
        return fail(ModLoadResult::WasmInvalid,
                    "mod.wasm is not a WebAssembly binary (bad magic; expected "
                    "\\0asm followed by version 1)");
    }

    // thumbnail: optional, never fatal.
    if (thumbIdx >= 0) {
        size_t tsz = 0;
        void* tdata = mz_zip_reader_extract_to_heap(&zip, (mz_uint)thumbIdx, &tsz, 0);
        if (!tdata) {
            m_warnings.push_back("thumbnail.png failed to decompress, ignoring");
        } else {
            const uint8_t* tp = (const uint8_t*)tdata;
            static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            bool ok = tsz >= 24 && memcmp(tp, kPng, 8) == 0 &&
                      memcmp(tp + 12, "IHDR", 4) == 0;
            if (ok) {
                uint32_t w = ((uint32_t)tp[16] << 24) | ((uint32_t)tp[17] << 16) |
                             ((uint32_t)tp[18] << 8) | tp[19];
                uint32_t h = ((uint32_t)tp[20] << 24) | ((uint32_t)tp[21] << 16) |
                             ((uint32_t)tp[22] << 8) | tp[23];
                if (w == 0 || h == 0 || w > 512 || h > 512) {
                    m_warnings.push_back("thumbnail.png is " + std::to_string(w) +
                                         "x" + std::to_string(h) +
                                         ", over 512x512; ignoring");
                    ok = false;
                }
            } else {
                m_warnings.push_back("thumbnail.png is not a PNG, ignoring");
            }
            if (ok) m_thumbnail.assign(tp, tp + tsz);
            mz_free(tdata);
        }
    }

    // Assets are not inflated. A mod shipping 200 MiB of art costs us its
    // zipped size until something actually asks for a file.
    // Audio reads from data/ too -- playing your own sound does not also
    // require the broader "read every file I ship" capability -- so a mod with
    // Audio and no Assets is shipping sounds, not making a mistake.
    if (!m_assetNames.empty() &&
        !(m_manifest.modules & (MODULE_ASSETS | MODULE_AUDIO)))
        m_warnings.push_back("archive contains " +
                             std::to_string(m_assetNames.size()) +
                             " data/ file(s) but does not request the Assets "
                             "module, so the mod cannot read them");

    m_diagnostic.clear();
    return ModLoadResult::Ok;
}

bool ModPackage::readAsset(const std::string& name,
                           std::vector<uint8_t>& out) const {
    int idx = -1;
    for (size_t i = 0; i < m_assetNames.size(); i++)
        if (m_assetNames[i] == name) { idx = (int)m_assetIndices[i]; break; }
    if (idx < 0 || m_archive.empty()) return false;

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, m_archive.data(), m_archive.size(), 0))
        return false;
    size_t sz = 0;
    void* d = mz_zip_reader_extract_to_heap(&zip, (mz_uint)idx, &sz, 0);
    mz_zip_reader_end(&zip);
    if (!d) return false;

    out.assign((uint8_t*)d, (uint8_t*)d + sz);
    mz_free(d);
    return true;
}
