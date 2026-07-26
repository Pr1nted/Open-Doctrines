/* Fixture: does the host call _initialize on a reactor-model module?
 *
 * TinyGo, Rust wasip1 and C# NativeAOT all follow the WASI reactor convention:
 * they export `_initialize` and expect the environment to call it before any
 * other export. WAMR only does that when built with WASM_ENABLE_LIBC_WASI, and
 * we deliberately build with it off -- that is the sandbox. So the host has to
 * call it itself. Without that, a TinyGo mod reaches mod_load with an
 * uninitialised runtime: "mod_load: Exception: unreachable".
 *
 * This models the convention exactly. `_initialize` is exported and calls
 * __wasm_call_ctors, which is NOT exported -- so WAMR's own post-instantiate
 * lookup cannot find it, and the constructor runs only if the host calls
 * `_initialize`. Take the fix out of ModRuntime.cpp and this fails.
 *
 * The constructor calls an import so the optimiser cannot fold it into the data
 * segment, which would make the test pass while proving nothing.
 */
#include "gearbox.h"

extern "C" void __wasm_call_ctors(void);   /* synthesised by wasm-ld */

static int g_initRan = 0;

struct Init {
    Init() {
        gearbox_log(GEARBOX_LOG_INFO, "reactor init ran", 16);
        g_initRan = 1;
    }
};

static Init g_init;

GEARBOX_EXPORT("_initialize")
extern "C" void _initialize(void) { __wasm_call_ctors(); }

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) { return 0; }

GEARBOX_EXPORT("t_init_ran")
int32_t t_init_ran(void) { return g_initRan; }
