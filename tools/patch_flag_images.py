#!/usr/bin/env python3
"""Update countries.json to reference downloaded flag images.

For countries where data/flags/{iso}.png exists, replace the
procedural flag patterns with image references.

Usage:
    python3 tools/patch_flag_images.py
"""

import json
import os

COUNTRIES_JSON = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "countries.json")
FLAG_DIR = os.path.join(os.path.dirname(COUNTRIES_JSON), "flags")

# Countries whose flags contain hate symbols (communist stars, AK-47, etc.)
# These get pixelated when censored mode is on
HATE_SYMBOL_ISOS = {
    "PRK",  # North Korea - red communist star
    "CHN",  # China - communist stars
    "VNM",  # Vietnam - communist star
    "LAO",  # Laos - communist symbolism
    "AGO",  # Angola - communist gear, machete, star
    "MOZ",  # Mozambique - AK-47 on flag
}

NAME_TO_ISO = {
    "France": "FRA",
    "Norway": "NOR",
    "Kosovo": "XKV",
    "Somaliland": "XSO",
    "N. Cyprus": "XNC",
}

with open(COUNTRIES_JSON) as f:
    data = json.load(f)

updated = 0
for entry in data.values():
    name = entry.get("name", "")
    iso = entry.get("iso_a3", "")
    if not iso or iso == "-99":
        iso = NAME_TO_ISO.get(name, "")
        if iso:
            entry["iso_a3"] = iso  # persist the fix
    if not iso:
        continue
    img_path = os.path.join(FLAG_DIR, f"{iso}.png")
    if os.path.exists(img_path):
        img_name = f"flags/{iso}.png"
        # Both actual and censored point to the same image
        # Censored ones get pixelated at runtime only if iso is in HATE_SYMBOL_ISOS
        entry["flag_actual"] = {"image": img_name}
        # All censored flags are pixelated at runtime when "Show actual flags" is off
        entry["flag_censored"] = {"image": img_name, "censored": True}
        updated += 1

with open(COUNTRIES_JSON, "w") as f:
    json.dump(data, f, indent=2)

print(f"Updated {updated} countries with image references in {COUNTRIES_JSON}")
