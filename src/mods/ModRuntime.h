#pragma once

// The WASM runtime behind the Gearbox mod system.
//
// Two backends, chosen at build time: WAMR on desktop, the browser's own
// WebAssembly engine on web. Neither is required -- a build with OD_ENABLE_MODS
// off still compiles, and reports mods as failed with a diagnostic instead.
//
// Everything a mod can reach arrives through modHostFunctions(). If a capability
// is not in that table, or the user did not grant it, the mod does not
// instantiate. There is no ambient authority to take away.

#include "ModPackage.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ModInstance;

// A host function exposed to mods.
//
// `signature` is WAMR's notation: i = i32, I = i64, f = f32, F = f64, with the
// return type after ')'. Every pointer argument is passed as a plain i32 linear
// memory offset that the host validates itself, rather than using WAMR's '*~'
// pointer sugar -- the two backends then see identical argument lists, and the
// bounds check lives in our code where it can be read and tested.
struct ModHostFn {
    const char* module;      // import module, e.g. "gearbox:core"
    const char* name;        // import name, e.g. "log"
    const char* signature;
    void*       fn;
    uint32_t    capability;  // ModModuleBit required to import this
};

const ModHostFn* modHostFunctions(size_t& count);

// Looks up a host function by import module/name. Returns nullptr if we do not
// provide it at all (as opposed to providing it but not granting it).
const ModHostFn* modFindHostFunction(const char* module, const char* name);

class ModRuntime {
public:
    static ModRuntime& get();

    // False when built without a backend. Not an error: the mod menu explains
    // it and every mod shows as Failed rather than the game refusing to start.
    bool available() const;
    const char* backendName() const;

    // Idempotent. Returns false with a reason if the engine cannot start.
    bool init(std::string& err);
    void shutdown();

    // Instantiates `pkg` with `granted` capabilities. Returns nullptr and sets
    // `err` on any failure, including a mod importing something it was not
    // granted -- that is refused here, at load, not at the call.
    std::unique_ptr<ModInstance> instantiate(const ModPackage& pkg,
                                             uint32_t granted,
                                             std::string& err);

private:
    ModRuntime() = default;
    bool m_inited = false;
};

class ModInstance {
public:
    ~ModInstance();
    ModInstance(const ModInstance&) = delete;
    ModInstance& operator=(const ModInstance&) = delete;

    const ModManifest& manifest() const { return m_manifest; }
    uint32_t granted() const { return m_granted; }
    bool has(uint32_t capability) const { return (m_granted & capability) != 0; }
    const std::string& id() const { return m_manifest.id; }

    // --- linear memory, always bounds-checked ---------------------------------
    //
    // Copying rather than handing out a pointer. That is not just caution: on
    // the web backend a mod's memory is a separate ArrayBuffer that the host
    // cannot address at all, so a pointer-returning API could never work there.
    // Both return false when [offset, offset+size) leaves the mod's memory.
    bool memRead(uint32_t offset, uint32_t size, void* dst) const;
    bool memWrite(uint32_t offset, uint32_t size, const void* src);
    bool readString(uint32_t ptr, uint32_t len, std::string& out) const;

    // --- exports --------------------------------------------------------------
    bool hasExport(const char* name) const;
    // `ret` may be null for void exports. Returns false and fills `err` if the
    // export traps, runs out of fuel, or does not exist.
    bool callExport(const char* name, const uint32_t* args, uint32_t argc,
                    uint32_t* ret, std::string& err);

    // --- fuel -----------------------------------------------------------------
    // Called by the host before entering a hook. `fuel` of 0 means unmetered.
    void resetFuel(uint64_t fuel);
    uint64_t fuelRemaining() const;

    // Set when a call trapped; the mod should be disabled and this shown.
    const std::string& lastError() const { return m_lastError; }

    // Raises a trap inside the mod, unwinding out of the current call. Used by
    // gearbox_abort, which the ABI declares noreturn.
    void setAbort(const std::string& msg);

    // Panels this instance registered, in registration order.
    std::vector<uint32_t>& panels() { return m_panels; }

    // WasiStub's random_get is a deterministic stream, so it needs a position
    // that advances rather than a fresh seed each call.
    uint64_t wasiRandomCounter() const { return m_wasiRandom; }
    void bumpWasiRandom(uint64_t n) { m_wasiRandom += n; }

    // Backing package, for Assets reads.
    const ModPackage* package() const { return m_package; }

private:
    friend class ModRuntime;
    ModInstance() = default;

    ModManifest  m_manifest;
    uint32_t     m_granted = 0;
    const ModPackage* m_package = nullptr;
    std::string  m_lastError;
    std::vector<uint32_t> m_panels;

    uint64_t m_wasiRandom = 0;
    uint64_t m_fuelBudget = 0;
    // Applied to mod_load and _initialize instead of m_fuelBudget: starting an
    // interpreter costs far more than any single turn, and it happens once.
    // See ModLimits::loadFuel.
    uint64_t m_loadFuelBudget = 0;
    // True only while a load hook is on the stack, so gearbox_fuel_budget can
    // answer for the hook that is actually running. sdk/gearbox.h promises "the
    // budget for the current hook"; with two budgets that needs saying which.
    bool     m_inLoadHook = false;
    void*    m_impl = nullptr;   // backend state
};

// Resolves the ModInstance that is currently executing, for use inside host
// natives. Valid only for the duration of a call into a mod.
ModInstance* modCurrentInstance(void* execEnv);
