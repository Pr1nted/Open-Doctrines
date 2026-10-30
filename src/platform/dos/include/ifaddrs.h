/* DOS stand-in: no interfaces; getifaddrs fails and the game shows no LAN
 * address. See sys/socket.h. */
#pragma once
#include <sys/socket.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ifaddrs {
    struct ifaddrs* ifa_next; char* ifa_name; unsigned int ifa_flags;
    struct sockaddr* ifa_addr; struct sockaddr* ifa_netmask; struct sockaddr* ifa_dstaddr; void* ifa_data;
};
static inline int getifaddrs(struct ifaddrs** out) { if (out) *out = 0; return -1; }
static inline void freeifaddrs(struct ifaddrs* p) { (void)p; }
#ifdef __cplusplus
}
#endif
