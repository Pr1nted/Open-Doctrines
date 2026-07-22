#!/usr/bin/env python3
"""Package data/ files into a .odmap archive (ZIP format)."""
import json, os, sys, zipfile

DATA_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")
ODMAP_PATH = os.path.join(DATA_DIR, "map.odmap")

REQUIRED_FILES = [
    "land_sea.png", "provinces.png", "provinces.json", "countries.json",
    "political.png", "metadata.json", "population.json",
    "political_compass.json", "minorities.json", "minority_colors.json",
    "starting_policies.json", "country_compass.json", "starting_minority_policies.json",
]

EXTRA_FILES = [
    "armies.json", "claims.json", "resources.json", "ports.json", "ships.json",
    "relations.json", "policies.json",
]

# License files
LICENSE_FILES = [
    "licenses/CC-BY-4.0.txt",
]

# Country flag SVGs
FLAGS_DIR = os.path.join(DATA_DIR, "flags")

# Thumbnail for the map browser (from STDmaps)
THUMB_FILES = [
    ("STDmaps/map_thumb.png", "thumb.png"),
]

def main():
    print(f"Packaging .odmap from {DATA_DIR}")
    found = 0
    with zipfile.ZipFile(ODMAP_PATH, "w", zipfile.ZIP_DEFLATED) as zf:
        # Always include scripts/ directory (empty marker)
        scripts_dir = os.path.join(DATA_DIR, "scripts")
        if os.path.isdir(scripts_dir):
            for root, dirs, files in os.walk(scripts_dir):
                for f in files:
                    fpath = os.path.join(root, f)
                    arcname = os.path.join("scripts", os.path.relpath(fpath, scripts_dir))
                    zf.write(fpath, arcname)
                    print(f"  + {arcname} (script)")
        else:
            # Create an empty scripts/ directory entry
            info = zipfile.ZipInfo("scripts/")
            info.external_attr = 0o40755 << 16
            zf.writestr(info, "")
            print(f"  + scripts/ (empty directory)")
        # Update metadata.json has_scripts and name based on scripts/ content
        metadata_path = os.path.join(DATA_DIR, "metadata.json")
        if os.path.exists(metadata_path):
            try:
                with open(metadata_path, "r") as mf:
                    meta = json.load(mf)
                has_scripts = os.path.isdir(scripts_dir) and len(os.listdir(scripts_dir)) > 0
                meta["has_scripts"] = has_scripts
                if "name" not in meta:
                    meta["name"] = "Modern Day"
                if "description" not in meta:
                    meta["description"] = "A modern world map starting in the year 2000."
                with open(metadata_path, "w") as mf:
                    json.dump(meta, mf)
            except Exception:
                pass

        for fname in REQUIRED_FILES:
            fpath = os.path.join(DATA_DIR, fname)
            if os.path.exists(fpath):
                zf.write(fpath, fname)
                print(f"  + {fname}")
                found += 1
            else:
                print(f"  - {fname} (missing, skipped)")
        # Also include extra data files used at runtime (not in REQUIRED list)
        for fname in EXTRA_FILES:
            fpath = os.path.join(DATA_DIR, fname)
            if os.path.exists(fpath):
                zf.write(fpath, fname)
                print(f"  + {fname} (extra)")
        # Include country flag PNGs (pre-rendered with aspect ratio preservation).
        # SVG files are only included as a fallback if no corresponding PNG exists.
        if os.path.isdir(FLAGS_DIR):
            png_isos = set()
            for f in sorted(os.listdir(FLAGS_DIR)):
                if f.endswith(".png"):
                    fpath = os.path.join(FLAGS_DIR, f)
                    arcname = "flags/" + f
                    zf.write(fpath, arcname)
                    png_isos.add(f.replace(".png", ""))
                    print(f"  + {arcname} (flag png)")
            for f in sorted(os.listdir(FLAGS_DIR)):
                if f.endswith(".svg"):
                    iso = f.replace(".svg", "")
                    if iso not in png_isos:
                        fpath = os.path.join(FLAGS_DIR, f)
                        arcname = "flags/" + f
                        zf.write(fpath, arcname)
                        print(f"  + {arcname} (flag svg fallback)")
        # Include license files
        for fname in LICENSE_FILES:
            fpath = os.path.join(DATA_DIR, fname)
            if os.path.exists(fpath):
                zf.write(fpath, fname)
                print(f"  + {fname} (license)")
        # Include symbol SVGs (used by rebel flag generation)
        symbols_dir = os.path.join(DATA_DIR, "symbols")
        if os.path.isdir(symbols_dir):
            for f in sorted(os.listdir(symbols_dir)):
                if f.endswith(".svg"):
                    fpath = os.path.join(symbols_dir, f)
                    arcname = "symbols/" + f
                    zf.write(fpath, arcname)
                    print(f"  + {arcname} (symbol)")
        # Include thumbnail
        for src, dest in THUMB_FILES:
            fpath = os.path.join(DATA_DIR, src)
            if os.path.exists(fpath):
                zf.write(fpath, dest)
                print(f"  + {dest} (thumb from {src})")
    print(f"Wrote {ODMAP_PATH} ({found}/{len(REQUIRED_FILES)} required files)")
    if found < 5:
        print("ERROR: fewer than 5 required files — map won't load!")
        sys.exit(1)
    print("Done")

if __name__ == "__main__":
    main()
