#!/bin/sh
# Does the same seed play the same game twice?
#
# WHY THIS EXISTS. Two separate bugs made the simulation non-reproducible, and
# neither was noticed for a long time because nothing checked:
#
#   * raylib's InitWindow calls SetRandomSeed(time(NULL)), and the turn resolver
#     used rand(), so every run of a fixed seed played a different game. Two
#     evaluations of one model read 0.32x and 0.54x -- a spread wider than most
#     effects worth measuring, silently inflating every interval.
#
#   * after that was fixed, the audio system's background pump thread was STILL
#     drawing from the same global rand(). Roughly one run in four diverged,
#     always from the same turn, and it vanished under tracing because the extra
#     output changed the thread interleaving. A Heisenbug, found only after it
#     had already produced several confident wrong conclusions.
#
# Both would have been caught here in seconds. Determinism is not a nicety for
# this project: every AI measurement is a comparison, and a comparison against a
# simulation that will not repeat itself is not a measurement at all.
#
# Runs are short and the horizon is small on purpose -- both historical bugs
# diverged by turn 3, so this does not need to play a whole game to catch them.
#
# SIX runs, not three, and the number is arithmetic rather than taste. The audio
# thread bug fired on roughly one run in four; with three runs this compares two
# pairs against the first and catches it about 44% of the time, which is a test
# that mostly does not work. Six gives about 76%. Even that is not certainty --
# a rarer divergence will still slip through, and the honest reading of a pass
# is "no divergence seen in six", not "deterministic". Raise the count if you
# are chasing something rarer.
#
# Usage: tests/determinism_check.sh <build-dir>
set -e
build="${1:-build}"
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

bin=""
# Both Windows layouts. Ninja writes build/OpenDoctrines.exe and MSBuild
# writes build/Release/OpenDoctrines.exe; the extensionless name is not a
# reliable test for either under Git Bash, and this check SKIPS rather than
# fails when it finds nothing -- so a missed path here is a check that quietly
# stops running.
for c in "$build/OpenDoctrines.app/Contents/MacOS/OpenDoctrines" \
         "$build/OpenDoctrines" "$build/OpenDoctrines.exe" \
         "$build/Release/OpenDoctrines.exe"; do
    [ -x "$c" ] && { bin="$c"; break; }
done
if [ -z "$bin" ]; then
    echo "  skip  no game binary in $build (determinism check needs one)"
    exit 0
fi

# A model is not required: a fresh one is deterministic too, and the check is
# about the SIMULATION repeating rather than about any particular policy.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

runs=6
i=1
while [ "$i" -le "$runs" ]; do
    OD_DET_TRACE=1 "$bin" --resource-limit 90 --eval-ai 1 25 4242 2 --vs-random 2>&1 \
        | grep -a '^\[DET\]' > "$tmp/run$i.txt" || true
    i=$((i + 1))
done

if [ ! -s "$tmp/run1.txt" ]; then
    echo "  skip  the binary produced no trace (headless display unavailable?)"
    exit 0
fi

turns=$(wc -l < "$tmp/run1.txt" | tr -d ' ')
fail=0
i=2
while [ "$i" -le "$runs" ]; do
    if ! cmp -s "$tmp/run1.txt" "$tmp/run$i.txt"; then
        echo "  FAIL  run $i diverged from run 1"
        # Name the turn: it is the single most useful fact for whoever debugs it.
        paste -d'@' "$tmp/run1.txt" "$tmp/run$i.txt" | awk -F'@' '$1!=$2{
            print "        A: " $1; print "        B: " $2; exit }'
        fail=1
    fi
    i=$((i + 1))
done

if [ "$fail" -eq 0 ]; then
    echo "  ok    $runs runs of seed 4242 agree over $turns turns"
else
    echo ""
    echo "  The same seed no longer plays the same game. Something in the turn"
    echo "  resolver depends on state that is not part of the seed -- shared"
    echo "  mutable state, a thread, or wall-clock time. OD_DEC_TRACE=1 prints"
    echo "  per-country decisions and feature hashes, which localises it fast."
fi
exit "$fail"
