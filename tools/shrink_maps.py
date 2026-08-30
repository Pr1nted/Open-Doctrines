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

What was still being paid after that was the ENCODING of the layers that
survived, not their content. Both PNG writers in use picked a scanline filter
per row by a heuristic meant for photographs, which on flat map regions costs
more than it saves -- the world map's province layer was 18% smaller with no
filter at all -- and both stopped at zlib, where zopfli finds a smaller stream
of the very same kind. Applied to the layers and to every other image in the
archive (the thumbnail, a couple of hundred flags), that is another
7.69 MB -> 5.66 MB: near enough a quarter again off every map. Nothing on the
reading side knows the difference, because filtering and the deflate stream are
internal to a PNG. See tools/odmap_pack.py.

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
from odmap_pack import (DERIVED, EFFORT_FAST, EFFORT_MAX,   # noqa: E402
                        layer_png, optimize_png, read_members, write_odmap)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAPS_DIR = os.path.join(ROOT, "data", "STDmaps")

# The two layers whose ENCODING is chosen rather than inherited: they are built
# from pixel arrays, so layer_png gets to decide indexed against truecolour and
# at what bit depth. Every other PNG in the archive -- the thumbnail, a couple
# of hundred flags -- arrived already encoded by whatever produced it, and goes
# through optimize_png, which keeps its every chunk and re-does only the
# filtering and the deflate.
RASTERS = ("land_sea.png", "provinces.png")


def shrink(path, check=False):
    """Returns (before, after, dropped) bytes. Writes nothing when check is set."""
    before = os.path.getsize(path)
    members, dirs = read_members(path)

    # --check only sizes; it must stay quick enough to run in the test suite, so
    # it re-encodes at the cheap effort and looks only at the layers that can
    # regress. A map compacted at full effort is SMALLER than what this
    # produces, which is why the comparison below is one-sided.
    effort = EFFORT_FAST if check else EFFORT_MAX

    for name in RASTERS:
        if name not in members:
            continue
        members[name] = layer_png(Image.open(io.BytesIO(members[name])), effort)

    if not check:
        for name in sorted(members):
            if name in RASTERS or name in DERIVED or not name.endswith(".png"):
                continue
            members[name] = optimize_png(members[name], effort)

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

    write_odmap(path, members, dirs, effort)
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
        if not args.check:
            # Zopfli over an 8192x4096 layer is minutes, not seconds. Say which
            # map is being worked on rather than looking hung, then write the
            # result over the same line.
            print(f"  {os.path.basename(p):<14} working...", end="", flush=True)
        before, after, dropped = shrink(p, args.check)
        if not args.check:
            print("\r" + " " * 40 + "\r", end="")
        total_before += before
        total_after += after
        note = f", dropped {dropped / 1e6:.2f} MB of derived layers" if dropped else ""
        if args.check and after >= before:
            # The cheap encoder could not beat what is on disk, which is the
            # expected answer for a map already compacted at full effort.
            # Printing "would shrink 0.82 MB -> 0.91 MB" for that was not
            # wrong so much as backwards.
            print(f"  {os.path.basename(p):<14} {before / 1e6:6.2f} MB  compact{note}")
        else:
            print(f"  {os.path.basename(p):<14} {before / 1e6:6.2f} MB -> {after / 1e6:5.2f} MB "
                  f"({after / before:5.1%}){note}")
        # SLACK covers the noise between one deflate run and the next. A map
        # that has genuinely reverted -- a tool writing truecolour land/sea
        # again, or putting political.png back -- is several times the size,
        # nowhere near this line.
        SLACK = 0.98
        if after < before * SLACK:
            regrown.append(os.path.basename(p))

    if total_before and not args.check:
        print(f"\n  shrank {total_before / 1e6:.2f} MB -> {total_after / 1e6:.2f} MB "
              f"({total_after / total_before:.1%}, "
              f"{(total_before - total_after) / 1e6:.2f} MB saved)")
    elif total_before:
        # No total in --check: the two numbers are not comparable across maps.
        # This pass re-encodes at the CHEAP effort, so on a compacted archive it
        # is measuring a bigger encoding of the same pixels, not a saving.
        print(f"\n  {len(paths) - len(regrown)} of {len(paths)} map(s) at or below "
              f"what a cheap re-encode produces")

    if args.check and regrown:
        print(f"\n{len(regrown)} map(s) are not in their compact form: "
              f"{', '.join(regrown)}.\nRun tools/shrink_maps.py to re-encode them.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
