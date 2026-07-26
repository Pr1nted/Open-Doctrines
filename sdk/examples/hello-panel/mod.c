/* Hello Panel — a complete, working OpenDoctrines mod.
 *
 * Shows a panel with the turn number, how many countries are alive, and the
 * treasury of whichever country you step to with the button. Uses three
 * capabilities: Core (log, env), UI (panel), GameState.Read (the world).
 *
 * Freestanding: no libc, no allocator, no startup code. Everything it needs is
 * in this file, which is the point -- a Tier 1 mod is a couple of kilobytes.
 *
 * Build:  ./build.sh          (produces hello-panel.odmod)
 */

#include "gearbox.h"

#define S(lit) lit, (uint32_t)(sizeof(lit) - 1)

static gearbox_env_t g_env;
static gearbox_panel g_panel;
static uint32_t      g_cursor;      /* which country we are showing */

/* --- tiny formatting helpers, since there is no libc ------------------- */

static uint32_t u64_to_str(uint64_t v, char* out) {
    char tmp[24];
    uint32_t n = 0;
    if (v == 0) { out[0] = '0'; return 1; }
    while (v > 0 && n < sizeof tmp) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (uint32_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

static uint32_t append(char* dst, uint32_t at, const char* src, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) dst[at + i] = src[i];
    return at + len;
}

/* --- lifecycle ---------------------------------------------------------- */

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) {
    g_env.size = sizeof g_env;
    gearbox_env(&g_env);

    if (g_env.is_headless) {
        /* A training run has no renderer. Registering a panel would be a
         * no-op anyway, but skipping it makes the intent explicit. */
        gearbox_log(GEARBOX_LOG_INFO, S("hello-panel: headless, no UI"));
        return 0;
    }

    g_panel = gearbox_panel_register(S("Hello Panel"), 280, 150);
    if (g_panel == 0) {
        /* UI was declared but revoked, or we hit the panel limit. Not fatal:
         * degrade rather than trap. */
        gearbox_log(GEARBOX_LOG_WARN, S("hello-panel: no panel, running quiet"));
    }
    return 0;   /* non-zero would refuse the load */
}

GEARBOX_EXPORT("mod_unload")
void mod_unload(void) {
    g_cursor = 0;
}

/* --- the panel ---------------------------------------------------------- */

GEARBOX_EXPORT("mod_draw_panel")
void mod_draw_panel(gearbox_panel panel, uint32_t w, uint32_t h) {
    char line[256];
    uint32_t n;

    (void)h;
    gearbox_draw_rect(panel, 0, 0, (int32_t)w, 1, 0x3C3C5AFFu);

    /* Turn number */
    n = append(line, 0, S("Turn "));
    n += u64_to_str(gearbox_turn_number(), line + n);
    gearbox_draw_text(panel, 8, 8, 0xFFFFFFFFu, line, n);

    /* Country count */
    uint32_t count = gearbox_country_count();
    n = append(line, 0, S("Countries: "));
    n += u64_to_str(count, line + n);
    gearbox_draw_text(panel, 8, 28, 0xB4B4C8FFu, line, n);

    if (count == 0) {
        gearbox_draw_text(panel, 8, 52, 0x9696A0FFu, S("No world loaded"));
        return;
    }
    if (g_cursor >= count) g_cursor = 0;

    gearbox_country c = gearbox_country_at(g_cursor);
    if (c != GEARBOX_INVALID) {
        /* Two-call sizing: ask for the length, then fill what fits. */
        uint32_t need = gearbox_country_name(c, 0, 0);
        char name[64];
        uint32_t got = gearbox_country_name(c, name, sizeof name);
        if (got > sizeof name) got = sizeof name;   /* truncated, not an error */
        (void)need;

        gearbox_draw_text(panel, 8, 52, 0xFFFFFFFFu, name, got);

        n = append(line, 0, S("Provinces: "));
        n += u64_to_str(gearbox_country_province_count(c), line + n);
        gearbox_draw_text(panel, 8, 72, 0xB4B4C8FFu, line, n);

        /* Treasury comes back as a double; print the integer part. */
        double t = gearbox_country_treasury(c);
        int negative = t < 0;
        if (negative) t = -t;
        n = append(line, 0, S("Treasury: "));
        if (negative) n = append(line, n, S("-"));
        n += u64_to_str((uint64_t)t, line + n);
        gearbox_draw_text(panel, 8, 92, 0xB4B4C8FFu, line, n);
    }

    if (gearbox_button(panel, 8, 116, 120, 24, S("Next country")))
        g_cursor++;
}
