#!/usr/bin/env bash
# Builds hello-panel-zig.odmod. Needs zig on PATH (or ZIG= pointing at one).
#
# This is the plain `zig build-exe` path. It uses only compiler flags, which
# have been far more stable across Zig releases than the build.zig API, so it
# is the one to reach for first. `../../build.zig` does the same thing through
# the build system if you prefer that.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
sdkzig="$(cd "$here/../.." && pwd)"
root="$(cd "$sdkzig/../.." && pwd)"

ZIG="${ZIG:-zig}"
command -v "$ZIG" >/dev/null 2>&1 || { echo "no zig found (set ZIG=)"; exit 1; }

# Built from $here so the compilation cache lands next to the example rather
# than wherever you happened to invoke this from.
cd "$here"

# --dep/-M pass gearbox.zig as a named module. It cannot be a relative @import:
# Zig refuses to import a file outside the root module's directory, and the SDK
# lives two levels up. For your own mod the simpler answer is to copy
# gearbox.zig next to your source and @import("gearbox.zig") directly.
"$ZIG" build-exe \
    -target wasm32-freestanding \
    -O ReleaseSmall \
    -fno-entry \
    --export=mod_load \
    --export=mod_unload \
    --export=mod_draw_panel \
    -femit-bin="$here/mod.wasm" \
    --dep gearbox \
    -Mroot="$here/main.zig" \
    -Mgearbox="$sdkzig/gearbox.zig"

"$root/tools/pack_odmod.sh" "$here" "$here/hello-panel-zig.odmod"
