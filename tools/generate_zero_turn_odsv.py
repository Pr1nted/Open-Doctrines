#!/usr/bin/env python3
"""Generate a zero-turn .odsv save file (just embedded map + metadata, no turns)."""

import json
import zipfile
import os
from datetime import datetime, timezone

DATA_DIR = os.path.join(os.path.dirname(__file__), '..', 'data')
ODMAP_PATH = os.path.join(DATA_DIR, 'STDmaps', 'map.odmap')
ODSV_PATH = os.path.join(DATA_DIR, 'saves', 'default.odsv')


def main():
    if not os.path.exists(ODMAP_PATH):
        print(f"Error: {ODMAP_PATH} not found.")
        return

    odm_data = open(ODMAP_PATH, 'rb').read()
    print(f"Read map.odmap: {len(odm_data):,} bytes")

    if os.path.exists(ODSV_PATH):
        os.remove(ODSV_PATH)

    now = datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')

    with zipfile.ZipFile(ODSV_PATH, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('map.odmap', odm_data)
        print(f"  Added map.odmap ({len(odm_data):,} bytes)")

        meta = {
            'save_name': 'Default Save (Zero Turns)',
            'created': now,
            'turn_count': 0,
        }
        z.writestr('metadata.json', json.dumps(meta, indent=2))
        print(f"  Added metadata.json")

        idx = {'turns': []}
        z.writestr('index.json', json.dumps(idx, indent=2))
        print(f"  Added index.json (empty turn list)")

    print(f"\nCreated {ODSV_PATH}")
    print(f"File size: {os.path.getsize(ODSV_PATH):,} bytes")
    print(f"Turns: 0 (just the map, no game history)")


if __name__ == '__main__':
    main()
