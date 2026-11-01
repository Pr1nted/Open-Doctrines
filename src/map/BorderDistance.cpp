#include "BorderDistance.h"
#include <algorithm>

// ── THE BORDER DISTANCE FIELD, TWO PASSES ──
//
// Each pass sets a pixel to the min of itself and its relaxed neighbours, so
// the ORDER those mins are taken in cannot change the answer -- only the
// sources have to be final when they are read. That splits a row in two:
//
//   the three neighbours in the row already finished (above, going down;
//   below, going up) are independent per pixel, so that step is a plain loop
//   the compiler vectorises at whatever width the target has;
//
//   the sideways neighbour is a running scan along the row. Relaxing k pixels
//   in one go is h_k(v) = v + 2k if that is at most 61, else 255 -- which is
//   exactly k single steps, gate and all -- and h_s(h_t(v)) = h_{s+t}(v), so
//   the scan is a prefix-min that runs sixteen pixels at a time in four
//   shifted steps plus the carry from the block before (Hillis-Steele).
//
// Bytes, saturating adds and unsigned mins: there is no rounding anywhere for
// an instruction set to do differently, and the scalar loop is the reference.

namespace {

inline uint8_t relax(uint8_t d, uint8_t step) {
    return d < 60 ? (uint8_t)(d + step) : (uint8_t)255;
}

void relaxFromRow(uint8_t* __restrict cur, const uint8_t* __restrict src, int w) {
    if (w == 1) { cur[0] = std::min(cur[0], relax(src[0], 2)); return; }
    cur[0] = std::min({cur[0], relax(src[0], 2), relax(src[1], 3)});
    for (int x = 1; x < w - 1; ++x) {
        const uint8_t v = std::min({relax(src[x], 2), relax(src[x - 1], 3), relax(src[x + 1], 3)});
        cur[x] = std::min(cur[x], v);
    }
    cur[w - 1] = std::min({cur[w - 1], relax(src[w - 1], 2), relax(src[w - 2], 3)});
}

}  // namespace

// OD_BORDER_SCALAR compiles the reference loop on any target; the test builds
// it both ways and requires the same bytes.
#if defined(OD_BORDER_SCALAR)
#define OD_BD_PATH "scalar"
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#include <arm_neon.h>
#define OD_BD_VECTOR 1
#define OD_BD_PATH "neon"
namespace {
using V = uint8x16_t;
inline V vsplat(uint8_t v) { return vdupq_n_u8(v); }
inline V vload(const uint8_t* p) { return vld1q_u8(p); }
inline void vstore(uint8_t* p, V v) { vst1q_u8(p, v); }
inline V vmin(V a, V b) { return vminq_u8(a, b); }
// h_k for a vector of k's: saturating add, then anything past 61 becomes 255.
inline V vh(V v, V twoK) {
    const V t = vqaddq_u8(v, twoK);
    return vorrq_u8(t, vcgtq_u8(t, vdupq_n_u8(61)));
}
template <int S> inline V shiftUp(V v)   { return vextq_u8(vdupq_n_u8(255), v, 16 - S); }
template <int S> inline V shiftDown(V v) { return vextq_u8(v, vdupq_n_u8(255), S); }
}  // namespace
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define OD_BD_VECTOR 1
#define OD_BD_PATH "sse2"
namespace {
using V = __m128i;
inline V vsplat(uint8_t v) { return _mm_set1_epi8((char)v); }
inline V vload(const uint8_t* p) { return _mm_loadu_si128((const __m128i*)p); }
inline void vstore(uint8_t* p, V v) { _mm_storeu_si128((__m128i*)p, v); }
inline V vmin(V a, V b) { return _mm_min_epu8(a, b); }
inline V vh(V v, V twoK) {
    const V t = _mm_adds_epu8(v, twoK);
    // t > 61 unsigned, as max(t, 62) == t: SSE2 has no unsigned byte compare.
    return _mm_or_si128(t, _mm_cmpeq_epi8(_mm_max_epu8(t, _mm_set1_epi8(62)), t));
}
template <int S> inline V shiftUp(V v) {
    return _mm_or_si128(_mm_slli_si128(v, S), _mm_srli_si128(_mm_set1_epi8((char)0xFF), 16 - S));
}
template <int S> inline V shiftDown(V v) {
    return _mm_or_si128(_mm_srli_si128(v, S), _mm_slli_si128(_mm_set1_epi8((char)0xFF), 16 - S));
}
}  // namespace
#elif defined(__wasm_simd128__)
#include <wasm_simd128.h>
#define OD_BD_VECTOR 1
#define OD_BD_PATH "wasm-simd128"
namespace {
using V = v128_t;
inline V vsplat(uint8_t v) { return wasm_u8x16_splat(v); }
inline V vload(const uint8_t* p) { return wasm_v128_load(p); }
inline void vstore(uint8_t* p, V v) { wasm_v128_store(p, v); }
inline V vmin(V a, V b) { return wasm_u8x16_min(a, b); }
inline V vh(V v, V twoK) {
    const V t = wasm_u8x16_add_sat(v, twoK);
    return wasm_v128_or(t, wasm_u8x16_gt(t, wasm_u8x16_splat(61)));
}
// Lane i of the result is lane i-S of v (shiftUp) or i+S (shiftDown); an
// index of 16 picks lane 0 of the all-255 vector.
#define OD_UP(S) wasm_i8x16_shuffle(v, f, \
    0  < S ? 16 : 0  - S, 1  < S ? 16 : 1  - S, 2  < S ? 16 : 2  - S, 3  < S ? 16 : 3  - S, \
    4  < S ? 16 : 4  - S, 5  < S ? 16 : 5  - S, 6  < S ? 16 : 6  - S, 7  < S ? 16 : 7  - S, \
    8  < S ? 16 : 8  - S, 9  < S ? 16 : 9  - S, 10 < S ? 16 : 10 - S, 11 < S ? 16 : 11 - S, \
    12 < S ? 16 : 12 - S, 13 < S ? 16 : 13 - S, 14 < S ? 16 : 14 - S, 15 < S ? 16 : 15 - S)
#define OD_DOWN(S) wasm_i8x16_shuffle(v, f, \
    0  + S > 15 ? 16 : 0  + S, 1  + S > 15 ? 16 : 1  + S, 2  + S > 15 ? 16 : 2  + S, 3  + S > 15 ? 16 : 3  + S, \
    4  + S > 15 ? 16 : 4  + S, 5  + S > 15 ? 16 : 5  + S, 6  + S > 15 ? 16 : 6  + S, 7  + S > 15 ? 16 : 7  + S, \
    8  + S > 15 ? 16 : 8  + S, 9  + S > 15 ? 16 : 9  + S, 10 + S > 15 ? 16 : 10 + S, 11 + S > 15 ? 16 : 11 + S, \
    12 + S > 15 ? 16 : 12 + S, 13 + S > 15 ? 16 : 13 + S, 14 + S > 15 ? 16 : 14 + S, 15 + S > 15 ? 16 : 15 + S)
template <int S> inline V shiftUp(V v)   { const V f = wasm_u8x16_splat(255); return OD_UP(S); }
template <int S> inline V shiftDown(V v) { const V f = wasm_u8x16_splat(255); return OD_DOWN(S); }
#undef OD_UP
#undef OD_DOWN
}  // namespace
#else
#define OD_BD_PATH "scalar"
#endif

namespace {

#ifdef OD_BD_VECTOR
// The k of h_k for each lane: its distance from the carried-in pixel.
alignas(16) const uint8_t kFromLeft[16]  = {2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32};
alignas(16) const uint8_t kFromRight[16] = {32, 30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8, 6, 4, 2};

void scanLeftToRight(uint8_t* r, int w) {
    const V fromLeft = vload(kFromLeft);
    const V s1 = vsplat(2), s2 = vsplat(4), s4 = vsplat(8), s8 = vsplat(16);
    uint8_t carry = 255;                     // nothing left of pixel 0
    int x = 0;
    for (; x + 16 <= w; x += 16) {
        V v = vload(r + x);
        v = vmin(v, vh(shiftUp<1>(v), s1));
        v = vmin(v, vh(shiftUp<2>(v), s2));
        v = vmin(v, vh(shiftUp<4>(v), s4));
        v = vmin(v, vh(shiftUp<8>(v), s8));
        v = vmin(v, vh(vsplat(carry), fromLeft));
        vstore(r + x, v);
        carry = r[x + 15];
    }
    for (; x < w; ++x)
        if (x > 0) r[x] = std::min(r[x], relax(r[x - 1], 2));
}

void scanRightToLeft(uint8_t* r, int w) {
    const V fromRight = vload(kFromRight);
    const V s1 = vsplat(2), s2 = vsplat(4), s4 = vsplat(8), s8 = vsplat(16);
    uint8_t carry = 255;                     // nothing right of the last pixel
    int x = w;
    for (; x >= 16; ) {
        x -= 16;
        V v = vload(r + x);
        v = vmin(v, vh(shiftDown<1>(v), s1));
        v = vmin(v, vh(shiftDown<2>(v), s2));
        v = vmin(v, vh(shiftDown<4>(v), s4));
        v = vmin(v, vh(shiftDown<8>(v), s8));
        v = vmin(v, vh(vsplat(carry), fromRight));
        vstore(r + x, v);
        carry = r[x];
    }
    // The pixels left of the last block, or the whole row when it is shorter
    // than one -- where the last pixel has nothing to its right.
    for (int xi = std::min(x, w - 1) - 1; xi >= 0; --xi)
        r[xi] = std::min(r[xi], relax(r[xi + 1], 2));
}
#else
void scanLeftToRight(uint8_t* r, int w) {
    for (int x = 1; x < w; ++x) r[x] = std::min(r[x], relax(r[x - 1], 2));
}
void scanRightToLeft(uint8_t* r, int w) {
    for (int x = w - 2; x >= 0; --x) r[x] = std::min(r[x], relax(r[x + 1], 2));
}
#endif

}  // namespace

void odBorderChamfer(uint8_t* buf, int w, int h, size_t stride, void (*perRow)()) {
    if (!buf || w <= 0 || h <= 0) return;
    for (int y = 0; y < h; ++y) {            // forward: up-left half of the mask
        if (perRow) perRow();
        uint8_t* cur = buf + (size_t)y * stride;
        if (y > 0) relaxFromRow(cur, cur - stride, w);
        scanLeftToRight(cur, w);
    }
    for (int y = h - 1; y >= 0; --y) {       // backward: down-right half
        if (perRow) perRow();
        uint8_t* cur = buf + (size_t)y * stride;
        if (y < h - 1) relaxFromRow(cur, cur + stride, w);
        scanRightToLeft(cur, w);
    }
}

const char* odBorderChamferPath() { return OD_BD_PATH; }
