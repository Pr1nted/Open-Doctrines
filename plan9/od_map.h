/* Where each province is, for the libdraw map: its centre on the world
 * raster (od_map_w x od_map_h), generated with the world data by
 * tools/freertos_world.py --map. Indexed like od_prov0. */
#ifndef OD_MAP_H
#define OD_MAP_H

#include <stdint.h>

extern const uint16_t od_map_w, od_map_h;
extern const uint16_t od_cx[], od_cy[];

#endif
