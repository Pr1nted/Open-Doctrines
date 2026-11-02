/* DOS stand-in: DJGPP's own netinet/in.h has only the byte-order helpers,
 * kept here (include_next); the address types the game's network code names
 * are added. See sys/socket.h beside this. */
#pragma once
#include_next <netinet/in.h>
#include <sys/socket.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in { sa_family_t sin_family; in_port_t sin_port; struct in_addr sin_addr; unsigned char sin_zero[8]; };
struct in6_addr { unsigned char s6_addr[16]; };
struct sockaddr_in6 { sa_family_t sin6_family; in_port_t sin6_port; uint32_t sin6_flowinfo; struct in6_addr sin6_addr; uint32_t sin6_scope_id; };
#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define INADDR_NONE      ((in_addr_t)0xffffffff)
#define IPPROTO_IP   0
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_IPV6 41
#define IPV6_V6ONLY  26
#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46
#ifdef __cplusplus
}
#endif
