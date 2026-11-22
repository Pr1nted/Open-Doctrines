/* LSDoctrines -- the map, but as though the cartographer were having a time.
 *
 * ── WHAT THIS IS NOW, AND WHAT IT WAS ──
 *
 * It used to paint a coloured disc on every province (the Render capability's
 * province_tint) and cycle the interface accent, and it needed a panel on
 * screen to do it: mod_draw_panel is the only per-frame export the ABI has, so
 * the panel was the clock.
 *
 * All three of those are gone. Gearbox 1.4's Shaders capability recolours the
 * SCREEN ITSELF rather than drawing dots on top of the map -- the board, the
 * panels, the sidebar, the lot -- and HUE_CYCLE rolls the hue by itself, so
 * there is nothing to tick and no window to put on screen. The whole mod is
 * now three calls in mod_load.
 *
 * It is worth being clear about which of those is the improvement. Not the
 * line count: the dots were never the effect, they were a mod reaching for the
 * only thing it was allowed to draw. The map is a texture the renderer owns,
 * and until there was a way to ask the host to recolour it, a mod could only
 * stipple over the top.
 *
 * ── WHAT IT DOES NOT CONTROL ──
 *
 * The SPEED is a request with a ceiling: HUE_CYCLE's 0..1000 maps to at most
 * half a turn of the wheel per second, the host integrates the phase, and
 * there is no way to ask for faster. A mod animating from its own clock could
 * pick any rate it liked, which is the parameter that actually matters for
 * anyone photosensitive.
 *
 * There is no off switch in here either, because there is no panel to put one
 * on. Disabling the mod is the off switch, the Mod Menu clears every effect
 * while it is open, and Settings -> Display -> Mod Screen Effects turns the
 * lot off without asking the mod -- which is the right place for all three.
 *
 * ── WHAT IS NOT COVERED ──
 *
 * The always-on-top layer: toasts, the feedback form, the pad cursor, and the
 * Mod Menu. That is deliberate and is what makes this safe to grant.
 *
 * The main menu, which is a different frame and is not captured.
 *
 * And the globe: on the sphere the board itself is untouched, though
 * everything drawn over it still moves, because SCREEN captures the frame
 * rather than the map layer.
 *
 * Freestanding: no libc, no allocator, no float.
 *
 * Build:  ./build.sh          (produces lsdoctrines.odmod)
 */

#include "gearbox.h"

#define S(lit) lit, (uint32_t)(sizeof(lit) - 1)

static gearbox_env_t g_env;

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) {
    g_env.size = sizeof g_env;
    gearbox_env(&g_env);

    if (g_env.is_headless) {
        /* A training or benchmark run has no renderer and nobody to see this.
         * The host would no-op the calls anyway; not making them is the
         * explicit version. */
        gearbox_log(GEARBOX_LOG_INFO, S("lsdoctrines: headless, staying sober"));
        return 0;
    }

    /* Ask before assuming. The target and effect lists grow by release, so a
     * mod built against a newer SDK than the host is running would otherwise
     * find out one silent refusal at a time. */
    if (!gearbox_shaders_supported(GEARBOX_SHADER_TARGET_SCREEN,
                                   GEARBOX_SHADER_EFFECT_WOBBLE)) {
        gearbox_log(GEARBOX_LOG_WARN,
                    S("lsdoctrines: this host has no screen wobble; needs Gearbox 1.4"));
        return 0;
    }

    /* THE WHOLE SCREEN, not just the map: the panels, the sidebar and the
     * turn bar roll and breathe along with the board. The always-on-top layer
     * and the Mod Menu are drawn clean whatever we do here, so this cannot
     * paint over its own off switch.
     *
     * Three quarters speed, and saturation up. Full speed is legal -- the
     * point of leaving headroom is that somebody reading this should see a
     * number that is a choice rather than a maximum. */
    if (!gearbox_shaders_set_effect(GEARBOX_SHADER_TARGET_SCREEN,
                                    GEARBOX_SHADER_EFFECT_HUE_CYCLE, 750)) {
        /* False means one of two things, and the message says both because
         * from in here they are indistinguishable: another mod already holds
         * the map target (one owner per target, first to claim it -- two mods
         * recolouring one layer produce a picture neither of them wrote), or
         * this host has no renderer to draw through at all, which is what a
         * server and the odmod-check tool look like.
         *
         * supported() does NOT separate them: it answers what the build can
         * do, not whether anything is being drawn. */
        gearbox_log(GEARBOX_LOG_WARN,
                    S("lsdoctrines: the map target was refused -- another mod "
                      "holds it, or there is no renderer here"));
        return 0;
    }
    gearbox_shaders_set_effect(GEARBOX_SHADER_TARGET_SCREEN,
                               GEARBOX_SHADER_EFFECT_SATURATION, 850);
    /* And the image pushed around as it goes. Full amplitude, which on this
     * target is a handful of pixels -- the host caps it so the interface
     * cannot drift far enough from itself to be unclickable. */
    gearbox_shaders_set_effect(GEARBOX_SHADER_TARGET_SCREEN,
                               GEARBOX_SHADER_EFFECT_WOBBLE, 1000);
    return 0;
}

GEARBOX_EXPORT("mod_unload")
void mod_unload(void) {
    /* The host releases a departing mod's targets anyway -- an effect left
     * behind by a mod that is no longer running is indistinguishable, to a
     * player, from the game being broken -- but a reload leaves the mod
     * running, so clearing here means a reload starts from a clean map. */
    gearbox_shaders_clear(GEARBOX_SHADER_TARGET_SCREEN);
}
