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

# The stand-in issuer signs with Ed25519 through WebCrypto, which arrived in
# Node 18. Older Node does not fail with "unsupported algorithm" -- it fails to
# PARSE the file, because the same file uses top-level await, and reports a
# bare `SyntaxError: Unexpected reserved word` pointing at a crypto line. That
# is a genuinely misleading way to be told your Node is eight years old, and it
# cost a full container run to diagnose, so it is checked up front.
#
# Ubuntu 22.04's `nodejs` package is 12.22, so `apt-get install nodejs` is
# enough to hit this on a perfectly ordinary machine.
node_major="$(node -p 'process.versions.node.split(".")[0]' 2>/dev/null || echo 0)"
if [ "${node_major:-0}" -lt 18 ]; then
    echo "FAIL: node $(node --version 2>/dev/null) is too old for the stand-in"
    echo "      account service, which needs WebCrypto Ed25519 (node 18+)."
    echo
    echo "      On Ubuntu the distro package is node 12; install a current one:"
    echo "        curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -"
    echo "        sudo apt-get install -y nodejs"
    echo "      In CI, use actions/setup-node (release-sdk.yml already does)."
    exit 1
fi

step "build"
# --config Release: Visual Studio ignores CMAKE_BUILD_TYPE and takes the
# configuration here. Harmless on Make and Ninja.
cmake --build "$build" --config Release --target NetConnectTest >/dev/null || {
    echo "build failed"; exit 1;
}

# A multi-config generator writes build/Release/, not build/. Without this the
# five cases below each reported "build/NetConnectTest: No such file or
# directory" -- which reads as a binary that failed to build, when in fact it
# had built perfectly well one directory over.
bin="$build"
if [ -x "$build/Release/NetConnectTest" ] || [ -f "$build/Release/NetConnectTest.exe" ]; then
    bin="$build/Release"
fi

# THE PORT IS CHOSEN BY WHATEVER BINDS IT.
#
# There used to be a pick_port() here that asked Node for a free port, printed
# it, and closed the listener -- and the comment above it conceded the race and
# argued a collision would show up as "a clean failure rather than a wrong
# result". It does, and that is the problem: it is a clean failure attributed
# to whichever commit was unlucky. One turned up on windows-x64 as
#
#     FAIL  the player is welcomed -- rejected: That server refused the connection.
#
# on a push that changed nothing but map data, while the same case passed
# seconds later in the same run. Something took the port between the close and
# the issuer's bind, the host could not reach the issuer to verify a ticket,
# and the join was refused.
#
# So the issuer is started with --port 0 and reports the port it actually got.
# There is no window left to lose.

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

    local log
    log="$(mktemp)"

    node "$root/tests/mock_issuer.mjs" --port 0 $extra > "$log" 2>&1 &
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

    # The ready line carries the bound port; take it from there rather than
    # assuming we know it. Anchored on the URL and digits so the trailing
    # " (publishing a WRONG key)" of the --wrong-key case cannot confuse it.
    local port
    port="$(sed -n 's|.*mock-issuer ready on http://localhost:\([0-9][0-9]*\).*|\1|p' "$log" | head -1)"
    if [ -z "$port" ]; then
        echo "the stand-in account service came up but reported no port:"
        cat "$log"
        rm -f "$log"
        cleanup
        MOCK_PID=""
        return 1
    fi

    "$bin/NetConnectTest" "http://localhost:$port" "$mode"
    local rc=$?

    cleanup
    MOCK_PID=""
    rm -f "$log"
    return $rc
}

step "a player joins a host"
run_case join || fail=1

# Three joiners rather than one, because a two-peer test cannot see the things
# that only go wrong with a party: seats that collide, a country claimed twice,
# a turn that begins for whoever was pumped last, and a reconnect that hands
# somebody another player's country. See testParty().
step "four in one game, including a disconnect and a reconnect"
run_case party || fail=1

step "a mismatched mod set is refused"
run_case mods || fail=1

step "a ticket the host cannot verify is refused"
run_case refuse --wrong-key || fail=1

# On a real network the client takes a few hundred milliseconds to fetch its
# ticket. That used to be enough to stall the join forever: the client's socket
# thread was parked in a blocking read, and the ticket it wanted to send waited
# behind it until something arrived. Localhost hid it completely, because the
# reply came back inside the 20 ms the thread spends waiting on its queue.
step "a join still completes when the account service is slow"
run_case join "--delay 600" || fail=1

# ── NOBODY SAYING ANYTHING IS THE NORMAL CASE ──
#
# Every case above keeps both ends talking. Players do not: they sit in a lobby
# without clicking, and their game stops pumping the socket entirely while it
# loads the world the host just sent. All of that used to read as a dead
# connection, and the host cut them off mid-game -- which is what happened to
# the first people who joined a 1.2.2a game. The intervals are shortened here
# so the rule can be watched in seconds; the rule is the game's own.
step "a quiet lobby, a busy client, a blocked host, and a four-megabyte world"
OD_NET_DEAD_SECONDS=4 OD_WS_PING_MS=700 run_case quiet || fail=1

# The same, against a client that predates the keepalive -- which is every copy
# of 1.2.2a anyone has installed. Such a client says nothing at all, so what
# has to carry it is the host's ping, the reply the WebSocket layer has always
# sent, and the host not counting its own blocked frame loop against it.
step "a player on the released build, who sends nothing, is still not dropped"
OD_NET_DEAD_SECONDS=4 OD_WS_PING_MS=700 OD_WS_NO_KEEPALIVE=1 run_case quiet || fail=1

# ── THE RELAY CASE, WITHOUT A RELAY ──
#
# Over the relay the host never sees the socket: it counts the frames the relay
# forwards, and a WebSocket ping is not one of them. Neither is anything a
# browser can send. So with BOTH pings switched off -- the client's and the
# host's -- the only thing that can keep this player seated is the session
# keepalive, which is a frame like any other and goes wherever frames go.
step "with no socket pings at all, the session's own keepalive carries the player"
OD_NET_DEAD_SECONDS=8 OD_NET_KEEPALIVE_SECONDS=2 OD_WS_PING_MS=600000 OD_WS_NO_KEEPALIVE=1 \
    run_case idle || fail=1

# The screen reads the lobby every frame, from the moment a host presses Host --
# including the seconds before the account service has answered. Whether that is
# SAFE is a question for tests/net_race_test.sh, which runs this same case under
# ThreadSanitizer; here it is a smoke test that the window works at all.
step "the lobby is read while the host is still opening"
run_case race "--delay-host 700" || fail=1

# ── THE WAY A BROWSER PLAYER ARRIVES ──
#
# Nothing exercised the relay until this case existed, and the first run of it
# found a crash: the relay join answered its own challenge on the worker
# thread, which joins that thread, which is an uncaught system_error. Only the
# web build escaped, because there the same work is queued rather than
# threaded. The stand-in relay is in tests/mock_issuer.mjs.
step "a host and two players, all through the relay"
run_case relay || fail=1

if [ "$fail" -eq 0 ]; then
    printf '\nCONNECTIVITY OK\n'
else
    printf '\nCONNECTIVITY FAILED\n'
fi
exit $fail
