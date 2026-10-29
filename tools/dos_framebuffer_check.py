#!/usr/bin/env python3
"""
Check that the DOS build's screen shows what the game drew.

    python3 tools/dos_framebuffer_check.py <framebuffer.ppm> <screenshot dir> [--bpp N]

The DOS build presents its frames through its own platform layer
(src/platform/dos/rcore_dos.c): rlsw's frame, converted to the VESA mode's
pixel format, with the mouse cursor painted in. On a Linux host the same layer
draws into a stand-in screen instead, and OD_DOS_FB_DUMP writes its last frame
out as a PPM -- what a VESA card would have shown.

The game's own screenshots (--screenshots) come from a different road: a
readback of rlsw's frame, which never passes through the conversion. So the
two agreeing is the conversion being right -- channel order, the 5/6-bit
packing, row order -- and the last frame shown must match one of the
screenshots almost exactly. Which one is not assumed: the closest wins.

Tolerances: a 15/16-bit mode has 5 or 6 bits a channel, so it is allowed the
rounding that implies; a 32-bit mode none. A few samples may differ where the
cursor sits.
"""
import argparse
import glob
import os
import sys

from PIL import Image


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ppm")
    ap.add_argument("shots")
    ap.add_argument("--bpp", type=int, default=32)
    a = ap.parse_args()

    fb = Image.open(a.ppm).convert("RGB")
    tol = {32: 2, 24: 2, 16: 12, 15: 12}[a.bpp]
    step = 7
    best = None
    for path in sorted(glob.glob(os.path.join(a.shots, "*.png"))):
        ref = Image.open(path).convert("RGB")
        if ref.size != fb.size:
            continue
        total = bad = 0
        for y in range(0, fb.size[1], step):
            for x in range(0, fb.size[0], step):
                total += 1
                p, q = fb.getpixel((x, y)), ref.getpixel((x, y))
                if max(abs(i - j) for i, j in zip(p, q)) > tol:
                    bad += 1
        if best is None or bad < best[1]:
            best = (path, bad, total)
    if best is None:
        print(f"FAILED: no {fb.size[0]}x{fb.size[1]} screenshot in {a.shots} to compare with")
        return 1
    path, bad, total = best
    print(f"{a.bpp}-bit screen vs {os.path.basename(path)}: {bad} of {total} samples differ")
    # The cursor covers ~12x19 pixels: at most a handful of samples.
    if bad > 8:
        print("FAILED: the DOS screen does not show what the game drew")
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
