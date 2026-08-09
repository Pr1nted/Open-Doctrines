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

# THE EASTERN HALF OF THE SECOND POLISH REPUBLIC.
#
# The first trace of this was drawn to the wrong line. Its eastern edge never
# went past 26.8E and dipped to 23.6E across Volhynia, which is roughly where
# Poland's border is TODAY -- so the carve it drove recovered the Kresy in name
# and left most of it Soviet. Measured against the real 1938 border, Poland
# came out at 73% of its own area, with Rowne, Pinsk, Stanislawow and Wilno all
# on the wrong side.
#
# The line below is the one the Treaty of Riga drew in 1921 and the one that
# held until 17 September 1939. It runs a good 200-400 km east of the modern
# border for most of its length: east of Wilno, east of Baranowicze, within
# 30 km of Minsk, east of Rowne, and down the Zbrucz to the Dniester.
#
# Traced in segments because each is a different country's border, and the
# reason each is where it is differs. Checkpoints, all verified before this was
# committed: Wilno, Grodno, Nowogrodek, Baranowicze, Pinsk, Kowel, Luck, Rowne,
# Tarnopol, Lwow and Stanislawow inside; Kaunas, Utena, Zarasai, Minsk,
# Polock, Mozyrz, Zytomierz, Kijow, Kamieniec Podolski and Czerniowce outside.

# Slovakia and the Carpathian ridge, west to east. Hungary annexed
# Carpatho-Ukraine in March 1939, so the southeastern stretch is Hungary's.
POL_SOUTH = [
    (19.40, 49.50), (19.80, 49.20), (20.40, 49.32), (21.00, 49.35),
    (21.80, 49.35), (22.55, 49.08), (22.90, 49.02),
    (23.70, 48.55), (24.50, 47.95),
]
# Romania: the Carpathian crest to the Dniester, along the Czeremosz.
POL_ROMANIA = [(24.90, 47.92), (25.30, 48.10), (25.75, 48.35), (26.25, 48.55)]
# The Riga line, south to north: up the Zbrucz, across Volhynia and Polesie,
# past Minsk and up to the Latvian tripoint on the Dzwina.
POL_SOVIET = [
    (26.32, 48.85), (26.38, 49.25), (26.45, 49.65), (26.65, 50.05),
    (26.85, 50.35), (27.00, 50.70), (26.90, 51.05), (27.05, 51.35),
    (27.50, 51.55), (27.60, 51.85), (27.20, 52.15), (26.85, 52.55),
    (27.00, 53.05), (27.20, 53.55), (27.15, 53.95), (27.00, 54.25),
    (27.10, 54.65), (27.25, 55.05), (27.50, 55.40), (27.55, 55.67),
]
POL_LATVIA = [(27.00, 55.62), (26.55, 55.55)]
# Lithuania, northeast to southwest. This is the administration line of 1923,
# not a border either side recognised: Poland took Wilno in 1920 and Lithuania
# claimed it until 1939 without ever holding it.
POL_LITHUANIA = [
    (26.10, 55.35), (25.60, 55.28), (25.20, 55.18), (24.85, 54.90),
    (24.55, 54.55), (24.30, 54.30), (23.90, 54.32), (23.40, 54.35),
    (22.90, 54.40),
]
# The Corridor's Baltic coast. Follows the shore rather than cutting the bay,
# because a straight line from the Vistula to the German border passes SOUTH of
# Gdynia -- the one port the Second Republic built for itself.
POL_COAST = [
    (19.30, 54.45), (18.95, 54.70), (18.55, 54.86), (18.00, 54.82),
    (17.30, 54.80), (16.90, 54.65),
]

# The Second Polish Republic on 1 September 1939, sharing both German edges.
#
# The Free City of Danzig falls inside this outline and should not: it was
# neither Polish nor German but a League mandate with its own government. There
# is no country for it on this map and it is about seven pixels across at this
# raster, so Poland -- which held the harbour rights, the post office and the
# Westerplatte garrison -- is the closest available answer.
POLAND_1939 = (
    GER_POL_WEST
    + POL_SOUTH + POL_ROMANIA + POL_SOVIET + POL_LATVIA + POL_LITHUANIA
    + EP_POL_SOUTH[::-1]
    + POL_COAST
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
# SURVEYED OUTLINES, WHERE THERE ARE ANY.
#
# tools/data/ohm_borders.json holds national outlines pulled from
# OpenHistoricalMap by tools/fetch_ohm_borders.py and simplified to under half
# a raster pixel. Where a country is in there, it is used in preference to the
# hand trace below, because a hand trace is a few dozen vertices and this is a
# few hundred taken off a surveyed relation dated to the day.
#
# The hand traces stay as the fallback. They are what the map looked like
# before, they still work, and a country OHM has not mapped at a given date
# still needs a border from somewhere.
def _as_date(s, dm=(1, 1)):
    import re as _re
    if not s:
        return None
    m = _re.match(r"^(-?\d{1,4})(?:-(\d{2}))?(?:-(\d{2}))?", str(s))
    if not m:
        return None
    return (int(m.group(1)),
            int(m.group(2)) if m.group(2) else dm[0],
            int(m.group(3)) if m.group(3) else dm[1])


def _ohm(date, name):
    """The outline for `name` valid on `date`.

    Falls back to an entry filed under a DIFFERENT date when that entry's own
    validity covers the one asked for. OHM relations are dated by when the
    border existed, not by when we happened to fetch them, and many of them
    span the whole scenario set -- Nepal's runs from 1860 with no end, Bhutan's
    to 1949, Paraguay's to 1938. Without this, five maps that want the same
    unchanged border mean five identical fetches and five copies in the file.
    """
    path = os.path.join(ROOT, "tools", "data", "ohm_borders.json")
    try:
        with open(path, encoding="utf-8") as f:
            borders = json.load(f)["borders"]
    except (OSError, KeyError, json.JSONDecodeError):
        return None
    entry = borders.get(date, {}).get(name)
    if entry is None:
        want = _as_date(date)
        for other, slot in borders.items():
            cand = slot.get(name)
            if cand is None:
                continue
            s = _as_date(cand.get("start_date"))
            e = _as_date(cand.get("end_date"), (12, 31))
            if s and s <= want and (e is None or e >= want):
                entry = cand
                break
    if entry is None:
        return None
    return [[(x, y) for x, y in ring] for ring in entry["rings"]]


GERMANY_1939_OHM = _ohm("1939-09-01", "German Reich")
POLAND_1939_OHM = _ohm("1939-09-01", "Poland")
AUH_1914_OHM = _ohm("1914-07-01", "Austria-Hungary")
AUH_1918_OHM = _ohm("1918-10-01", "Austria-Hungary")
FINLAND_1939_OHM = _ohm("1939-09-01", "Finland")
TURKEY_1939_OHM = _ohm("1939-09-01", "Turkey")
HUNGARY_1939_OHM = _ohm("1939-09-01", "Kingdom of Hungary")

# Asia and South America, from the same ranking. Nepal, Bhutan and Paraguay
# each have ONE relation spanning the whole scenario set -- Nepal's runs from
# 1860 with no end -- so they are looked up per date and resolve to the same
# outline; see _ohm.
def _asia_south_america(date):
    """The carve jobs both continents want, for whichever map is being built."""
    jobs = []
    ecu = _ohm(date, "Ecuador")
    if ecu:
        jobs.append((ecu, "ECU", ["PER", "COL"],
                     "The Ecuadorian Amazon. Ecuador reached the Maranon and "
                     "the Napo until the Rio Protocol of 1942 took roughly half "
                     "its territory; the modern raster draws the border where "
                     "Peru and Colombia meet Ecuador today, so the map was "
                     "running the 1942 settlement decades early and gave "
                     "Ecuador 43% of itself in 1914."))
    pry = _ohm(date, "Paraguay")
    if pry:
        jobs.append((pry, "PRY", ["BOL", "ARG"],
                     "The Chaco Boreal. Bolivia and Paraguay both claimed it "
                     "and neither effectively held it before the Chaco War of "
                     "1932-35; the map resolved it to Bolivia, OHM's dated "
                     "relation to Paraguay, and Paraguay is who ended up with "
                     "it. A genuine dispute, decided rather than discovered."))
    npl = _ohm(date, "Nepal")
    if npl:
        jobs.append((npl, "NPL", ["GBR", "IND"],
                     "Nepal, which was never colonised and is drawn here as "
                     "two whole modern provinces -- 38% of the real country "
                     "was left inside British India, because a province that "
                     "straddles the border can only go one way."))
    btn = _ohm(date, "Bhutan")
    if btn:
        jobs.append((btn, "BTN", ["GBR", "IND", "TIB", "CHN"],
                     "Bhutan, cut by carve_states.py from a polygon traced by "
                     "hand at fourteen vertices. This is the same border "
                     "surveyed, and picks up the fifth of it that trace missed."))
    tib = _ohm(date, "Tibet")
    if tib:
        jobs.append((tib, "TIB", ["CHN"],
                     "Tibet at the extent Lhasa claimed and OHM draws, which "
                     "includes Amdo and eastern Kham. The map previously gave "
                     "Tibet U-Tsang and western Kham only, on the argument that "
                     "Qinghai and Amdo were run by the Ma clique answering to "
                     "China rather than governed from Lhasa. Both readings are "
                     "defensible and this is a decision, not a correction: the "
                     "claimed extent is what most historical atlases draw and "
                     "what the scenario now uses."))
    return jobs


PLAN = {
    "1914.odmap": [
        (AUH_1914_OHM, "AUH", ["RUS", "ROU", "SRB", "ITA"],
         "Austria-Hungary in its 1908 borders, which it kept until it "
         "dissolved. The modern raster cuts the empire into the seven "
         "countries that came out of it, so the scenario could only hand it "
         "whole provinces and lost the edges: Galicia to Russia, Transylvania "
         "and the Banat to Romania, Bosnia to Serbia, Trentino and Trieste to "
         "Italy. Measured against the real border the map gave Austria-Hungary "
         "74% of itself."),
    ],
    "1918.odmap": [
        (AUH_1918_OHM or GALICIA_1918, "AUH", ["UKR", "ROU", "SRB", "ITA", "RUS"],
         "Austria-Hungary, still undissolved on 1 October 1918 -- the state "
         "survived until 31 October and this scenario opens four weeks before "
         "that. Replaces a hand-traced Galicia polygon that fixed one edge of "
         "the same problem: the map gave the empire 75% of itself, with "
         "Transylvania Romanian two years before Trianon."),
    ],
    "1939.odmap": [
        (GERMANY_1939_OHM or GERMANY_1937, "GER", ["POL"],
         "Silesia, Pomerania and eastern Brandenburg, German until 1945. The "
         "OHM outline is the Reich as it stood between Memel in March 1939 and "
         "the reorganisation after Poland, so it already includes Austria, the "
         "Protectorate and Memelland -- but the carve only takes ground the "
         "map currently gives POLAND, so none of that moves."),
        (EAST_PRUSSIA_1939, "GER", ["POL", "LTU"],
         "East Prussia and Memelland. Germany held 36% of it after "
         "reassignment; the rest was inside Polish and Lithuanian provinces. "
         "Redundant once the OHM Reich outline is in use, since that contains "
         "East Prussia; kept because it is what runs if the OHM file is absent."),
        (POLAND_1939_OHM or POLAND_1939, "POL", ["SOV", "LTU"],
         "The Kresy. Poland's eastern half was Polish until the Soviet "
         "invasion of 17 September; the modern raster draws that border where "
         "Belarus and Ukraine meet Poland today, four hundred kilometres west. "
         "Includes the Vilnius region, Polish from 1920 until 1939 however "
         "loudly Lithuania disputed it."),
        (FINLAND_1939_OHM, "FIN", ["SOV"],
         "Finland before the Winter War. The OHM relation ends at the Moscow "
         "Peace Treaty of 12 March 1940, so it is exactly the Finland this "
         "scenario opens with -- Karelia, Viipuri and the Rybachy peninsula "
         "still Finnish. The modern raster draws that border where Russia "
         "meets Finland today, which is the 1940 line."),
        (HUNGARY_1939_OHM, "HUN", ["SOV", "ROU"],
         "Carpatho-Ukraine, annexed by Hungary in March 1939, six months "
         "before this scenario opens. The relation runs from 4 April 1939 to "
         "the Second Vienna Award, so it is this scenario's Hungary and not "
         "the larger one that followed."),
        (TURKEY_1939_OHM, "TUR", ["FRA", "SOV"],
         "Hatay, which voted to join Turkey and was annexed on 29 June 1939 "
         "-- the OHM relation starts on that date -- and the Kars and Ardahan "
         "districts, Turkish since 1921 and drawn here as Soviet."),
    ],
}
# Asia and South America apply to every scenario, not just the ones that
# already had a European carve, so 1945 and 1962 get a list of their own here.
for _map, _date in (("1914.odmap", "1914-07-01"), ("1918.odmap", "1918-10-01"),
                    ("1939.odmap", "1939-09-01"), ("1945.odmap", "1945-09-01"),
                    ("1962.odmap", "1962-10-01")):
    PLAN.setdefault(_map, []).extend(_asia_south_america(_date))

# A carve whose outline could not be fetched is dropped rather than run against
# None, so a missing entry in ohm_borders.json costs that one correction and
# nothing else.
for _m in PLAN:
    PLAN[_m] = [j for j in PLAN[_m] if j[0]]

# Below this share of a province, cutting is not worth a new province id.
MIN_SHARE = 0.06
# ...and below this many raster pixels it is not worth one either, whatever the
# share. A hand-traced border is only good to a pixel or two, so re-running
# with a slightly different trace would otherwise spawn slivers along the whole
# frontier -- provinces too small to click, carrying a few hundred people.
MIN_PIXELS = 150


def inside(poly, lon, lat):
    """Ray casting, vectorised over a pixel array.

    Takes either one ring or a list of rings. Rings are XORed together, so a
    country made of several pieces -- the Reich in 1939 is four, once East
    Prussia and the offshore bits are counted -- comes out as one mask, and an
    inner ring would punch a hole rather than paint one.
    """
    rings = poly if (poly and isinstance(poly[0][0], (list, tuple))) else [poly]
    res = np.zeros(lon.shape, dtype=bool)
    for ring in rings:
        px = np.array([v[0] for v in ring])
        py = np.array([v[1] for v in ring])
        n = len(ring)
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

        def mask_for(poly):
            """inside(), but only over the polygon's own bounding box.

            The full raster is 33.5 million pixels and inside() touches all of
            them once per vertex. That was fine when a polygon was seventeen
            vertices traced by hand; the surveyed OHM outlines are three to
            eight hundred, and Tibet alone took longer than the rest of the
            pipeline put together. Every one of these borders covers a small
            part of the world, so clip to its box first -- Tibet goes from 33.5
            million pixel-tests per vertex to about two hundred thousand.
            """
            rings = poly if (poly and isinstance(poly[0][0], (list, tuple))) else [poly]
            xs = [v[0] for r in rings for v in r]
            ys = [v[1] for r in rings for v in r]
            x0 = max(0, int((min(xs) + 180.0) / 360.0 * W) - 2)
            x1 = min(W, int((max(xs) + 180.0) / 360.0 * W) + 3)
            y0 = max(0, int((90.0 - max(ys)) / 180.0 * H) - 2)
            y1 = min(H, int((90.0 - min(ys)) / 180.0 * H) + 3)
            out = np.zeros((H, W), dtype=bool)
            if x1 <= x0 or y1 <= y0:
                return out
            out[y0:y1, x0:x1] = inside(poly, LON[y0:y1, x0:x1], LAT[y0:y1, x0:x1])
            return out

        for poly, iso, from_isos, why in jobs:
            if iso not in by_iso:
                print(f"   WARN  {iso} not on this map", file=sys.stderr)
                continue
            dst = by_iso[iso]
            mask_in = mask_for(poly)
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
