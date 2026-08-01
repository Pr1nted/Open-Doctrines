#!/usr/bin/env python3
"""
Make the web build's favicon from the application icon.

    python3 tools/generate_web_favicon.py

WHY THIS IS NOT JUST A COPY

shell.html asks for icon.png next to the page, and the obvious fix is to copy
data/Icon/icon.png there. That file is the APPLICATION icon: 1024x1024 and
663 KB, sized for a macOS dock. A browser draws a favicon at 16 or 32 CSS
pixels, so copying it means every visitor downloads two thirds of a megabyte to
paint sixteen pixels -- on the one build where load time is something the player
sits and watches.

128x128 is the largest a tab actually asks for (32pt at 4x), and comes out
around 20 KB. The result is committed as packaging/web/favicon.png rather than
generated during the build, so building the web target needs no image library.

Run this only when the application icon changes.
"""

import os
import sys

SRC = os.path.join("data", "Icon", "icon.png")
DST = os.path.join("packaging", "web", "favicon.png")
SIZE = 128


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)

    try:
        from PIL import Image
    except ImportError:
        print("Pillow is needed for this one:  python3 -m pip install Pillow",
              file=sys.stderr)
        return 1

    if not os.path.exists(SRC):
        print(f"missing {SRC}", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(DST), exist_ok=True)
    src = Image.open(SRC).convert("RGBA")
    src.resize((SIZE, SIZE), Image.LANCZOS).save(DST, optimize=True)

    print(f"  {SRC}  {os.path.getsize(SRC):>8,} B  {src.size[0]}x{src.size[1]}")
    print(f"  {DST}  {os.path.getsize(DST):>8,} B  {SIZE}x{SIZE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
