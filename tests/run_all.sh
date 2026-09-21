#!/usr/bin/env bash
# Builds and runs every mod-system test. Non-zero exit means something failed.
#
#   tests/run_all.sh [build-dir]
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"

# ── EVERY PYTHON TOOL READS UTF-8, WHATEVER THE MACHINE'S LOCALE IS ──
#
# Python's open() uses the platform default encoding, which is UTF-8 on macOS
# and Linux and cp1252 on a US-English Windows runner. Sixteen of the twenty
# tools this suite runs open a source file without saying otherwise, and the
# sources are full of em-dashes -- src/Game_Policies.cpp alone has 2,945
# non-ASCII bytes -- so on Windows they raise UnicodeDecodeError and the check
# fails with a traceback about an encoding rather than about the code.
#
# check_effect_fields.py did exactly that, and only now: Windows had never got
# this far before, because it was dying at compile time first.
#
# UTF-8 mode (PEP 540) makes open() default to UTF-8 everywhere. It is one line
# instead of forty call sites, it covers tools written tomorrow, and Python 3.15
# makes it the default anyway. PYTHONIOENCODING keeps the OUTPUT side honest on
# a console that is not UTF-8.
export PYTHONUTF8=1
export PYTHONIOENCODING=utf-8
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
      ModAbiTest ModExamplesTest OdmodCheck GameUpdatesTest NativeDialogTest GifEncoderTest PngWriteTest OrderValidationTest PolicyRulesTest IndustryCapacityTest GoodsRecipeTest ReleaseRulesTest ArmySplitTest ShipRouteTest CombatDepthTest BattleRulesTest SupplyRulesTest TroopTypesTest ResearchGroupsTest DistrictRulesTest CountryProfileTest FeedbackClientTest MailRulesTest AdvisorTest LlmInfluenceTest NetConnectTimeoutTest LlmRoundTripTest ToolReleaseTest NeuralNetTest ModelBlobTest ScriptExprTest SaveDeltaTest SaveRoundTripTest NetAttestTest NetProtocolTest NetAccountTest NetLobbyTest NetChatTest AnnouncementsTest LfgTest ModDirTest RelayLinkTest StreamSafeTest ChatVoteTest IrcParseTest OverlayFeedTest JoinLinkTest PresenceTest NetWsServerTest NetCryptoTest NetTicketTest NetSealTest NetHostBookTest NetTunnelTest DialogTest LocaleTest TouchGestureTest MinorityShareTest PartyRulesTest NationalisationTest WorldProvenanceTest ModProtectedTest CountryFieldsTest ScriptCommandsTest ModRenderLayerTest ModContentTest -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
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
run "touch gestures"   "$bin/TouchGestureTest"
run "minority shares"  "$bin/MinorityShareTest"
run "party rules"      "$bin/PartyRulesTest" "$root/data/"
run "nationalisation rules" "$bin/NationalisationTest"
# Every test this RUNS must be one it BUILDS -- six were not, and that is what
# failed the SDK 1.3 release after a green local suite. See the checker.
"$PY" "$root/tools/check_suite_targets.py" "$0" || exit 1

run "world provenance" "$bin/WorldProvenanceTest"
run "core.protected"   "$bin/ModProtectedTest"
run "country fields"   "$bin/CountryFieldsTest"
run "script commands"  "$bin/ScriptCommandsTest"
run "mod render layer" "$bin/ModRenderLayerTest"
run "mod content"      "$bin/ModContentTest"
run "qualify play gate" "$root/tests/qualify_play_gate_test.sh"
run "mod sides + attestation" "$bin/NetAttestTest"
run "net protocol"     "$bin/NetProtocolTest"
run "account client"   "$bin/NetAccountTest"
run "lobby rules"      "$bin/NetLobbyTest"
run "relay framing"    "$bin/RelayLinkTest"
run "lobby chat"       "$bin/NetChatTest"
run "announcements"    "$bin/AnnouncementsTest"
run "lfg board"        "$bin/LfgTest"
run "mod directory" "$bin/ModDirTest"
run "stream safe"      "$bin/StreamSafeTest"
run "chat vote"        "$bin/ChatVoteTest"
run "twitch chat"      "$bin/IrcParseTest"
run "overlay feed"     "$bin/OverlayFeedTest"
run "join links"       "$bin/JoinLinkTest"
run "discord presence" "$bin/PresenceTest"
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
run "doctrine rules"   "$bin/PolicyRulesTest" "$root/data/"
# Twice: the tenure rule ships OFF, and a flag that is never exercised is a
# flag nobody knows works. The second run is the only place the ON contract is
# checked, because policyTenure caches the flag per process.
OD_DOCTRINE_TENURE=1 run "doctrine tenure"  "$bin/PolicyRulesTest" "$root/data/"
# And once for political capital, for the same reason: the rule caches its
# flag per process, and the arm that is never run is the arm nobody knows works.
OD_POLITICAL_CAPITAL=1 run "political capital"  "$bin/PolicyRulesTest" "$root/data/"
# Each new politics rule caches its flag in a function-local static, so one
# process only ever sees one state and every ON contract needs its own run.
OD_MINORITY_WORST=1 run "minority grievance"  "$bin/PolicyRulesTest" "$root/data/"
OD_WELLFED_ROOM=1 run "living standards"  "$bin/PolicyRulesTest" "$root/data/"
OD_INDOCTRINATION=1 run "assimilation"  "$bin/PolicyRulesTest" "$root/data/"
# Both arms of the two effects that were advertised and never applied. The
# off arm above already proved they are inert; this proves they arrive.
OD_PACIFICATION_REBATE=1 OD_AI_RECRUIT_CAP=1 \
  run "dead effects, flags on"  "$bin/PolicyRulesTest" "$root/data/"
# Industry in state hands, wired into a real map. The off arm above already
# proved it inert; this proves the ramp reaches income, upkeep and unrest.
OD_NATIONALISATION=1 run "nationalisation"  "$bin/PolicyRulesTest" "$root/data/"
# Where a factory may stand: the capacity rule's shape, its pinned constants,
# and the cos(latitude) area walk the loader runs. Pure arithmetic, no data dir.
run "industry capacity" "$bin/IndustryCapacityTest"
# The goods recipes: can every country feed itself, and do the strategic goods
# stay strategic. Pure arithmetic over the recipe table, no data dir.
run "goods recipes"  "$bin/GoodsRecipeTest"
# Which ground a country may release: contiguity, majorities, and the rule that
# a nation cannot release its own people. Pure arithmetic, no data dir.
run "release rules"  "$bin/ReleaseRulesTest"
# Splitting a garrison: shares must sum to the garrison, and a lone 50% order
# must still leave half. Pure arithmetic, no data dir.
run "army split"     "$bin/ArmySplitTest"
run "ship routes"    "$bin/ShipRouteTest"
# What the men behind the frontage are worth: numbers must matter, and a
# fortified pass must still stop numbers. Pure arithmetic, no data dir.
run "combat depth"   "$bin/CombatDepthTest"
# Standing battles: men in a fight are still on the payroll, and withdrawing
# neither creates nor destroys soldiers. Pure arithmetic, no data dir.
run "battle rules"   "$bin/BattleRulesTest"
# Supply: distance costs, and a fleet offshore is the difference between a
# beachhead and an encirclement. Pure arithmetic, no data dir.
run "supply rules"   "$bin/SupplyRulesTest"
# Troop types on the wire: a line-infantry world writes byte-identical saves,
# and an older build reads a mixed army at the right strength.
run "troop types"    "$bin/TroopTypesTest"
run "research groups" "$bin/ResearchGroupsTest"
run "districts"      "$bin/DistrictRulesTest"
run "country profile" "$bin/CountryProfileTest"
run "feedback client" "$bin/FeedbackClientTest"
run "mail rules" "$bin/MailRulesTest"
run "ai advisor" "$bin/AdvisorTest"
run "advisor influence" "$bin/LlmInfluenceTest"
run "connect timeout" "$bin/NetConnectTimeoutTest"
run "tool download gate" "$bin/ToolReleaseTest"

# The one thing the advisor's unit tests cannot cover: that the body we build is
# accepted over a real socket and what comes back becomes a letter. Run against
# a stand-in runner, so it needs no model, no GPU and no download.
if command -v python3 >/dev/null 2>&1; then
    step "llm round trip"
    python3 "$root/tests/llm_stub.py" 8791 >/dev/null 2>&1 &
    stub_pid=$!
    # WAIT FOR THE PORT, do not sleep at it. `sleep 1` was enough on every
    # machine anyone had run this on and not enough on the macos-x64 runner,
    # where the test failed with "could not reach 127.0.0.1 in time" -- a
    # started-too-slowly race reported as a broken round trip. Poll until the
    # socket answers, up to 20s, and say plainly if it never does.
    ready=""
    for _ in $(seq 1 100); do
        if python3 -c "import socket,sys; s=socket.socket(); s.settimeout(0.2); sys.exit(0 if s.connect_ex(('127.0.0.1',8791))==0 else 1)" 2>/dev/null; then
            ready=1; break
        fi
        sleep 0.2
    done
    if [ -z "$ready" ]; then
        echo "  FAIL  the stand-in runner never came up on 127.0.0.1:8791"
        fail=1
    elif "$bin/LlmRoundTripTest" http://127.0.0.1:8791/v1; then :; else fail=1; fi
    kill "$stub_pid" 2>/dev/null
    wait "$stub_pid" 2>/dev/null
else
    echo "skip: python3 is not installed, so the stand-in runner cannot start"
fi

# The dedicated server, started for real: config round-trip, data directory,
# map resolution, a whole world load, and the console. It is a second binary
# with its own raylib underneath it, so none of the above says anything about
# whether it runs.
cmake --build "$build" --config Release --target OpenDoctrinesServer \
      -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
      >> "$build/test-targets-build.log" 2>&1 \
    && run "dedicated server starts" "$root/tests/server_smoke_test.sh" "$build" \
    || note_fail "dedicated server build"
# Drives the same async loader the game does, which SaveRoundTripTest cannot
# reach: that one links SaveManager alone and never turns a save into a world.
#
# It earned its place on the first run: it segfaulted on every CI runner and
# nowhere else, and the cause was a four-byte read of the two-byte border
# layer in rebuildGlowMap -- a latent overrun since 7f878d0 that nothing had
# ever driven, because reloadBorders() runs on a save load and not on a map
# load. Gating again now that it is fixed.
run "a damaged save is refused" "$root/tests/save_corrupt_test.sh" "$build"
run "neural net gradients" "$bin/NeuralNetTest"
# The model container, against the shipped model when it is there: the file
# is the only copy of every hour of training, so "it round-trips" is checked
# on the real bytes and not only on synthetic ones.
run "ai model container" "$bin/ModelBlobTest" "$root/data/ai/model.bin"
run "map-script expressions" "$bin/ScriptExprTest"
run "odsv turn delta"  "$bin/SaveDeltaTest"
# The archive around those deltas: written, closed, and opened again. The
# failure it exists for is the one players report -- "my save will not load"
# -- which until now had no test at any level.
run "odsv round trip"  "$bin/SaveRoundTripTest"
# The dialogue markup and its line breaker, then the character rig: loading
# the shipped test character, skinning it, blending poses, and the blink
# clock. The rig test reads data/characters/test, so it runs from the root.
run "dialogue markup"  "$bin/DialogTest"
run "languages"        "$bin/LocaleTest" "$root/data/"
# The keys are generated from the source, so they go stale the moment a string
# is added to a menu. Cheap to notice here; invisible until a translator asks
# why half the screen is missing from their file.
run "translatable strings" python3 "$root/tools/i18n_extract.py" --check
# Specifiers that do not match the English are undefined behaviour at runtime,
# and a Latin letter inside a Cyrillic word is invisible by eye. Both are
# cheap to check and neither is catchable by reading.
run "translation lint"     python3 "$root/tools/i18n_sync.py" --lint

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

# And do the generated Python bindings COMPILE? check_bindings.py is a text
# lint by its own admission; this asks the compiler, which is the only thing
# that can catch a generated call with the wrong arity.
check "generated script bindings compile" bash "$root/tools/check_script_bindings.sh"

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

# Doctrines. data/policies.json is generated, and every .odmap carries its own
# copy which WINS over data/ -- so the file in the repository and the file the
# game loads are two different files, and were.
check "doctrine data" $PY "$root/tools/check_policies.py"
# And does anything READ the levers those doctrines advertise? Four effect
# fields have shipped summed-but-never-spent. Not --strict: three are still
# dead and that is a backlog, not a regression -- but the report is in the log
# so it cannot be forgotten again.
check "advertised effects are spent" $PY "$root/tools/check_effect_fields.py"

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
    # Revoking a declared capability must refuse the mod: every wasm import
    # resolves at instantiation, so a module that imports one it was not
    # granted cannot be linked. Checked for Content as well as UI, because a
    # mod that declares a capability and imports nothing from it would pass
    # this silently -- which is what a wrongly named import looks like.
    revoked_ok=1
    for capname in UI Content; do
        grep -q "\"$capname\"" "$(dirname "$m")/MANIFEST.json" 2>/dev/null || continue
        if "$bin/odmod-check" "$m" --revoke "$capname" >/dev/null 2>&1; then
            echo "FAIL  $rel (loaded with $capname revoked -- capability not enforced)"
            fail=1; revoked_ok=0; break
        fi
    done
    [ $revoked_ok -eq 1 ] || continue
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
