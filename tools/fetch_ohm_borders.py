#!/usr/bin/env python3
"""
Fetch a scenario's real national outlines from OpenHistoricalMap.

    python3 tools/fetch_ohm_borders.py 1939-09-01 "German Reich" Poland
    python3 tools/fetch_ohm_borders.py --list 1939-09-01

Writes tools/data/ohm_borders.json, which tools/carve_borders.py reads. NEEDS
NETWORK, and is meant to be run rarely and by hand -- the output is committed
so the pipeline never touches Overpass.

WHY THIS EXISTS

carve_borders.py used to hold borders traced by hand from a period atlas, and
a hand trace is a few dozen vertices. The German-Polish frontier was
seventeen. That is enough to put the border in roughly the right place and
nowhere near enough to make it look like a border: the Corridor came out as a
smooth arc, Upper Silesia as a bend, and the whole thing read as drawn rather
than surveyed, because it was.

OpenHistoricalMap has the same frontier as a relation with 28,874 vertices,
dated 1938-11-01 to 1939-10-06 -- interwar Poland exactly, ending at the
partition. Simplified to half a raster pixel it is 517 vertices, which is
small enough to commit and finer than anything this raster can draw.

WHY IT IS SAFE TO SHIP

OHM is CC0. No attribution, no share-alike, no obligation of any kind -- the
only historical boundary source with none, which is why it is here and
aourednik/historical-basemaps (GPL-3.0) is still only a report a human reads.
It is recorded in tools/provenance.json like every other input.

THE SIMPLIFICATION IS SUB-PIXEL

Douglas-Peucker at 0.02 degrees. The province raster is 8192x4096, so one
pixel is 0.044 degrees of longitude -- the tolerance is under half of that,
and no simplification survives rasterising. Raise it and the border starts
cutting corners the map can see.
"""

import argparse
import json
import math
import os
import re
import sys
import time
import urllib.parse
import urllib.request

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS_DIR)
OUT = os.path.join(TOOLS_DIR, "data", "ohm_borders.json")
OVERPASS = "https://overpass-api.openhistoricalmap.org/api/interpreter"
UA = "OpenDoctrines-borders/1.0"
TOL = 0.02


def as_date(s, dm=(1, 1)):
    if not s:
        return None
    m = re.match(r"^(-?\d{1,4})(?:-(\d{2}))?(?:-(\d{2}))?", str(s))
    if not m:
        return None
    return (int(m.group(1)),
            int(m.group(2)) if m.group(2) else dm[0],
            int(m.group(3)) if m.group(3) else dm[1])


def outline_for(date, name):
    """The committed outline for `name` valid on `date`, or None.

    The reader lives with the writer so there is one of it. carve_borders.py
    and carve_states.py both import this; a second copy would be a second
    chance for the two to disagree about what the file means.

    Falls back to an entry filed under a DIFFERENT date when that entry's own
    validity covers the one asked for. OHM dates a relation by when the border
    existed, not by when it was fetched, and several span the whole scenario
    set -- Nepal's runs from 1860 with no end, Tibet's 1912 to 1951. Without
    this, five maps wanting the same unchanged border mean five identical
    fetches and five copies in the file.
    """
    try:
        with open(OUT, encoding="utf-8") as f:
            borders = json.load(f)["borders"]
    except (OSError, KeyError, json.JSONDecodeError):
        return None
    entry = borders.get(date, {}).get(name)
    if entry is None:
        want = as_date(date)
        for slot in borders.values():
            cand = slot.get(name)
            if cand is None:
                continue
            s = as_date(cand.get("start_date"))
            e = as_date(cand.get("end_date"), (12, 31))
            if s and s <= want and (e is None or e >= want):
                entry = cand
                break
    if entry is None:
        return None
    return [[(x, y) for x, y in ring] for ring in entry["rings"]]


def overpass(query):
    req = urllib.request.Request(OVERPASS,
                                 data=urllib.parse.urlencode({"data": query}).encode(),
                                 headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=300) as r:
        return json.loads(r.read())


def live_at(date):
    want = as_date(date)
    d = overpass('[out:json][timeout:180];'
                 'relation["boundary"="administrative"]["admin_level"="2"];out ids tags;')
    out = []
    for e in d["elements"]:
        t = e.get("tags", {})
        s, en = as_date(t.get("start_date")), as_date(t.get("end_date"), (12, 31))
        if s is None or s > want or (en is not None and en < want):
            continue
        out.append((e["id"], t.get("name:en") or t.get("name") or str(e["id"])))
    return out


def rings_of(el):
    segs = [[(p["lon"], p["lat"]) for p in m["geometry"]]
            for m in el.get("members", [])
            if m.get("role") == "outer" and m.get("geometry")]
    rings, guard = [], 0
    while segs and guard < 500000:
        cur = segs.pop(0)
        changed = True
        while changed and cur[0] != cur[-1] and guard < 500000:
            guard += 1
            changed = False
            for i, s in enumerate(segs):
                if s[0] == cur[-1]:  cur += s[1:];            segs.pop(i); changed = True; break
                if s[-1] == cur[-1]: cur += s[::-1][1:];      segs.pop(i); changed = True; break
                if s[-1] == cur[0]:  cur = s[:-1] + cur;      segs.pop(i); changed = True; break
                if s[0] == cur[0]:   cur = s[::-1][:-1] + cur; segs.pop(i); changed = True; break
        if len(cur) > 3:
            rings.append(cur)
    return rings


def rdp(pts, tol):
    if len(pts) < 3:
        return pts

    def dist(p, a, b):
        (x, y), (x1, y1), (x2, y2) = p, a, b
        dx, dy = x2 - x1, y2 - y1
        if dx == 0 and dy == 0:
            return math.hypot(x - x1, y - y1)
        t = max(0.0, min(1.0, ((x - x1) * dx + (y - y1) * dy) / (dx * dx + dy * dy)))
        return math.hypot(x - (x1 + t * dx), y - (y1 + t * dy))

    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        i, j = stack.pop()
        if j <= i + 1:
            continue
        best, bi = -1.0, i
        for k in range(i + 1, j):
            d = dist(pts[k], pts[i], pts[j])
            if d > best:
                best, bi = d, k
        if best > tol:
            keep[bi] = True
            stack += [(i, bi), (bi, j)]
    return [p for p, k in zip(pts, keep) if k]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("date", help='scenario date, e.g. "1939-09-01"')
    ap.add_argument("names", nargs="*", help="OHM country names to fetch")
    ap.add_argument("--list", action="store_true", help="list what exists at that date and stop")
    ap.add_argument("--tol", type=float, default=TOL, help=f"simplify tolerance in degrees (default {TOL})")
    args = ap.parse_args()

    print(f"asking OHM what existed on {args.date} ...")
    have = live_at(args.date)
    if args.list:
        for _id, nm in sorted(have, key=lambda t: t[1]):
            print(f"   {nm}")
        return 0
    if not args.names:
        ap.error("name at least one country, or pass --list")

    by_name = {nm: i for i, nm in have}
    doc = {}
    if os.path.exists(OUT):
        doc = json.load(open(OUT, encoding="utf-8"))
    doc.setdefault("$comment",
                   "National outlines from OpenHistoricalMap (CC0), simplified to "
                   "under half a raster pixel. Generated by tools/fetch_ohm_borders.py "
                   "-- do not edit by hand. Read by tools/carve_borders.py.")
    doc.setdefault("source", "OpenHistoricalMap, https://www.openhistoricalmap.org/ (CC0)")
    doc.setdefault("borders", {})
    slot = doc["borders"].setdefault(args.date, {})

    for nm in args.names:
        if nm not in by_name:
            print(f"   {nm!r} does not exist at {args.date}; try --list", file=sys.stderr)
            continue
        el = overpass(f'[out:json][timeout:180];relation({by_name[nm]});out geom;')["elements"][0]
        rings = sorted(rings_of(el), key=len, reverse=True)
        simp = [r for r in (rdp(r, args.tol) for r in rings) if len(r) >= 4]
        slot[nm] = {
            "relation": by_name[nm],
            "start_date": el["tags"].get("start_date"),
            "end_date": el["tags"].get("end_date"),
            "rings": [[[round(x, 4), round(y, 4)] for x, y in r] for r in simp],
        }
        print(f"   {nm}: {sum(len(r) for r in rings):,} -> {sum(len(r) for r in simp):,} vertices "
              f"in {len(simp)} ring(s)  [{el['tags'].get('start_date')} .. {el['tags'].get('end_date')}]")
        time.sleep(2.0)

    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, separators=(",", ":"))
    print(f"\nwrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
