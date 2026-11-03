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
 *   Audio           Play sounds from your own assets. Volume is multiplied by
 *                   the player's own effects setting, so you cannot be louder
 *                   than they allowed, and every sound you started is stopped
 *                   when your mod unloads.
 *   Net             A channel between copies of YOUR mod on the players'
 *                   machines. It is not access to the game's traffic: orders,
 *                   deltas, tickets and chat never pass through it, and the
 *                   host stamps your mod id on what you send so you cannot
 *                   speak as another mod or another player. High trust cost
 *                   all the same -- it is the one capability that reaches off
 *                   the machine.
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

/* ---------------------------------------------------------------------------
 * Which side of a multiplayer game you are running on.
 *
 * `env.net_role` is the byte that used to be `reserved0`. STANDALONE is 0, so
 * a mod compiled before this existed reads the same byte and gets the right
 * answer for the only game it could have been in.
 *
 * WHY YOU SHOULD CARE
 *
 * In multiplayer the SERVER is authoritative for everything. A client's copy of
 * the world is overwritten by whatever the server sends at the end of each
 * turn, so a write you make on the client does not survive and was never going
 * to. That is not a restriction imposed on mods; it is what stops a modified
 * client from cheating, and it applies to your mod exactly as it applies to
 * everyone else's.
 *
 * So: presentation, panels and local convenience are client work. Anything that
 * changes the world belongs on the server side of your mod. Declare which side
 * you are in MANIFEST.json ("side": "client" | "server" | "both") -- a
 * client-side mod has GameState.Write and GameProcess masked off its grants for
 * the duration of a multiplayer session, whatever the user granted.
 * ------------------------------------------------------------------------- */
typedef enum {
    GEARBOX_NET_STANDALONE  = 0,   /* singleplayer: you are both sides       */
    GEARBOX_NET_CLIENT      = 1,   /* a server elsewhere is authoritative    */
    GEARBOX_NET_SERVER      = 2,   /* dedicated host: authoritative, no player */
    GEARBOX_NET_HOST_PLAYER = 3    /* authoritative, and playing             */
} gearbox_net_role;

/* ---------------------------------------------------------------------------
 * WHAT A COUNTRY PUBLISHES ABOUT ITSELF  (Gearbox 1.2)
 *
 * A country's profile shows its age, population, income, expenses, compass,
 * flags and districts to anybody who opens it. These four figures are the
 * country's own to publish or withhold -- and publishing is not a display
 * setting: migrants read what a country publishes, and good figures draw people
 * to it. A country with bad figures is better off saying nothing.
 *
 * Mirrors the `disclosure_field` enum in sdk/abi.json.
 * ------------------------------------------------------------------------- */
typedef enum {
    GEARBOX_DISCLOSURE_EXPENSES      = 0,  /* what its money is spent on      */
    GEARBOX_DISCLOSURE_DOCTRINES     = 1,  /* which doctrines are in force    */
    GEARBOX_DISCLOSURE_TREASURY      = 2,  /* what it held last turn          */
    GEARBOX_DISCLOSURE_DISTRICT_LAWS = 3   /* how its districts are governed  */
} gearbox_disclosure_field;

/* ---------------------------------------------------------------------------
 * SCREEN EFFECTS  (Gearbox 1.4)
 *
 * You name a TARGET and pick one of the host's effects with a 0..1000
 * parameter. You are not given a framebuffer, a shader or a draw callback, and
 * there is no target for the interface: the panels, the dialogs and the Mod
 * Menu that is the only way to switch you off are all drawn afterwards.
 *
 * Two things will surprise you if nobody says them first.
 *
 * What you ask for is NOT what is drawn this frame. The host moves the rendered
 * value toward yours at a bounded rate, so a parameter that jumps every frame
 * comes out as a fade. That is deliberate and it is not a performance
 * compromise: a full-screen value alternating at the frame rate is the thing
 * that triggers photosensitive seizures. Write the effect you want and let it
 * arrive.
 *
 * And a player can switch screen effects off entirely. Your calls still
 * succeed and nothing here reports it, because a mod that could read the
 * setting could route round it.
 *
 * Mirrors the `shader_target` and `shader_effect` enums in sdk/abi.json;
 * tools/check_sdk_enums.py fails the build if they drift apart.
 * ------------------------------------------------------------------------- */
typedef enum {
    GEARBOX_SHADER_TARGET_MAP   = 0,  /* the province raster, under everything */
    GEARBOX_SHADER_TARGET_PANEL = 1,  /* your own panels, clipped to their box  */
    /* The whole game screen -- map, counters, panels, sidebar, dialogs.
     *
     * NOT the always-on-top layer: toasts, the feedback form and the Mod Menu
     * are drawn after your effect, and the Mod Menu suppresses effects
     * entirely while it is open. You cannot paint over your own off switch. */
    GEARBOX_SHADER_TARGET_SCREEN = 2
} gearbox_shader_target;

typedef enum {
    GEARBOX_SHADER_EFFECT_HUE_ROTATE = 0,  /* 0..1000 -> 0..360 degrees        */
    GEARBOX_SHADER_EFFECT_SATURATION = 1,  /* 0..1000 -> 0..2x, 500 unchanged  */
    GEARBOX_SHADER_EFFECT_VIGNETTE   = 2,  /* 0..1000 -> none..full            */
    GEARBOX_SHADER_EFFECT_GRAIN      = 3,  /* 0..1000 -> none..full            */
    /* A hue that rolls on its own. 0..1000 is a SPEED, up to half a turn of
     * the wheel per second -- set it once and the host keeps turning.
     *
     * Reach for this rather than driving a hue from mod_draw_panel. That
     * export is the only per-frame hook the ABI has, so animating anything
     * otherwise means registering a panel to use as a clock, and the rate
     * ends up being the mod's rather than the host's. */
    GEARBOX_SHADER_EFFECT_HUE_CYCLE  = 4,
    /* Push the image around. 0..1000 is an amplitude; the host picks the
     * frequency and caps the distance. On SCREEN the cap is small on purpose:
     * a wobble moves pixels and not hit boxes, so a button drawn away from
     * itself still answers where it really is. */
    GEARBOX_SHADER_EFFECT_WOBBLE     = 5
} gearbox_shader_effect;

/* True wherever there is a local player to draw for -- which includes
 * singleplayer and a host who is also playing. Use this to decide whether to
 * put up UI. */
static inline int gearbox_is_client(const gearbox_env_t* env) {
    return env->net_role == GEARBOX_NET_STANDALONE
        || env->net_role == GEARBOX_NET_CLIENT
        || env->net_role == GEARBOX_NET_HOST_PLAYER;
}

/* True wherever this process owns the world. Use this to decide whether a
 * write is real. In singleplayer both this and gearbox_is_client are true,
 * because there is only one process and it is both. */
static inline int gearbox_is_server(const gearbox_env_t* env) {
    return env->net_role == GEARBOX_NET_STANDALONE
        || env->net_role == GEARBOX_NET_SERVER
        || env->net_role == GEARBOX_NET_HOST_PLAYER;
}

/* True only in a real multiplayer session, on either side. */
static inline int gearbox_is_multiplayer(const gearbox_env_t* env) {
    return env->net_role != GEARBOX_NET_STANDALONE;
}

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* GEARBOX_H */
