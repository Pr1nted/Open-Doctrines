#!/usr/bin/env python3
"""
Which countries have their real parties, and which are still generating?

    python3 tools/check_party_coverage.py            # report
    python3 tools/check_party_coverage.py --strict   # and fail on a real fault
    python3 tools/check_party_coverage.py --backlog  # the gap, worst first

WHAT IT CHECKS, AND THE DIFFERENCE THAT MATTERS

Two completely different things live in this report and only one of them is a
failure.

A FAULT is a table that can never load: an ISO code data/parties.json names
that the scenario's own countries.json does not have. The 1914 map calls the
German Empire GER and Austria-Hungary AUH; a table keyed DEU and AUT parses
fine, validates fine, and silently never applies -- which is indistinguishable
from "nobody has written Germany yet" unless something compares the two files.
Five of the nine entries first written for the pre-1945 maps were wrong this
way. --strict fails on these.

THE BACKLOG is a country with no entry at all. That is not a failure: the
shipped maps carry 55 to 185 countries and a fabricated party for one nobody
checked is worse than a generated one, which is at least honest about being a
description. tools/check_flag_dates.py draws the same line for flags, and for
the same reason -- Iran flew the Islamic Republic's flag on a 1962 map because
an unverifiable gap read as done.

ALT-HISTORY MAPS ARE NOT IN THE DENOMINATOR. 1936cp, 1962ax, 1984 and mars are
worlds where the recorded history did not happen, so they have no real parties
to be missing and counting them as uncovered would make the backlog a number
nobody can ever close.
"""

import argparse
import json
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAPS = os.path.join(ROOT, "data", "STDmaps")

# Worlds with a recorded history. The rest generate by design; see the module
# docstring and data/parties.json's own header.
REAL_WORLD = ["1914", "1918", "1939", "1945", "1962", "map"]


def map_isos(scenario):
    """Every ISO A3 the scenario's own country table uses, or None."""
    path = os.path.join(MAPS, scenario + ".odmap")
    if not os.path.exists(path):
        return None
    try:
        with zipfile.ZipFile(path) as z:
            data = json.loads(z.read("countries.json"))
    except (KeyError, OSError, ValueError):
        return None
    rows = list(data.values()) if isinstance(data, dict) else data
    out = set()
    for c in rows:
        if not isinstance(c, dict):
            continue
        iso = c.get("isoA3") or c.get("iso_a3") or c.get("iso")
        # UNC and BLC are the unclaimed and blocked pseudo-countries; they have
        # no politics and must not sit in the denominator.
        if iso and iso not in ("UNC", "BLC", "SPC"):
            out.add(iso)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero if any table names a country its map does not have")
    ap.add_argument("--backlog", action="store_true",
                    help="list the uncovered countries per scenario, worst first")
    args = ap.parse_args()

    with open(os.path.join(ROOT, "data", "parties.json"), encoding="utf-8") as f:
        doc = json.load(f)
    scenarios = doc.get("scenarios", {})

    faults = []
    rows = []
    for scen in REAL_WORLD:
        isos = map_isos(scen)
        if isos is None:
            faults.append(f"{scen}: no such map, or its countries.json could not be read")
            continue
        table = scenarios.get(scen, {})
        keyed = {k for k in table if isinstance(table.get(k), dict) and "parties" in table[k]}

        unknown = sorted(keyed - isos)
        for u in unknown:
            faults.append(f"{scen}: '{u}' is not a country on this map -- that table never loads")

        covered = sorted(keyed & isos)
        missing = sorted(isos - keyed)
        parties = sum(len(table[k]["parties"]) for k in covered)
        rows.append((scen, len(covered), len(isos), parties, missing))

    # Any scenario keyed in the file that is not a map at all.
    for scen in scenarios:
        if scen in REAL_WORLD:
            continue
        if not os.path.exists(os.path.join(MAPS, scen + ".odmap")):
            faults.append(f"'{scen}' is keyed in parties.json but no such map ships")
        else:
            print(f"note: '{scen}' is an alt-history map with party data; "
                  f"that is allowed but it is not counted as coverage")

    print("scenario   countries on record   of total   parties   coverage")
    total_cov = total_all = 0
    for scen, cov, all_, parties, _ in rows:
        total_cov += cov
        total_all += all_
        print(f"{scen:<10} {cov:>19}   {all_:>8}   {parties:>7}   {100.0 * cov / all_:>6.1f}%")
    if total_all:
        print(f"{'ALL':<10} {total_cov:>19}   {total_all:>8}   {'':>7}   "
              f"{100.0 * total_cov / total_all:>6.1f}%")

    if args.backlog:
        print("\nTHE BACKLOG -- countries generating, worst scenario first.")
        print("Not a failure: a generated party is honest, an invented one is not.")
        for scen, cov, all_, _, missing in sorted(rows, key=lambda r: r[2] - r[1], reverse=True):
            print(f"\n{scen} ({len(missing)} of {all_}):")
            for i in range(0, len(missing), 14):
                print("   " + " ".join(missing[i:i + 14]))

    if faults:
        print("\nFAULTS -- these tables cannot work, whatever the coverage says:")
        for f_ in faults:
            print("  " + f_)
    else:
        print("\nno faults: every table names a country its own map has")

    return 1 if (faults and args.strict) else 0


if __name__ == "__main__":
    sys.exit(main())
