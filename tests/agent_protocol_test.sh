#!/bin/bash
# The agent door's wire format, pinned.
#
# WHY THIS EXISTS. `--bench-agent` prints a turn as text and reads one line of
# `module:action` tokens back. That text is not a log -- it is an INTERFACE, and
# it already has readers outside this repository: Open Fly's driver
# (open_fly/protocol.py), Open Animal Stage's seats, and the TempleOS client,
# none of which are built or tested here. A rename in Game_Agent.cpp would
# compile, pass every other test, ship, and break all three silently; the fly
# would stop playing and nothing in this repository would have gone red.
#
# So this runs the real door for one turn and asserts the SHAPE of every line a
# reader depends on. It does not pin the numbers -- those change with the map,
# the seed and the model, and a test that fails when the AI improves is a test
# people delete. It pins the grammar.
#
#     tests/agent_protocol_test.sh <build-dir> [data-dir]
#
# Exit 0 if every pattern is present, 1 naming the first that is not.
set -u

build="${1:?usage: agent_protocol_test.sh <build-dir> [data-dir]}"
root="$(cd "$(dirname "$0")/.." && pwd)"
data="${2:-$root/data}"
server="$build/OpenDoctrinesServer"

[ -x "$server" ] || { echo "no $server -- build the OpenDoctrinesServer target first"; exit 1; }

# A capture can be supplied instead of running the door. Two uses: checking
# these assertions without a build, and the break-test that proves they fail --
# a format test nobody has ever seen fail is a format test nobody should trust.
if [ -n "${OD_AGENT_CAPTURE:-}" ]; then
    tmp="$(mktemp -d)"; out="$tmp/agent.txt"
    trap 'rm -rf "$tmp"' EXIT
    cp "$OD_AGENT_CAPTURE" "$out"
    capture_supplied=1
else
    capture_supplied=0
fi

if [ "$capture_supplied" -eq 0 ]; then
tmp="$(mktemp -d)"
fifo="$tmp/agent.fifo"
out="$tmp/agent.txt"
trap 'rm -rf "$tmp"' EXIT
mkfifo "$fifo" || exit 1

# ── ONE TURN, THEN STOP ──
#
# The door prints the position and blocks opening the FIFO for reading, so the
# capture is: start it, wait for "waiting", kill it. Nothing is ever written to
# the FIFO -- this tests what the door SAYS, and a turn does not need to resolve
# for that. A bounded wait, because a door that never prints is the failure this
# test is here to catch and it must not hang the suite.
"$server" --bench-agent 1914:SWE:rung "$fifo" --until 120 --seed 20260801 \
          --data "$data" > "$out" 2>&1 &
pid=$!
for _ in $(seq 1 120); do
    grep -q '^\[AGENT\] waiting$' "$out" 2>/dev/null && break
    kill -0 "$pid" 2>/dev/null || break
    sleep 1
done
kill "$pid" 2>/dev/null
wait "$pid" 2>/dev/null

fi   # end of "run the door ourselves"

if ! grep -q '^\[AGENT\] waiting$' "$out"; then
    echo "the door never reached '[AGENT] waiting' in 120s"
    echo "--- last 15 lines ---"
    tail -15 "$out"
    exit 1
fi

fail=0
# want <description> <extended-regex>
want() {
    if grep -Eq "$2" "$out"; then
        return 0
    fi
    echo "MISSING: $1"
    echo "         expected a line matching: $2"
    fail=1
}

# Each of these is read by at least one client. The comment names who, so
# whoever changes the format can see what they are about to break.
want "turn header (fly, stage, TempleOS: turn number, country, ISO)" \
     '^\[AGENT\] ===== turn [0-9]+/[0-9]+ +.+ \([A-Z0-9]+\) =====$'
want "position line (share/army/treasury -> the fly's sugar and water)" \
     '^\[AGENT\] land [0-9]+/[0-9]+ \([0-9.]+% of the world\) +army -?[0-9]+ +treasury -?[0-9.]+'
want "income line (net/gross -> the fly's reward signal)" \
     '^\[AGENT\] gross -?[0-9.]+ net -?[0-9.]+'
want "war line (drives the fly's Johnston's organ; '(nobody)' when at peace)" \
     '^\[AGENT\] at war with: '
want "economy menu" '^\[AGENT\] economy +e:[0-9]+ '
want "politics menu" '^\[AGENT\] politics +p:[0-9]+ '
want "war menu" '^\[AGENT\] war +w:[0-9]+ '
want "navy menu" '^\[AGENT\] navy +n:[0-9]+ '
want "budget line (every client caps its picks with this)" \
     '^\[AGENT\] budget e:[0-9]+ p:[0-9]+ w:[0-9]+ n:[0-9]+'
want "the prompt clients block on" '^\[AGENT\] waiting$'

# ── THE ITEM SEPARATOR IS LOAD-BEARING ──
#
# Menu items are `x:N name` joined by TWO spaces, and every client splits on
# exactly that: a single space cannot work, because names contain spaces
# ("declare war", "focus bldg"). One space here would silently merge two
# actions into one for every reader.
if ! grep -E '^\[AGENT\] economy ' "$out" | grep -Eq 'e:[0-9]+ [^ ]+.*  e:[0-9]+ '; then
    echo "MISSING: two-space separator between menu items"
    echo "         clients split on it; names contain single spaces"
    fail=1
fi

# ── AND THE MENU MUST NOT BE EMPTY ──
#
# A door that offers nothing is a door that looks healthy and cannot be played.
econ=$(grep -E '^\[AGENT\] economy ' "$out" | grep -oE 'e:[0-9]+' | wc -l | tr -d ' ')
if [ "${econ:-0}" -lt 2 ]; then
    echo "MISSING: a playable economy menu (found ${econ:-0} actions)"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "agent protocol: all 10 line shapes present, ${econ} economy actions offered"
fi
exit "$fail"
