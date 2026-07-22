#!/usr/bin/env python3
"""Fast ship generation using bounding-box ocean check instead of BFS flood fill."""
import json, os, math, random
import numpy as np
from PIL import Image

DATA_DIR = "data"
PROVINCES_JSON = os.path.join(DATA_DIR, "provinces.json")
COUNTRIES_JSON = os.path.join(DATA_DIR, "countries.json")
PORTS_JSON = os.path.join(DATA_DIR, "ports.json")
PROVINCES_PNG = os.path.join(DATA_DIR, "provinces.png")
LAND_SEA_PNG = os.path.join(DATA_DIR, "land_sea.png")
OUT_SHIPS = os.path.join(DATA_DIR, "ships.json")
MAP_W, MAP_H = 8192, 4096
random.seed(42)

NAVY_ENTRIES = [
    (5, 25, {"carrier": 2, "destroyer": 12, "boat": 11}),      # USA
    (23, 20, {"carrier": 1, "destroyer": 10, "boat": 9}),       # China
    (3, 15, {"carrier": 1, "destroyer": 7, "boat": 7}),        # Russia
    (10, 15, {"carrier": 1, "destroyer": 7, "boat": 7}),       # UK
    (47, 10, {"carrier": 1, "destroyer": 3, "boat": 6}),       # Japan (trimmed from 15)
    (27, 12, {"carrier": 1, "destroyer": 5, "boat": 6}),       # France
    (79, 15, {"carrier": 1, "destroyer": 7, "boat": 7}),       # India
    (68, 8, {"destroyer": 3, "boat": 5}),                       # S. Korea
    (40, 8, {"destroyer": 3, "boat": 5}),                       # Italy
    (51, 6, {"destroyer": 2, "boat": 4}),                       # Spain
    (215, 8, {"destroyer": 3, "boat": 5}),                      # Australia
    (2, 6, {"destroyer": 2, "boat": 4}),                        # Canada
    (62, 6, {"destroyer": 2, "boat": 4}),                       # Turkey
    (190, 6, {"destroyer": 2, "boat": 4}),                      # Brazil
    (19, 6, {"destroyer": 2, "boat": 4}),                       # Germany
    (22, 3, {"destroyer": 1, "boat": 2}),                       # Netherlands (trimmed from 5)
    (7, 5, {"destroyer": 2, "boat": 3}),                        # Sweden
]

def lonlat_to_pixel(lon, lat):
    return int((lon + 180.0) / 360.0 * MAP_W) % MAP_W, int((90.0 - lat) / 180.0 * MAP_H)

def pixel_to_lonlat(px, py):
    return (px / MAP_W) * 360.0 - 180.0, 90.0 - (py / MAP_H) * 180.0

def main():
    print("Loading province data...")
    with open(PROVINCES_JSON) as f:
        provinces = json.load(f)
    with open(COUNTRIES_JSON) as f:
        countries = json.load(f)
    try:
        with open(PORTS_JSON) as f:
            ports = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        print(f"  {PORTS_JSON} not found, generating empty ships.json")
        with open(SHIPS_JSON, "w") as f:
            json.dump({}, f)
        print(f"Saved {SHIPS_JSON} (empty)")
        return

    iso_to_cid = {}
    for cid_str, cdata in countries.items():
        iso = cdata.get("iso_a3")
        if iso:
            iso_to_cid[iso] = int(cid_str)

    # Build pid -> center
    print("Computing province centers...")
    img = Image.open(PROVINCES_PNG).convert("RGB")
    arr = np.asarray(img, dtype=np.uint32)
    h, w, _ = arr.shape
    ids = arr[:,:,0].astype(np.uint32) << 16 | arr[:,:,1].astype(np.uint32) << 8 | arr[:,:,2].astype(np.uint32)
    
    # Fast center computation using bincount
    flat_ids = ids.ravel()
    mask = flat_ids != 0
    valid_ids = flat_ids[mask]
    y_coords = np.arange(h).repeat(w)
    x_coords = np.tile(np.arange(w), h)
    valid_y = y_coords[mask]
    valid_x = x_coords[mask]
    
    max_id = int(valid_ids.max()) if len(valid_ids) > 0 else 0
    sum_x = np.bincount(valid_ids, weights=valid_x, minlength=max_id+1)
    sum_y = np.bincount(valid_ids, weights=valid_y, minlength=max_id+1)
    counts = np.bincount(valid_ids, minlength=max_id+1)
    
    centers = {}
    for pid in range(max_id+1):
        if counts[pid] > 0:
            centers[pid] = (int(sum_x[pid] / counts[pid]), int(sum_y[pid] / counts[pid]))
    print(f"  Computed {len(centers)} centers")

    # Load images as numpy arrays for fast access
    ls_img = Image.open(LAND_SEA_PNG).convert("L")
    ls_arr = np.asarray(ls_img, dtype=np.uint8)
    
    prov_img = Image.open(PROVINCES_PNG).convert("RGB")
    prov_arr = np.asarray(prov_img, dtype=np.uint8)
    pw, ph = prov_img.size

    # Pre-compute ocean mask: land_sea=0 AND provinces=black (0,0,0)
    is_ocean = (ls_arr == 0) & (prov_arr[:,:,0] == 0) & (prov_arr[:,:,1] == 0) & (prov_arr[:,:,2] == 0)

    ships = []
    MIN_SHIP_SPACING = 25  # minimum pixels between ships (prevents overlap on map)

    def too_close_to_ship(px, py, ship_list, min_dist=MIN_SHIP_SPACING):
        for s in ship_list:
            sx, sy = lonlat_to_pixel(s["lon"], s["lat"])
            if abs(sx - px) < min_dist and abs(sy - py) < min_dist:
                return True
        return False

    def is_large_water_fast(ox, oy, radius=100):
        """Check if ocean pixel is in large open water using numpy slicing."""
        step = 4
        y_min = max(0, oy - radius)
        y_max = min(ph, oy + radius + 1)
        x_indices = [(ox + dx) % pw for dx in range(-radius, radius + 1, step)]
        ocean_count = 0
        total_count = 0
        for dy in range(-radius, radius + 1, step):
            ny = oy + dy
            if 0 <= ny < ph:
                total_count += len(x_indices)
                ocean_count += int(np.sum(is_ocean[ny, x_indices]))
        return ocean_count / max(total_count, 1) > 0.3

    def nearest_ocean_fast(cx, cy, max_radius=200):
        """Find nearest ocean pixel using numpy."""
        for r in range(1, max_radius + 1):
            # Check perimeter of square at radius r
            for dy in range(-r, r + 1):
                for dx in (-r, r):
                    nx = (cx + dx) % pw
                    ny = cy + dy
                    if 0 <= ny < ph and is_ocean[ny, nx]:
                        return nx, ny
            for dx in range(-r + 1, r):
                for dy in (-r, r):
                    nx = (cx + dx) % pw
                    ny = cy + dy
                    if 0 <= ny < ph and is_ocean[ny, nx]:
                        return nx, ny
        return None

    for cid, num_ships, ship_types in NAVY_ENTRIES:
        country_ports = []
        for pid_str, port_data in ports.items():
            pid = int(pid_str)
            p = provinces.get(pid_str, {})
            if p.get("country_id") == cid:
                country_ports.append(pid)
        if not country_ports:
            continue
        cname = str(cid)
        for k, v in countries.items():
            if int(k) == cid:
                cname = v.get('name', str(cid))
                break
        print(f"  {cname}: {num_ships} ships, {len(country_ports)} ports")
        type_list = []
        for ship_type, count in ship_types.items():
            type_list.extend([ship_type] * count)
        random.shuffle(type_list)
        generated = 0
        port_idx = 0
        max_rounds = 20
        for _ in range(max_rounds * len(country_ports)):
            if generated >= num_ships or not country_ports:
                break
            pid = country_ports[port_idx % len(country_ports)]
            port_idx += 1
            center = centers.get(pid)
            if not center:
                continue
            cx, cy = center
            # Find a deep ocean pixel at least 60px from coast
            ocean_pos = nearest_ocean_fast(cx, cy, 200)
            if not ocean_pos:
                continue
            ocean_x, ocean_y = ocean_pos
            if not is_large_water_fast(ocean_x, ocean_y):
                continue
            if generated >= num_ships or not type_list:
                break
            st = type_list.pop()
            if not type_list:
                for st2, count2 in ship_types.items():
                    type_list.extend([st2] * count2)
                random.shuffle(type_list)
            placed = False
            for attempt in range(40):
                # Offset further from coast: 60-200px in random direction
                ox = random.randint(-200, 200)
                oy = random.randint(-200, 200)
                if abs(ox) < 60 and abs(oy) < 60:
                    continue  # too close to coast
                fx = max(0, min(pw - 1, ocean_x + ox))
                fy = max(0, min(ph - 1, ocean_y + oy))
                if not is_ocean[fy, fx]:
                    continue
                if too_close_to_ship(fx, fy, ships):
                    continue
                flon, flat = pixel_to_lonlat(fx, fy)
                crew = random.randint(300, 1500)
                ship = {
                    "country_id": cid,
                    "type": st,
                    "lat": round(flat, 6),
                    "lon": round(flon, 6),
                    "health": 100,
                    "crew": crew,
                }
                ships.append(ship)
                generated += 1
                placed = True
                break
            if not placed:
                # Fallback: place near coast but still check spacing
                for attempt in range(20):
                    ox = random.randint(-20, 20)
                    oy = random.randint(-20, 20)
                    fx = max(0, min(pw - 1, ocean_x + ox))
                    fy = max(0, min(ph - 1, ocean_y + oy))
                    if not is_ocean[fy, fx]:
                        continue
                    if too_close_to_ship(fx, fy, ships):
                        continue
                    flon, flat = pixel_to_lonlat(fx, fy)
                    crew = random.randint(300, 1500)
                    ship = {
                        "country_id": cid,
                        "type": st,
                        "lat": round(flat, 6),
                        "lon": round(flon, 6),
                        "health": 100,
                        "crew": crew,
                    }
                    ships.append(ship)
                    generated += 1
                    break

    print(f"Total ships generated: {len(ships)}")
    with open(OUT_SHIPS, "w") as f:
        json.dump(ships, f, indent=2, separators=(",", ":"))
    print(f"Wrote {OUT_SHIPS}")

if __name__ == "__main__":
    main()
