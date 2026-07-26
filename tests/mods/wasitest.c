/* Fixture for the WasiStub capability.
 *
 * Declares the real WASI imports the way an interpreter-in-a-mod does, so the
 * host is exercised through exactly the path CPython/ruby.wasm/Lua would take.
 */
#include "gearbox.h"

#define W(name) __attribute__((import_module("wasi_snapshot_preview1"), import_name(name)))

typedef struct { const void* buf; unsigned int len; } iovec_t;

W("fd_write")       int wasi_fd_write(int fd, const iovec_t* iovs, int n, unsigned int* out);
W("random_get")     int wasi_random_get(void* buf, unsigned int len);
W("clock_time_get") int wasi_clock_time_get(int id, unsigned long long prec, unsigned long long* out);
W("path_open")      int wasi_path_open(int dirfd, int dirflags, const char* p, int plen,
                                       int oflags, unsigned long long a,
                                       unsigned long long b, int fdflags, int* fd);

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) { return 0; }

/* Should reach the mod log. */
GEARBOX_EXPORT("t_write")
int32_t t_write(void) {
    static const char msg[] = "printed from wasi";
    iovec_t iov = { msg, (unsigned int)(sizeof(msg) - 1) };
    unsigned int written = 0;
    int rc = wasi_fd_write(1, &iov, 1, &written);
    return rc == 0 ? (int32_t)written : -rc;
}

/* Deterministic: the same value on every run of the same mod. */
GEARBOX_EXPORT("t_random")
uint32_t t_random(void) {
    unsigned char b[4] = {0, 0, 0, 0};
    if (wasi_random_get(b, 4) != 0) return 0;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Turn-derived, not wall clock. */
GEARBOX_EXPORT("t_clock_secs")
uint32_t t_clock_secs(void) {
    unsigned long long t = 0;
    if (wasi_clock_time_get(0, 0, &t) != 0) return 0xFFFFFFFFu;
    return (uint32_t)(t / 1000000000ull);
}

/* Must be refused. */
GEARBOX_EXPORT("t_open")
int32_t t_open(void) {
    int fd = -1;
    return wasi_path_open(3, 0, "/etc/passwd", 11, 0, 0, 0, 0, &fd);
}

/* fd 3 is not stdout/stderr; must be EBADF. */
GEARBOX_EXPORT("t_write_badfd")
int32_t t_write_badfd(void) {
    static const char msg[] = "x";
    iovec_t iov = { msg, 1 };
    unsigned int written = 0;
    return wasi_fd_write(3, &iov, 1, &written);
}
