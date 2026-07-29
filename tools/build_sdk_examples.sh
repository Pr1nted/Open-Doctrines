#!/usr/bin/env bash
# Builds every Gearbox SDK example this machine has the toolchain for.
#
#   tools/build_sdk_examples.sh [log-dir]
#   OD_SDK_REQUIRE="c cpp wat" tools/build_sdk_examples.sh
#
# WHY THIS EXISTS
#
# tests/run_all.sh runs ModExamplesTest, which drives every shipped .odmod
# through its full draw path and compares the command lists across languages.
# It is the only check that a language binding got draw_text's argument order
# right -- a binding that swapped two parameters passes every other test in the
# suite and then draws nonsense in the game.
#
# The .odmod files are build artifacts and .gitignore'd, so on a fresh checkout
# there are none, and that check printed "SKIP no .odmod files under .../sdk" on
# all four CI platforms -- while the workflow header claimed CI rebuilt every
# example from source and compared it against the reference. A check that skips
# on every platform is not a check, and one that reads as a pass is worse than
# one that fails.
#
# WHY IT DRIVES THE BUILD SCRIPTS RATHER THAN BEING A for-LOOP
#
# Each sdk/*/build.sh exits 1 both when its toolchain is absent and when the
# build is broken. Those are opposite meanings: nobody needs tinygo installed to
# work on the C example, but a Rust example that stopped compiling must be a
# failure. So the toolchain is probed HERE, first, and only a language whose
# tools are present can fail.
#
# OD_SDK_REQUIRE names the languages that must not be skipped. CI sets it, so a
# compiler quietly disappearing from a runner image is a red build rather than
# one less example silently going untested -- which is the bug this file exists
# to close, arriving by a different route.
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
logs="${1:-$root/build/sdk-examples}"
require="${OD_SDK_REQUIRE:-}"
# OD_SDK_ONLY restricts which languages are even attempted. CI sets it, because
# "the toolchain is present" is not the same as "attempting this is free": the
# AssemblyScript build npm-installs its compiler on first run, so on a runner
# that has node -- which is all of them -- an unrestricted run would reach out
# to the network and could fail for reasons that have nothing to do with the
# code being tested. Unset means attempt everything, which is what you want on
# a developer's machine.
only="${OD_SDK_ONLY:-}"
mkdir -p "$logs"

built=() skipped=() failed=()

# Finds a clang that can actually emit wasm32, and the result is EXPORTED as
# CC/CXX below so each build script uses this one rather than repeating its own
# search. They each had a slightly different list, and the differences were the
# bug: /usr/local/opt/llvm/bin is Homebrew's prefix on Intel macOS and was in
# tests/build_test_mods.sh's list but not the SDK examples', so the fixture mods
# built on the macos-x64 runner and the examples would not have.
#
# Apple's clang cannot target wasm32 at all, so on a Mac the usable one is
# Homebrew LLVM or the copy inside an emscripten install.
wasm_clang() {
    local exe="$1" c
    for c in "$(command -v "$exe" || true)" \
             /opt/homebrew/opt/llvm/bin/"$exe" \
             /usr/local/opt/llvm/bin/"$exe" \
             /opt/homebrew/Cellar/emscripten/*/libexec/llvm/bin/"$exe" \
             /usr/local/Cellar/emscripten/*/libexec/llvm/bin/"$exe"; do
        [ -x "$c" ] || continue
        if "$c" --print-targets 2>/dev/null | grep -qi wasm32; then echo "$c"; return 0; fi
    done
    return 1
}

# Resolved once, exported, and printed -- so a log says which compiler built the
# examples instead of leaving it to be guessed from a failure.
CC="${CC:-$(wasm_clang clang   || true)}"
CXX="${CXX:-$(wasm_clang clang++ || true)}"
export CC CXX
[ -n "$CC" ]  && printf '  cc:  %s\n' "$CC"
[ -n "$CXX" ] && printf '  cxx: %s\n' "$CXX"

have() {
    case "$1" in
        wasm-clang)   [ -n "$CC" ]  ;;
        wasm-clang++) [ -n "$CXX" ] ;;
        # Either assembler is fine; sdk/wat/build.sh accepts both.
        wat)  command -v wat2wasm >/dev/null 2>&1 || command -v wasm-tools >/dev/null 2>&1 ;;
        # Maven drives the compile but the JDK is what actually has to be there,
        # and a machine with mvn and no java fails deep inside the build.
        maven) command -v mvn >/dev/null 2>&1 && command -v java >/dev/null 2>&1 ;;
        # Not a command: a wasi-sdk and a prebuilt libpython that only
        # tools/sdk_toolchains.sh installs.
        wasi-python)
            [ -x "${WASI_SDK_20_CC:-$root/.toolchains/wasi-sdk-20/bin/clang}" ] &&
            [ -d "${GBX_LIBPYTHON:-$root/.toolchains/libpython}" ] ;;
        *) command -v "$1" >/dev/null 2>&1 ;;
    esac
}

declare -a not_requested=()

build() {
    local lang="$1" need="$2" script="$3"
    if [ -n "$only" ]; then
        case " $only " in
            *" $lang "*) ;;
            *) not_requested+=("$lang"); return ;;
        esac
    fi
    if [ ! -f "$root/$script" ]; then
        failed+=("$lang"); printf '  FAIL   %-15s no such script: %s\n' "$lang" "$script"; return
    fi
    if ! have "$need"; then
        skipped+=("$lang"); printf '  skip   %-15s (no %s)\n' "$lang" "$need"; return
    fi
    local log="$logs/$lang.log"
    if ( cd "$root" && bash "$root/$script" ) > "$log" 2>&1; then
        built+=("$lang"); printf '  ok     %-15s\n' "$lang"
    else
        failed+=("$lang")
        printf '  FAIL   %-15s (%s)\n' "$lang" "$log"
        tail -15 "$log" | sed 's/^/           /'
    fi
}

printf '\n=== SDK examples ===\n'

#     language        toolchain      build script
build c               wasm-clang     sdk/examples/hello-panel/build.sh
build cpp             wasm-clang++   sdk/cpp/build.sh
build wat             wat            sdk/wat/build.sh
build rust            cargo          sdk/rust/build.sh
build zig             zig            sdk/zig/examples/hello-panel/build.sh
build go              tinygo         sdk/go/examples/hello-panel/build.sh
build assemblyscript  node           sdk/assemblyscript/build.sh
build java            maven          sdk/java/examples/hello-panel/build.sh
build kotlin          maven          sdk/java/examples/hello-panel-kotlin/build.sh
build js              emcc           sdk/js/examples/hello-panel/build.sh
build ts              emcc           sdk/js/examples/hello-panel-ts/build.sh
build lua             emcc           sdk/lua/examples/hello-panel/build.sh
build python          wasi-python    sdk/python/examples/hello-panel/build.sh

printf '\n  %d built, %d skipped, %d failed\n' \
       "${#built[@]}" "${#skipped[@]}" "${#failed[@]}"
[ "${#built[@]}"   -gt 0 ] && printf '  built:   %s\n' "${built[*]}"
[ "${#skipped[@]}" -gt 0 ] && printf '  skipped: %s\n' "${skipped[*]}"
[ "${#failed[@]}"  -gt 0 ] && printf '  failed:  %s\n' "${failed[*]}"
[ "${#not_requested[@]}" -gt 0 ] && \
    printf '  not attempted (OD_SDK_ONLY): %s\n' "${not_requested[*]}"

rc=0
[ "${#failed[@]}" -eq 0 ] || rc=1

# A language CI is supposed to cover must not fall back to "skipped" because an
# image changed under us. Named languages that did not build are failures here
# even though the same skip is fine on a developer's machine.
if [ -n "$require" ]; then
    for want in $require; do
        case " ${built[*]} " in
            *" $want "*) ;;
            *) printf '  REQUIRED but not built: %s\n' "$want"; rc=1 ;;
        esac
    done
fi

# ModExamplesTest compares languages AGAINST EACH OTHER, so one example proves
# far less than two: with a single .odmod there is nothing to disagree with and
# a binding error in it looks identical to a correct one.
if [ "${#built[@]}" -eq 1 ]; then
    printf '  NOTE: only one example built, so the cross-language comparison\n'
    printf '        cannot run. It needs at least two.\n'
fi

exit $rc
