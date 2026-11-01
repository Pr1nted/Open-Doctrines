/* The C99 the FreeRTOS core expects, in terms of Plan 9's own libc.
 *
 * The core (freertos/core) includes <stdint.h>, <stdio.h>, <string.h> and the
 * rest. Plan 9 has none of those -- it has <u.h> and <libc.h> -- so the mkfile
 * puts this directory first on the include path, and every one of those names
 * here is a two-line header that includes this file. Nothing in the core is
 * forked for Plan 9; the core is only kept to what kencc compiles (no
 * declarations after statements, no compound literals, no inline).
 *
 * Plan 9's headers have no include guards, so they are included exactly once,
 * here, under this file's guard.
 */
#ifndef OD9_H
#define OD9_H

#include <u.h>
#include <libc.h>

typedef uchar  uint8_t;
typedef ushort uint16_t;
typedef uint   uint32_t;
typedef uvlong uint64_t;
typedef int    int32_t;
typedef vlong  int64_t;
typedef ulong  size_t;

#define UINT32_MAX  0xFFFFFFFFU
#define UINT16_MAX  0xFFFF
#define NULL        nil
#define inline

/* C's printf, not print(2): see od9fmt.c. */
#include "od9fmt.h"
#define vsnprintf od9_vsnprintf
#define snprintf  od9_snprintf
#define sprintf   od9_sprintf

#endif
