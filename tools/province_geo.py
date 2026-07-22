#!/usr/bin/env python3
"""Helper: find provinces by lat/lon proximity using provinces.png centroids."""
import json, numpy as np
from PIL import Image

WIDTH = 8192
HEIGHT = 4096

_cache = None

def load_province_centroids(data_dir="data"):
    global _cache
    if _cache is not None:
        return _cache
    with open(f"{data_dir}/provinces.json") as f:
        provs = json.load(f)
    img = Image.open(f"{data_dir}/provinces.png").convert("RGBA")
    arr = np.array(img, dtype=np.uint8)
    h, w = arr.shape[:2]

    # Build color->pid map
    color_to_pid = {}
    for pid_str, entry in provs.items():
        c = entry.get("color", "")
        if c and len(c) == 7:
            r = int(c[1:3], 16)
            g = int(c[3:5], 16)
            b = int(c[5:7], 16)
            enc = (r << 16) | (g << 8) | b
            color_to_pid[enc] = int(pid_str)

    centroids = {}
    for y in range(h):
        row = arr[y, :, :]
        r = row[:, 0].astype(np.int64)
        g = row[:, 1].astype(np.int64)
        b = row[:, 2].astype(np.int64)
        a = row[:, 3]
        mask = a > 0
        if not np.any(mask):
            continue
        colors = (r[mask] << 16) | (g[mask] << 8) | b[mask]
        xs = np.where(mask)[0].astype(np.float64)
        unique_c, inverse = np.unique(colors, return_inverse=True)
        counts = np.bincount(inverse)
        sum_x = np.bincount(inverse, weights=xs)
        sum_y = counts * y
        for i, c in enumerate(unique_c):
            c = int(c)
            if c not in centroids:
                centroids[c] = [0.0, 0.0, 0]
            centroids[c][0] += float(sum_x[i])
            centroids[c][1] += float(sum_y[i])
            centroids[c][2] += int(counts[i])

    result = {}
    for enc, (sx, sy, n) in centroids.items():
        pid = color_to_pid.get(enc)
        if pid is None:
            continue
        cx = sx / n
        cy = sy / n
        lon = (cx / WIDTH) * 360.0 - 180.0
        lat = 90.0 - (cy / HEIGHT) * 180.0
        result[pid] = (lon, lat)
    _cache = result
    return result

def find_province(target_lon, target_lat, centroids, iso_filter=None, provinces_iso=None):
    """Find nearest province by great-circle distance. Optionally filter by ISO."""
    best = None
    best_dist = float("inf")
    for pid, (lon, lat) in centroids.items():
        if iso_filter is not None and provinces_iso is not None:
            if provinces_iso.get(pid) != iso_filter:
                continue
        d = ((lon - target_lon) ** 2 + (lat - target_lat) ** 2) ** 0.5
        if d < best_dist:
            best_dist = d
            best = pid
    return best

def load_provinces_iso(data_dir="data"):
    with open(f"{data_dir}/provinces.json") as f:
        provs = json.load(f)
    return {int(pid_str): p.get("iso_a3", "") for pid_str, p in provs.items()}

if __name__ == "__main__":
    cents = load_province_centroids()
    print(f"Loaded {len(cents)} province centroids")
    for pid in sorted(cents.keys())[:5]:
        print(f"  {pid}: lon={cents[pid][0]:.2f}, lat={cents[pid][1]:.2f}")
