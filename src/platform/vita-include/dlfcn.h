/* PS Vita stand-in: there is no dynamic loader for homebrew, so nothing can
 * be opened. See the note in CMakeLists.txt ("THE PS VITA"). */
#pragma once
#define RTLD_LAZY   1
#define RTLD_NOW    2
#define RTLD_LOCAL  0
#define RTLD_GLOBAL 0x100
static inline void* dlopen(const char* file, int mode) { (void)file; (void)mode; return 0; }
static inline void* dlsym(void* handle, const char* name) { (void)handle; (void)name; return 0; }
static inline int dlclose(void* handle) { (void)handle; return 0; }
static inline char* dlerror(void) { return (char*)"no dynamic loading on the PS Vita"; }
