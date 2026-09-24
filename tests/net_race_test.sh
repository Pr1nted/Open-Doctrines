#!/usr/bin/env bash
# Does anything touch the lobby from two threads at once?
#
# The lobby has no lock, and it never needed one while only the frame loop
# touched it. The host's opener thread used to admit the host itself, a second
# or two after the invite code appeared -- so the screen could be walking
# roster() (drawMpLobby does, every frame, and "Find players for this game"
# does again on a click) while push_back reallocated underneath it. That is a
# crash, and the report was "the server crashes when I press the invite button".
#
# It cannot be caught by running the game: it is a race, it needs a slow account
# service to open the window, and when it does fire it fires as a segfault
# somewhere else entirely. So this builds the connectivity test with
# ThreadSanitizer and runs the one case that reads the lobby throughout the
# opening, against a stand-in account service told to be slow FOR THE HOST
# (--delay-host), which is what a real one is.
#
# Usage:  tests/net_race_test.sh [build-dir]
#
# Verified to catch what it is for: with the seating put back on the opener
# thread it reports two races on Lobby::m_members and aborts.

set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build-tsan}"

if ! command -v node >/dev/null 2>&1; then
    echo "skip: node is not installed, and the stand-in account service needs it"
    exit 0
fi

# Apple clang and every clang since 3.2 have it; gcc has it too. A toolchain
# without it skips rather than fails: this is an extra pass over code the rest
# of the suite already covers, not a gate on the build working.
if ! echo 'int main(){}' | "${CXX:-c++}" -fsanitize=thread -x c++ - -o /dev/null 2>/dev/null; then
    echo "skip: this compiler has no ThreadSanitizer"
    exit 0
fi
rm -f /dev/null.dSYM 2>/dev/null

echo "=== building the connectivity test with ThreadSanitizer ==="
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Debug -DOD_ENABLE_NET=ON \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
      -DCMAKE_C_FLAGS="-fsanitize=thread -g -O1" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" >/dev/null || {
    echo "FAIL: could not configure the sanitized build"; exit 1; }
cmake --build "$build" --target NetConnectTest -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null || {
    echo "FAIL: could not build the sanitized test"; exit 1; }

log="$(mktemp)"
node "$root/tests/mock_issuer.mjs" --port 0 --delay-host 700 > "$log" 2>&1 &
mock=$!
cleanup() { kill "$mock" 2>/dev/null; wait "$mock" 2>/dev/null; rm -f "$log"; }
trap cleanup EXIT

waited=0
while ! grep -q "mock-issuer ready" "$log" 2>/dev/null; do
    sleep 0.1
    waited=$((waited + 1))
    if [ "$waited" -gt 100 ] || ! kill -0 "$mock" 2>/dev/null; then
        echo "the stand-in account service never came up:"; cat "$log"; exit 1
    fi
done
port="$(sed -n 's|.*mock-issuer ready on http://localhost:\([0-9][0-9]*\).*|\1|p' "$log" | head -1)"

echo "=== reading the lobby while the host opens ==="
out="$(mktemp)"
TSAN_OPTIONS="halt_on_error=0" "$build/NetConnectTest" "http://localhost:$port" race > "$out" 2>&1
rc=$?
cat "$out"
races="$(grep -c 'WARNING: ThreadSanitizer' "$out")"
rm -f "$out"

if [ "$races" -ne 0 ]; then
    echo; echo "NET RACE FAILED: $races data race(s) reported"
    exit 1
fi
if [ "$rc" -ne 0 ]; then
    echo; echo "NET RACE FAILED: the case itself did not pass"
    exit 1
fi
echo; echo "NET RACE OK"
