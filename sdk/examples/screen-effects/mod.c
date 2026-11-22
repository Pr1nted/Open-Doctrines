/* Screen Effects -- what the Shaders capability is and what it refuses to do.
 *
 * Three capabilities: Core (log, env), UI (one panel, which is also the clock
 * and the off switch) and Shaders.
 *
 * WHAT IT DRAWS
 *
 *   the MAP target    hue rolling continuously, with saturation pushed up
 *   the PANEL target  a vignette and film grain on this panel alone
 *
 * WHAT IT CANNOT DRAW, which is the more instructive half.
 *
 * The "try to strobe" button does exactly what a mod would do to flash the
 * screen: it asks for full grain on one frame and none on the next, for as long
 * as it is held on. Watch the map while it runs. It fades, slowly, and settles
 * somewhere in the middle -- because the host moves the rendered value toward
 * the asked-for one at a bounded rate and a request that reverses every frame
 * never gets anywhere. A full-screen value alternating at the frame rate is the
 * thing that triggers photosensitive seizures, and the host makes it
 * impossible rather than asking mods not to do it.
 *
 * The panel shows what was asked for next to what the host is rendering, so the
 * gap is visible rather than something you have to take on trust.
 *
 * TWO MORE THINGS WORTH KNOWING
 *
 * A target has ONE owner. If another mod already holds the map, set_effect
 * returns false and this panel says so -- it is not a silent fight.
 *
 * And a player can switch screen effects off (Settings -> Display -> Mod Screen
 * Effects). The calls still succeed and nothing in the ABI reports it, so this
 * mod cannot tell, and cannot route round it. If the numbers below say the map
 * is being asked for a strong effect and the map looks normal, that is probably
 * what happened -- which is the correct amount of explanation for a mod to be
 * able to give.
 *
 * Freestanding: no libc, no allocator, no float.
 *
 * Build:  ./build.sh          (produces screen-effects.odmod)
 */

#include "gearbox.h"

#define S(lit) lit, (uint32_t)(sizeof(lit) - 1)

static gearbox_env_t g_env;
static gearbox_panel g_panel;
static uint32_t      g_phase;         /* advances once per frame           */
static uint32_t      g_on = 1;        /* the map effect                    */
static uint32_t      g_strobe;        /* the demonstration that fails      */
static uint32_t      g_mapHeld;       /* 1 while we own the MAP target     */
static uint32_t      g_refused;       /* 1 if somebody else owns it        */

/* --- no libc ----------------------------------------------------------- */

static uint32_t u32_to_str(uint32_t v, char* out) {
    char tmp[12];
    uint32_t n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (uint32_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    return n;
}

static uint32_t append(char* dst, uint32_t at, const char* src, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) dst[at + i] = src[i];
    return at + len;
}

/* --- the effect -------------------------------------------------------- */

/* Hue is per-mille of a full turn, so it wraps at 1000 rather than at 360.
 * Rolling it one per frame is ~17 seconds for a full cycle at 60fps, which is
 * roughly as fast as the host will let it move anyway: ask for a faster roll
 * and you get the same speed, because the slew limiter is the ceiling. */
static void apply(void) {
    if (!g_on) {
        if (g_mapHeld) {
            gearbox_shaders_clear(GEARBOX_SHADER_TARGET_MAP);
            g_mapHeld = 0;
        }
        return;
    }

    const uint32_t hue = (g_phase * 2u) % 1001u;
    const uint32_t ok = gearbox_shaders_set_effect(
        GEARBOX_SHADER_TARGET_MAP, GEARBOX_SHADER_EFFECT_HUE_ROTATE, hue);
    /* False means the target is somebody else's, or there is no renderer here
     * at all (a server, or odmod-check). Neither is a reason to keep calling
     * blind, and both are worth showing. */
    g_refused = ok ? 0u : 1u;
    g_mapHeld = ok;
    if (!ok) return;

    gearbox_shaders_set_effect(GEARBOX_SHADER_TARGET_MAP,
                               GEARBOX_SHADER_EFFECT_SATURATION, 800);

    /* THE DEMONSTRATION. Full grain, then none, then full, every frame -- the
     * most direct way to ask for a flash. The host answers with a fade. */
    if (g_strobe) {
        gearbox_shaders_set_effect(GEARBOX_SHADER_TARGET_MAP,
                                   GEARBOX_SHADER_EFFECT_GRAIN,
                                   (g_phase & 1u) ? 1000u : 0u);
    } else {
        gearbox_shaders_set_effect(GEARBOX_SHADER_TARGET_MAP,
                                   GEARBOX_SHADER_EFFECT_GRAIN, 0);
    }
}

/* --- exports ----------------------------------------------------------- */

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) {
    g_env.size = sizeof g_env;
    gearbox_env(&g_env);

    if (g_env.is_headless) {
        /* No renderer, nobody to draw for. The host would no-op these anyway;
         * not asking is the explicit version. */
        gearbox_log(GEARBOX_LOG_INFO, S("screen-effects: headless, nothing to draw"));
        return 0;
    }

    /* Ask before assuming. The target and effect lists grow by release, so a
     * mod built against a newer SDK than the host would otherwise discover the
     * gap one refusal at a time. */
    if (!gearbox_shaders_supported(GEARBOX_SHADER_TARGET_MAP,
                                   GEARBOX_SHADER_EFFECT_HUE_ROTATE)) {
        gearbox_log(GEARBOX_LOG_WARN,
                    S("screen-effects: this host has no map hue rotate"));
        return 0;
    }

    g_panel = gearbox_panel_register(S("Screen Effects"), 260, 176);
    if (g_panel == 0) {
        /* No panel means no per-frame call, which means no animation at all. */
        gearbox_log(GEARBOX_LOG_WARN,
                    S("screen-effects: no panel, so no frame hook and no effect"));
        return 0;
    }

    /* The panel's own look, set once: it does not animate, so there is nothing
     * to repaint every frame. A vignette and a little grain on a mod's own
     * panel is the safest thing in this capability -- the panel is already
     * clipped to its rectangle, so nothing here can reach a pixel outside it. */
    gearbox_shaders_set_effect(GEARBOX_SHADER_TARGET_PANEL,
                               GEARBOX_SHADER_EFFECT_VIGNETTE, 700);
    gearbox_shaders_set_effect(GEARBOX_SHADER_TARGET_PANEL,
                               GEARBOX_SHADER_EFFECT_GRAIN, 250);
    return 0;
}

GEARBOX_EXPORT("mod_unload")
void mod_unload(void) {
    /* The host releases a departing mod's targets anyway -- an effect left
     * behind by a mod that is no longer running is indistinguishable, to a
     * player, from the game being broken -- but a reload leaves the mod running,
     * so doing it here means a reload starts from a clean screen. */
    gearbox_shaders_clear(GEARBOX_SHADER_TARGET_MAP);
    gearbox_shaders_clear(GEARBOX_SHADER_TARGET_PANEL);
    g_mapHeld = 0;
}

GEARBOX_EXPORT("mod_draw_panel")
void mod_draw_panel(gearbox_panel panel, uint32_t w, uint32_t h) {
    (void)w;
    (void)h;
    if (panel != g_panel) return;

    ++g_phase;
    apply();

    char line[96];
    uint32_t n = 0;
    n = append(line, n, S("map hue asked "));
    n += u32_to_str((g_phase * 2u) % 1001u, line + n);
    gearbox_draw_text(panel, 12, 14, 0xFFFFFFFF, line, n);

    /* WHICH LINK FAILED, not just "nothing happened".
     *
     *   held 0, refused 1 -> another mod owns it, or there is no renderer
     *   held 0, refused 0 -> Shaders was not granted, or the map effect is off
     *   held 1, map plain -> the player switched screen effects off, which this
     *                        mod is not allowed to know
     */
    n = 0;
    n = append(line, n, S("held "));
    n += u32_to_str(g_mapHeld, line + n);
    n = append(line, n, S("  refused "));
    n += u32_to_str(g_refused, line + n);
    gearbox_draw_text(panel, 12, 30, 0xFF9AA0A6, line, n);

    /* S() expands to two arguments, so it cannot go inside a ternary. */
    if (g_on) { if (gearbox_button(panel, 12, 52, 100, 28, S("map off")))  g_on = 0; }
    else      { if (gearbox_button(panel, 12, 52, 100, 28, S("map on")))   g_on = 1; }
    if (g_strobe) {
        if (gearbox_button(panel, 124, 52, 112, 28, S("stop strobe"))) g_strobe = 0;
    } else {
        if (gearbox_button(panel, 124, 52, 112, 28, S("try to strobe"))) g_strobe = 1;
    }

    gearbox_draw_text(panel, 12, 92, 0xFF9AA0A6,
                      S("asking for 0 and 1000 on alternate"));
    gearbox_draw_text(panel, 12, 106, 0xFF9AA0A6,
                      S("frames gets you a slow fade, not a"));
    gearbox_draw_text(panel, 12, 120, 0xFF9AA0A6,
                      S("flash. that is the point."));
    gearbox_draw_text(panel, 12, 144, 0xFF9AA0A6,
                      S("this panel has its own vignette"));
    gearbox_draw_text(panel, 12, 158, 0xFF9AA0A6,
                      S("and grain, and the title stays clear."));
}
