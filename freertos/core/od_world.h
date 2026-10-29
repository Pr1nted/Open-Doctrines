/* The world, for a machine with kilobytes rather than gigabytes.
 *
 * Two halves, and the split is the whole design:
 *
 *   od_prov0, od_adj, od_ctry0, od_ship0 -- CONST, generated from
 *   templeos/world.odw by tools/freertos_world.py. On a microcontroller they
 *   stay in flash and cost no RAM at all: names, areas, neighbours, and the
 *   numbers the game starts from.
 *
 *   od_w -- the numbers that change during a game: who owns what, how many
 *   people and men are where, unrest, the treasuries, the fleets and this
 *   turn's orders. About 30 bytes a province, so the 1,632-province world
 *   plays in under 64 KB of RAM -- inside what an STM32F405 on a CubeSat's
 *   on-board computer has to spare.
 *
 * Indices, not ids, everywhere: a province is its index in od_prov0 and a
 * country is its slot in od_ctry0. Owners in od_prov0 are country IDS, as the
 * world file stores them, and od_world_reset turns them into slots once.
 */
#ifndef OD_WORLD_H
#define OD_WORLD_H

#include <stdint.h>

#define OD_MAXP  2048
#define OD_MAXC   256
#define OD_MAXS   256
#define OD_NONE  0xFFFF          /* no province, no country */

typedef struct {
    uint32_t pid;                /* the province's id in the full game */
    uint32_t area;               /* pixels on the ORIGINAL raster: width uses it */
    uint32_t pop;
    uint32_t army;
    uint32_t adjo;               /* first neighbour in od_adj */
    uint16_t owner;              /* country ID */
    uint16_t inc;                /* tenths of a dollar a turn */
    uint8_t  adjn;
    uint8_t  fort;
    uint8_t  ind;
    uint8_t  res;                /* resource bits */
    uint8_t  port;               /* harbour level, 0 inland */
    char     name[16];
} od_prov_t;

typedef struct {
    uint16_t cid;
    uint16_t tre;                /* tenths of a dollar */
    char     iso[4];
    char     name[20];
} od_ctry_t;

typedef struct {
    uint16_t cid;
    uint16_t prov;
    uint8_t  kind;               /* 0 boat .. 6 frigate */
    uint8_t  hp;                 /* percent */
} od_ship_t;

extern const uint16_t od_np, od_nc, od_ns;
extern const od_prov_t od_prov0[];
extern const uint16_t  od_adj[];
extern const od_ctry_t od_ctry0[];
extern const od_ship_t od_ship0[];

/* What changes. One of these, statically allocated: no heap. */
typedef struct {
    uint16_t own[OD_MAXP];       /* country SLOT, or OD_NONE */
    uint32_t pop[OD_MAXP];
    uint32_t army[OD_MAXP];
    uint16_t inc[OD_MAXP];
    uint8_t  fort[OD_MAXP];
    uint8_t  ind[OD_MAXP];
    uint8_t  unrest[OD_MAXP];    /* 0..100 */
    uint8_t  align[OD_MAXP];     /* 0..100, 50 is indifference */
    /* orders, for the player's provinces, cleared each turn */
    uint32_t ord_recruit[OD_MAXP];
    uint16_t ord_attack[OD_MAXP];
    uint16_t ord_bombard[OD_MAXP];

    int32_t  tre[OD_MAXC];
    uint16_t enemy[OD_MAXC];     /* slot at war with, or OD_NONE */
    uint8_t  wartime[OD_MAXC];

    uint16_t sprov[OD_MAXS];
    uint8_t  shp[OD_MAXS];

    uint16_t me;                 /* the player's slot, or OD_NONE */
    uint16_t turn;
    uint8_t  pacify;             /* percent of income spent keeping order */
    uint32_t rng;
} od_world_t;

extern od_world_t od_w;

void od_world_reset(uint32_t seed);
int  od_slot_of_iso(const char *iso);         /* -1 if none */
int  od_find_province(const char *name);      /* by index or name prefix; -1 */
const char *od_iso(uint16_t slot);
static inline int od_neighbour(int ix, int k) { return od_adj[od_prov0[ix].adjo + k]; }

#endif
