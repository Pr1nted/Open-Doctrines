#!/usr/bin/env python3
"""
Can this map be sold?

    python3 tools/check_data_licences.py            # fail if anything cannot
    python3 tools/check_data_licences.py --report   # print the full position

WHY THIS IS A SEPARATE QUESTION FROM THE GAME'S LICENCE

OpenDoctrines ships under a licence of its own choosing, currently the
OpenDoctrines Non-Commercial License, and the project can change that whenever
it likes -- it owns the code. It does not own the map data. Every dataset that
reaches a shipped .odmap arrived under somebody else's terms, and those terms
do not change because the game's do.

So there are two failure modes, and only the second one is dangerous:

  The game is free today and might be sold later.  Fine, and entirely the
  project's call, PROVIDED every third-party input already permits commercial
  use. A dataset that forbids it has to be found now, while there is still a
  cheap way out, and not on the day somebody decides to charge for this.

  A dataset forbids commercial use, or is silent.  Then the map cannot be sold
  at any price without renegotiating or replacing that input, and "silent" is
  as bad as "no": no grant means no permission.

gen_notices.py already checks that every input is RECORDED. This checks that
every recorded input is USABLE, which is the part that cannot be fixed by
writing more attribution.

WHAT IS AND IS NOT ACCEPTED

Accepted: public domain and its equivalents, CC0, CC BY, CC BY-SA, and open
government licences that grant commercial re-use. CC BY-SA is accepted with a
flag on it -- share-alike binds the derived image, not the game, but it is an
obligation and this prints how many carry it.

Refused: any NonCommercial or NoDerivatives clause, anything under GPL or
another copyleft that would fight a proprietary release, "free for
non-commercial use", "research use only", and -- most often -- silence. The
project has already turned down GREG and ACOR on exactly that last ground; see
the removed_data section of tools/provenance.json.

THE DEVELOPMENT-ONLY REFERENCE

tools/check_map_history.py reads aourednik/historical-basemaps, which is
GPL-3.0. Nothing from it is copied into a map and the repository does not
contain it: the tool prints a report about provinces, a person reads the
report, and the province-to-country tables in tools/fix_map_history.py are
edited by hand with the reason written out. What ships is which country held
which ground in a given year, at this project's own raster resolution -- a
historical fact, and facts are not copyrightable in either the US (Feist) or
the EU, where the database right protects a substantial extraction of the
contents rather than the facts themselves.

That reasoning is why the check below asserts the dataset is NOT present in
the tree. The argument only holds while it stays a reference that a human
reads. Vendoring it, or generating the correction tables from it
automatically, would turn a report into a derivation, and this check is what
notices.
"""

import argparse
import json
import os
import re
import sys
import zipfile

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS_DIR)
PROVENANCE = os.path.join(TOOLS_DIR, "provenance.json")
FLAGS_JSON = os.path.join(ROOT, "data", "licenses", "flags.json")
MAPS_DIR = os.path.join(ROOT, "data", "STDmaps")

# canonical licence -> the obligation it leaves on a commercial release.
#
# Keyed on canon() output, not on the exact string, because the same licence is
# written four ways across this project: provenance.json says "Public domain
# (work of the US Government, 17 U.S.C. 105)", the Wikimedia audit says "Public
# domain", and a map's metadata.json says "CC-BY-4.0" in SPDX form. A table
# keyed on the literal text turns a licence check into a spelling check and
# fails on the day somebody writes a correct licence a new way.
COMMERCIAL_OK = {
    "public domain": "none",
    "pd": "none",
    "cc0": "none",
    "cc0 1.0": "none",
    "cc by 1.0": "attribution",
    "cc by 2.0": "attribution",
    "cc by 2.5": "attribution",
    "cc by 3.0": "attribution",
    "cc by 4.0": "attribution",
    "cc by sa 1.0": "attribution + share-alike on the asset",
    "cc by sa 2.0": "attribution + share-alike on the asset",
    "cc by sa 2.5": "attribution + share-alike on the asset",
    "cc by sa 3.0": "attribution + share-alike on the asset",
    "cc by sa 4.0": "attribution + share-alike on the asset",
    "ogl om 1.0": "attribution",
    # Per-file terms, resolved by the per-flag audit below rather than here.
    "public domain cc0 and ogl om 1.0 per file": "per file, see FLAGS.md",
}


def canon(s):
    """Normalise a licence string enough to look it up.

    Drops parentheticals -- which is where the citations and the statutory
    references live -- and flattens the separators, so "CC-BY-4.0",
    "CC BY 4.0" and "cc_by_4.0" are one key. Deliberately runs AFTER the
    refusal scan below, so nothing here can launder a "-NC" into an accepted
    form by tidying it away.
    """
    s = re.sub(r"\(.*?\)", " ", s or "")
    s = s.lower().replace("_", "-")
    s = re.sub(r"[^a-z0-9.\- ]+", " ", s)
    s = s.replace("-", " ")
    return re.sub(r"\s+", " ", s).strip()

# Words that mean "not for sale" however they are dressed up. Matched
# case-insensitively against a licence string, so a new entry that says
# "CC BY-NC 4.0" or "free for non-commercial use" is caught on arrival.
REFUSE = ["noncommercial", "non-commercial", "non commercial", "-nc",
          "noderiv", "-nd", "research use", "academic use only",
          "all rights reserved", "gpl", "agpl"]

# Boundary datasets that must never end up inside this repository, whether as
# a reference a tool reads or as data a map ships. Checked August 2026:
#
#   historical-basemaps  GPL-3.0. Read as a report by check_map_history.py,
#                        from a directory outside the tree. See below.
#   cshapes              CC BY-NC-SA 4.0. NonCommercial -- disqualified.
#   gadm                 Non-commercial, redistribution needs permission.
#   euratlas, geacron    Commercial, per-seat.
#
# Deliberately NOT here: OpenHistoricalMap (CC0) and geoBoundaries (CC BY 4.0),
# both of which may be shipped. Add them to provenance.json first, like any
# other input, so the notices and credits pick them up.
REFERENCE_ONLY = ["historical-basemaps", "cshapes", "gadm", "euratlas", "geacron"]


def fail(msgs, m):
    msgs.append(m)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--report", action="store_true",
                    help="print every input and the obligation it carries")
    args = ap.parse_args()

    problems = []
    notes = []

    with open(PROVENANCE, encoding="utf-8") as f:
        prov = json.load(f)

    # ── 1. every dataset that reaches a map ──────────────────────
    for entry in prov.get("data", []):
        lic = entry["license"]
        low = lic.lower()
        hit = next((w for w in REFUSE if w in low), None)
        if hit:
            fail(problems, f"{entry['name']}: licence {lic!r} contains {hit!r} "
                           f"-- cannot be shipped commercially")
            continue
        key = canon(lic)
        if key not in COMMERCIAL_OK:
            fail(problems, f"{entry['name']}: licence {lic!r} is not in the "
                           f"accepted list. Read the terms, then add it to "
                           f"COMMERCIAL_OK with the obligation it leaves, or "
                           f"replace the dataset.")
            continue
        notes.append((entry["name"], lic, COMMERCIAL_OK[key],
                      entry.get("used_by", "")))

    # ── 2. every flag, which ships inside the .odmap ─────────────
    try:
        with open(FLAGS_JSON, encoding="utf-8") as f:
            flags = json.load(f)["flags"]
    except (FileNotFoundError, KeyError, json.JSONDecodeError) as e:
        fail(problems, f"{FLAGS_JSON}: unreadable ({e}). Regenerate with "
                       f"tools/audit_flag_licenses.py")
        flags = {}

    share_alike = []
    for iso, rec in sorted(flags.items()):
        lic = rec.get("license", "UNKNOWN")
        low = lic.lower()
        hit = next((w for w in REFUSE if w in low), None)
        if hit:
            fail(problems, f"flag {iso} ({rec.get('file')}): {lic!r} contains "
                           f"{hit!r}")
        elif canon(lic) not in COMMERCIAL_OK:
            fail(problems, f"flag {iso} ({rec.get('file')}): {lic!r} is not in "
                           f"the accepted list")
        elif "share-alike" in COMMERCIAL_OK[canon(lic)]:
            share_alike.append((iso, lic, rec.get("file")))

    # ── 3. the licence each map declares of ITSELF ───────────────
    #
    # A map states its own terms in metadata.json, and the map browser shows
    # them. Declaring something the project is not in a position to grant --
    # because an input forbids it -- is the same breach as omitting the
    # attribution, arriving from the other direction.
    for fn in sorted(os.listdir(MAPS_DIR)) if os.path.isdir(MAPS_DIR) else []:
        if not fn.endswith(".odmap"):
            continue
        with zipfile.ZipFile(os.path.join(MAPS_DIR, fn)) as z:
            try:
                meta = json.loads(z.read("metadata.json"))
            except KeyError:
                fail(problems, f"{fn}: no metadata.json")
                continue
        lic = meta.get("license", "")
        low = lic.lower()
        hit = next((w for w in REFUSE if w in low), None)
        if hit:
            fail(problems, f"{fn} declares itself {lic!r}, which contains "
                           f"{hit!r} -- that forbids the commercial use the "
                           f"inputs allow")
        elif canon(lic) not in COMMERCIAL_OK:
            fail(problems, f"{fn} declares itself {lic!r}, which is not a "
                           f"licence this check recognises")

    # ── 4. the reference dataset must not be in the tree ─────────
    for dirpath, dirnames, filenames in os.walk(ROOT):
        if any(part in dirpath for part in (".git", "build", "node_modules",
                                            ".toolchains", "_deps")):
            dirnames[:] = []
            continue
        for name in list(dirnames) + filenames:
            low = name.lower()
            for banned in REFERENCE_ONLY:
                if banned in low:
                    rel = os.path.relpath(os.path.join(dirpath, name), ROOT)
                    fail(problems, f"{rel}: {banned} must stay outside this "
                                   f"repository. It is a report a human reads, "
                                   f"not an input -- see the module docstring.")

    # ── report ───────────────────────────────────────────────────
    if args.report or problems:
        print("Data that reaches a shipped map:")
        for name, lic, obl, used in notes:
            print(f"  {name}")
            print(f"      {lic}  ->  obligation: {obl}")
            if used:
                print(f"      via {used}")
        print(f"\nFlag artwork: {len(flags)} files, "
              f"{len(flags) - len(share_alike)} with no share-alike obligation.")
        if share_alike:
            print(f"  {len(share_alike)} carry share-alike on the image itself. "
                  f"Commercial use is permitted; redistributing a MODIFIED "
                  f"version of one of these images means releasing that image "
                  f"under the same licence. The game is not affected.")
            for iso, lic, fn in share_alike:
                print(f"    {iso:8s} {lic:14s} {fn}")

    if problems:
        print("\nThis map could not be shipped commercially as it stands:\n")
        for p in problems:
            print(f"  - {p}")
        return 1

    print(f"\nEvery input to the shipped maps permits commercial use "
          f"({len(notes)} datasets, {len(flags)} flags). "
          f"{len(share_alike)} flag(s) carry share-alike; the rest are "
          f"attribution-only or public domain.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
