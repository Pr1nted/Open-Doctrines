/* DOS stand-in; see sys/socket.h. There is nothing to wait on. */
#pragma once
#include <errno.h>
#ifdef __cplusplus
extern "C" {
#endif
struct pollfd { int fd; short events; short revents; };
typedef unsigned long nfds_t;
#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020
static inline int poll(struct pollfd* f, nfds_t n, int t) { (void)f; (void)n; (void)t; errno = ENOSYS; return -1; }
#ifdef __cplusplus
}
#endif
