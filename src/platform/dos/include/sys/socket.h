/* DOS stand-in: no network stack. Every call fails with ENOSYS, which the
 * game's network code already treats as "cannot connect". See OD_DOS in
 * CMakeLists.txt; the multiplayer transport itself is not built for DOS. */
#pragma once
#include <stddef.h>
#include <sys/types.h>
#include <errno.h>
#ifndef ENOSYS
#define ENOSYS 88
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 115
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED 111
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;
struct sockaddr { sa_family_t sa_family; char sa_data[14]; };
struct sockaddr_storage { sa_family_t ss_family; char ss_pad[126]; };
#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_LOCAL  AF_UNIX
#define AF_INET   2
#define AF_INET6  10
#define PF_UNSPEC AF_UNSPEC
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOL_SOCKET  1
#define SO_REUSEADDR 2
#define SO_ERROR     4
#define SO_KEEPALIVE 9
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define SO_RCVBUF    8
#define SO_SNDBUF    7
#define MSG_NOSIGNAL 0x4000
#define MSG_PEEK     2
#define MSG_DONTWAIT 0x40
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2
#define SOMAXCONN 128
static inline int od_nosock(void) { errno = ENOSYS; return -1; }
static inline int socket(int d, int t, int p) { (void)d; (void)t; (void)p; return od_nosock(); }
static inline int connect(int s, const struct sockaddr* a, socklen_t n) { (void)s; (void)a; (void)n; return od_nosock(); }
static inline int bind(int s, const struct sockaddr* a, socklen_t n) { (void)s; (void)a; (void)n; return od_nosock(); }
static inline int listen(int s, int b) { (void)s; (void)b; return od_nosock(); }
static inline int accept(int s, struct sockaddr* a, socklen_t* n) { (void)s; (void)a; (void)n; return od_nosock(); }
static inline ssize_t send(int s, const void* b, size_t n, int f) { (void)s; (void)b; (void)n; (void)f; return od_nosock(); }
static inline ssize_t recv(int s, void* b, size_t n, int f) { (void)s; (void)b; (void)n; (void)f; return od_nosock(); }
static inline ssize_t sendto(int s, const void* b, size_t n, int f, const struct sockaddr* a, socklen_t l) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)l; return od_nosock(); }
static inline ssize_t recvfrom(int s, void* b, size_t n, int f, struct sockaddr* a, socklen_t* l) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)l; return od_nosock(); }
static inline int setsockopt(int s, int l, int o, const void* v, socklen_t n) { (void)s; (void)l; (void)o; (void)v; (void)n; return od_nosock(); }
static inline int getsockopt(int s, int l, int o, void* v, socklen_t* n) { (void)s; (void)l; (void)o; (void)v; (void)n; return od_nosock(); }
static inline int getsockname(int s, struct sockaddr* a, socklen_t* n) { (void)s; (void)a; (void)n; return od_nosock(); }
static inline int getpeername(int s, struct sockaddr* a, socklen_t* n) { (void)s; (void)a; (void)n; return od_nosock(); }
static inline int shutdown(int s, int h) { (void)s; (void)h; return od_nosock(); }
static inline int socketpair(int d, int t, int p, int sv[2]) { (void)d; (void)t; (void)p; (void)sv; return od_nosock(); }
#ifdef __cplusplus
}
#endif
