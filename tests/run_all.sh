#!/usr/bin/env bash
# Builds and runs every mod-system test. Non-zero exit means something failed.
#
#   tests/run_all.sh [build-dir]
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build}"
fail=0

# A python that RUNS, not one that merely exists. Windows ships a python3.exe
# stub that prints an advertisement for the Microsoft Store and exits, so every
# check below that shells out to python3 failed at once on a machine with a
# working Python installed as `python`. See tools/find_python.sh.
PY="$("$root/tools/find_python.sh" 2>/dev/null || true)"
if [ -z "$PY" ]; then
    echo "no working python3 was found. The tool index, both binding checks," >&2
    echo "the notices, the flag licences and the GIF decode all need one." >&2
    echo "  Windows: install python.org Python, or disable the WindowsApps" >&2
    echo "           python3 alias in Settings > Apps > App execution aliases." >&2
    PY=python3      # so the failures below name the command they tried
fi

step() { printf '\n=== %s ===\n' "$1"; }

step "fixture mods"
"$root/tests/build_test_mods.sh" "$build/testmods" || fail=1

step "build test targets"
# --config Release for the multi-config generators. Visual Studio and Xcode
# ignore CMAKE_BUILD_TYPE at configure time and take the configuration here
# instead; without it MSVC builds Debug, and then nothing below is where this
# script goes looking. Single-config generators (Make, Ninja) ignore the flag.
cmake --build "$build" --config Release --target ModArchiveTest ModRuntimeTest ModManagerTest \
      ModAbiTest ModExamplesTest OdmodCheck GameUpdatesTest NativeDialogTest GifEncoderTest PngWriteTest OrderValidationTest NeuralNetTest SaveDeltaTest NetAttestTest NetProtocolTest NetAccountTest NetLobbyTest NetWsServerTest NetCryptoTest NetTicketTest NetSealTest NetHostBookTest NetTunnelTest -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
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

# WHAT FAILED, not merely THAT something did.
#
# Every check here used to set one `fail` flag, and qualify.sh runs this whole
# script as a single step called "the whole test suite" -- so a red build ended
# with a summary that named a hundred checks collectively and none of them
# individually. Finding the real one meant scrolling a 4000-line CI log. The
# names are collected as they fail and printed at the end.
failed_steps=()

note_fail() { failed_steps+=("$1"); fail=1; }

run() {
    local name="$1"; shift
    step "$name"
    if "$@"; then :; else echo "$name FAILED"; note_fail "$name"; fi
}

# For the drift checks, which are a bare command rather than a named run().
check() {
    local name="$1"; shift
    step "$name"
    if "$@"; then :; else note_fail "$name"; fi
}

run "archive reader"   "$bin/ModArchiveTest"
# The file chooser, on the platform running this. It opens nothing: what it
# checks is the command built for the platform's own helper, and whether
# that helper accepts it. See the file for what a test cannot reach here.
run "file chooser"     "$bin/NativeDialogTest"
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

# The indexed-PNG writer every .odmap layer goes through. Same shape as the GIF
# check above and for the same reason: a map is only losslessly smaller if these
# bytes decode back to the pixels that went in, and deflate cannot check itself.
rm -rf "$build/pngtest" && mkdir -p "$build/pngtest"
run "png writer"       "$bin/PngWriteTest" "$build/pngtest"
# What the HOST does with orders a stranger chose. Every other multiplayer
# check is about who is talking; this is the only one about what they are
# allowed to say, and it is the check a modified client goes at first.
run "order validation" "$bin/OrderValidationTest" "$root/data/"

# The dedicated server, started for real: config round-trip, data directory,
# map resolution, a whole world load, and the console. It is a second binary
# with its own raylib underneath it, so none of the above says anything about
# whether it runs.
cmake --build "$build" --config Release --target OpenDoctrinesServer \
      -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
      >> "$build/test-targets-build.log" 2>&1 \
    && run "dedicated server starts" "$root/tests/server_smoke_test.sh" "$build" \
    || note_fail "dedicated server build"
run "neural net gradients" "$bin/NeuralNetTest"
run "odsv turn delta"  "$bin/SaveDeltaTest"

step "the same seed plays the same game"
"$root/tests/determinism_check.sh" "$build" || fail=1
run "gif decodes back" $PY "$root/tests/gif_encoder_check.py" "$build/giftest"
run "png decodes back" $PY "$root/tests/png_write_check.py" "$build/pngtest"

run "example mods, all languages" "$bin/ModExamplesTest" "$root/sdk"

# The dedicated server implements raylib itself (src/server/). The generated
# half must match the raylib the build actually uses, or the server links
# against a header it does not satisfy -- which shows up as a link error on
# whichever platform builds the server first, not here.
check "server raylib stubs vs raylib.h" \
      $PY "$root/tools/gen_server_raylib_stubs.py" --check

# Fails if a tool was added without a description or a group, so the
# index cannot quietly fall behind the directory.
check "tool index" $PY "$root/tools/help.py" --check

# Fails if a generated file was hand-edited or left stale after an ABI
# change. Regenerate with: python3 tools/gen_bindings.py
check "generated bindings vs abi.json" $PY "$root/tools/gen_bindings.py" --check

check "sdk bindings vs abi.json" $PY "$root/tools/check_bindings.py"

# THE COMPATIBILITY GATE, and a different question from the two checks above.
#
# ModAbiTest asks "does abi.json describe the host this build has?" -- both
# files move together, and deleting a function from both passes. That is no use
# for a modder whose .odmod was built last year: they need "does this host still
# satisfy the contract I compiled against?", and answering it needs a copy of
# that contract from back then. sdk/compat/abi-*.json holds one per shipped
# minor, frozen, and this asserts every symbol in every one of them is still
# present with the same wire signature and the same capability.
#
# Within a major version the ABI is append-only. Adding is free; renaming,
# re-signing or re-gating breaks a binary that already exists, and only a major
# bump is allowed to do that -- parseModManifest refuses a mismatched major
# outright, which is the mod being told rather than crashing.
check "abi compatibility vs frozen baselines" $PY "$root/tools/check_abi_compat.py"

# The wiki is generated from the same file the bindings are, and it is
# PUBLISHED -- .github/workflows/publish-wiki.yml pushes wiki/ to the Wiki tab
# on every merge that touches it. So a stale page here is not a stale file in a
# tree, it is a wrong API reference on the public wiki, promising functions the
# host does not have. Regenerate with: python3 tools/gen_wiki.py
check "wiki vs abi.json" $PY "$root/tools/gen_wiki.py" --check

# Fails if a dataset, library or font was added to the build without being
# recorded, or if NOTICE.md / data/credits.txt were edited by hand instead of
# regenerated. Attribution that drifts is a licence breach, not a typo.
# Regenerate with: python3 tools/gen_notices.py
check "third-party notices vs provenance.json" $PY "$root/tools/gen_notices.py" --check

# Offline: asserts every flag in download_flags_fast.py has a recorded licence
# and that none of them is under terms the project has not accepted. Refresh
# from Wikimedia with: python3 tools/audit_flag_licenses.py
check "flag licences" $PY "$root/tools/audit_flag_licenses.py" --check

# Fails if any data source that reaches a shipped map is under terms that would
# stop the map being used commercially. The game's own licence is a separate
# question and is ours to change; a third party's is not.
check "map data is commercially licensable" $PY "$root/tools/check_data_licences.py"

# The two shipped-map invariants that the ENGINE also depends on, checked
# against the maps as they will ship rather than against the tools that made
# them. Both are cheap to break by regenerating with an older tool.
#
#   - no water body below MIN_WATER_BODY, because the engine refuses to call
#     one a coast and the map should not draw one as a hole through a country
#   - every hull in open water and of a type the game can build, every port on
#     a province isProvinceCoastal agrees is coastal
check "shipped maps have no sub-sea-threshold water" \
      $PY "$root/tools/fill_water_speckle.py" --check
check "shipped maps have a usable naval layer" \
      $PY "$root/tools/fix_naval_layer.py" --check
#   - every id points at something that exists: no province painted without a
#     row, no army or port naming one that was deleted, no country holding
#     nothing. Silent at build time, a rendering hole much later.
check "shipped maps are internally consistent" \
      $PY "$root/tools/check_map_integrity.py" --strict
#   - the browser preview agrees with who actually owns the ground. Five steps
#     move borders after generate_scenario draws it, and none redraws it, so
#     this drifts without anything failing.
check "shipped maps preview the map they are" \
      $PY "$root/tools/rebuild_map_preview.py" --check
#   - the maps are still in their compact form. A dozen tools rewrite an
#     archive, and any one of them can put a derived layer back or write a
#     land/sea layer as truecolour again; both are silent, and both cost
#     megabytes per map. This fails on the way back up, not months later.
check "shipped maps are compactly encoded" \
      $PY "$root/tools/shrink_maps.py" --check

step "the Windows manifest is valid XML"
# One second here against ten minutes there. The manifest is embedded by the
# linker on Windows only, so a malformed one fails nowhere except a Windows CI
# job, at link time, as "LNK1327: failure during running mt.exe".
#
# The trap is specific and easy to walk into twice: this project writes an em
# dash as two hyphens, and XML forbids two hyphens inside a comment. That is
# exactly how the first version of this file broke the Windows build.
$PY - "$root/packaging/windows/OpenDoctrines.manifest" <<'PY' || fail=1
import re, sys, xml.dom.minidom
path = sys.argv[1]
src = open(path, encoding="utf-8").read()
try:
    xml.dom.minidom.parseString(src)
except Exception as e:
    print(f"  {path} is not valid XML: {e}")
    sys.exit(1)
bad = sum(c.count("--") for c in re.findall(r"<!--(.*?)-->", src, re.S))
if bad:
    print(f"  {path}: {bad} double hyphen(s) inside an XML comment.")
    print("  XML forbids them; mt.exe fails the Windows link with c1010070.")
    sys.exit(1)
if "activeCodePage" not in src:
    print(f"  {path}: no activeCodePage. Narrow paths fall back to the ANSI")
    print("  code page, and players with non-ASCII account names lose every file.")
    sys.exit(1)
print("  manifest: valid XML, UTF-8 code page declared")
PY

step "documented example"
"$root/tests/check_doc_examples.sh" "$build/doccheck" "$bin/odmod-check" || fail=1

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
[ $found -eq 1 ] || echo "skip  no example mods built yet"

printf '\n'
if [ $fail -eq 0 ]; then
    echo "ALL PASSED"
else
    echo "SOMETHING FAILED:"
    for s in "${failed_steps[@]}"; do echo "  - $s"; done
fi
exit $fail
