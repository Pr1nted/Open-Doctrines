#!/usr/bin/env python3
"""
Give the restored states real flag images instead of procedural ones.

    python3 tools/attach_scenario_flags.py --check   # report, change nothing
    python3 tools/attach_scenario_flags.py           # rewrite the .odmap archives

The states added by fix_map_history.py and carve_states.py flew procedural
flags -- patterns assembled from the engine's own stripe-and-symbol vocabulary.
That was a deliberate choice at the time, because it adds no third-party
licence obligation, but it can only ever approximate. Nepal's double pennant is
not a rectangle, Bhutan's dragon and Tibet's snow lions have no symbol, and the
approximations read as placeholders because that is what they are.

These are the real ones, from Wikimedia Commons via
tools/download_scenario_flags.py, rasterised by
tools/prerender_problematic_flags.py and licence-audited by
tools/audit_flag_licenses.py -- the same path the other 214 shipped flags took.

Each entry names the flag of that country AT THAT SCENARIO'S DATE, which is
usually not the modern one: Egypt flew green with a crescent until 1958, Yemen
the Mutawakkilite sword, Nepal a pennant whose sun and moon still had faces.
A country whose flag has not changed reuses the modern file.
"""

import json
import os
import shutil
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FLAGS_DIR = os.path.join(ROOT, "data", "flags")
MAPS_DIR = os.path.join(ROOT, "data", "STDmaps")

# map -> iso -> flag file stem in data/flags/
PLAN = {
    "1914.odmap": {"TIB": "TIB", "NPL": "NPL_PRE1962", "BTN": "BTN_1949",
                   "PAN": "PAN", "LUX": "LUX", "MNG": "MNG_BOGD",
                   # Restoration Spain. The map was flying ESP_1938, which is
                   # Franco's, twenty-four years before there was a Franco.
                   "ESP": "ESP_RESTORATION"},
    "1918.odmap": {"TIB": "TIB", "NPL": "NPL_PRE1962", "BTN": "BTN_1949",
                   "PAN": "PAN", "LUX": "LUX", "UKR": "UKR",
                   "MNG": "MNG_BOGD", "MNE": "MNE_KINGDOM",
                   "NJD": "NEJD", "HJZ": "HEJAZ",
                   "ESP": "ESP_RESTORATION"},
    "1939.odmap": {"TIB": "TIB", "NPL": "NPL_PRE1962", "BTN": "BTN_1949",
                   "PAN": "PAN", "LUX": "LUX", "IRL": "IRL",
                   "EGY": "EGY_KINGDOM", "YEM": "YEM_KINGDOM",
                   "MNG": "MNG_MPR1939", "IRQ": "IRQ_KINGDOM", "MCK": "MCK",
                   "SVK": "SVK",
                   # Siam adopted the tricolour in 1917 and was renamed
                   # Thailand in June 1939. The scenario had the name right and
                   # the flag twenty-two years stale -- a white elephant on red
                   # under the word "Thailand", which never happened.
                   "THA": "THA",
                   # Black-red-green with the mosque emblem, 1931-1973. Every
                   # map was falling back to a generated brown tricolour, and
                   # 1962 to the 2013-2021 flag.
                   "AFG": "AFG_KINGDOM"},
    "1945.odmap": {"TIB": "TIB", "NPL": "NPL_PRE1962", "BTN": "BTN_1949",
                   "PAN": "PAN", "LUX": "LUX", "EGY": "EGY_KINGDOM",
                   "JPN": "JPN", "LBN": "LBN", "AUT": "AUT",
                   "IRQ": "IRQ_KINGDOM", "SYR": "SYR_1932",
                   "YEM": "YEM_KINGDOM",
                   # Not added by fix_map_history -- these two come straight
                   # from the scenario file and never had a flag named, so
                   # build_countries fell back to a generated tricolour. Iran's
                   # was the right three colours with the lion and sun missing;
                   # Ethiopia's was a brown gradient with nothing to do with
                   # Ethiopia. Both have a real file already downloaded.
                   "IRN": "IRN_PERSIA", "ETH": "ETH_EMPIRE",
                   "AFG": "AFG_KINGDOM"},
    # October 1962 inherits the modern base map's flags for every country
    # `independent_default` fills in, so any flag adopted after 1962 arrives
    # wrong by default -- Canada's maple leaf is three years early here, and
    # Nepal's current flag was adopted that DECEMBER, two months after this
    # scenario opens.
    "1962.odmap": {"RVN": "VNM_SOUTH",
                   # The lion and sun flew until 1979.
                   "IRN": "IRN_PERSIA", "AFG": "AFG_KINGDOM",
                   # Already on disk, just never pointed at.
                   "NPL": "NPL_PRE1962",     # current flag is December 1962
                   "MNG": "MNG_1945",        # MPR, 1945-1992
                   "SRB": "YUG_SFR",         # the map renames this "Yugoslavia"
                   "SYR": "SYR_1932",        # readopted on leaving the UAR, 1961
                   "ETH": "ETH_EMPIRE",      # 1897-1974
                   # Downloaded for this.
                   "CAN": "CAN_RED_ENSIGN",  # maple leaf is 1965
                   "ZAF": "ZAF_1928",        # 1928-1994
                   "EGY": "EGY_UAR",         # the United Arab Republic
                   "LBY": "LBY_KINGDOM",     # Kingdom of Libya, 1951-1969
                   "SDN": "SDN_1956",        # blue-yellow-green, 1956-1970
                   "LAO": "LAO_KINGDOM",     # Kingdom of Laos, 1952-1975
                   "BGR": "BGR_1948",        # with the state emblem
                   "ALB": "ALB_1946",        # with the star
                   "BFA": "BFA_UPPER_VOLTA", # Upper Volta, not Burkina Faso
                   "BDI": "BDI_1962",        # 1962-1966
                   "CMR": "CMR_1961",        # 1961-1975
                   "ESP": "ESP_FRANCO",      # 1945-1977, not the 1938-1945 one
                   "MMR": "MMR_1948",        # Burma, 1948-1974
                   "ROU": "ROU_1952",        # the socialist emblem, 1952-1965
                   "RWA": "RWA_1962"},       # 1962-2001
}

# IDENTIFIED, NOT YET DOWNLOADED.
#
# Each of these is a 1962 flag this map currently gets wrong by inheriting the
# modern one, and each already has its Commons title resolved and sitting in
# tools/data/scenario_flags.json. Commons rate-limited the fetch (HTTP 429, and
# their error asks for a less disruptive approach, so it was not retried
# harder). Re-run tools/download_scenario_flags.py when it will serve them,
# then move the line up into the 1962 plan above.
PENDING = {
    "IRQ": "Iraq 1959-1963 -- the current file is post-1963",
    "LKA": "Ceylon 1951-1972",
    "KHM": "Cambodia 1948-1970",
    "TZA": "Tanganyika 1961-1964",
    "YEM": "North Yemen, from the September 1962 revolution",
}
# Wanted but no such file on Commons under the obvious title; needs a human to
# find the right one: Congo-Leopoldville 1960-1963, Eritrea 1952-1962.


def apply(name, check):
    path = os.path.join(MAPS_DIR, name)
    want = PLAN[name]
    work = tempfile.mkdtemp(prefix="odflagatt_")
    try:
        with zipfile.ZipFile(path) as z:
            entries = z.namelist()
            z.extractall(work)
        cp = os.path.join(work, "countries.json")
        countries = json.load(open(cp))
        by_iso = {v["iso_a3"]: v for v in countries.values()}

        print(f"\n=== {name} ===")
        done = []
        for iso, stem in sorted(want.items()):
            c = by_iso.get(iso)
            if c is None:
                print(f"   skip  {iso} not on this map")
                continue
            src = os.path.join(FLAGS_DIR, stem + ".png")
            if not os.path.exists(src):
                print(f"   WARN  {iso}: data/flags/{stem}.png is missing", file=sys.stderr)
                continue
            rel = f"flags/{stem}.png"
            was = "image" in c.get("flag_actual", {})
            print(f"   {iso:4s} {c['name'][:28]:28s} <- {stem}.png"
                  + ("  (replacing an image)" if was else "  (was procedural)"))
            if not check:
                os.makedirs(os.path.join(work, "flags"), exist_ok=True)
                shutil.copyfile(src, os.path.join(work, rel))
                if rel not in entries:
                    entries.append(rel)
                c["flag_actual"] = {"image": rel}
            done.append(iso)

        if check or not done:
            return 0
        json.dump(countries, open(cp, "w", encoding="utf-8"), separators=(",", ":"))
        tmp = path + ".tmp"
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
            for e in entries:
                if e.endswith("/"):
                    z.writestr(e, b"")
                else:
                    z.write(os.path.join(work, e), e)
        os.replace(tmp, path)
        print(f"   wrote {name} ({len(done)} flags)")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    check = "--check" in sys.argv
    for name in PLAN:
        apply(name, check)
    if PENDING:
        print("\nStill on procedural flags — no file downloaded yet:")
        for iso, who in PENDING.items():
            print(f"   {iso:10s} {who}")
    if check:
        print("\n--check: nothing written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
