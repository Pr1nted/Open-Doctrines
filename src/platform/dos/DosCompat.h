/* What DJGPP's C library and C++ library leave out, for the DOS build.
 *
 * Force-included into every C and C++ file of the game and of raylib when
 * building with DJGPP (see OD_DOS in CMakeLists.txt).
 *
 * MATHS. DJGPP 2.05's libm has most of C99 but not fmin/fmax (in any width),
 * exp2f, nearbyint, fdim or fma -- and raylib's raymath.h calls fminf/fmaxf in
 * every clamp. They are one line each, so here they are. And because its
 * libstdc++ was configured without C99 TR1 maths, <cmath> puts none of the C99
 * functions into std::, so std::round, std::lround, std::trunc and the rest are
 * missing even where ::round exists; they are brought in by name below.
 *
 * PI. DJGPP's math.h defines PI (and PI2) as DOUBLE macros, ahead of raylib.h,
 * whose own `#ifndef PI` then keeps them -- so every `x * PI` in the game turned
 * into a double, and std::min(float, double) stopped compiling. Undefined here,
 * so raylib's float PI is the one everything sees, as on every other platform.
 */
#ifndef OD_DOS_COMPAT_H
#define OD_DOS_COMPAT_H

#if defined(__DJGPP__)

#include <math.h>
#undef PI
#undef PI2

#ifdef __cplusplus
extern "C" {
#endif

static inline double fmax(double a, double b) { return (a != a) ? b : (b != b) ? a : (a > b ? a : b); }
static inline double fmin(double a, double b) { return (a != a) ? b : (b != b) ? a : (a < b ? a : b); }
static inline float fmaxf(float a, float b) { return (a != a) ? b : (b != b) ? a : (a > b ? a : b); }
static inline float fminf(float a, float b) { return (a != a) ? b : (b != b) ? a : (a < b ? a : b); }
static inline long double fmaxl(long double a, long double b) { return (a != a) ? b : (b != b) ? a : (a > b ? a : b); }
static inline long double fminl(long double a, long double b) { return (a != a) ? b : (b != b) ? a : (a < b ? a : b); }
static inline float exp2f(float x) { return (float)exp2((double)x); }
static inline double nearbyint(double x) { return rint(x); }
static inline float nearbyintf(float x) { return rintf(x); }
static inline double fdim(double a, double b) { return a > b ? a - b : 0.0; }
static inline float fdimf(float a, float b) { return a > b ? a - b : 0.0f; }
static inline double fma(double a, double b, double c) { return a * b + c; }
static inline float fmaf(float a, float b, float c) { return a * b + c; }

#ifdef __cplusplus
}  /* extern "C" */

#include <cmath>

namespace std {
using ::round; using ::roundf; using ::lround; using ::lroundf; using ::llround; using ::llroundf;
using ::trunc; using ::truncf; using ::rint; using ::rintf; using ::lrint; using ::lrintf; using ::llrint;
using ::nearbyint; using ::nearbyintf;
using ::fmin; using ::fmax; using ::fminf; using ::fmaxf; using ::fdim; using ::fma;
using ::cbrt; using ::cbrtf; using ::hypot; using ::hypotf; using ::copysign; using ::copysignf;
using ::exp2; using ::exp2f; using ::log2; using ::log1p; using ::expm1;
using ::remainder; using ::nextafter; using ::erf; using ::erfc; using ::lgamma;
using ::acosh; using ::asinh; using ::atanh; using ::scalbn; using ::ilogb; using ::logb;

// The float overloads <cmath> would have declared, so that std::round(1.5f)
// is a float as it is everywhere else (and narrowing into a Color still fails
// where it should, not where it should not).
inline float round(float x) { return ::roundf(x); }
inline long lround(float x) { return ::lroundf(x); }
inline long long llround(float x) { return ::llroundf(x); }
inline float trunc(float x) { return ::truncf(x); }
inline float rint(float x) { return ::rintf(x); }
inline float fmin(float a, float b) { return ::fminf(a, b); }
inline float fmax(float a, float b) { return ::fmaxf(a, b); }
inline float cbrt(float x) { return ::cbrtf(x); }
inline float hypot(float a, float b) { return ::hypotf(a, b); }
inline float copysign(float a, float b) { return ::copysignf(a, b); }
inline float exp2(float x) { return ::exp2f(x); }
}  // namespace std

#endif  /* __cplusplus */

#endif  /* __DJGPP__ */

#endif  /* OD_DOS_COMPAT_H */
