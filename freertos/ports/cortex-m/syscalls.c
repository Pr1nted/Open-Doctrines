/* newlib-nano's printf family drags in the file calls, even for vsnprintf
 * into a buffer, and libnosys's versions warn at every link. The game never
 * opens a file: these say so. */
#include <errno.h>
#include <sys/types.h>

int _close(int fd) { (void)fd; errno = EBADF; return -1; }
int _read(int fd, char *p, int n) { (void)fd; (void)p; (void)n; errno = EBADF; return -1; }
int _write(int fd, const char *p, int n) { (void)fd; (void)p; (void)n; errno = EBADF; return -1; }
off_t _lseek(int fd, off_t o, int w) { (void)fd; (void)o; (void)w; errno = EBADF; return -1; }
