#!/usr/bin/env python3
"""Assemble a clean, shippable copy of the game.

    tools/package.py --binary build/OpenDoctrines --out dist/OpenDoctrines-macos
    tools/package.py --binary build/OpenDoctrines --out dist/x --zip

WHAT "CLEAN" MEANS AND WHY IT IS AN ALLOWLIST

A working copy of data/ accumulates the developer's own saves, the worlds they
imported, the mods they installed and enabled, their config, AI training
exports. None of that belongs in a release: at best it is confusing, at worst
it leaks local paths and play history to everyone who downloads the game.

So what ships is an explicit allowlist. A denylist fails the wrong way -- the
day someone adds data/telemetry/ or data/scratch/, a denylist ships it because
nobody remembered to add it, while an allowlist simply leaves it out. Adding
shipped content should be a deliberate edit to this file.

Anything excluded is reported, not silently dropped, so an accidental omission
is visible in the build log.
"""

import argparse
import os
import shutil
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release as rel   # reuse the one allowlist, do not restate it
import odver

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024.0


def tree_size(path):
    if os.path.isfile(path):
        return os.path.getsize(path)
    total = 0
    for dirpath, _, files in os.walk(path):
        for f in files:
            fp = os.path.join(dirpath, f)
            if os.path.exists(fp):
                total += os.path.getsize(fp)
    return total


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True,
                    help="the built game (a file, or a .app bundle on macOS)")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--zip", action="store_true", help="also produce <out>.zip")
    args = ap.parse_args()

    version = odver.read()
    src_data = os.path.join(ROOT, "data")
    out = os.path.abspath(args.out)

    if not os.path.exists(args.binary):
        print(f"FAILED: no such binary: {args.binary}", file=sys.stderr)
        return 1

    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)

    print(f"packaging OpenDoctrines {version}")

    # --- the binary ---
    base = os.path.basename(args.binary.rstrip("/"))
    dst_bin = os.path.join(out, base)
    if os.path.isdir(args.binary):          # macOS .app bundle
        shutil.copytree(args.binary, dst_bin, symlinks=True)
    else:
        shutil.copy2(args.binary, dst_bin)
        os.chmod(dst_bin, 0o755)
    print(f"  binary   {base}  ({human(tree_size(dst_bin))})")

    # --- data, allowlisted ---
    dst_data = os.path.join(out, "data")
    os.makedirs(dst_data)
    shipped, missing = [], []
    for name in rel.DATA_ALLOWLIST:
        s = os.path.join(src_data, name)
        if not os.path.exists(s):
            missing.append(name)
            continue
        d = os.path.join(dst_data, name)
        if os.path.isdir(s):
            shutil.copytree(s, d, symlinks=False,
                            ignore=shutil.ignore_patterns(".DS_Store", "Icon*"))
        else:
            shutil.copy2(s, d)
        shipped.append((name, tree_size(d)))

    print("  data shipped:")
    for name, size in shipped:
        print(f"    {name:16s} {human(size)}")
    if missing:
        # Loud, because a missing allowlisted item means a broken release, not
        # a smaller one.
        print("  MISSING from data/ (release will be incomplete):")
        for m in missing:
            print(f"    {m}")

    # --- what was deliberately left out ---
    present = set(os.listdir(src_data)) if os.path.isdir(src_data) else set()
    excluded = sorted(present - set(rel.DATA_ALLOWLIST))
    if excluded:
        print("  excluded (user data / local state):")
        for e in excluded:
            tag = "" if e in rel.KNOWN_USER_DATA else "   <- not in the known list, check it"
            print(f"    {e}{tag}")

    # A release that still contains a save or a mod is a bug in this script, so
    # it is checked rather than assumed.
    leaked = []
    for dirpath, dirnames, files in os.walk(out):
        for f in files:
            if f.endswith((".odsv", ".odmod", ".kv")) or f == "mods.json" or f == "config.json":
                leaked.append(os.path.relpath(os.path.join(dirpath, f), out))
    if leaked:
        print("\nFAILED: user data reached the package:", file=sys.stderr)
        for l in leaked:
            print(f"    {l}", file=sys.stderr)
        return 1

    # Where the game will write the player's own files. Created empty so a
    # fresh install has somewhere to put them without needing to mkdir at
    # runtime as a first action.
    for d in ("saves", "custom_maps", "mods", "exports"):
        os.makedirs(os.path.join(dst_data, d), exist_ok=True)

    with open(os.path.join(out, "VERSION"), "w") as f:
        f.write(str(version) + "\n")

    total = tree_size(out)
    print(f"\n  {out}  ({human(total)})")

    if args.zip:
        zpath = out + ".zip"
        with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
            for dirpath, _, files in os.walk(out):
                for f in files:
                    fp = os.path.join(dirpath, f)
                    z.write(fp, os.path.relpath(fp, os.path.dirname(out)))
        print(f"  {zpath}  ({human(os.path.getsize(zpath))})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
