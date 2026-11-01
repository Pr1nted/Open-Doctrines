#include "od_world.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

od_world_t od_w;

static uint16_t slot_of_cid(uint16_t cid)
{
    uint16_t i;
    for (i = 0; i < od_nc; i++)
        if (od_ctry0[i].cid == cid) return i;
    return OD_NONE;
}

void od_world_reset(uint32_t seed)
{
    int i;
    memset(&od_w, 0, sizeof od_w);
    for (i = 0; i < od_np; i++) {
        const od_prov_t *p = &od_prov0[i];
        od_w.own[i]  = slot_of_cid(p->owner);
        od_w.pop[i]  = p->pop;
        od_w.army[i] = p->army;
        od_w.inc[i]  = p->inc;
        od_w.fort[i] = p->fort;
        od_w.ind[i]  = p->ind;
        od_w.align[i] = 50;
        od_w.ord_attack[i] = OD_NONE;
        od_w.ord_bombard[i] = OD_NONE;
    }
    for (i = 0; i < od_nc; i++) {
        od_w.tre[i] = od_ctry0[i].tre;
        od_w.enemy[i] = OD_NONE;
    }
    for (i = 0; i < od_ns; i++) {
        od_w.sprov[i] = od_ship0[i].prov;
        od_w.shp[i] = od_ship0[i].hp;
    }
    od_w.me = OD_NONE;
    /* xorshift32 has one state it can never leave: zero. */
    od_w.rng = seed ? seed : 0x0D0C7121u;
}

int od_slot_of_iso(const char *iso)
{
    int i;
    for (i = 0; i < od_nc; i++) {
        const char *a = od_ctry0[i].iso;
        int k = 0;
        while (k < 3 && a[k] && iso[k] && toupper((unsigned char)iso[k]) == a[k]) k++;
        if (k == 3 || (a[k] == 0 && iso[k] == 0)) return i;
    }
    return -1;
}

int od_find_province(const char *s)
{
    size_t n, k;
    int i;
    if (isdigit((unsigned char)s[0])) {
        char *end;
        long v = strtol(s, &end, 10);
        return (*end == 0 && v >= 0 && v < od_np) ? (int)v : -1;
    }
    n = strlen(s);
    if (n == 0) return -1;
    /* an exact name first, so "Paris" does not lose to "Paris Basin" */
    for (i = 0; i < od_np; i++) {
        const char *a = od_prov0[i].name;
        if (strlen(a) == n) {
            k = 0;
            while (k < n && tolower((unsigned char)a[k]) == tolower((unsigned char)s[k])) k++;
            if (k == n) return i;
        }
    }
    for (i = 0; i < od_np; i++) {
        const char *a = od_prov0[i].name;
        k = 0;
        while (k < n && a[k] && tolower((unsigned char)a[k]) == tolower((unsigned char)s[k])) k++;
        if (k == n) return i;
    }
    return -1;
}

const char *od_iso(uint16_t slot)
{
    return slot < od_nc ? od_ctry0[slot].iso : "--";
}
