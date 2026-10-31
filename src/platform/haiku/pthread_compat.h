/* Forced into raylib's C files on Haiku (CMakeLists.txt): Haiku's pthreads
 * have no pthread_attr_setinheritsched, which miniaudio calls after setting a
 * thread's priority. Reporting success is honest enough -- miniaudio already
 * treats that priority as a hint whose failure it ignores. */
#ifndef OD_HAIKU_PTHREAD_COMPAT_H
#define OD_HAIKU_PTHREAD_COMPAT_H
#include <pthread.h>
#ifndef PTHREAD_EXPLICIT_SCHED
#define PTHREAD_EXPLICIT_SCHED 1
#endif
static inline int od_pthread_attr_setinheritsched(pthread_attr_t *a, int s)
{
    (void)a;
    (void)s;
    return 0;
}
#define pthread_attr_setinheritsched od_pthread_attr_setinheritsched
#endif
