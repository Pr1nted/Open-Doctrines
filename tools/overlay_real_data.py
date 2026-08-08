#!/usr/bin/env python3
"""
Overlay real-world geographic datasets onto the province map.

Integrates:
  - USGS Major Mineral Deposits: gold, gemstone & metal deposits → resources.json
  - tools/data/oil_fields.json: project-authored oil regions → resources.json (oil)
  - Country-level data for rubber
  - Province centres → province_centers.json, consumed by generate_minorities.py

Ethnic composition is NOT generated here any more; see generate_minorities.py
and NOTICE.md for why GREG and ACOR were dropped.

Usage:
  pip install shapely pyshp pillow numpy
  python tools/overlay_real_data.py
"""

import json, os, sys, math, hashlib, random
from collections import defaultdict
import numpy as np
from PIL import Image
import shapefile

# ─── Paths ──────────────────────────────────────────────────────────
DATA_DIR = "data"
PROVINCES_PNG = os.path.join(DATA_DIR, "provinces.png")
PROVINCES_JSON = os.path.join(DATA_DIR, "provinces.json")
COUNTRIES_JSON = os.path.join(DATA_DIR, "countries.json")
OUT_RESOURCES = os.path.join(DATA_DIR, "resources.json")
OUT_ARMIES = os.path.join(DATA_DIR, "armies.json")
OUT_PORTS = os.path.join(DATA_DIR, "ports.json")
OUT_SHIPS = os.path.join(DATA_DIR, "ships.json")

OUT_PROVINCE_CENTERS = os.path.join(DATA_DIR, "province_centers.json")

# The only external dataset still read here. US Geological Survey publications
# are works of the US Government and carry no copyright. See NOTICE.md.
USGS_SHP = "/tmp/usgs_data/ofr20051294.shp"

# Project-authored, checked in, no download step.
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
OIL_FIELDS_JSON = os.path.join(TOOLS_DIR, "data", "oil_fields.json")

MAP_W = 8192
MAP_H = 4096

random.seed(42)

# ─── Coordinate helpers ─────────────────────────────────────────────
def pixel_to_lonlat(px, py):
    return (px / MAP_W) * 360.0 - 180.0, 90.0 - (py / MAP_H) * 180.0

def lonlat_to_pixel(lon, lat):
    return int((lon + 180.0) / 360.0 * MAP_W), int((90.0 - lat) / 180.0 * MAP_H)

# ─── Province data loading ──────────────────────────────────────────
def load_provinces():
    with open(PROVINCES_JSON) as f:
        pdata = json.load(f)
    with open(COUNTRIES_JSON) as f:
        cdata = json.load(f)
    # Map country id -> iso
    cid_to_iso = {}
    for cid_str, ci in cdata.items():
        cid_to_iso[int(cid_str)] = ci["iso_a3"]
    # Build province info
    provinces = {}
    for pid_str, pi in pdata.items():
        pid = int(pid_str)
        provinces[pid] = {
            "id": pid,
            "name": pi["name"],
            "country_id": pi["country_id"],
            "iso_a3": pi["iso_a3"],
        }
    return provinces, cid_to_iso

def compute_province_centers_numpy(provinces):
    """Compute mean pixel center for each province using numpy."""
    img = Image.open(PROVINCES_PNG).convert("RGB")
    arr = np.array(img, dtype=np.uint8)
    h, w, _ = arr.shape
    # Encode RGB -> province ID
    ids = arr[:,:,0].astype(np.uint32) << 16 | arr[:,:,1].astype(np.uint32) << 8 | arr[:,:,2].astype(np.uint32)
    mask = ids != 0
    # Flatten
    flat_ids = ids.ravel()
    flat_mask = mask.ravel()
    # Create coordinate grids
    y_grid, x_grid = np.mgrid[0:h, 0:w]
    flat_x = x_grid.ravel().astype(np.float64)
    flat_y = y_grid.ravel().astype(np.float64)
    # Filter valid pixels
    valid_ids = flat_ids[flat_mask]
    valid_x = flat_x[flat_mask]
    valid_y = flat_y[flat_mask]
    # Aggregate by province ID
    unique_ids = np.unique(valid_ids)
    result = {}
    for pid in unique_ids:
        mask_p = valid_ids == pid
        cx = float(valid_x[mask_p].mean())
        cy = float(valid_y[mask_p].mean())
        lon, lat = pixel_to_lonlat(cx, cy)
        area = int(mask_p.sum())
        result[int(pid)] = {"x": cx, "y": cy, "lon": lon, "lat": lat, "area": area}
    # Filter to only provinces that exist in the JSON
    result = {pid: c for pid, c in result.items() if pid in provinces}
    # Fill in for any missing provinces
    for pid in provinces:
        if pid not in result:
            # Try to find any pixel for this province
            pid_u32 = np.uint32(pid)
            mask_p = flat_ids == pid_u32
            if mask_p.any():
                cx = float(flat_x[mask_p].mean())
                cy = float(flat_y[mask_p].mean())
                area = int(mask_p.sum())
                lon, lat = pixel_to_lonlat(cx, cy)
                result[pid] = {"x": cx, "y": cy, "lon": lon, "lat": lat, "area": area}
    return result

def compute_province_centers_fast(provinces):
    """Fallback: sample every Nth pixel for speed."""
    img = Image.open(PROVINCES_PNG).convert("RGB")
    w, h = img.size
    pixels = img.load()
    centers = {}
    step = 4
    for y in range(0, h, step):
        for x in range(0, w, step):
            r, g, b = pixels[x, y]
            if r == 0 and g == 0 and b == 0:
                continue
            pid = (r << 16) | (g << 8) | b
            if pid not in centers:
                centers[pid] = {"count": 0, "sum_x": 0.0, "sum_y": 0.0}
            centers[pid]["count"] += 1
            centers[pid]["sum_x"] += x
            centers[pid]["sum_y"] += y
    result = {}
    for pid, c in centers.items():
        pid = int(pid)
        cx = c["sum_x"] / c["count"]
        cy = c["sum_y"] / c["count"]
        lon, lat = pixel_to_lonlat(cx, cy)
        area = c["count"]
        result[pid] = {"x": cx, "y": cy, "lon": lon, "lat": lat, "area": area}
    return result

# ─── Province centres ───────────────────────────────────────────────
# Ethnic composition was derived here from GREG (ETH Zurich) polygons until
# that dataset turned out to carry no licence at all -- its download page asks
# only to be cited "when using the GREG data for your research", and GREG is
# itself a digitisation of the 1964 Soviet Atlas Narodov Mira, so the people
# distributing it could not have sublicensed it either. See NOTICE.md.
#
# minorities.json is now produced by tools/generate_minorities.py from
# tools/data/ethnic_groups.json, which the project authors. The one thing that
# step needs from this file is the province centres, which are computed above
# anyway -- so they are written out here rather than recomputed from an 8192 x
# 4096 PNG a second time.
def write_province_centers(centers):
    out = {
        str(pid): {"lon": round(c["lon"], 4), "lat": round(c["lat"], 4)}
        for pid, c in centers.items()
    }
    with open(OUT_PROVINCE_CENTERS, "w") as f:
        json.dump(out, f, separators=(",", ":"))
    print(f"  Wrote {OUT_PROVINCE_CENTERS} ({len(out)} provinces)")

# ─── USGS: Mineral deposits ─────────────────────────────────────────
def load_usgs_gold():
    """Load gold and gemstone deposits from USGS Major Mineral Deposits."""
    sf = shapefile.Reader(USGS_SHP)
    fields = [f[0] for f in sf.fields[1:]]
    gold_points = []
    gem_points = []
    for i in range(sf.numRecords):
        rec = sf.record(i)
        d = dict(zip(fields, rec))
        comm = (d.get("COMMODITY") or "").strip().lower()
        shape = sf.shape(i)
        if not shape.points:
            continue
        lon, lat = shape.points[0]
        # Check if deposit is gold-related
        is_gold = comm.startswith("gold") or ", gold" in comm or "-gold" in comm
        # Check if deposit is gem-related
        is_gem = any(kw in comm for kw in ["gem", "diamond", "ruby", "sapphire", "emerald"])
        if is_gold:
            gold_points.append((lon, lat, d.get("DEP_NAME", "")))
        if is_gem:
            gem_points.append((lon, lat, d.get("DEP_NAME", "")))
    print(f"  Loaded {len(gold_points)} gold deposits, {len(gem_points)} gem deposits")
    return gold_points, gem_points

def load_usgs_metal():
    """Load base/industrial metal deposits from USGS Major Mineral Deposits."""
    metal_keywords = [
        "copper", "iron", "aluminum", "lead", "zinc", "tin", "nickel",
        "manganese", "molybdenum", "tungsten", "chromium", "cobalt",
        "titanium", "magnesium", "mercury", "silver", "vanadium",
        "bismuth", "antimony", "cadmium", "indium", "germanium",
        "gallium", "hafnium", "niobium", "tantalum", "beryllium",
        "lithium", "strontium", "rare earth", "uranium", "thorium",
        "platinum", "pge", "palladium", "rhodium", "ruthenium",
        "osmium", "iridium", "rhenium", "selenium", "tellurium",
        "thallium", "zirconium", "arsenic",
    ]
    skip_keywords = ["gold", "diamond", "gem", "ruby", "sapphire", "emerald",
                     "halite", "phosphate", "gypsum", "limestone", "barite",
                     "fluorspar", "potash", "graphite", "asbestos", "sand",
                     "gravel", "stone", "clay"]
    sf = shapefile.Reader(USGS_SHP)
    fields = [f[0] for f in sf.fields[1:]]
    metal_points = []
    for i in range(sf.numRecords):
        rec = sf.record(i)
        d = dict(zip(fields, rec))
        comm = (d.get("COMMODITY") or "").strip().lower()
        shape = sf.shape(i)
        if not shape.points:
            continue
        lon, lat = shape.points[0]
        is_metal = any(kw in comm for kw in metal_keywords)
        is_skip = any(kw in comm for kw in skip_keywords)
        if is_metal and not is_skip:
            metal_points.append((lon, lat, d.get("DEP_NAME", "")))
    print(f"  Loaded {len(metal_points)} metal deposits")
    return metal_points

# ─── Oil fields ─────────────────────────────────────────────────────
def load_oil_fields():
    """Oil and gas field locations from tools/data/oil_fields.json.

    Replaces ACOR (ETH Zurich), which shipped no licence and is itself a
    digitisation of a 1982 atlas -- see NOTICE.md. The replacement is a
    project-authored list of named producing regions rather than a copy of
    anyone's polygons.

    Each region carries a `fields` weight, expanded here into that many points
    scattered in a small deterministic disc around the centre. That keeps the
    input shape scale_resource() already expects (a count per province) and
    stops a 20-weight region landing entirely on one pixel when the province
    boundary happens to run through it.

    Returns (lon, lat, isos), where `isos` is the declared country followed by
    any `iso_fallback` entries. Carrying the country is what makes offshore
    fields work at all: their coordinates are at sea, so it cannot be recovered
    from the map. The fallbacks cover countries a given map does not model --
    a 2000-start map has no South Sudan. The `offshore` flag in the JSON is
    documentation for the reader and a count for this log line; placement does
    not branch on it.
    """
    with open(OIL_FIELDS_JSON, encoding="utf-8") as f:
        doc = json.load(f)
    if doc.get("schema") != 1:
        raise SystemExit(f"{OIL_FIELDS_JSON}: unsupported schema {doc.get('schema')!r}")

    rng = random.Random(20260727)   # fixed: the map must not churn between runs
    points = []
    offshore = 0
    for entry in doc["fields"]:
        lon, lat = float(entry["lon"]), float(entry["lat"])
        count = int(entry["fields"])
        if entry.get("offshore"):
            offshore += 1
        # A tuple of candidate countries, most specific first. Everything
        # downstream takes the first one the map actually models.
        isos = (entry["iso"],) + tuple(entry.get("iso_fallback", ()))
        points.append((lon, lat, isos))
        # Remaining points spread over roughly a 60 km disc.
        for _ in range(count - 1):
            points.append((
                lon + rng.uniform(-0.35, 0.35),
                lat + rng.uniform(-0.35, 0.35),
                isos,
            ))
    print(f"  Loaded {len(doc['fields'])} oil regions "
          f"({offshore} offshore) -> {len(points)} field points")
    return points

# ─── Resource assignment ────────────────────────────────────────────
def scale_resource(count):
    """Diminishing-returns scaling: 1→20, 2→33, 3→43, 5→58, 10→75, 20→90."""
    if count <= 0:
        return 0.0
    return round(100.0 * (1.0 - math.exp(-count / 5.0)), 1)

def assign_resources(provinces, province_centers, gold_points, gem_points, oil_centroids, metal_points=None, population=None, border_forts=None):
    """Assign resources to provinces based on deposit locations."""
    # Pre-load province pixel lookup for USGS point deposits
    img = Image.open(PROVINCES_PNG).convert("RGB")
    pixel_arr = np.array(img, dtype=np.uint8)
    del img

    resources = {}

    def pixel_to_pid_safe(px, py):
        if 0 <= px < MAP_W and 0 <= py < MAP_H:
            r = int(pixel_arr[py, px, 0])
            g = int(pixel_arr[py, px, 1])
            b = int(pixel_arr[py, px, 2])
            if r == 0 and g == 0 and b == 0:
                return 0
            return (r << 16) | (g << 8) | b
        return 0

    # Process gold deposits
    gold_counts = defaultdict(int)
    gold_provinces = set()
    for lon, lat, name in gold_points:
        px, py = lonlat_to_pixel(lon, lat)
        # Try 3×3 neighborhood to avoid single-pixel border issues
        found = False
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                pid = pixel_to_pid_safe(px + dx, py + dy)
                if pid > 0 and pid in provinces:
                    gold_counts[pid] += 1
                    gold_provinces.add(pid)
                    found = True
                    break
            if found:
                break
        if not found:
            pid = pixel_to_pid_safe(px, py)
            if pid > 0 and pid in provinces:
                gold_counts[pid] += 1
                gold_provinces.add(pid)
    print(f"  Gold deposits found in {len(gold_provinces)} provinces")

    # Process gem deposits
    gem_counts = defaultdict(int)
    gem_provinces = set()
    for lon, lat, name in gem_points:
        px, py = lonlat_to_pixel(lon, lat)
        found = False
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                pid = pixel_to_pid_safe(px + dx, py + dy)
                if pid > 0 and pid in provinces:
                    gem_counts[pid] += 1
                    gem_provinces.add(pid)
                    found = True
                    break
            if found:
                break
        if not found:
            pid = pixel_to_pid_safe(px, py)
            if pid > 0 and pid in provinces:
                gem_counts[pid] += 1
                gem_provinces.add(pid)
    print(f"  Gem deposits found in {len(gem_provinces)} provinces")

    # Process oil fields.
    #
    # A pixel lookup alone does not work here. Over half these regions are
    # offshore, so their coordinates are at sea where there is no province at
    # all, and an expanding pixel search finds whichever coast is nearest --
    # which put Danish fields in Germany, Egyptian ones in Cyprus, and lost
    # every North Sea field in the water between Britain and Norway.
    #
    # Each region declares the country it belongs to, so use it: try the exact
    # pixel first (precise inside large countries), and otherwise fall back to
    # the nearest province centre BELONGING TO THAT COUNTRY. Attribution is
    # then correct by construction, and the only thing approximated is which
    # of a country's own provinces gets the field.
    centers_by_iso = defaultdict(list)
    for pid, c in province_centers.items():
        iso = provinces.get(pid, {}).get("iso_a3", "")
        if iso:
            centers_by_iso[iso].append((pid, c["lon"], c["lat"]))

    def nearest_in_country(lon, lat, iso):
        best, best_d = 0, None
        for pid, clon, clat in centers_by_iso.get(iso, ()):
            # Planar distance with a cos(lat) correction. Exact great-circle
            # distance would change no answer at these separations.
            dx = (clon - lon) * math.cos(math.radians((clat + lat) * 0.5))
            dy = clat - lat
            d = dx * dx + dy * dy
            if best_d is None or d < best_d:
                best, best_d = pid, d
        return best

    oil_counts = defaultdict(int)
    oil_provinces = set()
    oil_unmodelled = defaultdict(int)
    for lon, lat, isos in oil_centroids:
        px, py = lonlat_to_pixel(lon, lat)
        pid = pixel_to_pid_safe(px, py)
        if not (pid > 0 and pid in provinces and
                provinces[pid].get("iso_a3", "") in isos):
            pid = 0
            for iso in isos:                 # declared country, then fallbacks
                pid = nearest_in_country(lon, lat, iso)
                if pid:
                    break
        if pid:
            oil_counts[pid] += 1
            oil_provinces.add(pid)
        else:
            oil_unmodelled[isos[0]] += 1
    print(f"  Oil fields found in {len(oil_provinces)} provinces")
    if oil_unmodelled:
        # Not a warning: a map is allowed not to model a country. Said out loud
        # anyway, because otherwise the oil just quietly is not there and the
        # first sign is someone asking why a producer has none. Give a country
        # here an iso_fallback if the oil should land somewhere instead.
        print("  Not placed — this map models neither the country nor a "
              "fallback: " + ", ".join(f"{iso} ({n} points)"
                                       for iso, n in sorted(oil_unmodelled.items())))

    # Process metal deposits
    metal_counts = defaultdict(int)
    metal_provinces = set()
    if metal_points:
        for lon, lat, name in metal_points:
            px, py = lonlat_to_pixel(lon, lat)
            found = False
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    pid = pixel_to_pid_safe(px + dx, py + dy)
                    if pid > 0 and pid in provinces:
                        metal_counts[pid] += 1
                        metal_provinces.add(pid)
                        found = True
                        break
                if found:
                    break
            if not found:
                pid = pixel_to_pid_safe(px, py)
                if pid > 0 and pid in provinces:
                    metal_counts[pid] += 1
                    metal_provinces.add(pid)
    print(f"  Metal deposits found in {len(metal_provinces)} provinces")

    # Build per-province resource data
    for pid in provinces:
        pid_int = int(pid)
        g = gold_counts.get(pid_int, 0)
        gem = gem_counts.get(pid_int, 0)
        o = oil_counts.get(pid_int, 0)
        m = metal_counts.get(pid_int, 0)

        # Convert counts to amounts using diminishing-returns scaling
        gold_amt = scale_resource(g)
        gem_amt = scale_resource(gem)
        oil_amt = scale_resource(o)
        metal_amt = scale_resource(m)

        # Industry boost scales with amount
        gold_boost = round(gold_amt * 0.3, 1)
        gem_boost = round(gem_amt * 0.25, 1)
        oil_boost = round(oil_amt * 0.4, 1)
        metal_boost = round(metal_amt * 0.35, 1)

        resources[str(pid)] = {
            "oil": {"a": round(oil_amt, 1), "b": oil_boost},
            "gold": {"a": round(gold_amt, 1), "b": gold_boost},
            "rubber": {"a": 0.0, "b": 0.0},
            "gemstones": {"a": round(gem_amt, 1), "b": gem_boost},
            "metal": {"a": round(metal_amt, 1), "b": metal_boost},
        }

    # Apply latitude-based rubber distribution (rubber is an agricultural resource,
    # so it follows climate zones, not political borders). Use province latitude
    # as a proxy for tropical climate suitability, then scale by country-level
    # production to keep relative proportions realistic. To avoid sharp border
    # cutoffs, each province also inherits the rubber base of neighboring countries.
    print("  Assigning rubber based on latitude with cross-border smoothing...")
    # Country-level rubber production base (per-province base for reference)
    rubber_base = {
        "THA": 80, "IDN": 75, "VNM": 65, "IND": 60, "CHN": 55,
        "MYS": 70, "PHL": 40, "LKA": 35, "NGA": 30, "CIV": 30,
        "BRA": 25, "MMR": 30, "KHM": 40, "LAO": 30, "PNG": 25,
        "GHA": 20, "CMR": 20, "COD": 20, "GAB": 20, "COG": 15,
        "TZA": 10, "UGA": 10, "RWA": 10, "BDI": 10, "ETH": 10,
        "MEX": 15, "GTM": 10, "COL": 10, "ECU": 15,
        "LBR": 25, "SLE": 20, "NPL": 20, "CAF": 20, "MDG": 15,
        "BGD": 15, "MOZ": 15, "GUY": 12, "SUR": 12, "TLS": 12,
        "BOL": 10, "CRI": 10, "VEN": 10, "BRN": 10, "SOM": 8,
        "GNQ": 8, "HTI": 5, "MWI": 5, "PAN": 5, "TGO": 5,
        "SLB": 5, "BFA": 5, "BTN": 12, "PAK": 8,
    }
    # Build country neighbor lists by checking which countries have provinces
    # within 120 pixels of each other
    print("    Building country neighborhood map for border smoothing...")
    country_neighbors = defaultdict(set)
    iso_pids = defaultdict(list)
    for pid in provinces:
        iso_pids[provinces[pid]["iso_a3"]].append(pid)
    all_isos = list(iso_pids.keys())
    for i in range(len(all_isos)):
        for j in range(i + 1, len(all_isos)):
            iso_a, iso_b = all_isos[i], all_isos[j]
            # Check proximity: sample first province of each country
            for pid_a in iso_pids[iso_a][:5]:
                ca = province_centers.get(pid_a, {})
                if not ca:
                    continue
                for pid_b in iso_pids[iso_b][:5]:
                    cb = province_centers.get(pid_b, {})
                    if not cb:
                        continue
                    dx = ca.get("x", 0) - cb.get("x", 0)
                    dy = ca.get("y", 0) - cb.get("y", 0)
                    dist = math.sqrt(dx*dx + dy*dy)
                    if dist < 120:
                        country_neighbors[iso_a].add(iso_b)
                        country_neighbors[iso_b].add(iso_a)
                        break
                if iso_b in country_neighbors.get(iso_a, set()):
                    break

    # Compute effective rubber base per country (own base + max of neighbors)
    effective_base = {}
    for iso in rubber_base:
        b = rubber_base.get(iso, 0)
        for n in country_neighbors.get(iso, set()):
            b = max(b, rubber_base.get(n, 0))
        effective_base[iso] = b
    # For countries not in rubber_base, check neighbors
    for iso in iso_pids:
        if iso not in effective_base:
            b = 0
            for n in country_neighbors.get(iso, set()):
                b = max(b, rubber_base.get(n, 0))
            effective_base[iso] = max(b, 5) if b > 0 else 0

    # Compute province-level latitude factor (tropical climate suitability)
    for pid in provinces:
        iso = provinces[pid]["iso_a3"]
        lat = province_centers.get(pid, {}).get("lat", 90)
        # Tropical band: 30°S to 30°N, peak at equator
        lat_factor = max(0.0, 1.0 - abs(lat) / 30.0)
        # Boost for very wet equatorial regions (5°S-5°N)
        if abs(lat) < 5:
            lat_factor = min(1.0, lat_factor * 1.3)
        country_base = effective_base.get(iso, 0)
        raw = lat_factor * country_base
        # Per-province noise
        noise = 0.7 + random.random() * 0.6  # ±30%
        amt = round(raw * noise, 1)
        boost = round(amt * 0.2, 1)
        resources[str(pid)]["rubber"] = {"a": amt, "b": boost}

    # ── Country-level resource corrections ──
    # The shapefile data counts raw deposit counts, not reserve sizes.
    # These multipliers correct per-country totals to match real-world proportions.
    # Countries not listed keep their raw shapefile-derived values.
    # OIL_FIX used to exist because ACOR's polygon centroids were wildly uneven
    # -- thousands of tiny US and North Sea entries, a handful for the Gulf --
    # so every major producer needed a multiplier to undo the source's shape.
    # tools/data/oil_fields.json is weighted directly for play balance instead,
    # so a correction on top would be a second, contradictory set of knobs.
    # Left in place, and empty, because the mechanism is still the right place
    # to put a per-country tweak if one is ever needed again.
    OIL_FIX = {}
    GOLD_FIX = {
        "USA": 0.15, "JPN": 0.02, "DEU": 0.05, "FRA": 0.02,
        "ITA": 0.05, "GBR": 0.1, "POL": 0.1, "ROU": 0.1,
        "CZE": 0.1, "UKR": 0.1, "ESP": 0.3, "PRT": 0.3,
        "GRC": 0.1, "BGR": 0.1, "AUT": 0.1, "CHE": 0.2,
        "RUS": 0.5, "CHN": 0.5, "IND": 0.5,
        "IDN": 0.5, "MYS": 0.5, "PHL": 0.5,
        "KAZ": 0.5, "UZB": 0.5, "MNG": 1.0,
        "AUS": 0.6, "NZL": 0.5, "PNG": 0.5,
        "ZAF": 1.0, "GHA": 0.5, "MLI": 0.5, "BFA": 0.5,
        "PER": 0.5, "CHL": 0.5, "ARG": 0.5, "BRA": 0.5,
        "COL": 0.5, "ECU": 0.5, "BOL": 0.5, "GUY": 0.5,
        "SUR": 0.5, "MEX": 0.5,
        "CAN": 0.5,
    }
    GEM_FIX = {
        "USA": 0.3, "JPN": 0.02, "RUS": 1.5,
        "CHN": 0.5, "IND": 0.5,
        "ZAF": 1.0, "BWA": 2.0, "NAM": 1.5,
        "AGO": 1.5, "COD": 1.0, "SLE": 1.0, "GIN": 1.0,
        "TZA": 1.5, "ZWE": 1.0, "ZMB": 1.0,
        "MMR": 1.0, "THA": 0.5, "LKA": 1.0,
        "AUS": 1.0, "CAN": 1.0,
    }
    METAL_FIX = {
        # Major mining countries — moderate reduction from raw point counts
        "CHN": 0.8, "RUS": 0.7, "AUS": 0.8, "BRA": 0.7, "CAN": 0.7,
        "USA": 0.3, "IND": 0.4, "ZAF": 0.7, "MEX": 0.5,
        # Major copper producers
        "CHL": 0.8, "PER": 0.7, "ZMB": 0.7, "COD": 0.6,
        # Major iron ore
        "UKR": 0.5, "KAZ": 0.6,
        # Significant but smaller producers
        "TUR": 0.3, "IRN": 0.3, "SWE": 0.3, "FIN": 0.3,
        "POL": 0.1, "ESP": 0.2, "PRT": 0.2, "GRC": 0.08,
        "BGR": 0.08, "ROU": 0.08, "SRB": 0.08, "HUN": 0.08,
        "CZE": 0.05, "SVK": 0.05, "AUT": 0.05,
        "BOL": 0.4, "PHL": 0.3, "IDN": 0.3, "PNG": 0.3,
        "MAR": 0.3, "DZA": 0.3, "EGY": 0.2, "TUN": 0.2,
        "SAU": 0.1, "OMN": 0.1, "YEM": 0.1, "MMR": 0.2,
        "MNG": 0.6, "KGZ": 0.3, "UZB": 0.3, "TJK": 0.3,
        "GIN": 0.4, "GHA": 0.2, "MDG": 0.2, "MOZ": 0.2,
        "AGO": 0.2, "NAM": 0.3, "BWA": 0.3, "ZWE": 0.4,
        "NGA": 0.15, "KEN": 0.1, "TZA": 0.1, "ETH": 0.1,
        "AFG": 0.2, "PAK": 0.15, "VNM": 0.2, "THA": 0.15,
        "MYS": 0.15, "LAO": 0.15, "KHM": 0.1,
        "ARG": 0.15, "COL": 0.1, "ECU": 0.1, "VEN": 0.08,
        "CUB": 0.15, "JAM": 0.3, "DOM": 0.1,
        "NCL": 0.5,  # New Caledonia — major nickel
        # Near-zero for countries with negligible mining
        "JPN": 0.08, "DEU": 0.35, "FRA": 0.25, "GBR": 0.25,
        "ITA": 0.12, "NLD": 0.08, "CHE": 0.0, "BEL": 0.0,
        "DNK": 0.0, "NOR": 0.2, "IRL": 0.15, "NZL": 0.08,
        "CYP": 0.08, "CZE": 0.15, "AUT": 0.15, "SVK": 0.12, "SVN": 0.08,
        "HRV": 0.08, "BIH": 0.15, "MNE": 0.1, "MKD": 0.08, "ALB": 0.08, "XKX": 0.08,
    }

    def apply_correction(rsrc_key, fix_map, province_iso_map):
        """Scale per-province resource amounts by country multiplier."""
        # Compute per-country totals
        country_raw = defaultdict(float)
        country_ids = defaultdict(list)
        for pid in provinces:
            iso = provinces[pid]["iso_a3"]
            val = resources.get(str(pid), {}).get(rsrc_key, {}).get("a", 0)
            if val > 0:
                country_raw[iso] += val
                country_ids[iso].append(pid)

        for iso, mul in fix_map.items():
            if iso not in country_raw or country_raw[iso] == 0:
                # Country has no data or doesn't exist in provinces, skip
                continue
            total = country_raw[iso]
            new_target = total * mul
            if new_target <= 0:
                for pid in country_ids[iso]:
                    resources[str(pid)][rsrc_key]["a"] = 0.0
                    resources[str(pid)][rsrc_key]["b"] = 0.0
                continue
            scale = new_target / total
            for pid in country_ids[iso]:
                old_a = resources[str(pid)][rsrc_key]["a"]
                new_a = round(old_a * scale, 1)
                resources[str(pid)][rsrc_key]["a"] = new_a
                resources[str(pid)][rsrc_key]["b"] = round(new_a * 0.4, 1) if rsrc_key == "oil" else (
                    round(new_a * 0.35, 1) if rsrc_key == "metal" else (
                        round(new_a * 0.3, 1) if rsrc_key == "gold" else round(new_a * 0.25, 1)
                    )
                )

    apply_correction("oil", OIL_FIX, provinces)
    apply_correction("gold", GOLD_FIX, provinces)
    apply_correction("gemstones", GEM_FIX, provinces)
    apply_correction("metal", METAL_FIX, provinces)

    # ── Spread resources across more provinces within each country ──
    # The ACOR/USGS shapefile data often deposits resources in just 1-2 provinces
    # per country. This step distributes the per-country total across more
    # provinces weighted by pixel area, so resource hotspots look natural.
    print("  Spreading resources across more provinces...")
    for rsrc_key in ["oil", "gold", "gemstones", "metal"]:
        # Build per-country lists
        country_pids = defaultdict(list)
        for pid in provinces:
            iso = provinces[pid]["iso_a3"]
            country_pids[iso].append(int(pid))

        for iso, pids in country_pids.items():
            if len(pids) < 3:
                continue
            # Provinces that currently have this resource
            seed_pids = [pid for pid in pids
                         if resources.get(str(pid), {}).get(rsrc_key, {}).get("a", 0) > 0]
            n_seed = len(seed_pids)
            if n_seed == 0:
                continue

            # Compute target number of provinces
            target_n = max(4, min(n_seed * 3, int(len(pids) * 0.35)))
            target_n = min(target_n, len(pids))

            if n_seed >= target_n:
                continue

            # Total resource amount for this country
            total = sum(resources.get(str(pid), {}).get(rsrc_key, {}).get("a", 0)
                        for pid in seed_pids)

            # Select additional provinces: largest area first
            non_seed = [pid for pid in pids if pid not in set(seed_pids)]
            non_seed.sort(key=lambda pid: province_centers.get(pid, {}).get("area", 0), reverse=True)
            add_pids = non_seed[:target_n - n_seed]
            all_pids = seed_pids + add_pids

            # Compute weights: seed provinces get area bonus, others get area penalty
            weights = {}
            for pid in all_pids:
                area = province_centers.get(pid, {}).get("area", 100)
                w = float(area) * (2.0 if pid in seed_pids else 0.5)
                weights[pid] = w
            total_w = sum(weights.values())

            # Redistribute
            for pid in all_pids:
                raw = total * weights[pid] / total_w
                amt = round(raw, 1)
                boost = round(amt * 0.4, 1) if rsrc_key == "oil" else (
                    round(amt * 0.35, 1) if rsrc_key == "metal" else (
                        round(amt * 0.3, 1) if rsrc_key == "gold" else round(amt * 0.25, 1)
                    )
                )
                resources[str(pid)][rsrc_key] = {"a": amt, "b": boost}

            # Zero out non-selected provinces
            for pid in pids:
                if pid not in all_pids:
                    resources[str(pid)][rsrc_key] = {"a": 0.0, "b": 0.0}

    return resources

    # ── Industry: real-world manufacturing base + resource contribution ──
    
    COUNTRY_INDUSTRY_BASE = {
        "CHN": 100, "USA": 80, "JPN": 300, "DEU": 65, "IND": 112, "KOR": 40,
        "GBR": 300, "FRA": 300, "ITA": 30, "BRA": 25, "RUS": 15, "CAN": 20,
        "MEX": 18, "IDN": 8, "ESP": 49, "TUR": 15, "THA": 14, "CHE": 13,
        "POL": 18, "NLD": 45, "SAU": 15, "AUS": 10, "MYS": 9, "SWE": 8,
        "BEL": 7, "AUT": 6, "CZE": 6, "IRL": 5, "SGP": 5, "LUX": 10, "FIN": 5,
        "NOR": 10, "DNK": 8, "PRT": 4, "GRC": 6, "NZL": 3, "ISR": 15,
        "UKR": 3, "ROU": 3, "HUN": 3, "EGY": 3, "ZAF": 3, "NGA": 3,
        "PAK": 3, "BGD": 3, "VNM": 4, "PHL": 3, "MMR": 1, "KAZ": 2,
        "ARG": 3, "CHL": 2, "COL": 2, "PER": 1, "VEN": 1, "KEN": 1,
        "ETH": 1, "TZA": 1, "COD": 1, "SDN": 0, "MAR": 2, "DZA": 2, "TCD": 0,
        "LBY": 1, "TUN": 1, "GHA": 1, "CIV": 1, "CMR": 1, "AGO": 1,
        "MOZ": 1, "ZWE": 1, "ZMB": 1, "UGA": 1, "IRN": 3, "IRQ": 1,
        "KWT": 1, "ARE": 2, "QAT": 1, "OMN": 31, "YEM": 0, "SYR": 0,
        "JOR": 0, "LBN": 30, "AFG": 0, "NPL": 0, "BTN": 0, "LKA": 1,
        "MMR": 1, "KHM": 0, "LAO": 30, "MNG": 0, "PRK": 32,
        "CUB": 32, "DOM": 0, "GTM": 0, "HND": 0, "SLV": 0, "NIC": 0,
        "CRI": 1, "PAN": 1, "BOL": 0, "PRY": 0, "URY": 0, "GUY": 0,
        "SUR": 0, "PNG": 0, "FJI": 0,
        "SRB": 1, "HRV": 1, "SVN": 1, "BIH": 1, "MKD": 0, "ALB": 0,
        "BGR": 4, "SVK": 1, "LTU": 1, "LVA": 2, "EST": 2, "BLR": 32,
        "GEO": 0, "ARM": 0, "AZE": 1, "TKM": 30, "UZB": 31, "KGZ": 0,
        "TJK": 0,
        "ATF": 28, "UNC": 0,
    }
    # Group provinces by country, compute per-country area total
    iso_prov_pids = defaultdict(list)
    for pid in provinces:
        iso_prov_pids[provinces[pid]["iso_a3"]].append(pid)
    country_areas = {}
    for iso, pids in iso_prov_pids.items():
        total_area = sum(province_centers.get(pid, {}).get("area", 0) for pid in pids)
        country_areas[iso] = max(total_area, 1)

    # ── Pass 1: compute raw incomes to find per-country max ──
    raw_industry = {}  # pid -> dict of intermediate values
    iso_raw_max = defaultdict(float)
    LEVEL6_ISOS = {"SGP"}

    for pid in provinces:
        iso = provinces[pid]["iso_a3"]
        country_base = COUNTRY_INDUSTRY_BASE.get(iso, 0.5)
        area = province_centers.get(pid, {}).get("area", 1)
        total_area = country_areas.get(iso, 1)
        area_share = area / total_area
        num_provs = len(iso_prov_pids.get(iso, [1]))

        # Load population for density calculation
        pop = population.get(str(pid), 0)
        pop_density = pop / max(area, 1)

        # Density penalty: remote sparsely populated regions get much lower industry
        # Density threshold: provinces below this get penalized heavily
        density_threshold = 0.5  # pixels per person (adjust based on data)
        if pop_density < density_threshold:
            density_factor = (pop_density / density_threshold) ** 0.5  # Square root penalty
        else:
            density_factor = 1.0

        base_income = country_base * 1.0 * (area_share ** 2.5) * num_provs * density_factor

        rsrc = resources.get(str(pid), {})
        oil   = rsrc.get("oil", {}).get("a", 0)
        gold  = rsrc.get("gold", {}).get("a", 0)
        rub   = rsrc.get("rubber", {}).get("a", 0)
        gem   = rsrc.get("gemstones", {}).get("a", 0)
        metal = rsrc.get("metal", {}).get("a", 0)
        resource_income = oil * 0.5 + gold * 0.5 + rub * 0.3 + gem * 0.5 + metal * 0.5
        resource_bonus = resource_income * 0.05

        total_income = base_income + resource_bonus
        random.seed(pid * 7 + 13)
        noise = 0.15 + random.random() * 1.7
        total_income *= noise

        raw_industry[pid] = {
            "iso": iso, "base_income": base_income, "resource_bonus": resource_bonus,
            "total_income": total_income, "area_share": area_share
        }
        iso_raw_max[iso] = max(iso_raw_max[iso], total_income)

    # ── Compute per-country scale so max income hits target proportional to real economy ──
    # China (base=100) targets level 5 (income ≤ 15). Other countries scale proportionally
    # using sqrt so that industrial powerhouses don't drop too far while small economies drop meaningfully.
    LEVEL5_TARGET = 15.0
    CHINA_BASE = COUNTRY_INDUSTRY_BASE.get("CHN", 100.0)
    LEVEL6_TARGET = 20.0
    iso_scale = {}
    iso_target = {}
    for iso, mx in iso_raw_max.items():
        if mx <= 0:
            iso_scale[iso] = 0
            iso_target[iso] = 0.0
            continue
        country_base = COUNTRY_INDUSTRY_BASE.get(iso, 0.5)
        if iso in LEVEL6_ISOS:
            target = LEVEL6_TARGET
        else:
            # Proportional scaling: target = LEVEL5_TARGET * sqrt(base / CHINA_BASE)
            target = LEVEL5_TARGET * (country_base / CHINA_BASE) ** 0.5
        target = max(target, 0.5)  # Minimum target to avoid zero
        iso_scale[iso] = target / mx
        iso_target[iso] = target

def generate_armies_and_fortifications(provinces, population):
    """Generate armies and fortifications for conflict-zone border provinces."""
    CONFLICT_LEVEL = {
        "ISR": 3, "PSE": 3, "SYR": 2, "IRQ": 2, "AFG": 2,
        "YEM": 2, "SDN": 1, "SSD": 1, "SOM": 2, "UKR": 3,
        "ARM": 2, "AZE": 2, "TWN": 2, "KOR": 2, "PRK": 2,
        "CYP": 2, "MMR": 1, "ETH": 1, "LBY": 1, "CAF": 1,
        "COD": 1, "MLI": 1, "BFA": 1, "NER": 1, "TCD": 1,
        "PAK": 1, "IRN": 2, "EGY": 1, "JOR": 1, "LBN": 2,
        "THA": 1, "KHM": 1,
    }
    CONFLICT_ENEMIES = {
        "ISR": ["PSE", "LBN", "SYR", "JOR", "EGY"],
        "PSE": ["ISR"],
        "KOR": ["PRK"],
        "PRK": ["KOR"],
        "UKR": ["RUS"],
        "ARM": ["AZE"],
        "AZE": ["ARM"],
        "TWN": ["CHN"],
        "CYP": ["TUR"],
        "LBN": ["ISR", "SYR"],
        "JOR": ["ISR", "SYR", "IRQ"],
        "EGY": ["ISR", "SDN", "LBY"],
        "SYR": ["ISR", "TUR", "IRQ", "LBN", "JOR"],
        "IRQ": ["SYR", "IRN", "TUR", "SAU", "JOR", "KWT"],
        "IRN": ["IRQ", "AFG", "PAK", "TKM", "AZE", "ARM"],
        "AFG": ["IRN", "PAK", "TKM", "UZB", "TJK"],
        "PAK": ["AFG", "IRN", "IND", "CHN"],
        "SDN": ["EGY", "LBY", "TCD", "CAF", "SSD", "ETH"],
        "SSD": ["SDN", "ETH", "COD", "CAF"],
        "YEM": ["SAU", "OMN"],
        "SOM": ["ETH", "KEN"],
        "TCD": ["LBY", "SDN", "CAF", "NER", "NGA"],
        "NER": ["TCD", "LBY", "MLI", "BFA", "NGA"],
        "MLI": ["NER", "BFA", "MRT", "DZA"],
        "BFA": ["MLI", "NER", "TCD"],
        "CAF": ["TCD", "SDN", "SSD", "COD"],
        "COD": ["CAF", "SSD", "ETH", "UGA", "RWA", "BDI", "TZA", "ZMB", "AGO"],
        "ETH": ["SDN", "SSD", "SOM", "KEN"],
        "MMR": ["THA", "LAO", "CHN", "IND", "BGD"],
        "LBY": ["EGY", "SDN", "TCD", "NER", "DZA", "TUN"],
        "THA": ["KHM"],
        "KHM": ["THA"],
    }
    print("  Detecting border provinces for fortifications...")
    border_forts = defaultdict(int)
    pimg = Image.open(PROVINCES_PNG).convert("RGB")
    pw, ph = pimg.size
    ppixels = pimg.load()
    step = 3
    for y in range(0, ph, step):
        for x in range(0, pw, step):
            r, g, b = ppixels[x, y]
            if r == 0 and g == 0 and b == 0:
                continue
            pid = (r << 16) | (g << 8) | b
            iso = provinces.get(pid, {}).get("iso_a3", "")
            enemies = CONFLICT_ENEMIES.get(iso, [])
            if not enemies:
                continue
            npid = None
            for ny in range(max(0, y - 2), min(ph, y + 3)):
                for nx in range(max(0, x - 2), min(pw, x + 3)):
                    wx = nx % pw
                    nr, ng, nb = ppixels[wx, ny]
                    if nr == 0 and ng == 0 and nb == 0:
                        continue
                    nid = (nr << 16) | (ng << 8) | nb
                    niso = provinces.get(nid, {}).get("iso_a3", "")
                    if niso in enemies:
                        npid = nid
                        break
                if npid is not None:
                    break
            if npid is not None:
                level = CONFLICT_LEVEL.get(iso, 1)
                border_forts[pid] = max(border_forts[pid], level)
    print(f"    Found {len(border_forts)} border provinces with fortifications")

    print("  Detecting border provinces for army distribution...")

    SYM_ENEMIES = defaultdict(set)
    for iso, enemies in CONFLICT_ENEMIES.items():
        for e in enemies:
            SYM_ENEMIES[iso].add(e)
            SYM_ENEMIES[e].add(iso)

    ARMY_LEVEL = dict(CONFLICT_LEVEL)
    for iso, enemies in CONFLICT_ENEMIES.items():
        base = CONFLICT_LEVEL.get(iso, 1)
        for e in enemies:
            if e not in ARMY_LEVEL:
                ARMY_LEVEL[e] = max(1, base - 1)

    iso_to_cid = {}
    for _, p in provinces.items():
        iso = p.get("iso_a3", "")
        cid = p.get("country_id", 0)
        if iso and cid:
            iso_to_cid[iso] = cid
    armies = defaultdict(list)
    country_border_pids = defaultdict(set)

    for y in range(0, ph, step):
        for x in range(0, pw, step):
            r, g, b = ppixels[x, y]
            if r == 0 and g == 0 and b == 0:
                continue
            pid = (r << 16) | (g << 8) | b
            iso = provinces.get(pid, {}).get("iso_a3", "")
            if iso not in ARMY_LEVEL:
                continue
            enemies = SYM_ENEMIES.get(iso, [])
            if not enemies:
                continue
            found_enemy = False
            for ny in range(max(0, y - 2), min(ph, y + 3)):
                for nx in range(max(0, x - 2), min(pw, x + 3)):
                    wx = nx % pw
                    nr, ng, nb = ppixels[wx, ny]
                    if nr == 0 and ng == 0 and nb == 0:
                        continue
                    nid = (nr << 16) | (ng << 8) | nb
                    niso = provinces.get(nid, {}).get("iso_a3", "")
                    if niso in enemies:
                        found_enemy = True
                        break
                if found_enemy:
                    break
            if found_enemy:
                country_border_pids[iso].add(pid)

    country_pop = defaultdict(int)
    if population is not None:
        for pid_str, pop in population.items():
            pid = int(pid_str)
            p = provinces.get(pid, {})
            iso = p.get("iso_a3", "")
            if iso:
                country_pop[iso] += pop

    POP_FRACTION = {1: 0.03, 2: 0.05, 3: 0.10}
    TROOPS_PER_LEVEL = {1: 1500000, 2: 6000000, 3: 15000000}
    raw_per_country = {}
    for iso, pids in country_border_pids.items():
        cid = iso_to_cid.get(iso, 0)
        if not cid:
            continue
        level = ARMY_LEVEL.get(iso, 1)
        total_troops = TROOPS_PER_LEVEL.get(level, 10000)
        raw_per_country[iso] = (total_troops, list(pids), level, cid)

    capped_per_country = {}
    for iso, (total_troops, pids, level, cid) in raw_per_country.items():
        pop = country_pop.get(iso, 0)
        max_fraction = POP_FRACTION.get(level, 0.03)
        cap = int(pop * max_fraction) if pop > 0 else total_troops
        capped = min(total_troops, cap)
        capped = max(capped, 1000 * len(pids))
        capped_per_country[iso] = capped

    for iso, (total_troops, pids, level, cid) in raw_per_country.items():
        capped = capped_per_country[iso]
        per_province = max(1000, round(capped / max(1, len(pids))))
        total_assigned = 0
        for pid in pids:
            prov_pop = population.get(str(pid), 0)
            capped_count = min(per_province, prov_pop) if prov_pop > 0 else per_province
            armies[pid].append({"country_id": cid, "count": capped_count})
            total_assigned += capped_count
        print(f"      {iso}: pop={country_pop.get(iso,0):>8}, raw={total_troops:>8} over {len(pids)} prov, cap={capped:>8} → {per_province}/prov (assigned {total_assigned})")
    print(f"    Distributed armies to {len(armies)} provinces across {len(country_border_pids)} countries")

    with open(OUT_ARMIES, "w") as f:
        json.dump(armies, f, indent=None, separators=(",", ":"))
    print(f"  Wrote {OUT_ARMIES} ({len(armies)} provinces)")

    # Update resources.json with correct fortification values
    try:
        with open(OUT_RESOURCES) as f:
            resources = json.load(f)
        valid_pids = set(provinces.keys())
        # Reset all fortifications, then set correct ones
        for pid_str in list(resources.keys()):
            if int(pid_str) in valid_pids:
                resources[pid_str]["fortification"] = border_forts.get(int(pid_str), 0)
            else:
                del resources[pid_str]
        # Add entries for provinces missing from resources.json
        for pid in valid_pids:
            if str(pid) not in resources:
                resources[str(pid)] = {
                    "oil": {"a": 0.0, "b": 0.0}, "gold": {"a": 0.0, "b": 0.0},
                    "rubber": {"a": 0.0, "b": 0.0}, "gemstones": {"a": 0.0, "b": 0.0},
                    "metal": {"a": 0.0, "b": 0.0},
                    "industry": {"level": 1, "income": 0.5, "specialization": "",
                                 "resourceIncome": 0.0, "popIncome": 0.0, "popModifier": 1.0},
                    "fortification": border_forts.get(pid, 0),
                }
        with open(OUT_RESOURCES, "w") as f:
            json.dump(resources, f, indent=None, separators=(",", ":"))
        print(f"  Updated {OUT_RESOURCES} fortifications ({sum(1 for v in resources.values() if v.get('fortification',0) > 0)} provinces with forts)")
    except Exception as e:
        print(f"  Warning: could not update resources.json fortifications: {e}")

    return border_forts


def compute_industry_levels(provinces, province_centers, population, resources, border_forts):
    """Compute industry levels from province area, country base, and population density.
    Modifies resources in-place. Returns nothing."""

    COUNTRY_INDUSTRY_BASE = {
        "CHN": 100, "USA": 80, "JPN": 300, "DEU": 65, "IND": 112, "KOR": 40,
        "GBR": 300, "FRA": 300, "ITA": 30, "BRA": 25, "RUS": 15, "CAN": 20,
        "MEX": 18, "IDN": 8, "ESP": 49, "TUR": 15, "THA": 14, "CHE": 13,
        "POL": 18, "NLD": 45, "SAU": 15, "AUS": 10, "MYS": 9, "SWE": 8,
        "BEL": 7, "AUT": 6, "CZE": 6, "IRL": 5, "SGP": 5, "LUX": 10, "FIN": 5,
        "NOR": 10, "DNK": 8, "PRT": 4, "GRC": 6, "NZL": 3, "ISR": 15,
        "UKR": 3, "ROU": 3, "HUN": 3, "EGY": 3, "ZAF": 3, "NGA": 3,
        "PAK": 3, "BGD": 3, "VNM": 4, "PHL": 3, "MMR": 1, "KAZ": 2,
        "ARG": 3, "CHL": 2, "COL": 2, "PER": 1, "VEN": 1, "KEN": 1,
        "ETH": 1, "TZA": 1, "COD": 1, "SDN": 0, "MAR": 2, "DZA": 2, "TCD": 0,
        "LBY": 1, "TUN": 1, "GHA": 1, "CIV": 1, "CMR": 1, "AGO": 1,
        "MOZ": 1, "ZWE": 1, "ZMB": 1, "UGA": 1, "IRN": 3, "IRQ": 1,
        "KWT": 1, "ARE": 2, "QAT": 1, "OMN": 31, "YEM": 0, "SYR": 0,
        "JOR": 0, "LBN": 30, "AFG": 0, "NPL": 0, "BTN": 0, "LKA": 1,
        "MMR": 1, "KHM": 0, "LAO": 30, "MNG": 0, "PRK": 32,
        "CUB": 32, "DOM": 0, "GTM": 0, "HND": 0, "SLV": 0, "NIC": 0,
        "CRI": 1, "PAN": 1, "BOL": 0, "PRY": 0, "URY": 0, "GUY": 0,
        "SUR": 0, "PNG": 0, "FJI": 0,
        "SRB": 1, "HRV": 1, "SVN": 1, "BIH": 1, "MKD": 0, "ALB": 0,
        "BGR": 4, "SVK": 1, "LTU": 1, "LVA": 2, "EST": 2, "BLR": 32,
        "GEO": 0, "ARM": 0, "AZE": 1, "TKM": 30, "UZB": 31, "KGZ": 0,
        "TJK": 0,
        "ATF": 28, "UNC": 0,
    }
    LEVEL6_ISOS = {"SGP"}
    LEVEL5_TARGET = 15.0
    CHINA_BASE = COUNTRY_INDUSTRY_BASE.get("CHN", 100.0)
    LEVEL6_TARGET = 20.0

    # Group provinces by country
    iso_prov_pids = defaultdict(list)
    for pid in provinces:
        iso_prov_pids[provinces[pid]["iso_a3"]].append(pid)
    country_areas = {}
    for iso, pids in iso_prov_pids.items():
        total_area = sum(province_centers.get(pid, {}).get("area", 0) for pid in pids)
        country_areas[iso] = max(total_area, 1)

    # Pass 1: compute raw incomes
    raw_industry = {}
    iso_raw_max = defaultdict(float)

    for pid in provinces:
        iso = provinces[pid]["iso_a3"]
        country_base = COUNTRY_INDUSTRY_BASE.get(iso, 0.5)
        area = province_centers.get(pid, {}).get("area", 1)
        total_area = country_areas.get(iso, 1)
        area_share = area / total_area
        num_provs = len(iso_prov_pids.get(iso, [1]))

        pop = population.get(str(pid), 0)
        pop_density = pop / max(area, 1)

        density_threshold = 0.5
        if pop_density < density_threshold:
            density_factor = (pop_density / density_threshold) ** 0.5
        else:
            density_factor = 1.0

        base_income = country_base * 1.0 * (area_share ** 2.5) * num_provs * density_factor

        rsrc = resources.get(str(pid), {})
        oil   = rsrc.get("oil", {}).get("a", 0)
        gold  = rsrc.get("gold", {}).get("a", 0)
        rub   = rsrc.get("rubber", {}).get("a", 0)
        gem   = rsrc.get("gemstones", {}).get("a", 0)
        metal = rsrc.get("metal", {}).get("a", 0)
        resource_income = oil * 0.5 + gold * 0.5 + rub * 0.3 + gem * 0.5 + metal * 0.5
        resource_bonus = resource_income * 0.05

        total_income = base_income + resource_bonus
        random.seed(pid * 7 + 13)
        noise = 0.15 + random.random() * 1.7
        total_income *= noise

        raw_industry[pid] = {
            "iso": iso, "base_income": base_income, "resource_bonus": resource_bonus,
            "total_income": total_income, "area_share": area_share
        }
        iso_raw_max[iso] = max(iso_raw_max[iso], total_income)

    # Per-country scale
    iso_scale = {}
    iso_target = {}
    for iso, mx in iso_raw_max.items():
        if mx <= 0:
            iso_scale[iso] = 0
            iso_target[iso] = 0.0
            continue
        country_base = COUNTRY_INDUSTRY_BASE.get(iso, 0.5)
        if iso in LEVEL6_ISOS:
            target = LEVEL6_TARGET
        else:
            target = LEVEL5_TARGET * (country_base / CHINA_BASE) ** 0.5
        target = max(target, 0.5)
        iso_scale[iso] = target / mx
        iso_target[iso] = target

    # Pass 2: apply scale and write industry to resources
    for pid, raw in raw_industry.items():
        iso = raw["iso"]
        scale = iso_scale.get(iso, 1.0)
        area_share = raw["area_share"]

        total_income = raw["total_income"] * scale
        target = iso_target.get(iso, 0.5)
        total_income = min(total_income, target)

        pop_mod = 1.0 + area_share * 0.5
        pop_inc = raw["base_income"] * scale * (pop_mod - 1.0)

        total_for_level = total_income + pop_inc
        if iso == "ATF" or iso == "UNC":
            total_for_level = 0.0

        if total_for_level <= 0:      level = 0
        elif total_for_level <= 0.5:  level = 1
        elif total_for_level <= 2.5:  level = 2
        elif total_for_level <= 5:    level = 3
        elif total_for_level <= 10:   level = 4
        elif total_for_level <= 15:   level = 5
        elif total_for_level <= 25:   level = 6
        elif total_for_level <= 40:   level = 7
        elif total_for_level <= 65:   level = 8
        elif total_for_level <= 100:  level = 9
        else:                         level = 10

        LEVEL_INCOMES = [0.0, 0.5, 2, 5, 10, 20, 50, 80, 100, 120, 150]
        total_income = LEVEL_INCOMES[level]

        if iso != "ATF" and iso != "UNC":
            total_income = max(0.1, total_income)

        rsrc = resources.get(str(pid), {})
        oil   = rsrc.get("oil", {}).get("a", 0)
        gold  = rsrc.get("gold", {}).get("a", 0)
        rub   = rsrc.get("rubber", {}).get("a", 0)
        gem   = rsrc.get("gemstones", {}).get("a", 0)
        metal = rsrc.get("metal", {}).get("a", 0)
        best_val = oil * 1.5
        spec = "Oil"
        if gold * 2.0 > best_val: best_val = gold * 2.0; spec = "Gold"
        if rub * 0.8 > best_val:  best_val = rub * 0.8;  spec = "Rubber"
        if gem * 3.0 > best_val:  best_val = gem * 3.0;  spec = "Gemstones"
        if metal * 1.0 > best_val: best_val = metal * 1.0; spec = "Metal"
        if best_val <= 0: spec = ""

        if level <= 0:
            spec = ""
        else:
            random.seed(pid * 13 + 7)
            if spec != "" and random.random() < 0.4:
                spec = ""

        resources[str(pid)]["industry"] = {
            "level": level,
            "income": round(total_income, 1),
            "specialization": spec,
            "resourceIncome": round(raw["resource_bonus"] * scale, 1),
            "popIncome": round(pop_inc, 1),
            "popModifier": round(pop_mod, 2),
        }
        resources[str(pid)]["fortification"] = border_forts.get(pid, 0)

    # Manual industry level overrides.
    #
    # These were raw province ids, and province ids are output of the
    # generator, not input to it: regenerate the map and 1697 is somewhere
    # else entirely, so the override lands on an unrelated province and the
    # one it was written for goes back to whatever the formula said. Keyed on
    # coordinates instead, which survive a regeneration -- the point of an
    # override is a place, not a number.
    INDUSTRY_OVERRIDE_AT = [
        # lon, lat, level, what it is
        (139.7, 35.7, 5, "Tokyo"),
        (-74.0, 40.7, 5, "New York"),
        (2.35, 48.86, 3, "Paris"),
        (-0.13, 51.51, 3, "London"),
        (13.4, 52.52, 3, "Berlin"),
        (121.5, 31.2, 3, "Shanghai"),
        (37.6, 55.75, 4, "Moscow"),
    ]
    if province_centers:
        for lon, lat, lvl, label in INDUSTRY_OVERRIDE_AT:
            best, best_d = None, None
            for pid, c in province_centers.items():
                d = (c["lon"] - lon) ** 2 + (c["lat"] - lat) ** 2
                if best_d is None or d < best_d:
                    best, best_d = pid, d
            if best is not None and str(best) in resources:
                resources[str(best)]["industry"]["level"] = lvl

    n_with_forts = sum(1 for v in resources.values() if v.get("fortification", 0) > 0)
    levels = defaultdict(int)
    for v in resources.values():
        ind = v.get("industry", {})
        levels[ind.get("level", 0)] += 1
    print(f"  Computed industry for {len(resources)} provinces (levels: {dict(sorted(levels.items()))})")
    print(f"  Fortifications: {n_with_forts} provinces")
def main():
    print("=== Loading province data ===")
    provinces, cid_to_iso = load_provinces()
    print(f"  Loaded {len(provinces)} provinces")

    # Load population data for industry density calculation
    print("\n=== Loading population data ===")
    with open(os.path.join(DATA_DIR, "population.json")) as f:
        population = json.load(f)
    print(f"  Loaded population for {len(population)} provinces")

    print("\n=== Computing province centers ===")
    try:
        centers = compute_province_centers_numpy(provinces)
        print(f"  Computed centers for {len(centers)} provinces (numpy)")
    except Exception as e:
        print(f"  numpy method failed ({e}), using fast scan...")
        centers = compute_province_centers_fast(provinces)
        print(f"  Computed centers for {len(centers)} provinces (fast scan)")

    # ── Province centres for generate_minorities.py ──
    print("\n=== Writing province centres ===")
    write_province_centers(centers)
    print("  minorities.json is generated by tools/generate_minorities.py, "
          "which runs after this step")

    # ── Armies & fortifications — always generate from current province data ──
    print("\n=== Generating armies and fortifications ===")
    border_forts = generate_armies_and_fortifications(provinces, population)

    # ── Resources — skip if shapefiles missing ──
    gold_points = []; gem_points = []; metal_points = []; oil_centroids = []
    resources_loaded = False
    try:
        print("\n=== Processing USGS mineral deposits ===")
        gold_points, gem_points = load_usgs_gold()
        print("\n=== Processing USGS metal deposits ===")
        metal_points = load_usgs_metal()
        print("\n=== Loading oil regions ===")
        oil_centroids = load_oil_fields()
        resources_loaded = True
    except Exception as e:
        print(f"  USGS shapefiles unavailable ({e}), preserving existing resources.json")
    if resources_loaded:
        print("\n=== Assigning resources ===")
        resources = assign_resources(provinces, centers, gold_points, gem_points, oil_centroids, metal_points, population, border_forts)
        valid_pids = set(int(k) for k in provinces)
        resources = {k: v for k, v in resources.items() if int(k) in valid_pids}
    else:
        print("  Loading existing resources.json for industry computation")
        try:
            with open(OUT_RESOURCES) as f:
                resources = json.load(f)
        except:
            resources = {}
        # Ensure all provinces have entries
        for pid in provinces:
            if str(pid) not in resources:
                resources[str(pid)] = {
                    "oil": {"a": 0.0, "b": 0.0}, "gold": {"a": 0.0, "b": 0.0},
                    "rubber": {"a": 0.0, "b": 0.0}, "gemstones": {"a": 0.0, "b": 0.0},
                    "metal": {"a": 0.0, "b": 0.0},
                }

    # Always compute industry levels (uses area, population, country base)
    print("\n=== Computing industry levels ===")
    compute_industry_levels(provinces, centers, population, resources, border_forts)
    with open(OUT_RESOURCES, "w") as f:
        json.dump(resources, f, indent=None, separators=(",", ":"))
    print(f"  Wrote {OUT_RESOURCES} ({len(resources)} provinces)")

    # ── Ports from World Port Index ──
    print("\n=== Processing World Port Index ===")
    import csv
    wpi_path = "/tmp/wpi/UpdatedPub150.csv"
    prov_img = Image.open(PROVINCES_PNG).convert("RGB")
    prov_px = prov_img.load()
    ls_img = Image.open(os.path.join(DATA_DIR, "land_sea.png")).convert("L")
    ls_px = ls_img.load()
    pw2, ph2 = prov_img.size

    # Coastal, decided the way the ENGINE decides it.
    #
    # This used to be its own approximation: sample every eighth pixel of the
    # province and look five pixels out for anything that is not land. It said
    # yes to inland Belgium, which is how every shipped map came to carry a
    # level-3 port -- Antwerp, whose Scheldt approach the raster does not
    # resolve -- on a province Game::isProvinceCoastal calls land-locked. The
    # player got an anchor in the middle of Belgium that could not be upgraded
    # and could not berth a ship.
    #
    # Two coastal tests will always eventually disagree, and when they do it is
    # the map data that is wrong, because the engine's answer is the one the
    # player sees. So there is one test now: some pixel of the province is
    # adjacent to a water body of at least MIN_WATER_BODY pixels.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from fill_water_speckle import label_components, min_water_body

    _ls_arr = np.array(ls_img, dtype=np.uint8) >= 128
    _pv_arr = np.array(prov_img.convert("RGB"), dtype=np.uint32)
    _pid_arr = (_pv_arr[:, :, 0] << 16 | _pv_arr[:, :, 1] << 8 | _pv_arr[:, :, 2])
    _lbl, _sizes = label_components(~_ls_arr)
    _big = (_lbl > 0) & (_sizes[_lbl] >= min_water_body())
    _grow = np.zeros_like(_big)
    _grow[1:, :] |= _big[:-1, :]
    _grow[:-1, :] |= _big[1:, :]
    _grow[:, 1:] |= _big[:, :-1]
    _grow[:, :-1] |= _big[:, 1:]
    COASTAL = set(np.unique(_pid_arr[_grow & (_pid_arr != 0)]).tolist()) - {0}
    print(f"  {len(COASTAL)} coastal provinces by the engine's own rule")

    def has_ocean_neighbor(pid, check_radius=None):
        return pid in COASTAL

    def nearest_ocean_xy(cx, cy, max_radius=200):
        """Find the nearest pixel that is truly ocean (land_sea=0 AND provinces.png is black).
        Searching outward in a square spiral pattern."""
        for r in range(1, max_radius + 1):
            for dy in range(-r, r + 1):
                for dx in range(-r, r + 1):
                    if abs(dx) != r and abs(dy) != r:
                        continue
                    nx = (cx + dx) % pw2
                    ny = cy + dy
                    if ny < 0 or ny >= ph2:
                        continue
                    if ls_px[nx, ny] == 0:
                        r2, g2, b2 = prov_px[nx, ny]
                        if r2 == 0 and g2 == 0 and b2 == 0:
                            return nx, ny
        return None

    def is_large_water(ox, oy, radius=80):
        """Check if ocean pixel is in large open water (not lake/enclosed sea)."""
        ocean = 0
        total = 0
        for dy in range(-radius, radius + 1, 4):
            for dx in range(-radius, radius + 1, 4):
                nx = (ox + dx) % pw2
                ny = oy + dy
                if 0 <= ny < ph2:
                    total += 1
                    if ls_px[nx, ny] == 0:
                        r2, g2, b2 = prov_px[nx, ny]
                        if r2 == 0 and g2 == 0 and b2 == 0:
                            ocean += 1
        return ocean / max(total, 1) > 0.3

    HARBOR_SIZE_LEVEL = {"Very Small": 1, "Small": 1, "Medium": 2, "Large": 3, "Very Large": 3}
    ports = {}
    if not os.path.exists(wpi_path):
        print(f"  WPI file not found at {wpi_path}, generating empty ports.json")
    else:
        with open(wpi_path, newline='', encoding='utf-8-sig') as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    lat = float(row["Latitude"])
                    lon = float(row["Longitude"])
                except:
                    continue
                px = int((lon + 180.0) / 360.0 * pw2) % pw2
                py = int((90.0 - lat) / 180.0 * ph2)
                if py < 0 or py >= ph2:
                    continue
                r, g, b = prov_px[px, py]
                if r == 0 and g == 0 and b == 0:
                    continue
                pid = (r << 16) | (g << 8) | b
                if pid not in provinces:
                    continue
                if not has_ocean_neighbor(pid, 5):
                    continue
                _, _, _, wbx, wby = None, None, None, None, None
                for sy in range(max(0, py - 20), min(ph2, py + 21)):
                    for sx in range(max(0, px - 20), min(pw2, px + 21)):
                        if ls_px[sx, sy] == 0:
                            r2, g2, b2 = prov_px[sx, sy]
                            if r2 == 0 and g2 == 0 and b2 == 0:
                                wbx, wby = sx, sy
                                break
                    if wbx is not None:
                        break
                if wbx is None or not is_large_water(wbx, wby):
                    continue
                level = HARBOR_SIZE_LEVEL.get(row.get("Harbor Size", ""), 1)
                cur = ports.get(str(pid), {}).get("level", 0)
                if level > cur:
                    ports[str(pid)] = {"level": level}
        print(f"  Found {len(ports)} port provinces from WPI ({sum(1 for v in ports.values() if v['level']==3)} level-3, {sum(1 for v in ports.values() if v['level']==2)} level-2, {sum(1 for v in ports.values() if v['level']==1)} level-1)")

    with open(OUT_PORTS, "w") as f:
        json.dump(ports, f, indent=None, separators=(",", ":"))
    print(f"  Wrote {OUT_PORTS}")

    print("\n=== Done! ===")


if __name__ == "__main__":
    main()