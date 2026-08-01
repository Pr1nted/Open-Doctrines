#!/bin/sh
# Take one AI benchmark reading and append it to the log.
#
#   tools/ai_benchmark.sh [maps] [turns]      default 2 maps x 300 turns
#
# Plays the trained model against countries choosing uniformly at random from
# the same legal moves, and records one line of the result. Prints it too, so it
# is useful interactively as well as from a loop.
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

out=$("$BIN" --resource-limit 20 --eval-ai "$MAPS" "$TURNS" "$SEED" 2 --vs-random 2>&1)

adv=$(printf  '%s\n' "$out" | grep -a 'ADVANTAGE'  | awk '{print $3}')
land=$(printf '%s\n' "$out" | grep -a 'land held'  | awk '{print $4}')
won=$(printf  '%s\n' "$out" | grep -a 'maps won'   | sed 's/.*maps won *//')
amph=$(printf '%s\n' "$out" | grep -a 'amphibious' | awk '{print $3}')
calls=$(printf '%s\n' "$out"| grep -a 'coalition'  | awk '{print $3}')

if [ -z "$adv" ]; then
    echo "the evaluation produced no ADVANTAGE line; full output follows" >&2
    printf '%s\n' "$out" | tail -20 >&2
    exit 1
fi

row=$(printf '%s  samples=%-14s ADVANTAGE=%-7s land=%-7s maps=%-22s landings=%-5s calls_answered=%s' \
    "$(date '+%m-%d %H:%M')" "$n" "$adv" "$land" "$won" "$amph" "$calls")
printf '%s\n' "$row" >> "$LOG"
printf '%s\n' "$row"
