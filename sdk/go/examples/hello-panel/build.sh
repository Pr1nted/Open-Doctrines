#!/usr/bin/env bash
# Builds hello-panel-go.odmod. Needs tinygo on PATH (or TINYGO= pointing at one).
#
# TinyGo, not go. Standard Go cannot produce a module the host will load:
# GOOS=js needs JavaScript glue that does not exist here, and GOOS=wasip1 emits
# WASI imports the host deliberately does not link. See ../../README.md.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
sdkgo="$(cd "$here/../.." && pwd)"
root="$(cd "$sdkgo/../.." && pwd)"

TINYGO="${TINYGO:-tinygo}"
command -v "$TINYGO" >/dev/null 2>&1 || { echo "no tinygo found (set TINYGO=)"; exit 1; }

# Run from the module root so the import of the gearbox package resolves
# locally, with no network access.
cd "$sdkgo"

#   -target=wasm-unknown   bare wasm32: no WASI, no JS. The only target whose
#                          import list the host will accept.
#   -no-debug              drops DWARF. A mod ships as a binary; the sections
#                          are dead weight inside the .odmod's 64 MiB limit.
#   -scheduler=none        no goroutines, no coroutine trampolines. A hook runs
#                          to completion or burns its fuel; there is nothing to
#                          schedule against.
#   -panic=trap            a Go panic becomes an unreachable instruction, which
#                          the host reports as a trap and disables the mod. The
#                          alternative, -panic=print, wants somewhere to print,
#                          and on wasm-unknown there is nowhere.
"$TINYGO" build \
    -target=wasm-unknown \
    -no-debug \
    -scheduler=none \
    -panic=trap \
    -o "$here/mod.wasm" \
    ./examples/hello-panel

# Worth a look before packing: this lists imports and exports, and the import
# list is exactly what the host checks against your granted capabilities.
if command -v wasm-objdump >/dev/null 2>&1; then
    echo "--- imports/exports ---"
    wasm-objdump -x "$here/mod.wasm" | sed -n '/^Import\[/,/^$/p;/^Export\[/,/^$/p'
fi

"$root/tools/pack_odmod.sh" "$here" "$here/hello-panel-go.odmod"
