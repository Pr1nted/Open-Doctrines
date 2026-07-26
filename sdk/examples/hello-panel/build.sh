#!/usr/bin/env bash
# Builds hello-panel.odmod. Needs a clang that can target wasm32.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"

CC="${CC:-}"
if [ -z "$CC" ]; then
    for c in "$(command -v clang || true)" \
             /opt/homebrew/Cellar/emscripten/*/libexec/llvm/bin/clang \
             /usr/local/Cellar/emscripten/*/libexec/llvm/bin/clang \
             /opt/homebrew/opt/llvm/bin/clang; do
        [ -x "$c" ] || continue
        if "$c" --print-targets 2>/dev/null | grep -qi wasm32; then CC="$c"; break; fi
    done
fi
[ -n "$CC" ] || { echo "no wasm32-capable clang found (set CC=)"; exit 1; }

"$CC" --target=wasm32 -nostdlib -O2 -I "$root/sdk" \
      -Wl,--no-entry -Wl,--allow-undefined \
      -o "$here/mod.wasm" "$here/mod.c"

"$root/tools/pack_odmod.sh" "$here" "$here/hello-panel.odmod"
