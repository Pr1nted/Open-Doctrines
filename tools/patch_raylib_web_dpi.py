#!/usr/bin/env python3
"""
Give the web build a device-pixel canvas so the UI is not "too zoomed in".

    python3 tools/patch_raylib_web_dpi.py <path to rcore_web.c>

WHY THIS EXISTS

raylib's web platform sizes the canvas BACKING STORE to window.innerWidth x
window.innerHeight -- CSS/layout pixels, with no devicePixelRatio. On a desktop
with a 1:1 display that is fine. On a phone (dpr 2-3) it is two problems at
once: the UI is laid out against ~390 logical pixels so every panel and glyph
is huge ("UI is a bit too zoomed in"), and the browser then upscales that small
backing store to the physical screen, so it is blurry as well.

The fix is to make the backing store the DEVICE resolution (innerWidth*dpr)
while leaving the CSS box at layout pixels, so the page occupies the same space
but the framebuffer is dense. Screen size then equals the backing store, in
device pixels, and the rest of the input path needs nothing: GLFW-emscripten
already reports the cursor in device pixels and raylib's touch callback already
normalises taps to the screen size, so a click and a tap both land where the UI
is drawn. (This is why only EmscriptenResizeCallback is touched, and the mouse
and touch callbacks are left stock.)

dpr is clamped to [1, 3]: 1 keeps unchanged behaviour on ordinary displays, and
3 caps the backing store so a 4x phone panel does not blow past the mobile
GL_MAX_TEXTURE_SIZE / memory budget for a gain no eye resolves.

WHY A SCRIPT, AND WHY IT IS FATAL WHEN IT FAILS

raylib is pinned at 5.5 and built from source (CMakeLists.txt), so the delta
stays the two hunks that matter and visible in one file rather than a vendored
fork. If the anchors no longer match, raylib has restructured the resize
callback and the patch must be re-derived -- shipping the web build unpatched
would silently reintroduce the zoomed-in, blurry mobile UI, which nothing else
in the build would catch. Exiting non-zero stops that.

Idempotent: CMake re-runs configure far more often than it re-fetches raylib.
"""

import os
import sys

# Hunk 1: size the backing store to device pixels. The stock two-line comment
# plus the two innerWidth/innerHeight reads are unique to this callback.
WANT1 = (
    "    // This event is called whenever the window changes sizes,\n"
    "    // so the size of the canvas object is explicitly retrieved below\n"
    "    int width = EM_ASM_INT( return window.innerWidth; );\n"
    "    int height = EM_ASM_INT( return window.innerHeight; );"
)
BECOME1 = (
    "    // OD PATCH (tools/patch_raylib_web_dpi.py): backing store at the DEVICE\n"
    "    // resolution, CSS box left at layout pixels. Upstream used innerWidth/\n"
    "    // innerHeight for both, which on a high-dpr phone laid the UI out against\n"
    "    // ~390 logical pixels (too zoomed in) and then upscaled it (blurry).\n"
    "    double odDpr = EM_ASM_DOUBLE({ var d = window.devicePixelRatio || 1; if (d < 1) d = 1; if (d > 3) d = 3; return d; });\n"
    "    int odCssW = EM_ASM_INT( return window.innerWidth; );\n"
    "    int odCssH = EM_ASM_INT( return window.innerHeight; );\n"
    "    int width = (int)(odCssW * odDpr + 0.5);\n"
    "    int height = (int)(odCssH * odDpr + 0.5);"
)

# Hunk 2: after setting the backing store, pin the CSS box back to layout pixels
# so the page footprint is unchanged. Anchored through SetupViewport so it only
# matches inside the resize callback (emscripten_set_canvas_element_size also
# appears during init).
WANT2 = (
    "    emscripten_set_canvas_element_size(\"#canvas\", width, height);\n"
    "\n"
    "    SetupViewport(width, height); // Reset viewport and projection matrix for new size"
)
BECOME2 = (
    "    emscripten_set_canvas_element_size(\"#canvas\", width, height);   // backing = device px\n"
    "    // OD PATCH: CSS box stays at layout pixels, so the page footprint is\n"
    "    // unchanged while the framebuffer behind it is denser.\n"
    "    EM_ASM({ var c = document.getElementById('canvas'); if (c) { c.style.width = $0 + 'px'; c.style.height = $1 + 'px'; } }, odCssW, odCssH);\n"
    "\n"
    "    SetupViewport(width, height); // Reset viewport and projection matrix for new size"
)

PATCHES = [(WANT1, BECOME1), (WANT2, BECOME2)]


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    path = argv[1]
    if not os.path.isfile(path):
        print(f"patch_raylib_web_dpi: no such file: {path}", file=sys.stderr)
        return 1

    with open(path, "r", encoding="utf-8") as f:
        src = f.read()

    applied = already = 0
    for want, become in PATCHES:
        if become in src:
            already += 1
            continue
        n = src.count(want)
        if n != 1:
            print(f"patch_raylib_web_dpi: expected exactly one occurrence of the "
                  f"anchor\n    {want.splitlines()[-1].strip()}\n  in {path}, found "
                  f"{n}. raylib's web resize callback has moved; the DPI patch must "
                  f"be re-derived before the web build can ship.",
                  file=sys.stderr)
            return 1
        src = src.replace(want, become, 1)
        applied += 1

    if applied:
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(src)
        os.replace(tmp, path)
        print(f"patch_raylib_web_dpi: applied {applied} hunk(s) to {os.path.basename(path)}")
    else:
        print(f"patch_raylib_web_dpi: already applied ({already} hunk(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
