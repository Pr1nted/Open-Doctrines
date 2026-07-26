#!/usr/bin/env bash
# Packs a directory into a .odmod.
#
# MANIFEST.json is added first because the loader requires it to be the archive's
# first entry -- that is what lets it validate a mod's declared limits before
# decompressing anything else.
#
# Usage: tools/pack_odmod.sh <dir> [out.odmod]
set -eu

dir="${1:?usage: pack_odmod.sh <dir> [out.odmod]}"
out="${2:-$(basename "$dir").odmod}"

[ -f "$dir/MANIFEST.json" ] || { echo "$dir has no MANIFEST.json"; exit 1; }
[ -f "$dir/mod.wasm" ]      || { echo "$dir has no mod.wasm"; exit 1; }

out="$(cd "$(dirname "$out")" && pwd)/$(basename "$out")"
rm -f "$out"

cd "$dir"
zip -q -X "$out" MANIFEST.json
zip -q -X "$out" mod.wasm
[ -f thumbnail.png ] && zip -q -X "$out" thumbnail.png
[ -f signature.bin ] && zip -q -X "$out" signature.bin
[ -d data ] && zip -q -X -r "$out" data
echo "packed $out ($(wc -c < "$out" | tr -d ' ') bytes)"
