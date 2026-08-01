#!/usr/bin/env python3
"""
Draw the symbols that cannot be downloaded.

WHY THESE ARE DRAWN AND NOT FETCHED

tools/download_symbols.py takes the geometric symbols from Wikimedia Commons.
The ones here are PICTORIAL -- a rose, a torch, a tree -- and Commons artwork
for those is heraldic linework that nanosvg, the game's rasteriser, renders as
noise at the size a flag symbol is actually drawn. Measured: the Commons rose
filled 90% of the canvas as one blob, the fasces was unreadable, a heraldic
eagle was 117 KB that came out as static.

So they are drawn instead, to the one rule that matters at 32px: few shapes,
big shapes, and nothing that depends on detail surviving. Every symbol here is
a white silhouette in viewBox "-100 -100 200 200", centred on the origin, and
is checked by rasterising it -- looking at the source is not checking.

    python3 tools/generate_symbols.py          # write them all
    python3 tools/generate_symbols.py rose     # just one
"""

import math
import os
import sys
from xml.etree import ElementTree

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(os.path.dirname(TOOLS_DIR), "data", "symbols")

HEAD = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">'


def wrap(body, note):
    # "--" cannot appear inside an XML comment. It is easy to type in prose and
    # the result is a file that renders perfectly in nanosvg -- which does not
    # parse XML strictly -- while being invalid to everything else, which is
    # exactly how the broken star_4 went unnoticed. Collapsed, not rejected:
    # the note is documentation, not data.
    note = note.replace("--", "-")
    out = (f"{HEAD}\n  <!-- {note}\n"
           f"       Drawn by tools/generate_symbols.py. Regenerate rather than\n"
           f"       hand-edit, and rasterise the result before trusting it. -->\n"
           f'  <g fill="#fff">\n{body}\n  </g>\n</svg>\n')
    # Never write a symbol that is not well-formed XML.
    ElementTree.fromstring(out)
    return out


def star_points(cx, cy, outer, inner=None, points=5, rot=-90.0):
    """Points of a star, as an SVG polygon 'points' string."""
    inner = inner if inner is not None else outer * 0.382   # golden, for a 5-point
    out = []
    for i in range(points * 2):
        r = outer if i % 2 == 0 else inner
        a = math.radians(rot + i * 180.0 / points)
        out.append(f"{cx + r * math.cos(a):.2f},{cy + r * math.sin(a):.2f}")
    return " ".join(out)


# ── the symbols ─────────────────────────────────────────────────────────────

def circle_stars():
    # Twelve stars in a ring. The old one drew each star with <use>, which
    # nanosvg does not implement, so it rendered blank; each is emitted in full
    # here for that reason -- the repetition IS the fix.
    ring, star = 66.0, 15.0
    body = "\n".join(
        f'    <polygon points="{star_points(ring * math.cos(math.radians(-90 + i * 30)), ring * math.sin(math.radians(-90 + i * 30)), star)}"/>'
        for i in range(12))
    return body, "Twelve five-pointed stars in a ring."


def crescent_star():
    # The crescent is the same path as crescent.svg -- see that file for where
    # (62.5, +/-67.7772) comes from.
    #
    # The star was hand-typed and irregular, and worse, it sat ON the crescent's
    # inner edge: two white shapes that touch are one white shape, so it fused
    # into the body and read as a lump rather than a star. A star-and-crescent
    # puts the star IN the opening. At y=0 the crescent body ends at x=-25
    # (the bite circle's left edge, 45-70), so a regular star centred at x=32
    # with a 30 radius clears it by 27 and still sits inside the outer circle.
    crescent = ('    <path d="M 62.5,-67.7772 A 80,80 0 1,0 62.5,67.7772 '
                'A 70,70 0 1,1 62.5,-67.7772 Z"/>')
    star = f'    <polygon points="{star_points(32.0, 0.0, 30.0)}"/>'
    return crescent + "\n" + star, "Star and crescent; the star sits in the opening, clear of the body."


def swastika():
    # Four bent arms, stroked rather than filled: as outlines the bends stay
    # even at any size, where a filled polygon of the same shape needs 20
    # points and goes wrong the moment one is off. The old file was a solid
    # rectangle -- not a swastika at all.
    a = 58.0
    body = (f'    <g fill="none" stroke="#fff" stroke-width="22" stroke-linecap="butt">\n'
            f'      <path d="M {-a},{-a} L {-a},0 L {a},0 L {a},{a}"/>\n'
            f'      <path d="M {-a},{a} L 0,{a} L 0,{-a} L {a},{-a}"/>\n'
            f'    </g>')
    return body, "Swastika. Censored in game -- see Game::buildRebelFlag."


def hammer_sickle():
    # The sickle as one thick stroked arc with a handle, the hammer as a bar
    # and a head across it. Anything more is detail that dies at 32px.
    # Sickle sweeping left-to-bottom, hammer laid across it the other way. The
    # first attempt crossed them through the middle and the two merged into one
    # unreadable mass; keeping the hammer head clear of the blade's arc is what
    # lets both still be two objects at 32px.
    body = ('    <g fill="none" stroke="#fff" stroke-width="16" stroke-linecap="round">\n'
            '      <path d="M -70,-30 A 72,72 0 0 1 30,52"/>\n'      # sickle blade
            '      <path d="M 30,52 L 58,72"/>\n'                    # sickle handle
            '      <path d="M -58,72 L 22,-30"/>\n'                  # hammer shaft
            '    </g>\n'
            '    <polygon points="6,-44 54,-30 42,-2 -2,-18"/>')     # hammer head, clear of the arc
    return body, "Hammer and sickle, kept apart so both read at small sizes."


def mountain():
    # Two peaks, and the notch between them is the whole symbol -- one peak is
    # a triangle, two is a mountain range. The first attempt put a snow cap on
    # top as a separate polygon and it was invisible: white on white does not
    # show, and in a single-colour silhouette the only way to draw an interior
    # line is to leave a hole. So the peaks are spaced instead, far enough that
    # the notch survives at 32px.
    body = ('    <polygon points="-46,-14 -86,58 -6,58"/>\n'         # lesser peak, left
            '    <polygon points="24,-72 -20,58 82,58"/>')           # main peak, right
    return body, "Two peaks; the notch between them is what makes it a range."


def rose():
    # A rose read from above: a ring of petals round a centre. Petals as
    # circles because at symbol size a petal outline is one blob anyway, and
    # five blobs in a ring is legible where a drawn flower is not.
    # The petals must NOT touch. At r=30 on a radius of 40 they overlapped and
    # the whole flower rasterised as one round blob -- five white circles that
    # meet are one white shape. Spaced so a gap survives: centres 51.7 apart,
    # diameter 50, and the centre disc clear of them by 4.
    ring, petal, eye = 44.0, 25.0, 15.0
    petals = "\n".join(
        f'    <circle cx="{ring * math.cos(math.radians(-90 + i * 72)):.2f}" '
        f'cy="{ring * math.sin(math.radians(-90 + i * 72)):.2f}" r="{petal}"/>'
        for i in range(5))
    body = (f'{petals}\n'
            f'    <circle cx="0" cy="0" r="{eye}"/>')
    return body, "Rose face-on: five petals round a centre, deliberately not touching."


def sword():
    # Point up. The blade tapers rather than being a bar, which is the only
    # thing that stops it reading as a plain cross at small sizes.
    body = ('    <polygon points="0,-86 13,-56 13,34 -13,34 -13,-56"/>\n'   # blade
            '    <rect x="-44" y="34" width="88" height="16" rx="4"/>\n'    # crossguard
            '    <rect x="-9" y="50" width="18" height="30"/>\n'            # grip
            '    <circle cx="0" cy="84" r="12"/>')                          # pommel
    return body, "Sword, point up."


def torch():
    # Flame, bowl, handle. The flame is one closed curve: two arcs meeting at
    # a point, so it stays a flame rather than becoming a blob.
    body = ('    <path d="M 0,-88 C 26,-58 34,-40 34,-24 '
            'C 34,-4 18,6 0,6 C -18,6 -34,-4 -34,-24 '
            'C -34,-42 -20,-52 -10,-36 C -12,-56 -8,-72 0,-88 Z"/>\n'
            '    <polygon points="-30,14 30,14 20,38 -20,38"/>\n'          # bowl
            '    <rect x="-9" y="38" width="18" height="50" rx="3"/>')     # handle
    return body, "Torch: flame, bowl and handle."


def tree():
    # A conifer: three tiers, each wider than the last. Tiers read as a tree at
    # sizes where a round canopy reads as a lollipop.
    body = ('    <polygon points="0,-86 -30,-30 30,-30"/>\n'
            '    <polygon points="0,-58 -44,10 44,10"/>\n'
            '    <polygon points="0,-28 -58,48 58,48"/>\n'
            '    <rect x="-11" y="44" width="22" height="42"/>')
    return body, "Conifer: three tiers and a trunk."


def star7():
    """FlagRenderer maps SymbolType::STAR_7 to star7.svg, but no such file was
    ever drawn, so every seven-pointed star silently rendered as nothing --
    Iraq's flag came out as a bare tricolour."""
    return ('    <polygon points="%s"/>'
            % star_points(0, 0, 92, 42, points=7),
            "Seven-pointed star, as on the Iraqi and Jordanian flags.")


SYMBOLS = {
    "circle_stars": circle_stars, "crescent_star": crescent_star,
    "star7": star7,
    "swastika": swastika,
    "hammer_sickle": hammer_sickle, "mountain": mountain, "rose": rose,
    "sword": sword, "torch": torch, "tree": tree,
}


def main():
    wanted = sys.argv[1:] or list(SYMBOLS)
    bad = [w for w in wanted if w not in SYMBOLS]
    if bad:
        print(f"unknown: {', '.join(bad)}", file=sys.stderr)
        return 2
    for name in wanted:
        body, note = SYMBOLS[name]()
        path = os.path.join(OUT_DIR, name + ".svg")
        with open(path, "w", encoding="utf-8") as f:
            f.write(wrap(body, note))
        print(f"  wrote {name}.svg")
    return 0


if __name__ == "__main__":
    sys.exit(main())
