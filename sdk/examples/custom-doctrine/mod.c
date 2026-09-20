/* Custom Doctrine — a mod that adds two doctrines and nothing else.
 *
 * The smallest useful content mod there is. No panel, no turn hook, no state:
 * mod_load declares two entries in the game's doctrine catalogue and stops.
 * Two capabilities, Core (log) and Content.
 *
 * ── WHERE THE DOCTRINES LIVE ──
 *
 * In doctrines.json, next to this file, as the SAME JSON data/policies.json
 * uses. build.sh turns that file into doctrines.h — a table of (id, json)
 * pairs — so the content stays data you can edit and diff, rather than string
 * literals escaped into C. Change a number there and rebuild; this file does
 * not move.
 *
 * That is also why there is no JSON parser here. Splitting the array happens
 * at build time, in python3, where a parser already exists.
 *
 * ── WHAT IT DEMONSTRATES ──
 *
 *   - PERSIST, so a save that recorded a country holding one of these still
 *     knows what it was after the mod is uninstalled;
 *   - a namespaced id (com_example:...), because catalogue ids are global and
 *     the first mod to claim one keeps it;
 *   - owner_of, used only when an add is REFUSED, to say who has the id --
 *     asking first would be wrong, since on a reload the answer would be this
 *     mod itself and the entry would be skipped;
 *   - a conflict with a doctrine this mod does not own (privatization,
 *     flat_tax, land_reform). One side is enough: the engine reads the pair
 *     from either member, which is the only thing that could work, since a mod
 *     cannot edit the shipped catalogue.
 *
 * See docs/gearbox-custom-policy.md for what every field in doctrines.json
 * does, including the sign convention for cost levers, which is inverted.
 *
 * Build:  ./build.sh          (produces custom-doctrine.odmod)
 */

#include "gearbox.h"
#include "doctrines.h"

#define S(lit) lit, (uint32_t)(sizeof(lit) - 1)

/* Catalogue kind 0 is doctrine, mode 1 is PERSIST. See src/ModContent.h. */
#define KIND_DOCTRINE 0u
#define MODE_PERSIST  1u

/* No libc: the log lines are built by hand. */
static uint32_t append(char* dst, uint32_t at, uint32_t cap,
                       const char* src, uint32_t len) {
    for (uint32_t i = 0; i < len && at < cap; i++) dst[at++] = src[i];
    return at;
}

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) {
    char line[256];
    uint32_t added = 0;

    for (uint32_t i = 0; i < gbx_doctrine_count; i++) {
        const gbx_doctrine* d = &gbx_doctrines[i];

        if (gearbox_content_add(KIND_DOCTRINE, d->id, d->id_len,
                                d->json, d->json_len, MODE_PERSIST)) {
            added++;
            continue;
        }

        /* Refused. Either the id belongs to somebody else or the definition
         * could not be read; owner_of separates the two, and a mod that only
         * logged "refused" would leave its author guessing which. */
        char owner[64];
        uint32_t n = gearbox_content_owner_of(KIND_DOCTRINE, d->id, d->id_len,
                                              owner, sizeof owner);
        if (n > sizeof owner) n = sizeof owner;

        uint32_t at = append(line, 0, sizeof line, S("doctrine '"));
        at = append(line, at, sizeof line, d->id, d->id_len);
        if (n > 0) {
            at = append(line, at, sizeof line, S("' is already owned by "));
            at = append(line, at, sizeof line, owner, n);
        } else {
            at = append(line, at, sizeof line,
                        S("' was refused: check the id and the JSON"));
        }
        gearbox_log(GEARBOX_LOG_ERROR, line, at);
    }

    /* A content mod that added nothing has nothing to do, and saying so at
     * load is kinder than sitting in the mod list looking active. The value is
     * shown to the player. */
    if (added == 0) return 1;

    uint32_t at = append(line, 0, sizeof line, S("custom-doctrine: added "));
    line[at++] = (char)('0' + added);        /* two entries; no formatter needed */
    at = append(line, at, sizeof line, S(" of "));
    line[at++] = (char)('0' + gbx_doctrine_count);
    at = append(line, at, sizeof line, S(" doctrines"));
    gearbox_log(GEARBOX_LOG_INFO, line, at);
    return 0;
}

/* The host removes a mod's content when it unloads, so there is nothing to
 * undo here. It is exported anyway because its absence reads as an oversight. */
GEARBOX_EXPORT("mod_unload")
void mod_unload(void) {}
