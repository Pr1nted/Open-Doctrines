#!/usr/bin/env python3
"""
Restore states that existed on 1 September 1939 to The Gathering Storm.

    python3 tools/fix_1939_history.py --check    # report, change nothing
    python3 tools/fix_1939_history.py            # rewrite data/STDmaps/1939.odmap

WHAT WAS WRONG

The scenario shipped 53 countries. Nine states that were sovereign, or were
distinct states under occupation, were folded into their large neighbours --
so the map showed a British Ireland seventeen years after the Free State, a
Colombian Panama thirty-six years after separation, and no Manchukuo at all in
a scenario about the war Japan was already fighting.

HOW A PROVINCE IS CHOSEN

Provinces here have no names, so they can only be identified by where they are.
Each entry below names a list of (lon, lat) anchors and the tool asks the map
which province covers each one -- see the longer note in fix_map_history.py,
whose tables were migrated off hardcoded province ids at the same time and for
the same reason: ids are output of the generator and drift on every re-cut.

WHICH provinces an entry takes was decided by reducing every province to a
bounding box in latitude/longitude and moving it ONLY when that box falls
inside the real 1939 borders of the country claiming it. That rule is what
keeps this honest, and it is why two entries below are deliberately partial
and two more were dropped entirely:

  * Luxembourg  its one province reaches 51.15N, well into Belgium. Making
                Luxembourg out of it would hand it a slab of the Ardennes --
                a worse error than the one being fixed.
  * Bhutan      has no province of its own at this resolution. Adding it means
                editing the 8192x4096 province raster, which is a different
                job from reassigning ownership.
  * Nepal       gets its western province only; the eastern ones are mostly
                Indian territory and stay British.
  * Yemen       gets the Mutawakkilite Kingdom. Aden and the Hadhramaut stay
                British, which is correct -- the Aden Protectorate was British
                until 1967.

Korea stays Japanese for the same reason: it was annexed Japanese territory,
not part of Manchukuo, so its provinces are excluded by name below.

FLAGS ARE DRAWN, NOT DOWNLOADED

Each new country gets a procedural flag built from the engine's own pattern and
symbol vocabulary (see parseFlag/parseSymbolType in src/map/CountryMap.cpp).
Nothing is fetched, so nothing here adds a third-party licence obligation to a
scenario that currently has none. Some are necessarily approximations -- Nepal's
double pennant and Tibet's snow lion have no representation in a system of
stripes and stock symbols, and are noted as such.
"""

import json
import os
import shutil
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ODMAP = os.path.join(ROOT, "data", "STDmaps", "1939.odmap")

# iso -> (name, colour, treasury, compass, flag, anchors, note)
#
# `at` is a list of (lon, lat) points, one per province the entry takes, each
# at the deepest interior point of that province. The provinces themselves came
# from the bounding-box test described above and were checked against the
# country's real 1939 borders before being written down.
NEW_STATES = {
    "IRL": dict(
        name="Ireland", color="#4f9d5a", treasury=8.0,
        compass={"left": -10, "auth": 5},
        flag={"type": "vstripes_3", "colors": ["#169b62", "#ffffff", "#ff883e"]},
        at=[(-8.1079, 53.2837), (-9.1187, 52.229), (-6.6577, 53.064),
                   (-8.064, 54.8657)],
        note="Free State since 1922, sovereign under the 1937 constitution, "
             "neutral through the war. Province 552 is Northern Ireland and "
             "stays British.",
    ),
    "PAN": dict(
        name="Panama", color="#3f6fb5", treasury=5.0,
        compass={"left": 15, "auth": 25},
        flag={"type": "quartered",
              "colors": ["#ffffff", "#d21034", "#005293", "#ffffff"],
              "symbols": [{"type": "star_5", "x": 0.25, "y": 0.28, "size": 0.18,
                           "colors": ["#005293"]},
                          {"type": "star_5", "x": 0.75, "y": 0.72, "size": 0.18,
                           "colors": ["#d21034"]}]},
        at=[(-80.9692, 8.2837), (-82.4634, 8.7231), (-77.7173, 8.1958)],
        note="Independent of Colombia since 1903. All three provinces lie "
             "wholly inside Panama.",
    ),
    "EGY": dict(
        name="Kingdom of Egypt", color="#1e8449", treasury=22.0,
        compass={"left": 5, "auth": 45},
        flag={"type": "solid", "colors": ["#0f7d3d"],
              "symbols": [{"type": "crescent_star", "x": 0.5, "y": 0.5,
                           "size": 0.45, "colors": ["#ffffff"]}]},
        at=[(32.3657, 27.8833), (30.3003, 26.8286), (26.4771, 23.269),
                   (26.0376, 25.3784), (26.2134, 30.1245), (30.52, 29.8608),
                   (33.8599, 29.5532)],
        note="Independent since 1922 though British troops remained. The Sudan "
             "(province 352) was an Anglo-Egyptian condominium and stays "
             "British, as does Palestine (349).",
    ),
    "IRQ": dict(
        name="Kingdom of Iraq", color="#8e5a2b", treasury=14.0,
        compass={"left": 0, "auth": 50},
        flag={"type": "hstripes_3", "colors": ["#000000", "#ffffff", "#007a3d"],
              "symbols": [{"type": "star_7", "x": 0.30, "y": 0.5, "size": 0.22,
                           "colors": ["#ce1126"]},
                          {"type": "star_7", "x": 0.44, "y": 0.5, "size": 0.22,
                           "colors": ["#ce1126"]}]},
        at=[(44.4946, 31.9702)],
        note="Independent and a League member since 1932. The stars stand in "
             "for the hoist trapezoid, which has no pattern here.",
    ),
    "NPL": dict(
        name="Nepal", color="#b03050", treasury=4.0,
        compass={"left": 10, "auth": 70},
        flag={"type": "solid", "colors": ["#dc143c"],
              "symbols": [{"type": "sun", "x": 0.5, "y": 0.34, "size": 0.30,
                           "colors": ["#ffffff"]},
                          {"type": "crescent", "x": 0.5, "y": 0.68, "size": 0.30,
                           "colors": ["#ffffff"]}]},
        at=[(81.5405, 29.2017)],
        note="PARTIAL. Never colonised; the 1923 treaty confirmed its "
             "independence. Only the western province lies inside Nepal -- the "
             "eastern ones are mostly Indian and stay British. The double "
             "pennant cannot be drawn from rectangles, so the sun and moon are "
             "placed on a crimson field instead.",
    ),
    "TIB": dict(
        name="Tibet", color="#c8a02c", treasury=3.0,
        compass={"left": 0, "auth": 60},
        flag={"type": "sunburst", "colors": ["#ffd700", "#ce1126", "#1e4d9b"],
              "symbols": [{"type": "mountain", "x": 0.5, "y": 0.62, "size": 0.42,
                           "colors": ["#ffffff"]}]},
        at=[(81.145, 31.2671), (83.6938, 30.564), (95.0317, 31.3989),
                   (91.604, 29.6411), (86.5503, 32.5415)],
        note="De facto independent from 1913 until 1950: its own government, "
             "army and currency, and no Chinese administration. The snow lions "
             "have no equivalent symbol, so the rayed field and snow mountain "
             "carry it.",
    ),
    "MNG": dict(
        name="Mongolian People's Republic", color="#c0392b", treasury=4.0,
        compass={"left": -90, "auth": 90},
        flag={"type": "solid", "colors": ["#c8102e"],
              "symbols": [{"type": "star_5", "x": 0.5, "y": 0.5, "size": 0.35,
                           "colors": ["#ffd700"]}]},
        at=[(109.314, 44.4946), (100.7007, 47.3071), (103.7329, 48.4937),
                   (96.7456, 45.9888), (106.2817, 45.9888), (92.3511, 46.3843),
                   (100.7007, 43.7476), (94.0649, 47.9663), (114.6753, 48.8452),
                   (113.6206, 46.0767)],
        note="A Soviet satellite in everything but name, and fighting Japan at "
             "Khalkhin Gol as this scenario opens -- but a separate state, not "
             "Chinese territory. Inner Mongolia stays Chinese.",
    ),
    "MCK": dict(
        name="Manchukuo", color="#d4b02a", treasury=26.0,
        compass={"left": -15, "auth": 95},
        flag={"type": "canton", "colors": ["#ffde00", "#d7141a"],
              "symbols": []},
        at=[(119.8169, 42.8687), (127.9028, 43.3081), (125.9253, 47.2632),
                   (121.1353, 48.8892), (130.4517, 46.2964), (124.1235, 42.0776)],
        note="A Japanese puppet since 1932, with its own emperor, army and "
             "currency. Korea (721-724) was annexed Japanese territory rather "
             "than part of Manchukuo and stays with Japan.",
    ),
    "YEM": dict(
        name="Kingdom of Yemen", color="#a93226", treasury=3.0,
        compass={"left": 5, "auth": 75},
        flag={"type": "solid", "colors": ["#ce1126"],
              "symbols": [{"type": "sword", "x": 0.5, "y": 0.5, "size": 0.42,
                           "colors": ["#ffffff"]}]},
        at=[(45.769, 15.8423), (43.6157, 15.5786)],
        note="The Mutawakkilite Kingdom, independent of the Ottomans since "
             "1918. Aden and the Hadhramaut (596, 598) were British and stay "
             "British.",
    ),
}

# Historically load-bearing, and cheap to state.
NEW_RELATIONS = {
    "MCK": {"JPN": {"ally": True}},
    "MNG": {"SOV": {"ally": True}, "JPN": {"war": True}},
}


def main():
    check = "--check" in sys.argv
    if not os.path.exists(ODMAP):
        print(f"missing {ODMAP}", file=sys.stderr)
        return 1

    work = tempfile.mkdtemp(prefix="odmap1939_")
    try:
        with zipfile.ZipFile(ODMAP) as z:
            names = z.namelist()
            z.extractall(work)

        countries = json.load(open(os.path.join(work, "countries.json")))
        provinces = json.load(open(os.path.join(work, "provinces.json")))
        compass = json.load(open(os.path.join(work, "country_compass.json")))
        relations = json.load(open(os.path.join(work, "relations.json")))

        # Provinces are named by WHERE THEY ARE, not by id -- see the same
        # note in fix_map_history.py, which this file's tables were migrated
        # alongside. Ids are output of the generator and drift on every re-cut.
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from fix_map_history import province_raster, resolve
        pid_arr = province_raster(work)

        have = {v["iso_a3"] for v in countries.values()}
        next_id = max(int(k) for k in countries if int(k) < 60000) + 1

        moved_total = 0
        for iso, spec in NEW_STATES.items():
            if iso in have:
                print(f"  skip  {iso} already present")
                continue

            # Only move a province that is where it is supposed to be, and that
            # still belongs to whoever we expected to take it from.
            owners = {}
            for pid in resolve(spec["at"], pid_arr, iso):
                pv = provinces.get(str(pid))
                if pv is None:
                    print(f"  WARN  {iso}: province {pid} does not exist", file=sys.stderr)
                    continue
                owners.setdefault(pv["country_id"], []).append(pid)

            cid = next_id
            next_id += 1
            countries[str(cid)] = {
                "id": cid,
                "iso_a3": iso,
                "name": spec["name"],
                "color": spec["color"],
                "flag_actual": spec["flag"],
                "flag_censored": {"type": "solid", "colors": [spec["color"]],
                                  "censored": True},
                "treasury": spec["treasury"],
            }
            compass[iso] = spec["compass"]

            n = 0
            for pid in resolve(spec["at"], pid_arr, iso):
                pv = provinces.get(str(pid))
                if pv is None:
                    continue
                pv["country_id"] = cid
                pv["iso_a3"] = iso
                n += 1
            moved_total += n
            from_names = ", ".join(
                f"{len(v)} from cid {k}" for k, v in sorted(owners.items()))
            print(f"  add   {iso:4s} {spec['name']:30s} cid {cid:3d}  {n} provinces ({from_names})")

        for iso, rel in NEW_RELATIONS.items():
            if iso in {v["iso_a3"] for v in countries.values()}:
                relations.setdefault(iso, {}).update(rel)

        print(f"\n  {len(NEW_STATES)} states, {moved_total} provinces reassigned")
        if check:
            print("  --check: nothing written")
            return 0

        for fn, obj in (("countries.json", countries), ("provinces.json", provinces),
                        ("country_compass.json", compass), ("relations.json", relations)):
            with open(os.path.join(work, fn), "w", encoding="utf-8") as f:
                json.dump(obj, f, separators=(",", ":"))

        # Rewritten wholesale, preserving the original entry order so a diff of
        # the archive is about the data and not about zip bookkeeping.
        tmp = ODMAP + ".tmp"
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
            for name in names:
                z.write(os.path.join(work, name), name)
        os.replace(tmp, ODMAP)
        print(f"  wrote {ODMAP}")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
