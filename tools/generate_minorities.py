#!/usr/bin/env python3
"""
Generate minorities.json — per-province ethnic composition — and the colour
table that goes with it.

WHERE THE NUMBERS COME FROM

Everything this reads is in `tools/data/ethnic_groups.json`, which the project
authors and owns. It replaced GREG (ETH Zurich), which shipped no licence at
all: its download page asks only that you cite it "when using the GREG data for
your research", which is a citation request under research framing rather than
a grant, and GREG is itself a digitisation of the 1964 Soviet *Atlas Narodov
Mira*, so the people distributing it are in no position to sublicense it
either. See NOTICE.md.

Nothing here is copied from a third-party dataset. Country composition figures
come from public-domain reference material (principally the CIA World Factbook,
a US Government work) and published census summaries, rounded and edited for
playability; the concentration boxes are hand-drawn.

HOW A PROVINCE GETS ITS MIX

  1. Start from the country's composition.
  2. Any concentration box containing the province centre pins its group to the
     box's share -- this is what puts Kurds in the southeast of Turkey rather
     than spread evenly over it. Requires data/province_centers.json, written
     by overlay_real_data.py; without it step 2 is skipped and the composition
     is country-uniform, which is the pre-concentration behaviour.
  3. Whatever share is left is split among the remaining groups in proportion
     to their national figures, with a deterministic per-province jitter so
     neighbouring provinces are not identical.

Output is stable for a given input: the jitter is seeded from the province id,
so re-running does not churn the map.
"""

import json
import hashlib
import os
import random
import re
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(TOOLS_DIR)
# `--data-dir` so this can be pointed at an extracted .odmap without writing
# into the working data/ directory. Defaults to the one the pipeline uses.
DATA_DIR = os.path.join(PROJECT_ROOT, "data")
GROUPS_JSON = os.path.join(TOOLS_DIR, "data", "ethnic_groups.json")

# A group has to clear this after normalisation to be listed at all. Below it
# the entry is noise that costs a line in every province panel.
MIN_SHARE = 0.5

# Concentrations are capped in total so a province always keeps some of the
# national mix. Three overlapping boxes must not sum to 100 and erase everyone.
MAX_CONCENTRATED = 92.0

GENERIC_PATTERNS = re.compile(r"\b(Other|Generic|Misc)\b", re.I)


def load_groups():
    with open(GROUPS_JSON, encoding="utf-8") as f:
        doc = json.load(f)
    if doc.get("schema") != 1:
        raise SystemExit(f"{GROUPS_JSON}: unsupported schema {doc.get('schema')!r}")
    iso_region = {}
    for region, isos in doc["iso_regions"].items():
        for iso in isos:
            iso_region[iso] = region
    return doc, iso_region


def load_centers():
    """pid -> (lon, lat), or {} when overlay_real_data.py has not run yet."""
    path = os.path.join(DATA_DIR, "province_centers.json")
    try:
        with open(path) as f:
            raw = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}
    return {int(pid): (c["lon"], c["lat"]) for pid, c in raw.items()}


def color_from_name(name):
    h = int(hashlib.md5(name.encode()).hexdigest()[:6], 16)
    r = (h & 0xFF) % 200 + 30
    g = ((h >> 8) & 0xFF) % 200 + 30
    b = ((h >> 16) & 0xFF) % 200 + 30
    return [r, g, b]


def clean_composition(groups, aliases):
    """Apply name aliases and drop the catch-all buckets."""
    out = []
    for name, base in groups:
        name = aliases.get(name, name)
        if GENERIC_PATTERNS.search(name.strip()):
            continue
        out.append((name, float(base)))
    return out


def in_box(lon, lat, bbox):
    lon0, lat0, lon1, lat1 = bbox
    return lon0 <= lon <= lon1 and lat0 <= lat <= lat1


def compose(pid, composition, pinned):
    """Blend the national composition with any concentrations that apply here.

    `pinned` is {group: share}. Pinned groups take their share off the top;
    everyone else divides the remainder in proportion to their national figure.
    """
    rng = random.Random(pid * 1000 + 777)

    settled = sum(pinned.values())
    if settled > MAX_CONCENTRATED:
        scale = MAX_CONCENTRATED / settled
        pinned = {g: s * scale for g, s in pinned.items()}
        settled = MAX_CONCENTRATED

    rest = [(n, b) for n, b in composition if n not in pinned]
    rest_total = sum(b for _, b in rest)
    remainder = max(0.0, 100.0 - settled)

    raw = []
    for name, share in pinned.items():
        # Pinned groups still wobble a little, or every province inside one box
        # reads as copy-pasted. Much less than the free groups do.
        raw.append((name, max(1.0, share + rng.uniform(-share * 0.08, share * 0.08))))
    for name, base in rest:
        weight = (base / rest_total * remainder) if rest_total > 0 else 0.0
        dev = rng.uniform(-max(weight * 0.3, 1.0), max(weight * 0.3, 1.0))
        raw.append((name, max(0.4, weight + dev)))

    total = sum(p for _, p in raw)
    if total <= 0:
        return []
    entries = [{"n": n, "p": round(p / total * 100, 1)} for n, p in raw]
    entries = [e for e in entries if e["p"] >= MIN_SHARE]
    if not entries:
        return []

    # Normalise to exactly 100 so the province panel never shows 99.7%.
    drift = round(100.0 - sum(e["p"] for e in entries), 1)
    if drift:
        biggest = max(entries, key=lambda e: e["p"])
        biggest["p"] = round(biggest["p"] + drift, 1)
    return entries


def main(argv):
    global DATA_DIR
    if "--data-dir" in argv:
        DATA_DIR = argv[argv.index("--data-dir") + 1]

    doc, iso_region = load_groups()
    aliases = doc["aliases"]
    countries = doc["countries"]
    fallback = doc["region_fallback"]

    with open(os.path.join(DATA_DIR, "provinces.json")) as f:
        provs = json.load(f)

    centers = load_centers()
    if centers:
        print(f"  Province centres: {len(centers)} loaded, concentrations active")
    else:
        print("  province_centers.json absent — country-uniform composition "
              "(run overlay_real_data.py first for regional concentration)")

    # Concentrations indexed by country so each province tests only its own.
    by_iso = {}
    for c in doc["concentrations"]:
        by_iso.setdefault(c["iso"], []).append(c)

    provinces_by_iso = {}
    for pid_str, entry in provs.items():
        iso = entry.get("iso_a3", "")
        if iso:
            provinces_by_iso.setdefault(iso, []).append(int(pid_str))

    result = {}
    colors = {}
    concentrated_provinces = 0
    fallback_isos = []

    for iso, pids in provinces_by_iso.items():
        groups = countries.get(iso)
        if groups is None:
            groups = fallback.get(iso_region.get(iso), [["European", 100]])
            fallback_isos.append(iso)
        composition = clean_composition(groups, aliases)
        if not composition:
            composition = [("European", 100.0)]

        boxes = by_iso.get(iso, [])
        for pid in pids:
            pinned = {}
            center = centers.get(pid)
            if center and boxes:
                lon, lat = center
                for box in boxes:
                    if in_box(lon, lat, box["bbox"]):
                        name = aliases.get(box["group"], box["group"])
                        pinned[name] = max(pinned.get(name, 0.0), float(box["share"]))
            if pinned:
                concentrated_provinces += 1

            entries = compose(pid, composition, pinned)
            if not entries:
                continue
            result[str(pid)] = entries
            for e in entries:
                colors.setdefault(e["n"], color_from_name(e["n"]))

    out_path = os.path.join(DATA_DIR, "minorities.json")
    with open(out_path, "w") as f:
        json.dump(result, f, separators=(",", ":"))
    col_path = os.path.join(DATA_DIR, "minority_colors.json")
    with open(col_path, "w") as f:
        json.dump(colors, f, separators=(",", ":"))

    print(f"Saved {out_path} ({len(result)} provinces, {len(colors)} unique groups)")
    print(f"Saved {col_path} ({len(colors)} colors)")
    print(f"  {concentrated_provinces} provinces hit at least one concentration box")
    if fallback_isos:
        print(f"  {len(fallback_isos)} countries had no composition and used the "
              f"regional fallback: {', '.join(sorted(fallback_isos))}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
