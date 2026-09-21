#!/usr/bin/env python3
"""Build data/menu_bg.png, the main menu's land silhouette, from map.odmap.

    tools/make_menu_bg.py            write data/menu_bg.png
    tools/make_menu_bg.py --check    fail if the committed file is stale

The menu only needs the land/sea map's red channel at about screen size, but
the only copy the game had was the 8192x4096 one inside map.odmap. Decoding it
cost 128 MB of RGBA per call, and macOS kept those pages counted against the
game after they were freed: 187 MB of the menu's footprint was an image the
menu had finished with. This is the same picture at 4096x2048, one channel.

--check compares pixels, not file bytes, so a different zlib cannot fail it.
"""

import io
import os
import sys
import zipfile

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "data", "STDmaps", "map.odmap")
OUT = os.path.join(ROOT, "data", "menu_bg.png")
WIDTH = 4096   # initMenuBackground() keeps at most this much; see Game_Menus.cpp


def build():
    with zipfile.ZipFile(SRC) as z:
        full = Image.open(io.BytesIO(z.read("land_sea.png")))
        full.load()
    red = full.convert("RGB").getchannel("R")
    height = full.height * WIDTH // full.width
    small = red.resize((WIDTH, height), Image.Resampling.BOX)
    return Image.frombytes("L", small.size, small.tobytes())   # no source metadata


def main():
    img = build()
    if "--check" in sys.argv[1:]:
        if not os.path.exists(OUT):
            print(f"missing {os.path.relpath(OUT, ROOT)}: run tools/make_menu_bg.py")
            return 1
        have = Image.open(OUT)
        if have.mode != "L" or have.size != img.size or have.tobytes() != img.tobytes():
            print(f"{os.path.relpath(OUT, ROOT)} does not match map.odmap: "
                  "run tools/make_menu_bg.py")
            return 1
        print(f"ok  {os.path.relpath(OUT, ROOT)} matches map.odmap ({img.width}x{img.height})")
        return 0
    img.save(OUT, optimize=True)
    print(f"wrote {os.path.relpath(OUT, ROOT)} ({img.width}x{img.height}, "
          f"{os.path.getsize(OUT) // 1024} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
