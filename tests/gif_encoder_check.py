#!/usr/bin/env python3
"""Decode the GIFs GifEncoderTest wrote, with a decoder we did not write.

A bitstream writer cannot test itself: a GIF with a mis-sized LZW code still has
a valid header, still opens, and often still shows something. The only way to
know the stream is right is to hand it to an independent decoder and compare
against what went in. Pillow is that decoder here.

    tests/gif_encoder_check.py <dir-written-by-GifEncoderTest>

Skips itself if Pillow is unavailable rather than failing the suite, the same
way the wasm cases do.
"""
import os
import sys

try:
    from PIL import Image, ImageSequence
except ImportError:
    print("skip  Pillow not installed — GIF decode not verified")
    sys.exit(0)


def regen(name, w, h, t):
    """Re-derive the frames the C++ side generated, so we can compare."""
    if name in ("gradient.gif", "single.gif"):
        return [[(x * 255 // (w - 1), y * 255 // (h - 1), t * 40)
                 for x in range(w)] for y in range(h)]
    if name in ("motion.gif", "wide.gif"):
        return [[(220, 30, 30) if (t * 8 <= x < t * 8 + 16 and 4 <= y < 20)
                 else (20, 40, 90) for x in range(w)] for y in range(h)]
    if name == "flatmany.gif":
        rows = []
        for y in range(h):
            row = []
            for x in range(w):
                if x < w * 6 // 10:
                    row.append((8, 8, 41))
                else:
                    b = (y * 11 + x * 7) % 120
                    row.append((40 + (b * 17) % 200, 40 + (b * 53) % 200,
                                40 + (b * 97) % 200))
            rows.append(row)
        return rows
    if name == "flat.gif":
        return [[(17, 99, 200)] * w for _ in range(h)]
    if name == "noise.gif":
        # Same xorshift32 the C++ side uses. This is the frame that pushes the
        # LZW dictionary past 512 entries and so past the first code-size
        # increase -- the place an off-by-one in the increment would corrupt the
        # stream. Worth reproducing exactly rather than checking structure only.
        s = (0x9E3779B9 ^ t) & 0xFFFFFFFF
        rows = []
        for _ in range(h):
            row = []
            for _ in range(w):
                s ^= (s << 13) & 0xFFFFFFFF; s &= 0xFFFFFFFF
                s ^= s >> 17
                s ^= (s << 5) & 0xFFFFFFFF; s &= 0xFFFFFFFF
                row.append(((s >> 24) & 0xFF, (s >> 16) & 0xFF, (s >> 8) & 0xFF))
            rows.append(row)
        return rows
    return None


def main(argv):
    if not argv:
        print("usage: gif_encoder_check.py <dir>")
        return 2
    d = argv[0]
    manifest = os.path.join(d, "manifest.txt")
    if not os.path.exists(manifest):
        print(f"FAILED  no manifest in {d} — did GifEncoderTest run?")
        return 1

    failures = 0
    with open(manifest) as f:
        entries = [ln.split() for ln in f if ln.strip()]

    for name, w, h, frames, delay in entries:
        w, h, frames, delay = int(w), int(h), int(frames), int(delay)
        path = os.path.join(d, name)
        try:
            im = Image.open(path)
        except Exception as e:
            print(f"FAILED  {name}: will not open ({e})")
            failures += 1
            continue

        problems = []
        if im.size != (w, h):
            problems.append(f"size {im.size} != ({w}, {h})")

        # ImageSequence.Iterator yields the SAME Image object repositioned, so
        # list()-ing it gives N references that all show the last frame. Copy.
        seq = [fr.convert("RGB").copy() for fr in ImageSequence.Iterator(im)]
        if len(seq) != frames:
            problems.append(f"{len(seq)} frames, expected {frames}")

        # Netscape loop block: Pillow surfaces it as info["loop"].
        im.seek(0)
        if frames > 1 and im.info.get("loop", None) != 0:
            problems.append(f"loop flag is {im.info.get('loop')!r}, expected 0")
        # Delay is stored in milliseconds by Pillow, centiseconds in the file.
        got_ms = im.info.get("duration")
        if got_ms is not None and abs(got_ms - delay * 10) > 10:
            problems.append(f"delay {got_ms}ms, expected {delay * 10}ms")

        # Pixel fidelity. The encoder quantises to a 256-entry RGB555-derived
        # palette, so allow real quantisation error but nothing structural: a
        # displaced or torn frame blows well past this.
        for t, fr in enumerate(seq):
            want = regen(name, w, h, t)
            if want is None:
                break
            got = fr
            worst = 0
            for y in range(0, h, max(1, h // 8)):
                for x in range(0, w, max(1, w // 8)):
                    gr, gg, gb = got.getpixel((x, y))
                    wr, wg, wb = want[y][x]
                    worst = max(worst, abs(gr - wr), abs(gg - wg), abs(gb - wb))
            # Tolerance is per-content, because 256 colours genuinely cannot
            # hold some of these: a three-axis gradient and random noise both
            # quantise visibly, a two-colour flag does not. What the numbers rule
            # out is structural damage, which lands in the hundreds.
            limit = {"noise.gif": 64, "gradient.gif": 40}.get(name, 24)
            if worst > limit:
                problems.append(f"frame {t}: pixels off by up to {worst}")
                break

        # There was a "last frame is not a single colour" heuristic here, meant to
        # catch a short decode. It fired on wide.gif -- 300px across with a block
        # at x=24..39, sampled every 50px, so it never saw the block -- and it was
        # redundant anyway: a short decode shows up in the pixel comparison above
        # as an error in the hundreds. Removed rather than tuned.

        if problems:
            print(f"FAILED  {name}: " + "; ".join(problems))
            failures += 1
        else:
            print(f"ok      {name}: {w}x{h}, {frames} frame(s), {delay}cs, pixels match")

    if failures:
        print(f"\n{failures} GIF(s) did not survive a round trip")
        return 1
    print(f"\nall {len(entries)} GIFs decode correctly with an independent decoder")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
