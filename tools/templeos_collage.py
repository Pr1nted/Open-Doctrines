#!/usr/bin/env python3
"""The same game on two machines, in one picture.

For posts that allow a single image. Side by side is the whole argument: the
desktop build and the TempleOS build have the same map, the same left-hand
statistics panel, the same tab bar along the bottom and the same four buttons
down the right -- so the comparison makes its own case without a caption doing
it for it. Which is why the desktop half is world-map.png and not one of the
screenshots with a panel open: an open panel is detail this picture is not
making an argument about, and it costs the side-by-side its symmetry.

Matched on HEIGHT, not width. The two screenshots are different shapes (16:9
against TempleOS's 1024x768), so scaling both to one width would leave the
shorter one floating in a gap and the pair reading as two pictures that happen
to be near each other rather than as one comparison.

    python3 tools/templeos_collage.py docs/img/templeos-compare.png
"""
from __future__ import annotations

import argparse
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

H = 800                  # height both screenshots are scaled to
PAD = 28
GAP = 24
LABEL_H = 48
BG = (14, 15, 19)
FG = (232, 234, 240)
DIM = (140, 146, 158)

LEFT = ("docs/img/world-map.png", "Windows, macOS, Linux, browser", "C++ and OpenGL")
RIGHT = ("docs/img/templeos-game.png", "TempleOS", "HolyC, no host")


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
    return im.resize((round(im.width * H / im.height), H), Image.LANCZOS)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    a = ap.parse_args()

    root = pathlib.Path(__file__).resolve().parent.parent
    try:
        left, right = scaled(root / LEFT[0]), scaled(root / RIGHT[0])
    except FileNotFoundError as e:
        print(f"missing screenshot: {e}", file=sys.stderr)
        return 1

    out = Image.new("RGB", (PAD * 2 + left.width + GAP + right.width,
                            PAD * 2 + LABEL_H + H), BG)
    d = ImageDraw.Draw(out)
    big, small = font(26, True), font(18)

    x = PAD
    for im, (_, title, note) in ((left, LEFT), (right, RIGHT)):
        d.text((x, PAD), title, font=big, fill=FG)
        d.text((x + d.textlength(title, font=big) + 14, PAD + 7), note,
               font=small, fill=DIM)
        out.paste(im, (x, PAD + LABEL_H))
        # A hairline, because both screenshots are nearly black at the edges
        # and would otherwise bleed into the background and into each other.
        d.rectangle([x, PAD + LABEL_H, x + im.width - 1, PAD + LABEL_H + H - 1],
                    outline=(46, 49, 56))
        x += im.width + GAP

    out.save(a.out)
    print(f"{a.out}: {out.width}x{out.height}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
