#!/usr/bin/env python3
"""Rebuild the itch.io cover art and page background from the game's own images.

    python3 tools/itch-cover.py

Writes docs/itch/cover.png, cover-titled.png and page-background.png from what
tools/screenshots.sh produced in docs/img/. Run it after the UI changes, or the
store page keeps advertising a version of the game that no longer exists.

WHY THE WORDMARK IS CROPPED AND NOT TYPED

The title is drawn in raylib's built-in font, which is not a TTF anything else
can load. Setting it in a lookalike would be visibly not-the-game to anyone who
had seen a screenshot. So it is lifted pixel-for-pixel out of the menu, and
every pixel that is not part of a gold stroke is made transparent -- pasting the
crop as a rectangle stamps a dark box across North Africa.

WHY THE BACKGROUND COMES FROM THE TIMELAPSE

A screenshot carries the game's UI: the Process Turn button, the sidebar, the
"PLAYING AS" caption. A page background with buttons drawn on it reads as
broken. A timelapse frame is pure map.
"""

import os
import sys

try:
    from PIL import Image, ImageEnhance
except ImportError:
    sys.exit("Pillow is needed: python3 -m pip install Pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(ROOT, "docs", "img")
OUT = os.path.join(ROOT, "docs", "itch")

# Europe, the Atlantic and North Africa, at 630x500's aspect ratio. Hand-picked
# because itch's own crop takes the middle of the world map, which is ocean.
COVER_CROP = (520, 60, 1150, 560)
COVER_SIZE = (630, 500)


def need(path: str) -> str:
    full = os.path.join(IMG, path)
    if not os.path.exists(full):
        sys.exit(f"missing {full}\nRun tools/screenshots.sh first.")
    return full


def wordmark() -> Image.Image:
    """The gold title, cut out of the menu with everything else transparent."""
    menu = Image.open(need("main-menu.png")).convert("RGBA")

    # Found rather than hardcoded: the title moves if the menu layout changes,
    # and a stale rectangle would crop half a letter without failing.
    px = menu.load()
    xs, ys = [], []
    for x in range(menu.width):
        for y in range(60, min(200, menu.height)):
            r, g, b, _ = px[x, y]
            if r > 200 and g > 160 and b < 80:
                xs.append(x)
                ys.append(y)
    if not xs:
        sys.exit("could not find the gold title in docs/img/main-menu.png -- "
                 "has the menu accent colour changed?")

    title = menu.crop((min(xs), min(ys), max(xs) + 1, max(ys) + 1))
    tp = title.load()
    for y in range(title.height):
        for x in range(title.width):
            r, g, b, _ = tp[x, y]
            tp[x, y] = (r, g, b, 255 if (r > 120 and g > 90 and b < 110) else 0)
    return title


def main() -> int:
    os.makedirs(OUT, exist_ok=True)

    # ---- plain cover ----
    world = Image.open(need("world-map.png"))
    cover = world.crop(COVER_CROP).resize(COVER_SIZE, Image.LANCZOS).convert("RGBA")
    cover.convert("RGB").save(os.path.join(OUT, "cover.png"))

    # ---- titled cover ----
    titled = cover.copy()
    top, bottom = 300, 430
    band = Image.new("RGBA", titled.size, (0, 0, 0, 0))
    bp = band.load()
    for y in range(top, bottom):
        # Softly peaked rather than a flat bar: a hard-edged strip across a map
        # looks like a UI element that failed to load.
        t = 1.0 - abs((y - (top + bottom) / 2) / ((bottom - top) / 2))
        a = int(215 * max(0.0, t) ** 0.6)
        for x in range(titled.width):
            bp[x, y] = (3, 5, 12, a)
    titled = Image.alpha_composite(titled, band)

    mark = wordmark()
    w = int(titled.width * 0.86)
    h = int(mark.height * (w / mark.width))
    mark = mark.resize((w, h), Image.LANCZOS)
    titled.alpha_composite(mark, ((titled.width - w) // 2, (top + bottom) // 2 - h // 2))
    titled.convert("RGB").save(os.path.join(OUT, "cover-titled.png"))

    # ---- page background ----
    gif = Image.open(need("timelapse-political.gif"))
    gif.seek(gif.n_frames - 1)              # the most territory decided
    bg = gif.convert("RGB").resize((1920, 960), Image.LANCZOS)
    bg = ImageEnhance.Brightness(bg).enhance(0.30)
    bg = ImageEnhance.Color(bg).enhance(0.75)   # muted, so screenshots stay the focus
    bg.save(os.path.join(OUT, "page-background.png"))

    for f in ("cover.png", "cover-titled.png", "page-background.png"):
        p = os.path.join(OUT, f)
        print(f"  {f:22} {Image.open(p).size[0]}x{Image.open(p).size[1]}"
              f"  ({os.path.getsize(p) // 1024} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
