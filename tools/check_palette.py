#!/usr/bin/env python3
"""Can the relation colours still be told apart without normal colour vision?

The relations view paints war, alliance, guarantee and non-aggression onto the
map and nothing but the hue says which is which. This simulates the three
common deficiencies and reports the closest pair under each, so "colour-blind
safe" is a measurement rather than a claim.

The simulation is Brettel/Vienot-style: linear RGB -> LMS, collapse the missing
cone onto the plane the remaining two span, and back. Distances are CIE76 in
Lab, which is close enough for "are these two obviously different".

    python3 tools/check_palette.py
"""

import sys

# Must match odPalette::relation in src/Palette.cpp.
# Every swatch in the legend, per mode, mirroring src/Palette.cpp. Checking
# only the four relations is how an early version shipped an Alliance blue
# almost identical to Self's.
ORIGINAL = {"Self": (0, 100, 255), "War": (255, 50, 50),
            "Alliance": (50, 200, 50), "Guarantee": (255, 255, 50),
            "Non-aggression": (255, 165, 0), "Neutral": (80, 80, 80)}

MODES = {
    "deutan": {"Self": (85, 51, 255), "War": (221, 34, 51),
               "Alliance": (255, 255, 238), "Guarantee": (255, 255, 0),
               "Non-aggression": (187, 153, 255), "Neutral": (80, 80, 80)},
    "protan": {"Self": (51, 85, 255), "War": (255, 51, 0),
               "Alliance": (0, 255, 204), "Guarantee": (221, 255, 0),
               "Non-aggression": (51, 187, 255), "Neutral": (80, 80, 80)},
    "tritan": {"Self": (0, 136, 221), "War": (255, 34, 34),
               "Alliance": (238, 170, 85), "Guarantee": (221, 255, 68),
               "Non-aggression": (170, 51, 85), "Neutral": (80, 80, 80)},
}
SAFE = MODES["deutan"]

# Below this, two patches on a map read as the same colour.
MIN_DELTA_E = 20.0


def srgb_to_linear(c):
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def linear_to_srgb(c):
    c = max(0.0, min(1.0, c))
    return 12.92 * c if c <= 0.0031308 else 1.055 * (c ** (1 / 2.4)) - 0.055


def simulate(rgb, kind):
    r, g, b = (srgb_to_linear(float(v)) for v in rgb)
    # RGB -> LMS (Hunt-Pointer-Estevez, normalised)
    L = 0.31399 * r + 0.63951 * g + 0.04649 * b
    M = 0.15537 * r + 0.75789 * g + 0.08670 * b
    S = 0.01775 * r + 0.10944 * g + 0.87262 * b
    if kind == "protan":
        L = 1.05118294 * M - 0.05116099 * S
    elif kind == "deutan":
        M = 0.9513092 * L + 0.04866992 * S
    elif kind == "tritan":
        S = -0.86744736 * L + 1.86727089 * M
    r2 = 5.47221206 * L - 4.6419601 * M + 0.16963708 * S
    g2 = -1.1252419 * L + 2.29317094 * M - 0.1678952 * S
    b2 = 0.02980165 * L - 0.19318073 * M + 1.16364789 * S
    return tuple(linear_to_srgb(v) * 255.0 for v in (r2, g2, b2))


def to_lab(rgb):
    r, g, b = (srgb_to_linear(float(v)) for v in rgb)
    x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047
    y = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 1.00000
    z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883
    f = lambda t: t ** (1 / 3) if t > 0.008856 else (7.787 * t + 16 / 116)
    fx, fy, fz = f(x), f(y), f(z)
    return (116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz))


def delta_e(a, b):
    la, lb = to_lab(a), to_lab(b)
    return sum((x - y) ** 2 for x, y in zip(la, lb)) ** 0.5


def report(name, palette):
    print(f"\n=== {name} ===")
    worst_overall = (1e9, "", "", "")
    for kind in ("normal", "protan", "deutan", "tritan"):
        keys = list(palette)
        worst = (1e9, "", "")
        for i in range(len(keys)):
            for j in range(i + 1, len(keys)):
                a = palette[keys[i]] if kind == "normal" else simulate(palette[keys[i]], kind)
                b = palette[keys[j]] if kind == "normal" else simulate(palette[keys[j]], kind)
                d = delta_e(a, b)
                if d < worst[0]:
                    worst = (d, keys[i], keys[j])
        flag = "" if worst[0] >= MIN_DELTA_E else "   <-- INDISTINGUISHABLE"
        print(f"  {kind:7} closest pair dE {worst[0]:6.1f}  "
              f"{worst[1]} / {worst[2]}{flag}")
        if worst[0] < worst_overall[0]:
            worst_overall = (worst[0], kind, worst[1], worst[2])
    return worst_overall


def swatches(path):
    """Draw both palettes as they look under each deficiency.

    A number says the colours are far apart; a picture says whether they look
    it. Both are worth having -- the numbers caught two collisions the eye
    missed, and the picture catches a palette that is separable but ugly.
    """
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("PIL not installed; skipping the image")
        return
    kinds = ["normal", "protan", "deutan", "tritan"]
    names = list(SAFE)
    cw, ch, pad, lab = 120, 54, 8, 130
    W = lab + len(names) * (cw + pad)
    H = 28 + 4 * (len(kinds) * (ch + pad) + 40)
    img = Image.new("RGB", (W, H), (16, 16, 20))
    d = ImageDraw.Draw(img)
    y = 8
    for title, pal in [("OFF", ORIGINAL)] + [(k.upper(), p) for k, p in MODES.items()]:
        d.text((8, y), title, fill=(235, 235, 240))
        y += 22
        for x, n in enumerate(names):
            d.text((lab + x * (cw + pad), y), n[:14], fill=(150, 150, 160))
        y += 14
        for kind in kinds:
            d.text((8, y + ch // 2 - 4), kind, fill=(190, 190, 200))
            for x, n in enumerate(names):
                c = pal[n] if kind == "normal" else simulate(pal[n], kind)
                c = tuple(int(max(0, min(255, v))) for v in c)
                d.rectangle([lab + x * (cw + pad), y,
                             lab + x * (cw + pad) + cw, y + ch], fill=c)
            y += ch + pad
        y += 18
    img.save(path)
    print(f"wrote {path}")


def main():
    if "--image" in sys.argv:
        swatches(sys.argv[sys.argv.index("--image") + 1])

    print("=== the ordinary palette, under each deficiency ===")
    worst_orig = 1e9
    for kind in ("normal", "protan", "deutan", "tritan"):
        d, a, b = closest(ORIGINAL, kind)
        flag = "" if d >= MIN_DELTA_E else "   <-- INDISTINGUISHABLE"
        print(f"  {kind:7} dE {d:6.1f}  {a} / {b}{flag}")
        worst_orig = min(worst_orig, d)

    print("\n=== each mode against the deficiency it is for ===")
    ok = True
    for kind, pal in MODES.items():
        # Its own deficiency is what it must survive; normal vision is checked
        # too, because somebody else may be watching the same screen.
        worst = min(closest(pal, kind)[0], closest(pal, "normal")[0])
        d, a, b = closest(pal, kind)
        flag = "" if worst >= MIN_DELTA_E else "   <-- INDISTINGUISHABLE"
        print(f"  {kind:7} dE {d:6.1f}  {a} / {b}{flag}")
        if worst < MIN_DELTA_E:
            ok = False
    print(f"\nfloor is {MIN_DELTA_E}; the ordinary palette bottoms out at "
          f"{worst_orig:.1f}")
    return 0 if ok else 1


def closest(pal, kind):
    keys = list(pal)
    worst = (1e9, "", "")
    for i in range(len(keys)):
        for j in range(i + 1, len(keys)):
            a = pal[keys[i]] if kind == "normal" else simulate(pal[keys[i]], kind)
            b = pal[keys[j]] if kind == "normal" else simulate(pal[keys[j]], kind)
            d = delta_e(a, b)
            if d < worst[0]:
                worst = (d, keys[i], keys[j])
    return worst


if __name__ == "__main__":
    sys.exit(main())
