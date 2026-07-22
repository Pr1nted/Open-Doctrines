#!/usr/bin/env python3
"""Inspect a .odsv save file: dump ZIP contents and decode binary turn deltas."""

import json
import struct
import sys
import zipfile


def unpack_turn(data):
    """Decode binary turn delta into human-readable dict."""
    pos = 0
    def r16():
        nonlocal pos
        v = struct.unpack_from('<H', data, pos)[0]
        pos += 2
        return v
    def r32():
        nonlocal pos
        v = struct.unpack_from('<I', data, pos)[0]
        pos += 4
        return v
    def rf32():
        nonlocal pos
        v = struct.unpack_from('<f', data, pos)[0]
        pos += 4
        return v

    if len(data) < 8:
        return {"error": "too short"}

    turn_num = r16()
    prov_count = r16()
    ship_count = r16()
    army_count = r16()

    result = {
        "turn_number": turn_num,
        "provinces": [],
        "ships": [],
        "armies": [],
    }

    field_names = {
        0: "owner",
        1: "population",
        2: "industry_level",
        3: "fortification",
        4: "income",
        5: "resource_income",
        6: "pop_income",
        7: "pop_modifier",
    }
    field_readers = {
        0: lambda: r16(),
        1: lambda: r32(),
        2: lambda: data[pos] if (pos := pos + 1) or True else None,  # hacky
        3: lambda: data[pos] if (pos := pos + 1) or True else None,
        4: lambda: rf32(),
        5: lambda: rf32(),
        6: lambda: rf32(),
        7: lambda: rf32(),
    }

    for _ in range(prov_count):
        pid = r16()
        mask = r16()
        fields = {"province_id": pid, "changed": {}}
        for bit in range(16):
            if mask & (1 << bit):
                name = field_names.get(bit, f"bit{bit}")
                if bit == 2:
                    fields["changed"][name] = data[pos]; pos += 1
                elif bit == 3:
                    fields["changed"][name] = data[pos]; pos += 1
                elif bit in (0,):
                    fields["changed"][name] = r16()
                elif bit in (1,):
                    fields["changed"][name] = r32()
                elif bit in (4, 5, 6, 7):
                    fields["changed"][name] = round(rf32(), 4)
                else:
                    fields["changed"][name] = f"<skip:{bit}>"
        result["provinces"].append(fields)

    ship_field_names = {0: "lat", 1: "lon", 2: "health", 3: "crew"}
    for _ in range(ship_count):
        sidx = r16()
        mask = data[pos]; pos += 1
        fields = {"ship_index": sidx, "changed": {}}
        for bit in range(8):
            if mask & (1 << bit):
                name = ship_field_names.get(bit, f"bit{bit}")
                if bit == 0:
                    fields["changed"][name] = round(rf32(), 6)
                elif bit == 1:
                    fields["changed"][name] = round(rf32(), 6)
                elif bit == 2:
                    fields["changed"][name] = data[pos]; pos += 1
                elif bit == 3:
                    fields["changed"][name] = r16()
                else:
                    fields["changed"][name] = f"<skip:{bit}>"
        result["ships"].append(fields)

    for _ in range(army_count):
        pid = r16()
        unit_count = data[pos]; pos += 1
        units = []
        for _ in range(unit_count):
            cid = r16()
            count = r32()
            units.append({"country_id": cid, "count": count})
        result["armies"].append({"province_id": pid, "units": units})

    return result


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <save.odsv> [turn_number]")
        print(f"       (turn_number defaults to 'all')")
        sys.exit(1)

    path = sys.argv[1]
    try:
        zf = zipfile.ZipFile(path, 'r')
    except Exception as e:
        print(f"Error opening {path}: {e}")
        sys.exit(1)

    print(f"=== .odsv: {path} ===\n")
    print(f"ZIP contents ({len(zf.namelist())} files):")
    for name in zf.namelist():
        info = zf.getinfo(name)
        print(f"  {name:40s} {info.file_size:>8,} bytes  (compressed: {info.compress_size:>8,})")

    print()

    # metadata.json
    if 'metadata.json' in zf.namelist():
        meta = json.loads(zf.read('metadata.json'))
        print(f"metadata.json:")
        for k, v in meta.items():
            print(f"  {k}: {v}")
    print()

    # index.json
    if 'index.json' in zf.namelist():
        idx = json.loads(zf.read('index.json'))
        print(f"index.json turns: {idx.get('turns', [])}")
    print()

    # Decode turns
    turn_files = sorted([n for n in zf.namelist() if n.startswith('turns/') and n.endswith('.dat')])
    if not turn_files:
        print("No turn files found.")
        zf.close()
        return

    # Filter to specific turn if requested
    if len(sys.argv) >= 3:
        want = f"turns/t_{int(sys.argv[2]):05d}.dat"
        if want in turn_files:
            turn_files = [want]
        else:
            print(f"Turn {sys.argv[2]} not found (available: {[t.split('/')[1] for t in turn_files]})")
            zf.close()
            return

    for tf in turn_files:
        data = zf.read(tf)
        decoded = unpack_turn(data)
        print(f"{tf} ({len(data)} bytes):")
        print(f"  turn_number: {decoded['turn_number']}")
        print(f"  provinces: {len(decoded['provinces'])} entries")
        for p in decoded['provinces'][:5]:
            print(f"    province {p['province_id']}: {p['changed']}")
        if len(decoded['provinces']) > 5:
            print(f"    ... and {len(decoded['provinces']) - 5} more")
        print(f"  ships: {len(decoded['ships'])} entries")
        for s in decoded['ships']:
            print(f"    ship {s['ship_index']}: {s['changed']}")
        print(f"  armies: {len(decoded['armies'])} entries")
        for a in decoded['armies'][:3]:
            print(f"    province {a['province_id']}: {len(a['units'])} units")
        print()

    zf.close()


if __name__ == '__main__':
    main()
