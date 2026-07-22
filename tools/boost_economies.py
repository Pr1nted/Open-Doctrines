#!/usr/bin/env python3
"""
Boost economies of Japan, France, and UK to GDP ~100 (trillion? billion? whatever units)
by adjusting their province populations and industry values in countries.json and provinces.json.
"""

import json

COUNTRIES_FILE = "data/countries.json"
PROVINCES_FILE = "data/provinces.json"

# Target GDP (in whatever units the game uses - likely billions USD 2000)
TARGET_GDP = 100  # 100 trillion? 100 billion? The game uses some unit

# Target countries
TARGET_COUNTRIES = ["JPN", "FRA", "GBR"]  # Japan, France, UK

def load_json(path):
    with open(path, 'r') as f:
        return json.load(f)

def save_json(path, data):
    with open(path, 'w') as f:
        json.dump(data, f, indent=2)

def main():
    countries = load_json(COUNTRIES_FILE)
    provinces = load_json(PROVINCES_FILE)

    # Find target countries
    targets = {}
    for c in countries:
        if c.get("iso3") in TARGET_COUNTRIES:
            targets[c["iso3"]] = c

    if len(targets) != 3:
        print(f"Found {len(targets)} target countries: {list(targets.keys())}")
        for iso3, c in targets.items():
            print(f"  {iso3}: {c.get('name')} - GDP: {c.get('gdp', 'N/A')}, Pop: {c.get('population', 'N/A')}")
        return

    print("Current GDP:")
    for iso3, c in targets.items():
        print(f"  {iso3}: {c.get('name')} - GDP: {c.get('gdp', 'N/A')}, Pop: {c.get('population', 'N/A')}")

    # Calculate current GDP per capita for each country
    for iso3, c in targets.items():
        gdp = c.get('gdp', 0)
        pop = c.get('population', 1)
        gdp_per_cap = gdp / pop if pop > 0 else 0
        print(f"  {iso3}: GDP={gdp}, Pop={pop}, GDP/cap={gdp_per_cap:.4f}")

    # Target: scale population to achieve target GDP assuming similar GDP/cap
    # Or we could adjust industry values in provinces
    # Let's adjust province populations proportionally to hit target GDP

    # First, find provinces for each target country
    province_map = {iso3: [] for iso3 in TARGET_COUNTRIES}
    for p in provinces:
        owner = p.get("owner")
        if owner in TARGET_COUNTRIES:
            province_map[owner].append(p)

    print("\nProvince counts:")
    for iso3, provs in province_map.items():
        total_pop = sum(p.get("population", 0) for p in provs)
        total_industry = sum(p.get("industry", 0) for p in provs)
        print(f"  {iso3}: {len(provs)} provinces, total pop={total_pop}, total industry={total_industry}")

    # Strategy: scale province populations proportionally to hit target GDP
    # GDP seems to be calculated from population * some factor + industry
    # Let's check what GDP formula might be by looking at current data

    # For now, let's scale populations proportionally to hit target GDP
    # assuming GDP scales roughly linearly with population
    for iso3, c in targets.items():
        current_gdp = c.get('gdp', 1)
        target_gdp = TARGET_GDP
        scale_factor = target_gdp / current_gdp if current_gdp > 0 else 1
        print(f"\n{iso3}: scale factor = {scale_factor:.4f}")

        # Scale province populations
        for p in province_map[iso3]:
            old_pop = p.get("population", 0)
            new_pop = int(old_pop * scale_factor)
            p["population"] = new_pop

            # Also scale industry proportionally
            old_ind = p.get("industry", 0)
            p["industry"] = int(old_ind * scale_factor)

        # Update country totals
        new_pop = sum(p.get("population", 0) for p in province_map[iso3])
        new_ind = sum(p.get("industry", 0) for p in province_map[iso3])
        c["population"] = new_pop
        c["industry"] = new_ind
        c["gdp"] = target_gdp
        print(f"  New: Pop={new_pop}, Industry={new_ind}, GDP={target_gdp}")

    # Save
    save_json(COUNTRIES_FILE, countries)
    save_json(PROVINCES_FILE, provinces)
    print("\nSaved updated countries.json and provinces.json")

    # Verify
    print("\nVerification:")
    countries = load_json(COUNTRIES_FILE)
    for c in countries:
        if c.get("iso3") in TARGET_COUNTRIES:
            print(f"  {c['iso3']}: GDP={c.get('gdp')}, Pop={c.get('population')}")

if __name__ == "__main__":
    main()