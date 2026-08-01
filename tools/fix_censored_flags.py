#!/usr/bin/env python3
"""Rewrite `flag_censored` in the shipped .odmap files.

"Show actual flags" is a censoring option. Turning it off used to replace every
nation's flag with a solid rectangle in its map colour, because
generate_scenario.py had no better fallback -- 28 of 65 nations in 1939,
including Ireland, Luxembourg and Panama, none of which has anything to censor.

The base map already had the right shape: `flag_censored` is the SAME image as
`flag_actual` with `"censored": true`, which makes FlagRenderer mosaic it. Only
the flags that need obscuring carry that marker; everyone else's entry is
identical to their real flag, so the toggle does nothing for them.

This applies that shape to the scenario maps, which were generated before
generate_scenario.py knew about it. Rules, in order:

    authored `flag_censored` in tools/data/scenarios/<id>.json
        -> that design, censored: true      (the 1939 Reich tricolour)
    flag basename listed in tools/data/scenario_flags.json "censor"
        -> the real flag, censored: true    (mosaicked)
    anything else
        -> the real flag, censored: false   (toggle is a no-op)

Only countries.json inside each archive is touched; every other member is
copied through byte for byte.

    python3 tools/fix_censored_flags.py            # patch data/STDmaps/*.odmap
    python3 tools/fix_censored_flags.py --dry-run  # report, change nothing
"""

import argparse
import glob
import json
import os
import shutil
import sys
import zipfile

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(TOOLS_DIR)
TOOLS_DATA = os.path.join(TOOLS_DIR, "data")
STDMAPS = os.path.join(PROJECT_ROOT, "data", "STDmaps")


def load_censor_set():
    with open(os.path.join(TOOLS_DATA, "scenario_flags.json")) as f:
        return set(json.load(f).get("censor", []))


def load_authored():
    """flag basename -> authored flag_censored, unioned over every scenario.

    Keyed by the flag, NOT by the ISO. The archive does not record which
    scenario built it, and an ISO is reused across eras for different flags:
    "GER" is the Reich in 1939 and the Empire in 1914, and the Reich's authored
    replacement has no business being stamped on the Empire's flag.
    """
    out = {}
    for path in sorted(glob.glob(os.path.join(TOOLS_DATA, "scenarios", "*.json"))):
        with open(path) as f:
            scen = json.load(f)
        for power in scen.get("powers", []):
            if not power.get("flag_censored"):
                continue
            key = power.get("flag_file") or flag_basename(power.get("flag"))
            if key:
                out[key] = power["flag_censored"]
    return out


def flag_basename(flag):
    """`{"image": "flags/SOV.png"}` -> `SOV`; procedural patterns -> ""."""
    if not isinstance(flag, dict) or not flag.get("image"):
        return ""
    return os.path.splitext(os.path.basename(flag["image"]))[0]


def censored_for(actual, censor, authored):
    name = flag_basename(actual)
    if name in authored:
        return dict(authored[name], censored=True)
    # `censored` is omitted rather than written as false, which is how the base
    # map already spells "nothing to censor here" — so a map that was already
    # right is left byte-identical instead of being rewritten cosmetically.
    return dict(actual, censored=True) if name in censor else dict(actual)


def patch_countries(raw, censor, authored):
    """Returns (new json text, changes, every nation left censored)."""
    countries = json.loads(raw)
    changes, censored = [], []
    for key, c in countries.items():
        if not isinstance(c, dict):
            continue
        actual = c.get("flag_actual")
        if not isinstance(actual, dict):
            continue
        iso = c.get("iso_a3", key)
        want = censored_for(actual, censor, authored)
        if want.get("censored"):
            censored.append((iso, want))
        if c.get("flag_censored") != want:
            changes.append((iso, c.get("flag_censored"), want))
            c["flag_censored"] = want
    return json.dumps(countries, indent=1, ensure_ascii=False), changes, censored


def patch_odmap(path, censor, authored, dry_run):
    with zipfile.ZipFile(path) as z:
        if "countries.json" not in z.namelist():
            print(f"  {os.path.basename(path)}: no countries.json — skipped")
            return 0
        members = [(i, z.read(i.filename)) for i in z.infolist()]

    new_json, changes, censored = None, [], []
    for info, data in members:
        if info.filename == "countries.json":
            new_json, changes, censored = patch_countries(data, censor, authored)

    name = os.path.basename(path)
    verb = "would restore" if dry_run else "restored"
    print(f"  {name}: {verb} {len(changes)} real flag(s), "
          f"{len(censored)} censored")
    for iso, entry in censored:
        print(f"      {iso}: {json.dumps(entry)}")
    if not changes:
        print("      (already correct — not rewritten)")
        return 0
    if dry_run:
        return len(changes)

    # Write beside the original and swap, so an interrupted run cannot leave a
    # half-written map where a working one used to be.
    tmp = path + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as out:
        for info, data in members:
            if info.filename == "countries.json":
                data = new_json.encode("utf-8")
            # Carry the original member's metadata so unrelated entries keep
            # their timestamps and compression choices.
            zi = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            zi.compress_type = info.compress_type
            zi.external_attr = info.external_attr
            out.writestr(zi, data)
    shutil.move(tmp, path)
    return len(changes)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("maps", nargs="*", help="odmap paths (default: data/STDmaps/*.odmap)")
    ap.add_argument("--dry-run", action="store_true", help="report, change nothing")
    args = ap.parse_args()

    censor = load_censor_set()
    authored = load_authored()
    print(f"censor set ({len(censor)}): {', '.join(sorted(censor))}")
    print(f"authored replacements: {', '.join(sorted(authored)) or 'none'}")

    maps = args.maps or sorted(glob.glob(os.path.join(STDMAPS, "*.odmap")))
    if not maps:
        print("no .odmap files found", file=sys.stderr)
        return 1
    total = sum(patch_odmap(m, censor, authored, args.dry_run) for m in maps)
    print(f"{'would rewrite' if args.dry_run else 'rewrote'} {total} flag entr"
          f"{'y' if total == 1 else 'ies'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
