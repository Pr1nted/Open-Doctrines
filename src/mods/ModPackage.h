#pragma once

// Reader for .odmod mod containers. See docs/modding.md for the format and
// docs/gearbox-sdk.md for the modder-facing side.
//
// This layer does exactly two things: it opens an untrusted ZIP without letting
// it hurt us, and it turns MANIFEST.json into a validated ModManifest. It does
// not instantiate anything, touch the filesystem, or know what WASM is beyond
// checking the magic number. Everything it rejects, it rejects with a reason a
// user can act on.

#include <cstdint>
#include <string>
#include <vector>

// Capability modules. Names match MANIFEST.json "modules" entries and the
// "gearbox:<module>" import namespaces in sdk/gearbox.h. A bitmask so a grant
// set is one word and revocation in the Advanced panel is a mask-off.
enum ModModuleBit : uint32_t {
    MODULE_CORE            = 1u << 0,
    MODULE_GAMESTATE_READ  = 1u << 1,
    MODULE_GAMESTATE_WRITE = 1u << 2,
    MODULE_GAMEPROCESS     = 1u << 3,
    MODULE_NEURAL          = 1u << 4,
    MODULE_UI              = 1u << 5,
    MODULE_MAP             = 1u << 6,
    MODULE_DIPLOMACY       = 1u << 7,
    MODULE_ASSETS          = 1u << 8,
    MODULE_STORAGE         = 1u << 9,

    // Not a Gearbox namespace: these are the real WASI import names, provided
    // so an interpreter-in-a-mod (Python, Ruby, Lua, Java) can boot. Deliberately
    // the narrowest possible surface, off by default, and never a real WASI --
    // see the WasiStub notes in ModHost.cpp and docs/modding.md.
    MODULE_WASISTUB        = 1u << 10,
};

uint32_t    modModuleFromName(const std::string& name);  // 0 when unknown
std::string modModuleMaskToString(uint32_t mask);

// Which side of a multiplayer game a mod belongs on. "both" is the default
// because it is what a mod written before multiplayer existed effectively is,
// and because it is the conservative answer: a mod the server does not need is
// merely wasteful, whereas one it needed and did not have is a desync.
//
// The distinction is enforced, not advisory:
//
//   Client  In a multiplayer session the mod's GameState.Write and GameProcess
//           grants are MASKED OFF, whatever the user granted. It cannot change
//           the world -- which is also why the server does not care whether a
//           joining player has it. This is the category almost every UI, map
//           overlay and quality-of-life mod belongs in.
//   Server  Never instantiated on a client at all. Clients do not need the
//           file and are never asked for it.
//   Both    Must be present on both ends, at the same version and the same
//           bytes. Even here the server instance is authoritative and the
//           client instance is presentation: the server never asks a client to
//           compute game state.
enum class ModSide : uint8_t {
    Both   = 0,
    Client = 1,
    Server = 2,
};

const char* modSideName(ModSide s);
ModSide     modSideFromName(const std::string& name, bool& known);

// Capabilities a client-side mod cannot hold while a multiplayer session is
// running. Applied as a mask, reusing the existing revocable-grant mechanism
// rather than adding a second notion of what a mod may do.
uint32_t    modSideGrantMask(ModSide side, bool multiplayer);

struct ModDependency {
    std::string id;
    std::string version;    // range expression, e.g. ">=2.0.0 <3.0.0"
    bool optional = false;
};

// A conflict the author already knows about. Most conflicts are NOT declared --
// nobody can enumerate every mod they will ever be incompatible with -- so this
// is a supplement to detection, not a substitute for it.
struct ModConflict {
    std::string id;         // the mod this one cannot run alongside
    std::string reason;     // shown to the user; required, because "conflicts"
                            // with no reason is not actionable
};

// A bridge mod: one that exists to make two otherwise-conflicting mods work
// together. While it is enabled, the conflict between those two is suppressed.
// This is the supported answer to "these two nearly work together" -- a third
// mod reconciles them, rather than either being forced to change.
struct ModBridge {
    std::string a, b;       // the two mod ids it reconciles
    std::string reason;
};

struct ModLimits {
    uint32_t memoryPages = 64;         // 64 KiB pages
    uint64_t fuelPerTurn = 1000000;

    // Budget for mod_load (and _initialize), which is a different kind of cost
    // from a per-turn hook: it happens once, when the user enables the mod, and
    // for an interpreter SDK it is dominated by starting the interpreter.
    // Ruby needs ~87M instructions before it runs a line of script and CPython
    // ~130M, against a fuelPerTurn ceiling of 100M -- so charging load against
    // the per-turn budget would refuse those outright, or force every mod to
    // declare a per-turn budget far larger than any turn actually needs.
    //
    // 0 means "let the host decide", which is the normal case: see
    // ModHostCaps::kDefaultLoadFuel. A mod only sets this if it wants LESS.
    uint64_t loadFuel = 0;
};

struct ModManifest {
    int schema = 0;
    std::string id;                    // reverse-DNS, immutable, the pinning key
    std::string name;
    std::string version;               // semver
    std::string description;
    std::vector<std::string> authors;
    int gearboxMajor = 0;
    int gearboxMinor = 0;
    uint32_t modules = 0;              // always includes MODULE_CORE
    ModSide  side = ModSide::Both;     // MANIFEST "side", default "both"
    std::vector<ModDependency> dependencies;
    std::vector<ModConflict>   conflicts;
    std::vector<ModBridge>     bridges;
    // Where a newer version can be found. Optional, and only ever used to show
    // the user that an update exists -- the game never downloads a mod by
    // itself. Must be https.
    std::string updateUrl;
    std::string publicKey;             // "ed25519:BASE64..."; reserved, unverified
    ModLimits limits;
};

// One per rejection cause. The UI shows the diagnostic string, not this; the
// enum exists so callers can react differently (e.g. offer "load anyway" for
// nothing, but distinguish a corrupt download from a hostile archive).
enum class ModLoadResult {
    Ok = 0,
    FileUnreadable,
    NotAnArchive,
    TooManyEntries,
    EntryNameUnsafe,
    PathTooDeep,
    TotalSizeExceeded,
    ArchiveRatioExceeded,
    EntryRatioExceeded,
    ManifestMissing,
    ManifestNotFirst,
    ManifestTooLarge,
    ManifestNotJson,
    ManifestInvalid,
    UnknownModule,
    IncompatibleApiVersion,
    WasmMissing,
    WasmInvalid,
    ExtractFailed,
};

const char* modLoadResultName(ModLoadResult r);

// Archive limits from docs/modding.md. Public so the tests and the docs can be
// checked against one definition.
struct ModArchiveLimits {
    static constexpr uint64_t kMaxTotalUncompressed = 256ull * 1024 * 1024;
    static constexpr uint64_t kMaxArchiveRatio      = 100;   // whole archive
    static constexpr uint64_t kMaxEntryRatio        = 200;   // single entry
    static constexpr uint32_t kMaxEntries           = 4096;
    static constexpr uint32_t kMaxPathDepth         = 16;
    static constexpr uint64_t kMaxManifestBytes     = 256 * 1024;
    static constexpr uint64_t kMaxWasmBytes         = 64ull * 1024 * 1024;
};

// Host ceilings for the mod-declared limits block. Declared values above these
// are clamped, not rejected: a mod asking for more memory than we allow is
// being optimistic, not hostile, and should still run.
struct ModHostCaps {
    static constexpr uint32_t kMaxMemoryPages = 1024;         // 64 MiB
    static constexpr uint64_t kMaxFuelPerTurn = 100000000ull;

    // What mod_load gets when the manifest does not ask for something smaller.
    // Every mod gets this; nothing has to declare that it is interpreted, which
    // is the point -- a mod author should not have to know what their SDK costs
    // to start.
    //
    // The trade this makes: a mod that spins in mod_load now stalls for up to
    // this many instructions (order of seconds) instead of the per-turn budget.
    // That is bounded, happens once, and happens at the moment the user clicked
    // "enable" -- whereas the per-turn budget, which is what stops a mod hanging
    // the game turn after turn, is untouched.
    static constexpr uint64_t kDefaultLoadFuel = 500000000ull;

    // WAMR takes the instruction limit as a signed int, so this cannot exceed
    // INT32_MAX; ModRuntime treats anything at or above that as "unmetered".
    static constexpr uint64_t kMaxLoadFuel = 2000000000ull;
};

class ModPackage {
public:
    // Opens and validates `path`. On failure returns the reason and leaves the
    // package unusable; diagnostic() explains it in one sentence.
    ModLoadResult open(const std::string& path);

    // Same, for bytes already in memory. `label` is only used in diagnostics.
    ModLoadResult openFromMemory(std::vector<uint8_t> bytes,
                                 const std::string& label);

    const ModManifest& manifest() const { return m_manifest; }
    const std::vector<uint8_t>& wasm() const { return m_wasm; }

    // SHA-256 of the whole .odmod, lowercase hex. Computed once at open().
    //
    // This is an INTEGRITY check, not an anti-tamper one, and the UI must say
    // so. A modified client on hardware its owner controls can report any
    // digest it likes; nothing in an open-source game can stop that. What this
    // catches is the common case: a wrong version, a half-finished download, a
    // mod someone edited and forgot to rebuild.
    //
    // Cheating is prevented elsewhere and by a different mechanism -- the
    // server is authoritative, so a client that lies gains nothing but its own
    // desync. See src/net/ModAttest.h.
    const std::string& sha256() const { return m_sha256; }
    const std::vector<uint8_t>& thumbnail() const { return m_thumbnail; }
    const std::string& path() const { return m_path; }
    const std::string& diagnostic() const { return m_diagnostic; }

    // Non-fatal notes: clamped limits, a newer-minor API target, an ignored
    // top-level file. The mod menu should surface these.
    const std::vector<std::string>& warnings() const { return m_warnings; }

    // data/ asset names, in archive order, without the "data/" prefix.
    const std::vector<std::string>& assetNames() const { return m_assetNames; }

    // Inflates one data/ asset on demand. Assets stay compressed in memory
    // until asked for, so a mod shipping 200 MiB of art costs us its zipped
    // size, not its unpacked size. Returns false if the name is not an asset.
    bool readAsset(const std::string& name, std::vector<uint8_t>& out) const;

private:
    ModLoadResult fail(ModLoadResult r, const std::string& msg);

    std::string  m_path;
    std::string  m_diagnostic;
    std::string  m_sha256;
    ModManifest  m_manifest;

    std::vector<uint8_t> m_archive;    // kept: assets are inflated on demand
    std::vector<uint8_t> m_wasm;
    std::vector<uint8_t> m_thumbnail;

    std::vector<std::string> m_warnings;
    std::vector<std::string> m_assetNames;
    std::vector<uint32_t>    m_assetIndices;   // parallel to m_assetNames
};

// Parses and validates a MANIFEST.json body on its own. Exposed for tests and
// for showing a mod's details without holding the archive open.
ModLoadResult parseModManifest(const std::string& json,
                               ModManifest& out,
                               std::vector<std::string>& warnings,
                               std::string& diagnostic);
