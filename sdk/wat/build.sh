#!/usr/bin/env bash
# Builds hello-panel-wat.odmod from hello.wat.
#
# Needs a WebAssembly text assembler. Either works:
#   wabt        -> wat2wasm       (brew install wabt)
#   wasm-tools  -> wasm-tools parse (cargo install wasm-tools)
#
# Neither was installed on the machine where this was written, so this script
# has not been run. If it is wrong it is wrong in an obvious way -- two
# commands and a call to the packer.
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"

if command -v wat2wasm >/dev/null 2>&1; then
    wat2wasm "$here/hello.wat" -o "$here/mod.wasm"
elif command -v wasm-tools >/dev/null 2>&1; then
    wasm-tools parse "$here/hello.wat" -o "$here/mod.wasm"
else
    echo "no wat assembler found." >&2
    echo "  brew install wabt          # gives wat2wasm" >&2
    echo "  cargo install wasm-tools   # gives wasm-tools parse" >&2
    exit 1
fi

echo "assembled $here/mod.wasm ($(wc -c < "$here/mod.wasm" | tr -d ' ') bytes)"

# MANIFEST.json must be the archive's first entry; that is what this handles.
"$root/tools/pack_odmod.sh" "$here" "$here/hello-panel-wat.odmod"

echo
echo "check it before shipping:"
echo "  odmod-check $here/hello-panel-wat.odmod"
echo "  odmod-check $here/hello-panel-wat.odmod --revoke UI"
