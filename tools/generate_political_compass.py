#!/usr/bin/env python3
"""
Generate political_compass.json with per-province political compass values.
Each entry: {province_id: {"left": int, "auth": int}}
  left: -100 (right) to 100 (left)
  auth: -100 (libertarian) to 100 (authoritarian)
"""
import json
import random
import hashlib

def base_from_iso(iso):
    v = int(hashlib.md5(iso.encode()).hexdigest()[:8], 16)
    left = (v & 0xFFFF) % 201 - 100
    auth = ((v >> 16) & 0xFFFF) % 201 - 100
    return left, auth

COUNTRY_BIAS = {
    "USA": (20, -10),
    "GBR": (10, 15),
    "CAN": (30, -20),
    "FRA": (40, 10),
    "DEU": (30, 15),
    "ITA": (25, 20),
    "ESP": (30, 25),
    "CHN": (-10, 80),
    "RUS": (-20, 85),
    "JPN": (10, 40),
    "IND": (10, 30),
    "BRA": (20, 15),
    "AUS": (15, -5),
    "TUR": (-15, 50),
    "KOR": (15, 35),
    "MEX": (20, 20),
    "IDN": (5, 45),
    "NGA": (10, 40),
    "EGY": (5, 55),
    "ZAF": (25, 10),
    "ARG": (30, 10),
    "SAU": (-30, 85),
    "IRN": (-20, 80),
    "PRK": (-40, 95),
    "CUB": (60, 85),
    "SWE": (50, -30),
    "NOR": (45, -35),
    "DNK": (40, -25),
    "FIN": (35, -20),
    "NLD": (35, -25),
    "CHE": (10, -10),
    "POL": (5, 40),
    "UKR": (15, 35),
    "GRC": (20, 30),
    "PRT": (30, 5),
    "IRL": (35, -15),
    "NZL": (30, -20),
    "CHL": (25, 10),
    "COL": (20, 25),
    "PER": (15, 30),
    "ISL": (60, -30),
    "ISR": (5, 30),
    "PAK": (-5, 50),
    "BGD": (10, 45),
    "VNM": (40, 80),
    "THA": (5, 50),
    "PHL": (15, 35),
    "MMR": (5, 70),
    "AGO": (10, 55),
    "KEN": (15, 40),
    "ETH": (10, 50),
    "TZA": (15, 45),
    "SDN": (5, 65),
    "MAR": (10, 50),
    "DZA": (5, 60),
    "IRQ": (-10, 65),
    "AFG": (-15, 70),
    "YEM": (-10, 70),
    "SYR": (-5, 75),
}

DEV_RANGE = 20

def main():
    data_dir = '/Users/vladyavdoshenko/CLionProjects/OpenDoctrines/data'
    with open(f'{data_dir}/provinces.json') as f:
        provs = json.load(f)
    result = {}
    by_iso = {}
    for pid_str, entry in provs.items():
        iso = entry.get('iso_a3', '')
        if not iso:
            continue
        if iso not in by_iso:
            by_iso[iso] = []
        by_iso[iso].append(int(pid_str))

    for iso, pids in by_iso.items():
        if iso in COUNTRY_BIAS:
            base_left, base_auth = COUNTRY_BIAS[iso]
        else:
            base_left, base_auth = base_from_iso(iso)
        rng = random.Random(iso)
        for pid in pids:
            prov_rng = random.Random(pid * 1000)
            left = max(-100, min(100, base_left + prov_rng.randint(-DEV_RANGE, DEV_RANGE)))
            auth = max(-100, min(100, base_auth + prov_rng.randint(-DEV_RANGE, DEV_RANGE)))
            result[str(pid)] = {"left": left, "auth": auth}
    out_path = f'{data_dir}/political_compass.json'
    with open(out_path, 'w') as f:
        json.dump(result, f, indent=2)
    print(f"Saved {out_path} ({len(result)} provinces)")

    # Also output country_compass.json with per-country base positions
    country_result = {}
    for iso, pids in by_iso.items():
        if iso in COUNTRY_BIAS:
            base_left, base_auth = COUNTRY_BIAS[iso]
        else:
            base_left, base_auth = base_from_iso(iso)
        country_result[iso] = {"left": base_left, "auth": base_auth}
    ccout = f'{data_dir}/country_compass.json'
    with open(ccout, 'w') as f:
        json.dump(country_result, f, indent=2)
    print(f"Saved {ccout} ({len(country_result)} countries)")

if __name__ == '__main__':
    main()
