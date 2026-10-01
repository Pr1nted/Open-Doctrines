#!/usr/bin/env bash
# Compile the generated capability mods to freestanding wasm32.
#
# Same toolchain discovery as tests/build_test_mods.sh, and the same reason
# for it: Apple's clang cannot target wasm32, emscripten's can, and so can any
# upstream LLVM. With none of them this exits 0 without producing anything and
# ModCapabilityTest says it skipped -- a machine with no wasm toolchain should
# not fail a run over a fixture it cannot build.
#
# Sources come from tools/gen_capability_mods.py, which writes them from
# sdk/abi.json.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
out="${1:-$root/build/capmods}"

find_clang() {
    for c in "$@"; do
        [ -n "$c" ] && [ -x "$c" ] || continue
        if "$c" --print-targets 2>/dev/null | grep -qi wasm32; then echo "$c"; return 0; fi
    done
    if command -v emcc >/dev/null 2>&1; then
        em="$(dirname "$(readlink -f "$(command -v emcc)")")/../upstream/bin/clang"
        [ -x "$em" ] && { echo "$em"; return 0; }
        for c in /opt/homebrew/Cellar/emscripten/*/libexec/llvm/bin/clang \
                 /usr/local/Cellar/emscripten/*/libexec/llvm/bin/clang; do
            [ -x "$c" ] && { echo "$c"; return 0; }
        done
    fi
    return 1
}

CC="$(find_clang "${CC:-}" "$(command -v clang || true)" \
                 /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang \
                 "/c/Program Files/LLVM/bin/clang.exe")" || {
    echo "no wasm32-capable clang found; skipping capability mod build"
    exit 0
}

status=0; built=0
for src in "$out"/cap_*.c; do
    [ -e "$src" ] || continue
    name="$(basename "$src" .c)"
    if "$CC" --target=wasm32 -nostdlib -O2 -I "$root/sdk" \
             -Wl,--no-entry -Wl,--allow-undefined \
             -o "$out/$name.wasm" "$src"; then
        built=$((built + 1))
    else
        echo "FAILED to build $name"; status=1
    fi
done
echo "built $built capability fixture(s) in $out"
exit $status
