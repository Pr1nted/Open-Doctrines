#!/usr/bin/env python3
"""
Generate resources.json with per-province resource deposits.
Resources: gold, oil, rubber, gemstones.
Each has: amount (0-100 scale) and industry boost multiplier.
"""
import json, random

# Base resource values by country ISO.
# Each resource: (base_amount, industry_boost_pct)
# Amount 0-100 where 0=none, 100=world-class deposits
# Boost: percentage boost to industry (e.g. 5 = +5% industry output)
RESOURCE_MAP = {
    # Middle East — oil giants
    "SAU": {"oil": (95, 40), "gold": (5, 2), "rubber": (0, 0), "gemstones": (2, 1)},
    "IRQ": {"oil": (90, 38), "gold": (3, 1), "rubber": (0, 0), "gemstones": (5, 3)},
    "KWT": {"oil": (95, 42), "gold": (0, 0), "rubber": (0, 0), "gemstones": (0, 0)},
    "ARE": {"oil": (90, 40), "gold": (2, 1), "rubber": (0, 0), "gemstones": (3, 2)},
    "QAT": {"oil": (92, 41), "gold": (0, 0), "rubber": (0, 0), "gemstones": (0, 0)},
    "OMN": {"oil": (75, 32), "gold": (5, 2), "rubber": (0, 0), "gemstones": (3, 2)},
    "BHR": {"oil": (60, 28), "gold": (0, 0), "rubber": (0, 0), "gemstones": (0, 0)},
    "IRN": {"oil": (85, 36), "gold": (10, 4), "rubber": (0, 0), "gemstones": (15, 8)},
    # North America
    "USA": {"oil": (70, 30), "gold": (40, 18), "rubber": (0, 0), "gemstones": (15, 8)},
    "CAN": {"oil": (65, 28), "gold": (30, 14), "rubber": (0, 0), "gemstones": (20, 10)},
    "MEX": {"oil": (60, 26), "gold": (20, 10), "rubber": (0, 0), "gemstones": (10, 5)},
    # South America
    "BRA": {"oil": (30, 14), "gold": (35, 16), "rubber": (50, 22), "gemstones": (40, 20)},
    "VEN": {"oil": (80, 35), "gold": (15, 7), "rubber": (10, 5), "gemstones": (10, 5)},
    "COL": {"oil": (35, 16), "gold": (25, 12), "rubber": (5, 3), "gemstones": (55, 28)},
    "ECU": {"oil": (40, 18), "gold": (10, 5), "rubber": (5, 3), "gemstones": (5, 3)},
    "PER": {"oil": (10, 5), "gold": (30, 14), "rubber": (15, 8), "gemstones": (8, 4)},
    "BOL": {"oil": (15, 7), "gold": (10, 5), "rubber": (5, 3), "gemstones": (5, 3)},
    "CHL": {"oil": (5, 3), "gold": (20, 10), "rubber": (0, 0), "gemstones": (10, 5)},
    "ARG": {"oil": (25, 12), "gold": (15, 7), "rubber": (0, 0), "gemstones": (5, 3)},
    "GUY": {"oil": (40, 18), "gold": (15, 7), "rubber": (0, 0), "gemstones": (10, 5)},
    "SUR": {"oil": (0, 0), "gold": (20, 10), "rubber": (5, 3), "gemstones": (3, 2)},
    # Europe
    "GBR": {"oil": (40, 18), "gold": (5, 3), "rubber": (0, 0), "gemstones": (3, 2)},
    "NOR": {"oil": (50, 22), "gold": (3, 2), "rubber": (0, 0), "gemstones": (2, 1)},
    "RUS": {"oil": (75, 33), "gold": (45, 20), "rubber": (0, 0), "gemstones": (35, 18)},
    "NLD": {"oil": (15, 7), "gold": (0, 0), "rubber": (0, 0), "gemstones": (0, 0)},
    "DNK": {"oil": (20, 10), "gold": (0, 0), "rubber": (0, 0), "gemstones": (0, 0)},
    "ROU": {"oil": (20, 10), "gold": (5, 3), "rubber": (0, 0), "gemstones": (3, 2)},
    "POL": {"oil": (5, 3), "gold": (10, 5), "rubber": (0, 0), "gemstones": (8, 4)},
    "DEU": {"oil": (5, 3), "gold": (8, 4), "rubber": (0, 0), "gemstones": (3, 2)},
    # Africa
    "NGA": {"oil": (70, 30), "gold": (5, 3), "rubber": (15, 8), "gemstones": (3, 2)},
    "AGO": {"oil": (75, 33), "gold": (3, 2), "rubber": (0, 0), "gemstones": (30, 15)},
    "DZA": {"oil": (80, 35), "gold": (3, 2), "rubber": (0, 0), "gemstones": (3, 2)},
    "LBY": {"oil": (80, 35), "gold": (0, 0), "rubber": (0, 0), "gemstones": (0, 0)},
    "EGY": {"oil": (30, 14), "gold": (10, 5), "rubber": (0, 0), "gemstones": (5, 3)},
    "SDN": {"oil": (20, 10), "gold": (25, 12), "rubber": (0, 0), "gemstones": (5, 3)},
    "SSD": {"oil": (60, 28), "gold": (10, 5), "rubber": (0, 0), "gemstones": (5, 3)},
    "GAB": {"oil": (50, 22), "gold": (3, 2), "rubber": (10, 5), "gemstones": (3, 2)},
    "COG": {"oil": (45, 20), "gold": (3, 2), "rubber": (5, 3), "gemstones": (3, 2)},
    "COD": {"oil": (5, 3), "gold": (30, 14), "rubber": (20, 10), "gemstones": (50, 25)},
    "CMR": {"oil": (15, 7), "gold": (3, 2), "rubber": (10, 5), "gemstones": (3, 2)},
    "TCD": {"oil": (30, 14), "gold": (0, 0), "rubber": (0, 0), "gemstones": (3, 2)},
    "GHA": {"oil": (15, 7), "gold": (50, 22), "rubber": (10, 5), "gemstones": (10, 5)},
    "CIV": {"oil": (10, 5), "gold": (15, 7), "rubber": (25, 12), "gemstones": (5, 3)},
    "LBR": {"oil": (0, 0), "gold": (10, 5), "rubber": (30, 14), "gemstones": (15, 8)},
    "SLE": {"oil": (0, 0), "gold": (5, 3), "rubber": (5, 3), "gemstones": (50, 25)},
    "GIN": {"oil": (0, 0), "gold": (5, 3), "rubber": (10, 5), "gemstones": (30, 15)},
    "MLI": {"oil": (0, 0), "gold": (25, 12), "rubber": (0, 0), "gemstones": (3, 2)},
    "BFA": {"oil": (0, 0), "gold": (20, 10), "rubber": (0, 0), "gemstones": (3, 2)},
    "NER": {"oil": (5, 3), "gold": (10, 5), "rubber": (0, 0), "gemstones": (15, 8)},
    "TZA": {"oil": (0, 0), "gold": (15, 7), "rubber": (5, 3), "gemstones": (25, 12)},
    "ZAF": {"oil": (5, 3), "gold": (80, 35), "rubber": (0, 0), "gemstones": (60, 30)},
    "BWA": {"oil": (0, 0), "gold": (5, 3), "rubber": (0, 0), "gemstones": (70, 35)},
    "NAM": {"oil": (0, 0), "gold": (3, 2), "rubber": (0, 0), "gemstones": (55, 28)},
    "ZMB": {"oil": (0, 0), "gold": (5, 3), "rubber": (0, 0), "gemstones": (20, 10)},
    "ZWE": {"oil": (0, 0), "gold": (10, 5), "rubber": (0, 0), "gemstones": (25, 12)},
    "MOZ": {"oil": (10, 5), "gold": (5, 3), "rubber": (0, 0), "gemstones": (15, 8)},
    "MDG": {"oil": (0, 0), "gold": (5, 3), "rubber": (5, 3), "gemstones": (20, 10)},
    "MAR": {"oil": (0, 0), "gold": (3, 2), "rubber": (0, 0), "gemstones": (10, 5)},
    "ERI": {"oil": (0, 0), "gold": (10, 5), "rubber": (0, 0), "gemstones": (5, 3)},
    "ETH": {"oil": (0, 0), "gold": (15, 7), "rubber": (0, 0), "gemstones": (10, 5)},
    "KEN": {"oil": (0, 0), "gold": (10, 5), "rubber": (0, 0), "gemstones": (10, 5)},
    "UGA": {"oil": (15, 7), "gold": (5, 3), "rubber": (0, 0), "gemstones": (10, 5)},
    "MWI": {"oil": (0, 0), "gold": (3, 2), "rubber": (0, 0), "gemstones": (5, 3)},
    # Asia
    "CHN": {"oil": (30, 14), "gold": (40, 18), "rubber": (5, 3), "gemstones": (20, 10)},
    "IND": {"oil": (20, 10), "gold": (15, 7), "rubber": (15, 8), "gemstones": (30, 15)},
    "IDN": {"oil": (40, 18), "gold": (20, 10), "rubber": (60, 28), "gemstones": (15, 8)},
    "MYS": {"oil": (30, 14), "gold": (10, 5), "rubber": (55, 26), "gemstones": (3, 2)},
    "THA": {"oil": (15, 7), "gold": (10, 5), "rubber": (65, 30), "gemstones": (30, 15)},
    "VNM": {"oil": (20, 10), "gold": (5, 3), "rubber": (30, 14), "gemstones": (8, 4)},
    "PHL": {"oil": (5, 3), "gold": (20, 10), "rubber": (15, 8), "gemstones": (15, 8)},
    "MMR": {"oil": (10, 5), "gold": (15, 7), "rubber": (10, 5), "gemstones": (60, 30)},
    "LAO": {"oil": (0, 0), "gold": (10, 5), "rubber": (5, 3), "gemstones": (25, 12)},
    "KHM": {"oil": (5, 3), "gold": (3, 2), "rubber": (10, 5), "gemstones": (10, 5)},
    "PNG": {"oil": (5, 3), "gold": (35, 16), "rubber": (0, 0), "gemstones": (10, 5)},
    "KAZ": {"oil": (60, 26), "gold": (25, 12), "rubber": (0, 0), "gemstones": (15, 8)},
    "UZB": {"oil": (15, 7), "gold": (20, 10), "rubber": (0, 0), "gemstones": (10, 5)},
    "MNG": {"oil": (5, 3), "gold": (15, 7), "rubber": (0, 0), "gemstones": (20, 10)},
    "TUR": {"oil": (5, 3), "gold": (15, 7), "rubber": (0, 0), "gemstones": (15, 8)},
    # Oceania
    "AUS": {"oil": (20, 10), "gold": (60, 28), "rubber": (0, 0), "gemstones": (40, 20)},
    "NZL": {"oil": (5, 3), "gold": (10, 5), "rubber": (0, 0), "gemstones": (5, 3)},
    "FJI": {"oil": (0, 0), "gold": (5, 3), "rubber": (0, 0), "gemstones": (3, 2)},
    # Caribbean
    "TTO": {"oil": (40, 18), "gold": (0, 0), "rubber": (0, 0), "gemstones": (0, 0)},
    "CUB": {"oil": (10, 5), "gold": (5, 3), "rubber": (0, 0), "gemstones": (3, 2)},
    "JAM": {"oil": (0, 0), "gold": (5, 3), "rubber": (0, 0), "gemstones": (3, 2)},
    # Europe — minimal resources
    "FRA": {"oil": (5, 3), "gold": (5, 3), "rubber": (0, 0), "gemstones": (3, 2)},
    "ESP": {"oil": (3, 2), "gold": (10, 5), "rubber": (0, 0), "gemstones": (5, 3)},
    "PRT": {"oil": (0, 0), "gold": (5, 3), "rubber": (0, 0), "gemstones": (3, 2)},
    "ITA": {"oil": (8, 4), "gold": (5, 3), "rubber": (0, 0), "gemstones": (8, 4)},
    "GRC": {"oil": (3, 2), "gold": (8, 4), "rubber": (0, 0), "gemstones": (5, 3)},
    "BGR": {"oil": (3, 2), "gold": (5, 3), "rubber": (0, 0), "gemstones": (3, 2)},
    "HUN": {"oil": (5, 3), "gold": (3, 2), "rubber": (0, 0), "gemstones": (3, 2)},
    "SRB": {"oil": (5, 3), "gold": (5, 3), "rubber": (0, 0), "gemstones": (3, 2)},
    "HRV": {"oil": (8, 4), "gold": (3, 2), "rubber": (0, 0), "gemstones": (3, 2)},
    "UKR": {"oil": (10, 5), "gold": (5, 3), "rubber": (0, 0), "gemstones": (5, 3)},
    "BLR": {"oil": (5, 3), "gold": (3, 2), "rubber": (0, 0), "gemstones": (3, 2)},
    "CZE": {"oil": (3, 2), "gold": (5, 3), "rubber": (0, 0), "gemstones": (8, 4)},
    "SVK": {"oil": (3, 2), "gold": (3, 2), "rubber": (0, 0), "gemstones": (3, 2)},
    "AUT": {"oil": (5, 3), "gold": (3, 2), "rubber": (0, 0), "gemstones": (5, 3)},
    "CHE": {"oil": (0, 0), "gold": (3, 2), "rubber": (0, 0), "gemstones": (3, 2)},
    "SWE": {"oil": (0, 0), "gold": (15, 7), "rubber": (0, 0), "gemstones": (5, 3)},
    "FIN": {"oil": (0, 0), "gold": (10, 5), "rubber": (0, 0), "gemstones": (8, 4)},
    "GRL": {"oil": (10, 5), "gold": (5, 3), "rubber": (0, 0), "gemstones": (10, 5)},
}

# Resources that all countries have at least trace amounts of
RESOURCE_NAMES = ["oil", "gold", "rubber", "gemstones"]

def get_country_resources(iso):
    return RESOURCE_MAP.get(iso, {})

def main():
    data_dir = '/Users/vladyavdoshenko/CLionProjects/OpenDoctrines/data'
    with open(f'{data_dir}/provinces.json') as f:
        provs = json.load(f)

    result = {}
    for pid_str, entry in provs.items():
        pid = int(pid_str)
        iso = entry.get('iso_a3', '')
        rng = random.Random(pid * 777 + 333)

        base = get_country_resources(iso)
        resources = {}
        for name in RESOURCE_NAMES:
            if name in base:
                base_amt, boost = base[name]
            else:
                base_amt, boost = (0, 0)

            if base_amt == 0:
                # Small chance of trace deposits
                if rng.random() < 0.02:
                    amt = rng.uniform(0.5, 3)
                    boost_val = max(0.5, boost)
                else:
                    amt = 0
                    boost_val = 0
            else:
                # Random variation around base value
                dev = rng.uniform(-base_amt * 0.3, base_amt * 0.3)
                amt = max(0.5, base_amt + dev)
                # Boost scales with amount
                boost_val = boost * (amt / base_amt) * rng.uniform(0.85, 1.15)

            resources[name] = {
                "a": round(amt, 1),
                "b": round(boost_val, 1)
            }

        result[pid_str] = resources

    out_path = f'{data_dir}/resources.json'
    with open(out_path, 'w') as f:
        json.dump(result, f, separators=(',', ':'))
    print(f"Saved {out_path} ({len(result)} provinces)")

if __name__ == '__main__':
    main()
