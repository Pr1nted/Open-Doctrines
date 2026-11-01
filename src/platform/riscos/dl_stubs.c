/* A static RISC OS program has no dynamic loader: UnixLib declares dlopen
 * and dlsym, and only the shared SharedUnixLibrary build provides them. The
 * game asks for them for Steam and the dev link, both desktop features that
 * already carry on without a library -- so dlopen finds nothing, here as on
 * any machine where the library is absent. Weak, so a real one wins. */
#include <stddef.h>

__attribute__((weak)) void *dlopen(const char *file, int mode) { (void)file; (void)mode; return NULL; }
__attribute__((weak)) void *dlsym(void *handle, const char *name) { (void)handle; (void)name; return NULL; }
__attribute__((weak)) int dlclose(void *handle) { (void)handle; return 0; }
__attribute__((weak)) char *dlerror(void) { return (char *)"no dynamic loading on RISC OS"; }
