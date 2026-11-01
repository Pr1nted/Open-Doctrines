/* DOS stand-in; see sys/socket.h. Text conversions work (they need no
 * network); nothing can be connected to. */
#pragma once
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
static inline int inet_pton(int af, const char* src, void* dst) {
    unsigned a, b, c, d; char tail;
    if (af != AF_INET) return 0;
    if (sscanf(src, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return 0;
    unsigned char* o = (unsigned char*)dst; o[0] = a; o[1] = b; o[2] = c; o[3] = d;
    return 1;
}
static inline const char* inet_ntop(int af, const void* src, char* dst, socklen_t n) {
    const unsigned char* p = (const unsigned char*)src;
    if (af != AF_INET || n < INET_ADDRSTRLEN) return 0;
    snprintf(dst, n, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return dst;
}
static inline in_addr_t inet_addr(const char* s) { in_addr_t a; return inet_pton(AF_INET, s, &a) == 1 ? a : INADDR_NONE; }
#ifdef __cplusplus
}
#endif
