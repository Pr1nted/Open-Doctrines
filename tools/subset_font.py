#!/usr/bin/env python3
"""Cut a font down to the characters the game actually draws.

WHY: data/fonts/unifont.ttf is 11 MB because Unifont covers essentially the
whole Basic Multilingual Plane -- every CJK ideograph, every script the game has
no text in. Game::init() then rasterises about six hundred codepoints out of it
(Latin, Latin Extended-A, Cyrillic and a dozen punctuation marks) into one
atlas, and every glyph outside that list is unreachable at runtime whatever the
file contains. On desktop that waste is disk. On the web it is 11 MB of a
download the player waits on before the menu draws, for glyphs no draw call can
ever ask for.

So the web build subsets it. The ranges below are deliberately WIDER than the
list in Game.cpp -- whole Unicode blocks rather than the exact codepoints -- so
that adding a character to that list does not silently produce a box on the web
and nowhere else. The whole set is still about a thousand glyphs.

Used from CMakeLists.txt at configure time, against the STAGED web copy only;
data/fonts/unifont.ttf itself is never modified and desktop builds keep the full
file. Needs fontTools (pip install fonttools). If it is missing this exits
non-zero without writing anything and the build falls back to the full font, so
a machine without it produces a fat page rather than a broken one.

    python3 tools/subset_font.py in.ttf out.ttf
"""

import sys

# Whole blocks, matching the SCRIPTS the interface uses rather than the exact
# characters. See the module docstring for why this is wider than Game.cpp's
# list. Keep in step with that list only in the sense that every codepoint it
# names must fall inside one of these.
BLOCKS = [
    # Latin-1 unbroken, controls included. Nothing draws U+007F-U+009F, but
    # Game.cpp asks LoadFontEx for 32..255 as one range and the cheapest way to
    # guarantee this file is a strict superset of what it asks for is not to
    # carve a hole in the middle of it. Thirty-three glyphs.
    (0x0020, 0x00FF),   # Basic Latin + Latin-1 Supplement
    (0x0100, 0x017F),   # Latin Extended-A
    (0x0180, 0x024F),   # Latin Extended-B
    (0x0370, 0x03FF),   # Greek and Coptic
    (0x0400, 0x04FF),   # Cyrillic
    (0x0500, 0x052F),   # Cyrillic Supplement
    (0x2000, 0x206F),   # General Punctuation (the dashes and quotes)
    (0x20A0, 0x20BF),   # Currency Symbols
    (0x2190, 0x21FF),   # Arrows
    (0x2500, 0x257F),   # Box Drawing
    (0x25A0, 0x25FF),   # Geometric Shapes
]


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    src, dst = argv[1], argv[2]

    try:
        from fontTools import subset
    except ImportError:
        print("subset_font: fontTools is not installed "
              "(pip install fonttools)", file=sys.stderr)
        return 1

    unicodes = []
    for lo, hi in BLOCKS:
        unicodes.extend(range(lo, hi + 1))

    options = subset.Options()
    # The game rasterises glyphs itself and never asks the font for shaping,
    # kerning or layout, so everything that supports those is dead weight here.
    options.layout_features = []
    options.name_IDs = []
    options.notdef_outline = True     # the box IS the fallback; keep it drawable
    options.recalc_bounds = True
    options.drop_tables += ["DSIG"]

    font = subset.load_font(src, options)
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=unicodes)
    subsetter.subset(font)
    subset.save_font(font, dst, options)
    font.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
