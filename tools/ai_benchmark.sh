#!/bin/sh
# Take one AI benchmark reading and append it to the log.
#
#   tools/ai_benchmark.sh [maps] [turns] [repeats]   default 2 x 300, 3 repeats
#
# Plays the trained model against countries choosing uniformly at random from
# the same legal moves, and records one line of the result. Prints it too, so it
# is useful interactively as well as from a loop.
#
# WHY IT REPEATS
#
# The turn simulation is not bit-deterministic. Measured directly: the same
# model, the same command and the same map seed produced 0.61x and 0.71x on two
# back-to-back runs, with identical maps, identical cohorts and identical
# survivors -- only the final province counts drifted a few percent, which in a
# ratio of two similar numbers is a tenth of a point. A single reading therefore
# cannot distinguish a real change from that drift.
#
# So each measurement is several evals against ONE frozen model (the merge
# happens once, before any of them), averaged. The logged row carries the range
# as well as the mean, so the noise is visible rather than implied.
#
# WHY IT MERGES FIRST
#
# A parallel training pool (tools/train_parallel.py) trains into
# data/ai/model.wN.bin and only folds those into data/ai/model.bin when the
# launcher exits. --eval-ai reads model.bin. So measuring during a run without
# merging first reports the same stale snapshot every time -- a dead flat line,
# for a reason that has nothing to do with learning. Merging also keeps
# model.bin current as a crash net; workers never read it after seeding, so
# writing it under them is safe. With no pool running there is nothing to merge
# and model.bin is measured directly.
#
# READING THE RESULT
#
# ADVANTAGE is land held by the model cohort over land held by the random one.
# Below 1.00 the trained policy is losing to chance. Both cohorts share every
# reflex and heuristic gate, so this measures what the learned DECISIONS add on
# top of the machinery, not "AI versus no AI". At two maps the scatter is wide:
# judge a trend across several rows, never a single reading.
#
#   OD_EVAL_LOG   where to append   (default /tmp/od-eval.log)
#   OD_EVAL_SEED  map seed          (default 20260801; keep it fixed or rows
#                                    are not comparable)

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG="${OD_EVAL_LOG:-/tmp/od-eval.log}"
SEED="${OD_EVAL_SEED:-20260801}"
MAPS="${1:-2}"
TURNS="${2:-300}"
REPEATS="${3:-${OD_EVAL_REPEATS:-3}}"

BIN=""
for c in "build/OpenDoctrines.app/Contents/MacOS/OpenDoctrines" \
         "build/OpenDoctrines" "build/OpenDoctrines.exe" \
         "build-release/OpenDoctrines.app/Contents/MacOS/OpenDoctrines" \
         "build-release/OpenDoctrines"; do
    [ -x "$ROOT/$c" ] && { BIN="$ROOT/$c"; break; }
done
[ -n "$BIN" ] || { echo "no built binary found. Run: cmake --build build -j" >&2; exit 1; }

cd "$ROOT" || exit 1
export OD_DATA_DIR="$ROOT/data/"

# Lifetime experience of the politics head — the x-axis for the curve. Samples
# rather than turns: it is what the exploration schedule reads.
samples() {
    python3 - "$1" <<'PY' 2>/dev/null || echo "?"
import struct, sys
d = open(sys.argv[1], 'rb').read()
q = 6
for i in range(2):                        # net index 1 is the politics head
    (ln,) = struct.unpack_from('<I', d, q); q += 4
    if i == 1: break
    q += ln
b = d[q:q+ln]
n = struct.unpack_from('<I', b, 8)[0]
print(f"{struct.unpack_from('<II', b, 12 + 4*n)[0]:,}")
PY
}

have=""
for f in data/ai/model.w0.bin data/ai/model.w1.bin data/ai/model.w2.bin \
         data/ai/model.w3.bin data/ai/model.w4.bin; do
    [ -f "$f" ] && have="$have $f"
done
# shellcheck disable=SC2086
[ -n "$have" ] && "$BIN" --merge-ai data/ai/model.bin $have >/dev/null 2>&1

[ -f data/ai/model.bin ] || { echo "no data/ai/model.bin to measure" >&2; exit 1; }
n=$(samples data/ai/model.bin)

# Every repeat measures the SAME model: nothing merges or trains into
# model.bin between them, so the spread below is pure measurement noise.
stats=$(
  i=0
  while [ "$i" -lt "$REPEATS" ]; do
    i=$((i + 1))
    o=$("$BIN" --resource-limit 20 --eval-ai "$MAPS" "$TURNS" "$SEED" 2 --vs-random 2>&1)
    a=$(printf  '%s\n' "$o" | grep -a 'ADVANTAGE'  | awk '{print $3}' | tr -d 'x')
    l=$(printf  '%s\n' "$o" | grep -a 'land held'  | awk '{print $4}' | tr -d '%')
    mw=$(printf '%s\n' "$o" | grep -a 'maps won'   | awk '{print $4}')
    rw=$(printf '%s\n' "$o" | grep -a 'maps won'   | awk '{print $6}')
    am=$(printf '%s\n' "$o" | grep -a 'amphibious' | awk '{print $3}' | tr -d '%')
    ca=$(printf '%s\n' "$o" | grep -a 'coalition'  | awk '{print $3}' | tr -d '%')
    [ -n "$a" ] && echo "$a $l $mw $rw $am $ca"
  done
)

[ -n "$stats" ] || { echo "every evaluation failed; nothing recorded" >&2; exit 1; }

summary=$(printf '%s\n' "$stats" | awk '
    { n++; a+=$1; l+=$2; mw+=$3; rw+=$4; am+=$5; ca+=$6
      if (n==1 || $1<lo) lo=$1; if (n==1 || $1>hi) hi=$1 }
    END { printf "%.2f %.2f %.2f %.1f %d %d %.0f %.0f %d", a/n, lo, hi, l/n, mw, rw, am/n, ca/n, n }')
set -- $summary
adv="$1"; lo="$2"; hi="$3"; land="$4"; mw="$5"; rw="$6"; amph="$7"; calls="$8"; nrun="$9"

row=$(printf '%s  samples=%-14s ADVANTAGE=%-5sx (%s-%s, n=%s)  land=%-6s maps=%s/%s model  landings=%-4s calls_answered=%s' \
    "$(date '+%m-%d %H:%M')" "$n" "$adv" "$lo" "$hi" "$nrun" \
    "$land%" "$mw" "$((mw + rw))" "$amph%" "$calls%")
printf '%s\n' "$row" >> "$LOG"
printf '%s\n' "$row"
