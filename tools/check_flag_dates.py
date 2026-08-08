#!/usr/bin/env python3
"""
Is every shipped flag the flag that country actually flew on that date?

    python3 tools/check_flag_dates.py            # report
    python3 tools/check_flag_dates.py --strict   # and exit non-zero if any is wrong

HOW IT KNOWS

It does not have to guess, and neither does anyone reading the report. Wikimedia
names a historical flag file for the years it was flown -- "Flag of Ethiopia
(1897-1974).svg", "Flag of Iran (1964-1980).svg" -- and
tools/audit_flag_licenses.py already records, for every flag this project
ships, the exact Commons filename it came from. So the source data carries its
own validity period, and this reads it back out and compares it against the
scenario's own map_date.

That is the whole trick. Nothing here encodes a human's belief about when a
flag changed; it reports a disagreement between two files already in the tree.

WHAT IT CANNOT SEE

A file with no date range in its name -- most modern flags -- is unverifiable
this way and is reported separately as "undated". That is not a clean bill of
health. The Modern Day map is the only one where an undated file is right by
default; on a 1914 map an undated file means "we shipped the current flag and
nobody has checked", which is how Iran flew the Islamic Republic's flag on the
1962 map and Thailand flew a white elephant seventeen years after it stopped.

Undated files are the backlog. Run with --list-undated to see it per scenario,
worst first, and work down it by adding a dated file to
tools/data/scenario_flags.json and an entry to tools/attach_scenario_flags.py.
"""

import argparse
import json
import os
import re
import sys
import zipfile

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS_DIR)
MAPS_DIR = os.path.join(ROOT, "data", "STDmaps")
FLAGS_JSON = os.path.join(ROOT, "data", "licenses", "flags.json")

# "(1897–1974)", "(1964-1980)", "(1935–1945)", "(2013–2021)", "(1928-present)"
RANGE = re.compile(r"\((\d{4})\s*[-–—]\s*(\d{4}|present)\)")
# "(1949)" alone, or "since 1949"
SINGLE = re.compile(r"\((\d{4})\)")


def span(filename):
    """(first_year, last_year) the file's own name claims, or None."""
    m = RANGE.search(filename or "")
    if m:
        lo = int(m.group(1))
        hi = 9999 if m.group(2) == "present" else int(m.group(2))
        return lo, hi
    m = SINGLE.search(filename or "")
    if m:
        return int(m.group(1)), 9999
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero if a dated flag does not cover its scenario")
    ap.add_argument("--list-undated", action="store_true",
                    help="also list the flags whose file name carries no date")
    args = ap.parse_args()

    try:
        src = json.load(open(FLAGS_JSON, encoding="utf-8"))["flags"]
    except (FileNotFoundError, KeyError, json.JSONDecodeError) as e:
        sys.exit(f"{FLAGS_JSON}: {e}. Run tools/audit_flag_licenses.py first.")
    commons = {k: v.get("file", "") for k, v in src.items()}

    wrong = 0
    for fn in sorted(os.listdir(MAPS_DIR)):
        if not fn.endswith(".odmap"):
            continue
        with zipfile.ZipFile(os.path.join(MAPS_DIR, fn)) as z:
            meta = json.loads(z.read("metadata.json"))
            countries = json.loads(z.read("countries.json"))
        m = re.search(r"(\d{4})", meta.get("map_date", ""))
        if not m:
            continue
        year = int(m.group(1))

        bad, undated, drawn = [], [], []
        for c in countries.values():
            iso = c.get("iso_a3", "")
            if iso in ("UNC", "BLC"):
                continue
            fa = c.get("flag_actual")
            if isinstance(fa, dict) and "type" in fa:
                drawn.append(iso)
                continue
            stem = ""
            if isinstance(fa, dict) and "image" in fa:
                stem = os.path.basename(fa["image"]).rsplit(".", 1)[0]
            if not stem:
                stem = iso
            f = commons.get(stem, "")
            sp = span(f)
            if sp is None:
                undated.append((iso, stem))
            elif not (sp[0] <= year <= sp[1]):
                bad.append((iso, c.get("name", ""), stem, f, sp))

        print(f"\n=== {fn}  [{meta.get('map_date')}] ===")
        print(f"  {len(bad)} flag(s) dated OUTSIDE this scenario, "
              f"{len(undated)} undated, {len(drawn)} still drawn by the engine")
        for iso, name, stem, f, sp in sorted(bad, key=lambda t: t[0]):
            hi = "present" if sp[1] == 9999 else sp[1]
            print(f"    {iso:5s} {name[:26]:26s} {stem:16s} is {sp[0]}-{hi}")
        if drawn:
            print(f"    drawn: {', '.join(sorted(drawn))}")
        if args.list_undated and undated:
            print(f"    undated: {', '.join(i for i, _ in sorted(undated))}")
        wrong += len(bad)

    print(f"\n{wrong} flag(s) carry a date range that does not cover their scenario.")
    return 1 if (args.strict and wrong) else 0


if __name__ == "__main__":
    sys.exit(main())
