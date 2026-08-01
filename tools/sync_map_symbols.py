#!/usr/bin/env python3
"""
Copy the current data/symbols/ into every shipped scenario.

    python3 tools/sync_map_symbols.py --check   # report drift, change nothing
    python3 tools/sync_map_symbols.py           # rewrite the .odmap archives

WHY THIS EXISTS

Every .odmap carries its own symbols/ directory. That is right for custom maps,
which have to be self-contained, but it means the shipped scenarios froze a
copy of data/symbols/ on the day they were packaged and never heard about it
again. So the symbol fixes never reached the game: the maps kept serving the
old files, and only the ones nothing referenced looked correct.

The visible symptom was Nepal. Its flag draws a crescent, the bundled
crescent.svg was the pre-fix one whose void is painted black instead of left
transparent, and so Nepal flew a crimson flag with a black blob on it -- while
data/symbols/crescent.svg on disk had been correct for hours. Twenty-four of
the twenty-eight symbols had drifted the same way.

WHAT IT DOES

For each scenario: overwrite every symbols/*.svg with the file of that name in
data/symbols/, add the ones the map is missing, and drop the ones no longer
shipped. Dropping is refused if any country's flag still references the symbol,
so this cannot silently blank a flag -- run it and read what it says.
"""

import hashlib
import json
import os
import shutil
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYMBOL_DIR = os.path.join(ROOT, "data", "symbols")
MAPS_DIR = os.path.join(ROOT, "data", "STDmaps")


def digest(b):
    return hashlib.sha1(b).hexdigest()[:10]


def shipped():
    return {f: open(os.path.join(SYMBOL_DIR, f), "rb").read()
            for f in sorted(os.listdir(SYMBOL_DIR)) if f.endswith(".svg")}


def sync(name, want, check):
    path = os.path.join(MAPS_DIR, name)
    work = tempfile.mkdtemp(prefix="odsymsync_")
    try:
        with zipfile.ZipFile(path) as z:
            entries = z.namelist()
            z.extractall(work)
            have = {n.split("/", 1)[1]: z.read(n) for n in entries
                    if n.startswith("symbols/") and n.endswith(".svg")}
            flags = z.read("countries.json").decode("utf-8", "replace")

        stale = [f for f, b in have.items() if f in want and digest(b) != digest(want[f])]
        missing = [f for f in want if f not in have]
        extra = [f for f in have if f not in want]

        # Never drop a symbol a flag still names.
        keep = [f for f in extra if os.path.splitext(f)[0] in flags]
        drop = [f for f in extra if f not in keep]

        print(f"\n=== {name} ===")
        print(f"   {len(stale):2d} stale, {len(missing):2d} missing, {len(drop):2d} to drop"
              + (f", {len(keep)} kept (still referenced: {', '.join(keep)})" if keep else ""))
        if stale:
            print(f"      stale: {', '.join(sorted(stale))}")
        if missing:
            print(f"      added: {', '.join(sorted(missing))}")
        if drop:
            print(f"      dropped: {', '.join(sorted(drop))}")
        if check or not (stale or missing or drop):
            return 0

        for f in stale + missing:
            with open(os.path.join(work, "symbols", f), "wb") as fh:
                fh.write(want[f])
        for f in drop:
            os.remove(os.path.join(work, "symbols", f))
            entries = [e for e in entries if e != "symbols/" + f]
        entries += ["symbols/" + f for f in missing]

        tmp = path + ".tmp"
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
            for e in entries:
                if e.endswith("/"):
                    z.writestr(e, b"")
                else:
                    z.write(os.path.join(work, e), e)
        os.replace(tmp, path)
        print(f"   wrote {name}")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    check = "--check" in sys.argv
    want = shipped()
    print(f"data/symbols/ has {len(want)} symbols")
    for name in sorted(os.listdir(MAPS_DIR)):
        if name.endswith(".odmap"):
            sync(name, want, check)
    if check:
        print("\n--check: nothing written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
