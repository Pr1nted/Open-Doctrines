/*
 * Does this ANGLE bundle draw? Run by .github/workflows/angle-prebuilts.yml
 * against every bundle before it can be published.
 *
 *     angle_smoke <bundle dir> [swiftshader|metal|d3d11|vulkan|opengl]
 *
 * Loads the bundle's libEGL from <bundle dir> -- not from the system -- opens a
 * display on the named backend (swiftshader by default: ANGLE's Vulkan on the
 * CPU, the one backend every CI runner has), clears a 16x16 offscreen surface
 * to a known colour and reads a pixel back. Exit 0 only if the pixel is that
 * colour. No EGL or GL headers: the dozen constants it needs are spelled out,
 * so it builds with nothing but a C compiler.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#define APIENTRY_ __stdcall
static void* openLib(const char* dir) {
    char p[1024];
    snprintf(p, sizeof p, "%s\\libEGL.dll", dir);
    SetDllDirectoryA(dir);   /* so libEGL finds libGLESv2 and SwiftShader beside it */
    return (void*)LoadLibraryA(p);
}
static void* sym(void* h, const char* n) { return (void*)GetProcAddress((HMODULE)h, n); }
#else
#include <dlfcn.h>
#define APIENTRY_
static void* openLib(const char* dir) {
    char p[1024];
#ifdef __APPLE__
    snprintf(p, sizeof p, "%s/libEGL.dylib", dir);
#else
    snprintf(p, sizeof p, "%s/libEGL.so", dir);
#endif
    return dlopen(p, RTLD_NOW | RTLD_LOCAL);
}
static void* sym(void* h, const char* n) { return dlsym(h, n); }
#endif

typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef int32_t EGLint;
typedef unsigned EGLBoolean;

typedef void* (APIENTRY_* PFN_GetProcAddress)(const char*);
typedef EGLDisplay (APIENTRY_* PFN_GetPlatformDisplayEXT)(unsigned, void*, const EGLint*);
typedef EGLBoolean (APIENTRY_* PFN_Initialize)(EGLDisplay, EGLint*, EGLint*);
typedef EGLBoolean (APIENTRY_* PFN_ChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef EGLSurface (APIENTRY_* PFN_CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*);
typedef EGLContext (APIENTRY_* PFN_CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef EGLBoolean (APIENTRY_* PFN_MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLint (APIENTRY_* PFN_GetError)(void);
typedef void (APIENTRY_* PFN_glClearColor)(float, float, float, float);
typedef void (APIENTRY_* PFN_glClear)(unsigned);
typedef void (APIENTRY_* PFN_glReadPixels)(int, int, int, int, unsigned, unsigned, void*);
typedef const char* (APIENTRY_* PFN_glGetString)(unsigned);

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: angle_smoke <bundle dir> [backend]\n"); return 2; }
    const char* backend = argc > 2 ? argv[2] : "swiftshader";
    EGLint type = 0x3450, device = 0;                                 /* TYPE_VULKAN */
    if (!strcmp(backend, "swiftshader")) device = 0x3487;             /* DEVICE_TYPE_SWIFTSHADER */
    else if (!strcmp(backend, "metal")) type = 0x3489;
    else if (!strcmp(backend, "d3d11")) type = 0x3208;
    else if (!strcmp(backend, "opengl")) type = 0x320d;
    else if (strcmp(backend, "vulkan")) { fprintf(stderr, "unknown backend %s\n", backend); return 2; }

    void* egl = openLib(argv[1]);
    if (!egl) { fprintf(stderr, "FAIL: cannot load libEGL from %s\n", argv[1]); return 1; }
    PFN_GetProcAddress gpa = (PFN_GetProcAddress)sym(egl, "eglGetProcAddress");
    PFN_GetError err = (PFN_GetError)sym(egl, "eglGetError");
    PFN_GetPlatformDisplayEXT getDisplay = gpa ? (PFN_GetPlatformDisplayEXT)gpa("eglGetPlatformDisplayEXT") : NULL;
    PFN_Initialize init = (PFN_Initialize)sym(egl, "eglInitialize");
    PFN_ChooseConfig choose = (PFN_ChooseConfig)sym(egl, "eglChooseConfig");
    PFN_CreatePbufferSurface pbuffer = (PFN_CreatePbufferSurface)sym(egl, "eglCreatePbufferSurface");
    PFN_CreateContext context = (PFN_CreateContext)sym(egl, "eglCreateContext");
    PFN_MakeCurrent current = (PFN_MakeCurrent)sym(egl, "eglMakeCurrent");
    if (!getDisplay || !init || !choose || !pbuffer || !context || !current || !err) {
        fprintf(stderr, "FAIL: libEGL is missing an entry point\n");
        return 1;
    }

    EGLint dattr[7] = {0x3203, type, 0x3038, 0, 0, 0, 0};             /* TYPE, type, NONE */
    if (device) { dattr[2] = 0x3209; dattr[3] = device; dattr[4] = 0x3038; }
    EGLDisplay d = getDisplay(0x3202 /* PLATFORM_ANGLE */, (void*)0, dattr);
    EGLint major = 0, minor = 0;
    if (!d || !init(d, &major, &minor)) { fprintf(stderr, "FAIL: eglInitialize on %s (0x%x)\n", backend, err()); return 1; }

    const EGLint cattr[] = {0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8,   /* R G B A */
                            0x3033, 0x0001, 0x3040, 0x0004, 0x3038};       /* PBUFFER, ES2, NONE */
    EGLConfig cfg; EGLint n = 0;
    if (!choose(d, cattr, &cfg, 1, &n) || n < 1) { fprintf(stderr, "FAIL: no ES2 pbuffer config\n"); return 1; }
    const EGLint sattr[] = {0x3057, 16, 0x3056, 16, 0x3038};              /* WIDTH HEIGHT NONE */
    EGLSurface s = pbuffer(d, cfg, sattr);
    const EGLint xattr[] = {0x3098, 2, 0x3038};                           /* CLIENT_VERSION 2 */
    EGLContext c = context(d, cfg, (EGLContext)0, xattr);
    if (!s || !c || !current(d, s, s, c)) { fprintf(stderr, "FAIL: no ES2 context (0x%x)\n", err()); return 1; }

    PFN_glClearColor clearColor = (PFN_glClearColor)gpa("glClearColor");
    PFN_glClear clear = (PFN_glClear)gpa("glClear");
    PFN_glReadPixels readPixels = (PFN_glReadPixels)gpa("glReadPixels");
    PFN_glGetString getString = (PFN_glGetString)gpa("glGetString");
    if (!clearColor || !clear || !readPixels || !getString) { fprintf(stderr, "FAIL: GLES entry points\n"); return 1; }
    clearColor(0.0f, 1.0f, 0.0f, 1.0f);
    clear(0x4000);                                                        /* COLOR_BUFFER_BIT */
    unsigned char px[4] = {0};
    readPixels(8, 8, 1, 1, 0x1908, 0x1401, px);                           /* RGBA UNSIGNED_BYTE */
    printf("renderer: %s\npixel: %d %d %d %d\n", getString(0x1F01), px[0], px[1], px[2], px[3]);
    if (px[0] != 0 || px[1] != 255 || px[2] != 0) { fprintf(stderr, "FAIL: cleared green, read something else\n"); return 1; }
    printf("PASS: %s draws\n", backend);
    return 0;
}
