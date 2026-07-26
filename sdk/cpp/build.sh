#!/usr/bin/env bash
# Builds hello-panel-cpp.odmod. Needs a clang++ that can target wasm32.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"

CXX="${CXX:-}"
if [ -z "$CXX" ]; then
    for c in "$(command -v clang++ || true)" \
             /opt/homebrew/Cellar/emscripten/*/libexec/llvm/bin/clang++ \
             /usr/local/Cellar/emscripten/*/libexec/llvm/bin/clang++ \
             /opt/homebrew/opt/llvm/bin/clang++; do
        [ -x "$c" ] || continue
        if "$c" --print-targets 2>/dev/null | grep -qi wasm32; then CXX="$c"; break; fi
    done
fi
[ -n "$CXX" ] || { echo "no wasm32-capable clang++ found (set CXX=)"; exit 1; }

# -fno-exceptions -fno-rtti: there is no unwinder and no typeinfo in a
# freestanding wasm module. -nostdlib: no libc, no libc++.
"$CXX" --target=wasm32 -nostdlib -fno-exceptions -fno-rtti -std=c++17 -O2 \
       -I "$root/sdk" -Wl,--no-entry -Wl,--allow-undefined \
       -o "$here/mod.wasm" "$here/mod.cpp"

python3 "$root/tools/wasm_imports.py" "$here/mod.wasm" >/dev/null
"$root/tools/pack_odmod.sh" "$here" "$here/hello-panel-cpp.odmod"
