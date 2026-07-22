#!/usr/bin/env python3
"""
Generate ship placements for countries with significant navies.

Ships are placed only in oceans and seas (not lakes/rivers), 
near coastal provinces of countries with large naval capacity.
"""

import json, os, sys, math, random
import numpy as np
from PIL import Image

# ─── Paths ──────────────────────────────────────────────────────────
DATA_DIR = "data"
LAND_SEA_PNG = os.path.join(DATA_DIR, "land_sea.png")
PROVINCES_PNG = os.path.join(DATA_DIR, "provinces.png")
PROVINCES_JSON = os.path.join(DATA_DIR, "provinces.json")
COUNTRIES_JSON = os.path.join(DATA_DIR, "countries.json")
PORTS_JSON = os.path.join(DATA_DIR, "ports.json")
OUT_SHIPS = os.path.join(DATA_DIR, "ships.json")

MAP_W = 8192
MAP_H = 4096

random.seed(42)

# ─── Countries with significant navies ───────────────────────────────
# Format: ISO_A3: (num_ships, ship_types_distribution)
# Ship types: "carrier", "battleship", "destroyer", "frigate", "submarine"
NAVY_COUNTRIES = {
    "USA": (50, {"carrier": 3, "battleship": 5, "destroyer": 15, "frigate": 20, "submarine": 7}),
    "CHN": (45, {"carrier": 2, "battleship": 4, "destroyer": 18, "frigate": 15, "submarine": 6}),
    "RUS": (40, {"carrier": 1, "battleship": 3, "destroyer": 12, "frigate": 15, "submarine": 9}),
    "GBR": (25, {"carrier": 2, "battleship": 2, "destroyer": 8, "frigate": 10, "submarine": 3}),
    "JPN": (30, {"carrier": 2, "battleship": 2, "destroyer": 12, "frigate": 12, "submarine": 2}),
    "FRA": (20, {"carrier": 1, "battleship": 1, "destroyer": 8, "frigate": 8, "submarine": 2}),
    "IND": (25, {"carrier": 1, "battleship": 1, "destroyer": 10, "frigate": 10, "submarine": 3}),
    "KOR": (15, {"carrier": 0, "battleship": 2, "destroyer": 6, "frigate": 5, "submarine": 2}),
    "ITA": (15, {"carrier": 1, "battleship": 1, "destroyer": 5, "frigate": 6, "submarine": 2}),
    "ESP": (12, {"carrier": 1, "battleship": 0, "destroyer": 4, "frigate": 5, "submarine": 2}),
    "AUS": (12, {"carrier": 1, "battleship": 0, "destroyer": 4, "frigate": 5, "submarine": 2}),
    "CAN": (15, {"carrier": 0, "battleship": 0, "destroyer": 6, "frigate": 7, "submarine": 2}),
    "TUR": (12, {"carrier": 0, "battleship": 0, "destroyer": 4, "frigate": 6, "submarine": 2}),
    "BRA": (10, {"carrier": 0, "battleship": 1, "destroyer": 3, "frigate": 4, "submarine": 2}),
    "DEU": (8, {"carrier": 0, "battleship": 0, "destroyer": 3, "frigate": 4, "submarine": 1}),
    "NLD": (6, {"carrier": 0, "battleship": 0, "destroyer": 2, "frigate": 3, "submarine": 1}),
    "SWE": (6, {"carrier": 0, "battleship": 0, "destroyer": 2, "frigate": 3, "submarine": 1}),
}

# ─── Coordinate helpers ─────────────────────────────────────────────
def pixel_to_lonlat(px, py):
    return (px / MAP_W) * 360.0 - 180.0, 90.0 - (py / MAP_H) * 180.0

def lonlat_to_pixel(lon, lat):
    return int((lon + 180.0) / 360.0 * MAP_W), int((90.0 - lat) / 180.0 * MAP_H)

# ─── Ocean detection ───────────────────────────────────────────────
def load_land_sea():
    """Load land_sea.png and return as numpy array."""
    img = Image.open(LAND_SEA_PNG)
    img = img.convert('L')  # Grayscale
    return np.array(img)

def is_ocean_pixel(land_sea, x, y):
    """Check if pixel is ocean (not land)."""
    if x < 0 or x >= MAP_W or y < 0 or y >= MAP_H:
        return False
    return land_sea[y, x] == 0  # 0 = water in land_sea.png

def is_connected_to_ocean(land_sea, x, y, max_distance=50):
    """
    Check if a water pixel is connected to the ocean (not just a lake).
    Ocean is defined as water that reaches the edge of the map or is very large.
    """
    if not is_ocean_pixel(land_sea, x, y):
        return False
    
    # Check if water reaches map edge (definitely ocean)
    if x <= 5 or x >= MAP_W - 5 or y <= 5 or y >= MAP_H - 5:
        return True
    
    # BFS to check water body size
    visited = set()
    queue = [(x, y)]
    water_pixels = 0
    
    while queue and water_pixels < 500:  # If water body is >500 pixels, consider it ocean
        cx, cy = queue.pop(0)
        if (cx, cy) in visited:
            continue
        visited.add((cx, cy))
        water_pixels += 1
        
        # Check 8 neighbors
        for dx in [-1, 0, 1]:
            for dy in [-1, 0, 1]:
                if dx == 0 and dy == 0:
                    continue
                nx, ny = cx + dx, cy + dy
                if 0 <= nx < MAP_W and 0 <= ny < MAP_H:
                    if is_ocean_pixel(land_sea, nx, ny) and (nx, ny) not in visited:
                        queue.append((nx, ny))
    
    return water_pixels >= 500

def find_nearest_ocean_pixel(land_sea, start_x, start_y, max_radius=100):
    """
    Find the nearest ocean pixel to a starting point.
    Returns (x, y) or None if no ocean found within radius.
    """
    for radius in range(1, max_radius + 1):
        for angle in range(0, 360, 5):  # Check every 5 degrees
            rad = math.radians(angle)
            x = int(start_x + radius * math.cos(rad))
            y = int(start_y + radius * math.sin(rad))
            if 0 <= x < MAP_W and 0 <= y < MAP_H:
                if is_ocean_pixel(land_sea, x, y) and is_connected_to_ocean(land_sea, x, y):
                    return x, y
    return None

# ─── Data loading ───────────────────────────────────────────────────
def load_provinces():
    with open(PROVINCES_JSON, 'r') as f:
        return json.load(f)

def load_countries():
    with open(COUNTRIES_JSON, 'r') as f:
        return json.load(f)

def load_ports():
    with open(PORTS_JSON, 'r') as f:
        return json.load(f)

def compute_province_centers():
    """Compute mean pixel center for each province using numpy."""
    img = Image.open(PROVINCES_PNG).convert("RGB")
    arr = np.array(img, dtype=np.uint32)
    h, w, _ = arr.shape
    
    # Encode RGB -> province ID
    ids = arr[:,:,0].astype(np.uint32) << 16 | arr[:,:,1].astype(np.uint32) << 8 | arr[:,:,2].astype(np.uint32)
    mask = ids != 0
    
    # Get unique province IDs
    unique_ids = np.unique(ids[mask])
    
    centers = {}
    for pid in unique_ids:
        y_coords, x_coords = np.where(ids == pid)
        if len(x_coords) > 0:
            cx = int(np.mean(x_coords))
            cy = int(np.mean(y_coords))
            centers[pid] = {"x": cx, "y": cy}
    
    return centers

def get_province_center(province_centers, pid):
    """Get center of a province from precomputed centers."""
    return province_centers.get(pid)

# ─── Ship generation ─────────────────────────────────────────────────
def generate_ships(land_sea, provinces, countries, ports, province_centers):
    ships = []
    
    # Build ISO to country ID mapping
    iso_to_cid = {}
    for cid, cdata in countries.items():
        iso = cdata.get("iso_a3")
        if iso:
            iso_to_cid[iso] = int(cid)
    
    # Process each navy country
    for iso, (num_ships, ship_types) in NAVY_COUNTRIES.items():
        if iso not in iso_to_cid:
            print(f"Warning: Country {iso} not found in countries.json")
            continue
        
        country_id = iso_to_cid[iso]
        
        # Find port provinces for this country
        country_ports = []
        for pid_str, port_data in ports.items():
            pid = int(pid_str)
            prov = provinces.get(str(pid))
            if prov and prov.get("country_id") == country_id:
                country_ports.append(pid)
        
        if not country_ports:
            print(f"Warning: No ports found for {iso}")
            continue
        
        print(f"Generating {num_ships} ships for {iso} ({len(country_ports)} ports)")
        
        # Build ship type list for this country
        type_list = []
        for ship_type, count in ship_types.items():
            type_list.extend([ship_type] * count)
        random.shuffle(type_list)
        
        # Distribute ships among ports
        ships_generated = 0
        port_index = 0
        
        while ships_generated < num_ships and port_index < len(country_ports):
            port_pid = country_ports[port_index]
            port_index += 1
            
            prov = provinces.get(str(port_pid))
            if not prov:
                continue
            
            # Get province center
            center = get_province_center(province_centers, port_pid)
            if not center:
                print(f"Warning: No center found for port province {port_pid}")
                continue
            
            center_x = center["x"]
            center_y = center["y"]
            
            # Find nearest ocean
            ocean_pos = find_nearest_ocean_pixel(land_sea, center_x, center_y, max_radius=200)
            if not ocean_pos:
                print(f"Warning: No ocean found near port {port_pid} for {iso}")
                continue
            
            ocean_x, ocean_y = ocean_pos
            
            # Generate 1-3 ships per port (depending on remaining ships)
            port_ships = min(random.randint(1, 3), num_ships - ships_generated)
            
            for i in range(port_ships):
                if ships_generated >= num_ships or not type_list:
                    break
                
                ship_type = type_list.pop()
                if not type_list:  # Replenish if empty
                    for st, count in ship_types.items():
                        type_list.extend([st] * count)
                    random.shuffle(type_list)
                
                # Add some randomness to position
                offset_x = random.randint(-50, 50)
                offset_y = random.randint(-50, 50)
                final_x = max(0, min(MAP_W - 1, ocean_x + offset_x))
                final_y = max(0, min(MAP_H - 1, ocean_y + offset_y))
                
                # Verify final position is still ocean
                if not (is_ocean_pixel(land_sea, final_x, final_y) and 
                        is_connected_to_ocean(land_sea, final_x, final_y)):
                    continue
                
                final_lon, final_lat = pixel_to_lonlat(final_x, final_y)
                
                ship = {
                    "country_id": country_id,
                    "type": ship_type,
                    "lat": round(final_lat, 6),
                    "lon": round(final_lon, 6),
                    "health": 100,
                    "crew": random.randint(50, 300)
                }
                ships.append(ship)
                ships_generated += 1
    
    return ships

# ─── Main ─────────────────────────────────────────────────────────
def main():
    print("Loading data...")
    land_sea = load_land_sea()
    provinces = load_provinces()
    countries = load_countries()
    ports = load_ports()
    
    print("Computing province centers...")
    province_centers = compute_province_centers()
    
    print("Generating ships...")
    ships = generate_ships(land_sea, provinces, countries, ports, province_centers)
    
    print(f"Generated {len(ships)} ships")
    
    with open(OUT_SHIPS, 'w') as f:
        json.dump(ships, f, indent=2)
    
    print(f"Saved to {OUT_SHIPS}")

if __name__ == "__main__":
    main()
