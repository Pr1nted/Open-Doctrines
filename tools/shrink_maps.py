#!/usr/bin/env python3
"""
Re-encode .odmap archives to the smallest form that loses nothing.

    python3 tools/shrink_maps.py                 # every map in data/STDmaps
    python3 tools/shrink_maps.py a.odmap b.odmap # those files
    python3 tools/shrink_maps.py --check         # report only, write nothing

WHY

Players said the maps were too big to move around, and they were right: a
scenario was six megabytes, thirty-seven for the six shipped ones. Almost none
of that was map data.

  political.png   4.70 MB   78.6%   the game never opens it
  provinces.png   0.60 MB   10.1%
  land_sea.png    0.28 MB    4.6%   one bit per pixel, stored as 32
  everything else 0.40 MB    6.7%

political.png is a *derived* layer. Game_Loading.cpp's needed[] list does not
name it, so loading a map never reads it; generatePoliticalTexture() draws the
board from provinces.png and countries.json instead, at load, every time. The
file was for the map editor, which now recomputes it with the identical
algorithm (MapEditor::computePoliticalGradient), and for thumbnails, which come
from the thumb.png every archive already carries. Nothing else asked for it.

land_sea.png is one bit of information per pixel -- LandSeaMap thresholds it
back down to "is this pixel land" the moment it loads -- shipped as RGBA
truecolour. Indexed at the depth its palette needs it decodes to byte-identical
RGBA, and stb_image expands PLTE/tRNS on the way in, so no reader changed.

Together: 37.0 MB -> 7.6 MB, and every pixel that survives is the pixel that
went in. odmap_pack.layer_png decodes each layer it writes and compares it
before returning, so a claim of losslessness is checked rather than asserted.

Older maps keep loading: nothing was renamed and nothing changed meaning, so a
build from before this still reads a map from after it, and vice versa.
"""

import argparse
import io
import os
import sys
import zipfile

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from odmap_pack import (DERIVED, layer_png, read_members,   # noqa: E402
                        write_odmap)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAPS_DIR = os.path.join(ROOT, "data", "STDmaps")

# Layers worth re-encoding. Everything else in the archive is JSON or a flag
# PNG small enough that deflate has already had the last word on it.
RASTERS = ("land_sea.png", "provinces.png")


def shrink(path, check=False):
    """Returns (before, after) bytes. Writes nothing when check is set."""
    before = os.path.getsize(path)
    members, dirs = read_members(path)

    for name in RASTERS:
        if name not in members:
            continue
        members[name] = layer_png(Image.open(io.BytesIO(members[name])))

    dropped = sum(len(members[d]) for d in DERIVED if d in members)

    if check:
        # Size the archive without touching the original.
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for name, data in members.items():
                if name in DERIVED:
                    continue
                z.writestr(name, data)
        return before, len(buf.getvalue()), dropped

    write_odmap(path, members, dirs)
    return before, os.path.getsize(path), dropped


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("maps", nargs="*", help="paths to .odmap files (default: data/STDmaps)")
    ap.add_argument("--check", action="store_true",
                    help="report what would be saved and write nothing")
    args = ap.parse_args()

    paths = args.maps
    if not paths:
        paths = sorted(os.path.join(MAPS_DIR, f) for f in os.listdir(MAPS_DIR)
                       if f.endswith(".odmap"))
    if not paths:
        print("no .odmap files found")
        return 1

    total_before = total_after = 0
    regrown = []
    for p in paths:
        if not os.path.exists(p):
            print(f"  {os.path.basename(p)}: no such file, skipped")
            continue
        before, after, dropped = shrink(p, args.check)
        total_before += before
        total_after += after
        note = f", dropped {dropped / 1e6:.2f} MB of derived layers" if dropped else ""
        print(f"  {os.path.basename(p):<14} {before / 1e6:6.2f} MB -> {after / 1e6:5.2f} MB "
              f"({after / before:5.1%}){note}")
        # SLACK covers the noise between one deflate run and the next. A map
        # that has genuinely reverted -- a tool writing truecolour land/sea
        # again, or putting political.png back -- is several times the size,
        # nowhere near this line.
        SLACK = 0.98
        if after < before * SLACK:
            regrown.append(os.path.basename(p))

    if total_before:
        verb = "would shrink" if args.check else "shrank"
        print(f"\n  {verb} {total_before / 1e6:.2f} MB -> {total_after / 1e6:.2f} MB "
              f"({total_after / total_before:.1%}, "
              f"{(total_before - total_after) / 1e6:.2f} MB saved)")

    if args.check and regrown:
        print(f"\n{len(regrown)} map(s) are not in their compact form: "
              f"{', '.join(regrown)}.\nRun tools/shrink_maps.py to re-encode them.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
