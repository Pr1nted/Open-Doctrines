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
      ModAbiTest ModExamplesTest OdmodCheck GameUpdatesTest GifEncoderTest NetAttestTest NetProtocolTest NetAccountTest NetLobbyTest NetWsServerTest NetCryptoTest NetTicketTest NetSealTest NetHostBookTest NetTunnelTest -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
      >/dev/null || { echo "build failed"; exit 1; }

run() {
    local name="$1"; shift
    step "$name"
    if "$@"; then :; else echo "$name FAILED"; fail=1; fi
}

run "archive reader"   "$build/ModArchiveTest"
run "mod sides + attestation" "$build/NetAttestTest"
run "net protocol"     "$build/NetProtocolTest"
run "account client"   "$build/NetAccountTest"
run "lobby rules"      "$build/NetLobbyTest"
run "crypto vectors"   "$build/NetCryptoTest"
run "join tickets"     "$build/NetTicketTest"
run "sealed orders"    "$build/NetSealTest"
run "seats remembered" "$build/NetHostBookTest"
run "tunnel parsing"   "$build/NetTunnelTest"
# Binds a loopback port on an OS-chosen number. Opens nothing to the network.
run "websocket server" "$build/NetWsServerTest"
# A real host and a real client over loopback, against a stand-in account
# service. Skips itself if node is missing; see tests/connectivity_test.sh.
run "connectivity"     "$root/tests/connectivity_test.sh" "$build"
run "abi conformance"  "$build/ModAbiTest" "$root/sdk/abi.json"
run "runtime"          "$build/ModRuntimeTest" "$build/testmods"
run "mod manager"      "$build/ModManagerTest" "$build/testmods" "$build/modmgr_scratch"
# Run from the repository root: the version test shells out to tools/odver.py
# and reads tests/fixtures/, and both are relative to it.
run "game updater"     "$build/GameUpdatesTest"

# Timelapse writer. The C++ side writes GIFs with known content; the Python side
# decodes them with Pillow and compares. An encoder cannot verify its own LZW --
# a stream with a mis-sized code still has a valid header and still opens.
rm -rf "$build/giftest" && mkdir -p "$build/giftest"
run "gif encoder"      "$build/GifEncoderTest" "$build/giftest"
run "gif decodes back" python3 "$root/tests/gif_encoder_check.py" "$build/giftest"

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
