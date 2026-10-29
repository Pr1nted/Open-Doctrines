// The process and file calls newlib declares but these consoles do not
// implement (Vita, and the 3DS and Wii, which link the same way). Every
// caller is a desktop feature -- curl and Tor as child processes, the native
// file dialog, Steam's library path, mod updates -- that already copes with
// the call failing, which is all these do: -1 and ENOSYS, as POSIX says an
// unsupported call should.
//
// Weak, so a platform library that does provide one of them wins.
#include <cerrno>
#include <cstdio>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define OD_WEAK __attribute__((weak))

extern "C" {
OD_WEAK int dup2(int, int) { errno = ENOSYS; return -1; }
OD_WEAK int execv(const char*, char* const[]) { errno = ENOSYS; return -1; }
OD_WEAK int execvp(const char*, char* const[]) { errno = ENOSYS; return -1; }
OD_WEAK int execlp(const char*, const char*, ...) { errno = ENOSYS; return -1; }
OD_WEAK pid_t waitpid(pid_t, int*, int) { errno = ENOSYS; return -1; }
OD_WEAK ssize_t readlink(const char*, char*, size_t) { errno = ENOSYS; return -1; }
OD_WEAK FILE* popen(const char*, const char*) { errno = ENOSYS; return nullptr; }
OD_WEAK int pclose(FILE*) { errno = ENOSYS; return -1; }
}

// newlib declares getrusage outside extern "C" on these targets, so the
// game's call is to the C++ name; defined here the same way the header
// declares it, whichever that is.
OD_WEAK int getrusage(int, struct rusage*) { errno = ENOSYS; return -1; }
