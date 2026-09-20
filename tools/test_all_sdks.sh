#!/usr/bin/env bash
# Builds every Gearbox SDK example and runs the full mod test suite.
#
#   eval "$(tools/sdk_toolchains.sh env)"      # once per shell
#   tools/test_all_sdks.sh
#
# Each language that builds is then driven through its real draw path by
# ModExamplesTest, which compares the output across languages -- that is the
# part that actually validates a binding, not merely that it links.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build}"
fail=0
built=""; skipped=""

try() {   # try <label> <script...>
    local label="$1"; shift
    printf '\n=== %s ===\n' "$label"
    if [ ! -x "$1" ]; then echo "  no build script"; skipped="$skipped $label"; return; fi
    if "$@" >/tmp/sdkbuild.$$ 2>&1; then
        tail -1 /tmp/sdkbuild.$$
        built="$built $label"
    else
        echo "  build failed:"; tail -8 /tmp/sdkbuild.$$ | sed 's/^/    /'
        skipped="$skipped $label"
    fi
    rm -f /tmp/sdkbuild.$$
}

try C              "$root/sdk/examples/hello-panel/build.sh"
# A content mod, not a panel one: no UI, no hooks, two doctrines. Built
# here so a change to the Content bindings has to keep it compiling.
try "C (content)"  "$root/sdk/examples/custom-doctrine/build.sh"
try "Lua (content)" "$root/sdk/lua/examples/custom-doctrine/build.sh"
try "Lua (first)"   "$root/sdk/lua/examples/first-doctrine/build.sh"
try C++            "$root/sdk/cpp/build.sh"
try AssemblyScript "$root/sdk/assemblyscript/build.sh"
try Rust           "$root/sdk/rust/build.sh"
try Zig            "$root/sdk/zig/examples/hello-panel/build.sh"
try Go             "$root/sdk/go/examples/hello-panel/build.sh"
try WAT            "$root/sdk/wat/build.sh"
# The six below were missing for as long as this script existed, and their
# absence is not visible in its output: ModExamplesTest drives whatever .odmod
# files are ON DISK, so a stale Java or Python module built months ago compares
# green against freshly built ones and nothing says it was never rebuilt. A
# change to a shared example -- as the Content call was -- would then be
# "verified across every language" while six of the twelve still ran the old
# code. Building all thirteen here is what makes the comparison mean what it
# says.
try Java           "$root/sdk/java/examples/hello-panel/build.sh"
try Kotlin         "$root/sdk/java/examples/hello-panel-kotlin/build.sh"
try TypeScript     "$root/sdk/js/examples/hello-panel-ts/build.sh"
try JavaScript     "$root/sdk/js/examples/hello-panel/build.sh"
try Lua            "$root/sdk/lua/examples/hello-panel/build.sh"
try Python         "$root/sdk/python/examples/hello-panel/build.sh"

printf '\nbuilt:  %s\nfailed/skipped: %s\n' "${built:-none}" "${skipped:-none}"

printf '\n=== full mod test suite ===\n'
"$root/tests/run_all.sh" "$build" || fail=1
exit $fail
