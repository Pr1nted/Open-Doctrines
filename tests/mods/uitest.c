/* Fixture mod exercising the UI capability.
 *
 * Also tries to draw into a panel it does not own, which the host must ignore.
 */

#include "gearbox.h"

#define S(lit) lit, (uint32_t)(sizeof(lit) - 1)

static gearbox_panel g_panel;
static uint32_t      g_clicks;

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) {
    g_panel = gearbox_panel_register(S("UI Test"), 200, 100);
    gearbox_log(GEARBOX_LOG_INFO, S("uitest loaded"));
    return 0;
}

GEARBOX_EXPORT("mod_unload")
void mod_unload(void) { g_clicks = 0; }

GEARBOX_EXPORT("mod_draw_panel")
void mod_draw_panel(gearbox_panel panel, uint32_t w, uint32_t h) {
    gearbox_draw_rect(panel, 0, 0, (int32_t)w, (int32_t)h, 0x101020FFu);
    gearbox_draw_text(panel, 8, 8, 0xFFFFFFFFu, S("hello"));
    if (gearbox_button(panel, 10, 40, 100, 24, S("Click"))) g_clicks++;
}

GEARBOX_EXPORT("t_panel")
uint32_t t_panel(void) { return g_panel; }

GEARBOX_EXPORT("t_clicks")
uint32_t t_clicks(void) { return g_clicks; }

/* A handle this mod was never given. The host must reject it on ownership
 * rather than trusting the number. */
GEARBOX_EXPORT("t_foreign_draw")
void t_foreign_draw(void) {
    gearbox_draw_text(9999u, 0, 0, 0xFFFFFFFFu, S("should not appear"));
}
