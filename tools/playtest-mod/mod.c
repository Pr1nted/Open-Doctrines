/* Bridge Check -- the mod tools/playtest.sh loads to exercise Net and Audio.
 *
 * WHAT IT IS FOR
 *
 * The Net and Audio imports are the only two that leave the mod host: Net goes
 * out through NetHost/NetSession to another machine, Audio goes out to raylib.
 * Both are reached through a bridge the game installs, and a bridge that was
 * never installed fails SILENTLY -- every import answers "not a network game"
 * or "could not play", which is also the correct answer in singleplayer. That
 * is exactly the failure a unit test cannot see and a person can.
 *
 * So this puts the answers on the screen. Run it on three clients and the panel
 * says whether the bridge is live, what the host thinks this machine's peer id
 * is, and what has actually arrived from the others.
 *
 * WHAT TO LOOK FOR
 *
 *   peers  0 in singleplayer, 3 with a playing host and two joiners.
 *   self   different on each client, and NOT 0 on a playing host.
 *   host   1 on exactly one client.
 *   Ping   press on Alice; Bob and Carol's "last from" becomes Alice's id
 *          within a frame. Press on Bob; Alice and Carol see it, Bob does not
 *          see his own -- a broadcast reaches everyone ELSE, plus the host.
 *   Sound  plays locally only. It is here to prove the audio bridge is wired,
 *          not to be networked.
 */

#include "gearbox.h"

#define S(lit) lit, (uint32_t)(sizeof(lit) - 1)

static gearbox_panel g_panel;

static uint32_t g_sent;
static uint32_t g_received;
static int32_t  g_lastFrom = -1;
static uint32_t g_lastLen;
static char     g_lastText[64];

/* Freestanding wasm: there is no libc, so the few string things this needs are
 * here. Deliberately tiny -- this mod is a probe, not a demonstration. */
static uint32_t uToA(uint32_t v, char* out) {
    char tmp[12];
    uint32_t n = 0;
    if (!v) { out[0] = '0'; return 1; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (uint32_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

static uint32_t iToA(int32_t v, char* out) {
    if (v < 0) { out[0] = '-'; return 1 + uToA((uint32_t)(-v), out + 1); }
    return uToA((uint32_t)v, out);
}

/* label + number, into a caller's buffer. Returns the length. */
static uint32_t line(char* out, const char* label, uint32_t labelLen, int32_t v) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < labelLen; i++) out[n++] = label[i];
    n += iToA(v, out + n);
    return n;
}

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) {
    g_panel = gearbox_panel_register(S("Bridge Check"), 280, 210);
    gearbox_log(GEARBOX_LOG_INFO, S("bridge check loaded"));
    return 0;
}

GEARBOX_EXPORT("mod_unload")
void mod_unload(void) {
    g_sent = 0;
    g_received = 0;
    g_lastFrom = -1;
    g_lastLen = 0;
}

/* Drain whatever arrived. Called from the panel, which runs every frame while
 * the game is being played -- the queue is bounded, so a mod that never drains
 * loses the oldest, and this one drains fully each time. */
static void drain(void) {
    for (;;) {
        char     buf[64];
        uint32_t from = 0;
        const uint32_t n = gearbox_recv(buf, (uint32_t)sizeof(buf), (char*)&from);
        if (!n) return;
        g_received++;
        g_lastFrom = (int32_t)from;
        g_lastLen = n;
        const uint32_t keep = n < sizeof(g_lastText) ? n : (uint32_t)sizeof(g_lastText);
        for (uint32_t i = 0; i < keep; i++) g_lastText[i] = buf[i];
        g_lastLen = keep;
    }
}

GEARBOX_EXPORT("mod_draw_panel")
void mod_draw_panel(gearbox_panel panel, uint32_t w, uint32_t h) {
    drain();

    gearbox_draw_rect(panel, 0, 0, (int32_t)w, (int32_t)h, 0x101020FFu);

    char buf[80];
    uint32_t n;

    n = line(buf, S("peers: "), (int32_t)gearbox_peer_count());
    gearbox_draw_text(panel, 8, 8, 0xFFFFFFFFu, buf, n);

    n = line(buf, S("self:  "), (int32_t)gearbox_self_peer());
    gearbox_draw_text(panel, 8, 26, 0xFFFFFFFFu, buf, n);

    n = line(buf, S("host:  "), (int32_t)gearbox_is_host());
    gearbox_draw_text(panel, 8, 44, 0xFFFFFFFFu, buf, n);

    n = line(buf, S("sent:  "), (int32_t)g_sent);
    gearbox_draw_text(panel, 8, 68, 0xFF88FF88u, buf, n);

    n = line(buf, S("recvd: "), (int32_t)g_received);
    gearbox_draw_text(panel, 8, 86, 0xFF88FF88u, buf, n);

    n = line(buf, S("from:  "), g_lastFrom);
    gearbox_draw_text(panel, 8, 104, 0xFF88FF88u, buf, n);

    /* The payload itself, so a message that arrives mangled is visible as
     * mangled rather than merely counted. */
    if (g_lastLen) gearbox_draw_text(panel, 8, 122, 0xFFFFCC66u, g_lastText, g_lastLen);

    if (gearbox_button(panel, 8, 148, 120, 26, S("Ping all"))) {
        /* -1 is broadcast. The text is short and recognisable on the other
         * side; the host stamps this mod's id on it, not the mod. */
        if (gearbox_send((uint32_t)-1, S("ping from a peer"))) g_sent++;
    }

    if (gearbox_button(panel, 140, 148, 120, 26, S("Sound"))) {
        /* From this mod's OWN assets. A path outside the package is refused
         * rather than resolved, which is the point of routing it this way. */
        gearbox_play(S("ping.ogg"), 0.8f);
    }
}
