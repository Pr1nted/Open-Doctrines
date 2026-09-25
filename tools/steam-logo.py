#!/usr/bin/env python3
"""The one Steam capsule tools/banner.py does not build: the library logo.

Steam lays this over the library hero, so unlike every other capsule it is
mostly transparent -- it is the wordmark and nothing else. Which is why it is
not in banner.py: that tool composes a map crop and stamps a title on it, and
here the map is the hero underneath and must show through.

The wordmark is lifted out of the game's own menu by tools/itch-cover.py, so
that function is imported rather than reimplemented. It is not typed in a
lookalike font: the title is drawn in raylib's built-in font, which nothing
else can load, and a near-miss reads as not-the-game to anyone who has seen a
screenshot.

    python3 tools/steam-logo.py        -> docs/steam/banner-steam-logo.png
"""
from __future__ import annotations

import importlib.util
import pathlib
import sys

from PIL import Image

W, H = 1280, 720
# Two thirds of the canvas, centred. Steam scales and positions the logo over
# the hero from the library settings, so what this width buys is headroom: a
# wordmark that already fills its canvas can only be made smaller there, and
# one with margin can go either way.
FILL = 0.66


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    cover = root / "tools" / "itch-cover.py"
    spec = importlib.util.spec_from_file_location("itch_cover", cover)
    ic = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ic)

    mark = ic.wordmark()
    w = round(W * FILL)
    mark = mark.resize((w, max(1, round(mark.height * w / mark.width))), Image.LANCZOS)

    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    out.paste(mark, ((W - mark.width) // 2, (H - mark.height) // 2), mark)

    dest = root / "docs" / "steam" / "banner-steam-logo.png"
    out.save(dest)
    print(f"{dest.relative_to(root)}: {W}x{H}, wordmark {mark.width}x{mark.height}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
