#!/usr/bin/env python3
"""
Build a historical scenario .odmap from the Modern Day base map.

WHY SCENARIOS SHARE ONE PROVINCE LAYER

A scenario is the same 1,631 provinces under different owners. Nothing about
the geometry changes -- provinces.png, land_sea.png and the province ids are
copied through byte for byte -- so saves, mods, the map editor and every tool
that indexes by province id keep working across all of them.

That is a deliberate choice and not only a cheap one. The alternative is
per-era boundary geometry, and the world historical boundary datasets that
exist are, with one exception, unusable here. Checked August 2026:

  historical-basemaps (aourednik)  GPL-3.0 (its LICENSE file is GPLv3 verbatim).
                                   Copyleft on the data. Used as a REFERENCE
                                   ONLY, never shipped -- see
                                   tools/check_map_history.py.
  CShapes 2.0 (ETH Zurich)         CC BY-NC-SA 4.0. NonCommercial, so it is
                                   disqualified outright and no amount of
                                   attribution fixes it. (An earlier note here
                                   said "citation request, no grant", lumping
                                   it in with GREG and ACOR. That was wrong: it
                                   does carry a licence, and the licence is
                                   worse than no licence would have been.)
  GADM                             Free for academic and non-commercial use;
                                   redistribution or commercial use needs
                                   permission. Modern boundaries only, and the
                                   obvious thing to reach for by mistake.
  Euratlas, GeaCron                Commercial, per-seat.
  OpenHistoricalMap                CC0, and therefore the ONLY historical
                                   boundary source with no obligation at all.
                                   Coverage is the problem rather than the
                                   licence: modern borders are broadly there,
                                   historical ones are contributor-driven and
                                   patchy, with no complete world snapshot for
                                   any of this project's five years.
  geoBoundaries                    CC BY 4.0, commercial use fine. Modern only,
                                   so it is the permissive replacement for GADM
                                   and not for anything here.

The route with no obligations and no coverage gap is not a dataset at all: it
is public-domain SOURCE MAPS, traced into this project's own vectors. CIA and
Army Map Service sheets are US federal works and public domain by 17 U.S.C.
105; anything published before 1930 is public domain by age. Both are
available in bulk from the Perry-Castaneda collection and the Library of
Congress. That is what carve_borders.py already does by hand for the
German-Polish frontier.

So the honest options were "approximate historical borders with modern province
outlines" or "do not ship historical scenarios". The approximation is visible in
a few places -- interwar Poland's eastern border runs through modern Ukrainian
and Belarusian provinces, Austria-Hungary has no crownland divisions -- and the
scenario file can split the difference with bounding boxes. Colonial Africa
needs almost no correction, because those borders largely *are* the modern ones.

Where a scenario genuinely needs a province the base map does not have, split it
in the map editor and export a new base; nothing here assumes 1,631.

WHAT THE GAME ACTUALLY READS

political.png is NOT the in-game political layer. Game_Loading.cpp builds that
from provinces.png plus the colours in countries.json and computes the border
gradient itself at load. political.png is only the map browser's preview, so a
flat recolour is correct and cheap.

USAGE

    python3 tools/generate_scenario.py 1939
    python3 tools/generate_scenario.py --all
    python3 tools/generate_scenario.py 1939 --base data/STDmaps/map.odmap

SCENARIO FORMAT -- tools/data/scenarios/<id>.json

    {
      "schema": 1,
      "id": "1939",
      "name": "The Gathering Storm",
      "date": "1 September 1939 AD",
      "description": "shown in the map browser",

      "population_scale": 0.42,     // world population vs the 2000 baseline
      "industry_scale":   0.35,     // province industry income vs 2000

      "powers": [
        {
          "iso":   "GER",            // scenario-local, need not be a real ISO
          "name":  "German Reich",
          "color": "#6f7a63",
          "owns":  ["DEU", "AUT", "CZE"],   // whole modern countries
          "boxes": [                        // parts of others; most specific wins
            {"from": "POL", "bbox": [14.1, 53.9, 19.6, 54.9]}
          ],
          "flag_from": "DEU",        // reuse a base-map flag, when unchanged
          "flag": {"type": "hstripes_3", "colors": ["#000", "#fff", "#c00"]},
          "pop_scale": 0.9,          // multiplies population_scale
          "army": 3700000,           // men, spread over provinces by population
          "navy": 22,                // ships, berthed at this power's ports
          "compass": {"left": -60, "auth": 95},
          "policies": ["conscription", "war_economy"]
        }
      ],

      "relations": {
        "wars":      [["GER", "POL"]],
        "alliances": [["GBR", "FRA"]],
        "guarantees":[["GBR", "POL"]]
      },
      "claims": [ {"by": "GER", "on": "POL"} ]   // whole-country claims
    }

Resolution order for a province, most specific first: `provinces` (explicit
pid), `boxes`, `owns`, then Unclaimed. Any modern country nobody claims is
reported by name rather than quietly becoming sea.
"""

import json
import os
import shutil
import sys
import zipfile

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(TOOLS_DIR)
DATA_DIR = os.path.join(PROJECT_ROOT, "data")
STDMAPS = os.path.join(DATA_DIR, "STDmaps")
TOOLS_DATA = os.path.join(TOOLS_DIR, "data")
SCENARIO_DIR = os.path.join(TOOLS_DATA, "scenarios")
DEFAULT_BASE = os.path.join(STDMAPS, "map.odmap")

MAP_W, MAP_H = 8192, 4096
UNC_CID = 65534          # Game.h
BLC_CID = 65535

# Carried through untouched: geometry, and data that is not era-specific.
COPY_VERBATIM = ["land_sea.png", "provinces.png"]
# Carried through with a note. Modern ethnic composition is wrong for a 1939
# Europe that has not had its post-war expulsions yet, but it is a great deal
# closer than nothing, and the province panel needs *something*.
COPY_WITH_CAVEAT = ["minorities.json", "minority_colors.json", "policies.json"]


# The game advances the date with sscanf(m_mapDate, "%s %d") and then matches
# the first token against this list (Game_TurnLogic.cpp:360). A date with a day
# in it -- "1 September 1939 AD" -- puts "1" in the month slot, the match fails,
# and the year silently falls back to the initialiser, which is 2000. So the
# first turn of a 1939 scenario lands in January 2001. Hence the check below:
# it is cheaper to refuse the date here than to explain that bug later.
MONTHS = ["January", "February", "March", "April", "May", "June",
          "July", "August", "September", "October", "November", "December"]


def die(msg):
    print(f"error: {msg}")
    sys.exit(1)


def validate_date(date):
    parts = date.split()
    if len(parts) < 2 or parts[0] not in MONTHS or not parts[1].lstrip("-").isdigit():
        die(f'map_date {date!r} will not parse. The game reads "<Month> <Year> <AD|BC>" '
            f'and resets the year to 2000 on anything else — no day of the month, '
            f'month spelled in full. Try "{MONTHS[8]} 1939 AD".')


def validate_policies(scen, base=None):
    """Every starting policy must be an id policies.json actually defines.

    data/policies.json exists only DURING a pipeline run -- step 20 sweeps it --
    so reading only from there meant this check quietly did nothing on every
    standalone invocation, which is most of them. The base map carries its own
    copy; use that when the loose file is gone.
    """
    known = None
    path = os.path.join(DATA_DIR, "policies.json")
    try:
        with open(path) as f:
            known = {p["id"] for p in json.load(f)["policies"]}
    except (FileNotFoundError, KeyError, json.JSONDecodeError):
        pass
    if known is None and base is not None and "policies.json" in base["_names"]:
        try:
            known = {p["id"] for p in json.loads(base["_zip"].read("policies.json"))["policies"]}
        except (KeyError, json.JSONDecodeError):
            known = None
    if known is None:
        print("  policies.json not found in data/ or in the base map — "
              "starting policies NOT checked")
        return
    bad = {}
    for power in scen["powers"]:
        for pol in power.get("policies", []):
            if pol not in known:
                bad.setdefault(pol, []).append(power["iso"])
    if bad:
        listing = "; ".join(f"{p} ({', '.join(sorted(isos))})" for p, isos in sorted(bad.items()))
        die(f"unknown policy id(s): {listing}\n"
            f"       known ids: {', '.join(sorted(known))}")


def hex_to_rgb(h):
    h = h.lstrip("#")
    if len(h) == 3:
        h = "".join(c * 2 for c in h)
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


# ── base map ────────────────────────────────────────────────────────
def load_base(path):
    if not os.path.exists(path):
        die(f"base map not found: {path}")
    z = zipfile.ZipFile(path)
    base = {"_zip": z, "_names": set(z.namelist())}
    for name in ("provinces.json", "countries.json", "population.json",
                 "resources.json", "ports.json"):
        base[name] = json.loads(z.read(name))
    return base


def province_centers(base):
    """pid -> (lon, lat). Computed from provinces.png; ~2s with numpy."""
    import numpy as np
    from PIL import Image
    import io as _io
    img = Image.open(_io.BytesIO(base["_zip"].read("provinces.png"))).convert("RGB")
    arr = np.array(img, dtype=np.uint8)
    h, w, _ = arr.shape
    ids = (arr[:, :, 0].astype(np.uint32) << 16 |
           arr[:, :, 1].astype(np.uint32) << 8 |
           arr[:, :, 2].astype(np.uint32))
    flat = ids.ravel()
    mask = flat != 0
    yg, xg = np.mgrid[0:h, 0:w]
    fx = xg.ravel()[mask].astype(np.float64)
    fy = yg.ravel()[mask].astype(np.float64)
    vid = flat[mask]
    order = np.argsort(vid, kind="stable")
    vid, fx, fy = vid[order], fx[order], fy[order]
    uniq, start = np.unique(vid, return_index=True)
    end = np.append(start[1:], len(vid))
    out = {}
    for pid, s, e in zip(uniq, start, end):
        cx = fx[s:e].mean() * (MAP_W / w)
        cy = fy[s:e].mean() * (MAP_H / h)
        out[int(pid)] = (cx / MAP_W * 360.0 - 180.0, 90.0 - cy / MAP_H * 180.0)
    return out


def sea_mask(base):
    """Boolean array, True where land_sea.png says water."""
    import numpy as np
    from PIL import Image
    import io as _io
    img = Image.open(_io.BytesIO(base["_zip"].read("land_sea.png"))).convert("L")
    arr = np.array(img, dtype=np.uint8)
    return arr < 128


# ── ownership ───────────────────────────────────────────────────────
def expand_independents(base, scen):
    """`independent_default`: every unclaimed modern country becomes itself.

    Without this a 1962 scenario is 120 near-identical entries saying "Ghana
    owns Ghana", and the diff against 1939 -- which is the interesting part --
    is buried in them. A scenario lists what differs from the present day; the
    rest is filled in from the base map's own names, flags and colours.
    """
    if not scen.get("independent_default"):
        return
    claimed = {iso for p in scen["powers"] for iso in p.get("owns", [])}
    claimed |= {p["iso"] for p in scen["powers"]}
    skip = set(scen.get("independent_except", [])) | {"UNC", "BLC"}

    added = []
    for country in base["countries.json"].values():
        iso = country["iso_a3"]
        if iso in claimed or iso in skip:
            continue
        scen["powers"].append({
            "iso": iso,
            "name": country["name"],
            "color": country["color"],
            "owns": [iso],
            "flag_from": iso,
            "_independent_default": True,
        })
        added.append(iso)
    if added:
        print(f"  {len(added)} countries defaulted to independent: "
              f"{', '.join(sorted(added))}")


def resolve_ownership(base, scen):
    """pid -> power iso. Reports every modern country nobody claimed."""
    provinces = base["provinces.json"]
    centers = None

    owns = {}                       # modern iso -> power iso
    for power in scen["powers"]:
        for iso in power.get("owns", []):
            if iso in owns:
                die(f"{iso} is claimed by both {owns[iso]} and {power['iso']}")
            owns[iso] = power["iso"]

    boxes = []                      # (from_iso or None, bbox, power iso)
    for power in scen["powers"]:
        for b in power.get("boxes", []):
            boxes.append((b.get("from"), b["bbox"], power["iso"]))
    if boxes:
        centers = province_centers(base)

    explicit = {}                   # pid -> power iso
    for power in scen["powers"]:
        for pid in power.get("provinces", []):
            explicit[int(pid)] = power["iso"]

    owner = {}
    unclaimed_isos = {}
    for pid_str, prov in provinces.items():
        pid = int(pid_str)
        modern = prov.get("iso_a3", "")

        if pid in explicit:
            owner[pid] = explicit[pid]
            continue

        hit = None
        if centers and pid in centers:
            lon, lat = centers[pid]
            for from_iso, bbox, power_iso in boxes:
                if from_iso and from_iso != modern:
                    continue
                x0, y0, x1, y1 = bbox
                if x0 <= lon <= x1 and y0 <= lat <= y1:
                    hit = power_iso
                    break
        if hit:
            owner[pid] = hit
            continue

        if modern in owns:
            owner[pid] = owns[modern]
        elif modern in ("UNC", "BLC"):
            owner[pid] = "UNC"
        else:
            owner[pid] = "UNC"
            unclaimed_isos[modern] = unclaimed_isos.get(modern, 0) + 1

    if unclaimed_isos:
        listing = ", ".join(f"{iso} ({n})" for iso, n in sorted(unclaimed_isos.items()))
        print(f"  {sum(unclaimed_isos.values())} provinces in {len(unclaimed_isos)} "
              f"modern countries nobody claimed -> Unclaimed: {listing}")
    return owner


# ── per-scenario province geometry ──────────────────────────────────
def province_adjacency(base):
    """(a, b) pairs of base provinces that share a border."""
    import numpy as np
    from PIL import Image
    import io as _io
    img = Image.open(_io.BytesIO(base["_zip"].read("provinces.png"))).convert("RGB")
    a = np.array(img, dtype=np.uint32)
    ids = a[:, :, 0] << 16 | a[:, :, 1] << 8 | a[:, :, 2]
    pairs = set()
    for x, y in ((ids[:, :-1], ids[:, 1:]), (ids[:-1, :], ids[1:, :])):
        m = (x != y) & (x != 0) & (y != 0)
        for u, v in zip(x[m].tolist(), y[m].tolist()):
            pairs.add((u, v) if u < v else (v, u))
    adj = {}
    for u, v in pairs:
        adj.setdefault(u, set()).add(v)
        adj.setdefault(v, set()).add(u)
    return adj


def province_areas(base):
    import numpy as np
    from PIL import Image
    import io as _io
    img = Image.open(_io.BytesIO(base["_zip"].read("provinces.png"))).convert("RGB")
    a = np.array(img, dtype=np.uint32)
    ids = (a[:, :, 0] << 16 | a[:, :, 1] << 8 | a[:, :, 2]).ravel()
    ids = ids[ids != 0]
    u, c = np.unique(ids, return_counts=True)
    return dict(zip(u.tolist(), c.tolist()))


def rebuild_geometry(base, scen, owner, centers):
    """Re-cut the province layer along the scenario's OWN country borders.

    A scenario used to inherit the Modern Day province layer whole, which meant
    1914 Austria-Hungary was drawn with the internal borders of seven modern
    countries running through it, and was allocated the sum of seven countries'
    provinces. The seams were in places that did not exist yet and the count was
    wrong.

    So each power is re-cut as one country: its base provinces are clustered
    into as many groups as the area formula gives a country that size, growing
    each cluster from a seed across the adjacency graph. Cluster borders are
    unions of base province borders -- no new geometry is invented, nothing
    crosses a scenario country border, and the province layer is the scenario's
    rather than the present day's.

    Returns pid -> new province id, or None when the scenario opts out.
    """
    if not scen.get("rebuild_geometry", True):
        return None

    import math
    import random as _random

    adj = province_adjacency(base)
    areas = province_areas(base)

    by_power = {}
    for pid, iso in owner.items():
        by_power.setdefault(iso, []).append(pid)

    scale = float(scen.get("geometry_scale", 1.0))
    remap = {}
    next_id = 1
    rng = _random.Random(20260728)

    def components(pids):
        """Connected landmasses within one power's territory.

        Allocating per POWER rather than per landmass was wrong in the way that
        matters most here: the British Empire is one power spanning a quarter of
        the world, so a single sqrt() allocation capped at 120 gave British
        India the same treatment as Britain. The base generator allocates per
        connected component for exactly this reason -- India, Canada, Australia
        and the home islands are separate landmasses and each earns its own
        provinces.
        """
        remaining = set(pids)
        out = []
        while remaining:
            seed = remaining.pop()
            group, stack = [seed], [seed]
            while stack:
                cur = stack.pop()
                for nb in adj.get(cur, ()):
                    if nb in remaining:
                        remaining.discard(nb)
                        group.append(nb)
                        stack.append(nb)
            out.append(sorted(group))
        return out

    for iso in sorted(by_power):
      for pids in components(sorted(by_power[iso])):
          total_area = sum(areas.get(p, 0) for p in pids)
          if total_area <= 0:
              for p in pids:
                  remap[p] = next_id
              next_id += 1
              continue

          # The generator's own allocation, so a scenario country gets what a
          # country that size and shape would get on the base map.
          lat = sum(centers[p][1] for p in pids if p in centers) / max(1, len(pids))
          lon = sum(centers[p][0] for p in pids if p in centers) / max(1, len(pids))
          merc = max(math.cos(math.radians(lat)), 0.3)
          europe = 2.0 if (35.0 <= lat <= 70.0 and -10.0 <= lon <= 40.0) else 1.0
          want = int(math.sqrt(total_area * merc * europe / 250.0) * scale)
          want = max(1, min(120, want, len(pids)))

          if want >= len(pids):
              for p in pids:
                  remap[p] = next_id
                  next_id += 1
              continue

          # Seeds spread by centre distance, then simultaneous region growing so
          # clusters stay compact and connected.
          member = set(pids)
          shuffled = list(pids)
          rng.shuffle(shuffled)
          seeds = []
          min_sep = math.sqrt(total_area / want) * 0.6
          for p in shuffled:
              if len(seeds) >= want:
                  break
              if p not in centers:
                  continue
              if all(math.dist(centers[p], centers[q]) > min_sep / 60.0 for q in seeds):
                  seeds.append(p)
          for p in shuffled:
              if len(seeds) >= want:
                  break
              if p not in seeds:
                  seeds.append(p)

          cluster = {}
          frontier = []
          for i, sp in enumerate(seeds):
              cluster[sp] = i
              frontier.append(sp)
          qi = 0
          while qi < len(frontier):
              cur = frontier[qi]; qi += 1
              for nb in adj.get(cur, ()):
                  if nb in member and nb not in cluster:
                      cluster[nb] = cluster[cur]
                      frontier.append(nb)
          # Islands the growth could not reach join the nearest seed's cluster.
          for p in pids:
              if p in cluster:
                  continue
              if p in centers and seeds:
                  near = min(seeds, key=lambda q: math.dist(centers[p], centers[q])
                             if q in centers else 1e9)
                  cluster[p] = cluster[near]
              else:
                  cluster[p] = 0

          local = {}
          for p in pids:
              k = cluster[p]
              if k not in local:
                  local[k] = next_id
                  next_id += 1
              remap[p] = local[k]

    print(f"  geometry re-cut along this scenario's borders: "
          f"{len(set(remap.values()))} provinces from {len(remap)} base provinces")
    return remap


def apply_geometry(base, remap, files, owner, cid_of):
    """Rewrite provinces.json and merge per-province data onto the new layer."""
    new_prov = {}
    for pid, npid in remap.items():
        iso = owner.get(pid, "UNC")
        new_prov.setdefault(str(npid), {
            "id": npid, "name": "", "country_id": cid_of.get(iso, UNC_CID),
            "iso_a3": iso, "color": "#%06x" % npid,
        })
    files["provinces.json"] = new_prov

    # Population sums; a merged province holds everyone in it.
    pop = files["population.json"]
    merged_pop = {}
    for pid, npid in remap.items():
        merged_pop[str(npid)] = merged_pop.get(str(npid), 0) + pop.get(str(pid), 0)
    files["population.json"] = merged_pop

    # Resources: deposits add up, industry level takes the best of the group --
    # merging two provinces should not dilute the factory in one of them.
    res = files["resources.json"]
    out = {}
    for pid, npid in remap.items():
        src = res.get(str(pid))
        if not src:
            continue
        dst = out.setdefault(str(npid), json.loads(json.dumps(src)))
        if dst is src:
            continue
        for key in ("oil", "gold", "rubber", "gemstones", "metal"):
            if key in src and key in dst:
                dst[key]["a"] = round(min(100.0, dst[key]["a"] + src[key]["a"] * 0.5), 1)
                dst[key]["b"] = round(dst[key]["a"] * 0.4, 1)
        if "industry" in src and "industry" in dst:
            d, sI = dst["industry"], src["industry"]
            d["level"] = max(d.get("level", 0), sI.get("level", 0))
            for k in ("income", "resourceIncome", "popIncome"):
                d[k] = round(d.get(k, 0) + sI.get(k, 0), 2)
            d["fortification"] = max(d.get("fortification", 0), sI.get("fortification", 0))
    files["resources.json"] = out

    ports = files.get("ports.json", {})
    merged_ports = {}
    for pid, npid in remap.items():
        p = ports.get(str(pid))
        if p:
            cur = merged_ports.get(str(npid))
            if not cur or p.get("level", 0) > cur.get("level", 0):
                merged_ports[str(npid)] = p
    files["ports.json"] = merged_ports
    return new_prov


# ── censoring ───────────────────────────────────────────────────────
# "Show actual flags" is a censoring option, not a flag randomiser. Turning it
# off used to hand every power with a real flag a solid rectangle in its map
# colour -- 28 of 65 nations in 1939, including Ireland, Luxembourg and Panama,
# none of which has anything to censor. What it produces now:
#
#   authored `flag_censored`  -> that design (the 1939 Reich tricolour)
#   listed in scenario_flags  -> the real flag, mosaicked by the renderer
#   anything else             -> the real flag, untouched; the toggle is a no-op
#
# which is the shape data/STDmaps/map.odmap already had and the scenarios did
# not. The list lives in tools/data/scenario_flags.json so this generator and
# tools/fix_censored_flags.py cannot disagree about it.
def censor_set():
    try:
        with open(os.path.join(TOOLS_DATA, "scenario_flags.json")) as f:
            return set(json.load(f).get("censor", []))
    except Exception:
        return set()


def censored_flag_for(power, actual):
    """The `flag_censored` entry for one power, given its `flag_actual`."""
    authored = power.get("flag_censored")
    if authored:
        return dict(authored, censored=True)
    name = power.get("flag_file") or ""
    if not name and isinstance(actual, dict) and actual.get("image"):
        name = os.path.splitext(os.path.basename(actual["image"]))[0]
    # `censored` omitted rather than written false, which is how the base map
    # spells it -- so regenerating a scenario produces no diff against a map
    # tools/fix_censored_flags.py has already corrected.
    return dict(actual, censored=True) if name in censor_set() else dict(actual)


# ── country table ───────────────────────────────────────────────────
def build_countries(base, scen, owner):
    """countries.json for the scenario, plus power iso -> cid."""
    base_countries = base["countries.json"]
    flag_by_iso = {c["iso_a3"]: (c.get("flag_actual"), c.get("flag_censored"))
                   for c in base_countries.values()}

    used = sorted({o for o in owner.values() if o != "UNC"},
                  key=lambda iso: [p["iso"] for p in scen["powers"]].index(iso))
    cid_of = {iso: i + 1 for i, iso in enumerate(used)}
    cid_of["UNC"] = UNC_CID

    out = {}
    used_flag_files = set()
    for power in scen["powers"]:
        iso = power["iso"]
        if iso not in cid_of:
            print(f"  {iso} ({power['name']}) owns no province — dropped")
            continue
        cid = cid_of[iso]

        # A flag is either a base-map image reused where the design did not
        # change, or a procedural pattern the renderer draws from colours.
        # Nothing here downloads artwork, so a scenario adds no licence surface.
        if power.get("flag_file"):
            # A real historical flag, downloaded by download_scenario_flags.py
            # and rasterised into data/flags/.
            actual = {"image": "flags/" + power["flag_file"] + ".png"}
            used_flag_files.add(power["flag_file"])
        elif power.get("flag_from") and power["flag_from"] in flag_by_iso:
            actual = flag_by_iso[power["flag_from"]][0]
        elif power.get("flag"):
            actual = dict(power["flag"])
        else:
            r, g, b = hex_to_rgb(power["color"])
            light = "#%02x%02x%02x" % (min(r + 60, 255), min(g + 60, 255), min(b + 60, 255))
            dark = "#%02x%02x%02x" % (max(r - 50, 0), max(g - 50, 0), max(b - 50, 0))
            actual = {"type": "hstripes_3", "colors": [dark, power["color"], light]}

        censored = censored_flag_for(power, actual)

        out[str(cid)] = {
            "id": cid,
            "iso_a3": iso,
            "name": power["name"],
            "color": power["color"],
            "flag_actual": actual,
            "flag_censored": censored,
            "treasury": 10.0,      # replaced below, once income is known
        }

    for special in (UNC_CID, BLC_CID):
        if str(special) in base_countries:
            out[str(special)] = base_countries[str(special)]

    missing = sorted(f for f in used_flag_files
                     if not os.path.exists(os.path.join(DATA_DIR, "flags", f + ".png")))
    if missing:
        print(f"  flag image(s) referenced but not on disk, so the power will "
              f"render blank: {', '.join(missing)}\n"
              f"    run: python3 tools/download_scenario_flags.py")
    return out, cid_of, used_flag_files


# ── era scaling ─────────────────────────────────────────────────────
def scale_population(base, scen, owner, power_by_iso):
    """Per-province population for the era.

    A power may give an absolute `population`, which is then distributed over
    its provinces in proportion to their modern population. That is the form to
    prefer: a single global scale cannot be right, because Europe's share of
    world population in 1939 was far larger than in 2000 and Africa's far
    smaller. A flat 0.38 gives Poland 14 million when it had 35.

    Powers with no figure fall back to `population_scale` x `pop_scale`, which
    is also what unclaimed land gets.
    """
    world = float(scen.get("population_scale", 1.0))
    base_pop = base["population.json"]

    modern_total = {}
    for pid_str, pop in base_pop.items():
        modern_total[owner.get(int(pid_str), "UNC")] = \
            modern_total.get(owner.get(int(pid_str), "UNC"), 0) + pop

    out = {}
    for pid_str, pop in base_pop.items():
        iso = owner.get(int(pid_str), "UNC")
        power = power_by_iso.get(iso)
        target = power.get("population") if power else None
        if target and modern_total.get(iso):
            out[pid_str] = max(0, int(round(pop * float(target) / modern_total[iso])))
        else:
            local = float(power.get("pop_scale", 1.0)) if power else 1.0
            out[pid_str] = max(0, int(round(pop * world * local)))

    missing = sorted(p["iso"] for p in scen["powers"]
                     if "population" not in p and p["iso"] in modern_total
                     and not p.get("_independent_default"))
    if missing:
        print(f"  {len(missing)} power(s) have no population figure and used the "
              f"global scale: {', '.join(missing)}")
    return out


def scale_resources(base, scen):
    """Industry for the era. Deposits stay where geology put them.

    Keyed on the province's MODERN country, not on the scenario power that owns
    it, because the base map's industry levels encode modern industrial
    geography and that is what needs correcting. Britain and India are one power
    in 1939 and could not be less alike industrially; a per-power factor cannot
    express that, and a single global factor flattens everything to level 1.
    """
    default = float(scen.get("industry_scale", 1.0))
    by_iso = scen.get("industry_by_iso", {})
    cap = int(scen.get("industry_level_cap", 10))
    provinces = base["provinces.json"]

    out = json.loads(json.dumps(base["resources.json"]))
    for pid_str, entry in out.items():
        ind = entry.get("industry")
        if not ind:
            continue
        modern = provinces.get(pid_str, {}).get("iso_a3", "")
        factor = float(by_iso.get(modern, default))
        ind["level"] = max(0, min(cap, int(round(ind.get("level", 0) * factor))))
        for key in ("income", "resourceIncome", "popIncome"):
            if key in ind:
                ind[key] = round(ind[key] * factor, 2)
    return out


def build_armies(scen, owner, population, cid_of):
    """Spread each power's manpower over its provinces, weighted by population."""
    by_power = {}
    for pid, iso in owner.items():
        if iso != "UNC":
            by_power.setdefault(iso, []).append(pid)

    default_ratio = float(scen.get("default_army_ratio", 0.006))
    armies = {}
    for power in scen["powers"]:
        iso = power["iso"]
        pids = by_power.get(iso)
        if not pids:
            continue
        total = int(power.get("army", 0))
        if not total:
            # No figure authored. Raise a garrison from population instead of
            # leaving the country undefended -- an unarmed Ghana is a worse
            # error than an approximate one, and every scenario has a long tail
            # of countries nobody is going to hand-tune.
            held = sum(population.get(str(p), 0) for p in pids)
            total = int(held * default_ratio)
        if total <= 0:
            continue
        weights = {pid: max(1, population.get(str(pid), 0)) for pid in pids}
        pool = sum(weights.values())
        # Garrison the 40% most populous provinces; a nation does not station
        # troops evenly across every square of its own territory.
        ranked = sorted(pids, key=lambda p: -weights[p])
        chosen = ranked[:max(1, int(len(ranked) * 0.4))]
        pool = sum(weights[p] for p in chosen)
        for pid in chosen:
            count = int(round(total * weights[pid] / pool))
            if count <= 0:
                continue
            armies.setdefault(str(pid), []).append(
                {"country_id": cid_of[iso], "count": count})
    return armies


def build_ships(scen, owner, base, cid_of, centers, ports=None, province_ids=None):
    """Berth each power's fleet at its own ports -- in open water.

    Ships were first placed at the port province's centroid, which is a point
    on land, and 95% of a scenario's fleet ended up inland. The fix for that
    walked the hull out to the nearest water pixel and nudged it four pixels
    further, which moved the fleet into the sea by the only test anyone was
    running -- and left 98% of it within four pixels of the shore, under an
    icon twelve pixels across. Correct coordinates, ships drawn in fields.

    So the berth is chosen by clearance now: the search wants open water on
    every side of the hull, prefers more of it to less, and keeps hulls apart
    from one another. The rules and the reasoning are in naval_placement.py,
    which tools/fix_naval_layer.py also uses so that regenerating a map cannot
    quietly undo a repair made to a shipped one.
    """
    import numpy as np
    sys.path.insert(0, TOOLS_DIR)
    from naval_placement import (clearance_field, coastal_anchor, find_berth,
                                 hull_type, pixel_to_lonlat)
    from fill_water_speckle import label_components, min_water_body

    sea = sea_mask(base)
    sh, sw = sea.shape
    land = ~sea
    if province_ids is None:
        province_ids = province_id_array(base)

    lbl, sizes = label_components(sea)
    big_water = (lbl > 0) & (sizes[lbl] >= min_water_body())
    clear = clearance_field(land)

    if ports is None:
        ports = base["ports.json"]

    def to_px(lon, lat):
        return (int((lon + 180.0) / 360.0 * sw) % sw,
                min(sh - 1, max(0, int((90.0 - lat) / 180.0 * sh))))

    # A harbour with no water wide enough for a hull is not a harbour. A
    # dammed river reservoir clears MIN_WATER_BODY on pixel count alone and is
    # one pixel across, so the coastal test says yes and there is still
    # nowhere to float anything.
    by_power, unusable = {}, 0
    for pid_str, info in ports.items():
        pid = int(pid_str)
        iso = owner.get(pid, "UNC")
        if iso == "UNC" or pid not in centers:
            continue
        anchor = coastal_anchor(province_ids == pid, big_water,
                                to_px(*centers[pid]))
        if find_berth(clear, anchor[0], anchor[1], []) is None:
            unusable += 1
            continue
        by_power.setdefault(iso, []).append((pid, info.get("level", 1), anchor))

    ships = []
    taken = []
    stranded = 0
    for power in scen["powers"]:
        iso, n = power["iso"], int(power.get("navy", 0))
        harbours = sorted(by_power.get(iso, []), key=lambda t: (-t[1], t[0]))
        if not n or not harbours:
            continue
        for i in range(n):
            _pid, _level, anchor = harbours[i % len(harbours)]
            berth = find_berth(clear, anchor[0], anchor[1], taken)
            if berth is None:
                stranded += 1
                continue
            bx, by, _c = berth
            taken.append((bx, by))
            lon, lat = pixel_to_lonlat(bx, by, sw, sh)
            ships.append({
                "country_id": cid_of[iso],
                "type": hull_type(i),
                "lat": round(lat, 6),
                "lon": round(lon, 6),
                "health": 100,
                "crew": 400 + (i * 37) % 900,
            })
    if unusable:
        print(f"  {unusable} port(s) have no water wide enough for a hull and "
              f"were not used as a berth")
    if stranded:
        print(f"  {stranded} ship(s) found no berth within reach of their port "
              f"and were dropped")
    return ships


def build_relations(scen):
    rel = {}

    def link(a, b, key):
        rel.setdefault(a, {}).setdefault(b, {})[key] = True
        rel.setdefault(b, {}).setdefault(a, {})[key] = True

    for a, b in scen.get("relations", {}).get("wars", []):
        link(a, b, "war")
    for group in scen.get("relations", {}).get("alliances", []):
        for i, a in enumerate(group):
            for b in group[i + 1:]:
                link(a, b, "ally")
    for a, b in scen.get("relations", {}).get("guarantees", []):
        link(a, b, "guarantee")
    for a, b in scen.get("relations", {}).get("non_aggression", []):
        link(a, b, "nonAggression")
    return rel


def validate_claims(scen):
    """A claim names powers, not modern countries.

    `{"by": "SOM", "on": "XSO"}` reads correctly and does nothing: in 1962 the
    holder of Somaliland is Britain, so the target has to be GBR narrowed by a
    bbox. Caught here rather than at the shrug in build_claims, because a claim
    that silently does nothing looks exactly like a claim nobody wrote.
    """
    powers = {p["iso"] for p in scen["powers"]}
    bad = [f"{c['by']}->{c['on']}" for c in scen.get("claims", [])
           if c["by"] not in powers or c["on"] not in powers]
    if bad:
        die("claim(s) naming something that is not a power in this scenario: "
            + ", ".join(bad)
            + "\n       a claim targets whoever HOLDS the ground; narrow with bbox")


def build_claims(scen, owner, centers, province_ids=None):
    """Territorial claims, optionally narrowed to a region.

    A claim without a `bbox` covers everything the target holds, which is right
    for Taiwan and China and wrong for almost everything else: Germany in 1939
    wanted Danzig and Silesia, not Lwow, and a claim on all of Poland reads as
    a claim on all of Poland. So `bbox` filters the target's provinces down to
    the region actually being claimed, and `note` records what it is meant to be
    so the next person editing the file knows.

    Selection is by OVERLAP, not by province centre. Testing centres worked while
    provinces were small and broke as soon as they were not: Alsace-Lorraine,
    Southern Dobruja and Trieste are all narrower than one province now, so no
    centre fell inside and ten claims across five scenarios silently fell back to
    "nearest province". Widening the boxes to fix that is the wrong repair -- the
    box that contains Alsace-Lorraine's province also reaches the Ruhr, so the
    claim grows to territory nobody claimed. A province that covers part of the
    region IS in the region; that is what the box was drawing.
    """
    by_power = {}
    for pid, iso in owner.items():
        by_power.setdefault(iso, []).append(pid)

    overlapping = None
    if province_ids is not None:
        import numpy as np
        h, w = province_ids.shape

        def overlapping(bbox):
            x0, y0, x1, y1 = bbox
            px0 = max(0, int((x0 + 180.0) / 360.0 * w))
            px1 = min(w, int((x1 + 180.0) / 360.0 * w) + 1)
            py0 = max(0, int((90.0 - y1) / 180.0 * h))
            py1 = min(h, int((90.0 - y0) / 180.0 * h) + 1)
            if px0 >= px1 or py0 >= py1:
                return set()
            return {int(v) for v in np.unique(province_ids[py0:py1, px0:px1]) if v}

    claims = {}
    for c in scen.get("claims", []):
        pids = by_power.get(c["on"], [])
        bbox = c.get("bbox")
        if bbox:
            if overlapping is not None:
                hit = overlapping(bbox)
                pids = [p for p in pids if p in hit]
            else:
                x0, y0, x1, y1 = bbox
                pids = [p for p in pids if p in centers
                        and x0 <= centers[p][0] <= x1 and y0 <= centers[p][1] <= y1]
        if pids:
            claims.setdefault(c["by"], []).extend(pids)
        else:
            print(f"  claim {c['by']} on {c['on']}"
                  + (f" ({c['note']})" if c.get("note") else "")
                  + " matched no province and was dropped")
    return {k: sorted(set(v)) for k, v in claims.items()}


def build_compass(scen, owner):
    country = {p["iso"]: p.get("compass", {"left": 0, "auth": 0}) for p in scen["powers"]}
    province = {}
    for pid, iso in owner.items():
        c = country.get(iso, {"left": 0, "auth": 0})
        # Deterministic per-province wobble so the compass view is not flat.
        jitter = (pid * 2654435761) % 21 - 10
        province[str(pid)] = {
            "left": max(-100, min(100, c["left"] + jitter)),
            "auth": max(-100, min(100, c["auth"] + (jitter // 2))),
        }
    return country, province


def compute_treasuries(countries, owner, resources, cid_of):
    income = {}
    for pid, iso in owner.items():
        ind = resources.get(str(pid), {}).get("industry")
        if not ind or iso == "UNC":
            continue
        income[iso] = income.get(iso, 0.0) + (
            ind.get("income", 0) + ind.get("resourceIncome", 0) + ind.get("popIncome", 0))
    for entry in countries.values():
        entry["treasury"] = max(10.0, round(income.get(entry["iso_a3"], 0.0), 2))


def build_minorities(scen, owner, centers):
    """Per-province ethnic composition for the era, or None to carry the base.

    Carrying the modern composition into 1939 puts post-war Europe on a pre-war
    map: no Germans east of the Oder, no Jewish population in Poland or the
    Pale, Turkey without its Greek and Armenian communities. Those are not
    rounding errors, they are the demographics the era's politics ran on, and
    this game has an ethnic-policy system that reads them.

    `ethnic_overrides` in a scenario replaces the composition of a modern
    country, keyed by modern ISO because that is what the base map's provinces
    are labelled with. Everything else -- concentration boxes, the jitter, the
    colour table -- is generate_minorities.py's, imported rather than copied so
    the two cannot drift.
    """
    overrides = scen.get("ethnic_overrides")
    if not overrides:
        return None, None

    sys.path.insert(0, TOOLS_DIR)
    import generate_minorities as gm

    doc, iso_region = gm.load_groups()
    aliases, fallback = doc["aliases"], doc["region_fallback"]
    countries = dict(doc["countries"])
    countries.update({iso: [list(x) for x in comp] for iso, comp in overrides.items()
                      if not iso.startswith("$")})

    by_iso = {}
    for c in doc["concentrations"]:
        by_iso.setdefault(c["iso"], []).append(c)

    result, colors = {}, {}
    for pid, power in owner.items():
        modern = scen["_modern_of"].get(pid, "")
        groups = countries.get(modern)
        if groups is None:
            groups = fallback.get(iso_region.get(modern), [["European", 100]])
        composition = gm.clean_composition(groups, aliases)
        if not composition:
            continue
        pinned = {}
        center = centers.get(pid)
        if center:
            lon, lat = center
            for box in by_iso.get(modern, ()):
                if gm.in_box(lon, lat, box["bbox"]):
                    name = aliases.get(box["group"], box["group"])
                    # A concentration for a group the era does not have -- post-war
                    # immigrant communities in a 1914 scenario -- must not be pinned.
                    if any(n == name for n, _ in composition):
                        pinned[name] = max(pinned.get(name, 0.0), float(box["share"]))
        entries = gm.compose(pid, composition, pinned)
        if not entries:
            continue
        result[str(pid)] = entries
        for e in entries:
            colors.setdefault(e["n"], gm.color_from_name(e["n"]))
    print(f"  minorities regenerated for the era: {len(result)} provinces, "
          f"{len(colors)} groups, {len([k for k in overrides if not k.startswith('$')])} "
          f"country composition(s) overridden")
    return result, colors


# ── raster ──────────────────────────────────────────────────────────
def border_distance(cid_arr, cap=60):
    """The game's border distance field, in numpy.

    Game_Loading.cpp seeds every pixel that has a 4-neighbour of a different
    country at 0 and does a multi-source BFS outward with orthogonal steps
    costing 2 and diagonal steps 3, capped at 60. That is a chamfer 2-3 metric,
    and relaxing the whole array against its eight neighbours until nothing
    changes computes the same field -- at most cap//2 rounds, because every
    round advances the frontier by at least one orthogonal step.
    """
    import numpy as np
    d = np.full(cid_arr.shape, cap, dtype=np.int16)
    border = np.zeros(cid_arr.shape, dtype=bool)
    border[:-1, :] |= cid_arr[:-1, :] != cid_arr[1:, :]
    border[1:, :] |= cid_arr[:-1, :] != cid_arr[1:, :]
    border[:, :-1] |= cid_arr[:, :-1] != cid_arr[:, 1:]
    border[:, 1:] |= cid_arr[:, :-1] != cid_arr[:, 1:]
    d[border] = 0

    for _ in range(cap // 2):
        prev = d
        n = d.copy()
        # orthogonal, +2
        np.minimum(n[1:, :], d[:-1, :] + 2, out=n[1:, :])
        np.minimum(n[:-1, :], d[1:, :] + 2, out=n[:-1, :])
        np.minimum(n[:, 1:], d[:, :-1] + 2, out=n[:, 1:])
        np.minimum(n[:, :-1], d[:, 1:] + 2, out=n[:, :-1])
        # diagonal, +3
        np.minimum(n[1:, 1:], d[:-1, :-1] + 3, out=n[1:, 1:])
        np.minimum(n[1:, :-1], d[:-1, 1:] + 3, out=n[1:, :-1])
        np.minimum(n[:-1, 1:], d[1:, :-1] + 3, out=n[:-1, 1:])
        np.minimum(n[:-1, :-1], d[1:, 1:] + 3, out=n[:-1, :-1])
        np.minimum(n, cap, out=n)
        d = n
        if np.array_equal(d, prev):
            break
    return d


def build_thumbnail(base, owner, countries, cid_of, width=512):
    """The browser thumbnail, with its gradient computed at thumbnail scale.

    Downscaling the full-resolution political.png does not work. The gradient
    reaches 60 map pixels inward, so at 1/16 scale it is under four thumbnail
    pixels wide and every land pixel averages to its darkest value -- the whole
    world comes out near black, which is not what the game looks like at any
    zoom a player uses.

    So the cid array is downsampled FIRST and the same distance field is run on
    it with the cap scaled to match. That is the view the game gives when you
    zoom out to the whole world, which is what a thumbnail is supposed to be.
    """
    import numpy as np
    from PIL import Image
    import io as _io
    img = Image.open(_io.BytesIO(base["_zip"].read("provinces.png"))).convert("RGB")
    step = max(1, img.width // width)
    small = np.array(img, dtype=np.uint8)[::step, ::step]
    ids = (small[:, :, 0].astype(np.uint32) << 16 |
           small[:, :, 1].astype(np.uint32) << 8 |
           small[:, :, 2].astype(np.uint32))

    cid_lut = np.zeros(int(ids.max()) + 1, dtype=np.int32)
    for pid, iso in owner.items():
        if pid <= ids.max():
            cid_lut[pid] = cid_of.get(iso, UNC_CID)
    cid_arr = cid_lut[ids]

    cap = max(6, (60 // step) | 1)
    t = np.minimum(1.0, border_distance(cid_arr, cap=cap) / float(cap))

    color_lut = np.zeros((max(int(cid_arr.max()), max(int(k) for k in countries)) + 1, 3),
                         dtype=np.float32)
    for k, v in countries.items():
        color_lut[int(k)] = hex_to_rgb(v["color"])
    out = color_lut[cid_arr] * (1.0 - t[..., None] * 0.4) + 40.0 * t[..., None] * 0.3
    inv = 1.0 - t
    sea = np.stack([8 + inv * 16, 10 + inv * 22, 15 + inv * 38], axis=-1)
    is_sea = cid_arr <= 0
    out[is_sea] = sea[is_sea]
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), "RGB")


def build_political_png(base, owner, countries, cid_of):
    """Ownership recolour with the border gradient the game draws at load.

    A flat recolour is what the game *reads* -- it rebuilds the political layer
    from provinces.png and countries.json regardless. But political.png is what
    the map browser and the map editor show, and the base map's has the gradient
    baked in (9,630 distinct colours against a flat map's 52). A preview that
    does not look like the game is a preview that misleads, and at thumbnail
    size the gradient is most of what distinguishes one era from another.

    So this reproduces generatePoliticalTexture() exactly: the same distance
    field, the same blend toward (40,40,40), the same darkened sea, and the same
    1px border pass at a third brightness.
    """
    import numpy as np
    from PIL import Image
    import io as _io
    img = Image.open(_io.BytesIO(base["_zip"].read("provinces.png"))).convert("RGB")
    arr = np.array(img, dtype=np.uint8)
    h, w, _ = arr.shape
    ids = (arr[:, :, 0].astype(np.uint32) << 16 |
           arr[:, :, 1].astype(np.uint32) << 8 |
           arr[:, :, 2].astype(np.uint32))

    # cid per pixel, 0 for sea -- the same array the game builds.
    cid_lut = np.zeros(int(ids.max()) + 1, dtype=np.int32)
    for pid, iso in owner.items():
        if pid <= ids.max():
            cid_lut[pid] = cid_of.get(iso, UNC_CID)
    cid_arr = cid_lut[ids]

    dist = border_distance(cid_arr)
    t = np.minimum(1.0, dist / 60.0)

    color_lut = np.zeros((max(int(cid_arr.max()), max(int(k) for k in countries)) + 1, 3),
                         dtype=np.float32)
    for k, v in countries.items():
        color_lut[int(k)] = hex_to_rgb(v["color"])
    base_rgb = color_lut[cid_arr]

    # blendColor(): base * (1 - t*0.4) + 40 * t * 0.3
    out = base_rgb * (1.0 - t[..., None] * 0.4) + 40.0 * t[..., None] * 0.3

    # Sea, darkening away from the coast, exactly as the game does it.
    inv = (1.0 - t)
    sea_rgb = np.stack([8 + (inv * 16).astype(np.uint8),
                        10 + (inv * 22).astype(np.uint8),
                        15 + (inv * 38).astype(np.uint8)], axis=-1).astype(np.float32)
    is_sea = cid_arr <= 0
    out[is_sea] = sea_rgb[is_sea]

    out = np.clip(out, 0, 255).astype(np.uint8)

    # 1px dark border at country boundaries, land only.
    edge = np.zeros(cid_arr.shape, dtype=bool)
    edge[:-1, :] |= cid_arr[:-1, :] != cid_arr[1:, :]
    edge[1:, :] |= cid_arr[:-1, :] != cid_arr[1:, :]
    edge[:, :-1] |= cid_arr[:, :-1] != cid_arr[:, 1:]
    edge[:, 1:] |= cid_arr[:, :-1] != cid_arr[:, 1:]
    edge &= ~is_sea
    out[edge] = out[edge] // 3

    return Image.fromarray(out, "RGB")


# ── packaging ───────────────────────────────────────────────────────
def province_id_array(base, remap=None):
    """Province id per pixel, after the scenario's re-cut if there was one."""
    import numpy as np
    from PIL import Image
    import io as _io
    img = Image.open(_io.BytesIO(base["_zip"].read("provinces.png"))).convert("RGB")
    a = np.array(img, dtype=np.uint32)
    ids = a[:, :, 0] << 16 | a[:, :, 1] << 8 | a[:, :, 2]
    if not remap:
        return ids
    lut = np.zeros(int(ids.max()) + 1, dtype=np.uint32)
    for pid, npid in remap.items():
        if pid <= ids.max():
            lut[pid] = npid
    return lut[ids]


def encode_provinces_png(ids):
    import numpy as np
    from PIL import Image
    import io as _io
    out = np.stack([(ids >> 16) & 0xFF, (ids >> 8) & 0xFF, ids & 0xFF],
                   axis=-1).astype(np.uint8)
    buf = _io.BytesIO()
    Image.fromarray(out, "RGB").save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def write_odmap(path, base, scen, files, political_img, thumb, provinces_png=None):
    import io as _io
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        info = zipfile.ZipInfo("scripts/")
        info.external_attr = 0o40755 << 16
        zf.writestr(info, "")

        for name in COPY_VERBATIM:
            if name == "provinces.png" and provinces_png is not None:
                zf.writestr(name, provinces_png)     # re-cut for this scenario
                continue
            zf.writestr(name, base["_zip"].read(name))
        for name in COPY_WITH_CAVEAT:
            if name in files:
                continue          # the scenario generated its own
            if name in base["_names"]:
                zf.writestr(name, base["_zip"].read(name))

        for name, payload in files.items():
            zf.writestr(name, json.dumps(payload, separators=(",", ":")))

        buf = _io.BytesIO()
        political_img.save(buf, format="PNG", optimize=True)
        zf.writestr("political.png", buf.getvalue())

        tbuf = _io.BytesIO()
        thumb.save(tbuf, format="PNG", optimize=True)
        zf.writestr("thumb.png", tbuf.getvalue())

        for name in sorted(base["_names"]):
            if name.startswith("symbols/"):
                zf.writestr(name, base["_zip"].read(name))

        # Licences from data/licenses/ rather than from the base archive: a
        # scenario ships its own flag set, and FLAGS.md is the attribution for
        # it. An archive that carries the artwork carries the terms.
        licenses_dir = os.path.join(DATA_DIR, "licenses")
        if os.path.isdir(licenses_dir):
            for f in sorted(os.listdir(licenses_dir)):
                fp = os.path.join(licenses_dir, f)
                if os.path.isfile(fp):
                    with open(fp, "rb") as fh:
                        zf.writestr("licenses/" + f, fh.read())
        else:
            for name in sorted(base["_names"]):
                if name.startswith("licenses/"):
                    zf.writestr(name, base["_zip"].read(name))

        # Flags: whatever countries.json points at. Some come from the base
        # archive (a power reusing an unchanged modern flag), some from
        # data/flags/ (a historical flag this scenario downloaded).
        wanted = set()
        for entry in files["countries.json"].values():
            for key in ("flag_actual", "flag_censored"):
                img = (entry.get(key) or {}).get("image")
                if img:
                    wanted.add(img)
        packed = 0
        for name in sorted(wanted):
            if name in base["_names"]:
                zf.writestr(name, base["_zip"].read(name))
                packed += 1
                continue
            local = os.path.join(DATA_DIR, name)
            if os.path.exists(local):
                with open(local, "rb") as f:
                    zf.writestr(name, f.read())
                packed += 1
        print(f"  packed {packed}/{len(wanted)} flag image(s)")
    return thumb


def update_index(scen, filename):
    path = os.path.join(STDMAPS, "maps_index.json")
    try:
        with open(path) as f:
            index = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        index = []
    index = [e for e in index if e.get("filename") != filename]
    index.append({
        "name": scen["name"],
        "filename": filename,
        "author": scen.get("author", "OpenDoctrines"),
        "description": scen["description"],
        "thumbnail": scen["id"] + "_thumb.png",
        "hasScripts": False,
    })
    order = {"map.odmap": ""}
    index.sort(key=lambda e: order.get(e["filename"], e["filename"]))
    with open(path, "w") as f:
        json.dump(index, f, indent=2)
        f.write("\n")


def generate(scenario_id, base_path):
    scen_path = os.path.join(SCENARIO_DIR, scenario_id + ".json")
    if not os.path.exists(scen_path):
        die(f"no scenario file: {scen_path}")
    with open(scen_path, encoding="utf-8") as f:
        scen = json.load(f)
    if scen.get("schema") != 1:
        die(f"{scen_path}: unsupported schema {scen.get('schema')!r}")
    validate_date(scen["date"])

    print(f"=== {scen['id']}: {scen['name']} ({scen['date']}) ===")
    base = load_base(base_path)
    validate_policies(scen, base)
    expand_independents(base, scen)
    power_by_iso = {p["iso"]: p for p in scen["powers"]}

    validate_claims(scen)
    owner = resolve_ownership(base, scen)
    countries, cid_of, flag_files = build_countries(base, scen, owner)
    held = sum(1 for o in owner.values() if o != "UNC")
    print(f"  {len(countries) - 2} powers, {held}/{len(owner)} provinces held")

    centers = province_centers(base)
    population = scale_population(base, scen, owner, power_by_iso)
    resources = scale_resources(base, scen)

    # Re-cut the province layer along this scenario's borders, then move every
    # per-province figure onto the new layer. Everything derived AFTER this
    # point -- armies, the compass, claims, minorities -- is computed on the new
    # provinces and their new centres, not the base map's.
    base_owner = dict(owner)      # political.png/thumb read the base raster
    remap = rebuild_geometry(base, scen, owner, centers)
    provinces = json.loads(json.dumps(base["provinces.json"]))
    for pid_str, prov in provinces.items():
        iso = owner[int(pid_str)]
        prov["iso_a3"] = iso
        prov["country_id"] = cid_of.get(iso, UNC_CID)

    files = {"provinces.json": provinces,
             "population.json": population,
             "resources.json": resources,
             "ports.json": base["ports.json"]}
    if remap:
        provinces = apply_geometry(base, remap, files, owner, cid_of)
        population = files["population.json"]
        resources = files["resources.json"]
        # Area-weighted centres and ownership for the merged provinces.
        areas = province_areas(base)
        acc = {}
        for pid, npid in remap.items():
            if pid not in centers:
                continue
            w = areas.get(pid, 1)
            lon, lat = centers[pid]
            a = acc.setdefault(npid, [0.0, 0.0, 0])
            a[0] += lon * w; a[1] += lat * w; a[2] += w
        centers = {npid: (a[0] / a[2], a[1] / a[2]) for npid, a in acc.items() if a[2]}
        owner = {int(k): v["iso_a3"] for k, v in provinces.items()}
        base_of_new = {}
        for pid, npid in remap.items():
            base_of_new.setdefault(npid, pid)
        scen["_modern_of_new"] = {npid: base["provinces.json"][str(pid)].get("iso_a3", "")
                                  for npid, pid in base_of_new.items()}

    province_ids = province_id_array(base, remap)
    compute_treasuries(countries, owner, resources, cid_of)
    armies = build_armies(scen, owner, population, cid_of)
    ships = build_ships(scen, owner, base, cid_of, centers, files["ports.json"],
                        province_ids)
    country_compass, province_compass = build_compass(scen, owner)

    files.update({
        "provinces.json": provinces,
        "countries.json": countries,
        "population.json": population,
        "resources.json": resources,
        "armies.json": armies,
        "ships.json": ships,
        "relations.json": build_relations(scen),
        "claims.json": build_claims(scen, owner, centers, province_ids),
        "country_compass.json": country_compass,
        "political_compass.json": province_compass,
        "starting_policies.json": {
            "starting_policies": {p["iso"]: p.get("policies", [])
                                  for p in scen["powers"] if p["iso"] in cid_of}
        },
        "starting_minority_policies.json": {},
        "metadata.json": {
            "name": scen["name"],
            "description": scen["description"],
            "author": scen.get("author", "Pr1nted"),
            "map_date": scen["date"],
            "license": "CC-BY-4.0",
            "has_scripts": False,
        },
    })

    scen["_modern_of"] = scen.get("_modern_of_new") or {
        int(pid): p.get("iso_a3", "") for pid, p in base["provinces.json"].items()}
    minorities, minority_colors = build_minorities(scen, owner, centers)
    if minorities:
        files["minorities.json"] = minorities
        files["minority_colors.json"] = minority_colors

    political = build_political_png(base, base_owner, countries, cid_of)
    thumb = build_thumbnail(base, base_owner, countries, cid_of)
    out_name = scen["id"] + ".odmap"
    out_path = os.path.join(STDMAPS, out_name)
    write_odmap(out_path, base, scen, files, political, thumb,
                encode_provinces_png(province_ids) if remap else None)
    thumb.save(os.path.join(STDMAPS, scen["id"] + "_thumb.png"), optimize=True)
    update_index(scen, out_name)

    print(f"  armies in {len(armies)} provinces, {len(ships)} ships")
    print(f"  wrote {os.path.relpath(out_path, PROJECT_ROOT)} "
          f"({os.path.getsize(out_path) / 1e6:.1f} MB)")
    return out_path


def rethumb(odmap_path, thumb_name):
    """Rebuild one .odmap's thumbnail in place, using its own ownership.

    The Modern Day map's stored preview came from MapGenerator, which has its
    own hue table and a flat sea -- France is orange there and blue in the game.
    Next to five scenarios rendered the way the game actually draws, it reads as
    a different product. This regenerates it from the archive's own
    provinces.png and countries.json through build_thumbnail(), so every entry
    in the browser is the same picture of the same thing.
    """
    import zipfile as _zip
    base = load_base(odmap_path)
    countries = base["countries.json"]
    owner, cid_of = {}, {}
    for entry in countries.values():
        cid_of[entry["iso_a3"]] = int(entry["id"])
    for pid_str, prov in base["provinces.json"].items():
        owner[int(pid_str)] = prov.get("iso_a3", "UNC")

    thumb = build_thumbnail(base, owner, countries, cid_of)

    import io as _io
    buf = _io.BytesIO()
    thumb.save(buf, format="PNG", optimize=True)
    payload = buf.getvalue()

    # Rewrite the archive with the new thumb.png; zipfile cannot replace in place.
    entries = [(n, base["_zip"].read(n)) for n in base["_zip"].namelist()]
    base["_zip"].close()
    with _zip.ZipFile(odmap_path, "w", _zip.ZIP_DEFLATED) as zf:
        for name, data in entries:
            zf.writestr(name, payload if name == "thumb.png" else data)
        if not any(n == "thumb.png" for n, _ in entries):
            zf.writestr("thumb.png", payload)
    thumb.save(os.path.join(STDMAPS, thumb_name), optimize=True)
    print(f"  rebuilt thumbnail for {os.path.basename(odmap_path)} "
          f"-> {thumb_name} ({thumb.size[0]}x{thumb.size[1]})")


def main(argv):
    base_path = DEFAULT_BASE
    if "--base" in argv:
        base_path = argv[argv.index("--base") + 1]
        argv = [a for i, a in enumerate(argv)
                if a != "--base" and argv[i - 1] != "--base"]

    if "--rethumb" in argv:
        target = argv[argv.index("--rethumb") + 1]
        path = target if target.endswith(".odmap") else os.path.join(STDMAPS, target + ".odmap")
        stem = os.path.basename(path)[:-len(".odmap")]
        rethumb(path, ("map_thumb.png" if stem == "map" else stem + "_thumb.png"))
        return 0

    ids = [a for a in argv if not a.startswith("-")]
    if "--all" in argv:
        ids = sorted(f[:-5] for f in os.listdir(SCENARIO_DIR) if f.endswith(".json"))
    if not ids:
        print(__doc__.split("USAGE")[1].split("SCENARIO FORMAT")[0].strip())
        available = sorted(f[:-5] for f in os.listdir(SCENARIO_DIR)) if os.path.isdir(SCENARIO_DIR) else []
        print("\nScenarios: " + (", ".join(available) or "none yet"))
        return 1

    for scenario_id in ids:
        generate(scenario_id, base_path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
