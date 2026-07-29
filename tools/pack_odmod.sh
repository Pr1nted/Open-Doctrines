#!/usr/bin/env bash
# Packs a directory into a .odmod.
#
# MANIFEST.json is added first because the loader requires it to be the archive's
# first entry -- that is what lets it validate a mod's declared limits before
# decompressing anything else.
#
# Usage: tools/pack_odmod.sh <dir> [out.odmod]
#
# WHY PYTHON RATHER THAN THE zip CLI
#
# This used to shell out to `zip`, and Git Bash on Windows does not have it --
# nor does the Windows CI runner, where the whole test suite failed on a single
# `zip: command not found` after every other check had passed. Windows is a
# supported platform for playing AND for building mods, so a packer that only
# runs on three of the four is not a packer.
#
# 7z is on the Windows image, but that would mean two implementations of the one
# rule that matters here -- MANIFEST.json first -- diverging on the platform
# nobody tests by hand. python3 is already required by the rest of the tooling
# (tools/help.py, gen_bindings.py, gen_notices.py and the test suite all use it),
# its zipfile writes entries in call order on every platform, and one packer
# everywhere cannot drift.
set -eu

dir="${1:?usage: pack_odmod.sh <dir> [out.odmod]}"
out="${2:-$(basename "$dir").odmod}"

[ -f "$dir/MANIFEST.json" ] || { echo "$dir has no MANIFEST.json"; exit 1; }
[ -f "$dir/mod.wasm" ]      || { echo "$dir has no mod.wasm"; exit 1; }

out="$(cd "$(dirname "$out")" && pwd)/$(basename "$out")"
rm -f "$out"

python3 - "$dir" "$out" <<'PY'
import sys, zipfile
from pathlib import Path

src, out = Path(sys.argv[1]), Path(sys.argv[2])

# Deflate, matching what `zip` did by default: the size limits in MANIFEST.json
# are checked against the compressed archive, and storing everything would push
# mods that used to fit over them.
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    # Order is the contract. MANIFEST.json first, then the module; the rest is
    # whatever the mod ships.
    names = ["MANIFEST.json", "mod.wasm", "thumbnail.png", "signature.bin"]
    for name in names:
        f = src / name
        if f.is_file():
            z.write(f, name)

    # Sorted, so the same directory packs to the same archive twice running --
    # os.walk's order is the filesystem's, which differs between machines and
    # made two builds of one mod impossible to compare.
    data = src / "data"
    if data.is_dir():
        for f in sorted(p for p in data.rglob("*") if p.is_file()):
            # as_posix(), because a zip entry is always forward-slashed. Windows
            # would otherwise write data\foo.png as a literal name and the mod
            # would look empty to a loader on any other platform.
            z.write(f, f.relative_to(src).as_posix())

print(f"packed {out} ({out.stat().st_size} bytes)")
PY
