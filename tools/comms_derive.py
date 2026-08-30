#!/usr/bin/env python3
"""Derive extra speakers from one character's drawing.

    python3 tools/comms_derive.py data/characters-src/concept.jpg data/comms

A cast needs several people and there is art for one. These two are built out
of that art rather than invented beside it, which is the only way they end up
in the same style:

  unknown.png   a GENERIC bust in the dark -- head, neck, shoulders, a lit rim
                and a question mark. Deliberately not derived: a silhouette
                taken from the character is recognisably that character, hat
                and coat and all, which is the opposite of unknown.

  officer.png   drawn from scratch in the same primitives the character is
                made of -- flat tones, one hard black line -- using HIS
                palette, sampled off his own signal. An army cap, hair, and a
                small smile. Deriving her from his face gave somebody who was
                obviously him in a wig.

WHAT IS INVENTED here is small and deliberate: two flat hair shapes, one
curve for a smile, one question mark. Everything else is the original's own
pixels. In a flat style with a hard black line those additions are the same
primitives the drawing is already made of, which is why this works at all --
it would not work on a painted character.

Output is GREYSCALE with alpha, like every signal: the accent colour is the
filter's job, never the art's.
"""
import argparse
import math
import os
import sys
from collections import deque

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

# Measured off the source once. Everything below is expressed against these.
HEAD = (520, 268, 122)          # centre x, centre y, radius
EYES = ((472, 245), (570, 245))
MOUTH_BAND = (468, 300, 594, 342)
CROP = (300, 58, 740, 612)


def key_background(img, tolerance=30):
    """Flood in from the border; what it cannot reach is the character."""
    w, h = img.size
    px = img.load()
    rows = [(px[0, y], px[w - 1, y]) for y in range(h)]

    def close(c, ref):
        return sum((a - b) ** 2 for a, b in zip(c[:3], ref[:3])) <= tolerance * tolerance

    seen = bytearray(w * h)
    q = deque()
    for x in range(w):
        for y in (0, h - 1):
            q.append((x, y)); seen[y * w + x] = 1
    for y in range(h):
        for x in (0, w - 1):
            q.append((x, y)); seen[y * w + x] = 1
    while q:
        x, y = q.popleft()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if not (0 <= nx < w and 0 <= ny < h) or seen[ny * w + nx]:
                continue
            l, r = rows[ny]
            if close(px[nx, ny], l) or close(px[nx, ny], r):
                seen[ny * w + nx] = 1
                q.append((nx, ny))
    a = np.frombuffer(bytes(seen), dtype=np.uint8).reshape(h, w)
    return Image.fromarray(((1 - a) * 255).astype(np.uint8), "L")


def catmull(points, samples=20):
    p = [points[0]] + list(points) + [points[-1]]
    out = []
    for i in range(len(p) - 3):
        p0, p1, p2, p3 = p[i:i + 4]
        for s in range(samples):
            t = s / samples
            t2, t3 = t * t, t * t * t
            out.append((
                0.5 * ((2 * p1[0]) + (-p0[0] + p2[0]) * t +
                       (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 +
                       (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3),
                0.5 * ((2 * p1[1]) + (-p0[1] + p2[1]) * t +
                       (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 +
                       (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3)))
    out.append(points[-1])
    return out


def grey_of(src, lift=0.06, contrast=1.25):
    g = np.asarray(src.convert("L"), dtype=np.float32) / 255.0
    g = np.clip((g - 0.5) * contrast + 0.5, 0.0, 1.0)
    return Image.fromarray(((lift + (1 - lift) * g) * 255).astype(np.uint8), "L")


def paint_out(lum, box, pad=6):
    """Rebuild a band of skin by blending the rows above and below it."""
    x0, y0, x1, y1 = box
    a = np.asarray(lum, dtype=np.float32).copy()
    top = a[max(0, y0 - 3):y0, x0:x1].mean(axis=0)
    bot = a[y1:y1 + 3, x0:x1].mean(axis=0)
    for i in range(y1 - y0):
        t = (i + 0.5) / (y1 - y0)
        a[y0 + i, x0:x1] = top * (1 - t) + bot * t
    return Image.fromarray(a.clip(0, 255).astype(np.uint8), "L")


# ── the unknown speaker ───────────────────────────────────────────────

def make_unknown(src, alpha, out_path):
    """A figure nobody can place.

    NOT derived from the drawing. The first version took the character's own
    silhouette and put the light out, and it was instantly recognisable --
    the fedora, the wide coat, the raised arms. An unknown contact has to be
    unplaceable, so this is a generic bust: no hat, no coat, no gesture, just
    a head, a neck and a pair of shoulders.

    Generic is not the same as crude, though. The head is an egg that narrows
    to a jaw rather than a circle, the neck runs into the trapezius instead of
    meeting the shoulders at a corner, and the shoulders fall away in a curve.
    Those three things are what separate a person in the dark from a cardboard
    cut-out, and they cost about twenty numbers.
    """
    w, h = CROP[2] - CROP[0], CROP[3] - CROP[1]     # same canvas as the others
    mask = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(mask)

    cx = w * 0.5
    head_cy, head_rx, head_ry = h * 0.36, w * 0.185, h * 0.170

    # The head: an ellipse whose lower half narrows, which is the whole
    # difference between a head and a ball.
    egg = []
    for i in range(96):
        t = i * math.tau / 96
        narrow = 1.0 - 0.20 * max(0.0, math.sin(t))
        egg.append((cx + math.cos(t) * head_rx * narrow,
                    head_cy + math.sin(t) * head_ry))
    d.polygon(egg, fill=255)

    # Neck into shoulders, as one shape. The join is a curve, not a corner:
    # a rectangle on a circle is what reads as a cut-out.
    jaw = head_cy + head_ry * 0.72
    # A narrow neck that widens late. Curving out from the jaw straight away
    # is what turned the first attempt into a bowling pin.
    body_r = [
        (cx + w * 0.058, jaw - h * 0.012),
        (cx + w * 0.064, jaw + h * 0.048),
        (cx + w * 0.112, jaw + h * 0.082),
        (cx + w * 0.232, jaw + h * 0.120),
        (cx + w * 0.352, jaw + h * 0.174),
        (cx + w * 0.442, jaw + h * 0.256),
        (cx + w * 0.492, jaw + h * 0.380),
        (cx + w * 0.505, h + 10.0),
    ]
    body_l = [(2 * cx - x, y) for x, y in reversed(body_r)]
    d.polygon([(p[0], p[1]) for p in catmull(body_l + body_r, samples=14)], fill=255)

    a = np.asarray(mask, dtype=np.float32) / 255.0
    lum = np.full((h, w), 30.0, dtype=np.float32)

    # The rim: the figure's own boundary, lit. Same trick as before -- it is
    # what makes a shape read as an object with an edge rather than a hole.
    solid = Image.fromarray((a > 0.5).astype(np.uint8) * 255, "L")
    rim = np.asarray(solid.filter(ImageFilter.MaxFilter(9)), dtype=np.float32) - \
          np.asarray(solid, dtype=np.float32)
    rim = np.asarray(Image.fromarray(rim.clip(0, 255).astype(np.uint8), "L")
                     .filter(ImageFilter.GaussianBlur(2.5)), dtype=np.float32) / 255.0
    lum = np.maximum(lum, rim * 205.0)

    yy, xx = np.mgrid[0:h, 0:w]
    glow = np.clip(1.0 - (((xx - w * 0.34) / (w * 0.95)) ** 2 +
                          ((yy - h * 0.76) / (h * 0.75)) ** 2), 0, 1)
    lum = np.maximum(lum, glow * 70.0 * (a > 0.5))

    img = Image.fromarray(lum.clip(0, 255).astype(np.uint8), "L")
    d = ImageDraw.Draw(img)

    # The question mark, where the face is: a stroke, so it carries the same
    # weight as a drawn line rather than looking like a typed character.
    qx, qy, qs = cx, head_cy - h * 0.012, w * 0.027
    hook = [(qx - qs * 1.9, qy - qs * 1.9), (qx - qs * 1.5, qy - qs * 3.4),
            (qx + qs * 0.4, qy - qs * 3.8), (qx + qs * 1.8, qy - qs * 2.8),
            (qx + qs * 1.4, qy - qs * 1.1), (qx - qs * 0.1, qy - qs * 0.1),
            (qx - qs * 0.2, qy + qs * 1.4)]
    pts = catmull(hook)
    d.line(pts, fill=232, width=int(qs * 1.05), joint="curve")
    for p in (pts[0], pts[-1]):
        d.ellipse([p[0] - qs * 0.52, p[1] - qs * 0.52, p[0] + qs * 0.52, p[1] + qs * 0.52], fill=232)
    d.ellipse([qx - qs * 0.62, qy + qs * 2.6, qx + qs * 0.62, qy + qs * 3.85], fill=232)

    img = img.filter(ImageFilter.GaussianBlur(0.6))
    # The alpha has to cover the RIM as well as the body. Using the body mask
    # alone cut the lit edge off at exactly the pixel that made it read as an
    # edge, and the figure sank into the background.
    out_alpha = np.maximum(a, np.asarray(solid.filter(ImageFilter.MaxFilter(9)),
                                         dtype=np.float32) / 255.0)
    out_alpha = Image.fromarray((out_alpha * 255).clip(0, 255).astype(np.uint8), "L") \
                     .filter(ImageFilter.GaussianBlur(0.9))
    Image.merge("RGBA", (img, img, img, out_alpha)).save(out_path)
    print(f"  unknown  -> {out_path}  {w}x{h}  (generic bust, not derived)")


# ── the signals officer ───────────────────────────────────────────────
#
# Drawn, not derived. Patching the advisor's face gave a character who was
# obviously him in a wig; she is built from the same primitives his drawing is
# made of instead -- flat tones, one hard black line, no shading -- so she
# belongs to the same hand without being the same person.
#
# The palette is HIS, sampled off data/comms/advisor.png, so nothing here can
# drift out of the set: ink 16, coat 40, cap 50, skin 112, band 150, shirt 210.

INK   = 16
PEAK  = 26     # under the cap's peak, the darkest thing on her
TUNIC = 44
CAP   = 54
CAPHI = 88     # light along the crown
BAND  = 32     # the cap band, darker than the crown above it
HAIR  = 70
HAIRHI = 104
SKIN  = 118
SHADE = 96     # under the jaw and the peak: the one shading tone
BADGE = 176
BOARD = 190
SHIRT = 214
LINE  = 6


def make_officer(out_path):
    """Drawn from scratch, and CONSTRUCTED rather than assembled.

    The earlier attempts stacked primitives -- a circle on a trapezoid, two
    slabs for hair -- and read as exactly that. This one is built the way a
    head is drawn: a cranium that narrows through the cheekbones to a jaw and
    a chin, ears on the eye line, a neck with a throat and a trapezius, and
    shoulders that slope. Everything after that is detail, which is the other
    half of what was missing: a cap with a seam and piping, collar tabs with
    rank pips, buttons, shoulder boards with stripes, and hair with a parting
    and strands rather than one flat tone.

    Her palette is his, sampled off data/comms/advisor.png.
    """
    w, h = CROP[2] - CROP[0], CROP[3] - CROP[1]
    img = Image.new("L", (w, h), 8)
    d = ImageDraw.Draw(img)
    cx = 220

    def curve(points, samples=16, **kw):
        d.polygon(catmull(points, samples=samples), **kw)

    def stroke(points, colour=INK, width=LINE, samples=16):
        pts = catmull(points, samples=samples)
        d.line(pts, fill=colour, width=width, joint="curve")
        for p in (pts[0], pts[-1]):
            d.ellipse([p[0] - width / 2, p[1] - width / 2,
                       p[0] + width / 2, p[1] + width / 2], fill=colour)

    # ── the tunic ─────────────────────────────────────────────────────
    # Shoulders that slope from the neck rather than a flat bar.
    d.polygon([(-20, h + 10), (-14, 470), (34, 424), (110, 384), (168, 366),
               (cx, 358), (272, 366), (330, 384), (406, 424), (454, 470),
               (460, h + 10)], fill=TUNIC)
    stroke([(-14, 470), (34, 424), (110, 384), (168, 366), (cx, 358),
            (272, 366), (330, 384), (406, 424), (454, 470)])

    # ── neck: throat, then the trapezius running into the shoulders ──
    curve([(cx - 36, 284), (cx - 40, 322), (cx - 72, 366), (cx + 72, 366),
           (cx + 40, 322), (cx + 36, 284)], fill=SKIN, outline=INK)
    # the shadow the jaw casts on it, which is what stops a neck reading as a post
    curve([(cx - 40, 292), (cx, 316), (cx + 40, 292), (cx + 36, 286), (cx - 36, 286)],
          fill=SHADE)

    # ── hair, behind the head ─────────────────────────────────────────
    curve([(cx - 30, 84), (cx - 130, 140), (cx - 154, 244), (cx - 142, 348),
           (cx - 104, 408), (cx - 66, 360), (cx - 78, 262), (cx - 66, 180),
           (cx, 160), (cx + 66, 180), (cx + 78, 262), (cx + 66, 360),
           (cx + 104, 408), (cx + 142, 348), (cx + 154, 244), (cx + 130, 140),
           (cx + 30, 84)], fill=HAIR, outline=INK)

    # ── the head: cranium -> cheekbone -> jaw -> chin ────────────────
    head = [(cx, 96), (cx + 62, 106), (cx + 90, 150), (cx + 94, 196),
            (cx + 84, 240), (cx + 58, 282), (cx, 302), (cx - 58, 282),
            (cx - 84, 240), (cx - 94, 196), (cx - 90, 150), (cx - 62, 106)]
    d.polygon(catmull(head + [head[0]], samples=18), fill=SKIN, outline=INK)
    d.line(catmull(head + [head[0]], samples=18), fill=INK, width=LINE, joint="curve")

    # Ears, on the eye line, tucked behind the hair's inner edge.
    for side in (-1, 1):
        d.ellipse([cx + side * 92 - 9, 196, cx + side * 92 + 9, 232],
                  fill=SKIN, outline=INK, width=4)

    for side in (-1, 1):
        stroke([(cx + side * 112, 176), (cx + side * 132, 254), (cx + side * 120, 332)],
               colour=HAIRHI, width=7)

    # ── the cap: crown, seam, piping, band, badge, peak ─────────────
    curve([(cx - 112, 100), (cx - 124, 62), (cx - 92, 34), (cx, 26),
           (cx + 92, 34), (cx + 124, 62), (cx + 112, 100)], fill=CAP, outline=INK)
    stroke([(cx - 92, 66), (cx - 34, 44), (cx + 36, 46)], colour=CAPHI, width=9)
    stroke([(cx - 110, 94), (cx, 86), (cx + 110, 94)], colour=CAPHI, width=4)   # piping
    d.rounded_rectangle([cx - 108, 96, cx + 108, 128], radius=7, fill=BAND,
                        outline=INK, width=LINE)
    curve([(cx - 138, 126), (cx - 106, 152), (cx - 54, 168), (cx, 172),
           (cx + 54, 168), (cx + 106, 152), (cx + 138, 126)], fill=PEAK, outline=INK)
    stroke([(cx - 128, 132), (cx, 158), (cx + 128, 132)], colour=SHADE, width=3)
    # badge: a small shield, not a dot
    d.polygon([(cx - 15, 100), (cx + 15, 100), (cx + 15, 116), (cx, 128), (cx - 15, 116)],
              fill=BADGE, outline=INK)

    # Hair escaping at the temples, drawn AFTER the cap. Put before it, the
    # peak covered every pixel of it and she looked bald under the brim.
    for side in (-1, 1):
        curve([(cx + side * 74, 150), (cx + side * 104, 158), (cx + side * 116, 196),
               (cx + side * 96, 186), (cx + side * 76, 168)], fill=HAIR, outline=INK)

    # ── collar, tie, buttons, boards ────────────────────────────────
    d.polygon([(cx - 78, 368), (cx, 386), (cx + 78, 368), (cx + 62, 420),
               (cx, 404), (cx - 62, 420)], fill=SHIRT, outline=INK)
    d.polygon([(cx - 20, 388), (cx + 20, 388), (cx + 15, 448), (cx, 476),
               (cx - 15, 448)], fill=BAND, outline=INK)
    for side in (-1, 1):   # collar tabs with a rank pip
        d.polygon([(cx + side * 80, 376), (cx + side * 114, 394),
                   (cx + side * 106, 414), (cx + side * 72, 396)],
                  fill=BOARD, outline=INK)
        d.ellipse([cx + side * 92 - 4, 392, cx + side * 92 + 4, 400], fill=INK)
    for side in (-1, 1):   # shoulder boards, two stripes each
        d.polygon([(cx + side * 158, 396), (cx + side * 222, 430),
                   (cx + side * 212, 456), (cx + side * 150, 422)],
                  fill=BOARD, outline=INK)
        for k in (0.36, 0.62):
            x0 = cx + side * (158 + 64 * k)
            d.line([(x0, 400 + 30 * k), (x0 - side * 8, 424 + 30 * k)], fill=INK, width=4)
    for by in (452, 500):  # buttons down the tunic
        d.ellipse([cx + 44, by, cx + 60, by + 16], fill=BADGE, outline=INK, width=3)

    # ── brows and a small smile ─────────────────────────────────────
    for side in (-1, 1):
        ex = cx + side * 42
        d.arc([ex - 30, 168, ex + 30, 206], 195, 345, fill=INK, width=5)
    stroke([(cx + 4, 216), (cx + 12, 240), (cx - 4, 246)], width=4)      # nose
    stroke([(cx - 32, 262), (cx - 15, 274), (cx, 276), (cx + 15, 274), (cx + 32, 262)],
           width=5)

    # The eyes are NOT drawn: the window puts live ones over this flat skin.
    img = img.filter(ImageFilter.GaussianBlur(0.4))
    a = np.asarray(img, dtype=np.float32)
    alpha = Image.fromarray(((a > 14) * 255).astype(np.uint8), "L").filter(ImageFilter.GaussianBlur(0.9))
    Image.merge("RGBA", (img, img, img, alpha)).save(out_path)
    print(f"  officer  -> {out_path}  {w}x{h}  (drawn; tones {int(a.min())}-{int(a.max())})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("outdir")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    src = Image.open(args.source).convert("RGB")
    alpha = key_background(src.copy()).filter(ImageFilter.GaussianBlur(0.8))

    make_unknown(src, alpha, os.path.join(args.outdir, "unknown.png"))
    make_officer(os.path.join(args.outdir, "officer.png"))


if __name__ == "__main__":
    main()
