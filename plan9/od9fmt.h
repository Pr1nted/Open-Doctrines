/* printf-family formatting for the core, the C way.
 *
 * Plan 9's own print(2) is not C's printf: %u is a flag there, not a verb,
 * and %lu means something else again. The core speaks C99 printf, so this
 * is the subset of it the core uses -- flags '-' and '0', a width, the
 * length modifiers l and ll, and d i u x c s % -- written out once, with no
 * dependence on any libc, so the host can check it against the real thing
 * (tests/plan9_test.sh builds the core with it and plays the transcript).
 */
#ifndef OD9FMT_H
#define OD9FMT_H

#include <stdarg.h>

int od9_vsnprintf(char *buf, unsigned long n, const char *fmt, va_list ap);
int od9_snprintf(char *buf, unsigned long n, const char *fmt, ...);
int od9_sprintf(char *buf, const char *fmt, ...);

#endif
