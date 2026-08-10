#!/usr/bin/env python3
"""
Redraw the map browser's preview after something moved a border.

    python3 tools/rebuild_map_preview.py --check   # report drift only
    python3 tools/rebuild_map_preview.py           # redraw it

WHY

thumb.png is not the layer the game plays on. Game_Loading.cpp builds that one
at load, straight from provinces.png and countries.json, so the board is always
right no matter what this file says. thumb.png is what the map browser shows
you BEFORE you load -- see generate_scenario.py, which says the same thing
where it writes it.

That split is why this drifts silently. generate_scenario.py draws the preview
in step 18b, and then four separate steps move borders after it:

    18c  carve_states     invents provinces for states too small to have one
    18d  fix_1939_history restores the states 1939 was missing
    18e  carve_borders    splits provinces along a border that ran through them
    18f  fix_map_history  restores each scenario's own states
    18g  carve_borders    again

Every one of those changes who owns a pixel, and none of them redraws the
preview. So the browser can show you Tibet inside China, or Austria-Hungary
already partitioned, on a map that plays correctly once loaded. The preview is
the only picture of a scenario you get while choosing one, which makes it the
worst place to be out of date.

This used to guard a full-resolution political.png as well. That layer is no
longer stored -- nothing read it (Game_Loading's needed[] list never named it,
and the editor recomputes it with MapEditor::computePoliticalGradient), and at
4.7 MB of a 6 MB archive it was almost the entire size of a map. So the one
preview that can go stale is the thumbnail, and it is what this checks.

It is drawn at thumbnail scale rather than shrunk from a full raster, via
fill_water_speckle.build_thumb, so every tool that rewrites a shipped map draws
the preview the one way. It runs last, after every step that can move a border.

--check reports how many pixels drifted and writes nothing, which is what
tests/run_all.sh calls so a stale preview fails the build instead of shipping.
"""

import argparse
import io
import json
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fill_water_speckle import build_thumb                       # noqa: E402
from odmap_pack import layer_png, province_ids, read_members, write_odmap  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAPS_DIR = os.path.join(ROOT, "data", "STDmaps")


def drift_px(stored, fresh):
    """Pixels where the stored image was drawn for a different owner.

    A tolerance of 2 per channel, because both sides go through PNG and the
    blend is float: anything larger than a rounding step means the colour came
    from somewhere else, not from re-encoding.
    """
    if stored is None:
        return fresh.size // 3
    old = np.array(Image.open(io.BytesIO(stored)).convert("RGB"), dtype=np.int16)
    if old.shape != fresh.shape:
        return fresh.size // 3
    return int((np.abs(old - fresh).max(axis=2) > 2).sum())


def process(name, check):
    path = os.path.join(MAPS_DIR, name)
    members, dirs = read_members(path)

    pid = province_ids(members)
    thumb = build_thumb(pid, json.loads(members["provinces.json"]),
                        json.loads(members["countries.json"]))
    fresh = np.array(thumb, dtype=np.int16)
    drift = drift_px(members.get("thumb.png"), fresh)
    total = fresh.shape[0] * fresh.shape[1]

    print(f"  {name:14} {drift:>7,} / {total:,} px stale ({drift / total:.2%})")
    if check or drift == 0:
        return drift

    members["thumb.png"] = layer_png(thumb)
    write_odmap(path, members, dirs)
    print("       redrew thumb.png")
    return drift


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="report drift and exit non-zero, writing nothing")
    ap.add_argument("--map", action="append", dest="maps",
                    help="limit to one map (repeatable)")
    args = ap.parse_args()

    maps = [m if m.endswith(".odmap") else m + ".odmap"
            for m in (args.maps or [])] or sorted(
        f for f in os.listdir(MAPS_DIR) if f.endswith(".odmap"))

    print("Browser preview vs province ownership:")
    total = sum(process(n, args.check) for n in maps)
    if args.check and total:
        print(f"\n{total:,} px of preview disagree with the map they preview.\n"
              f"Run tools/rebuild_map_preview.py to redraw them.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
