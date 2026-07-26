/* gearbox.h — OpenDoctrines mod ABI, Gearbox v1.0
 *
 * This header is the contract. Every SDK in every language binds to exactly
 * these imports and exports; if something is not described here it is not part
 * of the ABI and may change without notice.
 *
 * Two directions:
 *   - IMPORTS  (gearbox_*)  the host provides, your mod calls.
 *   - EXPORTS  (mod_*)      your mod provides, the host calls.
 *
 * Imports live in WASM modules named "gearbox:<module>", matching the
 * capability names in MANIFEST.json. You only receive imports for capabilities
 * you declared AND the user granted. An undeclared import is a link error at
 * load time — the mod is refused with a diagnostic, it does not fail later at
 * an inconvenient moment.
 *
 * Strings are (ptr, len) pairs of UTF-8 bytes in your linear memory. They are
 * NOT null-terminated and NOT owned by the host after the call returns. The
 * host never retains a pointer into your memory past the duration of a call.
 *
 * WHERE THE DECLARATIONS LIVE
 *
 * The declarations themselves are in gearbox_generated.h, produced from
 * sdk/abi.json by tools/gen_bindings.py — the same file the host's capability
 * table is pinned to by ModAbiTest, and the same file every other language
 * binding is generated from. Adding a function to the ABI is one edit there,
 * not thirteen here.
 *
 * This file keeps what a generator has no business owning: the prose, the
 * macros, and the handle typedefs.
 */

#ifndef GEARBOX_H
#define GEARBOX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GEARBOX_IMPORT(mod, name) \
    __attribute__((import_module("gearbox:" mod), import_name(name)))
#define GEARBOX_EXPORT(name) __attribute__((export_name(name)))

/* Entity handles are opaque 32-bit values, never pointers, and are only valid
 * for the duration of the hook that produced them. Do not cache across turns:
 * a handle you kept may be stale or reassigned. Store the country's name, or
 * your own key, instead.
 *
 * These are declared before the generated header because it uses them. */
typedef uint32_t gearbox_country;
typedef uint32_t gearbox_province;
typedef uint32_t gearbox_panel;

/* ---------------------------------------------------------------------------
 * The capabilities, and what each costs you in user trust:
 *
 *   Core            Always granted, cannot be revoked. Logging, environment,
 *                   abort, fuel budget.
 *   GameState.Read  The turn, countries, names, treasuries, provinces. Low
 *                   trust cost.
 *   UI              A host-managed rectangle you may draw in. You cannot draw
 *                   outside it, read the framebuffer, or capture global input.
 *                   All coordinates are panel-relative, and every UI import is
 *                   a silent no-op when env.is_headless is 1.
 *   Assets          Read-only access to your own data/ directory, straight out
 *                   of your .odmod. The archive is never unpacked to disk, and
 *                   names are looked up in your package's entry list rather
 *                   than resolved as paths — there is no other mod's data to
 *                   reach and no filesystem to escape into. Names are relative
 *                   to data/ and use '/' separators: data/flags/fr.png is
 *                   "flags/fr.png".
 *
 * Anything returning variable-length data uses two-call sizing: it returns the
 * FULL length and writes at most `cap` bytes. Call with cap 0 to size,
 * allocate, then call again. A returned length greater than your `cap` means
 * truncation, not an error.
 *
 * Of the exports, only mod_load is mandatory. There is no autorun: mods load
 * only from the mod menu, so mod_load runs on every session in which the user
 * enables you. Do not assume prior state.
 * ------------------------------------------------------------------------- */

#include "gearbox_generated.h"

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* GEARBOX_H */
