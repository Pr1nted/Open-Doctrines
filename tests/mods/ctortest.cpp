/* Fixture: does the host run static constructors before mod_load?
 *
 * The constructor calls an imported host function. That is the point: a call to
 * an import cannot be constant-folded, so the compiler is forced to emit a real
 * constructor and register it in __wasm_call_ctors. An earlier version of this
 * fixture just assigned a constant, which clang quietly folded into the data
 * segment -- making the test pass while proving nothing.
 *
 * Matters beyond C++: every toolchain following the WASI reactor convention
 * (TinyGo, Rust wasip1, C# NativeAOT) puts its runtime setup behind an
 * `_initialize` export and relies on the host calling it.
 */
#include "gearbox.h"

static int g_ctorRan = 0;

struct Ctor {
    Ctor() {
        gearbox_log(GEARBOX_LOG_INFO, "ctor ran", 8);
        g_ctorRan = 1;
    }
};

static Ctor g_ctor;

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) { return 0; }

/* 1 only if the constructor actually executed. */
GEARBOX_EXPORT("t_ctor_ran")
int32_t t_ctor_ran(void) { return g_ctorRan; }
