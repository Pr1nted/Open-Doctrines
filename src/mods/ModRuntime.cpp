#include "ModRuntime.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#ifdef OD_MODS_WAMR
#include "wasm_export.h"
#include "lib_export.h"
#endif

// ---------------------------------------------------------------- helpers --

const ModHostFn* modFindHostFunction(const char* module, const char* name) {
    size_t n = 0;
    const ModHostFn* t = modHostFunctions(n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(t[i].module, module) == 0 && strcmp(t[i].name, name) == 0)
            return &t[i];
    return nullptr;
}

ModRuntime& ModRuntime::get() {
    static ModRuntime r;
    return r;
}

// ============================================================== WAMR ========
#ifdef OD_MODS_WAMR

namespace {

struct WamrInstance {
    wasm_module_t      module = nullptr;
    wasm_module_inst_t inst = nullptr;
    wasm_exec_env_t    env = nullptr;
    std::vector<uint8_t> wasmCopy;   // WAMR keeps a pointer into the buffer
};

// WAMR's native registration is global and keyed by import module name, so it
// cannot express per-instance grants. Capability enforcement therefore happens
// at instantiation: every import a module declares is checked against the
// granted mask before it runs, and an ungranted import refuses the load. The
// effect is what the docs promise -- a mod never reaches a capability it was
// not given -- and the diagnostic can name the missing module, which a bare
// link error could not.
bool registerNatives(std::string& err) {
    size_t n = 0;
    const ModHostFn* t = modHostFunctions(n);

    std::map<std::string, std::vector<NativeSymbol>> byModule;
    for (size_t i = 0; i < n; i++) {
        NativeSymbol s{};
        s.symbol = t[i].name;
        s.func_ptr = t[i].fn;
        s.signature = t[i].signature;
        s.attachment = nullptr;
        byModule[t[i].module].push_back(s);
    }

    // WAMR keeps the pointers we hand it -- both the module name and the symbol
    // array -- and never copies them. Everything passed here must therefore
    // outlive the runtime, so it lives in function-static storage. Passing the
    // key of a local map instead leaves WAMR holding a dangling name, and the
    // symptom is a baffling "failed to link import function" at instantiate
    // time rather than a failure here.
    static std::vector<std::string> names;
    static std::vector<std::vector<NativeSymbol>> tables;
    names.clear();
    tables.clear();
    names.reserve(byModule.size());     // no reallocation, so c_str() stays put
    tables.reserve(byModule.size());

    for (auto& kv : byModule) {
        names.push_back(kv.first);
        tables.push_back(kv.second);
        if (!wasm_runtime_register_natives(names.back().c_str(),
                                           tables.back().data(),
                                           (uint32_t)tables.back().size())) {
            err = "failed to register host functions for " + kv.first;
            return false;
        }
    }
    return true;
}

}  // namespace

bool ModRuntime::available() const { return true; }
const char* ModRuntime::backendName() const { return "WAMR (interpreter)"; }

bool ModRuntime::init(std::string& err) {
    if (m_inited) return true;

    RuntimeInitArgs args{};
    args.mem_alloc_type = Alloc_With_System_Allocator;

    if (!wasm_runtime_full_init(&args)) {
        err = "WAMR failed to initialise";
        return false;
    }
    if (!registerNatives(err)) {
        wasm_runtime_destroy();
        return false;
    }
    m_inited = true;
    return true;
}

void ModRuntime::shutdown() {
    if (!m_inited) return;
    wasm_runtime_destroy();
    m_inited = false;
}

std::unique_ptr<ModInstance> ModRuntime::instantiate(const ModPackage& pkg,
                                                     uint32_t granted,
                                                     std::string& err) {
    if (!m_inited && !init(err)) return nullptr;

    auto w = new WamrInstance();
    auto cleanup = [&]() {
        if (w->env) wasm_runtime_destroy_exec_env(w->env);
        if (w->inst) wasm_runtime_deinstantiate(w->inst);
        if (w->module) wasm_runtime_unload(w->module);
        delete w;
    };

    w->wasmCopy = pkg.wasm();
    char buf[256] = {0};
    w->module = wasm_runtime_load(w->wasmCopy.data(), (uint32_t)w->wasmCopy.size(),
                                  buf, sizeof buf);
    if (!w->module) {
        err = std::string("mod.wasm did not load: ") + buf;
        cleanup();
        return nullptr;
    }

    // Capability enforcement. Anything the module imports must be a function we
    // provide *and* a capability the user granted.
    int32_t importCount = wasm_runtime_get_import_count(w->module);
    for (int32_t i = 0; i < importCount; i++) {
        wasm_import_t imp{};
        wasm_runtime_get_import_type(w->module, i, &imp);
        const char* mname = imp.module_name ? imp.module_name : "";
        const char* iname = imp.name ? imp.name : "";

        if (imp.kind != WASM_IMPORT_EXPORT_KIND_FUNC) {
            err = std::string("mod.wasm imports a non-function (\"") + mname +
                  "\".\"" + iname +
                  "\"); only host functions may be imported";
            cleanup();
            return nullptr;
        }

        const ModHostFn* fn = modFindHostFunction(mname, iname);
        if (!fn) {
            err = std::string("mod.wasm imports \"") + mname + "\".\"" + iname +
                  "\", which this host does not provide. Check the Gearbox "
                  "version it targets.";
            cleanup();
            return nullptr;
        }
        if (fn->capability && !(granted & fn->capability)) {
            err = std::string("mod.wasm imports \"") + mname + "\".\"" + iname +
                  "\" but the " + modModuleMaskToString(fn->capability) +
                  " module is not granted";
            cleanup();
            return nullptr;
        }
    }

    InstantiationArgs iargs{};
    // 64 KiB was enough for a hand-written C mod and nothing else. An
    // interpreter recurses deeply while parsing and evaluating: CPython 3.12
    // fails to initialise at 512 KiB and succeeds at 1 MiB, measured by
    // bisecting this value. 2 MiB is that floor with headroom.
    //
    // This is per instance, not per mod file, and it is address space rather
    // than resident memory -- the cost of being generous here is far smaller
    // than the cost of a mod that mysteriously dies inside its own parser.
    iargs.default_stack_size = 2 * 1024 * 1024;
    iargs.host_managed_heap_size = 0;
    // Verified in tests/mod_runtime_test.cpp ("memory limit"): a module can grow
    // to exactly this many pages and no further.
    //
    // WAMR logs "Cannot override max memory with value greater than module max
    // memory" here for any module that declares no maximum of its own, which is
    // every module clang emits. The message is misleading -- the cap applied is
    // ours, and the test pins that -- so do not go chasing it.
    iargs.max_memory_pages = pkg.manifest().limits.memoryPages;

    w->inst = wasm_runtime_instantiate_ex(w->module, &iargs, buf, sizeof buf);
    if (!w->inst) {
        err = std::string("mod.wasm did not instantiate: ") + buf;
        cleanup();
        return nullptr;
    }

    w->env = wasm_runtime_create_exec_env(w->inst, iargs.default_stack_size);
    if (!w->env) {
        err = "could not create an execution environment for the mod";
        cleanup();
        return nullptr;
    }

    std::unique_ptr<ModInstance> mi(new ModInstance());
    mi->m_manifest = pkg.manifest();
    mi->m_granted = granted;
    mi->m_package = &pkg;
    mi->m_impl = w;
    mi->m_fuelBudget = pkg.manifest().limits.fuelPerTurn;
    mi->m_loadFuelBudget = pkg.manifest().limits.loadFuel;

    // So host natives can find their way back from an exec_env.
    wasm_runtime_set_custom_data(w->inst, mi.get());

    // Run the module's initialiser, if it has one.
    //
    // WAMR calls __wasm_call_ctors for us, which covers C and C++ static
    // constructors. It does NOT call _initialize: that lookup lives inside
    // `#if WASM_ENABLE_LIBC_WASI != 0` (wasm_runtime.c, "WASI reactor instances
    // may assume that _initialize will be called"), and we build with
    // WAMR_BUILD_LIBC_WASI=0 on purpose -- that is the sandbox.
    //
    // So every toolchain following the reactor convention -- TinyGo emits
    // exactly this, as do Rust wasip1 and C# NativeAOT -- would reach mod_load
    // with an uninitialised runtime and trap on the first allocation. Observed
    // with TinyGo 0.41: "mod_load: Exception: unreachable".
    //
    // wasm-ld emits either _initialize (reactor model) or an exported
    // __wasm_call_ctors, never both, so this cannot double-run constructors.
    if (wasm_runtime_lookup_function(w->inst, "_initialize")) {
        std::string initErr;
        if (!mi->callExport("_initialize", nullptr, 0, nullptr, initErr)) {
            err = "mod initialiser failed: " + initErr;
            return nullptr;
        }
    }
    return mi;
}

ModInstance::~ModInstance() {
    auto* w = (WamrInstance*)m_impl;
    if (!w) return;
    if (w->env) wasm_runtime_destroy_exec_env(w->env);
    if (w->inst) wasm_runtime_deinstantiate(w->inst);
    if (w->module) wasm_runtime_unload(w->module);
    delete w;
    m_impl = nullptr;
}

bool ModInstance::memRead(uint32_t offset, uint32_t size, void* dst) const {
    auto* w = (WamrInstance*)m_impl;
    if (!w || !w->inst || size == 0 || !dst) return false;
    if (!wasm_runtime_validate_app_addr(w->inst, offset, size)) return false;
    memcpy(dst, wasm_runtime_addr_app_to_native(w->inst, offset), size);
    return true;
}

bool ModInstance::memWrite(uint32_t offset, uint32_t size, const void* src) {
    auto* w = (WamrInstance*)m_impl;
    if (!w || !w->inst || size == 0 || !src) return false;
    if (!wasm_runtime_validate_app_addr(w->inst, offset, size)) return false;
    memcpy(wasm_runtime_addr_app_to_native(w->inst, offset), src, size);
    return true;
}

bool ModInstance::hasExport(const char* name) const {
    auto* w = (WamrInstance*)m_impl;
    if (!w || !w->inst) return false;
    return wasm_runtime_lookup_function(w->inst, name) != nullptr;
}

// The one-time calls, which draw on the load budget rather than the per-turn
// one. Deliberately a closed list: every other export is a per-turn hook and
// must stay on the tight budget, so a new export defaults to the safe side.
static bool isLoadHook(const char* name) {
    return name && (strcmp(name, "mod_load") == 0 ||
                    strcmp(name, "_initialize") == 0);
}

bool ModInstance::callExport(const char* name, const uint32_t* args,
                             uint32_t argc, uint32_t* ret, std::string& err) {
    auto* w = (WamrInstance*)m_impl;
    if (!w || !w->inst || !w->env) { err = "mod is not instantiated"; return false; }

    wasm_function_inst_t fn = wasm_runtime_lookup_function(w->inst, name);
    if (!fn) { err = std::string("mod does not export ") + name; return false; }

    // One slot per i32; the return value comes back in slot 0.
    uint32_t argv[8] = {0};
    if (argc > 8) { err = "too many arguments"; return false; }
    for (uint32_t i = 0; i < argc; i++) argv[i] = args[i];

    // Refuel per call. A hook that burns its budget is terminated by the
    // interpreter rather than being allowed to hang the turn.
    //
    // mod_load and _initialize draw on a separate, larger budget: they run once
    // when the user enables the mod, and for an interpreter SDK they are
    // dominated by starting the interpreter rather than by anything the mod
    // does per turn. Charging that to fuelPerTurn would mean a Ruby or Python
    // mod could only load by declaring a per-turn budget it never needs.
    const bool loadHook = isLoadHook(name);
    const uint64_t budget = loadHook ? m_loadFuelBudget : m_fuelBudget;
    if (budget > 0 && budget < (uint64_t)INT32_MAX)
        wasm_runtime_set_instruction_count_limit(w->env, (int)budget);
    else
        wasm_runtime_set_instruction_count_limit(w->env, -1);

    // Restored rather than cleared: mod_load may call back into the host, and a
    // nested call must not leave the outer hook reporting the wrong budget.
    const bool wasInLoadHook = m_inLoadHook;
    m_inLoadHook = loadHook;
    struct Restore {
        bool* flag; bool prev;
        ~Restore() { *flag = prev; }
    } restore{&m_inLoadHook, wasInLoadHook};

    wasm_runtime_clear_exception(w->inst);
    bool ok = wasm_runtime_call_wasm(w->env, fn, argc, argv);
    if (!ok) {
        const char* ex = wasm_runtime_get_exception(w->inst);
        err = std::string(name) + ": " + (ex ? ex : "trapped");
        m_lastError = err;
        return false;
    }
    if (ret) *ret = argv[0];
    return true;
}

void ModInstance::resetFuel(uint64_t fuel) { m_fuelBudget = fuel; }

uint64_t ModInstance::fuelRemaining() const {
    // WAMR enforces the instruction limit but exposes no live counter, so this
    // is the budget for the current hook, not a countdown. See the note on
    // gearbox_fuel_budget in sdk/gearbox.h.
    const uint64_t budget = m_inLoadHook ? m_loadFuelBudget : m_fuelBudget;
    return budget == 0 ? UINT64_MAX : budget;
}

void ModInstance::setAbort(const std::string& msg) {
    auto* w = (WamrInstance*)m_impl;
    m_lastError = msg;
    if (w && w->inst) wasm_runtime_set_exception(w->inst, msg.c_str());
}

ModInstance* modCurrentInstance(void* execEnv) {
    if (!execEnv) return nullptr;
    auto env = (wasm_exec_env_t)execEnv;
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    if (!inst) return nullptr;
    return (ModInstance*)wasm_runtime_get_custom_data(inst);
}

// =============================================================== browser ====
#elif defined(OD_MODS_WEB)

// The web build is itself WebAssembly, so the browser's own engine is the
// sandbox. That is what makes hot-loading work on itch.io with no page reload.
//
// KNOWN GAP, stated plainly: the browser exposes no instruction limit, so
// limits.fuelPerTurn is NOT enforced here. A mod with an infinite loop in a hook
// will hang the tab. On desktop WAMR enforces it. See docs/modding.md.

#include <emscripten.h>

namespace {

// One record per host function, in a layout the JS side reads directly.
struct WebHostRec {
    const char* module;
    const char* name;
    void*       fn;
    uint32_t    capability;
};

std::vector<WebHostRec>& webTable() {
    static std::vector<WebHostRec> t;
    if (t.empty()) {
        size_t n = 0;
        const ModHostFn* h = modHostFunctions(n);
        for (size_t i = 0; i < n; i++)
            t.push_back({h[i].module, h[i].name, h[i].fn, h[i].capability});
    }
    return t;
}

}  // namespace

EM_JS(void, od_mods_boot, (), {
    if (!Module.__odMods) Module.__odMods = {};
});

// Returns 0 on success. On failure writes a message into errPtr.
EM_JS(int, od_mods_instantiate,
      (uint32_t inst, uint32_t bytesPtr, uint32_t len, uint32_t recPtr,
       uint32_t recCount, uint32_t granted, uint32_t errPtr, uint32_t errCap), {
    function fail(msg) { stringToUTF8(msg, errPtr, errCap); return 1; }
    try {
        // Read the host function table the C side handed us: 4 words per record.
        const provided = {};
        for (let i = 0; i < recCount; i++) {
            const r = (recPtr + i * 16) >> 2;
            provided[UTF8ToString(HEAPU32[r]) + "\0" + UTF8ToString(HEAPU32[r + 1])] = {
                m: UTF8ToString(HEAPU32[r]),
                n: UTF8ToString(HEAPU32[r + 1]),
                p: HEAPU32[r + 2],
                c: HEAPU32[r + 3]
            };
        }

        const mod = new WebAssembly.Module(HEAPU8.slice(bytesPtr, bytesPtr + len));

        // Same capability check the desktop backend does, for the same reason:
        // a mod importing something it was not granted must be refused at load,
        // with a diagnostic that names the module.
        const imports = {};
        for (const d of WebAssembly.Module.imports(mod)) {
            if (d.kind !== "function")
                return fail('mod.wasm imports a non-function ("' + d.module + '"."' +
                            d.name + '"); only host functions may be imported');
            const rec = provided[d.module + "\0" + d.name];
            if (!rec)
                return fail('mod.wasm imports "' + d.module + '"."' + d.name +
                            '", which this host does not provide. Check the ' +
                            'Gearbox version it targets.');
            if (rec.c !== 0 && (granted & rec.c) === 0)
                return fail('mod.wasm imports "' + d.module + '"."' + d.name +
                            '" but that capability is not granted');
            if (!imports[rec.m]) imports[rec.m] = {};
            const f = wasmTable.get(rec.p);
            // Every host native takes the instance handle first; see ExecEnv in
            // ModHost.cpp, which on this backend is the ModInstance pointer.
            if (rec.m === "gearbox:core" && rec.n === "abort") {
                // gearbox_abort is declared noreturn, and on WAMR it really does
                // not return -- the host raises an exception that unwinds. The
                // browser has no equivalent, so the trap has to come from here.
                // Without this the mod keeps running after aborting, which the
                // ABI says cannot happen.
                imports[rec.m][rec.n] = function() {
                    f(inst, ...arguments);
                    throw new Error("gearbox_abort");
                };
            } else {
                imports[rec.m][rec.n] = function() { return f(inst, ...arguments); };
            }
        }

        const instance = new WebAssembly.Instance(mod, imports);

        // WAMR calls these for us after instantiation (wasm_runtime.c,
        // "WASI reactor instances may assume that _initialize will be called");
        // WebAssembly.Instance does not. Without this a C++ mod's static
        // constructors never run, and any toolchain following the reactor
        // convention -- TinyGo, Rust wasip1, C# NativeAOT -- starts with an
        // uninitialised runtime and fails in ways that look like host bugs.
        const init = instance.exports._initialize || instance.exports.__wasm_call_ctors;
        if (typeof init === "function") {
            try {
                init();
            } catch (e) {
                return fail("mod initialiser trapped: " + String(e));
            }
        }

        Module.__odMods[inst] = instance;
        return 0;
    } catch (e) {
        return fail("mod.wasm did not instantiate: " + String(e));
    }
});

EM_JS(void, od_mods_destroy, (uint32_t inst), {
    if (Module.__odMods) delete Module.__odMods[inst];
});

EM_JS(int, od_mods_has_export, (uint32_t inst, uint32_t namePtr), {
    const it = Module.__odMods && Module.__odMods[inst];
    if (!it) return 0;
    return (typeof it.exports[UTF8ToString(namePtr)] === "function") ? 1 : 0;
});

EM_JS(int, od_mods_call,
      (uint32_t inst, uint32_t namePtr, uint32_t argsPtr, uint32_t argc,
       uint32_t retPtr, uint32_t errPtr, uint32_t errCap), {
    const name = UTF8ToString(namePtr);
    const it = Module.__odMods && Module.__odMods[inst];
    if (!it) { stringToUTF8("mod is not instantiated", errPtr, errCap); return 1; }
    const fn = it.exports[name];
    if (typeof fn !== "function") {
        stringToUTF8("mod does not export " + name, errPtr, errCap);
        return 1;
    }
    try {
        const args = [];
        for (let i = 0; i < argc; i++) args.push(HEAPU32[(argsPtr >> 2) + i]);
        const r = fn.apply(null, args);
        HEAPU32[retPtr >> 2] = (r === undefined || r === null) ? 0 : (Number(r) >>> 0);
        return 0;
    } catch (e) {
        stringToUTF8(name + ": " + String(e), errPtr, errCap);
        return 1;
    }
});

// The mod's linear memory is a separate ArrayBuffer; these copy across.
EM_JS(int, od_mods_mem_read,
      (uint32_t inst, uint32_t off, uint32_t size, uint32_t dst), {
    const it = Module.__odMods && Module.__odMods[inst];
    if (!it || !it.exports.memory) return 0;
    const u8 = new Uint8Array(it.exports.memory.buffer);
    if (size > u8.length || off > u8.length - size) return 0;
    HEAPU8.set(u8.subarray(off, off + size), dst);
    return 1;
});

EM_JS(int, od_mods_mem_write,
      (uint32_t inst, uint32_t off, uint32_t size, uint32_t src), {
    const it = Module.__odMods && Module.__odMods[inst];
    if (!it || !it.exports.memory) return 0;
    const u8 = new Uint8Array(it.exports.memory.buffer);
    if (size > u8.length || off > u8.length - size) return 0;
    u8.set(HEAPU8.subarray(src, src + size), off);
    return 1;
});

bool ModRuntime::available() const { return true; }
const char* ModRuntime::backendName() const {
    return "browser WebAssembly (no fuel metering)";
}

bool ModRuntime::init(std::string& err) {
    (void)err;
    if (m_inited) return true;
    od_mods_boot();
    m_inited = true;
    return true;
}

void ModRuntime::shutdown() { m_inited = false; }

std::unique_ptr<ModInstance> ModRuntime::instantiate(const ModPackage& pkg,
                                                     uint32_t granted,
                                                     std::string& err) {
    if (!m_inited && !init(err)) return nullptr;

    std::unique_ptr<ModInstance> mi(new ModInstance());
    mi->m_manifest = pkg.manifest();
    mi->m_granted = granted;
    mi->m_package = &pkg;
    mi->m_fuelBudget = pkg.manifest().limits.fuelPerTurn;
    mi->m_loadFuelBudget = pkg.manifest().limits.loadFuel;
    mi->m_impl = mi.get();          // the handle JS keys on is the instance itself

    auto& tbl = webTable();
    char buf[512] = {0};
    int r = od_mods_instantiate((uint32_t)(uintptr_t)mi.get(),
                                (uint32_t)(uintptr_t)pkg.wasm().data(),
                                (uint32_t)pkg.wasm().size(),
                                (uint32_t)(uintptr_t)tbl.data(),
                                (uint32_t)tbl.size(), granted,
                                (uint32_t)(uintptr_t)buf, (uint32_t)sizeof buf);
    if (r != 0) {
        err = buf[0] ? buf : "mod.wasm did not instantiate";
        return nullptr;
    }
    return mi;
}

ModInstance::~ModInstance() {
    if (m_impl) od_mods_destroy((uint32_t)(uintptr_t)m_impl);
    m_impl = nullptr;
}

bool ModInstance::memRead(uint32_t offset, uint32_t size, void* dst) const {
    if (!m_impl || size == 0 || !dst) return false;
    return od_mods_mem_read((uint32_t)(uintptr_t)m_impl, offset, size,
                            (uint32_t)(uintptr_t)dst) != 0;
}

bool ModInstance::memWrite(uint32_t offset, uint32_t size, const void* src) {
    if (!m_impl || size == 0 || !src) return false;
    return od_mods_mem_write((uint32_t)(uintptr_t)m_impl, offset, size,
                             (uint32_t)(uintptr_t)src) != 0;
}

bool ModInstance::hasExport(const char* name) const {
    if (!m_impl) return false;
    return od_mods_has_export((uint32_t)(uintptr_t)m_impl,
                              (uint32_t)(uintptr_t)name) != 0;
}

bool ModInstance::callExport(const char* name, const uint32_t* args, uint32_t argc,
                             uint32_t* ret, std::string& err) {
    if (!m_impl) { err = "mod is not instantiated"; return false; }
    uint32_t argv[8] = {0};
    if (argc > 8) { err = "too many arguments"; return false; }
    for (uint32_t i = 0; i < argc; i++) argv[i] = args[i];

    // No fuel is applied here: the browser has no instruction limit to set.
    char buf[512] = {0};
    uint32_t out = 0;
    int r = od_mods_call((uint32_t)(uintptr_t)m_impl, (uint32_t)(uintptr_t)name,
                         (uint32_t)(uintptr_t)argv, argc,
                         (uint32_t)(uintptr_t)&out,
                         (uint32_t)(uintptr_t)buf, (uint32_t)sizeof buf);
    if (r != 0) {
        err = buf[0] ? buf : "mod call failed";
        m_lastError = err;
        return false;
    }
    if (ret) *ret = out;
    return true;
}

void ModInstance::resetFuel(uint64_t fuel) { m_fuelBudget = fuel; }
uint64_t ModInstance::fuelRemaining() const {
    // Reported so a mod can size its work, but not enforced on this backend.
    return m_fuelBudget == 0 ? UINT64_MAX : m_fuelBudget;
}

void ModInstance::setAbort(const std::string& msg) { m_lastError = msg; }

ModInstance* modCurrentInstance(void* execEnv) {
    // On this backend the "exec env" JS passes back is the ModInstance itself.
    return (ModInstance*)execEnv;
}

// ============================================================== no backend ==
#else

bool ModRuntime::available() const { return false; }
const char* ModRuntime::backendName() const {
    return "none (built without OD_ENABLE_MODS)";
}

bool ModRuntime::init(std::string& err) {
    err = "this build has no WebAssembly runtime, so mods cannot run";
    return false;
}

void ModRuntime::shutdown() {}

std::unique_ptr<ModInstance> ModRuntime::instantiate(const ModPackage&, uint32_t,
                                                     std::string& err) {
    err = "this build has no WebAssembly runtime, so mods cannot run";
    return nullptr;
}

ModInstance::~ModInstance() = default;
bool ModInstance::memRead(uint32_t, uint32_t, void*) const { return false; }
bool ModInstance::memWrite(uint32_t, uint32_t, const void*) { return false; }
bool ModInstance::hasExport(const char*) const { return false; }
bool ModInstance::callExport(const char*, const uint32_t*, uint32_t, uint32_t*,
                             std::string& err) {
    err = "no runtime";
    return false;
}
void ModInstance::resetFuel(uint64_t f) { m_fuelBudget = f; }
uint64_t ModInstance::fuelRemaining() const { return UINT64_MAX; }
void ModInstance::setAbort(const std::string& msg) { m_lastError = msg; }
ModInstance* modCurrentInstance(void*) { return nullptr; }

#endif

// ------------------------------------------------------ backend-independent --

bool ModInstance::readString(uint32_t ptr, uint32_t len, std::string& out) const {
    if (len == 0) { out.clear(); return true; }
    // Cap before touching memory: a mod claiming a 4 GiB string should be
    // refused, not attempted.
    if (len > 1u << 20) return false;
    out.assign(len, '\0');
    if (!memRead(ptr, len, out.data())) { out.clear(); return false; }
    return true;
}
