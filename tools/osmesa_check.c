/*
 * Does this machine render OpenGL at all, with no display whatsoever?
 *
 *   cc tools/osmesa_check.c -lOSMesa -lGL -o osmesa_check && ./osmesa_check
 *
 * WHY THIS EXISTS
 *
 * Every "does the game draw" check so far has needed a display server -- an X
 * server on the desktop, Xvfb in CI. That is a dependency with teeth: it
 * brought in the FreeBSD xkb failure that blocked the BSD window check, and on
 * a brand-new porting target (Solaris, a bare container) getting an X server up
 * is often harder than getting Mesa up.
 *
 * OSMesa renders into a plain memory buffer. No X, no Wayland, no window, no
 * GLFW -- just Mesa's software rasteriser writing pixels into malloc. So this
 * answers the one question those checks all rest on, with none of their
 * dependencies: can this environment create a GL context and execute GL
 * commands that land in a buffer?
 *
 * It is NOT a check of the game's own renderer -- there is no window and no
 * input, so nothing a player does. It is the CI precondition the software-GL
 * fallback and every Xvfb check assume: that software GL works here at all. A
 * runner where this fails will fail every GL test downstream, and this says so
 * in one second instead of thirty.
 *
 * WHAT IT ASSERTS
 *
 * A context is created, a known clear colour is drawn, and the buffer is read
 * back and checked to actually hold that colour -- so a context that comes up
 * but renders nothing (a stub, a broken swrast) fails rather than passes. The
 * GL vendor/renderer/version are printed, which is how you tell llvmpipe from
 * a real driver that happened to be present.
 */
#include <GL/osmesa.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

int main(void) {
    OSMesaContext ctx = OSMesaCreateContextExt(OSMESA_RGBA, 24, 8, 0, NULL);
    if (!ctx) { fprintf(stderr, "osmesa_check: OSMesaCreateContext failed\n"); return 1; }

    unsigned char* buf = malloc(W * H * 4);
    if (!buf) { fprintf(stderr, "osmesa_check: out of memory\n"); return 1; }

    if (!OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, W, H)) {
        fprintf(stderr, "osmesa_check: OSMesaMakeCurrent failed\n");
        return 1;
    }

    const char* vendor   = (const char*)glGetString(GL_VENDOR);
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    const char* version  = (const char*)glGetString(GL_VERSION);
    printf("osmesa_check: vendor=%s\n", vendor   ? vendor   : "(null)");
    printf("osmesa_check: renderer=%s\n", renderer ? renderer : "(null)");
    printf("osmesa_check: version=%s\n", version  ? version  : "(null)");
    if (!version) { fprintf(stderr, "osmesa_check: no GL context\n"); return 1; }

    /* A known colour, then prove it landed in the buffer. */
    glClearColor(0.2f, 0.4f, 0.8f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    /* Centre pixel. OSMesa is bottom-up RGBA. */
    const unsigned char* px = buf + (H / 2 * W + W / 2) * 4;
    const int ok = px[0] > 40 && px[0] < 70     /* ~0.2 * 255 */
                && px[1] > 90 && px[1] < 120    /* ~0.4 * 255 */
                && px[2] > 190 && px[2] < 215;  /* ~0.8 * 255 */
    if (!ok) {
        fprintf(stderr, "osmesa_check: clear colour did not land "
                        "(got %d,%d,%d) -- context renders nothing\n",
                px[0], px[1], px[2]);
        OSMesaDestroyContext(ctx);
        return 2;
    }

    printf("osmesa_check: a frame rendered and read back correctly\n");
    OSMesaDestroyContext(ctx);
    free(buf);
    return 0;
}
