#!/usr/bin/env python3
"""
Make raylib's screen readback right way up, and the right colours, on rlsw.

    python3 tools/patch_raylib_sw_readback.py <path to rlgl.h>

WHY THIS EXISTS

A software-rendered build (OD_SOFTWARE_RENDER: raylib on rlsw, no GL) read
its own screen back upside down and with red and blue swapped: every
screenshot, every feedback report's picture, and the frame the pause screen
blurs. The window itself was right.

rlReadScreenPixels is written for OpenGL, whose glReadPixels returns rows
bottom-up, so it flips them. Under rlsw that glReadPixels is swReadPixels,
which already returns rows TOP-down -- and, because rlsw is built to hand its
framebuffer to an SDL window surface, in BGRA order (RLSW_FRAMEBUFFER_OUTPUT_
BGRA, on by default). raylib's flip then turns a correct image over, and
nothing turns the channels back.

The fix is a software-only branch ahead of the flip: swap the channels back
into RGBA when rlsw output BGRA, set alpha opaque as the flip loop does, and
return without flipping. GL builds never compile it.

Reported upstream with a raylib-only reproduction (PLATFORM=Memory, a red
band drawn at the top reads back at the bottom as 55,41,230); drop this
patch when raylib fixes it -- the anchor stops matching and says so.

FATAL WHEN IT FAILS, IDEMPOTENT, as the other patch_raylib_*.py: an unpatched
readback is wrong pictures that nothing else in the build would notice, and
CMake re-runs configure far more often than it re-fetches raylib.
"""

import os
import sys

MARK = "[od patch_raylib_sw_readback]"
ANCHOR = ("    // Flip image vertically\n"
          "    // NOTE: Alpha value has already been applied to RGB in framebuffer, not needed anymore\n")
INSERT = ("#if defined(GRAPHICS_API_OPENGL_SOFTWARE)\n"
          "    // " + MARK + " rlsw returns rows top-down, and as BGRA when it is\n"
          "    // built for SDL output (RLSW_FRAMEBUFFER_OUTPUT_BGRA, the default): put\n"
          "    // the channels back in RGBA and return without the GL flip below.\n"
          "    for (int i = 0; i < width*height*4; i += 4)\n"
          "    {\n"
          "    #if RLSW_FRAMEBUFFER_OUTPUT_BGRA\n"
          "        unsigned char t = imgData[i];\n"
          "        imgData[i] = imgData[i + 2];\n"
          "        imgData[i + 2] = t;\n"
          "    #endif\n"
          "        imgData[i + 3] = 255;\n"
          "    }\n"
          "    return imgData;\n"
          "#endif\n")


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    path = argv[1]
    if not os.path.isfile(path):
        print(f"patch_raylib_sw_readback: no such file: {path}", file=sys.stderr)
        return 1
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()
    if MARK in src:
        print("patch_raylib_sw_readback: already applied")
        return 0
    n = src.count(ANCHOR)
    if n != 1:
        print(f"patch_raylib_sw_readback: expected exactly one rlReadScreenPixels flip in {path}, "
              f"found {n}. raylib's readback has moved; the patch must be re-derived.",
              file=sys.stderr)
        return 1
    src = src.replace(ANCHOR, INSERT + ANCHOR, 1)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(src)
    os.replace(tmp, path)
    print("patch_raylib_sw_readback: applied to " + os.path.basename(path))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
