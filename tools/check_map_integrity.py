#!/usr/bin/env python3
"""
Every id in a shipped map points at something that exists.

    python3 tools/check_map_integrity.py           # report
    python3 tools/check_map_integrity.py --strict  # and fail the build on it

WHY

A map is a dozen files that agree with each other by convention and nothing
else. The province id is a pixel colour in provinces.png; six more files are
keyed by that integer, and three others (armies, ports, claims) hold it as a
foreign key. Nothing enforces any of it. Miss an entry and the game reads a
hole -- carve_states.py says so in its own header, because that is how it was
found the first time.

Every tool in this directory adds, splits, merges or deletes provinces:
carve_borders cuts new ones out of old, carve_states invents them,
absorb_rinds deletes them, fix_map_history moves owners, generate_scenario
renumbers the lot. Any of them can leave a key behind pointing at an id that no
longer exists, or a province with no population line. The failure is silent at
build time and looks like a rendering bug much later.

So this asks the questions the format cannot ask itself:

  raster vs table   every id painted in provinces.png has a row in
                    provinces.json, and every row is painted somewhere
  keyed files       every province has population, resources, minorities and a
                    political compass
  foreign keys      armies, ports and claims name provinces that exist; armies
                    and ships name countries that exist
  countries         every province's owner is a real country, and every country
                    holds at least one province
  relations         both sides of every diplomatic relation are on the map
  values            population is not negative, compass is in range

It reports rather than repairs. Which fix is right depends on which tool left
the mess, and a checker that guesses would hide the bug it found.
"""

import argparse
import io
import json
import os
import sys
import zipfile

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAPS_DIR = os.path.join(ROOT, "data", "STDmaps")

# KNOWN, DELIBERATE, AND STILL OPEN.
#
# A gate that fails on something nobody intends to fix gets switched off, and
# then it stops catching the things it was built for. So the one finding that
# is understood and unresolved is listed here with its reason, reported apart
# from the rest, and does not fail --strict. Anything NOT on this list does.
#
# Keep this list short. An entry is a decision someone has to come back to, not
# a way to silence a checker.
ACCEPTED = {
    ("map.odmap", "landless", "COM"):
        "Comoros has no land anywhere in this raster -- checked 41-47E, "
        "14-10S -- because the base map's rasteriser gave those pixels to "
        "Madagascar's province 2405, which reaches 400 km out to the "
        "archipelago. The three islands are about 81 pixels at 1:8192, so they "
        "would fit; they were simply never painted. Cape Verde is in the same "
        "position and does have a province (1947, 170 px, none of it land), "
        "which is the shape a fix would take. It is not applied because there "
        "is no source for the geometry: OHM's Comoros relation is a 12-vertex "
        "hull that rasterises to 1,090 px against 81 px of real island, and "
        "tracing one by hand is what the OHM pass was done to stop. Until the "
        "base raster is regenerated, the honest options are to give Comoros a "
        "water province the way Cape Verde has one, or to drop it from "
        "countries.json -- a content call, not a bug fix.",
}
# Game.h: the sentinel owners. Not real countries, never hold provinces.
SENTINELS = {65534, 65535, 65533}
KEYED = ["population.json", "resources.json", "minorities.json",
         "political_compass.json"]


def check(name):
    path = os.path.join(MAPS_DIR, name)
    problems = []

    def bad(kind, msg):
        problems.append((kind, msg))

    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())

        def rd(fn, default=None):
            if fn not in names:
                return default
            try:
                return json.loads(z.read(fn))
            except json.JSONDecodeError as e:
                bad("unreadable", f"{fn}: {e}")
                return default

        provinces = rd("provinces.json", {})
        countries = rd("countries.json", {})
        armies = rd("armies.json", {})
        ports = rd("ports.json", {})
        claims = rd("claims.json", {})
        ships = rd("ships.json", [])
        relations = rd("relations.json", {})
        keyed = {fn: rd(fn, {}) for fn in KEYED}

        pv = np.array(Image.open(io.BytesIO(z.read("provinces.png"))).convert("RGB"),
                      dtype=np.uint32)
        pid = pv[:, :, 0] << 16 | pv[:, :, 1] << 8 | pv[:, :, 2]

    painted = set(int(v) for v in np.unique(pid)) - {0}
    tabled = set(int(k) for k in provinces)

    # ── raster against table ─────────────────────────────────────
    for p in sorted(painted - tabled):
        n = int((pid == p).sum())
        bad("orphan-pixels", f"province {p} is painted ({n} px) with no row in "
                             f"provinces.json -- the game reads it as a hole")
    for p in sorted(tabled - painted):
        bad("ghost-province", f"province {p} has a row but is painted nowhere")

    # ── the files keyed by province id ───────────────────────────
    for fn, obj in keyed.items():
        if obj is None:
            continue
        have = set(int(k) for k in obj)
        for p in sorted(tabled - have)[:6]:
            bad("missing-data", f"province {p} has no entry in {fn}")
        extra = sorted(have - tabled)
        for p in extra[:6]:
            bad("stale-data", f"{fn} has an entry for province {p}, which does "
                              f"not exist")
        if len(extra) > 6:
            bad("stale-data", f"{fn}: ...and {len(extra) - 6} more stale entries")

    # ── claims: keyed by ISO, VALUES are province ids ────────────
    #
    # Not by province id like armies and ports. Checking it the same way as
    # those reported every claimant as a bad key, which is the checker being
    # wrong about the format rather than the format being wrong.
    live_iso_early = {v.get("iso_a3") for v in countries.values()}
    for iso, plist in (claims or {}).items():
        if iso not in live_iso_early:
            bad("dangling", f"claims.json: {iso!r} claims ground but is not a "
                            f"country on this map")
        for p in (plist if isinstance(plist, list) else []):
            if int(p) not in tabled:
                bad("dangling", f"claims.json: {iso} claims province {p}, which "
                                f"does not exist")

    # ── foreign keys onto provinces ──────────────────────────────
    for label, obj in (("armies.json", armies), ("ports.json", ports)):
        if not isinstance(obj, dict):
            continue
        for k in obj:
            try:
                p = int(k)
            except (TypeError, ValueError):
                bad("bad-key", f"{label}: key {k!r} is not a province id")
                continue
            if p not in tabled:
                bad("dangling", f"{label} names province {p}, which does not exist")

    # ── countries ────────────────────────────────────────────────
    cids = set(int(k) for k in countries)
    owned = {}
    for k, v in provinces.items():
        c = v.get("country_id")
        if c is None:
            bad("no-owner", f"province {k} has no country_id")
            continue
        owned[int(c)] = owned.get(int(c), 0) + 1
        if int(c) not in cids and int(c) not in SENTINELS:
            bad("dangling", f"province {k} is owned by country {c}, which is "
                            f"not in countries.json")
    for c in sorted(cids):
        if c in SENTINELS:
            continue
        if owned.get(c, 0) == 0:
            iso = countries[str(c)].get("iso_a3", "?")
            nm = countries[str(c)].get("name", "?")
            bad("landless", f"country {c} {iso} ({nm}) holds no province")

    # iso_a3 on a province must match the country it points at
    iso_of = {int(k): v.get("iso_a3") for k, v in countries.items()}
    mismatched = 0
    for k, v in provinces.items():
        c = v.get("country_id")
        if c is None or int(c) in SENTINELS:
            continue
        want = iso_of.get(int(c))
        if want and v.get("iso_a3") and v["iso_a3"] != want:
            mismatched += 1
            if mismatched <= 4:
                bad("iso-mismatch", f"province {k} says iso {v['iso_a3']!r} but "
                                    f"its country {c} is {want!r}")
    if mismatched > 4:
        bad("iso-mismatch", f"...and {mismatched - 4} more")

    # ── armies and ships name real countries ─────────────────────
    for k, units in (armies or {}).items():
        for u in units if isinstance(units, list) else []:
            c = u.get("country_id")
            if c is not None and int(c) not in cids and int(c) not in SENTINELS:
                bad("dangling", f"armies.json province {k} has a unit owned by "
                                f"country {c}, which does not exist")
    for i, s in enumerate(ships if isinstance(ships, list) else []):
        c = s.get("country_id")
        if c is not None and int(c) not in cids and int(c) not in SENTINELS:
            bad("dangling", f"ship {i} is owned by country {c}, which does not exist")

    # ── relations name countries on this map ─────────────────────
    live_iso = {v.get("iso_a3") for v in countries.values()}
    seen = set()
    for a, targets in (relations or {}).items():
        for b in (targets if isinstance(targets, dict) else {}):
            for iso in (a, b):
                if iso not in live_iso and iso not in seen:
                    seen.add(iso)
                    bad("dangling", f"relations.json mentions {iso!r}, which is "
                                    f"not a country on this map")

    # ── values ───────────────────────────────────────────────────
    pop = keyed.get("population.json") or {}
    neg = [k for k, v in pop.items() if isinstance(v, (int, float)) and v < 0]
    for k in neg[:4]:
        bad("bad-value", f"province {k} has negative population {pop[k]}")
    comp = keyed.get("political_compass.json") or {}
    oor = [k for k, v in comp.items()
           if isinstance(v, dict) and not (-100 <= v.get("left", 0) <= 100
                                           and -100 <= v.get("auth", 0) <= 100)]
    for k in oor[:4]:
        bad("bad-value", f"province {k} compass out of range: {comp[k]}")

    return problems, len(tabled), len(cids)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero if anything is inconsistent")
    ap.add_argument("--map", action="append", dest="maps")
    args = ap.parse_args()

    maps = [m if m.endswith(".odmap") else m + ".odmap"
            for m in (args.maps or [])] or sorted(
        f for f in os.listdir(MAPS_DIR) if f.endswith(".odmap"))

    total = 0
    accepted = []
    for name in maps:
        problems, nprov, ncty = check(name)

        def waived(kind, msg):
            for (m, k, needle), why in ACCEPTED.items():
                if m == name and k == kind and needle in msg:
                    return why
            return None

        keep = []
        for kind, msg in problems:
            why = waived(kind, msg)
            if why:
                accepted.append((name, kind, msg, why))
            else:
                keep.append((kind, msg))
        problems = keep
        total += len(problems)
        status = "clean" if not problems else f"{len(problems)} problem(s)"
        print(f"\n=== {name}  ({nprov} provinces, {ncty} countries) -- {status} ===")
        bykind = {}
        for kind, msg in problems:
            bykind.setdefault(kind, []).append(msg)
        for kind in sorted(bykind):
            for msg in bykind[kind][:8]:
                print(f"  [{kind}] {msg}")
            if len(bykind[kind]) > 8:
                print(f"  [{kind}] ...and {len(bykind[kind]) - 8} more")

    print(f"\n{total} inconsistency(ies) across {len(maps)} map(s).")
    if accepted:
        print(f"\n{len(accepted)} known and accepted, not failing the build:")
        for name, kind, msg, why in accepted:
            print(f"  [{kind}] {name}: {msg}")
            print(f"      {why}")
    return 1 if (args.strict and total) else 0


if __name__ == "__main__":
    sys.exit(main())
