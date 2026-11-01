#include "od_shell.h"
#include "od_rules.h"
#include "od_world.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const od_io_t *io;

/* The banner's first words. A port that is not a FreeRTOS board (Plan 9's
 * console program) says so here before od_shell_run. */
const char *od_edition = "FreeRTOS";

void od_printf(const char *fmt, ...)
{
    char buf[160];
    char out[200];
    size_t k = 0;
    int n, i;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = sizeof buf - 1;
    /* A serial terminal wants CR LF; a pipe on the host wants LF alone. */
    for (i = 0; i < n && k < sizeof out - 2; i++) {
        if (buf[i] == '\n' && io->echo) out[k++] = '\r';
        out[k++] = buf[i];
    }
    io->write(out, k);
}

/* 64-bit numbers by hand: newlib-nano's printf has no %llu, and a world's
 * army total does not fit in 32 bits. Grouped, because 31415926 men is a
 * number nobody reads at a glance. */
static const char *num(uint64_t v, char *buf)
{
    char tmp[32];
    int n = 0, k = 0, i;
    do {
        if (n && n % 3 == 0) tmp[k++] = ',';
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
        n++;
    } while (v);
    for (i = 0; i < k; i++) buf[i] = tmp[k - 1 - i];
    buf[k] = 0;
    return buf;
}

static const char *money(int64_t tenths, char *buf)
{
    char t[32];
    sprintf(buf, "%s.%d", num((uint64_t)(tenths / 10), t), (int)(tenths % 10));
    return buf;
}

static int need_me(void)
{
    if (od_w.me < od_nc) return 1;
    od_printf("pick a country first: play SWE (\"countries\" lists them)\n");
    return 0;
}

static int province_arg(const char *s)
{
    int p;
    if (!s) {
        od_printf("which province? a number from \"list\", or a name\n");
        return -1;
    }
    p = od_find_province(s);
    if (p < 0) od_printf("no province \"%s\"\n", s);
    return p;
}

static void cmd_help(void)
{
    /* A line at a time: od_printf formats into a line-sized buffer. */
    static const char *const lines[] = {
        "play ISO          govern a country (SWE, FRA, JPN ...)",
        "countries [N]     the N largest countries (10)",
        "status            your country",
        "list [PAGE]       your provinces, 20 a page",
        "show P            a province and its neighbours (P: number or name)",
        "recruit P N|max   raise men in P this turn",
        "attack FROM TO    assault TO with everything in FROM",
        "bombard FROM TO   shell TO from the fleet in FROM's harbour",
        "war ISO / peace   declare war; make peace",
        "pacify PCT        spend PCT% of income on order each turn",
        "end [N]           end the turn (N turns)",
        "log               what happened last turn",
        "hk                housekeeping: tasks, stacks, heap, uptime",
        "new [SEED]        start again",
        "quit              leave (QEMU exits; a real board idles)",
    };
    size_t i;
    for (i = 0; i < sizeof lines / sizeof lines[0]; i++) od_printf("  %s\n", lines[i]);
}

static void cmd_countries(const char *arg)
{
    static uint16_t count[OD_MAXC];
    static uint8_t shown[OD_MAXC];
    int n = arg ? atoi(arg) : 10;
    int c, i, r;
    if (n < 1) n = 1;
    for (c = 0; c < od_nc; c++) count[c] = 0;
    for (i = 0; i < od_np; i++)
        if (od_w.own[i] < od_nc) count[od_w.own[i]]++;
    /* n passes of a selection: n is small and this runs once a command */
    memset(shown, 0, sizeof shown);
    for (r = 0; r < n; r++) {
        int best = -1;
        uint32_t prov, inc;
        uint64_t men;
        char a[32], b[32];
        for (c = 0; c < od_nc; c++)
            if (!shown[c] && count[c] && (best < 0 || count[c] > count[best])) best = c;
        if (best < 0) break;
        shown[best] = 1;
        od_country_stats(best, &prov, &men, &inc);
        od_printf("%3d. %s %-19s %4lu prov %13s men  %10s in treasury%s\n", r + 1,
                  od_ctry0[best].iso, od_ctry0[best].name, (unsigned long)prov,
                  num(men, a), money(od_w.tre[best], b),
                  od_w.enemy[best] < od_nc ? "  AT WAR" : "");
    }
}

static void cmd_status(void)
{
    uint16_t me;
    uint32_t prov, inc;
    uint64_t men;
    char a[32], b[32], c[32];
    int c2;
    if (!need_me()) return;
    me = od_w.me;
    od_country_stats(me, &prov, &men, &inc);
    od_printf("%s (%s), turn %u\n", od_ctry0[me].name, od_ctry0[me].iso, od_w.turn);
    od_printf("  %lu provinces, %s men, income %s a turn, treasury %s\n",
              (unsigned long)prov, num(men, a), money(inc, b), money(od_w.tre[me], c));
    if (od_w.enemy[me] < od_nc)
        od_printf("  at war with %s\n", od_ctry0[od_w.enemy[me]].name);
    for (c2 = 0; c2 < od_nc; c2++)
        if (c2 != me && od_w.enemy[c2] == me)
            od_printf("  %s is at war with you\n", od_ctry0[c2].name);
    if (od_w.pacify) od_printf("  pacifying: %u%% of income\n", od_w.pacify);
}

static void cmd_list(const char *arg)
{
    int page, k = 0, shown = 0, i;
    char a[32], b[32];
    if (!need_me()) return;
    page = arg ? atoi(arg) : 1;
    if (page < 1) page = 1;
    for (i = 0; i < od_np; i++)
        if (od_w.own[i] == od_w.me) {
            if (k / 20 == page - 1) {
                od_printf("%5d %-15s %12s men %13s people  unrest %3u%%\n", i, od_prov0[i].name,
                          num(od_w.army[i], a), num(od_w.pop[i], b), od_w.unrest[i]);
                shown++;
            }
            k++;
        }
    if (!shown) od_printf("no page %d: you have %d provinces\n", page, k);
    else if (k > page * 20) od_printf("  (list %d for more)\n", page + 1);
}

static void cmd_show(const char *arg)
{
    int p = province_arg(arg);
    char a[32], b[32], c[32];
    const od_prov_t *pr;
    uint32_t fleet;
    int k;
    if (p < 0) return;
    pr = &od_prov0[p];
    od_printf("%d %s, held by %s\n", p, pr->name, od_iso(od_w.own[p]));
    od_printf("  %s people, %s men, frontage %s, fort %u, harbour %u\n",
              num(od_w.pop[p], a), num(od_w.army[p], b), num(od_width(p), c),
              od_w.fort[p], pr->port);
    od_printf("  income %s, unrest %u%%, alignment %u%%",
              money(od_w.inc[p], a), od_w.unrest[p], od_w.align[p]);
    fleet = od_fleet_at(p, od_w.own[p]);
    if (fleet) od_printf(", fleet strength %lu", (unsigned long)fleet);
    od_printf("\n  borders:\n");
    for (k = 0; k < pr->adjn; k++) {
        int nb = od_neighbour(p, k);
        od_printf("  %5d %-15s %s %12s men%s\n", nb, od_prov0[nb].name, od_iso(od_w.own[nb]),
                  num(od_w.army[nb], a),
                  od_at_war(od_w.own[p], od_w.own[nb]) ? "  (at war)" : "");
    }
}

static void cmd_recruit(const char *ps, const char *ns)
{
    int p;
    uint32_t cap, men;
    char a[32], b[32];
    if (!need_me()) return;
    p = province_arg(ps);
    if (p < 0) return;
    if (od_w.own[p] != od_w.me) {
        od_printf("%s is not yours\n", od_prov0[p].name);
        return;
    }
    cap = od_recruit_cap(p);
    men = (!ns || !strcmp(ns, "max")) ? cap : (uint32_t)strtoul(ns, NULL, 10);
    if (men > cap) men = cap;
    od_w.ord_recruit[p] = men;
    od_printf("ordered: raise %s men in %s, for %s\n", num(men, a), od_prov0[p].name,
              money(od_recruit_cost(p, men), b));
}

static int adjacent(int a, int b)
{
    int k;
    for (k = 0; k < od_prov0[a].adjn; k++)
        if (od_neighbour(a, k) == b) return 1;
    return 0;
}

static void cmd_attack(const char *fs, const char *ts, int bombard)
{
    int f, t;
    char a[32], b[32], c[32];
    if (!need_me()) return;
    f = province_arg(fs);
    if (f < 0) return;
    t = province_arg(ts);
    if (t < 0) return;
    if (od_w.own[f] != od_w.me) {
        od_printf("%s is not yours\n", od_prov0[f].name);
        return;
    }
    if (!adjacent(f, t)) {
        od_printf("%s does not border %s (\"show %d\")\n", od_prov0[f].name, od_prov0[t].name, f);
        return;
    }
    if (od_w.own[t] == od_w.me) {
        od_printf("%s is already yours\n", od_prov0[t].name);
        return;
    }
    if (bombard) {
        if (!od_fleet_at(f, od_w.me)) {
            od_printf("no fleet in %s's harbour\n", od_prov0[f].name);
            return;
        }
        od_w.ord_bombard[f] = (uint16_t)t;
        od_printf("ordered: shell %s from %s\n", od_prov0[t].name, od_prov0[f].name);
        return;
    }
    if (!od_at_war(od_w.me, od_w.own[t])) {
        od_printf("(declaring war on %s)\n", od_ctry0[od_w.own[t]].name);
        od_declare_war(od_w.me, od_w.own[t]);
    }
    od_w.ord_attack[f] = (uint16_t)t;
    od_printf("ordered: %s men from %s against %s defenders; frontage %s\n",
              num(od_w.army[f], a), od_prov0[f].name, num(od_w.army[t], b), num(od_width(t), c));
}

static void cmd_war(const char *iso)
{
    int s;
    if (!need_me()) return;
    s = iso ? od_slot_of_iso(iso) : -1;
    if (s < 0 || s == od_w.me) {
        od_printf("war on whom? an ISO code, e.g. war NOR\n");
        return;
    }
    od_declare_war(od_w.me, (uint16_t)s);
    od_printf("%s is at war with %s\n", od_ctry0[od_w.me].name, od_ctry0[s].name);
}

static void end_turns(const char *arg)
{
    int n = arg ? atoi(arg) : 1;
    int t, i;
    char a[32], b[32];
    if (n < 1) n = 1;
    for (t = 0; t < n; t++) {
        io->end_turn();
        if (od_w.me < od_nc) {
            uint32_t prov, inc;
            uint64_t men;
            od_country_stats(od_w.me, &prov, &men, &inc);
            od_printf("turn %u: %lu provinces, %s men, treasury %s, %lu battles world-wide\n",
                      od_w.turn, (unsigned long)prov, num(men, a), money(od_w.tre[od_w.me], b),
                      (unsigned long)od_battles);
            if (!prov) {
                od_printf("%s has fallen. \"new\" to start again.\n", od_ctry0[od_w.me].name);
                break;
            }
        } else {
            od_printf("turn %u: %lu battles world-wide\n", od_w.turn, (unsigned long)od_battles);
        }
        for (i = 0; i < od_nlog; i++) od_printf("  %s\n", od_log[i]);
    }
}

static char *next_word(char **s)
{
    char *w;
    while (**s == ' ' || **s == '\t') ++*s;
    if (!**s) return NULL;
    w = *s;
    while (**s && **s != ' ' && **s != '\t') ++*s;
    if (**s) *(*s)++ = 0;
    return w;
}

int od_shell_line(char *line)
{
    char *s = line;
    char *cmd = next_word(&s);
    char *a1, *a2;
    int i;
    if (!cmd) return 1;
    a1 = next_word(&s);
    a2 = next_word(&s);

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) cmd_help();
    else if (!strcmp(cmd, "play")) {
        int c = a1 ? od_slot_of_iso(a1) : -1;
        if (c < 0) od_printf("no country \"%s\": \"countries 200\" lists them\n", a1 ? a1 : "");
        else {
            od_w.me = (uint16_t)c;
            cmd_status();
        }
    }
    else if (!strcmp(cmd, "countries")) cmd_countries(a1);
    else if (!strcmp(cmd, "status")) cmd_status();
    else if (!strcmp(cmd, "list")) cmd_list(a1);
    else if (!strcmp(cmd, "show")) {
        /* the one command whose argument may be a name with spaces in it */
        char name[64];
        if (a1 && a2) {
            while (*s == ' ') s++;
            snprintf(name, sizeof name, "%s %s%s%s", a1, a2, *s ? " " : "", s);
            a1 = name;
        }
        cmd_show(a1);
    }
    else if (!strcmp(cmd, "recruit")) cmd_recruit(a1, a2);
    else if (!strcmp(cmd, "attack")) cmd_attack(a1, a2, 0);
    else if (!strcmp(cmd, "bombard")) cmd_attack(a1, a2, 1);
    else if (!strcmp(cmd, "war")) cmd_war(a1);
    else if (!strcmp(cmd, "peace")) {
        if (need_me()) {
            od_ceasefire(od_w.me);
            od_printf("at peace\n");
        }
    }
    else if (!strcmp(cmd, "pacify")) {
        int v = a1 ? atoi(a1) : 0;
        od_w.pacify = (uint8_t)(v < 0 ? 0 : v > 50 ? 50 : v);
        od_printf("pacifying with %u%% of income\n", od_w.pacify);
    }
    else if (!strcmp(cmd, "end")) end_turns(a1);
    else if (!strcmp(cmd, "log")) {
        if (!od_nlog) od_printf("nothing of yours happened\n");
        for (i = 0; i < od_nlog; i++) od_printf("  %s\n", od_log[i]);
    }
    else if (!strcmp(cmd, "hk")) {
        if (io->housekeeping) io->housekeeping();
        else od_printf("no housekeeping on this board\n");
    }
    else if (!strcmp(cmd, "new")) {
        od_world_reset(a1 ? (uint32_t)strtoul(a1, NULL, 10) : 0);
        od_nlog = 0;
        od_printf("a new world: %u provinces, %u countries\n", od_np, od_nc);
    }
    else if (!strcmp(cmd, "quit")) return 0;
    else od_printf("\"%s\"? \"help\" lists the commands\n", cmd);
    return 1;
}

void od_shell_attach(const od_io_t *the_io)
{
    io = the_io;
}

void od_shell_run(const od_io_t *the_io)
{
    char line[96];
    od_shell_attach(the_io);
    od_printf("\nOpen Doctrines on %s, %s\n", od_edition, io->board);
    od_printf("%u provinces, %u countries, %u hulls. \"help\" for commands.\n",
              od_np, od_nc, od_ns);
    for (;;) {
        size_t n = 0;
        int c;
        od_printf("> ");
        for (;;) {
            c = io->getch();
            if (c < 0) {
                if (n == 0) {
                    od_printf("\n");
                    return;
                }
                break;
            }
            if (c == '\r' || c == '\n') {
                if (io->echo) io->write("\r\n", 2);
                break;
            }
            if (c == 8 || c == 127) {
                if (n > 0) {
                    n--;
                    if (io->echo) io->write("\b \b", 3);
                }
                continue;
            }
            if (c < 32 || n >= sizeof line - 1) continue;
            line[n++] = (char)c;
            if (io->echo) {
                char ch = (char)c;
                io->write(&ch, 1);
            }
        }
        line[n] = 0;
        if (!od_shell_line(line)) return;
        if (c < 0) return;
    }
}
