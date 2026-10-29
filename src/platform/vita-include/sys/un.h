/* PS Vita stand-in: the address type for a local socket. The Vita has no
 * local sockets, so the connect that follows fails, as it does on a desktop
 * with no Discord running. See CMakeLists.txt. */
#pragma once
#include <sys/socket.h>
struct sockaddr_un {
    sa_family_t sun_family;
    char        sun_path[104];
};
/* libctru's socket header (the 3DS) has no AF_UNIX at all; the value is the
 * one every BSD and Linux uses, and the socket() call with it simply fails. */
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
