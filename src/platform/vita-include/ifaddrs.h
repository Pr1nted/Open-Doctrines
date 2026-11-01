/* PS Vita stand-in: no interface list; getifaddrs fails, and the game then
 * shows no LAN address. See CMakeLists.txt. */
#pragma once
#include <sys/socket.h>
struct ifaddrs {
    struct ifaddrs*  ifa_next;
    char*            ifa_name;
    unsigned int     ifa_flags;
    struct sockaddr* ifa_addr;
    struct sockaddr* ifa_netmask;
    struct sockaddr* ifa_dstaddr;
    void*            ifa_data;
};
static inline int getifaddrs(struct ifaddrs** out) { if (out) *out = 0; return -1; }
static inline void freeifaddrs(struct ifaddrs* p) { (void)p; }
