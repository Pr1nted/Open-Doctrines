/* PS Vita stand-in: the address type for a local socket. The Vita has no
 * local sockets, so the connect that follows fails, as it does on a desktop
 * with no Discord running. See CMakeLists.txt. */
#pragma once
#include <sys/socket.h>
struct sockaddr_un {
    sa_family_t sun_family;
    char        sun_path[104];
};
