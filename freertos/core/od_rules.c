#include "od_rules.h"
#include "od_world.h"

#include <stdarg.h>
#include <stdio.h>

/* The desktop game's combat constants, generated (see od_rules.h). */
#include "Sync.HH"

/* Everything else is Rules.HC's own, and named as it names them. */
#define R_FORT_DEF_PCT      10      /* per fort level */
#define R_RECRUIT_CAP_PCT   20      /* of a province's people, per turn */
#define R_GROWTH_PER_MILLE   5      /* population, per turn */
#define R_AI_ODDS            2      /* attack at this multiple of the defence */
#define R_WAR_MAX_TURNS     12      /* then exhaustion, and peace */
#define R_WAR_ODDS         600      /* 1 in this many per country per turn */
#define R_UPKEEP_PER_MAN  20000     /* men per tenth of a dollar a turn */
#define R_SHIP_UPKEEP        2      /* tenths of a dollar, per hull per turn */
#define R_UNREST_CONQUEST   40
#define R_UNREST_FRONT       2      /* per turn, next to a war */
#define R_UNREST_HOSTILE     3      /* per turn, from a disaffected people */
#define R_ALIGN_LOW         30

/* boat, destroyer, cruiser, submarine, carrier, battleship, frigate */
static const uint8_t r_shipstr[7] = {1, 4, 6, 5, 10, 8, 3};

char     od_log[OD_LOG_LINES][OD_LOG_LEN];
int      od_nlog;
uint32_t od_battles;

/* xorshift32: the same game for the same seed on every board, which is what
 * makes a transcript a test. */
uint32_t od_rand(void)
{
    uint32_t x = od_w.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return od_w.rng = x;
}

static void rlog(const char *fmt, ...)
{
    va_list ap;
    if (od_nlog >= OD_LOG_LINES) return;
    va_start(ap, fmt);
    vsnprintf(od_log[od_nlog++], OD_LOG_LEN, fmt, ap);
    va_end(ap);
}

static void clear_orders(void)
{
    int i;
    for (i = 0; i < od_np; i++) {
        od_w.ord_recruit[i] = 0;
        od_w.ord_attack[i] = OD_NONE;
        od_w.ord_bombard[i] = OD_NONE;
    }
}

uint32_t od_fleet_at(int ix, uint16_t slot)
{
    uint32_t n = 0;
    int i;
    if (slot >= od_nc) return 0;
    for (i = 0; i < od_ns; i++)
        if (od_w.sprov[i] == ix && od_w.shp[i] > 0 && od_ship0[i].cid == od_ctry0[slot].cid)
            n += (uint32_t)r_shipstr[od_ship0[i].kind % 7] * od_w.shp[i] / 100;
    return n;
}

static uint32_t hulls_of(uint16_t slot)
{
    uint32_t n = 0;
    int i;
    for (i = 0; i < od_ns; i++)
        if (od_w.shp[i] > 0 && od_ship0[i].cid == od_ctry0[slot].cid) n++;
    return n;
}

int od_at_war(uint16_t a, uint16_t b)
{
    if (a >= od_nc || b >= od_nc) return 0;
    return od_w.enemy[a] == b || od_w.enemy[b] == a;
}

void od_declare_war(uint16_t a, uint16_t b)
{
    if (a >= od_nc) return;
    od_w.enemy[a] = b;
    od_w.wartime[a] = 0;
}

void od_ceasefire(uint16_t a)
{
    uint16_t b;
    if (a >= od_nc) return;
    b = od_w.enemy[a];
    od_w.enemy[a] = OD_NONE;
    od_w.wartime[a] = 0;
    if (b < od_nc && od_w.enemy[b] == a) {
        od_w.enemy[b] = OD_NONE;
        od_w.wartime[b] = 0;
    }
}

static uint64_t strength(uint16_t slot)
{
    uint64_t n = 0;
    int i;
    for (i = 0; i < od_np; i++)
        if (od_w.own[i] == slot) n += od_w.army[i];
    return n;
}

/* ── THE FRONTAGE ──
 *
 * How many men can be on the line at once: above it, extra men do not fight.
 * The rule that makes this game itself rather than numbers-beat-numbers. */
uint32_t od_width(int ix)
{
    uint64_t w = (uint64_t)od_prov0[ix].area * SY_WIDTH_PER_AREA;
    if (w < SY_WIDTH_MIN) w = SY_WIDTH_MIN;
    if (od_w.fort[ix] > 0) {
        int narrow = 100 - od_w.fort[ix] * SY_FORT_NARROW_PCT;
        if (narrow < 25) narrow = 25;          /* a fort narrows, it does not seal */
        w = w * narrow / 100;
    }
    if (w < 1) w = 1;
    return w > UINT32_MAX ? UINT32_MAX : (uint32_t)w;
}

static uint32_t sub_floor(uint32_t a, uint64_t b) { return b >= a ? 0 : (uint32_t)(a - b); }

/* One assault. Losses are the force each side actually overcame. */
int od_assault(int from, int to, uint32_t men)
{
    uint32_t wid, fortmul;
    uint64_t eatk, edef, def;
    if (from < 0 || to < 0 || men == 0) return 0;
    wid = od_width(to);
    eatk = men < wid ? men : wid;
    edef = od_w.army[to] < wid ? od_w.army[to] : wid;
    fortmul = 100 + od_w.fort[to] * R_FORT_DEF_PCT;
    def = edef * fortmul / 100;
    /* A fleet in the harbour fights for it, worth men rather than a multiplier. */
    def += (uint64_t)od_fleet_at(to, od_w.own[to]) * 2000;

    if (eatk > def) {
        int u;
        od_w.army[from] = sub_floor(od_w.army[from], men);
        od_w.army[to] = sub_floor(men, def);
        od_w.own[to] = od_w.own[from];
        /* Taking a province is not the same as holding it. */
        u = od_w.unrest[to] + R_UNREST_CONQUEST;
        od_w.unrest[to] = u > 100 ? 100 : u;
        od_w.align[to] = od_w.align[to] > 20 ? od_w.align[to] - 20 : 0;
        return 1;
    }
    od_w.army[from] = sub_floor(od_w.army[from], eatk);
    od_w.army[to] = sub_floor(od_w.army[to], eatk * 100 / fortmul);
    return 0;
}

/* ── THE OPPONENT ──
 *
 * Deliberately legible rather than clever: raise men where the people are,
 * and attack a neighbour you are at war with and heavily outnumber. */
static void ai_turn(uint16_t slot)
{
    int i, k, best;
    uint32_t bestpop;
    if (od_w.enemy[slot] != OD_NONE) {
        if (++od_w.wartime[slot] > R_WAR_MAX_TURNS) {
            od_w.enemy[slot] = OD_NONE;
            od_w.wartime[slot] = 0;
        }
    } else if (!(od_rand() % R_WAR_ODDS)) {
        uint64_t mine = strength(slot);
        uint16_t cand = OD_NONE;
        for (i = 0; i < od_np && cand == OD_NONE; i++)
            if (od_w.own[i] == slot)
                for (k = 0; k < od_prov0[i].adjn; k++) {
                    int nb = od_neighbour(i, k);
                    uint16_t o = od_w.own[nb];
                    if (o != slot && o != OD_NONE && mine > strength(o) * 3 / 2) {
                        cand = o;
                        break;
                    }
                }
        if (cand != OD_NONE) {
            od_w.enemy[slot] = cand;
            od_w.wartime[slot] = 0;
        }
    }

    /* Raise men in the most populous province that can pay for them. */
    best = -1;
    bestpop = 0;
    for (i = 0; i < od_np; i++)
        if (od_w.own[i] == slot && od_w.pop[i] > bestpop) {
            best = i;
            bestpop = od_w.pop[i];
        }
    if (best >= 0) {
        uint32_t men = od_recruit_cap(best);
        int32_t cost;
        if (men > 200000) men = 200000;
        cost = men / 1000;
        if (cost > 0 && od_w.tre[slot] >= cost) {
            od_w.tre[slot] -= cost;
            od_w.army[best] += men;
        }
    }

    for (i = 0; i < od_np; i++)
        if (od_w.own[i] == slot && od_w.army[i] > 0)
            for (k = 0; k < od_prov0[i].adjn; k++) {
                int nb = od_neighbour(i, k);
                if (od_w.own[nb] != slot && od_at_war(slot, od_w.own[nb])) {
                    uint64_t tgt = (uint64_t)od_w.army[nb] * R_AI_ODDS;
                    if (tgt < 1) tgt = 1;
                    if (od_w.army[i] > tgt && od_w.army[i] > 10000) {
                        if (od_assault(i, nb, od_w.army[i])) od_battles++;
                        break;             /* one assault per province per turn */
                    }
                }
            }
}

uint32_t od_recruit_cap(int ix)
{
    return (uint32_t)((uint64_t)od_w.pop[ix] * R_RECRUIT_CAP_PCT / 100);
}

/* Men are dearer where the province resents you. */
uint32_t od_recruit_cost(int ix, uint32_t men)
{
    return men / 1000 * (100 + od_w.unrest[ix]) / 100;
}

/* ── A TURN ──
 *
 * Income, then the player's orders, then everybody else, then the people. */
void od_turn(void)
{
    static uint64_t men_of[OD_MAXC];
    uint16_t me, c;
    int i, k;

    od_nlog = 0;
    od_battles = 0;
    me = od_w.me;

    /* Income, less what unrest costs: a province in disorder does not pay. */
    for (i = 0; i < od_np; i++)
        if (od_w.own[i] < od_nc)
            od_w.tre[od_w.own[i]] += (int32_t)od_w.inc[i] * (100 - od_w.unrest[i]) / 100;

    /* What the standing army and the fleet cost to keep. Summed per country
     * in one pass over the provinces, where Rules.HC goes over them once per
     * country: 183 x 1,632 is a third of a million steps a turn, which a
     * TempleOS PC does not notice and a 168 MHz Cortex-M4 does. */
    for (c = 0; c < od_nc; c++) men_of[c] = 0;
    for (i = 0; i < od_np; i++)
        if (od_w.own[i] < od_nc) men_of[od_w.own[i]] += od_w.army[i];
    for (c = 0; c < od_nc; c++) {
        int64_t cost = (int64_t)(men_of[c] / R_UPKEEP_PER_MAN) + hulls_of(c) * R_SHIP_UPKEEP;
        int64_t t = od_w.tre[c] - cost;
        od_w.tre[c] = t < 0 ? 0 : (int32_t)t;
    }

    if (me < od_nc) {
        for (i = 0; i < od_np; i++)
            if (od_w.ord_recruit[i] > 0 && od_w.own[i] == me) {
                uint32_t men = od_w.ord_recruit[i];
                uint32_t cap = od_recruit_cap(i);
                int32_t cost;
                if (men > cap) men = cap;
                cost = od_recruit_cost(i, men);
                if (cost > 0 && od_w.tre[me] >= cost) {
                    od_w.tre[me] -= cost;
                    od_w.army[i] += men;
                    rlog("raised %lu men in %s", (unsigned long)men, od_prov0[i].name);
                }
            }

        /* Ships shell what is next to their harbour. They kill; they take no ground. */
        for (i = 0; i < od_np; i++)
            if (od_w.ord_bombard[i] != OD_NONE && od_w.own[i] == me) {
                int s = od_w.ord_bombard[i];
                uint32_t men = od_fleet_at(i, me) * 1500;
                if (men > od_w.army[s]) men = od_w.army[s];
                od_w.army[s] -= men;
                rlog("shelled %s, %lu dead", od_prov0[s].name, (unsigned long)men);
            }

        for (i = 0; i < od_np; i++)
            if (od_w.ord_attack[i] != OD_NONE && od_w.own[i] == me && od_w.army[i] > 0) {
                int t = od_w.ord_attack[i];
                if (od_assault(i, t, od_w.army[i]))
                    rlog("took %s", od_prov0[t].name);
                else
                    rlog("assault on %s failed, %lu defenders left",
                         od_prov0[t].name, (unsigned long)od_w.army[t]);
            }
    }

    for (c = 0; c < od_nc; c++)
        if (c != me) ai_turn(c);

    /* Order, and the cost of keeping it: spread over the provinces that need it. */
    if (me < od_nc && od_w.pacify > 0) {
        uint32_t restless = 0, income = 0;
        for (i = 0; i < od_np; i++)
            if (od_w.own[i] == me) {
                income += od_w.inc[i];
                if (od_w.unrest[i] > 0) restless++;
            }
        if (restless > 0) {
            int32_t cost = income * od_w.pacify / 100;
            uint32_t each;
            od_w.tre[me] = od_w.tre[me] > cost ? od_w.tre[me] - cost : 0;
            each = (uint32_t)cost * 4 / (restless * 10);
            if (each < 1) each = 1;
            for (i = 0; i < od_np; i++)
                if (od_w.own[i] == me && od_w.unrest[i] > 0)
                    od_w.unrest[i] = od_w.unrest[i] > each ? od_w.unrest[i] - each : 0;
        }
    }

    /* Unrest from the front and from disaffection. */
    for (i = 0; i < od_np; i++) {
        int u = od_w.unrest[i];
        if (od_w.align[i] < R_ALIGN_LOW) u += R_UNREST_HOSTILE;
        for (k = 0; k < od_prov0[i].adjn; k++) {
            int nb = od_neighbour(i, k);
            if (od_w.own[nb] != od_w.own[i] && od_at_war(od_w.own[i], od_w.own[nb])) {
                u += R_UNREST_FRONT;
                break;
            }
        }
        od_w.unrest[i] = u > 100 ? 100 : u;
    }

    for (i = 0; i < od_np; i++) {
        uint64_t p = od_w.pop[i] + (uint64_t)od_w.pop[i] * R_GROWTH_PER_MILLE / 1000;
        od_w.pop[i] = p > UINT32_MAX ? UINT32_MAX : (uint32_t)p;
    }

    clear_orders();
    od_w.turn++;
}

void od_country_stats(uint16_t slot, uint32_t *prov, uint64_t *men, uint32_t *inc)
{
    int i;
    *prov = 0;
    *men = 0;
    *inc = 0;
    for (i = 0; i < od_np; i++)
        if (od_w.own[i] == slot) {
            ++*prov;
            *men += od_w.army[i];
            *inc += od_w.inc[i];
        }
}
