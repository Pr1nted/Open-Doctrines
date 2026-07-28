#!/usr/bin/env python3
"""
Generate starting_minority_policies.json — per-country per-minority ethnic policy defaults.
Each entry: {isoA3: {minority_name: [optIdx x 6 categories]}}

Category order: deportation, economic, cultural, political, language, integration

Defaults are derived from country compass position (authoritarian/libertarian, left/right).
Hardcoded overrides for notable conflict minorities.
"""

import json
import os

DATA_DIR = os.path.join(os.path.dirname(__file__), '..', 'data')

def main():
    with open(os.path.join(DATA_DIR, 'countries.json')) as f:
        countries = json.load(f)
    with open(os.path.join(DATA_DIR, 'provinces.json')) as f:
        provinces = json.load(f)
    with open(os.path.join(DATA_DIR, 'country_compass.json')) as f:
        country_compass = json.load(f)

    # Build province -> iso mapping from provinces.json
    prov_iso = {}
    for pid_str, info in provinces.items():
        iso = info.get('iso_a3', '')
        if iso:
            prov_iso[int(pid_str)] = iso

    # Build set of minorities per country
    try:
        with open(os.path.join(DATA_DIR, 'minorities.json')) as f:
            minorities = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        print("  minorities.json not found, skipping minority policies")
        out_path = os.path.join(DATA_DIR, 'starting_minority_policies.json')
        with open(out_path, 'w') as f:
            json.dump({}, f)
        print(f"Saved {out_path} (empty)")
        return

    country_minorities = {}
    for pid_str, groups in minorities.items():
        pid = int(pid_str)
        iso = prov_iso.get(pid)
        if not iso:
            continue
        if iso not in country_minorities:
            country_minorities[iso] = {}
        for g in groups:
            name = g['n']
            if name not in country_minorities[iso]:
                country_minorities[iso][name] = 0.0
            country_minorities[iso][name] += g['p']

    # HARDCODED OVERRIDES: specific country-minority → policy option indices
    # Format: iso -> minority -> [dep, eco, cul, pol, lan, int]
    #
    # Group names must match tools/data/ethnic_groups.json exactly. They were
    # rewritten when GREG was dropped: GREG used plural ethnonyms ("Chechens",
    # "Tatars") and country-qualified oddities ("Germans (China)"), the authored
    # table uses singular adjectival forms and China's actual listed minorities.
    # An override naming a group its country does not have is reported below
    # rather than silently doing nothing, which is how the old CHN block came to
    # be entirely dead without anyone noticing.
    OVERRIDES = {
        "RUS": {
            "Chechen":           [0, 2, 2, 2, 2, 2],  # Harsh, None, Suppress, Disenfranchised, Ban, None
            "Tatar":             [1, 2, 1, 1, 1, 1],  # Medium, None, Partial, Standard, Tolerance, Passive
            "Ukrainian":         [0, 2, 2, 1, 1, 2],  # Harsh, None, Suppress, Standard, Tolerance, None
            "Bashkir":           [1, 2, 1, 1, 1, 1],
            "Chuvash":           [1, 2, 1, 1, 1, 1],
            "Avar":              [1, 2, 1, 1, 1, 1],
            "Mordvin":           [1, 2, 1, 1, 1, 1],
            "Udmurt":            [1, 2, 1, 1, 1, 1],
            "Mari":              [1, 2, 1, 1, 1, 1],
        },
        "CHN": {
            "Zhuang":            [1, 1, 1, 1, 1, 1],
            "Hui":               [1, 2, 1, 1, 1, 1],
            "Manchu":            [1, 2, 1, 1, 1, 1],
            "Mongol":            [1, 2, 1, 1, 1, 1],
            "Korean":            [1, 2, 1, 1, 1, 1],
        },
    }

    def heuristic_from_compass(iso, auth, left):
        """Return default option indices based on compass position."""
        # Base (centrist): neutral options — net ~+3% align/turn
        defaults = [1, 2, 1, 1, 1, 1]

        # Moderate authoritarian (auth > 10): slightly less generous
        if auth > 10:
            defaults[0] = 1  # Medium
            defaults[2] = 1  # Partial
            defaults[3] = 1  # Standard
            defaults[4] = 1  # Tolerance
            defaults[5] = 1  # Passive

        # Authoritarian + right
        if auth > 20 and left < -5:
            defaults[1] = 2  # No economic incentives

        return defaults

    # An override that matches nothing is a silent no-op, and a silent no-op is
    # indistinguishable from a deliberate neutral policy when you read the data
    # back. Say so instead.
    unmatched = [
        f"{iso}/{name}"
        for iso, groups in OVERRIDES.items()
        for name in groups
        if name not in country_minorities.get(iso, {})
    ]
    if unmatched:
        print(f"  WARNING: {len(unmatched)} override(s) name a group the country "
              f"does not have, and will do nothing: {', '.join(unmatched)}")

    result = {}
    for iso, minorities_dict in country_minorities.items():
        compass = country_compass.get(iso, {"left": 0, "auth": 0})
        auth = compass["auth"]
        left = compass["left"]

        # Find the majority group (largest share) — they get neutral/default policies
        majority_name = max(minorities_dict, key=minorities_dict.get)

        for mname, share in minorities_dict.items():
            # Check hardcoded override first
            if iso in OVERRIDES and mname in OVERRIDES[iso]:
                result.setdefault(iso, {})[mname] = OVERRIDES[iso][mname]
            elif mname == majority_name:
                # Majority group always gets default/neutral policies
                result.setdefault(iso, {})[mname] = [1, 1, 1, 1, 1, 1]
            else:
                result.setdefault(iso, {})[mname] = heuristic_from_compass(iso, auth, left)

    out_path = os.path.join(DATA_DIR, 'starting_minority_policies.json')
    with open(out_path, 'w') as f:
        json.dump(result, f, indent=2)
    print(f"Saved {out_path} ({sum(len(v) for v in result.values())} entries across {len(result)} countries)")

if __name__ == '__main__':
    main()
