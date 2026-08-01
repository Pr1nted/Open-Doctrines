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
                   "PAN": "PAN", "LUX": "LUX", "MNG": "MNG_BOGD"},
    "1918.odmap": {"TIB": "TIB", "NPL": "NPL_PRE1962", "BTN": "BTN_1949",
                   "PAN": "PAN", "LUX": "LUX", "UKR": "UKR",
                   "MNG": "MNG_BOGD", "MNE": "MNE_KINGDOM",
                   "NJD": "NEJD", "HJZ": "HEJAZ"},
    "1939.odmap": {"TIB": "TIB", "NPL": "NPL_PRE1962", "BTN": "BTN_1949",
                   "PAN": "PAN", "LUX": "LUX", "IRL": "IRL",
                   "EGY": "EGY_KINGDOM", "YEM": "YEM_KINGDOM",
                   "MNG": "MNG_MPR1939", "IRQ": "IRQ_KINGDOM", "MCK": "MCK",
                   "SVK": "SVK"},
    "1945.odmap": {"TIB": "TIB", "NPL": "NPL_PRE1962", "BTN": "BTN_1949",
                   "PAN": "PAN", "LUX": "LUX", "EGY": "EGY_KINGDOM",
                   "JPN": "JPN", "LBN": "LBN", "AUT": "AUT",
                   "IRQ": "IRQ_KINGDOM", "SYR": "SYR_1932",
                   "YEM": "YEM_KINGDOM"},
    "1962.odmap": {"RVN": "VNM_SOUTH"},
}

# Still procedural, because Commons rate-limited the fetch and these have no
# file yet. Re-run download_scenario_flags.py and add them here.
PENDING = {}   # all downloaded


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
