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

# A WATCHDOG, because a hung binary is not a slow one.
#
# With the Mac's display asleep, the windowed binary stops inside raylib's
# initialisation and never comes back -- it prints its module list, reaches
# "raudio: loaded", and sits there for ever with no window and no crash. This
# loop then waited on it for ever, and since tests/run_all.sh calls this script,
# THE WHOLE SUITE HUNG.
#
# That is not a hypothetical cost. It is how stale doctrine data sat in all
# seven shipped maps for a day: the suite could not be run to completion, so the
# check that would have caught it was never read. A check that cannot run in the
# environment it is used in has to SKIP, loudly, not block.
#
# `timeout` is not available on macOS, so this is done by hand.
det_timeout="${OD_DET_TIMEOUT:-180}"
run_bounded() {
    "$@" &
    _pid=$!
    _waited=0
    while kill -0 "$_pid" 2>/dev/null; do
        if [ "$_waited" -ge "$det_timeout" ]; then
            kill -9 "$_pid" 2>/dev/null || true
            wait "$_pid" 2>/dev/null || true
            return 124
        fi
        sleep 1
        _waited=$((_waited + 1))
    done
    wait "$_pid" 2>/dev/null || true
    return 0
}

# ── PIN THE MODEL, OR THIS MEASURES THE FILESYSTEM ──
#
# Every run loads data/ai/model.bin, and that file is WRITTEN BY OTHER THINGS:
# a game running with AI Learning on saves it as the player takes turns, and a
# training loop rewrites it outright. Six runs spread over a few minutes can
# therefore load six different models, diverge for exactly that reason, and be
# reported as "the same seed no longer plays the same game" -- pointing whoever
# reads it at the turn resolver, where there is nothing to find.
#
# Observed directly: model.bin went 6bedb6ce -> c10c35f7 DURING one of these
# checks, while a game was open in another window.
#
# So the runs get their own data directory with a frozen copy. Nothing else can
# write it, and a divergence now means what the message says it means.
det_data="$tmp/data"
mkdir -p "$det_data/ai"
if [ -f "$root/data/ai/model.bin" ]; then
    cp "$root/data/ai/model.bin" "$det_data/ai/model.bin"
    # Everything else the run needs is read-only, so it is linked rather than
    # copied: the maps alone are hundreds of megabytes.
    for entry in "$root/data"/*; do
        name=$(basename "$entry")
        [ "$name" = "ai" ] && continue
        ln -s "$entry" "$det_data/$name" 2>/dev/null || true
    done
    export OD_DATA_DIR="$det_data"
    echo "  (model pinned for the run: $(basename "$det_data"))"
fi

export OD_DET_TRACE=1
runs=6
i=1
while [ "$i" -le "$runs" ]; do
    rc=0
    run_bounded "$bin" --resource-limit 90 --eval-ai 1 25 4242 2 --vs-random \
        > "$tmp/raw$i.txt" 2>&1 || rc=$?
    if [ "$rc" -eq 124 ]; then
        echo "  skip  the binary did not finish in ${det_timeout}s and was killed."
        echo "        On macOS this is almost always a sleeping display: raylib"
        echo "        stalls in init with no monitor. Wake the screen and re-run;"
        echo "        caffeinate cannot wake a display that is already asleep."
        exit 0
    fi
    grep -a '^\[DET\]' "$tmp/raw$i.txt" > "$tmp/run$i.txt" || true
    i=$((i + 1))
done

if [ ! -s "$tmp/run1.txt" ]; then
    echo "  skip  the binary produced no trace (headless display unavailable?)"
    exit 0
fi

turns=$(wc -l < "$tmp/run1.txt" | tr -d ' ')
fail=0
empty=0
i=2
while [ "$i" -le "$runs" ]; do
    # AN EMPTY RUN IS NOT A DIVERGENCE, and calling it one sends somebody
    # hunting a determinism bug that is not there.
    #
    # The windowed binary intermittently produces no trace at all under load --
    # a different run each time, while every run that DID trace agreed with the
    # first. That is the display or the GPU losing a race with whatever else is
    # on the machine, not the simulation disagreeing with itself. It was
    # reported as "run 3 diverged from run 1" with an empty B, which is the
    # least helpful true statement available.
    if [ ! -s "$tmp/run$i.txt" ]; then
        echo "  note  run $i produced no trace at all and is not being compared"
        echo "        (a windowed run that never got a window; see the skip above)"
        empty=$((empty + 1))
        i=$((i + 1))
        continue
    fi
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
    compared=$((runs - empty))
    if [ "$empty" -gt 0 ]; then
        echo "  ok    $compared of $runs runs of seed 4242 agree over $turns turns"
        echo "        ($empty produced no trace and were skipped -- see the notes above.)"
        # Two runs is not the six this check is sized for; say so rather than
        # let a thin pass read like a full one. See the note on SIX runs above.
        [ "$compared" -lt 3 ] && echo "  warn  too few runs compared to mean much; re-run on a quiet machine."
    else
        echo "  ok    $runs runs of seed 4242 agree over $turns turns"
    fi
else
    echo ""
    echo "  The same seed no longer plays the same game. Something in the turn"
    echo "  resolver depends on state that is not part of the seed -- shared"
    echo "  mutable state, a thread, or wall-clock time. OD_DEC_TRACE=1 prints"
    echo "  per-country decisions and feature hashes, which localises it fast."
fi
exit "$fail"
