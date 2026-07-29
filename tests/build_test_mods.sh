#!/usr/bin/env bash
# Compiles the fixture mods in tests/mods/ to freestanding wasm32 modules.
#
# Needs a clang that can target wasm32. Apple's clang cannot; the one bundled
# with Emscripten can, and so can any upstream LLVM. If none is found this exits
# 0 without producing anything -- the runtime tests then skip the wasm cases and
# say so, rather than failing on a machine that simply has no wasm toolchain.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
out="${1:-$root/build/testmods}"

find_clang() {
    for c in "$@"; do
        [ -x "$c" ] || continue
        if "$c" --print-targets 2>/dev/null | grep -qi wasm32; then echo "$c"; return 0; fi
    done
    # emcc knows where its own clang lives
    if command -v emcc >/dev/null 2>&1; then
        local em
        em="$(dirname "$(readlink -f "$(command -v emcc)")")/../upstream/bin/clang"
        [ -x "$em" ] && { echo "$em"; return 0; }
        for c in /opt/homebrew/Cellar/emscripten/*/libexec/llvm/bin/clang \
                 /usr/local/Cellar/emscripten/*/libexec/llvm/bin/clang; do
            [ -x "$c" ] && { echo "$c"; return 0; }
        done
    fi
    return 1
}

CC="$(find_clang "$(command -v clang || true)" /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang)" || {
    echo "no wasm32-capable clang found; skipping fixture mod build"
    exit 0
}

mkdir -p "$out"
status=0
# clang++ sits next to clang in every layout we probe for
CCPP="${CC}++"

for src in "$here"/mods/*.c "$here"/mods/*.cpp; do
    [ -e "$src" ] || continue
    case "$src" in
        *.cpp) name="$(basename "$src" .cpp)"; tool="$CCPP"; extra="-fno-exceptions -fno-rtti -std=c++17" ;;
        *)     name="$(basename "$src" .c)";   tool="$CC";   extra="" ;;
    esac
    # shellcheck disable=SC2086
    if "$tool" --target=wasm32 -nostdlib -O2 -I "$root/sdk" $extra \
             -Wl,--no-entry -Wl,--allow-undefined \
             -o "$out/$name.wasm" "$src"; then
        echo "built $out/$name.wasm ($(wc -c < "$out/$name.wasm" | tr -d ' ') bytes)"
    else
        echo "FAILED to build $name"
        status=1
    fi
done
exit $status
