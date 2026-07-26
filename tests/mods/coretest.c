/* Fixture mod for the runtime tests.
 *
 * Exercises the Core capability and, deliberately, several ways a mod can
 * misbehave: burning its fuel, aborting, and handing the host an
 * out-of-bounds pointer. The host is expected to survive all three.
 *
 * Built by tests/build_test_mods.sh into a freestanding wasm32 module.
 */

#include "gearbox.h"

#define S(lit) lit, (uint32_t)(sizeof(lit) - 1)

static gearbox_env_t g_env;

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) {
    g_env.size = sizeof g_env;
    gearbox_env(&g_env);
    gearbox_log(GEARBOX_LOG_INFO, S("coretest loaded"));
    return 0;
}

GEARBOX_EXPORT("mod_unload")
void mod_unload(void) {
    gearbox_log(GEARBOX_LOG_INFO, S("coretest unloaded"));
}

/* --- introspection the host asserts against ---------------------------- */

GEARBOX_EXPORT("t_env_size")
uint32_t t_env_size(void) { return g_env.size; }

GEARBOX_EXPORT("t_env_major")
uint32_t t_env_major(void) { return g_env.gearbox_major; }

GEARBOX_EXPORT("t_env_platform")
uint32_t t_env_platform(void) { return g_env.platform; }

GEARBOX_EXPORT("t_env_headless")
uint32_t t_env_headless(void) { return g_env.is_headless; }

GEARBOX_EXPORT("t_fuel_budget_lo")
uint32_t t_fuel_budget_lo(void) { return (uint32_t)gearbox_fuel_budget(); }

/* --- misbehaviour ------------------------------------------------------- */

/* Never returns on its own. The interpreter's instruction limit must stop it,
 * or the turn would hang. */
GEARBOX_EXPORT("t_burn_fuel")
void t_burn_fuel(void) {
    volatile uint32_t x = 0;
    for (;;) x++;
}

GEARBOX_EXPORT("t_abort")
void t_abort(void) {
    gearbox_abort(S("deliberate abort"));
}

/* Hands the host a pointer far outside linear memory. The host must refuse it
 * rather than read whatever is at that address. */
GEARBOX_EXPORT("t_oob_log")
void t_oob_log(void) {
    gearbox_log(GEARBOX_LOG_INFO, (const char*)0x7FFFFFF0u, 64u);
}

/* Reports how far memory can grow. The manifest's memoryPages caps this. */
GEARBOX_EXPORT("t_grow")
uint32_t t_grow(uint32_t pages) {
    return (uint32_t)__builtin_wasm_memory_grow(0, pages);
}

GEARBOX_EXPORT("t_mem_pages")
uint32_t t_mem_pages(void) {
    return (uint32_t)__builtin_wasm_memory_size(0);
}
