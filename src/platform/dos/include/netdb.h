/* DOS stand-in; see sys/socket.h. No resolver: every lookup fails. */
#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#ifdef __cplusplus
extern "C" {
#endif
struct addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr* ai_addr;
    char* ai_canonname;
    struct addrinfo* ai_next;
};
struct hostent { char* h_name; char** h_aliases; int h_addrtype; int h_length; char** h_addr_list; };
#define AI_PASSIVE     1
#define AI_CANONNAME   2
#define AI_NUMERICHOST 4
#define AI_NUMERICSERV 0x400
#define AI_ADDRCONFIG  0x20
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_MAXHOST 1025
#define NI_MAXSERV 32
#define EAI_FAIL   (-4)
#define EAI_NONAME (-2)
#define EAI_AGAIN  (-3)
static inline int getaddrinfo(const char* n, const char* s, const struct addrinfo* h, struct addrinfo** r) {
    (void)n; (void)s; (void)h; if (r) *r = 0; return EAI_FAIL;
}
static inline void freeaddrinfo(struct addrinfo* a) { (void)a; }
static inline const char* gai_strerror(int e) { (void)e; return "no network on DOS"; }
static inline int getnameinfo(const struct sockaddr* a, socklen_t l, char* h, socklen_t hl, char* s, socklen_t sl, int f) {
    (void)a; (void)l; (void)h; (void)hl; (void)s; (void)sl; (void)f; return EAI_FAIL;
}
static inline struct hostent* gethostbyname(const char* n) { (void)n; return 0; }
#ifdef __cplusplus
}
#endif
