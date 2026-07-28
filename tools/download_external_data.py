#!/usr/bin/env python3
"""
Download external data dependencies used by overlay_real_data.py and other
data-generation tools.

EVERY DATASET HERE IS PUBLIC DOMAIN. That is a rule, not a coincidence.

Two datasets used to be downloaded here and are not any more:

  GREG (ethnic group polygons, ETH Zurich)
  ACOR (oil and gas field polygons, ETH Zurich)

Neither offers a licence. Both ask only to be cited, GREG explicitly framing
that as research use, and both are digitisations of mid-century atlases their
distributors do not own. Data derived from them shipped inside map.odmap, which
is redistribution, which nobody had granted. They were replaced by
project-authored tables — tools/data/ethnic_groups.json and
tools/data/oil_fields.json — that are checked in and need no download at all.
The full reasoning is in NOTICE.md.

The test to apply before adding anything below: can you point at the sentence
that grants redistribution? If not, it does not go in the game.

Sources:
  USGS: https://mrdata.usgs.gov/major-deposits/ofr20051294.zip → /tmp/usgs_data/
  WPI:  See note below                                         → /tmp/wpi/UpdatedPub150.csv

WPI note: The World Port Index (UpdatedPub150.csv) is published by the US
National Geospatial-Intelligence Agency (NGA) as Pub 150. The official
source is https://msi.nga.mil/Publications/WPI but may require accepting
terms. A mirror may be found at:
  https://raw.githubusercontent.com/telegeography/www.downloads/master/misc/UpdatedPub150.csv
"""

import os, shutil, subprocess, sys, zipfile, urllib.request

TMP = "/tmp"

DATASETS = {
    "usgs": {
        "url": "https://mrdata.usgs.gov/major-deposits/ofr20051294.zip",
        "dest": "/tmp/usgs_data/",
        "expected": ["ofr20051294.shp", "ofr20051294.dbf", "ofr20051294.shx"],
        "license": "Public domain — work of the US Government (17 U.S.C. 105)",
    },
    "wpi": {
        "url": "https://ckan.rimes.int/it/dataset/ef461b79-7a50-4ffc-8327-31d71a690c6b/resource/23538e38-830f-4df1-b69d-4469fa6ee7af/download/UpdatedPub150.csv",
        "dest": "/tmp/wpi/UpdatedPub150.csv",
        "expected": ["UpdatedPub150.csv"],
        "license": "Public domain — work of the US Government (NGA Pub 150)",
    },
}


def download_file(url, dest_path):
    print(f"  Downloading {url} ...")
    try:
        req = urllib.request.Request(url, headers={
            "User-Agent": "Mozilla/5.0 (compatible; OpenDoctrines/1.0)",
        })
        with urllib.request.urlopen(req, timeout=60) as r:
            with open(dest_path, "wb") as f:
                f.write(r.read())
        print(f"  Saved {dest_path} ({os.path.getsize(dest_path):,} bytes)")
        return True
    except Exception as e:
        print(f"  FAILED: {e}")
        return False


def download_and_extract_zip(url, dest_dir, expected_files):
    os.makedirs(dest_dir, exist_ok=True)
    zip_path = os.path.join(TMP, os.path.basename(url))
    if not download_file(url, zip_path):
        return False
    print(f"  Extracting to {dest_dir}...")
    try:
        with zipfile.ZipFile(zip_path) as zf:
            zf.extractall(dest_dir)
    except Exception as e:
        print(f"  Extract failed: {e}")
        return False
    os.remove(zip_path)

    # Handle nested subdirectories (e.g., USGS extracts to a subfolder)
    for root, dirs, files in os.walk(dest_dir):
        for f in files:
            src = os.path.join(root, f)
            dst = os.path.join(dest_dir, f)
            if src != dst:
                if not os.path.exists(dst):
                    os.rename(src, dst)
                    print(f"  Moved {f} up from {os.path.relpath(root, dest_dir)}")

    # Check expected files
    found = all(os.path.exists(os.path.join(dest_dir, f)) for f in expected_files)
    if not found:
        print(f"  WARNING: not all expected files found in {dest_dir}")
        print(f"    Expected: {expected_files}")
        for f in expected_files:
            fp = os.path.join(dest_dir, f)
            print(f"    {f}: {'FOUND' if os.path.exists(fp) else 'MISSING'}")
    else:
        print(f"  Verified: all {len(expected_files)} expected files present")
    return True


def main():
    for name, ds in DATASETS.items():
        dest = ds["dest"]
        dest_dir = dest if dest.endswith("/") else os.path.dirname(dest)
        expected = ds["expected"]
        # Check if all expected files exist
        all_exist = all(os.path.exists(os.path.join(dest_dir, f)) for f in expected)
        if all_exist:
            print(f"{name}: already exists, skipping")
            continue
        print(f"{name}: missing or incomplete, downloading...")

        print(f"\n=== {name.upper()} ===")
        print(f"  Licence: {ds['license']}")
        if "zip" in ds["url"]:
            ok = download_and_extract_zip(ds["url"], dest, ds["expected"])
        else:
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            ok = download_file(ds["url"], dest)

        if ok:
            print(f"  {name}: ready")
        else:
            print(f"  {name}: FAILED")

    print("\nDone.")


if __name__ == "__main__":
    main()
