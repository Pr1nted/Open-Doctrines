/* Forced into raylib's C files on SerenityOS (CMakeLists.txt). Its LibC has
 * no scheduling-policy attributes for threads and does not declare alloca,
 * all of which raylib's miniaudio and stb_vorbis call. The policy calls are
 * hints miniaudio ignores the failure of; alloca is the compiler's own. */
#ifndef OD_SERENITY_RAYLIB_COMPAT_H
#define OD_SERENITY_RAYLIB_COMPAT_H
#include <pthread.h>
#ifndef PTHREAD_EXPLICIT_SCHED
#define PTHREAD_EXPLICIT_SCHED 1
#endif
static inline int od_pthread_attr_setinheritsched(pthread_attr_t *a, int s) { (void)a; (void)s; return 0; }
static inline int od_pthread_attr_setschedpolicy(pthread_attr_t *a, int p) { (void)a; (void)p; return -1; }
#define pthread_attr_setinheritsched od_pthread_attr_setinheritsched
#define pthread_attr_setschedpolicy od_pthread_attr_setschedpolicy
#ifndef alloca
#define alloca __builtin_alloca
#endif
#endif
