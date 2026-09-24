#!/bin/bash
# The TempleOS bridge, played by two processes that share only a directory.
#
# WHY THIS AND NOT THE BRIDGE'S OWN --stub-player. That flag proves the engine
# side: the door is driven, turns resolve. It proves nothing about the thing the
# bridge exists for, because the stub is inside the same process and never
# touches a file. This runs the bridge and a SEPARATE player that share nothing
# but a path -- which is the guest's situation exactly, minus the disk image.
#
# And it checks the failure that will actually happen. A file left over from an
# earlier turn looks exactly like one written for this turn; if the bridge took
# it, the player would silently stop being asked and the game would play itself.
# So the test plants a stale orders.txt before the game starts and requires that
# it is not used.
#
#     tests/templeos_bridge_test.sh <build-dir> [data-dir]
set -u

build="${1:?usage: templeos_bridge_test.sh <build-dir> [data-dir]}"
root="$(cd "$(dirname "$0")/.." && pwd)"
data="${2:-$root/data}"
PY="$("$root/tools/find_python.sh" 2>/dev/null || echo python3)"
turns=5

[ -x "$build/OpenDoctrinesServer" ] || {
    echo "no $build/OpenDoctrinesServer -- build that target first"; exit 1; }

dir="$(mktemp -d)"
trap 'rm -rf "$dir"' EXIT

# ── THE STALE FILE ──
#
# Planted for a turn that will never come round, and left in place. If the
# bridge ever reads whatever is lying there rather than a file naming the turn
# on offer, it consumes this and the guest's own answer for turn 0 is never
# waited for -- so the guest answers fewer turns than it should, and the count
# below catches it.
# The guest plants one DURING each turn (--plant-stale). Planting before the
# run does nothing: the bridge clears orders.txt on startup, so a file put there
# first is gone before turn 0 -- which is how the first version of this test
# came to pass with the defence deleted.

"$PY" "$root/templeos/bridge.py" --build "$build" --dir "$dir" --data "$data" \
      --turns "$turns" > "$dir/bridge.log" 2>&1 &
bridge=$!

"$PY" "$root/templeos/guest_sim.py" --dir "$dir" --turns "$turns" --plant-stale \
      > "$dir/guest.log" 2>&1
guest_rc=$?

wait "$bridge" 2>/dev/null
bridge_rc=$?

fail=0
if [ "$guest_rc" -ne 0 ]; then
    echo "the guest did not answer $turns turns"; cat "$dir/guest.log"; fail=1
fi
if [ "$bridge_rc" -ne 0 ]; then
    echo "the bridge exited $bridge_rc"; tail -20 "$dir/bridge.log"; fail=1
fi

exchanged=$(grep -oE 'bridge: ([0-9]+) turn' "$dir/bridge.log" | grep -oE '[0-9]+' | head -1)
if [ "${exchanged:-0}" -lt "$turns" ]; then
    echo "only ${exchanged:-0} of $turns turns were exchanged"
    tail -20 "$dir/bridge.log"; fail=1
fi

# ── THE ASSERTION THAT MATTERS ──
#
# Counting turns is not enough, and this test learned that the hard way: with
# the turn-number check deleted the bridge happily ate the planted stale file
# for turn 0, the guest went on to answer turns 1..4 instead, and BOTH counts
# still came out right. The test passed while the defence it was written for
# was gone.
#
# So compare the content. Every line the engine was SENT must be a line the
# guest WROTE, for the same turn. A consumed stale file breaks that immediately:
# the engine gets the planted tokens for turn 0 and the guest never wrote them.
"$PY" - "$dir/bridge.log" "$dir/guest.log" <<'EOF' || fail=1
import re, sys
sent = dict(re.findall(r"turn (\d+): sent '([^']*)'", open(sys.argv[1]).read()))
wrote = dict(re.findall(r"guest: turn (\d+) -> '([^']*)'", open(sys.argv[2]).read()))
if not sent:
    print("the bridge logged no sends at all"); raise SystemExit(1)
bad = [(t, sent[t], wrote.get(t)) for t in sent if wrote.get(t) != sent[t]]
if bad:
    for t, s, w in bad:
        print(f"turn {t}: engine was sent {s!r} but the guest wrote {w!r}")
    print("an order reached the engine that the player did not write")
    raise SystemExit(1)
print(f"  every one of {len(sent)} orders matched what the guest wrote")
EOF

[ "$fail" -eq 0 ] && echo "templeos bridge: $exchanged turns over files, stale order refused"
exit "$fail"
