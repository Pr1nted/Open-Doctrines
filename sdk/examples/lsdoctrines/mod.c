/* LSDoctrines -- a complete, working OpenDoctrines mod, and a practical one.
 *
 * Rolls colour across every province on the map, continuously, so the board
 * looks like the cartographer was having a time of it. Four capabilities:
 * Core (log, env), UI (one panel), GameState.Read (how many provinces there
 * are) and Render (the tints).
 *
 * WHY A PANEL AT ALL, for a mod that draws on the map rather than in a panel.
 * mod_draw_panel is the ONLY per-frame export the ABI has -- mod_pre_turn and
 * mod_post_turn fire once a turn, which would give one colour change per turn
 * and no animation. So the panel is the clock. It is also the off switch and
 * the intensity control, which is the honest version of the arrangement: a
 * mod that repaints the whole map should be visibly present and easy to stop.
 *
 * Freestanding: no libc, no allocator, no float. The colour wheel is integer
 * arithmetic over a 6-segment ramp, because -nostdlib means no libm and a
 * sinf() here would not link.
 *
 * Build:  ./build.sh          (produces lsdoctrines.odmod)
 */

#include "gearbox.h"

#define S(lit) lit, (uint32_t)(sizeof(lit) - 1)

static gearbox_env_t g_env;
static gearbox_panel g_panel;
static uint32_t      g_phase;        /* advances once per frame            */
static uint32_t      g_intensity = 3; /* 0 = off, 5 = unwise               */
static uint32_t      g_painted;      /* provinces tinted on the last pass  */
static uint32_t      g_playerAccent; /* theirs, read once, handed back     */
static uint32_t      g_accentHeld;   /* 1 while we are overriding it       */

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

/* --- the colour wheel, in integers ------------------------------------- */

/* hue 0..1535 around the wheel, back to 0xRRGGBB. Six ramps of 256: red to
 * yellow to green to cyan to blue to magenta and round again. No float, no
 * table, and the same answer on every platform -- which matters more than it
 * looks, because a mod that drew different colours on two machines in a
 * multiplayer game would be a bug report nobody could reproduce. */
static uint32_t wheel(uint32_t hue) {
    const uint32_t seg = (hue / 256u) % 6u;
    const uint32_t t   = hue % 256u;
    uint32_t r = 0, g = 0, b = 0;
    switch (seg) {
        case 0: r = 255;     g = t;       b = 0;       break;
        case 1: r = 255 - t; g = 255;     b = 0;       break;
        case 2: r = 0;       g = 255;     b = t;       break;
        case 3: r = 0;       g = 255 - t; b = 255;     break;
        case 4: r = t;       g = 0;       b = 255;     break;
        default:r = 255;     g = 0;       b = 255 - t; break;
    }
    return (r << 16) | (g << 8) | b;
}

/* --- the effect -------------------------------------------------------- */

static void paint(void) {
    const uint32_t count = gearbox_province_count();

    if (g_intensity == 0) {
        /* Alpha zero REMOVES a tint, and one call does set and clear, so
         * turning the effect off is the same loop rather than a special
         * path. render_clear would also work; this keeps one code path. */
        if (g_painted != 0) {
            gearbox_render_clear();
            g_painted = 0;
        }
        return;
    }

    /* The cap is 4096 tints per mod. A map larger than that is not a reason
     * to paint a partial rainbow and leave the rest plain -- that reads as a
     * broken mod rather than a deliberate one -- so stop at the cap and say
     * so once, which mod_load already did. */
    const uint32_t limit = count > 4096u ? 4096u : count;

    /* HUE COMES FROM WHERE A PROVINCE IS, NOT FROM ITS INDEX.
     *
     * The first version of this used the loop counter, which scatters
     * neighbours to unrelated hues: the map came out as confetti, because
     * province order has nothing to do with geography. Reading the centre
     * instead makes the colour a function of position, so what crosses the
     * map is a diagonal band -- which is the thing this mod is for.
     *
     * The host draws a tint as a disc at the province centre, so a wave in
     * position space is a wave on screen. */
    const uint32_t mw = gearbox_width();
    const uint32_t mh = gearbox_height();
    if (mw == 0u || mh == 0u) {    /* no world; nothing to stand on */
        g_painted = 0;
        return;
    }

    /* One full turn of the wheel across the map's diagonal, times the
     * intensity. Integer per-mille of the map so there is no float beyond
     * the two the host hands back. */
    const uint32_t turns = g_intensity;
    const uint32_t alpha = 60u + 28u * g_intensity;   /* 88..200 */

    uint32_t painted = 0;
    for (uint32_t i = 0; i < limit; ++i) {
        const gearbox_province p = gearbox_province_at(i);

        const double cx = gearbox_province_center_x(p);
        const double cy = gearbox_province_center_y(p);
        /* Per-mille along each axis, clamped: a province centre outside the
         * raster would otherwise wrap the hue to somewhere unrelated. */
        int32_t fx = (int32_t)((cx * 1000.0) / (double)mw);
        int32_t fy = (int32_t)((cy * 1000.0) / (double)mh);
        if (fx < 0) fx = 0; else if (fx > 1000) fx = 1000;
        if (fy < 0) fy = 0; else if (fy > 1000) fy = 1000;

        /* Diagonal: x and y both carry the wave, so the band runs corner to
         * corner rather than straight down a meridian. */
        const uint32_t along = (uint32_t)(fx + fy) / 2u;          /* 0..1000 */
        const uint32_t hue = (along * 1536u * turns / 1000u + g_phase * 6u) % 1536u;

        if (gearbox_province_tint(p, (wheel(hue) << 8) | alpha)) ++painted;
    }
    g_painted = painted;
}

/* --- exports ----------------------------------------------------------- */

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) {
    g_env.size = sizeof g_env;
    gearbox_env(&g_env);

    if (g_env.is_headless) {
        /* A training run has no renderer and nobody to see this. Tints would
         * be refused anyway; not asking is the explicit version. */
        gearbox_log(GEARBOX_LOG_INFO, S("lsdoctrines: headless, staying sober"));
        return 0;
    }

    /* Read the player's own accent BEFORE overriding it. theme_accent
     * returns what they chose, never another mod's override, so this is
     * safe to take at any point -- but taking it at load keeps the restore
     * honest even if another mod is also meddling. */
    g_playerAccent = gearbox_theme_accent();

    g_panel = gearbox_panel_register(S("LSDoctrines"), 260, 140);
    if (g_panel == 0) {
        /* Without a panel there is no per-frame call, so there is no effect
         * at all. Say so plainly rather than loading into a silent nothing. */
        gearbox_log(GEARBOX_LOG_WARN,
                    S("lsdoctrines: no panel, so no frame hook and no effect"));
        return 0;
    }

    if (gearbox_province_count() > 4096u) {
        gearbox_log(GEARBOX_LOG_WARN,
                    S("lsdoctrines: map is over the 4096-tint cap; "
                      "the first 4096 provinces get the treatment"));
    }
    return 0;
}

GEARBOX_EXPORT("mod_unload")
void mod_unload(void) {
    /* Hand the interface back. The host drops the override when no mod is
     * running anyway, so this is belt and braces -- but a reload leaves the
     * mod running, and without this the player's own colour would be gone
     * until they restarted. */
    if (g_accentHeld) {
        gearbox_set_theme_accent(g_playerAccent);
        g_accentHeld = 0;
    }
    /* The host clears a departing mod's marks anyway -- a mark left by a mod
     * that is no longer running is indistinguishable from the game being
     * wrong -- but doing it here means a reload starts from a clean map
     * rather than relying on that. */
    gearbox_render_clear();
    g_painted = 0;
}

GEARBOX_EXPORT("mod_draw_panel")
void mod_draw_panel(gearbox_panel panel, uint32_t w, uint32_t h) {
    (void)w;
    (void)h;
    if (panel != g_panel) return;

    ++g_phase;
    paint();

    /* THE MENUS TOO, not just the map. The accent is read at over a hundred
     * sites -- headings, highlights, selections, buttons -- so moving it is
     * the whole interface, for one call a frame.
     *
     * A quarter turn off the map's own phase, so the panels are never quite
     * the colour of the dots beside them; the same hue everywhere reads as a
     * tinted screenshot rather than as something happening. */
    if (g_intensity == 0) {
        if (g_accentHeld) {
            gearbox_set_theme_accent(g_playerAccent);
            g_accentHeld = 0;
        }
    } else {
        const uint32_t ui = (g_phase * 4u * g_intensity + 384u) % 1536u;
        gearbox_set_theme_accent(wheel(ui));
        g_accentHeld = 1;
    }

    /* Three numbers, not one, because when nothing appears on the map the
     * question is always WHICH link failed, and one number cannot say.
     *
     *   provinces 0   -> no world loaded, or Map was not granted
     *   painted 0     -> Render was not granted; the tint calls returned false
     *   held 0        -> the host took none of them
     *
     * A panel that only said "intensity 3" would leave every one of those
     * looking identical from the outside. */
    char line[96];
    uint32_t n = 0;
    n = append(line, n, S("intensity "));
    n += u32_to_str(g_intensity, line + n);
    gearbox_draw_text(panel, 12, 14, 0xFFFFFFFF, line, n);

    n = 0;
    n = append(line, n, S("provinces "));
    n += u32_to_str(gearbox_province_count(), line + n);
    n = append(line, n, S("  painted "));
    n += u32_to_str(g_painted, line + n);
    n = append(line, n, S("  held "));
    n += u32_to_str(gearbox_tint_count(), line + n);
    gearbox_draw_text(panel, 12, 30, 0xFF9AA0A6, line, n);

    if (gearbox_button(panel, 12, 52, 100, 28, S("calmer"))) {
        if (g_intensity > 0) --g_intensity;
    }
    if (gearbox_button(panel, 124, 52, 100, 28, S("stronger"))) {
        if (g_intensity < 5) ++g_intensity;
    }

    gearbox_draw_text(panel, 12, 92, 0xFF9AA0A6, S("0 restores the map and the theme"));
}
