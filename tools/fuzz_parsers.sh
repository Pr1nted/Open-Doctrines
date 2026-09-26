#!/usr/bin/env bash
# The parser fuzzer, built with the sanitisers that make it worth running.
#
#   tools/fuzz_parsers.sh                 8 seeds x 2,000,000 cases
#   tools/fuzz_parsers.sh 4 500000        seeds, cases per seed
#
# WHY A SCRIPT AND NOT THE CMAKE TARGET
#
# tests/run_all.sh runs FuzzParsersTest as an ordinary test: a fixed seed, a
# second of work, no sanitiser. That catches a case that was already found and
# nothing else, because without AddressSanitizer a read one byte past a buffer
# is silent, and without UndefinedBehaviorSanitizer a signed overflow is
# silent -- and signed overflow in the expression evaluator is exactly what
# this found the first time it was run this way.
#
# So the finding configuration is this one, and it is separate because the
# sanitised build has to be its own build: -fsanitize=address,undefined on the
# whole project would slow every other test down for nothing.
#
# It compiles the sources directly rather than configuring CMake. The target
# links eleven files and no window library, so a full configure would install
# X11 on a machine that opens nothing.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
seeds="${1:-8}"
iters="${2:-2000000}"
out="${TMPDIR:-/tmp}/od-fuzz-parsers"
mkdir -p "$out"

cxx="${CXX:-clang++}"
command -v "$cxx" >/dev/null || { echo "no $cxx on this machine" >&2; exit 1; }

echo "building the sanitised fuzzer with $cxx"
"$cxx" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -fno-sanitize-recover=all \
    -x c++ \
    -I "$root/src" -I "$root/tests" -I "$root/tools/map_generator" \
    -o "$out/fuzz" \
    "$root/tests/fuzz_parsers_test.cpp" \
    "$root/src/net/NetProtocol.cpp" \
    "$root/src/net/WorldSync.cpp" \
    "$root/src/net/NetUrl.cpp" \
    "$root/src/net/WsCommon.cpp" \
    "$root/src/net/WsUnavailable.cpp" \
    "$root/src/util/Ed25519.cpp" \
    "$root/src/script/Expr.cpp" \
    "$root/src/script/Blocks.cpp" \
    "$root/src/script/ScriptValue.cpp" \
    "$root/src/mods/ModPackage.cpp" \
    "$root/src/net/ModAttest.cpp" \
    "$root/tools/map_generator/miniz.c" \
    "$root/tools/map_generator/miniz_tdef.c" \
    "$root/tools/map_generator/miniz_tinfl.c" \
    "$root/tools/map_generator/miniz_zip.c" \
    2>&1 | grep -vE 'treating .c. input as .c\+\+.' || true
test -x "$out/fuzz" || { echo "the fuzzer did not build" >&2; exit 1; }

# -fno-sanitize-recover makes UBSan EXIT rather than print and carry on, so a
# finding fails the run instead of scrolling past in a log nobody reads.
fail=0
for i in $(seq 1 "$seeds"); do
    seed=$(( i * 7919 ))       # spread out, and the same every time
    printf '\n=== seed %s, %s cases ===\n' "$seed" "$iters"
    if ! OD_FUZZ_SEED="$seed" OD_FUZZ_ITERS="$iters" "$out/fuzz"; then
        echo "  ^^ seed $seed found something. Reproduce with:"
        echo "     OD_FUZZ_SEED=$seed OD_FUZZ_ITERS=$iters $out/fuzz"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo
    echo "the fuzzer found something. The failing input is printed above as hex."
    exit 1
fi
echo
echo "ok  $seeds seeds x $iters cases, no crash, no overflow, nothing out of bounds"
