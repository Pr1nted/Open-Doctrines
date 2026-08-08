#!/usr/bin/env python3
"""
Put the naval layer of the shipped maps where the engine can use it.

    python3 tools/fix_naval_layer.py --check     # report, change nothing
    python3 tools/fix_naval_layer.py             # rewrite data/STDmaps/*.odmap
    python3 tools/fix_naval_layer.py --map 1939  # just one

This repairs maps that already exist. tools/naval_placement.py holds the rules
and tools/generate_scenario.py applies the same ones when a map is built from
scratch, so a regeneration produces what this produces rather than undoing it.

WHAT IT FIXES

  Hulls on the beach   98% of the ships in the historical maps were within four
                       pixels of land -- 167 of 170 on The Powder Keg, against
                       13% on Modern Day, which was built by a different script
                       with an open-water test. See naval_placement.py.

  Hulls nobody owns    111 battleships across the five historical maps. A
                       battleship is not buildable, is not priced by
                       Game_Economy.cpp, and until recently had no sprite and
                       dealt no damage. Sinking one removed it from the game
                       permanently with no way to replace it.

  Fleets in a queue    Ships berthed from the same port landed on top of one
                       another, because nothing kept them apart.

  Land-locked ports    Every map carried a level-3 port on the inland Belgian
                       province. That is Antwerp: a real seaport reached by a
                       river the raster does not resolve, so the port landed in
                       a province the engine calls land-locked. The player saw
                       an anchor in the middle of Belgium, could not upgrade it,
                       and no fleet could be berthed at it. overlay_real_data.py
                       let it through because its coastal test sampled every
                       eighth pixel and looked five pixels out, where
                       isProvinceCoastal walks the whole province and measures
                       the water body. A port goes to the nearest coastal
                       province of the same country instead.

WHAT IT DOES NOT CHANGE

Fleet sizes, owners and crews, and which countries have ports at all. How big a
navy a power starts with is a scenario decision that lives in
tools/data/scenarios/*.json; this decides where each hull floats, what it is
called, and which province the harbour is in.
"""

import argparse
import io
import json
import os
import shutil
import sys
import tempfile
import zipfile

import numpy as np
from PIL import Image

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS_DIR)
MAPS_DIR = os.path.join(ROOT, "data", "STDmaps")
ALL_MAPS = ["map", "1914", "1918", "1939", "1945", "1962"]

sys.path.insert(0, TOOLS_DIR)
from naval_placement import (          # noqa: E402
    HULL_CYCLE, WANT_CLEARANCE, MIN_CLEARANCE, HULL_SPACING,
    clearance_field, coastal_anchor, find_berth, hull_type, pixel_to_lonlat)
from fill_water_speckle import label_components, min_water_body  # noqa: E402

LEGACY_HULLS = {"battleship", "cruiser"}


def read_map(path):
    with zipfile.ZipFile(path) as z:
        members = {i.filename: z.read(i.filename) for i in z.infolist()
                   if not i.is_dir()}
        dirs = [i.filename for i in z.infolist() if i.is_dir()]
    return members, dirs


def write_map(path, members, dirs):
    fd, tmp = tempfile.mkstemp(suffix=".odmap", dir=os.path.dirname(path))
    os.close(fd)
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
        for d in dirs:
            z.writestr(d, b"")
        for fn, data in members.items():
            z.writestr(fn, data)
    shutil.move(tmp, path)


def process(name, check):
    path = os.path.join(MAPS_DIR, f"{name}.odmap")
    if not os.path.exists(path):
        print(f"  {name}: no such map, skipped")
        return 0

    members, dirs = read_map(path)
    ls = np.array(Image.open(io.BytesIO(members["land_sea.png"])).convert("RGBA"))
    pv = np.array(Image.open(io.BytesIO(members["provinces.png"])).convert("RGBA"))
    land = ls[:, :, 0] > 128
    h, w = land.shape
    pid = (pv[:, :, 0].astype(np.uint32) << 16 |
           pv[:, :, 1].astype(np.uint32) << 8 |
           pv[:, :, 2].astype(np.uint32))

    ships = json.loads(members["ships.json"])
    ports = json.loads(members["ports.json"])
    provinces = json.loads(members["provinces.json"])

    # ── the water everything here is judged against ──────────────
    lbl, sizes = label_components(~land)
    big_water = (lbl > 0) & (sizes[lbl] >= min_water_body())
    clear = clearance_field(land)

    # province centroids over the map's own raster
    sel = pid != 0
    ys, xs = np.nonzero(sel)
    ids = pid[sel]
    top = int(ids.max()) + 1
    cnt = np.bincount(ids, minlength=top)
    sx = np.bincount(ids, weights=xs, minlength=top)
    sy = np.bincount(ids, weights=ys, minlength=top)

    # Coastal exactly as Game::isProvinceCoastal decides it: some pixel of the
    # province is 4-adjacent to a water body of at least MIN_WATER_BODY. The
    # province pixel need not itself be land -- an island smaller than one
    # raster cell is all water and is still coastal.
    grow = np.zeros_like(big_water)
    grow[1:, :] |= big_water[:-1, :]
    grow[:-1, :] |= big_water[1:, :]
    grow[:, 1:] |= big_water[:, :-1]
    grow[:, :-1] |= big_water[:, 1:]
    coastal = set(np.unique(pid[grow & sel]).tolist()) - {0}

    # ── measure what is there now ────────────────────────────────
    legacy = sum(1 for s in ships if s["type"] in LEGACY_HULLS)
    beached = 0
    for s in ships:
        px = int((s["lon"] + 180.0) / 360.0 * w) % w
        py = min(h - 1, max(0, int((90.0 - s["lat"]) / 180.0 * h)))
        y0, y1 = max(0, py - 4), min(h, py + 5)
        x0, x1 = max(0, px - 4), min(w, px + 5)
        if land[y0:y1, x0:x1].any():
            beached += 1
    stranded_ports = [int(p) for p in ports if int(p) not in coastal]
    print(f"  {name}: {len(ships)} ships -- {legacy} unbuildable hulls, "
          f"{beached} within 4px of land; {len(ports)} ports -- "
          f"{len(stranded_ports)} land-locked")
    if check:
        return legacy + beached + len(stranded_ports)
    if not ships and not stranded_ports:
        return 0

    # ── move a land-locked port to its country's nearest coast ───
    moved = []
    for p in stranded_ports:
        cid = provinces.get(str(p), {}).get("country_id")
        if cid is None or p >= top or not cnt[p]:
            continue
        cx, cy = sx[p] / cnt[p], sy[p] / cnt[p]
        best, best_d = None, None
        for q_str, q_prov in provinces.items():
            q = int(q_str)
            if q not in coastal or q_prov.get("country_id") != cid:
                continue
            if q >= top or not cnt[q]:
                continue
            d = (sx[q] / cnt[q] - cx) ** 2 + (sy[q] / cnt[q] - cy) ** 2
            if best_d is None or d < best_d:
                best, best_d = q, d
        level = int(ports[str(p)].get("level", 1))
        del ports[str(p)]
        if best is None:
            # A country with no coastal province at all. There is no harbour to
            # move this to, and inventing one inland is how it got here.
            moved.append((p, None, level))
            continue
        keep = max(level, int(ports.get(str(best), {}).get("level", 0)))
        ports[str(best)] = {"level": keep}
        moved.append((p, best, keep))
    if moved:
        members["ports.json"] = json.dumps(ports, separators=(",", ":")).encode()

    # ── each harbour's anchor, and whether a hull fits in front of it ──
    #
    # A province can pass isProvinceCoastal and still have nowhere to put a
    # ship. MIN_WATER_BODY asks how many pixels of water there are, not what
    # shape they are, so a dammed river reservoir -- the Kuybyshev on the
    # Volga is the one that turns up here -- is a big enough body to make a
    # province five hundred kilometres inland count as coastal. It is one
    # pixel wide. Berthing a fleet there is what left Soviet destroyers
    # sitting outside Kazan.
    anchors, unusable = {}, []
    for ps in ports:
        p = int(ps)
        if p >= top or not cnt[p]:
            continue
        c = (int(sx[p] / cnt[p]), int(sy[p] / cnt[p]))
        a = coastal_anchor(pid == p, big_water, c)
        if find_berth(clear, a[0], a[1], []) is None:
            unusable.append(p)
            continue
        anchors[p] = a

    # ── each power's own harbours, best first ────────────────────
    by_country = {}
    for ps, info in ports.items():
        p = int(ps)
        if p not in anchors:
            continue
        cid = provinces.get(ps, {}).get("country_id")
        if cid is None:
            continue
        by_country.setdefault(int(cid), []).append((p, int(info.get("level", 1))))
    for v in by_country.values():
        v.sort(key=lambda t: (-t[1], t[0]))

    # ── re-berth, in a fixed order so the result is reproducible ──
    fleets = {}
    for s in ships:
        fleets.setdefault(int(s["country_id"]), []).append(s)

    taken = []
    placed, dropped, tight = 0, 0, 0
    out = []
    for cid in sorted(fleets):
        harbours = by_country.get(cid, [])
        fleet = fleets[cid]
        if not harbours:
            # A navy whose every port was lost with the ports file. Leave the
            # hulls exactly where they were rather than inventing a home for
            # them somewhere they never sailed from.
            for s in fleet:
                out.append(s)
            dropped += len(fleet)
            continue
        for i, s in enumerate(fleet):
            p = harbours[i % len(harbours)][0]
            ox, oy = anchors[p]
            berth = find_berth(clear, ox, oy, taken)
            if berth is None:
                out.append(s)
                dropped += 1
                continue
            bx, by, c = berth
            taken.append((bx, by))
            if c < WANT_CLEARANCE:
                tight += 1
            lon, lat = pixel_to_lonlat(bx, by, w, h)
            s = dict(s)
            s["type"] = hull_type(i)
            s["lon"] = round(lon, 6)
            s["lat"] = round(lat, 6)
            out.append(s)
            placed += 1

    members["ships.json"] = json.dumps(out, indent=2,
                                       separators=(",", ": ")).encode()
    write_map(path, members, dirs)

    from collections import Counter
    for p, to, lvl in moved:
        where = f"-> prov {to} (level {lvl})" if to else "dropped, no coast in that country"
        print(f"      land-locked port on prov {p} {where}")
    print(f"      re-berthed {placed}, kept {dropped} in place for want of a "
          f"port or a berth; {tight} had to settle for under {WANT_CLEARANCE}px "
          f"of clearance")
    if unusable:
        print(f"      {len(unusable)} port(s) have no water wide enough for a "
              f"hull and were skipped: {unusable[:6]}")
    print(f"      hulls now: {dict(Counter(s['type'] for s in out))}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="report and exit non-zero if any map needs work")
    ap.add_argument("--map", action="append", dest="maps",
                    help="one map id; repeatable. Default: all of them")
    args = ap.parse_args()

    print(f"want {WANT_CLEARANCE}px clearance, floor {MIN_CLEARANCE}px, "
          f"{HULL_SPACING}px between hulls, hulls from {sorted(set(HULL_CYCLE))}")
    bad = 0
    for name in args.maps or ALL_MAPS:
        bad += process(name, args.check)

    if args.check:
        if bad:
            print(f"\n{bad} defect(s) in the naval layer. "
                  f"Run without --check to repair them.")
            return 1
        print("\nEvery fleet is in open water, every hull is one the game can "
              "build, and every port is on a coast.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
