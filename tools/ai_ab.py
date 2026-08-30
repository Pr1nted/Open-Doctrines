#!/usr/bin/env python3
"""Measure one AI build over several seeds, in the statistic that behaves.

    tools/ai_ab.py --label before --seeds 5
    ...change the code, rebuild...
    tools/ai_ab.py --label after  --seeds 5 --compare before

WHY THIS EXISTS
===============

Every A/B in this project is a comparison of two builds on the same maps, and
until now it was read off the `ADVANTAGE` line. That line is a RATIO of two land
totals, so it is unbounded and it explodes whenever the scripted cohort is
nearly wiped out. Measured on 2026-08-29 with one frozen model, one binary and
one configuration -- nothing varying but the seed, at 400 turns:

    seed 20260801   ADVANTAGE 1.51x
    seed 4242       ADVANTAGE 4.04x

A spread of two and a half against effect sizes of about a third. On the first
of those seeds a change that loses land looked like a 55% improvement, and the
write-up was finished before the control run on the second seed contradicted it.
Four hours of reasoning went into the bin.

`land held` is bounded in [0, 1] and moves monotonically with the thing being
measured. tools/train_parallel.py::_fitness has said so since PBT was ranking
workers backwards. This tool exists so that saying it once is enough.

WHAT IT DOES
============

Runs `--eval-ai` on N seeds, pulls the `land held` share, and reports the mean
with its spread. `--compare` sets the previous run beside it and answers whether
the difference is real -- PAIRED, seed by seed, because the two runs played the
same worlds and a paired difference cancels the variation between them. It also
prints how many of the world sets the change helped on, which is often the more
honest number than the mean: a change that wins on five worlds out of five is a
different animal from one that wins on three and loses on two by more.

WHAT IT DOES NOT DO
===================

It does not run the two builds itself. Only one build exists at a time in a
working tree, so the honest workflow is: measure, change, rebuild, measure. The
results file is what carries the first half across the rebuild.

A note on what the seeds are actually varying. A single (seed, build) run is
DETERMINISTIC -- same seed, same binary, same model, same answer every time. So
the spread here is not measurement noise in the usual sense: it is the variance
of the underlying question, "how much land does this AI hold", across different
worlds. That is exactly the thing a two-map run cannot see, and exactly the
thing that decides whether a change generalises.
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "build", "ai_ab_results.json")
LAND = re.compile(r"^\[EVAL\] land held\s+([0-9.]+)%")

# Fixed and arbitrary, which is the point: every run must use the SAME worlds or
# two results are not comparable. Extending the list is fine; reordering it is
# not, because a 3-seed run must be a prefix of a 5-seed one.
SEEDS = [20260801, 4242, 90210, 13371337, 5150, 271828, 161803, 8675309]


def run_one(binary, seed, turns, maps, difficulty, exploit, model):
    env = dict(os.environ)
    if model:
        env["OD_EVAL_MODEL"] = model
    cmd = [binary, "--eval-ai", str(maps), str(turns), str(seed), str(difficulty),
           "--scenarios"]
    cmd += ["--vs-exploit", str(exploit)] if exploit else ["--vs-script"]
    try:
        # errors="replace", because the run is not always valid UTF-8: the
        # procedural namer emits country names in bytes that are not, and a
        # decode error here throws away a completed 20-minute measurement for
        # a character in a line this tool does not even read.
        out = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True,
                             text=True, errors="replace", timeout=7200).stdout
    except (subprocess.SubprocessError, OSError) as e:
        print(f"  seed {seed}: FAILED ({e})")
        return None
    for line in out.splitlines():
        m = LAND.match(line)
        if m:
            return float(m.group(1)) / 100.0
    print(f"  seed {seed}: no 'land held' line in {len(out.splitlines())} lines")
    return None


def summarise(vals):
    mean = statistics.mean(vals)
    sd = statistics.stdev(vals) if len(vals) > 1 else 0.0
    se = sd / (len(vals) ** 0.5) if len(vals) > 1 else 0.0
    return mean, sd, se


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", required=True, help="name for this measurement")
    ap.add_argument("--seeds", type=int, default=5,
                    help=f"how many of the fixed seed list to use (max {len(SEEDS)})")
    ap.add_argument("--turns", type=int, default=200)
    ap.add_argument("--maps", type=int, default=2)
    ap.add_argument("--difficulty", type=int, default=2)
    ap.add_argument("--exploit", type=int, default=0,
                    help="control cohort plays this exploit instead of the rung")
    ap.add_argument("--model", default=None, help="OD_EVAL_MODEL override")
    ap.add_argument("--binary",
                    default=os.path.join(ROOT, "build", "OpenDoctrines.app",
                                         "Contents", "MacOS", "OpenDoctrines"))
    ap.add_argument("--compare", default=None, help="label of an earlier run")
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        sys.exit(f"no binary at {args.binary}")
    seeds = SEEDS[:min(args.seeds, len(SEEDS))]
    what = f"exploit {args.exploit}" if args.exploit else "scripted rung"
    print(f"[AB] {args.label}: {len(seeds)} seed(s), {args.maps} map(s) x "
          f"{args.turns} turns, vs the {what}")

    vals = []
    for s in seeds:
        v = run_one(args.binary, s, args.turns, args.maps, args.difficulty,
                    args.exploit, args.model)
        if v is None:
            continue
        vals.append(v)
        print(f"  seed {s:>9}: {v * 100:5.1f}%")
    if not vals:
        sys.exit("[AB] nothing measured")

    mean, sd, se = summarise(vals)
    print(f"[AB] {args.label}: mean {mean*100:.1f}%  sd {sd*100:.1f}  "
          f"se {se*100:.1f}  (n={len(vals)})")

    os.makedirs(os.path.dirname(RESULTS), exist_ok=True)
    store = {}
    if os.path.exists(RESULTS):
        try:
            with open(RESULTS) as f:
                store = json.load(f)
        except (ValueError, OSError):
            store = {}
    store[args.label] = {"vals": vals, "turns": args.turns, "maps": args.maps,
                         "exploit": args.exploit, "difficulty": args.difficulty}
    with open(RESULTS, "w") as f:
        json.dump(store, f, indent=1)

    if args.compare:
        prev = store.get(args.compare)
        if not prev:
            print(f"[AB] no earlier run labelled {args.compare!r}")
            return
        for k in ("turns", "maps", "exploit", "difficulty"):
            if prev.get(k) != getattr(args, k):
                print(f"[AB] REFUSING to compare: {args.compare} used {k}="
                      f"{prev.get(k)}, this run used {getattr(args,k)}")
                return
        pmean, psd, pse = summarise(prev["vals"])
        print(f"[AB] {args.compare}: mean {pmean*100:.1f}%  sd {psd*100:.1f} "
              f"(n={len(prev['vals'])})")

        # ── PAIRED, BECAUSE THE SEEDS ARE THE SAME WORLDS ──
        #
        # A single (seed, build) run is deterministic: same seed, same binary,
        # same model, same answer. So two builds measured on seed 4242 differ
        # ONLY by the code, with no sampling error at all -- the spread in the
        # column above is variation between WORLDS, not measurement noise, and
        # it is shared by both runs.
        #
        # Comparing the means unpaired throws that away and inflates the band
        # enormously: on the first baseline it demanded 5.7 points, which is
        # larger than most changes worth making. Differencing seed by seed
        # cancels the world entirely and asks the only question that matters --
        # does this change help on EVERY world, or did it help on one?
        n = min(len(vals), len(prev["vals"]))
        deltas = [vals[i] - prev["vals"][i] for i in range(n)]
        dmean, dsd, dse = summarise(deltas)
        wins = sum(1 for d in deltas if d > 0)
        print("[AB] per seed: " +
              "  ".join(f"{d*100:+.1f}" for d in deltas))
        band = 2.0 * dse
        print(f"[AB] paired difference {dmean*100:+.1f} points "
              f"(sd {dsd*100:.1f}, band +/-{band*100:.1f}), "
              f"better on {wins}/{n} world sets")
        if abs(dmean) <= band:
            print("[AB] NOT SEPARABLE -- do not claim this change did anything.")
        else:
            print(f"[AB] {args.label} is {'BETTER' if dmean > 0 else 'WORSE'}.")


if __name__ == "__main__":
    main()
