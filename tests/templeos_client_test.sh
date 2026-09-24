#!/bin/bash
# Does the TempleOS client still understand what the door says?
#
# The other two tests guard the engine: agent_protocol_test.sh pins the shape of
# the door's output, templeos_bridge_test.sh plays a game over files. Neither
# says anything about OpenDoc.HC, because checking that needs a TempleOS, and CI
# does not have one.
#
# But the way OpenDoc.HC breaks does not need one either. It finds its fields by
# matching literal prefixes -- "land ", "at war with: " -- and then skipping a
# fixed number of characters past them. So two things can rot, and both rot
# SILENTLY: the client draws a turn with blank fields and nobody is told.
#
#   1. The door renames a line. The client matches nothing.
#   2. Somebody edits a literal and not the offset beside it, or the reverse.
#      The client matches, then reads from the wrong column.
#
# Both are checkable here. The first against a real capture, the second against
# the source alone -- an offset must equal the length of the literal it follows.
#
#     tests/templeos_client_test.sh <build-dir> [data-dir]
#     OD_AGENT_CAPTURE=file tests/templeos_client_test.sh   (no build needed)
#     OD_TEMPLEOS_CLIENT=file ...                           (for break-testing)
set -u

build="${1:-}"
root="$(cd "$(dirname "$0")/.." && pwd)"
data="${2:-$root/data}"
client="${OD_TEMPLEOS_CLIENT:-$root/templeos/OpenDoc.HC}"
PY="$("$root/tools/find_python.sh" 2>/dev/null || echo python3)"

[ -f "$client" ] || { echo "no $client"; exit 1; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
out="$tmp/agent.txt"

if [ -n "${OD_AGENT_CAPTURE:-}" ]; then
    cp "$OD_AGENT_CAPTURE" "$out"
else
    server="$build/OpenDoctrinesServer"
    [ -x "$server" ] || { echo "no $server -- build the OpenDoctrinesServer target first"; exit 1; }
    fifo="$tmp/agent.fifo"
    mkfifo "$fifo" || exit 1
    # Same capture as agent_protocol_test.sh: start it, wait for the block on
    # the FIFO, kill it. Nothing is ever written back -- this tests what the
    # door SAYS, and a turn need not resolve for that.
    "$server" --bench-agent 1914:SWE:rung "$fifo" --until 120 --seed 20260801 \
              --data "$data" > "$out" 2>&1 &
    pid=$!
    for _ in $(seq 1 120); do
        grep -q '^\[AGENT\] waiting$' "$out" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.25
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
fi

grep -q '^\[AGENT\] ' "$out" || { echo "the capture has no [AGENT] lines at all"; exit 1; }

"$PY" - "$client" "$out" <<'EOF'
import re, sys

client, capture = open(sys.argv[1]).read(), open(sys.argv[2]).read()
agent = [l[8:] for l in capture.splitlines() if l.startswith("[AGENT] ")]
fail = []

# ── 1. every prefix the client matches must still be said ──
#
# ODStarts(a, "X") means "a line whose body begins X". The one match against
# the whole line is the "[AGENT] " prefix itself, checked by the shell above.
prefixes = [m for m in re.findall(r'ODStarts\(a,\s*"([^"]+)"\)', client)]
if not prefixes:
    fail.append("found no ODStarts(a, ...) prefixes in the client -- has it been rewritten?")
for p in prefixes:
    if not any(l.startswith(p) for l in agent):
        fail.append(f'the client looks for a line beginning {p!r}; the door prints none')

# ── 2. every word it searches for mid-line must still be there ──
for w in re.findall(r'StrMatch\("([^"]+)"', client):
    if not any(w in l for l in agent):
        fail.append(f'the client searches for {w!r} inside a line; no line contains it')

# ── 3. the offsets must match the literals ──
#
# The pattern is: test a prefix, then skip past it by a number written out by
# hand. `ODStarts(a, "land ")` ... `q = a + 5`. If the two disagree the client
# still matches the line and then reads from the wrong column, which is the
# quietest possible failure.
#
# Scanned line by line, not with one regex over the branch body. The first
# version of this used a regex that could not cross the `}` of a nested if, so
# it silently matched no branches at all and passed a deliberately broken
# client. Hence also the count in the success line: a check that quietly stops
# checking anything should be visible from the outside.
checked = 0
lines = client.splitlines()
for i, line in enumerate(lines):
    m = re.search(r'ODStarts\(a,\s*"([^"]+)"\)', line)
    if not m:
        continue
    lit = m[1]
    for probe in lines[i:i + 4]:
        off = re.search(r'\ba\s*\+\s*(\d+)', probe)
        if off:
            checked += 1
            if int(off[1]) != len(lit):
                fail.append(f'the client skips {off[1]} characters past {lit!r}, '
                            f'which is {len(lit)} long -- it will read from the '
                            f'wrong column')
            break

if checked < len(prefixes) - 1:   # "waiting" is a flag and skips nothing
    fail.append(f"only {checked} of {len(prefixes)} prefixes had an offset to "
                f"check -- this test has stopped testing what it says it does")

for f in fail:
    print("  " + f)
if fail:
    print(f"the TempleOS client no longer matches the door ({len(fail)} problem(s))")
    raise SystemExit(1)
print(f"templeos client: {len(prefixes)} prefixes, {checked} offsets, all still match the door")
EOF
