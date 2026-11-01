/* Open Doctrines' rules, in C99, for FreeRTOS.
 *
 * A port of templeos/Rules.HC -- the same turn, in the same order, with the
 * same constants -- and the combat numbers are not copied at all: they come
 * from templeos/Sync.HH, which tools/templeos_sync.py generates from the
 * desktop game's src/Game.h and the suite holds to it. Retune combat width
 * there and all three builds move together.
 *
 * What the TempleOS edition has and this one does not: research and policies
 * (game.odd is 43 KB of text this build has no room to parse), artillery
 * types, pacts, and a sea to sail fleets across. Fleets are here, in their
 * harbours: they defend them and bombard what is next to them.
 */
#ifndef OD_RULES_H
#define OD_RULES_H

#include <stdint.h>

#define OD_LOG_LINES 8
#define OD_LOG_LEN   72

extern char     od_log[OD_LOG_LINES][OD_LOG_LEN];
extern int      od_nlog;
extern uint32_t od_battles;        /* assaults resolved in the last turn */

uint32_t od_rand(void);
uint32_t od_width(int ix);
int      od_assault(int from, int to, uint32_t men);   /* 1 if it changed hands */
uint32_t od_fleet_at(int ix, uint16_t slot);
int      od_at_war(uint16_t a, uint16_t b);
void     od_declare_war(uint16_t a, uint16_t b);
void     od_ceasefire(uint16_t a);
uint32_t od_recruit_cap(int ix);
uint32_t od_recruit_cost(int ix, uint32_t men);
void     od_turn(void);
void     od_country_stats(uint16_t slot, uint32_t *prov, uint64_t *men, uint32_t *inc);

#endif
