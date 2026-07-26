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
try C++            "$root/sdk/cpp/build.sh"
try AssemblyScript "$root/sdk/assemblyscript/build.sh"
try Rust           "$root/sdk/rust/build.sh"
try Zig            "$root/sdk/zig/examples/hello-panel/build.sh"
try Go             "$root/sdk/go/examples/hello-panel/build.sh"
try WAT            "$root/sdk/wat/build.sh"

printf '\nbuilt:  %s\nfailed/skipped: %s\n' "${built:-none}" "${skipped:-none}"

printf '\n=== full mod test suite ===\n'
"$root/tests/run_all.sh" "$build" || fail=1
exit $fail
