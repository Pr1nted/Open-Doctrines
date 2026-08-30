#!/usr/bin/env python3
"""
Re-encode PNGs to the same pixels in fewer bytes.

    python3 tools/shrink_pngs.py                    # every PNG git tracks
    python3 tools/shrink_pngs.py data/flags docs    # those files and trees
    python3 tools/shrink_pngs.py --check            # report only, write nothing

WHY

Fourteen megabytes of this repository is PNG: two hundred and fifty flags, the
app icon, the screenshots on the store pages. Every one of them was encoded by
whatever produced it -- an export from a drawing program, a download from
Wikimedia, the game's own screenshot key -- and none of those encoders was
trying very hard.

There are two knobs, and neither is a quality trade:

  * The scanline filter. PNG stores each row either raw or as a delta against
    its neighbours, one choice per row, and the choice is the encoder's. Most
    encoders make it with a heuristic that suits photographs; a flag, an icon
    or a map layer is flat regions of one colour, where the raw bytes are
    already long runs and every filter turns them into deltas. Trying all six
    strategies and keeping the smallest is worth 20-40% on flat art.

  * Zopfli. An ordinary deflate stream, found by searching much harder for it.
    Every PNG decoder in existence reads the result without knowing anything is
    different. Another 5-15%, at minutes per megabyte.

Both are internal to the PNG. The decoder puts them back, so the pixels that
come out are the pixels that went in -- which is checked, not asserted:
odmap_pack.optimize_png decodes what it is about to return and compares it,
and a file that does not come back identical is left exactly as it was. Every
other chunk is copied through byte for byte, so a tEXt chunk carrying an
author or a licence survives; this is not a metadata stripper.

.icns files are handled too. An app icon is a container of PNGs at half a dozen
sizes -- 1.3 MB of them, twice over, since the same icon sits in data/Icon and
in packaging/macos -- and each is re-encoded in place while every other member
(the JPEG-2000 and raw entries older macOS wants, the `info` record) is copied
untouched.

The .odmap archives are NOT handled here -- their PNGs live inside a zip and
are re-encoded by tools/shrink_maps.py, which knows how to rewrite the
archive around them.
"""

import argparse
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from odmap_pack import EFFORT_FAST, EFFORT_MAX, optimize_png   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Slack between one run and the next, and between a machine with zopfli and one
# without. --check is asking "has something dropped an unoptimised PNG in
# here", and such a file is tens of percent larger, nowhere near this line.
SLACK = 0.95


def tracked_images():
    """Every .png and .icns git knows about. Generated files and scratch are not ours."""
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z", "*.png", "*.PNG", "*.icns"],
                         capture_output=True, text=True, check=True).stdout
    return sorted(os.path.join(ROOT, p) for p in out.split("\0") if p)


def expand(paths):
    found = []
    for p in paths:
        if os.path.isdir(p):
            for base, _, names in os.walk(p):
                found += [os.path.join(base, n) for n in sorted(names)
                          if n.lower().endswith((".png", ".icns"))]
        elif p.lower().endswith((".png", ".icns")):
            found.append(p)
        else:
            print(f"  {p}: not a PNG, an .icns or a directory, skipped")
    return found


def optimize_icns(data, effort):
    """Re-encode the PNG members of an .icns, leaving every other member alone.

    The container is a header and a run of typed chunks, each `type` then a
    big-endian length that INCLUDES its own eight-byte header. Only the members
    that are PNG are touched; `is2x`-era JPEG-2000 entries, the ancient raw and
    RLE ones and the `info` record are copied through, since re-encoding those
    is a different problem and they are not where the megabyte is.
    """
    if data[:4] != b"icns" or len(data) < 8:
        return data
    body, p = b"", 8
    while p + 8 <= len(data):
        tag = data[p:p + 4]
        size = struct.unpack(">I", data[p + 4:p + 8])[0]
        if size < 8 or p + size > len(data):
            return data                     # malformed: leave it exactly as it is
        payload = data[p + 8:p + size]
        if payload[:8] == b"\x89PNG\r\n\x1a\n":
            payload = optimize_png(payload, effort)
        body += tag + struct.pack(">I", len(payload) + 8) + payload
        p += size
    if p != len(data):
        return data                         # trailing bytes we do not understand
    out = b"icns" + struct.pack(">I", len(body) + 8) + body
    return out if len(out) < len(data) else data


def shrink(path, check):
    """(before, after). Writes nothing when check is set, or when nothing helps."""
    with open(path, "rb") as f:
        data = f.read()
    effort = EFFORT_FAST if check else EFFORT_MAX
    out = optimize_icns(data, effort) if path.lower().endswith(".icns") \
        else optimize_png(data, effort)
    if not check and len(out) < len(data):
        # Same directory and a rename, so an interrupted run cannot leave a
        # half-written image where a whole one used to be.
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(out)
        os.replace(tmp, path)
    return len(data), len(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*",
                    help="PNG/.icns files or directories (default: git-tracked)")
    ap.add_argument("--check", action="store_true",
                    help="report what would be saved and write nothing")
    ap.add_argument("--quiet", action="store_true", help="only print the total")
    args = ap.parse_args()

    paths = expand(args.paths) if args.paths else tracked_images()
    if not paths:
        print("no images found")
        return 1

    total_before = total_after = 0
    fat = []
    for p in paths:
        rel = os.path.relpath(p, ROOT)
        if not args.check and not args.quiet:
            # Zopfli on a megabyte screenshot is a minute. Name the file being
            # worked on rather than looking hung.
            print(f"  {rel[:56]:<58} working...", end="", flush=True)
        before, after = shrink(p, args.check)
        total_before += before
        total_after += after
        if not args.check and not args.quiet:
            print("\r" + " " * 76 + "\r", end="")
        if after < before * SLACK:
            fat.append(rel)
            if not args.quiet:
                verb = "would shrink" if args.check else "shrank"
                print(f"  {rel[:56]:<58} {before:>9} -> {after:>9}  "
                      f"({after / before:5.1%}) {verb}")

    saved = total_before - total_after
    print(f"\n  {len(paths)} image(s): {total_before / 1e6:.2f} MB -> {total_after / 1e6:.2f} MB "
          f"({total_after / total_before:.1%}, {saved / 1e6:.2f} MB saved)")

    if args.check and fat:
        print(f"\n{len(fat)} image(s) are not in their compact form.\n"
              f"Run tools/shrink_pngs.py to re-encode them.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
