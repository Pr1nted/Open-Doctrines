#pragma once
#include <cstddef>
#include <cstdint>

/**
 * The two chamfer passes of the border distance field, in place.
 *
 * `buf` is h rows of w bytes, `stride` apart. On entry a seed pixel holds 0 and
 * every other pixel 255 (or any value it should be capped at). On return each
 * pixel holds the cheapest 2-3 chamfer path to it from a seed, expanded only
 * from pixels below 60 -- so nothing above 62 is written and anything further
 * in keeps its entry value. `perRow`, when given, is called once a row.
 *
 * BIT-IDENTICAL ON EVERY CPU. This is integer work -- byte adds and mins --
 * and every path (NEON, SSE2, WASM SIMD, scalar) produces exactly the field
 * the original one-pixel-at-a-time loop did; tests/border_distance_test.cpp
 * holds that loop and compares against it.
 */
void odBorderChamfer(uint8_t* buf, int w, int h, size_t stride, void (*perRow)() = nullptr);

/** Which scan the build compiled: "neon", "sse2", "wasm-simd128" or "scalar". */
const char* odBorderChamferPath();
