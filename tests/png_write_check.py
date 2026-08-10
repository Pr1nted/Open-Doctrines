#!/usr/bin/env python3
"""
Decode what PngWriteTest wrote, with a decoder that is not stb_image.

    python3 tests/png_write_check.py <dir-PngWriteTest-wrote-to>

WHY A SECOND DECODER

The C++ side already round-trips through stb_image, which is the decoder the
game reads maps with, so that check is the one that matters for "will this
load". It is not the one that matters for "is this a valid PNG". stb is
famously forgiving, and a writer that emits, say, a wrong tRNS length or a row
that is a byte short can still come back out of the same library looking
correct. Pillow is stricter and shares no code with it, so the two agreeing is
evidence the file is a PNG rather than merely a file stb likes.

This exists because a .odmap's whole size claim rests on the encoder being
lossless -- see tools/odmap_pack.py. Same reasoning as
tests/gif_encoder_check.py, which does this for the timelapse writer.
"""

import os
import struct
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is needed: python3 -m pip install Pillow")


def expected_pixel(i, colours, width):
    """The pixel PngWriteTest::makeImage() should have written at index i."""
    c = (i // 7 + i // (width * 3)) % colours
    return ((c * 7 + 1) & 0xFF, (c * 13 + 2) & 0xFF, (c * 29 + 3) & 0xFF,
            0 if c == 0 else 255)


def check_file(path, colours, width, height):
    raw = open(path, "rb").read()
    if raw[:8] != b"\x89PNG\r\n\x1a\x1a"[:4] + b"\r\n\x1a\n":
        return f"{os.path.basename(path)}: not a PNG signature"

    w, h = struct.unpack(">II", raw[16:24])
    depth, colour_type = raw[24], raw[25]
    if (w, h) != (width, height):
        return f"{os.path.basename(path)}: {w}x{h}, expected {width}x{height}"
    if colour_type != 3:
        return f"{os.path.basename(path)}: colour type {colour_type}, expected 3 (indexed)"

    img = Image.open(path)
    if img.mode != "P":
        return f"{os.path.basename(path)}: Pillow read mode {img.mode}, expected P"
    px = img.convert("RGBA").load()

    for i in range(0, w * h, 997):          # a prime stride, so rows are not sampled in phase
        want = expected_pixel(i, colours, w)
        got = px[i % w, i // w]
        if got != want:
            return (f"{os.path.basename(path)}: pixel {i} is {got}, expected {want} "
                    f"(bit depth {depth})")

    print(f"  {os.path.basename(path):<14} {w}x{h}  bit depth {depth}  "
          f"{colours} colours  ok")
    return None


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip())
    out = sys.argv[1]
    manifest = os.path.join(out, "manifest.txt")
    if not os.path.exists(manifest):
        sys.exit(f"no manifest in {out} -- run PngWriteTest first")

    print("PngWrite files, decoded with Pillow:")
    errors = []
    seen = 0
    for line in open(manifest):
        if line.startswith("#") or not line.strip():
            continue
        name, colours, width, height = line.split()
        seen += 1
        err = check_file(os.path.join(out, name), int(colours), int(width), int(height))
        if err:
            errors.append(err)

    if not seen:
        sys.exit("manifest listed no files")
    for e in errors:
        print(f"  {e}")
    print("all ok" if not errors else f"{len(errors)} file(s) wrong")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
