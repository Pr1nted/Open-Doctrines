/* DOS stand-in: DJGPP's sys/mman.h has mprotect alone. No file mappings;
 * every map fails, which callers already treat as "not available". */
#pragma once
#include_next <sys/mman.h>
#include <stddef.h>
#include <sys/types.h>
#ifndef PROT_READ
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#endif
#define MAP_SHARED    1
#define MAP_PRIVATE   2
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED ((void*)-1)
#ifdef __cplusplus
extern "C" {
#endif
static inline void* mmap(void* a, size_t n, int p, int f, int fd, off_t o) { (void)a; (void)n; (void)p; (void)f; (void)fd; (void)o; return MAP_FAILED; }
static inline int munmap(void* a, size_t n) { (void)a; (void)n; return -1; }
static inline int shm_open(const char* n, int f, int m) { (void)n; (void)f; (void)m; return -1; }
static inline int shm_unlink(const char* n) { (void)n; return -1; }
#ifdef __cplusplus
}
#endif
