#!/usr/bin/env python3
"""
Overlay real-world geographic datasets onto the province map.

Integrates:
  - GREG: ethnic group polygons → minorities.json
  - USGS Major Mineral Deposits: gold & gemstone deposits → resources.json
  - ACOR: oil/gas field polygons → resources.json (oil)
  - Country-level data for rubber

Usage:
  pip install shapely pyshp pillow numpy
  python tools/overlay_real_data.py
"""

import json, os, sys, math, hashlib, random
from collections import defaultdict
import numpy as np
from PIL import Image
import shapefile
from shapely.geometry import Point, Polygon as ShapelyPolygon, shape as shapely_shape
from shapely import prepared

# ─── Paths ──────────────────────────────────────────────────────────
DATA_DIR = "data"
PROVINCES_PNG = os.path.join(DATA_DIR, "provinces.png")
PROVINCES_JSON = os.path.join(DATA_DIR, "provinces.json")
COUNTRIES_JSON = os.path.join(DATA_DIR, "countries.json")
OUT_MINORITIES = os.path.join(DATA_DIR, "minorities.json")
OUT_MINORITY_COLORS = os.path.join(DATA_DIR, "minority_colors.json")
OUT_RESOURCES = os.path.join(DATA_DIR, "resources.json")
OUT_ARMIES = os.path.join(DATA_DIR, "armies.json")
OUT_PORTS = os.path.join(DATA_DIR, "ports.json")
OUT_SHIPS = os.path.join(DATA_DIR, "ships.json")

GREG_SHP = "/tmp/greg_data/GREG.shp"
USGS_SHP = "/tmp/usgs_data/ofr20051294.shp"
ACOR_SHP = "/tmp/acor_data/acor_0.2.shp"

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

# ─── GREG: Ethnic groups ────────────────────────────────────────────
def load_greg():
    """Load GREG polygons, return list of (group_name, prepared_polygon)."""
    sf = shapefile.Reader(GREG_SHP, encoding="latin-1")
    fields = [f[0] for f in sf.fields[1:]]
    groups = []
    for i in range(sf.numRecords):
        rec = sf.record(i)
        d = dict(zip(fields, rec))
        # Use the most specific group name available
        gname = (d.get("G3SHORTNAM") or "").strip()
        if not gname:
            gname = (d.get("G2SHORTNAM") or "").strip()
        if not gname:
            gname = (d.get("G1SHORTNAM") or "").strip()
        if not gname:
            continue
        # Read geometry
        shp = sf.shape(i)
        if shp.shapeType != 5:  # POLYGON
            continue
        # Build shapely polygon from shapefile parts
        parts = []
        for pi in range(len(shp.parts)):
            start = shp.parts[pi]
            end = shp.parts[pi + 1] if pi + 1 < len(shp.parts) else len(shp.points)
            ring = shp.points[start:end]
            if len(ring) >= 3:
                # WGS84 coords are (lon, lat)
                parts.append(ring)
        if not parts:
            continue
        # Build polygon (outer ring + holes)
        poly = ShapelyPolygon(parts[0], parts[1:] if len(parts) > 1 else None)
        if poly.is_valid and not poly.is_empty:
            groups.append((gname, prepared.prep(poly)))
    print(f"  Loaded {len(groups)} GREG ethnic group polygons")
    return groups

# Canonical name mapping: GREG variant -> standardized name
NAME_CANON = {
    # Same group, variant names
    "Byelorussians": "Belarusians",
    "Poles": "Polish",
    "Gypsies": "Romani",
    "Tsigani": "Romani",
    "Gitanos": "Romani",
    "Rom": "Romani",
    "Azerbaijanians": "Azerbaijanis",
    "Kirghiz": "Kyrgyz",
    "Kirghis": "Kyrgyz",
    "Khalka": "Mongols",
    "Moldavians": "Moldovans",
    "Bosnian Muslims": "Bosniaks",
    "Ruthenians": "Ukrainians",
    "Little Russians": "Ukrainians",
    "Great Russians": "Russians",
    "Rumanians": "Romanians",
    "Scotsmen": "Scottish",
    "Irishmen": "Irish",
    "Englishmen": "English",
    "Dutchmen": "Dutch",
    "Turks": "Turkish",
    "Turkomans": "Turkmens",
    "Letts": "Latvians",
    "Uzbeks of Afghanistan": "Uzbeks",
    "Uighurs": "Uyghur",
    "Mashona": "Shona",
    "Matebele": "Ndebele",
    "Fula": "Fulani",
    "Fulbe": "Fulani",
    "Somali": "Somalis",
    "Siamese": "Thai",
    "Xosa": "Xhosa",
    # Arabic group variants
    "Syria Arabs": "Arabs",
    "Arabs of Yemen": "Arabs",
    "West Sahara Arabs": "Arabs",
    "Arabs of Libya": "Arabs",
    "Arabs of Sudan": "Arabs",
    "Sudan Arabs": "Arabs",
    "Lebanon Arabs": "Arabs",
    "Iran Arabs": "Arabs",
    "Shoa-Arabs": "Arabs",
    # Somali variants
    "Somalis of Ogaden": "Somalis",
    "Somalis (Issa)": "Somalis",
    "Somali (Gadabursi)": "Somalis",
    # Chinese group variants
    "Chinese (Han)": "Han Chinese",
    "Chuang": "Zhuang",
    "Mongols of Chinese Peoples' Republic": "Mongols",
    # Other
    "Pashai": "Pashtun",
    "Various": "Unknown",
}

def canonicalize_names(minority_data):
    """Merge variant ethnic group names into canonical forms."""
    canon_count = 0
    for pid_str, entries in minority_data.items():
        merged = {}
        for e in entries:
            name = NAME_CANON.get(e["n"], e["n"])
            merged[name] = merged.get(name, 0.0) + e["p"]
        merged_list = [{"n": n, "p": round(p, 1)} for n, p in merged.items()]
        diff = 100.0 - sum(e["p"] for e in merged_list)
        if merged_list and abs(diff) > 0.01:
            merged_list[-1]["p"] = round(merged_list[-1]["p"] + diff, 1)
        if len(merged_list) < len(entries):
            canon_count += 1
        minority_data[pid_str] = merged_list
    print(f"  Canonicalized names in {canon_count} provinces")
    return minority_data

def assign_ethnic_groups(province_centers, greg_groups):
    """For each province center, test which GREG polygons contain it."""
    print("  Assigning ethnic groups to provinces...")
    province_groups = defaultdict(list)
    total = len(province_centers)
    for idx, (pid, pc) in enumerate(province_centers.items()):
        pt = Point(pc["lon"], pc["lat"])
        for gname, prep_poly in greg_groups:
            if prep_poly.contains(pt):
                province_groups[pid].append(gname)
        if (idx + 1) % 200 == 0:
            print(f"    {idx+1}/{total}")
    # Build ethnic composition per province
    # If multiple groups overlap, split evenly
    # If no groups found, mark as "Unknown"
    minority_data = {}
    for pid in province_centers:
        groups = province_groups.get(pid, ["Unknown"])
        total_pct = 100.0 / len(groups)
        entries = [{"n": g, "p": round(total_pct, 1)} for g in groups]
        # Normalize to exactly 100
        diff = 100.0 - sum(e["p"] for e in entries)
        if entries and diff != 0:
            entries[-1]["p"] = round(entries[-1]["p"] + diff, 1)
        minority_data[str(pid)] = entries
    # Canonicalize names to merge variants
    minority_data = canonicalize_names(minority_data)
    print(f"  Assigned ethnic groups to {len(minority_data)} provinces")
    return minority_data

# Country-level ethnic composition for secondary groups
# (name, base_pct) per ISO code — used to add secondary groups
COUNTRY_ETHNIC = {
    "UKR": [("Ukrainians", 78), ("Russians", 17), ("Belarusians", 1), ("Moldovans", 1), ("Crimean Tatars", 1)],
    "RUS": [("Russians", 81), ("Tatars", 4), ("Ukrainians", 1), ("Chuvash", 1), ("Bashkirs", 1), ("Chechens", 1)],
    "USA": [("White Americans", 60), ("Hispanic Americans", 19), ("African Americans", 12), ("Asian Americans", 6)],
    "CHN": [("Han Chinese", 92), ("Zhuang", 1), ("Hui", 1), ("Manchu", 1), ("Uyghur", 1)],
    "IND": [("Indo-Aryan", 72), ("Dravidian", 25)],
    "CAN": [("White Canadians", 70), ("Asian Canadians", 16), ("Indigenous Canadians", 5)],
    "GBR": [("English", 84), ("Scottish", 8), ("Welsh", 5), ("Northern Irish", 3)],
    "FRA": [("French", 87), ("North African", 6), ("Sub-Saharan African", 3)],
    "DEU": [("Germans", 88), ("Turkish", 3), ("Polish", 1)],
    "AUS": [("White Australians", 76), ("Asian Australians", 12), ("Indigenous Australians", 3)],
    "BRA": [("White Brazilians", 48), ("Mixed-race Brazilians", 43), ("Black Brazilians", 8)],
    "ZAF": [("Black South Africans", 81), ("White South Africans", 8), ("Coloured", 9)],
    "KAZ": [("Kazakhs", 68), ("Russians", 20), ("Uzbeks", 3)],
    "BLR": [("Belarusians", 84), ("Russians", 8), ("Polish", 3)],
    "BGR": [("Bulgarians", 85), ("Turkish", 9), ("Romani", 4)],
    "ROU": [("Romanians", 89), ("Hungarians", 6), ("Romani", 3)],
    "SRB": [("Serbs", 83), ("Hungarians", 4), ("Bosniaks", 2), ("Romani", 2)],
    "MKD": [("Macedonians", 64), ("Albanians", 25), ("Turkish", 4)],
    "MNE": [("Montenegrins", 45), ("Serbs", 29), ("Bosniaks", 9), ("Albanians", 5)],
    "BIH": [("Bosniaks", 50), ("Serbs", 31), ("Croats", 15)],
    "HRV": [("Croats", 90), ("Serbs", 5)],
    "SVN": [("Slovenes", 83), ("Serbs", 2), ("Croats", 2)],
    "HUN": [("Hungarians", 90), ("Romani", 4), ("Germans", 1)],
    "SVK": [("Slovaks", 86), ("Hungarians", 10), ("Romani", 2)],
    "CZE": [("Czechs", 90), ("Slovaks", 2), ("Polish", 1)],
    "POL": [("Polish", 94), ("Germans", 2), ("Ukrainians", 2), ("Belarusians", 1), ("Silesian", 1)],
    "AUT": [("Austrians", 91), ("Germans", 2), ("Serbs", 1)],
    "CHE": [("Swiss Germans", 63), ("Swiss French", 23), ("Swiss Italians", 8)],
    "ITA": [("Italians", 86), ("Arabs", 1.5), ("Romanians", 1.5), ("Albanians", 1)],
    "ESP": [("Spaniards", 84), ("Catalans", 16)],
    "PRT": [("Portuguese", 89), ("Black Brazilians", 2), ("Ukrainians", 1)],
    "GRC": [("Greeks", 93), ("Albanians", 4)],
    "TUR": [("Turkish", 75), ("Kurds", 18)],
    "IRN": [("Persians", 60), ("Azerbaijanis", 16), ("Kurds", 10)],
    "IRQ": [("Arabs", 75), ("Kurds", 20)],
    "SYR": [("Arabs", 90), ("Kurds", 9)],
    "ISR": [("Jews", 74), ("Arabs", 21)],
    "SAU": [("Arabs", 90), ("Afro-Arabs", 10)],
    "EGY": [("Egyptians", 91), ("Bedouins", 5)],
    "MAR": [("Arabs", 66), ("Berbers", 33)],
    "DZA": [("Arabs", 75), ("Berbers", 25)],
    "TUN": [("Arabs", 80), ("Berbers", 20)],
    "LBY": [("Arabs", 92), ("Berbers", 5)],
    "SDN": [("Arabs", 70), ("Fur", 5), ("Beja", 5)],
    "ETH": [("Oromo", 34), ("Amhara", 27), ("Somali", 6), ("Tigray", 6)],
    "KEN": [("Kikuyu", 17), ("Luhya", 14), ("Kalenjin", 13), ("Luo", 11)],
    "NGA": [("Hausa", 25), ("Yoruba", 21), ("Igbo", 18), ("Fulani", 6)],
    "AFG": [("Pashtun", 42), ("Tajik", 27), ("Hazara", 9), ("Uzbek", 9)],
    "PAK": [("Punjabi", 45), ("Pashtun", 15), ("Sindhi", 14), ("Saraiki", 10)],
    "BGD": [("Bengalis", 93), ("Biharis", 3), ("Hill tribes", 1)],
    "IDN": [("Javanese", 40), ("Sundanese", 15), ("Malay", 4), ("Batak", 4)],
    "MYS": [("Malays", 50), ("Chinese", 23), ("Indians", 7)],
    "PHL": [("Tagalog", 28), ("Cebuano", 13), ("Ilocano", 9)],
    "VNM": [("Vietnamese", 86), ("Tay", 2), ("Thai", 2)],
    "MMR": [("Bamar", 68), ("Shan", 9), ("Karen", 7)],
    "THA": [("Thai", 90), ("Chinese", 10)],
    "MNG": [("Mongols", 93), ("Kazakhs", 4)],
    "JPN": [("Japanese", 95), ("Korean", 1), ("Chinese", 1)],
    "KOR": [("Koreans", 96), ("Chinese", 1)],
    "PRK": [("Koreans", 97), ("Chinese", 1)],
    "GRL": [("Inuit", 89), ("Danes", 10)],
    "NOR": [("Norwegians", 86), ("Poles", 2), ("Swedes", 1)],
    "MEX": [("Mestizo", 62), ("Indigenous", 21), ("White Mexicans", 7)],
    "COL": [("Mestizo", 50), ("White Colombians", 20), ("Afro-Colombians", 10)],
    "MDG": [("Malagasy", 94), ("French", 1)],
    "IRL": [("Irish", 85), ("White Irish", 10)],
    "CUB": [("White Cubans", 64), ("Mixed-race Cubans", 27)],
    "VEN": [("Mestizo", 50), ("White Venezuelans", 40)],
    "MLI": [("Bambara", 34), ("Fula", 14), ("Soninke", 9)],
    "PRY": [("Mestizo", 90), ("Indigenous", 4), ("White Paraguayans", 1)],
    "BOL": [("Mestizo", 68), ("Indigenous", 20)],
    "PER": [("Mestizo", 60), ("Indigenous", 26)],
    "ECU": [("Mestizo", 71), ("Indigenous", 7)],
    "GTM": [("Mestizo", 60), ("Indigenous", 40)],
    "HND": [("Mestizo", 85), ("Indigenous", 5), ("Black Hondurans", 1)],
    "DOM": [("Mixed Dominicans", 70), ("White Dominicans", 15)],
    "HTI": [("Black Haitians", 90), ("Mixed Haitians", 5)],
    "JAM": [("Black Jamaicans", 88), ("Mixed Jamaicans", 4)],
    "TTO": [("Indo-Trinidadian", 35), ("Afro-Trinidadian", 34)],
    "GUY": [("Indo-Guyanese", 40), ("Afro-Guyanese", 30)],
    "SUR": [("Indo-Surinamese", 27), ("Maroon", 21), ("Creole", 16)],
    "BHS": [("Black Bahamians", 85), ("White Bahamians", 5)],
    "BRB": [("Black Barbadians", 88), ("White Barbadians", 3)],
    "PAN": [("Mestizo", 60), ("Indigenous", 5)],
    "CRI": [("White Costa Ricans", 80), ("Mestizo", 4)],
    "NIC": [("Mestizo", 65), ("Indigenous", 4)],
    "BLZ": [("Mestizo", 52), ("Creole", 26)],
    "SLV": [("Mestizo", 82), ("Indigenous", 3)],
    "URY": [("White Uruguayans", 83), ("Mestizo", 5)],
    "ARG": [("White Argentines", 85), ("Mestizo", 10)],
    "CHL": [("Mestizo", 60), ("White Chileans", 30)],
    "COD": [("Luba", 18), ("Kongo", 16), ("Mongo", 14), ("Rwanda", 10)],
    "AGO": [("Ovimbundu", 37), ("Kimbundu", 25), ("Kongo", 13)],
    "TCD": [("Sara", 28), ("Arab", 12), ("Kanembu", 9)],
    "SWE": [("Swedes", 85), ("Finns", 3), ("Iraqis", 2)],
    "FIN": [("Finns", 88), ("Swedish Finns", 5), ("Russians", 1)],
    "NER": [("Hausa", 53), ("Songhai", 21), ("Tuareg", 11)],
    "ZMB": [("Bemba", 33), ("Tonga", 15), ("Chewa", 7)],
    "NAM": [("Ovambo", 50), ("Kavango", 9), ("Herero", 7)],
    "MOZ": [("Makua", 26), ("Tsonga", 11), ("Malawi", 9)],
    "MWI": [("Chewa", 34), ("Lomwe", 19), ("Yao", 13)],
    "BWA": [("Tswana", 79), ("Kalanga", 11)],
    "UGA": [("Baganda", 17), ("Banyankole", 10), ("Basoga", 8)],
    "GAB": [("Fang", 32), ("Punu", 18), ("Nzebi", 13)],
    "TZA": [("Sukuma", 16), ("Nyamwezi", 5), ("Chagga", 5)],
    "MRT": [("Arabs", 70), ("Black Moors", 30)],
    "LAO": [("Lao", 55), ("Khmu", 11), ("Hmong", 8)],
    "FJI": [("iTaukei", 57), ("Indo-Fijian", 37)],
    "ARM": [("Armenians", 94), ("Russians", 2), ("Yazidis", 1)],
    "KWT": [("Arabs", 78), ("Kuwaitis", 20)],
    "GNQ": [("Fang", 86), ("Bubi", 7)],
    "DNK": [("Danes", 87), ("Polish", 1), ("Turkish", 1)],
    "UZB": [("Uzbeks", 84), ("Tajiks", 5), ("Kazakhs", 3)],
    "CAF": [("Baya", 33), ("Banda", 27), ("Mandjia", 13)],
    "PNG": [("Papuan", 65), ("Melanesian", 30)],
    "NZL": [("New Zealand Europeans", 70), ("Māori", 17), ("Asian New Zealanders", 15)],
    "TKM": [("Turkmens", 85), ("Uzbeks", 5), ("Russians", 4)],
    "YEM": [("Arabs", 85), ("Somalis", 5), ("Afro-Arabs", 5)],
    "CMR": [("Fang", 20), ("Bamileke", 18), ("Bassa", 12), ("Fulani", 10)],
    "SOM": [("Somalis", 85), ("Arabs", 5), ("Bantu", 5)],
    "ZWE": [("Shona", 82), ("Ndebele", 14)],
    "OMN": [("Arabs", 75), ("Baloch", 5), ("South Asian", 5)],
    "CIV": [("Akan", 42), ("Gur", 18), ("Northern Mande", 10)],
    "COG": [("Kongo", 51), ("Teke", 17), ("Mboshi", 12)],
    "BFA": [("Mossi", 52), ("Fulani", 8), ("Gurunsi", 7)],
    "GIN": [("Fulani", 33), ("Malinke", 30), ("Sussu", 20)],
    "GHA": [("Akan", 47), ("Mole-Dagbon", 17), ("Ewe", 13)],
    "SLB": [("Melanesian", 90), ("Polynesian", 4)],
    "KGZ": [("Kyrgyz", 73), ("Uzbeks", 15), ("Russians", 6)],
    "SEN": [("Wolof", 39), ("Fulani", 28), ("Serer", 15)],
    "KHM": [("Khmer", 90), ("Vietnamese", 3), ("Chinese", 1)],
    "ISL": [("Icelanders", 88), ("Polish", 3), ("Danes", 1)],
    "LVA": [("Latvians", 62), ("Russians", 26)],
    "LTU": [("Lithuanians", 85), ("Poles", 7), ("Russians", 5)],
    "TJK": [("Tajiks", 84), ("Uzbeks", 14)],
    "NPL": [("Nepali", 80), ("Madheshi", 10)],
    "ERI": [("Tigrinya", 55), ("Tigre", 30)],
    "BEN": [("Fon", 39), ("Adja", 15), ("Yoruba", 12)],
    "LBR": [("Kpelle", 20), ("Bassa", 14), ("Grebo", 8)],
    "GMB": [("Mandinka", 34), ("Fulani", 24), ("Wolof", 15)],
    "TGO": [("Ewe", 46), ("Kabye", 14), ("Tem", 6)],
    "SLE": [("Temne", 35), ("Mende", 31), ("Kono", 5)],
    "BDI": [("Hutu", 85), ("Tutsi", 14)],
    "RWA": [("Hutu", 85), ("Tutsi", 14)],
    "LSO": [("Sotho", 95), ("Zulu", 3)],
    "SWZ": [("Swazi", 93), ("Zulu", 4)],
    "GNB": [("Fulani", 30), ("Balanta", 30), ("Mandinka", 13)],
    "DJI": [("Somali", 60), ("Afar", 35)],
    "EST": [("Estonians", 69), ("Russians", 26)],
    "LVA": [("Latvians", 62), ("Russians", 26)],
    "NLD": [("Dutch", 80), ("Frisians", 3), ("Turkish", 2)],
    "BEL": [("Flemings", 58), ("Walloons", 31), ("Other", 11)],
    "AZE": [("Azerbaijanis", 92), ("Russians", 2), ("Armenians", 2)],
    "ALB": [("Albanians", 83), ("Greeks", 1), ("Other", 5)],
    "ARE": [("Arabs", 60), ("South Asian", 30)],
    "JOR": [("Arabs", 94), ("Circassians", 2)],
    "GEO": [("Georgians", 87), ("Azerbaijanis", 6), ("Armenians", 5)],
    "MDA": [("Moldovans", 75), ("Romanians", 7), ("Ukrainians", 7), ("Russians", 6)],
    "LKA": [("Sinhalese", 75), ("Sri Lankan Tamils", 11), ("Sri Lankan Moors", 9)],
    "ESH": [("Arabs", 80), ("Saharawis", 20)],
    "XSO": [("Somalis", 85), ("Other", 15)],
    "BTN": [("Ngalop", 50), ("Sharchop", 35)],
    "TWN": [("Han Taiwanese", 84), ("Indigenous Taiwanese", 2)],
    "FLK": [("British", 90)],
    "LUX": [("Luxembourgers", 55), ("Portuguese", 16), ("French", 6)],
    "XNC": [("Turkish Cypriots", 50), ("Greek Cypriots", 5)],
    "CYP": [("Greek Cypriots", 60), ("Turkish Cypriots", 18)],
    "LBN": [("Lebanese", 68), ("Syrian", 8), ("Palestinian", 8)],
    "PSE": [("Palestinians", 85), ("Arabs", 10)],
    "KWT": [("Kuwaitis", 31), ("Arabs", 28), ("South Asian", 37)],
    "QAT": [("Arabs", 40), ("South Asian", 36)],
    "BHR": [("Bahrainis", 46), ("South Asian", 36)],
    "BRN": [("Malay", 66), ("Chinese", 10), ("Indians", 3)],
    "VUT": [("Melanesian", 90), ("Polynesian", 4)],
    "MUS": [("Indo-Mauritian", 68), ("Creole", 27)],
    "PRI": [("White Puerto Ricans", 65), ("Mixed Puerto Ricans", 12)],
    "CPV": [("Creole", 70), ("African", 28)],
    "NCL": [("Melanesian", 40), ("European", 30), ("Polynesian", 10)],
    "PYF": [("Polynesian", 80), ("French", 12)],
    "ALA": [("Swedes", 50), ("Finns", 45)],
    "FRO": [("Danes", 50), ("Faroese", 45)],
    "ATF": [("French", 60), ("Various", 40)],
    "SGS": [("British", 60), ("Various", 40)],
}

def find_matching_name(greg_name, country_names):
    """Best-effort match between a GREG group name and the country composition names."""
    canon = NAME_CANON.get(greg_name, greg_name)
    cl = canon.lower()
    for cn in country_names:
        if cn.lower() == cl:
            return cn
    for cn in country_names:
        cnl = cn.lower()
        if cnl in cl or cl in cnl:
            return cn
    return None

def supplement_with_secondary_groups(minority_data, provinces, centers, greg_groups):
    """Merge ALL GREG-assigned groups with country-level composition."""
    random.seed(42)
    for pid_str, entries in list(minority_data.items()):
        pid = int(pid_str)
        if pid not in provinces:
            continue
        iso = provinces[pid]["iso_a3"]
        if iso not in COUNTRY_ETHNIC:
            continue

        comp = COUNTRY_ETHNIC[iso]
        country_map = {n: p for n, p in comp}

        # Collect ALL GREG groups for this province (canonicalized, deduped)
        greg_map = {}
        for e in entries:
            canon = NAME_CANON.get(e['n'], e['n'])
            greg_map[canon] = greg_map.get(canon, 0.0) + e['p']

        greg_names = list(greg_map.keys())

        if all(g in ("Unknown", "unknown") for g in greg_names):
            # No GREG data — allocate minorities with meaningful floor, majority fills remainder
            all_groups = {}
            minority_settled = 0.0
            for cn, cp in comp:
                if cp >= 50:
                    continue
                share = max(2.5, cp * random.uniform(0.5, 2.0))
                all_groups[cn] = share
                minority_settled += share
            if minority_settled > 40:
                scale = 40.0 / minority_settled
                for n in list(all_groups.keys()):
                    all_groups[n] *= scale
                minority_settled = 40.0
            majority_names = [cn for cn, cp in comp if cp >= 50]
            if majority_names:
                all_groups[majority_names[0]] = max(0, 100.0 - minority_settled)
            # Add ±15% per-province variation
            for n in list(all_groups.keys()):
                all_groups[n] *= 1 + random.uniform(-0.15, 0.15)
            raw = [(n, p) for n, p in all_groups.items()]
        else:
            n_greg = len(greg_names)
            equal_share = 100.0 / n_greg

            # Build composition: allocate minorities first, let majority fill remainder
            all_groups = {}
            minority_settled = 0.0

            for gn in greg_names:
                cn = find_matching_name(gn, list(country_map.keys()))
                if cn:
                    base_pct = country_map[cn]
                    if base_pct < 50:
                        # GREG found a minority group — boost significantly
                        share = max(25.0, base_pct + random.uniform(15, 30))
                        share *= 1 + random.uniform(-0.15, 0.15)
                        all_groups[cn] = share
                        minority_settled += share
                else:
                    # Local GREG group not in country comp — preserve it
                    share = max(3, equal_share * random.uniform(0.3, 0.6))
                    all_groups[gn] = share
                    minority_settled += share

            # Add COUNTRY_ETHNIC minority groups not assigned by GREG — with a meaningful floor
            existing_names = set(all_groups.keys())
            existing_canon = {NAME_CANON.get(g, g) for g in existing_names}
            for cn, cp in country_map.items():
                if cp >= 50:
                    continue
                ccn = NAME_CANON.get(cn, cn)
                if cn not in existing_names and ccn not in existing_canon:
                    share = max(2.5, cp * random.uniform(0.5, 2.0))
                    all_groups[ccn] = share
                    minority_settled += share

            # Cap minority total at 40% to leave room for majority
            if minority_settled > 40:
                scale = 40.0 / minority_settled
                for n in list(all_groups.keys()):
                    all_groups[n] *= scale
                minority_settled = 40.0

            # Add the majority group(s) — fill the remainder
            majority_names = [cn for cn, cp in comp if cp >= 50]
            if majority_names:
                remainder = max(0, 100.0 - minority_settled)
                all_groups[majority_names[0]] = remainder

            # Per-province ±10% variation on all groups
            for n in list(all_groups.keys()):
                all_groups[n] *= 1 + random.uniform(-0.10, 0.10)

            raw = [(n, p) for n, p in all_groups.items()]

        # Filter tiny entries
        raw = [(n, p) for n, p in raw if p > 0.5]
        # Renormalize
        total = sum(p for _, p in raw)
        if total > 0:
            norm = [(n, round(p / total * 100, 1)) for n, p in raw]
            diff = 100.0 - sum(p for _, p in norm)
            if norm and abs(diff) > 0.01:
                norm[-1] = (norm[-1][0], round(norm[-1][1] + diff, 1))
        else:
            norm = [(greg_names[0], 100.0)]

        # Ensure diversity: break up 100% single-group provinces
        if len(norm) == 1 and norm[0][0] != "Unknown":
            if len(country_map) >= 2 and len([cp for cn, cp in comp if cp >= 50]) < 2:
                # Try to add a minority group from country comp
                second_name = max([cn for cn, cp in comp if cp < 50] or [""],
                                  key=lambda n: country_map.get(n, 0) if n else 0)
                if second_name:
                    second_pct = max(0.5, country_map[second_name] * random.uniform(0.5, 1.0))
                    norm[0] = (norm[0][0], round(norm[0][1] - second_pct, 1))
                    norm.append((second_name, round(second_pct, 1)))
                else:
                    other_pct = max(0.5, random.uniform(1, 5))
                    norm[0] = (norm[0][0], round(norm[0][1] - other_pct, 1))
                    norm.append(("Unknown", round(other_pct, 1)))
            else:
                other_pct = max(0.5, random.uniform(1, 5))
                norm[0] = (norm[0][0], round(norm[0][1] - other_pct, 1))
                norm.append(("Unknown", round(other_pct, 1)))

        minority_data[pid_str] = [{"n": n, "p": p} for n, p in norm]
    return minority_data


def ensure_diversity(minority_data, provinces):
    """Final pass: break up 100% single-group provinces not covered by COUNTRY_ETHNIC."""
    random.seed(7)
    changes = 0
    for pid_str, entries in list(minority_data.items()):
        pid = int(pid_str)
        if pid not in provinces:
            continue
        if len(entries) == 1 and entries[0]['n'] != 'Unknown':
            pct = entries[0]['p']
            other = max(0.5, random.uniform(1, 5))
            minority_data[pid_str] = [
                {'n': entries[0]['n'], 'p': round(pct - other, 1)},
                {'n': 'Unknown', 'p': round(other, 1)},
            ]
            changes += 1
    print(f"  Ensured diversity in {changes} provinces")
    return minority_data


def generate_minority_colors(minority_data):
    """Generate deterministic colors for each ethnic group name."""
    colors = {}
    for pid_str, entries in minority_data.items():
        for e in entries:
            name = e["n"]
            if name not in colors:
                h = hashlib.md5(name.encode()).digest()
                colors[name] = [
                    30 + (h[0] % 200),
                    30 + (h[1] % 200),
                    30 + (h[2] % 200),
                ]
    return colors

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

# ─── ACOR: Oil fields ──────────────────────────────────────────────
def load_acor_oil():
    """Load oil field polygons from ACOR dataset.
    Returns centroids (lon, lat) for each oil field polygon.
    """
    sf = shapefile.Reader(ACOR_SHP)
    fields = [f[0] for f in sf.fields[1:]]
    oil_centroids = []
    for i in range(sf.numRecords):
        rec = sf.record(i)
        d = dict(zip(fields, rec))
        if d.get("type") != "oil":
            continue
        shp = sf.shape(i)
        if not shp.parts:
            continue
        parts = []
        for pi in range(len(shp.parts)):
            start = shp.parts[pi]
            end = shp.parts[pi + 1] if pi + 1 < len(shp.parts) else len(shp.points)
            ring = shp.points[start:end]
            if len(ring) >= 3:
                parts.append(ring)
        if not parts:
            continue
        poly = ShapelyPolygon(parts[0], parts[1:] if len(parts) > 1 else None)
        if poly.is_valid and not poly.is_empty and poly.area > 0:
            centroid = poly.centroid
            oil_centroids.append((centroid.x, centroid.y))
    print(f"  Loaded {len(oil_centroids)} oil field centroids")
    return oil_centroids

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

    # Process oil fields via centroid-to-pixel lookup
    oil_counts = defaultdict(int)
    oil_provinces = set()
    for lon, lat in oil_centroids:
        px, py = lonlat_to_pixel(lon, lat)
        found = False
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                pid = pixel_to_pid_safe(px + dx, py + dy)
                if pid > 0 and pid in provinces:
                    oil_counts[pid] += 1
                    oil_provinces.add(pid)
                    found = True
                    break
            if found:
                break
        if not found:
            pid = pixel_to_pid_safe(px, py)
            if pid > 0 and pid in provinces:
                oil_counts[pid] += 1
                oil_provinces.add(pid)
    print(f"  Oil fields found in {len(oil_provinces)} provinces")

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
    OIL_FIX = {
        # Middle East — massively undercounted by ACOR centroid data
        "SAU": 3.0, "IRQ": 2.5, "IRN": 2.0, "KWT": 5.0, "ARE": 3.5,
        "QAT": 3.5, "OMN": 1.5, "BHR": 4.0,
        # Americas
        "VEN": 2.5, "CAN": 1.5, "USA": 0.08, "MEX": 0.7,
        "BRA": 1.5, "COL": 1.0, "ECU": 1.0, "ARG": 0.15,
        "TTO": 1.0, "CUB": 1.0,
        # Africa — ACOR misses many fields
        "NGA": 1.5, "AGO": 1.5, "DZA": 0.8, "LBY": 1.2,
        "EGY": 1.0, "SDN": 2.0, "GAB": 1.5, "COG": 1.5,
        "TCD": 1.0, "CMR": 1.5, "GNQ": 2.0,
        # Europe — massive overcount from small seeps
        "GBR": 0.3, "NOR": 0.3, "NLD": 0.1, "DNK": 0.1,
        "ROU": 0.1, "POL": 0.05, "DEU": 0.02, "FRA": 0.02,
        "ITA": 0.05, "GRC": 0.05, "UKR": 0.05, "BLR": 0.1,
        "HRV": 0.1, "SRB": 0.1, "HUN": 0.1, "BGR": 0.1,
        "AUT": 0.1, "CZE": 0.1, "SVK": 0.1, "CHE": 0.0,
        # Asia
        "JPN": 0.01, "CHN": 0.5, "IND": 0.3,
        "IDN": 0.3, "MYS": 0.5, "MMR": 0.8,
        "KAZ": 0.6, "UZB": 0.5, "AZE": 0.5, "TKM": 0.6,
        "VNM": 0.3, "THA": 0.3, "PHL": 0.2,
        "PNG": 1.0, "AUS": 0.3, "NZL": 0.2,
        "TUR": 0.2, "SYR": 0.5, "YEM": 0.5,
        "TUN": 0.5,
    }
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

    # Manual industry level overrides for specific provinces
    INDUSTRY_OVERRIDE = {1697: 5, 1328: 5, 1533: 3, 1487: 2, 1535: 3, 1821: 3, 1822: 4}
    for pid_str, lvl in INDUSTRY_OVERRIDE.items():
        if str(pid_str) in resources:
            resources[str(pid_str)]["industry"]["level"] = lvl

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

    # ── Ethnic groups — skip if shapefiles missing ──
    greg_groups = []
    minority_data = {}
    colors = {}
    try:
        print("\n=== Processing GREG ethnic groups ===")
        greg_groups = load_greg()
        minority_data = assign_ethnic_groups(centers, greg_groups)
        minority_data = supplement_with_secondary_groups(minority_data, provinces, centers, greg_groups)
        minority_data = ensure_diversity(minority_data, provinces)
        colors = generate_minority_colors(minority_data)
        valid_pids = set(provinces.keys())
        minority_data = {k: v for k, v in minority_data.items() if int(k) in valid_pids}
        with open(OUT_MINORITIES, "w") as f:
            json.dump(minority_data, f, indent=None, separators=(",", ":"))
        print(f"  Wrote {OUT_MINORITIES} ({len(minority_data)} provinces)")
        with open(OUT_MINORITY_COLORS, "w") as f:
            json.dump(colors, f, indent=None, separators=(",", ":"))
        print(f"  Wrote {OUT_MINORITY_COLORS} ({len(colors)} groups)")
    except Exception as e:
        print(f"  GREG shapefile unavailable ({e}), keeping existing minority files")

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
        print("\n=== Processing ACOR oil fields ===")
        oil_centroids = load_acor_oil()
        resources_loaded = True
    except Exception as e:
        print(f"  USGS/ACOR shapefiles unavailable ({e}), preserving existing resources.json")
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

    def has_ocean_neighbor(pid, check_radius=3):
        """Check if any pixel of this province is within check_radius pixels of ocean (land_sea=0)."""
        for y in range(0, ph2, 8):
            for x in range(0, pw2, 8):
                r, g, b = prov_px[x, y]
                if r == 0 and g == 0 and b == 0:
                    continue
                if (r << 16) | (g << 8) | b != pid:
                    continue
                for ny in range(max(0, y - check_radius), min(ph2, y + check_radius + 1)):
                    for nx in range(max(0, x - check_radius), min(pw2, x + check_radius + 1)):
                        if ls_px[nx % pw2, ny] == 0:
                            return True
        return False

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