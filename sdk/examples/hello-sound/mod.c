/* Hello Sound -- the smallest complete audio mod, and the thing that proves
 * the mod audio path works end to end.
 *
 * Three capabilities: Core (log, env), UI (one panel) and Audio. It plays a
 * chime from its own assets at load and whenever the button is pressed, and
 * the panel reports the handle and whether that handle is still sounding.
 *
 * WHY THE PANEL REPORTS is_playing. Audio is the one capability whose result
 * nobody can see in a screenshot or a test. A handle that comes back non-zero
 * proves the host decoded the file and accepted it; is_playing going true and
 * then false on its own proves the mixer actually advanced through the
 * samples rather than accepting them and doing nothing.
 *
 * Freestanding: no libc, no allocator.
 *
 * Build:  ./build.sh          (produces hello-sound.odmod)
 */

#include "gearbox.h"

#define S(lit) lit, (uint32_t)(sizeof(lit) - 1)

static gearbox_env_t g_env;
static gearbox_panel g_panel;
static uint32_t      g_handle;      /* the last sound we started */
static uint32_t      g_plays;       /* how many times we asked   */

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

static void chime(void) {
    /* Full volume as far as this mod is concerned; the host multiplies by the
     * player's effects and master sliders, so this is "as loud as they
     * allowed" and not "as loud as possible". */
    /* "chime.wav", not "data/chime.wav". The file lives in data/ in the
     * source tree and in the archive, but the loader presents assets with
     * that prefix stripped -- odmod-check lists this one as `chime.wav`.
     * Shipping it also needs the Assets module alongside Audio, or the
     * package refuses to hand the bytes over and play() returns 0. */
    g_handle = gearbox_play(S("chime.wav"), 1.0f);
    ++g_plays;
    if (g_handle == 0)
        gearbox_log(GEARBOX_LOG_WARN, S("hello-sound: the host refused the chime"));
}

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) {
    g_env.size = sizeof g_env;
    gearbox_env(&g_env);

    /* Headless has no device. play() would return 0 anyway; not asking makes
     * the intent explicit and keeps a training run silent. */
    if (g_env.is_headless) {
        gearbox_log(GEARBOX_LOG_INFO, S("hello-sound: headless, staying quiet"));
        return 0;
    }

    chime();
    g_panel = gearbox_panel_register(S("Hello Sound"), 260, 120);
    if (g_panel == 0)
        gearbox_log(GEARBOX_LOG_WARN, S("hello-sound: no panel; the chime still played"));
    return 0;
}

GEARBOX_EXPORT("mod_unload")
void mod_unload(void) {
    /* The host stops a departing mod's sounds anyway. Doing it here means a
     * reload does not overlap the old chime with the new one. */
    if (g_handle) gearbox_stop(g_handle);
    g_handle = 0;
}

GEARBOX_EXPORT("mod_draw_panel")
void mod_draw_panel(gearbox_panel panel, uint32_t w, uint32_t h) {
    (void)w;
    (void)h;
    if (panel != g_panel) return;

    char line[96];
    uint32_t n = 0;
    n = append(line, n, S("plays "));
    n += u32_to_str(g_plays, line + n);
    n = append(line, n, S("   handle "));
    n += u32_to_str(g_handle, line + n);
    gearbox_draw_text(panel, 12, 14, 0xFFFFFFFF, line, n);

    /* The live one. It should read "sounding" for about six tenths of a
     * second after each press and "silent" the rest of the time. A handle
     * that never goes quiet means the mixer is not advancing; one that is
     * never sounding means nothing reached the bus. */
    const uint32_t live = g_handle ? gearbox_is_playing(g_handle) : 0;
    gearbox_draw_text(panel, 12, 32, live ? 0xFF6ADF6A : 0xFF9AA0A6,
                      live ? "sounding" : "silent", live ? 8u : 6u);

    if (gearbox_button(panel, 12, 56, 100, 28, S("play"))) chime();
    if (gearbox_button(panel, 124, 56, 100, 28, S("stop"))) {
        if (g_handle) gearbox_stop(g_handle);
        g_handle = 0;
    }
}
