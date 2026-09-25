#!/usr/bin/env python3
"""The same game on two machines, in one picture.

For posts that allow a single image. TempleOS on the left, everything else on
the right, butted together with no gutter so it reads as one picture rather
than two screenshots near each other. They line up element for element -- the
same four buttons down the side in the same order, the same eight tabs along
the bottom, the same green Process Turn in the corner -- so the comparison
makes its own case without a caption doing it for it. Which is why the desktop
half is world-map.png and not one of the screenshots with a panel open: an
open panel is detail this picture is not arguing about, and it costs the
side-by-side its symmetry.

Matched on HEIGHT, not width. The two are different shapes (TempleOS is
1024x768 against the desktop's 16:9), so scaling both to one width would leave
the shorter one floating in a gap.

The badges are the RTX ON / RTX OFF layout, which is doing a job here: it is
already understood to mean "same scene, one thing changed", so the constant
goes in the small line and the variable in the big one. Green marks the
surprising half, the way the meme marks the expensive one.

    python3 tools/templeos_collage.py docs/img/templeos-compare.png
"""
from __future__ import annotations

import argparse
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

H = 820                  # height both screenshots are scaled to
SEAM = 3                 # the bright divider down the middle
BOTTOM = 150             # badge baseline, clear of each build's tab bar
SIDE = 46                # badge inset from its half's left edge

WHITE = (244, 245, 248)
INK = (17, 18, 22)
GREY = (122, 128, 138)
GREEN = (74, 168, 88)    # the game's own Process Turn green, not NVIDIA's
SLATE = (108, 114, 124)

LEFT = ("docs/img/templeos-game.png", "OPEN DOCTRINES", "TEMPLEOS", GREEN)
RIGHT = ("docs/img/world-map.png", "OPEN DOCTRINES", "EVERYTHING ELSE", SLATE)


def font(paths, size):
    for path in paths:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


HEAVY = ["/System/Library/Fonts/Supplemental/Arial Black.ttf",
         "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"]
BOLD = ["/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"]


def tracked(d, xy, text, f, fill, track, draw=True):
    """Uppercase labels need letter-spacing and PIL has none, so place each
    glyph. Returns the width, because the box has to be sized around it."""
    x, y = xy
    for ch in text:
        if draw:
            d.text((x, y), ch, font=f, fill=fill)
        x += d.textlength(ch, font=f) + track
    return x - xy[0] - track


def badge(d, x, y, small, big, accent, scale):
    """Bottom-left anchored: (x, y) is the badge's lower-left corner.

    Sized from the glyphs' own ink boxes rather than from font sizes. Arial
    Black overshoots its nominal size enough that guessed padding clipped the
    descenders off the big word and ran it out through the right edge.
    """
    sf, bf = font(BOLD, round(23 * scale)), font(HEAVY, round(64 * scale))
    track, pad, gap = 3.0 * scale, round(26 * scale), round(12 * scale)

    sw = tracked(d, (0, 0), small, sf, None, track, draw=False)
    sb = d.textbbox((0, 0), small, font=sf)
    bb = d.textbbox((0, 0), big, font=bf)
    bigw, bigh = bb[2] - bb[0], bb[3] - bb[1]

    boxw = round(max(sw, bigw)) + pad * 2
    boxh = pad * 2 + (sb[3] - sb[1]) + gap + bigh
    top = y - boxh

    # The angled accent, skewed like the meme's -- a plain rectangle reads as
    # a colour swatch rather than as part of the badge.
    skew, aw = round(16 * scale), round(34 * scale)
    d.polygon([(x + skew, top), (x + skew + aw, top),
               (x + aw, y), (x, y)], fill=accent)

    bx = x + skew + aw + round(10 * scale)
    d.rectangle([bx, top, bx + boxw, y], fill=WHITE)
    tracked(d, (bx + pad, top + pad - sb[1]), small, sf, GREY, track)
    d.text((bx + pad - bb[0], top + pad + (sb[3] - sb[1]) + gap - bb[1]),
           big, font=bf, fill=INK)


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

    out = Image.new("RGB", (left.width + SEAM + right.width, H), WHITE)
    out.paste(left, (0, 0))
    out.paste(right, (left.width + SEAM, 0))
    d = ImageDraw.Draw(out)

    scale = H / 820
    badge(d, SIDE, H - BOTTOM, LEFT[1], LEFT[2], LEFT[3], scale)
    badge(d, left.width + SEAM + SIDE, H - BOTTOM, RIGHT[1], RIGHT[2], RIGHT[3], scale)

    out.save(a.out)
    print(f"{a.out}: {out.width}x{out.height}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
