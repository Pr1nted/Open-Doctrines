#pragma once

// A world loaded at a fraction of its resolution, for machines that cannot
// hold it whole.
//
// A world's two rasters, provinces.png and land_sea.png, are 8192x4096: 33
// million pixels, and everything per-pixel the game builds from them -- the
// province index, population lookups, the political texture -- scales with
// that. Compact as the game already holds it (ProvinceMap's 16-bit index),
// a world still needs ~270 MB, which is a PC's or a phone's to give and not a
// Nintendo 3DS's (64-178 MB for an application) or a Wii's (88 MB in all).
//
// With a divisor of N the rasters are sampled every Nth pixel as they are
// decoded -- 4 gives 2048x1024, 16 times fewer pixels -- and nothing else
// changes: every rule in the game is per PROVINCE, keyed by id, and the one
// quantity measured in pixels that feeds the rules, a province's land area, is
// kept in full-resolution units (see buildPopulationLookups). A province too
// small to be hit by the sampling keeps one pixel, where it first appeared,
// so no province is ever lost (1939 at 2048x1024 would otherwise lose one).
//
// Set at build time with -DOD_MAP_DIVISOR=N (the small-memory platforms), or
// for a run with the environment variable of the same name. 1, 2, 4 or 8.
namespace odmap {

int rasterDivisor();
void setRasterDivisor(int divisor);

/**
 * A length the rules state in pixels of the FULL-resolution map (a ship's
 * range, a radar's reach), as pixels of the raster actually loaded. Every such
 * rule was written against 8192x4096 and is compared with distances measured
 * on the loaded raster, so on a reduced world it has to shrink with it -- or a
 * boat at 1/4 resolution sails four times as far.
 */
inline float loadedPx(float fullResolutionPixels) { return fullResolutionPixels / (float)rasterDivisor(); }

}  // namespace odmap
