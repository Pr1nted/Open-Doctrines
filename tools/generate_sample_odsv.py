#!/usr/bin/env python3
"""Generate a sample .odsv save file for inspection."""

import json
import struct
import zipfile
import os
import shutil
from datetime import datetime

DATA_DIR = os.path.join(os.path.dirname(__file__), '..', 'data')
ODMAP_PATH = os.path.join(DATA_DIR, 'STDmaps', 'map.odmap')
ODSV_PATH = os.path.join(DATA_DIR, 'saves', 'sample.odsv')


def pack_turn(turn_num, provinces, ships, armies):
    """Pack a turn delta into binary format (little-endian)."""

    def u16(buf, v): buf.extend(struct.pack('<H', v))
    def u32(buf, v): buf.extend(struct.pack('<I', v))
    def f32(buf, v): buf.extend(struct.pack('<f', v))

    buf = bytearray()
    # Header
    u16(buf, turn_num)
    u16(buf, len(provinces))
    u16(buf, len(ships))
    u16(buf, len(armies))

    # Province entries
    for pid, fields in provinces:
        u16(buf, pid)
        mask = 0
        bits = []
        if 'owner' in fields:          mask |= 1 << 0; bits.append(lambda: u16(buf, fields['owner']))
        if 'population' in fields:     mask |= 1 << 1; bits.append(lambda: u32(buf, fields['population']))
        if 'industry' in fields:       mask |= 1 << 2; bits.append(lambda: buf.append(fields['industry']))
        if 'fort' in fields:           mask |= 1 << 3; bits.append(lambda: buf.append(fields['fort']))
        if 'income' in fields:         mask |= 1 << 4; bits.append(lambda: f32(buf, fields['income']))
        if 'res_income' in fields:     mask |= 1 << 5; bits.append(lambda: f32(buf, fields['res_income']))
        if 'pop_income' in fields:     mask |= 1 << 6; bits.append(lambda: f32(buf, fields['pop_income']))
        if 'pop_mod' in fields:        mask |= 1 << 7; bits.append(lambda: f32(buf, fields['pop_mod']))
        u16(buf, mask)
        for b in bits: b()

    # Ship entries
    for sidx, fields in ships:
        u16(buf, sidx)
        mask = 0
        bits = []
        if 'lat' in fields:    mask |= 1 << 0; bits.append(lambda: f32(buf, fields['lat']))
        if 'lon' in fields:    mask |= 1 << 1; bits.append(lambda: f32(buf, fields['lon']))
        if 'health' in fields: mask |= 1 << 2; bits.append(lambda: buf.append(fields['health']))
        if 'crew' in fields:   mask |= 1 << 3; bits.append(lambda: u16(buf, fields['crew']))
        buf.append(mask)
        for b in bits: b()

    # Army entries
    for pid, units in armies:
        u16(buf, pid)
        buf.append(len(units))
        for country_id, count in units:
            u16(buf, country_id)
            u32(buf, count)

    return bytes(buf)


def main():
    if not os.path.exists(ODMAP_PATH):
        print(f"Error: {ODMAP_PATH} not found. Run cmake --build first.")
        return

    odm_data = open(ODMAP_PATH, 'rb').read()
    print(f"Read map.odmap: {len(odm_data):,} bytes")

    # Load provinces.json to get real province IDs
    prov_path = os.path.join(DATA_DIR, 'provinces.json')
    provinces_data = json.load(open(prov_path)) if os.path.exists(prov_path) else {}
    all_pids = sorted([int(k) for k in provinces_data.keys()])
    print(f"Loaded {len(all_pids)} provinces from provinces.json")

    # Load ships.json to get real ship indices
    ships_path = os.path.join(DATA_DIR, 'ships.json')
    ships_data = json.load(open(ships_path)) if os.path.exists(ships_path) else []
    print(f"Loaded {len(ships_data)} ships from ships.json")

    # ── Generate 3 sample turns ──

    # Turn 1: population drift on all provinces + some industry
    t1_provinces = []
    for pid in all_pids[:50]:  # first 50 provinces
        t1_provinces.append((pid, {'population': 1_000_000 + pid * 100}))
    t1_provinces.append((all_pids[100], {'owner': 1, 'population': 500_000}))  # owner change
    t1_provinces.append((all_pids[200], {'industry': 3, 'income': 45.5, 'fort': 2}))
    t1_ships = []
    for si in range(min(5, len(ships_data))):
        t1_ships.append((si, {'lat': ships_data[si]['lat'] + 0.1, 'lon': ships_data[si]['lon'] + 0.1}))
    t1_armies = [(all_pids[50], [(2, 5000), (3, 3000)]), (all_pids[60], [(2, 10000)])]

    # Turn 2: more changes
    t2_provinces = []
    for pid in all_pids[:30]:
        t2_provinces.append((pid, {'population': 1_100_000 + pid * 100}))
    t2_provinces.append((all_pids[100], {'owner': 5, 'population': 600_000}))  # another owner
    t2_ships = [(si, {'lat': ships_data[si]['lat'] + 0.2}) for si in range(min(3, len(ships_data)))]
    t2_armies = [(all_pids[50], [(2, 8000), (4, 2000)])]

    # Turn 3: clean-up turn
    t3_provinces = []
    for pid in all_pids[:10]:
        t3_provinces.append((pid, {'population': 1_200_000 + pid * 100}))
    t3_ships = [(0, {'health': 85, 'crew': 450})]
    t3_armies = []

    # ── Build .odsv ZIP ──

    if os.path.exists(ODSV_PATH):
        os.remove(ODSV_PATH)

    now = datetime.utcnow().strftime('%Y-%m-%dT%H:%M:%SZ')
    turn_count = 3

    with zipfile.ZipFile(ODSV_PATH, 'w', zipfile.ZIP_DEFLATED) as z:
        # Embed map.odmap
        z.writestr('map.odmap', odm_data)
        print(f"  Added map.odmap ({len(odm_data):,} bytes)")

        # metadata.json
        meta = {
            'save_name': 'Sample Save',
            'created': now,
            'turn_count': turn_count,
            'province_count': len(all_pids),
            'ship_count': len(ships_data),
        }
        z.writestr('metadata.json', json.dumps(meta, indent=2))
        print(f"  Added metadata.json")

        # index.json
        idx = {'turns': [f'{t:05d}' for t in range(1, turn_count + 1)]}
        z.writestr('index.json', json.dumps(idx, indent=2))
        print(f"  Added index.json")

        # Turn deltas
        for tnum, (provs, ships, armies) in enumerate([
            (t1_provinces, t1_ships, t1_armies),
            (t2_provinces, t2_ships, t2_armies),
            (t3_provinces, t3_ships, t3_armies),
        ], start=1):
            data = pack_turn(tnum, provs, ships, armies)
            z.writestr(f'turns/t_{tnum:05d}.dat', data)
            print(f"  Added turns/t_{tnum:05d}.dat ({len(data)} bytes)")

    print(f"\nCreated {ODSV_PATH}")
    print(f"File size: {os.path.getsize(ODSV_PATH):,} bytes")
    print(f"\nTo inspect:")
    print(f"  unzip -l {ODSV_PATH}")
    print(f"  python3 tools/read_odsv.py {ODSV_PATH}")


if __name__ == '__main__':
    main()
