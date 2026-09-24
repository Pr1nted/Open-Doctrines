#!/usr/bin/env python3
"""Bake raylib's default font for TempleOS, glyph for glyph.

Open Doctrines draws its interface with GetFontDefault() -- raylib's built-in
bitmap font, embedded in rtext.c as 512 packed words. So "make the TempleOS
text look like the game" is not a matter of picking a similar typeface: it is
the SAME font, and the only correct result is pixel-identical.

That also settles a question that would otherwise be a judgement call. The font
is one bit per pixel, so the desktop game's text is not antialiased either, and
a smooth atlas baked from a TTF would look less like the game, not more.

The layout algorithm is rtext.c's own, transcribed rather than re-invented:
glyphs sit in a 128x128 sheet, one pixel of padding, wrapping when the next
one would not fit.

    python3 tools/templeos_font.py build-web/_deps/raylib-src/src/rtext.c \\
            templeos/font.odf
"""
from __future__ import annotations

import argparse
import pathlib
import re
import struct
import sys

MAGIC, VERSION = b"ODTF", 1
SHEET = 128
HEIGHT = 10
DIVISOR = 1
FIRST = 32


def parse(src: str):
    m = re.search(r"defaultFontData\[512\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        raise SystemExit("could not find defaultFontData in that file")
    data = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{8})", m[1])]
    if len(data) != 512:
        raise SystemExit(f"expected 512 words, found {len(data)}")

    m = re.search(r"charsWidth\[224\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        raise SystemExit("could not find charsWidth in that file")
    widths = [int(x) for x in re.findall(r"-?\d+", m[1])]
    if len(widths) != 224:
        raise SystemExit(f"expected 224 widths, found {len(widths)}")
    return data, widths


def sheet_bits(data) -> bytearray:
    """The 128x128 one-bit sheet, unpacked exactly as rtext.c unpacks it.

    Each word covers 32 consecutive pixels, and bit j of the word is pixel
    i + j -- not i + (31 - j). Getting that backwards produces a sheet that
    looks like plausible font-ish noise, which is the worst kind of wrong.
    """
    px = bytearray(SHEET * SHEET)
    counter = 0
    for i in range(0, SHEET * SHEET, 32):
        for j in range(31, -1, -1):
            if data[counter] & (1 << j):
                px[i + j] = 1
        counter += 1
    return px


def rects(widths):
    """Where each glyph sits, by rtext.c's own walk."""
    out = []
    line, posx, testx = 0, DIVISOR, DIVISOR
    for w in widths:
        x = posx
        y = DIVISOR + line * (HEIGHT + DIVISOR)
        testx += w + DIVISOR
        if testx >= SHEET:
            line += 1
            posx = 2 * DIVISOR + w
            testx = posx
            x = DIVISOR
            y = DIVISOR + line * (HEIGHT + DIVISOR)
        else:
            posx = testx
        out.append((x, y, w))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("rtext")
    ap.add_argument("out")
    ap.add_argument("--preview", help="write a PNG of the sheet to check by eye")
    a = ap.parse_args()

    data, widths = parse(pathlib.Path(a.rtext).read_text(errors="replace"))
    px = sheet_bits(data)
    rc = rects(widths)

    # ── ONE GLYPH PER RECORD, ROWS PADDED TO TWO BYTES ──
    #
    # The widest glyph is nine pixels, so two bytes a row covers every one and
    # the guest never has to think about a glyph straddling a byte boundary.
    # 224 glyphs x 10 rows x 2 bytes is 4.5 KB, which is not worth compressing.
    blob = bytearray()
    blob += MAGIC
    blob += struct.pack("<HHHH", VERSION, len(widths), HEIGHT, FIRST)
    for w in widths:
        blob += struct.pack("<B", w)
    for (x, y, w) in rc:
        for row in range(HEIGHT):
            bits = 0
            for col in range(w):
                if px[(y + row) * SHEET + (x + col)]:
                    bits |= 1 << col
            blob += struct.pack("<H", bits)

    pathlib.Path(a.out).write_bytes(blob)
    ink = sum(px)
    print(f"{a.out}: {len(widths)} glyphs, {HEIGHT}px tall, "
          f"{len(blob)} bytes ({ink} lit pixels in the sheet)")

    if a.preview:
        from PIL import Image
        im = Image.new("L", (SHEET, SHEET))
        im.putdata([255 if v else 0 for v in px])
        im.resize((SHEET * 4, SHEET * 4), Image.NEAREST).save(a.preview)
        print(f"  preview: {a.preview}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
