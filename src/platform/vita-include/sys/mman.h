/* PS Vita stand-in: no file mappings for homebrew; every map fails, which
 * callers already treat as "not available". See CMakeLists.txt. */
#pragma once
#include <stddef.h>
#include <sys/types.h>
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED    1
#define MAP_PRIVATE   2
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED ((void*)-1)
static inline void* mmap(void* a, size_t n, int p, int f, int fd, off_t o) {
    (void)a; (void)n; (void)p; (void)f; (void)fd; (void)o; return MAP_FAILED;
}
static inline int munmap(void* a, size_t n) { (void)a; (void)n; return -1; }
