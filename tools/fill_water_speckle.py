#!/usr/bin/env python3
"""
Fill the water bodies that are too small to be sea, in every shipped map.

    python3 tools/fill_water_speckle.py --check     # report, change nothing
    python3 tools/fill_water_speckle.py             # rewrite data/STDmaps/*.odmap
    python3 tools/fill_water_speckle.py --map 1914  # just one

WHAT WAS WRONG

The coastline raster comes from Natural Earth at 8192x4096, which is about 4.9
km per pixel at the equator. At that resolution a lake of a few square hundred
kilometres survives as a handful of pixels, and land_sea.png has 7,436 water
bodies smaller than 75 pixels.

Every one of them punches a hole straight through whatever province it lands
in. The province layer paints them as sea, the country border traces around
each one, and the province selection outline traces around them again -- so a
country reads as shredded rather than as a country. On the Tibetan plateau 72
of the 74 water bodies are like this; in Fennoscandia, 801 of 815. That is what
the map actually looks like at any zoom above the whole-world view: a country
sprayed with black dots and outlined around each of them.

WHY 75 PIXELS AND NOT SOME OTHER NUMBER

Because the game has already made this exact judgement somewhere else, and it
should only make it once. isProvinceCoastal (Game_TurnLogic.cpp) walks the
water bodies a province touches and declares the province land-locked unless
one of them reaches MIN_WATER_BODY = 75 pixels. A body under that size is
already, by the engine's own definition, not sea: you cannot build a port on
it, no hull can be berthed in it, and no fleet can cross it.

So it should not punch a hole in a country either. Any threshold picked
independently here would be a second opinion about the same question, and the
two would drift apart the first time either was tuned. This reads the constant
from the engine header so they cannot.

WHAT SURVIVES

Everything that is actually a lake. The Great Lakes (7,185 / 5,281 / 1,459 /
1,123 / 315 px), the Caspian (26,619), Victoria (2,889), Balaton, Baikal and
the Rift Valley lakes are all one to three orders of magnitude above the line.
Filling takes 36,598 pixels out of 33.5 million -- 0.1% of the raster -- and
removes 7,436 holes.

WHAT THE FILLED PIXELS BECOME

Land, owned by the province around them. A pixel takes the province id of the
nearest pixel on the body's rim, so a lake sitting on a border splits between
both owners along the line the border already took, rather than jumping whole
to whichever id happened to be lowest.

Province ids are not renumbered and none are created or destroyed, so saves,
mods and every province-keyed file stay valid. population.json and
resources.json are per-province totals, not per-pixel densities, so a province
gaining a few pixels of former lakebed does not change them.
"""

import argparse
import io
import json
import os
import re
import sys
import zipfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from odmap_pack import layer_png, write_odmap   # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# THE GRADIENT FIELD IS THE GAME'S, AND THERE IS ONLY ONE OF IT.
#
# This file used to carry its own border_distance(): 4-connected, one unit per
# orthogonal step. Game_Loading.cpp::rebuildGradientField() does something else
# -- 8-connected, orthogonal steps costing 2 and diagonal 3, capped at 60, a
# chamfer 2-3 metric. So the shading written here reached its full depth at
# sixty pixels where the game reaches it at thirty, and every preview this tool
# wrote was drawn with a gradient twice as wide as the one the player sees.
#
# The copy in generate_scenario.py was the correct one and is documented
# against that C++ line by line, so it is now the only copy. A third
# implementation is how the first one went wrong unnoticed.
from generate_scenario import border_distance          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAPS_DIR = os.path.join(ROOT, "data", "STDmaps")
ALL_MAPS = ["map", "1914", "1918", "1939", "1945", "1962"]

LAND_RGBA = (255, 255, 255, 255)


def min_water_body():
    """Read MIN_WATER_BODY out of the engine rather than restating it.

    A copy of this number here that drifts from the one in Game_TurnLogic.cpp
    would put the map and the port rule back into disagreement, which is the
    whole problem this tool exists to end.
    """
    src = os.path.join(ROOT, "src", "Game_TurnLogic.cpp")
    with open(src, encoding="utf-8") as f:
        m = re.search(r"MIN_WATER_BODY\s*=\s*(\d+)", f.read())
    if not m:
        sys.exit(f"MIN_WATER_BODY not found in {src} -- has isProvinceCoastal moved?")
    return int(m.group(1))


# ── connected components ────────────────────────────────────────────
def label_components(mask, wrap_x=True):
    """4-connected labelling of a boolean raster. Returns (labels, sizes).

    Run-length union-find: each row is cut into runs of True, every run is a
    node, and a run is unioned with the runs it overlaps in the row above. Cost
    is linear in the number of runs, and a coastline has vastly fewer runs than
    pixels -- the whole 8192x4096 raster labels in well under a second, where a
    per-pixel flood fill in Python does not finish in a useful time.

    Columns wrap, because the map is a cylinder and the Pacific is one ocean.
    """
    h, w = mask.shape
    parent = [0]

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[max(ra, rb)] = min(ra, rb)

    labels = np.zeros((h, w), dtype=np.int32)
    prev = []
    for y in range(h):
        row = mask[y]
        if not row.any():
            prev = []
            continue
        d = np.diff(np.concatenate(([0], row.view(np.int8), [0])))
        runs = []
        for s, e in zip(np.flatnonzero(d == 1).tolist(),
                        np.flatnonzero(d == -1).tolist()):
            node = len(parent)
            parent.append(node)
            labels[y, s:e] = node
            runs.append((s, e, node))
        if wrap_x and len(runs) > 1 and runs[0][0] == 0 and runs[-1][1] == w:
            union(runs[0][2], runs[-1][2])
        i = j = 0
        while i < len(runs) and j < len(prev):
            s0, e0, n0 = runs[i]
            s1, e1, n1 = prev[j]
            if s0 < e1 and s1 < e0:
                union(n0, n1)
            if e0 < e1:
                i += 1
            else:
                j += 1
        prev = runs

    root = np.zeros(len(parent), dtype=np.int32)
    for i in range(1, len(parent)):
        root[i] = find(i)
    _, compact = np.unique(root[1:], return_inverse=True)
    remap = np.zeros(len(parent), dtype=np.int32)
    remap[1:] = compact.astype(np.int32) + 1
    out = remap[labels]
    return out, np.bincount(out.ravel())


# ── province assignment ─────────────────────────────────────────────
def claim_by_nearest_rim(pid, fill):
    """Give every filled pixel the province id of the nearest rim pixel.

    Multi-source BFS outward from the province pixels bordering the fill. The
    alternative -- one majority id per body -- hands a border lake wholly to
    one side and puts a visible notch in the border. This lets the border keep
    the shape it already had across the lakebed.

    Returns the number of pixels that found no owner at all, which happens only
    for a body whose entire rim is unowned water.
    """
    h, w = pid.shape
    out = pid.copy()
    todo = fill.copy()

    # Seed: filled pixels 4-adjacent to a pixel that already has an owner.
    while todo.any():
        src = np.zeros((h, w), dtype=np.uint32)
        seeded = np.zeros((h, w), dtype=bool)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            nb = np.roll(np.roll(out, dy, axis=0), dx, axis=1)
            take = todo & ~seeded & (nb != 0)
            src[take] = nb[take]
            seeded |= take
        if not seeded.any():
            break            # rim is entirely unowned; nothing more to spread
        out[seeded] = src[seeded]
        todo &= ~seeded
    return out, int(todo.sum())


# ── political.png, exactly as the game rebuilds it ──────────────────
def build_political(pid, provinces, countries):
    """Reproduce generatePoliticalTexture(): the browser preview must match
    what the game draws from the same province layer at load."""
    cid_lut = np.zeros(int(pid.max()) + 1, dtype=np.int32)
    for k, p in provinces.items():
        i = int(k)
        if i <= int(pid.max()):
            cid_lut[i] = int(p.get("country_id", 0))
    cid = cid_lut[pid]

    t = np.minimum(1.0, border_distance(cid) / 60.0)
    top = max(int(cid.max()), max(int(k) for k in countries)) + 1
    col = np.zeros((top, 3), dtype=np.float32)
    for k, v in countries.items():
        h = v["color"].lstrip("#")
        col[int(k)] = [int(h[i:i + 2], 16) for i in (0, 2, 4)]

    out = col[cid] * (1.0 - t[..., None] * 0.4) + 40.0 * t[..., None] * 0.3
    inv = 1.0 - t
    sea = np.stack([8 + (inv * 16).astype(np.uint8),
                    10 + (inv * 22).astype(np.uint8),
                    15 + (inv * 38).astype(np.uint8)], axis=-1).astype(np.float32)
    is_sea = cid <= 0
    out[is_sea] = sea[is_sea]
    out = np.clip(out, 0, 255).astype(np.uint8)

    edge = np.zeros(cid.shape, dtype=bool)
    edge[:-1, :] |= cid[:-1, :] != cid[1:, :]
    edge[1:, :] |= cid[:-1, :] != cid[1:, :]
    edge[:, :-1] |= cid[:, :-1] != cid[:, 1:]
    edge[:, 1:] |= cid[:, :-1] != cid[:, 1:]
    edge &= ~is_sea
    out[edge] = out[edge] // 3
    return Image.fromarray(out, "RGB").convert("RGBA")


def build_thumb(pid, provinces, countries, width=512):
    """The browser thumbnail, with its gradient computed at thumbnail scale.

    Shrinking the full-resolution political.png does not give this. The shading
    runs 60 map pixels inward, which at 1/16 scale is under four thumbnail
    pixels, so averaging drives the whole world to its darkest value and
    sampling picks each pixel's shading at an arbitrary depth. Either way the
    thumbnail comes out darker than the game and noisier than the map.

    So the province ids are downsampled FIRST and the distance field is run on
    the small array with the cap scaled to match -- the view the game gives at
    full zoom-out, which is what a thumbnail is for. This is the same reasoning
    generate_scenario.build_thumbnail() is built on; it lives here so the tools
    that rewrite a shipped map share one copy of it.
    """
    step = max(1, pid.shape[1] // width)
    small = pid[::step, ::step]

    cid_lut = np.zeros(int(pid.max()) + 1, dtype=np.int32)
    for k, p in provinces.items():
        i = int(k)
        if i <= int(pid.max()):
            cid_lut[i] = int(p.get("country_id", 0))
    cid = cid_lut[small]

    cap = max(6, (60 // step) | 1)
    t = np.minimum(1.0, border_distance(cid, cap=cap) / float(cap))

    top = max(int(cid.max()), max(int(k) for k in countries)) + 1
    col = np.zeros((top, 3), dtype=np.float32)
    for k, v in countries.items():
        h = v["color"].lstrip("#")
        col[int(k)] = [int(h[i:i + 2], 16) for i in (0, 2, 4)]

    out = col[cid] * (1.0 - t[..., None] * 0.4) + 40.0 * t[..., None] * 0.3
    inv = 1.0 - t
    sea = np.stack([8 + inv * 16, 10 + inv * 22, 15 + inv * 38], axis=-1)
    out[cid <= 0] = sea[cid <= 0]
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), "RGB")


# ── one map ─────────────────────────────────────────────────────────
def process(name, threshold, check):
    path = os.path.join(MAPS_DIR, f"{name}.odmap")
    if not os.path.exists(path):
        print(f"  {name}: no such map, skipped")
        return 0

    with zipfile.ZipFile(path) as z:
        members = {i.filename: z.read(i.filename) for i in z.infolist()
                   if not i.is_dir()}
        dirs = [i.filename for i in z.infolist() if i.is_dir()]

    ls = np.array(Image.open(io.BytesIO(members["land_sea.png"])).convert("RGBA"))
    pv = np.array(Image.open(io.BytesIO(members["provinces.png"])).convert("RGBA"))
    land = ls[:, :, 0] > 128
    pid = (pv[:, :, 0].astype(np.uint32) << 16 |
           pv[:, :, 1].astype(np.uint32) << 8 |
           pv[:, :, 2].astype(np.uint32))

    lbl, sizes = label_components(~land)
    small = (lbl > 0) & (sizes[lbl] < threshold)
    n_bodies = int((sizes[1:] < threshold).sum())
    n_px = int(small.sum())

    print(f"  {name}: {len(sizes) - 1:,} water bodies, "
          f"{n_bodies:,} under {threshold}px covering {n_px:,} px")
    if check or n_px == 0:
        return n_bodies

    ls[small] = LAND_RGBA
    new_pid, orphans = claim_by_nearest_rim(pid, small)
    if orphans:
        # A body whose rim never touched an owned pixel. Leave those pixels as
        # sea rather than invent an owner: better a hole we can see than a
        # province that silently annexed open water.
        keep_sea = small & (new_pid == 0)
        ls[keep_sea] = (0, 0, 0, 0)
        print(f"      {orphans:,} px had no province on any rim, left as water")

    changed = new_pid != pid
    pv[:, :, 0] = (new_pid >> 16) & 0xFF
    pv[:, :, 1] = (new_pid >> 8) & 0xFF
    pv[:, :, 2] = new_pid & 0xFF
    pv[:, :, 3] = np.where(new_pid != 0, 255, 0).astype(np.uint8)

    provinces = json.loads(members["provinces.json"])
    countries = json.loads(members["countries.json"])

    members["land_sea.png"] = layer_png(ls)
    members["provinces.png"] = layer_png(pv)
    members["thumb.png"] = layer_png(build_thumb(new_pid, provinces, countries))
    # political.png is not written into the archive -- see odmap_pack.DERIVED.
    # It is still drawn here because the side thumbnail is cut from it.
    pol = build_political(new_pid, provinces, countries)

    write_odmap(path, members, dirs)

    thumb_side = os.path.join(MAPS_DIR, f"{name}_thumb.png")
    if os.path.exists(thumb_side):
        pol.convert("RGB").resize((160, 80), Image.NEAREST).save(thumb_side)

    print(f"      filled {n_px:,} px, {int(changed.sum()):,} pixels changed owner; "
          f"rewrote land_sea, provinces and thumb")
    return n_bodies


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="report what would be filled and exit non-zero if anything would")
    ap.add_argument("--map", action="append", dest="maps",
                    help="one map id; repeatable. Default: all of them")
    args = ap.parse_args()

    threshold = min_water_body()
    maps = args.maps or ALL_MAPS
    print(f"MIN_WATER_BODY = {threshold} (read from src/Game_TurnLogic.cpp)")

    remaining = 0
    for name in maps:
        remaining += process(name, threshold, args.check)

    if args.check:
        if remaining:
            print(f"\n{remaining:,} sub-{threshold}px water bodies still in the "
                  f"shipped maps. Run without --check to fill them.")
            return 1
        print("\nNo water body below the engine's own sea threshold. Clean.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
