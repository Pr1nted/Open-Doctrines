#!/usr/bin/env bash
# Builds and runs every mod-system test. Non-zero exit means something failed.
#
#   tests/run_all.sh [build-dir]
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build}"
fail=0

# SKIPS ARE NOT PASSES
#
# Five checks here do nothing at all when a toolchain is absent -- the fixture
# mods without a wasm32 clang, connectivity without node, the GIF decode without
# Pillow, the documented example without clang, and the SDK example comparison
# without any built .odmod. Every one of them used to exit 0, so the suite
# printed ALL PASSED either way.
#
# That is not hypothetical. The SDK example comparison -- the only check that a
# language binding got draw_text's argument order right -- skipped on all four
# CI platforms simultaneously, for as long as CI has existed, while the workflow
# header claimed it rebuilt every example and compared it against the reference.
# Nothing was red, because nothing ran.
#
# So each of those exits 77 now (the usual "skipped" code) and skips are
# collected and named in the verdict, which says PASSED, N SKIPPED rather than
# ALL PASSED. Locally that is the whole change: a developer without tinygo
# should not get a red suite, they should be told what they did not test.
#
# CI sets OD_STRICT_SKIPS=1, and there an unexpected skip fails the run --
# because a hole nobody is told about is exactly how this went unnoticed for
# months. OD_EXPECTED_SKIPS is the escape hatch: a space-separated list of tags
# the caller declares genuinely impossible in its environment rather than merely
# missing, set per platform in the workflow so each exception is written down
# somewhere a person reads.
declare -a skipped=()
expected="${OD_EXPECTED_SKIPS:-}"
strict="${OD_STRICT_SKIPS:-0}"

step() { printf '\n=== %s ===\n' "$1"; }

note_skip() {
    skipped+=("$1")
    printf '  SKIPPED [%s] %s\n' "$1" "$2"
}

step "fixture mods"
"$root/tests/build_test_mods.sh" "$build/testmods"
case $? in
    0)  ;;
    77) note_skip fixture-mods "no wasm32 clang: every mod test below has nothing to load" ;;
    *)  fail=1 ;;
esac

step "build test targets"
# --config Release for the multi-config generators. Visual Studio and Xcode
# ignore CMAKE_BUILD_TYPE at configure time and take the configuration here
# instead; without it MSVC builds Debug, and then nothing below is where this
# script goes looking. Single-config generators (Make, Ninja) ignore the flag.
cmake --build "$build" --config Release --target ModArchiveTest ModRuntimeTest ModManagerTest \
      ModAbiTest ModExamplesTest OdmodCheck GameUpdatesTest GifEncoderTest NetAttestTest NetProtocolTest NetAccountTest NetLobbyTest NetWsServerTest NetCryptoTest NetTicketTest NetSealTest NetHostBookTest NetTunnelTest -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
      > "$build/test-targets-build.log" 2>&1 || {
    # Not >/dev/null. Suppressing this meant a compile error on a platform
    # nobody had built the tests on reported itself as the word "build failed"
    # and nothing else -- one line, no file, no reason.
    echo "build failed:"
    grep -iE 'error|fatal' "$build/test-targets-build.log" | head -20
    echo "  (full log: $build/test-targets-build.log)"
    exit 1
}

# Where the test EXECUTABLES ended up, which is not always the build directory.
# A multi-config generator writes build/Release/, so every "$build/SomeTest"
# below was a path that does not exist on Windows -- reported as
# "No such file or directory" for each of five connectivity cases in turn,
# which reads like a missing binary rather than a wrong folder.
#
# Data directories ($build/testmods and friends) stay under $build: they are
# created by these scripts, not by the generator.
bin="$build"
if [ -x "$build/Release/ModArchiveTest" ] || [ -f "$build/Release/ModArchiveTest.exe" ]; then
    bin="$build/Release"
fi

# Step names are prose ("example mods, all languages") and skip tags have to go
# in an environment variable, so the tag is the name reduced to lowercase words
# joined by dashes: example-mods-all-languages. Derived rather than written out
# twice, so a renamed step cannot keep an out-of-date tag.
slug() {
    printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | tr -cs '[:alnum:]' '-' \
        | sed 's/-*$//'
}

run() {
    local name="$1"; shift
    step "$name"
    "$@"
    case $? in
        0)  ;;
        # 77 is "this check did not run". It has to travel up as a skip rather
        # than a pass, or the suite goes green on a machine that tested nothing.
        77) note_skip "$(slug "$name")" "did not run here" ;;
        *)  echo "$name FAILED"; fail=1 ;;
    esac
}

run "archive reader"   "$bin/ModArchiveTest"
run "mod sides + attestation" "$bin/NetAttestTest"
run "net protocol"     "$bin/NetProtocolTest"
run "account client"   "$bin/NetAccountTest"
run "lobby rules"      "$bin/NetLobbyTest"
run "crypto vectors"   "$bin/NetCryptoTest"
run "join tickets"     "$bin/NetTicketTest"
run "sealed orders"    "$bin/NetSealTest"
run "seats remembered" "$bin/NetHostBookTest"
run "tunnel parsing"   "$bin/NetTunnelTest"
# Binds a loopback port on an OS-chosen number. Opens nothing to the network.
run "websocket server" "$bin/NetWsServerTest"
# A real host and a real client over loopback, against a stand-in account
# service. Skips itself if node is missing; see tests/connectivity_test.sh.
run "connectivity"     "$root/tests/connectivity_test.sh" "$build"
run "abi conformance"  "$bin/ModAbiTest" "$root/sdk/abi.json"
run "runtime"          "$bin/ModRuntimeTest" "$build/testmods"
run "mod manager"      "$bin/ModManagerTest" "$build/testmods" "$build/modmgr_scratch"
# Run from the repository root: the version test shells out to tools/odver.py
# and reads tests/fixtures/, and both are relative to it.
run "game updater"     "$bin/GameUpdatesTest"

# Timelapse writer. The C++ side writes GIFs with known content; the Python side
# decodes them with Pillow and compares. An encoder cannot verify its own LZW --
# a stream with a mis-sized code still has a valid header and still opens.
rm -rf "$build/giftest" && mkdir -p "$build/giftest"
run "gif encoder"      "$bin/GifEncoderTest" "$build/giftest"
run "gif decodes back" python3 "$root/tests/gif_encoder_check.py" "$build/giftest"

run "example mods, all languages" "$bin/ModExamplesTest" "$root/sdk"

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

step "third-party notices vs provenance.json"
# Fails if a dataset, library or font was added to the build without being
# recorded, or if NOTICE.md / data/credits.txt were edited by hand instead of
# regenerated. Attribution that drifts is a licence breach, not a typo.
# Regenerate with: python3 tools/gen_notices.py
python3 "$root/tools/gen_notices.py" --check || fail=1

step "flag licences"
# Offline: asserts every flag in download_flags_fast.py has a recorded licence
# and that none of them is under terms the project has not accepted. Refresh
# from Wikimedia with: python3 tools/audit_flag_licenses.py
python3 "$root/tools/audit_flag_licenses.py" --check || fail=1

step "documented example"
"$root/tests/check_doc_examples.sh" "$build/doccheck" "$bin/odmod-check"
case $? in
    0)  ;;
    77) note_skip doc-example "no wasm32 clang: the documented hello-world was not compiled" ;;
    *)  fail=1 ;;
esac

step "shipped example mods"
# Every .odmod that has been built, in any language. Each must load, and each
# must be refused when its UI capability is revoked.
found=0
while IFS= read -r m; do
    found=1
    rel="${m#$root/}"
    out=$("$bin/odmod-check" "$m" 2>&1) || {
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
        if "$bin/odmod-check" "$m" --revoke UI >/dev/null 2>&1; then
            echo "FAIL  $rel (loaded with UI revoked -- capability not enforced)"; fail=1; continue
        fi
    fi
    echo "ok    $rel"
done < <(find "$root/sdk" -name "*.odmod" -not -path "*/node_modules/*" | sort)
[ $found -eq 1 ] || note_skip sdk-examples \
    "no .odmod built: capability enforcement was checked against nothing"

printf '\n'

# The verdict names what did not run. "ALL PASSED" is reserved for a run where
# everything actually ran -- it is the sentence people read instead of the log,
# so it must not be printable by a suite that skipped half of itself.
if [ ${#skipped[@]} -gt 0 ]; then
    printf 'SKIPPED %d check(s) -- these were NOT tested:\n' "${#skipped[@]}"
    for s in "${skipped[@]}"; do printf '  - %s\n' "$s"; done

    if [ "$strict" != "0" ]; then
        for s in "${skipped[@]}"; do
            case " $expected " in
                *" $s "*) ;;
                *) printf 'UNEXPECTED SKIP: %s\n' "$s"
                   printf '  This environment is supposed to run that check. Either install\n'
                   printf '  what it needs, or add "%s" to OD_EXPECTED_SKIPS and say why.\n' "$s"
                   fail=1 ;;
            esac
        done
    fi
    printf '\n'
fi

if [ $fail -ne 0 ]; then
    echo "SOMETHING FAILED"
elif [ ${#skipped[@]} -gt 0 ]; then
    echo "PASSED, ${#skipped[@]} SKIPPED"
else
    echo "ALL PASSED"
fi
exit $fail
