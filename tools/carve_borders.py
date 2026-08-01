#!/usr/bin/env python3
"""
Cut provinces along a historical border and hand the far side to its real owner.

    python3 tools/carve_borders.py --check    # report, change nothing
    python3 tools/carve_borders.py            # rewrite data/STDmaps/1939.odmap

WHAT WAS WRONG

Every scenario is drawn on one province raster, and that raster has MODERN
borders. Reassigning whole provinces fixes the cases where a modern province
happens to sit inside one historical country. It cannot fix a border that moved
THROUGH a province -- and the German-Polish border moved further than almost
any other in the twentieth century.

So The Gathering Storm shipped Poland reaching to 14.15E. That is the
Oder-Neisse line, drawn at Potsdam in 1945, six years after this scenario
opens. Silesia, Pomerania and eastern Brandenburg were German in 1939 and the
map gave them to Poland; measured against the 1937 border, province 854 is 75%
German and 856 is 69% German. Neither can be moved whole without handing Poland
or Germany a slab of the other.

WHAT THIS DOES

Rasterises the real border as a polygon and splits every province that crosses
it. The pixels inside become a NEW province owned by the country that held them
then; the rest stay where they were. Both halves keep a complete set of
province-keyed data -- population and resources split by area, minorities and
political compass inherited from the parent -- because the game reads a hole if
any of the six files is missing an id.

WHAT IT DOES NOT FIX

The cut follows the polygon, and the polygon is traced by hand from the real
border, so it is accurate to roughly the width of a raster pixel (0.044 degrees
of longitude, about 3 km at this latitude). Cities within a few km of the line
can land on the wrong side. This is a much better approximation than a border
six years in the future, not a survey.
"""

import json
import io
import os
import shutil
import sys
import tempfile
import zipfile

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAPS_DIR = os.path.join(ROOT, "data", "STDmaps")

# THE GERMAN-POLISH FRONTIER, TRACED ONCE.
#
# Germany's border and Poland's border are the same line, so tracing them
# separately guarantees they disagree -- the first attempt here overlapped by
# 9% of Poland's area along the whole frontier, which made every coverage
# measurement wrong in both directions. Both polygons are now built from these
# shared edges, so the two countries meet exactly and neither claims ground the
# other also claims.

# West of the Polish Corridor: Baltic coast down to the Czechoslovak tripoint.
GER_POL_WEST = [
    (16.70, 54.45), (16.35, 54.00), (16.20, 53.55), (15.85, 53.10),
    (15.90, 52.80), (16.15, 52.50), (16.35, 52.20), (16.60, 51.90),
    (16.90, 51.55), (17.20, 51.30), (17.65, 51.05), (18.10, 50.75),
    (18.45, 50.58), (18.80, 50.42), (19.00, 50.18), (18.92, 49.95),
    (18.85, 49.52),
]

# East Prussia's southern border with Poland, west to east.
EP_POL_SOUTH = [
    (19.30, 54.35), (19.60, 53.60), (20.30, 53.15), (21.50, 53.20),
    (22.10, 53.50), (22.80, 54.40),
]

# Germany's own western, southern and Baltic edges, plus Austria and Bohemia.
_GER_WEST_SOUTH = [
    (5.87, 51.05), (6.02, 50.18), (6.37, 49.47), (8.23, 48.97), (7.58, 47.59),
    (9.60, 47.53), (10.45, 46.87), (12.15, 46.93), (13.70, 46.50), (14.60, 46.43),
    (16.00, 46.68), (16.50, 47.00), (17.16, 47.87), (17.00, 48.35),
]
_GER_BALTIC = [
    (14.20, 54.10), (12.00, 54.45), (10.00, 54.90), (8.60, 55.06), (8.30, 54.90),
    (9.00, 53.90), (7.00, 53.70), (6.75, 53.55), (5.95, 51.80),
]

# The Reich on 1 September 1939: Germany in its 1937 borders plus Austria and
# the Protectorate. The eastern edge is the shared frontier above, so Posen and
# eastern Upper Silesia fall outside it and stay Polish.
GERMANY_1937 = _GER_WEST_SOUTH + GER_POL_WEST[::-1] + _GER_BALTIC

# East Prussia in its 1939 extent, Memelland included -- Germany annexed Memel
# in March 1939, six months before this scenario opens. The modern raster cuts
# this territory in three: Kaliningrad is its own province, Warmia-Masuria is
# inside a Polish one and Memelland inside a Lithuanian one.
EAST_PRUSSIA_1939 = EP_POL_SOUTH + [
    (22.90, 55.05), (22.20, 55.35), (21.20, 55.85), (20.90, 55.30),
    (19.90, 54.95), (19.50, 54.60),
]

# The Second Polish Republic on 1 September 1939, sharing both edges above.
POLAND_1939 = (
    GER_POL_WEST
    + [(19.80, 49.20), (21.00, 49.30), (22.90, 49.10),        # Slovakia
       (24.50, 49.10), (25.70, 49.60), (24.10, 50.40),        # Romania, USSR
       (23.60, 51.00), (23.70, 51.60), (25.20, 51.60),
       (25.50, 52.00), (26.00, 52.50), (26.50, 53.50),
       (26.20, 54.50), (26.80, 55.30),                        # Latvia
       (25.60, 54.30), (24.30, 54.30)]                        # Lithuania
    + EP_POL_SOUTH[::-1]
    + [(19.30, 54.45), (16.90, 54.60)]                        # the Corridor
)

# Austrian Galicia and Bukovina, as held in October 1918. Austria-Hungary did
# not dissolve until November, so eastern Galicia was still Austro-Hungarian
# when this scenario opens -- the West Ukrainian People's Republic was declared
# on 1 November. The modern raster puts Galicia and Russian Volhynia in one
# province, 55% the former and 43% the latter, so neither owner is right whole.
GALICIA_1918 = [
    (19.00, 50.40), (20.50, 50.55), (22.00, 50.60), (23.50, 50.45),
    (25.00, 50.20), (26.20, 49.85), (26.30, 48.30), (25.60, 47.75),
    (24.50, 47.95), (23.00, 48.40), (22.10, 48.95), (20.80, 49.30),
    (19.50, 49.50), (18.90, 49.90),
]

# map -> [(polygon, iso that held the inside, provinces it may cut, why)]
PLAN = {
    "1918.odmap": [
        (GALICIA_1918, "AUH", ["UKR"],
         "Galicia and Bukovina, Austro-Hungarian until November 1918."),
    ],
    "1939.odmap": [
        (GERMANY_1937, "GER", ["POL"],
         "Silesia, Pomerania and eastern Brandenburg, German until 1945."),
        (EAST_PRUSSIA_1939, "GER", ["POL", "LTU"],
         "East Prussia and Memelland. Germany held 36% of it after "
         "reassignment; the rest was inside Polish and Lithuanian provinces."),
        (POLAND_1939, "POL", ["SOV", "LTU"],
         "The Kresy. Poland's eastern half was Polish until the Soviet "
         "invasion of 17 September; the modern raster draws that border where "
         "Belarus and Ukraine meet Poland today, four hundred kilometres west. "
         "Includes the Vilnius region, Polish from 1920 until 1939 however "
         "loudly Lithuania disputed it."),
    ],
}

# Below this share of a province, cutting is not worth a new province id.
MIN_SHARE = 0.06
# ...and below this many raster pixels it is not worth one either, whatever the
# share. A hand-traced border is only good to a pixel or two, so re-running
# with a slightly different trace would otherwise spawn slivers along the whole
# frontier -- provinces too small to click, carrying a few hundred people.
MIN_PIXELS = 150


def inside(poly, lon, lat):
    """Ray casting, vectorised over a pixel array."""
    px = np.array([v[0] for v in poly])
    py = np.array([v[1] for v in poly])
    res = np.zeros(lon.shape, dtype=bool)
    n = len(poly)
    for i in range(n):
        j = (i - 1) % n
        cond = (py[i] > lat) != (py[j] > lat)
        with np.errstate(divide="ignore", invalid="ignore"):
            xint = (px[j] - px[i]) * (lat - py[i]) / (py[j] - py[i]) + px[i]
        res ^= cond & (lon < xint)
    return res


def carve(name, jobs, check):
    path = os.path.join(MAPS_DIR, name)
    work = tempfile.mkdtemp(prefix="odcarveb_")
    try:
        with zipfile.ZipFile(path) as z:
            entries = z.namelist()
            z.extractall(work)

        def rd(fn):
            p = os.path.join(work, fn)
            return json.load(open(p)) if os.path.exists(p) else None

        countries = rd("countries.json")
        provinces = rd("provinces.json")
        pop = rd("population.json") or {}
        res = rd("resources.json") or {}
        mino = rd("minorities.json") or {}
        comp = rd("political_compass.json") or {}
        ports = rd("ports.json") or {}

        img = Image.open(os.path.join(work, "provinces.png")).convert("RGB")
        a = np.array(img)
        H, W = a.shape[:2]
        pid = ((a[:, :, 0].astype(np.int64) << 16)
               | (a[:, :, 1].astype(np.int64) << 8)
               | a[:, :, 2].astype(np.int64))

        lon = (np.arange(W) / W) * 360.0 - 180.0
        lat = 90.0 - (np.arange(H) / H) * 180.0
        LON, LAT = np.meshgrid(lon, lat)

        by_iso = {v["iso_a3"]: int(k) for k, v in countries.items()}
        next_pid = max(int(k) for k in provinces) + 1
        print(f"\n=== {name} ===")
        total_moved = 0

        for poly, iso, from_isos, why in jobs:
            if iso not in by_iso:
                print(f"   WARN  {iso} not on this map", file=sys.stderr)
                continue
            dst = by_iso[iso]
            mask_in = inside(poly, LON, LAT)
            victims = [int(k) for k, v in provinces.items()
                       if v["iso_a3"] in from_isos]
            print(f"   {iso} <- {'/'.join(from_isos)}: {why}")
            for p in sorted(victims):
                sel = pid == p
                tot = int(sel.sum())
                if tot == 0:
                    continue
                cut = sel & mask_in
                ncut = int(cut.sum())
                share = ncut / tot
                if share < MIN_SHARE or ncut < MIN_PIXELS:
                    continue
                if share > 1.0 - MIN_SHARE:
                    # Wholly inside: no need for a new id, just change hands.
                    provinces[str(p)]["country_id"] = dst
                    provinces[str(p)]["iso_a3"] = iso
                    print(f"      prov {p:5d} {share*100:5.1f}% inside -> whole province to {iso}")
                    total_moved += tot
                    continue

                new = next_pid
                next_pid += 1
                if not check:
                    a[cut] = [(new >> 16) & 255, (new >> 8) & 255, new & 255]
                    provinces[str(new)] = {
                        "id": new, "name": "", "country_id": dst, "iso_a3": iso,
                        "color": "#%06x" % new,
                    }
                    # Population and resources scale with the land taken; the
                    # parent must lose exactly what the child gains or the
                    # scenario's totals drift.
                    ppop = int(pop.get(str(p), 0))
                    take = int(round(ppop * share))
                    pop[str(new)] = take
                    pop[str(p)] = max(0, ppop - take)
                    pres = res.get(str(p))
                    if isinstance(pres, dict):
                        res[str(new)] = {k: round(v * share, 4) if isinstance(v, (int, float)) else v
                                         for k, v in pres.items()}
                        res[str(p)] = {k: round(v * (1 - share), 4) if isinstance(v, (int, float)) else v
                                       for k, v in pres.items()}
                    elif pres is not None:
                        res[str(new)] = pres
                    # These describe people, not area, so the child inherits.
                    if str(p) in mino:
                        mino[str(new)] = mino[str(p)]
                    if str(p) in comp:
                        comp[str(new)] = comp[str(p)]
                    if str(p) in ports:
                        ports[str(new)] = ports[str(p)]
                print(f"      prov {p:5d} {share*100:5.1f}% inside -> new prov {new} "
                      f"({ncut:,} px, {int(round(pop.get(str(p),0)+0)):,} left behind)")
                total_moved += ncut

        print(f"   {total_moved:,} raster pixels changed hands")
        if check:
            return 0

        Image.fromarray(a).save(os.path.join(work, "provinces.png"))
        for fn, obj in (("provinces.json", provinces), ("population.json", pop),
                        ("resources.json", res), ("minorities.json", mino),
                        ("political_compass.json", comp), ("ports.json", ports)):
            if obj is None:
                continue
            with open(os.path.join(work, fn), "w", encoding="utf-8") as f:
                json.dump(obj, f, separators=(",", ":"))

        tmp = path + ".tmp"
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
            for e in entries:
                if e.endswith("/"):
                    z.writestr(e, b"")
                else:
                    z.write(os.path.join(work, e), e)
        os.replace(tmp, path)
        print(f"   wrote {name}")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    check = "--check" in sys.argv
    for name, jobs in PLAN.items():
        carve(name, jobs, check)
    if check:
        print("\n--check: nothing written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
