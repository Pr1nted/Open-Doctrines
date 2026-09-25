#!/usr/bin/env python3
"""The same game on two machines, in one picture.

For posts that allow a single image. Side by side is the whole argument: the
desktop build and the TempleOS build have the same map, the same left-hand
statistics panel, the same tab bar along the bottom and the same buttons down
the right -- so the comparison makes its own case without a caption doing it.

Stacked rather than side by side because both are landscape; sitting them in a
column keeps each one wide enough to read.

    python3 tools/templeos_collage.py docs/img/templeos-compare.png
"""
from __future__ import annotations

import argparse
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

W = 1280                 # width each screenshot is scaled to
PAD = 28
GAP = 20
LABEL_H = 46
BG = (14, 15, 19)
FG = (232, 234, 240)
DIM = (140, 146, 158)

TOP = ("docs/img/province.png", "Windows, macOS, Linux, browser", "C++ and OpenGL")
BOT = ("docs/img/templeos-game.png", "TempleOS", "HolyC, 1024x768, no host")


def font(size, bold=False):
    for path in ("/System/Library/Fonts/Helvetica.ttc",
                 "/System/Library/Fonts/Supplemental/Arial.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(path, size, index=1 if bold and path.endswith("ttc") else 0)
        except Exception:                                   # noqa: BLE001
            continue
    return ImageFont.load_default()


def scaled(path):
    im = Image.open(path).convert("RGB")
    return im.resize((W, round(im.height * W / im.width)), Image.LANCZOS)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    a = ap.parse_args()

    root = pathlib.Path(__file__).resolve().parent.parent
    try:
        top, bot = scaled(root / TOP[0]), scaled(root / BOT[0])
    except FileNotFoundError as e:
        print(f"missing screenshot: {e}", file=sys.stderr)
        return 1

    h = PAD + LABEL_H + top.height + GAP + LABEL_H + bot.height + PAD
    out = Image.new("RGB", (W + PAD * 2, h), BG)
    d = ImageDraw.Draw(out)
    big, small = font(24, True), font(17)

    y = PAD
    for im, (_, title, note) in ((top, TOP), (bot, BOT)):
        d.text((PAD, y), title, font=big, fill=FG)
        tw = d.textlength(title, font=big)
        d.text((PAD + tw + 14, y + 6), note, font=small, fill=DIM)
        y += LABEL_H
        out.paste(im, (PAD, y))
        # A hairline keeps the two screenshots from bleeding into each other
        # where both are nearly black at the edges.
        d.rectangle([PAD, y, PAD + W - 1, y + im.height - 1], outline=(46, 49, 56))
        y += im.height + GAP

    out.save(a.out)
    print(f"{a.out}: {out.width}x{out.height}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
