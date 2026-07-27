#!/usr/bin/env bash
# Builds and runs every mod-system test. Non-zero exit means something failed.
#
#   tests/run_all.sh [build-dir]
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build}"
fail=0

step() { printf '\n=== %s ===\n' "$1"; }

step "fixture mods"
"$root/tests/build_test_mods.sh" "$build/testmods" || fail=1

step "build test targets"
cmake --build "$build" --target ModArchiveTest ModRuntimeTest ModManagerTest \
      ModAbiTest ModExamplesTest OdmodCheck GameUpdatesTest -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
      >/dev/null || { echo "build failed"; exit 1; }

run() {
    local name="$1"; shift
    step "$name"
    if "$@"; then :; else echo "$name FAILED"; fail=1; fi
}

run "archive reader"   "$build/ModArchiveTest"
run "abi conformance"  "$build/ModAbiTest" "$root/sdk/abi.json"
run "runtime"          "$build/ModRuntimeTest" "$build/testmods"
run "mod manager"      "$build/ModManagerTest" "$build/testmods" "$build/modmgr_scratch"
# Run from the repository root: the version test shells out to tools/odver.py
# and reads tests/fixtures/, and both are relative to it.
run "game updater"     "$build/GameUpdatesTest"

run "example mods, all languages" "$build/ModExamplesTest" "$root/sdk"

step "tool index"
# Fails if a tool was added without a description or a group, so the
# index cannot quietly fall behind the directory.
python3 "$root/tools/help.py" --check || fail=1

step "generated bindings vs abi.json"
# Fails if a generated file was hand-edited or left stale after an ABI
# change. Regenerate with: python3 tools/gen_bindings.py
python3 "$root/tools/gen_bindings.py" --check || fail=1

step "sdk bindings vs abi.json"
python3 "$root/tools/check_bindings.py" || fail=1

step "documented example"
"$root/tests/check_doc_examples.sh" "$build/doccheck" "$build/odmod-check" || fail=1

step "shipped example mods"
# Every .odmod that has been built, in any language. Each must load, and each
# must be refused when its UI capability is revoked.
found=0
while IFS= read -r m; do
    found=1
    rel="${m#$root/}"
    out=$("$build/odmod-check" "$m" 2>&1) || {
        # Not every mod can run on every build. CPython is too big for WAMR's
        # fast interpreter (an INT16_MAX operand-stack limit in the loader), so
        # the Python example only loads under -DOD_MODS_FAST_INTERP=OFF.
        # Matching the loader's own message rather than the mod's name keeps
        # this honest: a Python mod broken for any OTHER reason still fails.
        case "$out" in
            *"fast interpreter offset overflow"*)
                echo "skip  $rel (needs -DOD_MODS_FAST_INTERP=OFF)"; continue ;;
            *)
                echo "FAIL  $rel (does not load)"; fail=1; continue ;;
        esac
    }
    if grep -q '"UI"' "$(dirname "$m")/MANIFEST.json" 2>/dev/null; then
        if "$build/odmod-check" "$m" --revoke UI >/dev/null 2>&1; then
            echo "FAIL  $rel (loaded with UI revoked -- capability not enforced)"; fail=1; continue
        fi
    fi
    echo "ok    $rel"
done < <(find "$root/sdk" -name "*.odmod" -not -path "*/node_modules/*" | sort)
[ $found -eq 1 ] || echo "skip  no example mods built yet"

printf '\n'
[ $fail -eq 0 ] && echo "ALL PASSED" || echo "SOMETHING FAILED"
exit $fail
