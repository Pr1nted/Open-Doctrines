#!/usr/bin/env bash
# Can a player actually join a host? Start to finish, on this machine.
#
# Brings up a stand-in account service (tests/mock_issuer.mjs), then runs a real
# NetHost and a real NetSession against each other over loopback. Nothing here
# touches the network or a real account: the mock signs with a throwaway key it
# generates at startup.
#
# Two runs, and the second matters as much as the first:
#
#   join    -- a well-formed ticket gets the player in
#   refuse  -- a ticket the host cannot verify does NOT
#
# Usage:  tests/connectivity_test.sh [build-dir]

set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build}"
fail=0

step() { printf '\n=== %s ===\n' "$1"; }

if ! command -v node >/dev/null 2>&1; then
    echo "skip: node is not installed, and the stand-in account service needs it"
    echo "      (everything else in tests/run_all.sh still covers each half"
    echo "       separately -- this is the test of the seam between them)"
    exit 0
fi

step "build"
cmake --build "$build" --target NetConnectTest >/dev/null || {
    echo "build failed"; exit 1;
}

# A port nothing else is on. Asking the OS for one and closing it immediately
# is racy in principle; in practice it is what is available without a helper,
# and a collision shows up as a clean failure rather than a wrong result.
pick_port() {
    node -e 'const s=require("net").createServer();s.listen(0,"127.0.0.1",()=>{console.log(s.address().port);s.close();});'
}

MOCK_PID=""
cleanup() {
    if [ -n "$MOCK_PID" ] && kill -0 "$MOCK_PID" 2>/dev/null; then
        kill "$MOCK_PID" 2>/dev/null
        wait "$MOCK_PID" 2>/dev/null
    fi
}
trap cleanup EXIT

run_case() {
    local mode="$1"; shift
    local extra="${1:-}"

    local port
    port="$(pick_port)"
    local log
    log="$(mktemp)"

    node "$root/tests/mock_issuer.mjs" --port "$port" $extra > "$log" 2>&1 &
    MOCK_PID=$!

    # Wait for it to say it is listening rather than sleeping a fixed amount.
    local waited=0
    while ! grep -q "mock-issuer ready" "$log" 2>/dev/null; do
        sleep 0.1
        waited=$((waited + 1))
        if [ "$waited" -gt 100 ]; then
            echo "the stand-in account service never came up:"
            cat "$log"
            rm -f "$log"
            return 1
        fi
        if ! kill -0 "$MOCK_PID" 2>/dev/null; then
            echo "the stand-in account service exited:"
            cat "$log"
            rm -f "$log"
            return 1
        fi
    done

    "$build/NetConnectTest" "http://localhost:$port" "$mode"
    local rc=$?

    cleanup
    MOCK_PID=""
    rm -f "$log"
    return $rc
}

step "a player joins a host"
run_case join || fail=1

step "a ticket the host cannot verify is refused"
run_case refuse --wrong-key || fail=1

# On a real network the client takes a few hundred milliseconds to fetch its
# ticket. That used to be enough to stall the join forever: the client's socket
# thread was parked in a blocking read, and the ticket it wanted to send waited
# behind it until something arrived. Localhost hid it completely, because the
# reply came back inside the 20 ms the thread spends waiting on its queue.
step "a join still completes when the account service is slow"
run_case join "--delay 600" || fail=1

if [ "$fail" -eq 0 ]; then
    printf '\nCONNECTIVITY OK\n'
else
    printf '\nCONNECTIVITY FAILED\n'
fi
exit $fail
