#!/usr/bin/env python3
"""
Analyze provinces.png to map pixel province IDs to real province names.
Uses centroid positions to identify major cities.
"""
from PIL import Image
from collections import defaultdict

def province_id_from_color(r, g, b):
    """Reverse the color encoding: id = (r*256 + g)*256 + b"""
    return (r << 16) | (g << 8) | b

def analyze_country(img, provinces_json, iso_a3):
    """Analyze provinces for a specific country."""
    w, h = img.size
    pixels = img.load()

    # Get provinces for this country
    country_provs = {}
    for pid_str, entry in provinces_json.items():
        if entry.get('iso_a3') == iso_a3:
            pid = entry['id']
            name = entry.get('name', '')
            color = entry.get('color', '')
            country_provs[pid] = {'name': name, 'color': color}

    # Count pixels and collect coords per province
    prov_pixels = defaultdict(int)
    prov_coords = defaultdict(list)
    for y in range(h):
        for x in range(w):
            r, g, b, a = pixels[x, y]
            if a == 0:
                continue
            pid = province_id_from_color(r, g, b)
            if pid in country_provs:
                prov_pixels[pid] += 1
                prov_coords[pid].append((x, y))

    total_pixels = sum(prov_pixels.values())
    return country_provs, prov_pixels, prov_coords, total_pixels, w, h

def print_analysis(iso_a3, country_provs, prov_pixels, prov_coords, total_pixels, w, h):
    print(f"\n=== {iso_a3} Provinces ({len(country_provs)} pixel provs, {total_pixels} pixels) ===")
    print(f"{'Pixel ProvID':>14} | {'Name':>30} | {'Pixels':>8} | {'%':>6} | {'CX_norm':>8} | {'CY_norm':>8}")
    print("-" * 90)

    sorted_provs = sorted(prov_pixels.items(), key=lambda x: -x[1])
    for pid, px_count in sorted_provs:
        name = country_provs[pid]['name']
        pct = px_count / total_pixels * 100
        coords = prov_coords[pid]
        cx = sum(c[0] for c in coords) / len(coords)
        cy = sum(c[1] for c in coords) / len(coords)
        cx_n = cx / w
        cy_n = cy / h  # 0=top, 1=bottom (image coords)
        print(f"{pid:>14} | {name:>30} | {px_count:>8} | {pct:>5.1f}% | {cx_n:>8.3f} | {cy_n:>8.3f}")

def main():
    import json

    data_dir = '/Users/vladyavdoshenko/CLionProjects/OpenDoctrines/data'

    # Load provinces.json
    with open(f'{data_dir}/provinces.json') as f:
        provinces_json = json.load(f)

    # Read provinces.png
    img = Image.open(f'{data_dir}/provinces.png')
    print(f"Image: {img.size[0]}x{img.size[1]}")

    # Turkey: Istanbul is at the north, western part of Turkey
    # Anatolia (central) fills most of the country
    # Let's look at centroid Y to identify north (low Y) vs south (high Y)
    print("\n\n===== TURKEY =====")
    print("Istanbul is in the north-west of Turkey (low Y, low X)")
    print("Ankara is central (around Y=0.30)")
    cp, pp, pc, tp, w, h = analyze_country(img, provinces_json, 'TUR')
    print_analysis('TUR', cp, pp, pc, tp, w, h)

    # UK: London is in the south-east of England
    print("\n\n===== UNITED KINGDOM =====")
    print("London is south-east England (low Y, high X in England)")
    print("Edinburgh is in Scotland (north)")
    cp, pp, pc, tp, w, h = analyze_country(img, provinces_json, 'GBR')
    print_analysis('GBR', cp, pp, pc, tp, w, h)

    # Germany: Berlin is in the north-east
    print("\n\n===== GERMANY =====")
    print("Berlin is NE Germany (low Y, high X)")
    print("Munich is SE Germany (high Y, low X)")
    cp, pp, pc, tp, w, h = analyze_country(img, provinces_json, 'DEU')
    print_analysis('DEU', cp, pp, pc, tp, w, h)

    # France: Paris is in the north-center
    print("\n\n===== FRANCE =====")
    print("Paris is north-center France")
    cp, pp, pc, tp, w, h = analyze_country(img, provinces_json, 'FRA')
    if cp:
        print_analysis('FRA', cp, pp, pc, tp, w, h)
    else:
        # Check iso_a3 for France
        print("No FRA found, checking...")
        for pid_str, entry in provinces_json.items():
            if 'France' in entry.get('name', ''):
                print(f"  {entry.get('iso_a3')}: {entry.get('name')}")
                break

    # Italy: Rome is central-south
    print("\n\n===== ITALY =====")
    cp, pp, pc, tp, w, h = analyze_country(img, provinces_json, 'ITA')
    print_analysis('ITA', cp, pp, pc, tp, w, h)

    # Spain: Madrid is center
    print("\n\n===== SPAIN =====")
    cp, pp, pc, tp, w, h = analyze_country(img, provinces_json, 'ESP')
    print_analysis('ESP', cp, pp, pc, tp, w, h)

    # Poland: Warsaw is center-east
    print("\n\n===== POLAND =====")
    cp, pp, pc, tp, w, h = analyze_country(img, provinces_json, 'POL')
    print_analysis('POL', cp, pp, pc, tp, w, h)

    # USA: New York east coast, LA west coast
    print("\n\n===== USA =====")
    cp, pp, pc, tp, w, h = analyze_country(img, provinces_json, 'USA')
    print_analysis('USA', cp, pp, pc, tp, w, h)

    # Japan: Tokyo is east-central
    print("\n\n===== JAPAN =====")
    cp, pp, pc, tp, w, h = analyze_country(img, provinces_json, 'JPN')
    print_analysis('JPN', cp, pp, pc, tp, w, h)

    # China: Beijing north, Shanghai east coast
    print("\n\n===== CHINA =====")
    cp, pp, pc, tp, w, h = analyze_country(img, provinces_json, 'CHN')
    print_analysis('CHN', cp, pp, pc, tp, w, h)

    # India: Delhi north, Mumbai west
    print("\n\n===== INDIA =====")
    cp, pp, pc, tp, w, h = analyze_country(img, provinces_json, 'IND')
    print_analysis('IND', cp, pp, pc, tp, w, h)

    # Russia: Moscow west, many provinces
    print("\n\n===== RUSSIA =====")
    cp, pp, pc, tp, w, h = analyze_country(img, provinces_json, 'RUS')
    print_analysis('RUS', cp, pp, pc, tp, w, h)

if __name__ == '__main__':
    main()