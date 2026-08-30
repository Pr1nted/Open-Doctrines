#!/usr/bin/env python3
"""Turn a character drawing into a transmission signal.

    python3 tools/comms_signal.py concept.jpg data/comms/advisor.png \\
        --crop 300 60 740 612 --eye 472 245 --eye 570 245 --eye-size 46 30

WHAT A SIGNAL IS. The communication window feeds a LUMINANCE image through a
filter that colours it from the player's accent, so a character enters the
pipeline in grey and comes out in whatever colour the player picked. There is
no palette to maintain, and no recolouring by hand, ever -- which is the whole
reason the colour lives in the shader.

So this does three things and nothing else:

  1. keys the background out, so the figure can sit in the window's own lit
     space rather than bringing a rectangle of sky with it
  2. converts to luminance and lifts the contrast, because the filter's ramp
     expects the midtones to carry the picture
  3. paints out the eyes, and prints where they were

Step 3 is the important one. The eyes are the only part of a speaker that
moves, so they cannot be baked into the picture: the window draws them live
on top, blinking and looking around, at the coordinates printed here.
"""
import argparse
from collections import deque

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


def key_background(img, tolerance, min_pocket_override=None):
    """Flood in from the border; whatever it cannot reach is the character."""
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
    # ENCLOSED pockets of background, which the flood above cannot reach.
    # A figure with a hand on its hip encloses a triangle of sky between arm
    # and body; flooding only from the border keeps it, and it arrives in the
    # game as a patch of white inside her coat. So: any remaining region that
    # is the background's own colour and is bigger than a highlight is
    # background too.
    # The threshold has to sit between the two things it must tell apart. On
    # this character: the gaps under her arms are ~1780 px and the whites of
    # her eyes are ~690, so anything in between works and the eyes survive.
    # Delete the eyes here and they come back as transparent holes with the
    # window's own background showing through them.
    min_pocket = min_pocket_override or max(400, (w * h) // 1000)
    for sy in range(h):
        for sx in range(w):
            i = sy * w + sx
            if seen[i]:
                continue
            l, r = rows[sy]
            if not (close(px[sx, sy], l) or close(px[sx, sy], r)):
                continue
            pocket = []
            q2 = deque([(sx, sy)])
            seen[i] = 1
            while q2:
                x, y = q2.popleft()
                pocket.append(y * w + x)
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if not (0 <= nx < w and 0 <= ny < h) or seen[ny * w + nx]:
                        continue
                    ll, rr = rows[ny]
                    if close(px[nx, ny], ll) or close(px[nx, ny], rr):
                        seen[ny * w + nx] = 1
                        q2.append((nx, ny))
            if len(pocket) < min_pocket:
                for j in pocket:      # a highlight, not a hole: give it back
                    seen[j] = 0

    a = np.frombuffer(bytes(seen), dtype=np.uint8).reshape(h, w)
    return Image.fromarray(((1 - a) * 255).astype(np.uint8), "L")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("out")
    ap.add_argument("--crop", type=int, nargs=4, metavar=("X0", "Y0", "X1", "Y1"), required=True)
    ap.add_argument("--eye", type=int, nargs=2, action="append", metavar=("X", "Y"),
                    help="an eye centre in SOURCE pixels; give it twice")
    ap.add_argument("--eye-size", type=int, nargs=2, default=(46, 30), metavar=("W", "H"))
    ap.add_argument("--pupil", type=int, nargs=3, action="append", metavar=("X", "Y", "R"),
                    help="keep the DRAWN eye and paint out only the pupil. For art whose eyes "
                         "are worth keeping: the window then moves a pupil inside them instead "
                         "of rebuilding the whole eye, so the lashes and the rim stay the "
                         "artist's own. Use this INSTEAD of --eye.")
    ap.add_argument("--mouth", type=int, nargs=4, metavar=("X0", "Y0", "X1", "Y1"),
                    help="paint the mouth out too, for a speaker whose mouth is animated")
    ap.add_argument("--tolerance", type=int, default=30)
    ap.add_argument("--contrast", type=float, default=1.25)
    ap.add_argument("--min-pocket", type=int, default=None,
                    help="smallest enclosed background pocket to remove, in pixels; "
                         "must sit above the area of an eye white and below an arm gap")
    ap.add_argument("--chroma", type=float, default=0.38,
                    help="how much a saturated colour darkens; 0 is a flat response")
    ap.add_argument("--lift", type=float, default=0.06, help="black level, so the ramp has floor")
    args = ap.parse_args()

    loaded = Image.open(args.source)
    src = loaded.convert("RGB")
    if loaded.mode in ("RGBA", "LA") and np.asarray(loaded.convert("RGBA"))[:, :, 3].min() < 20:
        # The art already knows where it ends. Always prefer this: a keyed
        # edge carries a pale fringe that cannot be unmixed, and a flood
        # cannot reach a gap enclosed by an arm. tools/kra_export.py produces
        # such a file straight from a .kra by switching its background off.
        alpha = loaded.convert("RGBA").getchannel("A")
        print("  using the source's own alpha")
    else:
        alpha = key_background(src.copy(), args.tolerance, args.min_pocket)
        print("  no alpha in the source; keying the background out instead")

    x0, y0, x1, y1 = args.crop
    src = src.crop((x0, y0, x1, y1))
    alpha = alpha.crop((x0, y0, x1, y1)).filter(ImageFilter.GaussianBlur(0.8))
    w, h = src.size

    # NOT a plain luminance conversion. Standard weights make green the
    # BRIGHTEST channel, so a character in a green hat, a tan coat and pale
    # skin comes out as one flat bright shape -- which is what happened. This
    # is closer to how an early monochrome tube saw the world: reds carry,
    # greens fall back, and a saturated colour reads darker than a washed-out
    # one of the same brightness. That is what puts her hat, her coat and her
    # face on three separate tones.
    rgb = np.asarray(src, dtype=np.float32) / 255.0
    grey = 0.55 * rgb[:, :, 0] + 0.25 * rgb[:, :, 1] + 0.20 * rgb[:, :, 2]
    sat = rgb.max(axis=2) - rgb.min(axis=2)
    grey = grey * (1.0 - args.chroma * sat)
    grey = np.clip((grey - 0.5) * args.contrast + 0.5, 0.0, 1.0)
    grey = args.lift + (1.0 - args.lift) * grey
    lum = Image.fromarray((grey * 255).astype(np.uint8), "L")

    # Paint the eyes out with the skin around them: the window draws live ones
    # on top, and a baked pupil showing through a moving one looks like a fault.
    d = ImageDraw.Draw(lum)
    ew, eh = args.eye_size
    eyes = []
    for px_, py_, pr_ in (args.pupil or []):
        # Painted out with the white AROUND it, so what is left is her eye
        # with an empty sclera waiting for a pupil.
        cx, cy = px_ - x0, py_ - y0
        ring = [lum.getpixel((int(cx + dx), int(cy + dy)))
                for dx, dy in ((-pr_ * 1.9, 0), (pr_ * 1.9, 0), (0, -pr_ * 1.2), (0, pr_ * 1.2))
                if 0 <= cx + dx < w and 0 <= cy + dy < h]
        white = int(np.percentile(ring, 75)) if ring else 240
        d.ellipse([cx - pr_ * 1.25, cy - pr_ * 1.25, cx + pr_ * 1.25, cy + pr_ * 1.25], fill=white)
        print(f"  pupil  centre ({cx / w:.3f}, {cy / h:.3f})  r {pr_ / w:.4f}  white {white}")
    for ex, ey in (args.eye or []):
        cx, cy = ex - x0, ey - y0
        ring = [lum.getpixel((int(cx + dx), int(cy + dy)))
                for dx, dy in ((-ew, 0), (ew, 0), (0, -eh), (0, eh))
                if 0 <= cx + dx < w and 0 <= cy + dy < h]
        skin = int(np.median(ring)) if ring else 150
        d.ellipse([cx - ew * 0.72, cy - eh * 0.95, cx + ew * 0.72, cy + eh * 0.95], fill=skin)
        eyes.append((cx / w, cy / h))
    if args.mouth:
        # The mouth is animated for the same reason the eyes are: the window
        # draws it, shaped by what the speaker is feeling and by the glyph
        # being typed. A drawn-in mouth would sit under the live one.
        mx0, my0, mx1, my1 = args.mouth
        cxm, cym = (mx0 + mx1) / 2 - x0, (my0 + my1) / 2 - y0
        rw, rh = (mx1 - mx0) / 2, (my1 - my0) / 2
        ring = [lum.getpixel((int(cxm + dx), int(cym + dy)))
                for dx, dy in ((-rw * 1.6, 0), (rw * 1.6, 0), (0, -rh * 2.2), (0, rh * 2.2))
                if 0 <= cxm + dx < w and 0 <= cym + dy < h]
        d.ellipse([cxm - rw * 1.15, cym - rh * 1.5, cxm + rw * 1.15, cym + rh * 1.5],
                  fill=int(np.median(ring)) if ring else 150)
        print(f"  mouth  x {(mx0 - x0) / w:.3f}..{(mx1 - x0) / w:.3f}"
              f"  centre ({cxm / w:.3f}, {cym / h:.3f})  width {(mx1 - mx0) / w:.3f}")
    lum = lum.filter(ImageFilter.GaussianBlur(0.4))

    out = Image.merge("RGBA", (lum, lum, lum, alpha))
    out.save(args.out)

    print(f"wrote {args.out}  {w}x{h}")
    if eyes:
        gap = abs(eyes[0][0] - eyes[1][0])
        print("profile numbers, as fractions of the signal image:")
        print(f"  eyeGap  {gap:.3f}")
        print(f"  eyeY    {(eyes[0][1] + eyes[1][1]) / 2:.3f}")
        print(f"  eyeW    {ew / w:.3f}")
        print(f"  eyeH    {eh / h:.3f}")


if __name__ == "__main__":
    main()
