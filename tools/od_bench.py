#!/usr/bin/env python3
"""The absolute score. One number per model, and one for a person.

    tools/od_bench.py                      # score data/ai/model.bin
    tools/od_bench.py --model path.bin     # score a specific model
    tools/od_bench.py --compare a b        # two stored results side by side
    tools/od_bench.py --list-seats         # what a person has to play

WHY THIS EXISTS
===============

Every other instrument in this project is RELATIVE, and on 2026-08-29 that cost
five hours of training. `ADVANTAGE` is a ratio against whoever was in the other
cohort. `--vs-model` plays a model against its own parent. Even `land held`, the
one that behaves, is a share of a world split with an opponent. All three answer
"better than that", and none of them answers "good".

The failure that follows is not hypothetical. Over five hours the model beat its
own predecessor at every merge -- the pool's keep-the-winner guard passed each
one -- while losing SEVEN POINTS of land against a rusher, on five worlds out of
five. A lineage can walk downhill indefinitely while every relative reading says
it is climbing, because the thing it is being compared to is walking down beside
it.

WHAT A SEAT IS
==============

One country, played for a fixed number of turns, on a fixed map from a fixed
seed, with EVERY OTHER COUNTRY IN THE WORLD played by the scripted rung. The
rung is hand-written and frozen: it does not learn, so it does not drift, so it
is a ruler rather than an opponent.

The score for a seat is simply the share of the world that seat holds at the
end. Nothing in that number depends on what it was measured against, which means
a score taken today is comparable with one taken next month, and -- the point of
the whole exercise -- with one taken by a person playing the same seat.

READING THE NUMBER
==================

`par` is what the seat starts with. A seat scores 100 when it ends the run the
size it began, 200 when it doubled, 0 when it was wiped off the map. The rating
is the mean of those, so:

    100   held every seat
    >100  grew on balance
    <100  lost ground on balance
    0     annihilated everywhere

Scoring each seat against ITS OWN par is what makes the seats commensurable. A
mean of raw world-shares does not work, and the first run proved it: the model
was annihilated on two seats of five -- Norway and China both to zero -- and
still scored well above par, because France and the USA are ten times the size
of Norway and drowned it. Dying is not allowed to be a rounding error.

Par is a property of the seat, fixed forever in the table below, so this is
still an absolute number: nothing in it moves when the model moves. That is the
one thing ADVANTAGE and --vs-model cannot say.

The ratio is capped at CAP per seat. A ratio against a small par explodes -- the
lesson ADVANTAGE taught expensively -- and without a cap a single runaway Norway
would swamp the other four seats and the rating would be measuring one seat.

Do not add, remove or reorder seats, or change par, without renaming the
benchmark. The moment the set changes, old scores stop being comparable to new
ones, which is the exact disease this exists to cure.
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import time
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "build", "od_bench_results.json")
# Where the latest rating is left for anything that wants to show it without
# running a bench of its own -- the training HUD reads this. One line, plain
# text, so a C++ reader needs no parser: "RATING SEATS SEEDS EPOCH".
#
# Epoch seconds rather than a timestamp: what a HUD needs to say is how STALE
# the number is, and "14m ago" is the readable form of that. A reader that only
# has an ISO string has to parse a date to work out the one thing it wants.
SCORE_FILE = os.path.join(ROOT, "data", "ai", "bench_score.txt")
SCORE = re.compile(r"^\[BENCH\] seat (\S+)\s+score ([0-9.]+)")

# ── THE SEAT SET. FIXED. ──
#
# Five seats spanning three eras and a wide range of starting sizes, so the
# score is not just "can it play France". `par` is the seat's starting share of
# the world, measured from the map data; it is recorded here so the report can
# print it without loading the map.
#
# TURNS is 120 for every seat, chosen so a person can actually sit down and play
# one. A model does not care; the human half of this benchmark is the reason the
# number exists at all, and a 400-turn seat would never be played by anybody.
TURNS = 120
DIFFICULTY = 3          # the rung that ships as the hardest
# Seeds a seat is played on. A single seed is ONE game, and one game of a grand
# strategy title is mostly weather -- measured across this project, single-seed
# readings pointed the wrong way five times in a day. The rating averages a seat
# over all of them, so a seat is a distribution rather than an anecdote.
SEEDS = [20260801, 4242, 90210]
# The most a single seat may score. Five times its starting size is a runaway
# result by any standard, and the cap stops one lucky small seat from being the
# whole rating. See READING THE NUMBER.
CAP = 5.0

# `world` is who everybody ELSE plays:
#   "rung"  the ordinary hand-written policy -- can this play the game at all
#   "rush"  SCRIPT_BLITZ everywhere -- can it survive a world at war
#   "hood"  SCRIPT_BLITZ for the seat's single largest land neighbour, everyone
#           else on the rung -- can it survive one aggressive neighbour
#
# "hood" is one neighbour and not all of them, which is the opposite of what it
# sounds like it should be. Measured on both: with EVERY neighbour rushing, the
# seat is the only non-rusher any of them can see and they all converge on it,
# so it dies harder than in a world-wide rush where the aggressors are busy with
# each other. 1939:NOR scored 13 with the world rushing, 23 with all neighbours
# rushing -- and identically for two very different models in both cases, which
# is the definition of a seat that ranks nobody. With ONE rushing neighbour it
# survives and often doubles, and the two models come apart 144 to 113.
#
# BOTH KINDS ARE REQUIRED, and the first version of this file got that wrong.
# It scored every seat against the rung, so it could not see rush-resistance at
# all -- the exact failure it was built to catch. Scored on rung seats alone, a
# model that had lost seven points of rush-resistance came out 62 points AHEAD,
# and on that reading the wrong model was nearly shipped. A benchmark blind to
# the failure it was written for is worse than none, because it is trusted.
SEATS = [
    # (map, isoA3, world, par% at start, what makes it interesting)
    ("1914",   "FRA", "rung", 6.7, "a great power with hostile neighbours on two sides"),
    ("1914",   "SWE", "rung", 1.0, "a small neutral with room to expand if it dares"),
    ("1939",   "USA", "rung", 5.6, "large, rich, and nothing adjacent to fight"),
    ("modern", "CHN", "rung", 2.5, "a mid power in the crowded present-day world"),
    ("1914",   "FRA", "rush", 6.7, "the same France, in a world where everyone attacks"),
    ("1939",   "NOR", "hood", 1.3, "small and exposed, with one aggressive neighbour"),
]

# The exploit variant --vs-exploit takes for a rushing world. 3 is SCRIPT_BLITZ.
RUSH_VARIANT = 3


def binary_path(explicit):
    if explicit:
        return explicit
    return os.path.join(ROOT, "build", "OpenDoctrines.app", "Contents",
                        "MacOS", "OpenDoctrines")


def run_seat(binary, mapname, iso, world, model, seed):
    env = dict(os.environ)
    if model:
        env["OD_EVAL_MODEL"] = os.path.abspath(model)
    cmd = [binary, "--eval-ai", "1", str(TURNS), str(seed), str(DIFFICULTY),
           "--scenarios", "--bench-seat", f"{mapname}:{iso}"]
    if world == "rush":
        cmd += ["--vs-exploit", str(RUSH_VARIANT)]
    elif world == "hood":
        cmd += ["--vs-exploit", str(RUSH_VARIANT), "--rush-neighbours", "1"]
    try:
        # Gated: a bench run alongside a training worker and a recording
        # put a 16 GB machine into swap. See tools/odlock.py.
        gated = [sys.executable, os.path.join(ROOT, "tools", "odlock.py"), "--"] + cmd
        out = subprocess.run(gated, cwd=ROOT, env=env, capture_output=True,
                             text=True, errors="replace", timeout=7200).stdout
    except (subprocess.SubprocessError, OSError) as e:
        print(f"  {mapname}:{iso} seed {seed} FAILED ({e})")
        return None
    for line in out.splitlines():
        m = SCORE.match(line)
        if m:
            return float(m.group(2))
    print(f"  {mapname}:{iso}: no [BENCH] line in {len(out.splitlines())} lines")
    return None


def load_store():
    if not os.path.exists(RESULTS):
        return {}
    try:
        with open(RESULTS) as f:
            return json.load(f)
    except (ValueError, OSError):
        return {}


def seat_score(share, par):
    """A seat's score: 100 = ended the size it started. See READING THE NUMBER."""
    if par <= 0:
        return 0.0
    return min(share / par, CAP) * 100.0


def report(label, scores):
    """scores: {"map:iso": world share}. Returns the rating."""
    print(f"\n  {'seat':<18} {'held':>6} {'par':>6} {'score':>7}   ")
    print(f"  {'-'*18} {'-'*6} {'-'*6} {'-'*7}")
    vals = []
    for mapname, iso, world, par, _why in SEATS:
        key = f"{mapname}:{iso}:{world}"
        v = scores.get(key)
        label_ = f"{mapname}:{iso} {world}"
        if v is None:
            print(f"  {label_:<18} {'--':>6} {par:>6.1f} {'--':>7}")
            continue
        sc = seat_score(v, par)
        vals.append(sc)
        note = "  wiped out" if v <= 0.05 else ("  capped" if v / par > CAP else "")
        print(f"  {label_:<18} {v:>6.1f} {par:>6.1f} {sc:>7.0f}{note}")
    if not vals:
        return None
    rating = statistics.mean(vals)
    print(f"\n  {label}: OD BENCH {rating:.0f}   "
          f"(100 = held every seat; 0 = annihilated everywhere)")
    return rating


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", default=None,
                    help="name to store this result under (default: the model's filename)")
    ap.add_argument("--model", default=None,
                    help="model file to score (default: whatever the game loads)")
    ap.add_argument("--binary", default=None)
    ap.add_argument("--compare", nargs=2, metavar=("A", "B"),
                    help="print two stored results side by side and stop")
    ap.add_argument("--list-seats", action="store_true",
                    help="print the seats a person needs to play, and stop")
    ap.add_argument("--record", nargs="+", metavar="SEAT=HELD",
                    help="store a score played by a PERSON, e.g. "
                         "--record 1914:FRA:rush=8.2 --label vlad")
    ap.add_argument("--seed", type=int, default=None,
                    help="with --record: which seed was played (default the first)")
    ap.add_argument("--quick", action="store_true",
                    help="a fast estimate: the two France seats on one seed. Not a "
                         "rating -- see QUICK below")
    args = ap.parse_args()

    if args.list_seats:
        print(__doc__.split("READING THE NUMBER")[0].rstrip())
        print(f"\nPlay each of these for {TURNS} turns, then read your score off "
              f"the end-of-run screen.\n")
        for mapname, iso, world, par, why in SEATS:
            extra = ("  [every other country ATTACKS without pause]" if world == "rush"
                     else "  [your largest neighbour ATTACKS without pause]" if world == "hood"
                     else "")
            print(f"  {mapname:<8} {iso:<4} par {par:>4.1f}%   {why}{extra}")
        return

    store = load_store()

    # ── A SCORE A PERSON PLAYED ──
    #
    # Recorded rather than run, and kept in the same file as the models so
    # --compare works across the two without anything special. The seed matters
    # and is stored: a seat run is deterministic, so a person's game is only
    # comparable to the AI's on the SAME world, and comparing it to the AI's
    # three-seed mean would be comparing one game to an average of three.
    if args.record:
        if not args.label:
            sys.exit("--record needs --label to say whose score it is")
        seed = args.seed if args.seed is not None else SEEDS[0]
        seats = store.get(args.label, {}).get("seats", {})
        known = {f"{m}:{i}:{w}": par for m, i, w, par, _ in SEATS}
        for item in args.record:
            if "=" not in item:
                sys.exit(f"expected SEAT=HELD, got {item!r}")
            key, val = item.rsplit("=", 1)
            # A seat with no world spelled out means the ordinary one.
            if key.count(":") == 1:
                key += ":rung"
            if key not in known:
                sys.exit(f"no seat {key!r}. Seats: {', '.join(sorted(known))}")
            try:
                seats[key] = float(val)
            except ValueError:
                sys.exit(f"{val!r} is not a share of the world")
            print(f"  recorded {key} = {float(val):.1f} "
                  f"(par {known[key]:.1f}, score {seat_score(float(val), known[key]):.0f})")
        store[args.label] = {"seats": seats, "turns": TURNS,
                             "difficulty": DIFFICULTY, "seeds": [seed],
                             "human": True,
                             "seat_set": [f"{m}:{i}:{w}" for m, i, w, _, _ in SEATS]}
        os.makedirs(os.path.dirname(RESULTS), exist_ok=True)
        with open(RESULTS, "w") as f:
            json.dump(store, f, indent=1)
        report(args.label, seats)
        print(f"\n  stored as {args.label!r}. Compare with:  "
              f"tools/od_bench.py --compare {args.label} rolled-back")
        return

    if args.compare:
        a, b = args.compare
        for name in (a, b):
            if name not in store:
                sys.exit(f"no stored result named {name!r}. "
                         f"have: {', '.join(sorted(store)) or '(none)'}")
        ca = report(a, store[a]["seats"])
        cb = report(b, store[b]["seats"])
        if ca is not None and cb is not None:
            d = cb - ca
            print(f"\n  {b} is {abs(d):.0f} {'above' if d > 0 else 'below'} {a}")
            better = sum(1 for m, i, w, _, _ in SEATS
                         if f"{m}:{i}:{w}" in store[a]["seats"]
                         and f"{m}:{i}:{w}" in store[b]["seats"]
                         and store[b]["seats"][f"{m}:{i}:{w}"] > store[a]["seats"][f"{m}:{i}:{w}"])
            print(f"  better on {better}/{len(SEATS)} seats")
        return

    binary = binary_path(args.binary)
    if not os.path.exists(binary):
        sys.exit(f"no binary at {binary}")

    # ── QUICK: an estimate, deliberately not called a rating ──
    #
    # Two seats on one seed, both France: the ordinary world answers "can it
    # play" and the rushing one "can it survive", which are the two halves the
    # full set exists to keep separate. It runs in about a minute instead of
    # twenty, which is what makes it usable from inside a training run.
    #
    # It is NOT comparable to a full rating and must not be stored as one: one
    # seed is one game, and single-seed readings in this project pointed the
    # wrong way five times in a single day. It is a trend line, not a score.
    seats, seeds = SEATS, SEEDS
    if args.quick:
        seats = [s for s in SEATS if s[0] == "1914" and s[1] == "FRA"]
        seeds = SEEDS[:1]
    label = args.label or (os.path.basename(args.model) if args.model else "model.bin")

    print(f"[BENCH] {label}: {len(SEATS)} seats x {len(SEEDS)} seeds x "
          f"{TURNS} turns, difficulty {DIFFICULTY}")
    scores = {}
    for mapname, iso, world, par, _why in seats:
        got = [run_seat(binary, mapname, iso, world, args.model, sd) for sd in seeds]
        got = [g for g in got if g is not None]
        if not got:
            continue
        mean = statistics.mean(got)
        scores[f"{mapname}:{iso}:{world}"] = mean
        spread = "  ".join(f"{g:.1f}" for g in got)
        print(f"  {mapname}:{iso} {world:<5} {mean:5.1f}  (par {par:.1f})   [{spread}]")

    rating = report(label, scores)
    if rating is None:
        sys.exit("[BENCH] nothing measured")

    store[label] = {"seats": scores, "turns": TURNS,
                    "difficulty": DIFFICULTY, "seeds": SEEDS,
                    "seat_set": [f"{m}:{i}:{w}" for m, i, w, _, _ in SEATS]}
    os.makedirs(os.path.dirname(RESULTS), exist_ok=True)
    with open(RESULTS, "w") as f:
        json.dump(store, f, indent=1)
    print(f"  stored as {label!r} in {os.path.relpath(RESULTS, ROOT)}")

    # Left for the training HUD; see SCORE_FILE.
    try:
        os.makedirs(os.path.dirname(SCORE_FILE), exist_ok=True)
        with open(SCORE_FILE, "w") as f:
            # Trailing field so an older reader still parses the first four.
            f.write(f"{rating:.0f} {len(seats)} {len(seeds)} {int(time.time())} {len(SEATS)}\n")
    except OSError:
        pass


if __name__ == "__main__":
    main()
