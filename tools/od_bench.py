#!/usr/bin/env python3
"""The absolute score. One number per model, and one for a person.

    tools/od_bench.py                      # score data/ai/model.bin
    tools/od_bench.py --model path.bin     # score a specific model
    tools/od_bench.py --compare a b        # two stored results side by side
    tools/od_bench.py --list-seats         # what a person has to play

CONFIRM BEFORE YOU SHIP
=======================

The three seeds below are FIXED, so anything tuned against them is fitted to
three worlds. Measured 2026-09-06: a war-bar change worth +52 on these seeds
was worth -28 on three unseen ones, and a day of careful A/Bs (own controls,
replicated across three models) produced a configuration that gained 8 rating
and lost survival 100 -> 85 on worlds it had not been chosen against.

    OD_BENCH_SEEDS=31337,777001,20251225 tools/od_bench.py --model x.bin

Use it to CONFIRM a change before it becomes a default -- never to choose one.
Tuning against a hold-out set burns it; pick fresh seeds if that happens.
Cross-model agreement is not independent evidence when the models share worlds.

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
import math
import statistics
import subprocess
import time
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "build", "od_bench_results.json")


def save_label(label, entry):
    """Merge ONE label into the results file under a lock, re-reading it first.

    Two benches finishing in the same minute used to lose one: each had
    loaded the store at its start and wrote its own stale copy at its end
    (N13's result vanished under N11's, 2026-09-04). Re-read, merge, write to
    a temp file, rename -- so a concurrent bench can neither clobber this
    label nor read a half-written file."""
    import fcntl, tempfile
    os.makedirs(os.path.dirname(RESULTS), exist_ok=True)
    with open(RESULTS + ".lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        current = {}
        if os.path.exists(RESULTS):
            try:
                with open(RESULTS) as f:
                    current = json.load(f)
            except (OSError, ValueError):
                current = {}
        current[label] = entry
        fd, tmp = tempfile.mkstemp(dir=os.path.dirname(RESULTS), prefix=".od_bench_", suffix=".json")
        with os.fdopen(fd, "w") as f:
            json.dump(current, f, indent=1)
        os.replace(tmp, RESULTS)
# Where the latest rating is left for anything that wants to show it without
# running a bench of its own -- the training HUD reads this. One line, plain
# text, so a C++ reader needs no parser: "RATING SEATS SEEDS EPOCH".
#
# Epoch seconds rather than a timestamp: what a HUD needs to say is how STALE
# the number is, and "14m ago" is the readable form of that. A reader that only
# has an ISO string has to parse a date to work out the one thing it wants.
SCORE_FILE = os.path.join(ROOT, "data", "ai", "bench_score.txt")
SCORE = re.compile(r"^\[BENCH\] seat (\S+)\s+score ([0-9.]+)")
# Phase 1 of the community roadmap (2026-09): one line per surviving country
# at the end of a run, "[CAPACITY] cid=6 used=199 total=650 overcap=3".
# used = built industry levels, total = per-province capacity summed,
# overcap = provinces grandfathered above their cap. Countries with no
# capacity are skipped, so absent is not zero. Collected per seat-run into
# CAP_RUNS and summarised at the end; the seat's own country is not
# identifiable here (the line carries a cid, the seat an iso), so the
# readout is the WORLD's utilisation on that seat's map.
CAPACITY = re.compile(r"^\[CAPACITY\] cid=(\d+) used=(\d+) total=(\d+) overcap=(\d+)")
CAP_RUNS = []   # one dict per seat-run: {"used", "total", "overcap", "countries"}

# Phase 2 (goods economy), silent unless OD_GOODS is on:
#   [LIVING] cid=6 ls=1.000 idle=3 factories=40 sold=12.5
#   [GOODS]  cid=6 good=consumer produced=1.20 consumed=1.10 demand=1.30 stock=4.00
# ls = consumer supply over demand (1.0 = fed). Per seat-run: mean ls over the
# countries printed, idle share of all factories, and per-good sums. Keyed by
# good id, so a two-good or four-good world differs in rows only.
LIVING = re.compile(r"^\[LIVING\] cid=(\d+) ls=([0-9.]+) idle=(\d+) factories=(\d+) sold=([0-9.]+)")
GOODS  = re.compile(r"^\[GOODS\] cid=(\d+) good=(\w+) produced=([0-9.]+) consumed=([0-9.]+) demand=([0-9.]+) stock=([0-9.]+)")
GOODS_RUNS = []

# People and armies at the end of a seat-run: "[POP] world pop=N army=N" (one
# per run) after per-country lines. The world's population was found to grow
# ~52x over 120 turns (2026-09-04); this is the readout that keeps it visible.
POP_WORLD = re.compile(r"^\[POP\] world pop=(\d+) army=(\d+)")
POP_RUNS = []

# Phase 6 combat width: "[WIDTH] assaults=N bound=N (P%)" once per run.
WIDTH = re.compile(r"^\[WIDTH\] assaults=(\d+) bound=(\d+)")
WIDTH_RUNS = []


def note_width(out):
    for line in out.splitlines():
        m = WIDTH.match(line)
        if m:
            WIDTH_RUNS.append({"assaults": int(m.group(1)), "bound": int(m.group(2))}); return


def width_summary():
    if not WIDTH_RUNS:
        return None
    a = sum(r["assaults"] for r in WIDTH_RUNS); b = sum(r["bound"] for r in WIDTH_RUNS)
    return {"runs": len(WIDTH_RUNS), "assaults": a, "bound": b, "bound_share": b / a if a else 0.0}


def print_width():
    w = width_summary()
    if w:
        print(f"  combat width ({w['runs']} runs): {w['bound']} of {w['assaults']} assaults capped ({100.0 * w['bound_share']:.1f}%)")


def note_pop(out):
    for line in out.splitlines():
        m = POP_WORLD.match(line)
        if m:
            POP_RUNS.append({"pop": int(m.group(1)), "army": int(m.group(2))})
            return


def pop_summary():
    if not POP_RUNS:
        return None
    n = len(POP_RUNS)
    return {"runs": n, "pop_mean": sum(r["pop"] for r in POP_RUNS) / n,
            "army_mean": sum(r["army"] for r in POP_RUNS) / n}


def print_pop():
    p = pop_summary()
    if p:
        print(f"  people and armies at turn {TURNS} (world, {p['runs']} runs): "
              f"population {p['pop_mean'] / 1e9:.2f} billion, army {p['army_mean'] / 1e6:.2f} million units")


def note_goods(out):
    ls = []; idle = fac = 0; sold = 0.0; goods = {}
    for line in out.splitlines():
        m = LIVING.match(line)
        if m:
            ls.append(float(m.group(2))); idle += int(m.group(3)); fac += int(m.group(4)); sold += float(m.group(5))
            continue
        m = GOODS.match(line)
        if m:
            g = goods.setdefault(m.group(2), {"produced": 0.0, "consumed": 0.0, "demand": 0.0, "stock": 0.0})
            g["produced"] += float(m.group(3)); g["consumed"] += float(m.group(4))
            g["demand"] += float(m.group(5)); g["stock"] += float(m.group(6))
    if ls or goods:
        GOODS_RUNS.append({"countries": len(ls), "ls_mean": sum(ls) / len(ls) if ls else None,
                           "idle": idle, "factories": fac, "sold": sold, "goods": goods})


def goods_summary():
    if not GOODS_RUNS:
        return None
    lsv = [r["ls_mean"] for r in GOODS_RUNS if r["ls_mean"] is not None]
    fac = sum(r["factories"] for r in GOODS_RUNS); idle = sum(r["idle"] for r in GOODS_RUNS)
    per_good = {}
    for r in GOODS_RUNS:
        for k, g in r["goods"].items():
            a = per_good.setdefault(k, {"produced": 0.0, "consumed": 0.0, "demand": 0.0})
            for f in a: a[f] += g[f]
    n = len(GOODS_RUNS)
    # EACH GOOD OVER THE RUNS THAT HAD IT. Dividing every good by n understates
    # any good that only some runs report -- a good present in half the runs
    # read at half its value. Counted per good instead (journal 340).
    n_good = {}
    for r in GOODS_RUNS:
        for k in r["goods"]:
            n_good[k] = n_good.get(k, 0) + 1
    return {"runs": n, "ls_runs": len(lsv),
            "ls_mean": sum(lsv) / len(lsv) if lsv else None,
            "idle_share": idle / fac if fac else None,
            "good_runs": n_good,
            "goods": {k: {f: v / n_good[k] for f, v in a.items()}
                      for k, a in per_good.items()}}


def print_goods():
    g = goods_summary()
    if not g:
        return
    ls = f"{g['ls_mean']:.2f}" if g["ls_mean"] is not None else "--"
    idle = f"{100.0 * g['idle_share']:.0f}%" if g["idle_share"] is not None else "--"
    _lr = g.get("ls_runs", g["runs"])
    _ln = "" if _lr == g["runs"] else f" (over {_lr} of them)"
    print(f"  goods economy ({g['runs']} runs): living standards {ls}{_ln}, "
          f"factories idle {idle}")
    for k, a in sorted(g["goods"].items()):
        met = 100.0 * a["consumed"] / a["demand"] if a["demand"] else 0.0
        print(f"    {k:<10} produced {a['produced']:8.1f}  consumed {a['consumed']:8.1f}  "
              f"demand {a['demand']:8.1f}  ({met:.0f}% of demand met)")


def note_capacity(out):
    used = total = overcap = n = 0
    for line in out.splitlines():
        m = CAPACITY.match(line)
        if m:
            used += int(m.group(2)); total += int(m.group(3))
            overcap += int(m.group(4)); n += 1
    if n:
        CAP_RUNS.append({"used": used, "total": total, "overcap": overcap, "countries": n})


def cap_summary():
    """Mean world utilisation and grandfathered provinces over the seat-runs
    that printed capacity lines; None on a binary that does not print them."""
    if not CAP_RUNS:
        return None
    util = [r["used"] / r["total"] for r in CAP_RUNS if r["total"] > 0]
    # "runs" is what OVERCAP averages over; utilisation skips runs with no
    # capacity, so it carries its own count. Printing one N beside two means
    # is how a denominator stops being the one that was measured.
    return {"runs": len(CAP_RUNS), "util_runs": len(util),
            "utilisation": sum(util) / len(util) if util else 0.0,
            "overcap": sum(r["overcap"] for r in CAP_RUNS) / len(CAP_RUNS)}


def print_capacity():
    c = cap_summary()
    if c:
        _ur = c.get("util_runs", c["runs"])
        _un = "" if _ur == c["runs"] else f" (over the {_ur} with capacity)"
        print(f"  industry capacity (world, {c['runs']} runs): "
              f"{100.0 * c['utilisation']:.1f}% utilised{_un}, "
              f"{c['overcap']:.1f} grandfathered provinces per run")

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
# OD_BENCH_TURNS lengthens the horizon. 120 is the scoring standard; a longer
# run is for CHECKING that a model does not come apart later, which a short
# horizon cannot see and which has bitten this project before. Scores at a
# different horizon are not comparable to the standard ones.
if os.environ.get("OD_BENCH_TURNS"):
    TURNS = int(os.environ["OD_BENCH_TURNS"])
# The rung that ships as the hardest. OVERRIDABLE (OD_BENCH_DIFFICULTY)
# because every rule measured on 2026-09-05/06 was measured on this rung
# alone, and the rungs are not the same game: difficulty selects which
# faculties are switched on at all (coalitions, the aim head, the action
# budget). A rule that helps on 3 and hurts on 1 is a rule that helps
# nobody who plays the game on normal. A stored label records the rung it
# was taken on, so numbers from two rungs can never be compared by accident.
DIFFICULTY = int(os.environ.get("OD_BENCH_DIFFICULTY", "3"))
# Seeds a seat is played on. A single seed is ONE game, and one game of a grand
# strategy title is mostly weather -- measured across this project, single-seed
# readings pointed the wrong way five times in a day. The rating averages a seat
# over all of them, so a seat is a distribution rather than an anecdote.
SEEDS = [20260801, 4242, 90210]
# OD_BENCH_SEEDS overrides the three fixed seeds with a comma-separated list.
# For CONFIRMING a result only: a config tuned on the three above and then
# reported on them is fitted to them, and the only way to tell the difference
# is to score it on worlds it was not chosen against.
if os.environ.get("OD_BENCH_SEEDS"):
    SEEDS = [int(x) for x in os.environ["OD_BENCH_SEEDS"].split(",") if x.strip()]
# OD_BENCH_VERBOSE=1 restores the full explanation under each [BENCH] warning.
# Default is one line per condition: journal 292 measured a routine six-seat run
# at 32 lines of which 16 were warnings, and the graded-count line -- which
# decides whether two arms are even comparable (journals 290, 295) -- was the
# fourteenth of sixteen. The reasoning lives in the comments beside each block
# and is printed on request; it is not deleted.
VERBOSE = bool(os.environ.get("OD_BENCH_VERBOSE"))

# The most a single seat may score. Five times its starting size is a runaway
# result by any standard, and the cap stops one lucky small seat from being the
# whole rating. See READING THE NUMBER.
CAP = 5.0

# THE SCORE IS A SHARE OF THE WORLD. A rule that helps EVERY country keep its
# provinces (the bankruptcy-unrest ramp, v16) lowers every seat's score even
# when the seat plays identically, because the scripted countries keep their
# land too: N24 read 227 on v13 and 193 on v16 without a line of AI code
# changing. Read a fall after a survivability fix as a moved ruler, not a
# regression -- re-gate every model on the new build and compare within it.
#
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

# OD_BENCH_SEATS restricts the run to a subset, comma-separated, matched as
# "map:iso" or "map:iso:world" -- e.g. OD_BENCH_SEATS=1914:FRA,1939:USA.
#
# This exists because its ABSENCE cost six iterations. Journals 281-286 wanted
# the three "reliable" seats over eight fresh seeds, could not ask for that
# here, and so called the server directly in a shell loop and summed the seat
# scores by hand. That hand-rolled harness reproduced the arithmetic exactly --
# and skipped every warning in report(). modern:CHN:rung straddles its regimes
# on those eight seeds (0.0 to 23.9, par 2.5), so the BISTABLE check below
# would have fired on the FIRST of those runs. It never ran. Five entries then
# read a coin flip as a floor and a variance.
#
# So: if a measurement needs a seat subset, take it from here and keep the
# warnings. The stored result records its own seat_set, and the rating line
# prints "N of M seats -- NOT COMPARABLE", so a filtered run cannot be quietly
# compared against a full one.
#
# AND IT PRINTS WHAT IT MATCHED, because the two-part form EXPANDS. "1914:FRA"
# means every world of that seat -- the ordinary one AND the rushing one -- so
# a run asking for three seats can quietly execute four. Journal 338 did
# exactly that: it pre-registered the three rung seats, got 1914:FRA:rush as
# well, and read a 4-seat rating against a 3-seat hypothesis. The header said
# "4 seats x 8 seeds" and was read past. Spell the matched seats out.
FULL_SEAT_COUNT = len(SEATS)
if os.environ.get("OD_BENCH_SEATS"):
    want = {s.strip() for s in os.environ["OD_BENCH_SEATS"].split(",") if s.strip()}
    SEATS = [s for s in SEATS
             if f"{s[0]}:{s[1]}" in want or f"{s[0]}:{s[1]}:{s[2]}" in want]
    if not SEATS:
        raise SystemExit(f"OD_BENCH_SEATS={sorted(want)} matched no seat")
    _matched = [f"{s[0]}:{s[1]}:{s[2]}" for s in SEATS]
    print(f"[BENCH] OD_BENCH_SEATS matched {len(_matched)} seat(s): "
          f"{', '.join(_matched)}")
    if len(_matched) > len(want):
        print(f"[BENCH] NOTE: {len(want)} pattern(s) expanded to {len(_matched)} "
              f"seats -- a two-part pattern takes EVERY world of that seat")

# The exploit variant --vs-exploit takes for a rushing world. 3 is SCRIPT_BLITZ.
RUSH_VARIANT = 3


def binary_path(explicit):
    """The freshest built binary, headless where one exists.

    Shared with tools/ai_bench.py rather than reimplemented, because the two
    have to agree about which tree is current: a rating measured against
    yesterday's rules and a comparison measured against today's look identical
    on the page. find_binary() prefers OpenDoctrinesServer -- the same
    Game::runAIEvaluation with no renderer linked -- and warns if it is stale.

    This used to hardcode the windowed .app, and had to, because --bench-seat,
    --vs-exploit and --rush-neighbours were parsed only by main.cpp. They are
    parsed by src/server/ServerMain.cpp now, so a rating no longer opens
    eighteen OpenGL windows in front of whoever is using the machine, no longer
    dies when the display sleeps mid-run, and costs ~0.6 GB of peak RSS per
    seat instead of ~2 GB. Verified equal, not assumed: all six seats on seed
    20260801 score identically under both binaries (6.6 / 0.2 / 8.5 / 0.3 /
    1.8 / 0.7), which is the only thing that lets old ratings stay comparable.
    """
    if explicit:
        return explicit
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from ai_bench import find_binary
    return find_binary()


AI_VERSION_RE = re.compile(r"\[EVAL\] ai\s+(\S+ [0-9.]+)")


def note_ai_version(out):
    """The AI's own version, from the binary that just ran.

    Recorded per label because "which model" and "which rules" are different
    questions: a seat number is only comparable with another taken by the
    same ParrotZero version. See src/ai/AIVersion.h.
    """
    m = AI_VERSION_RE.search(out)
    if m:
        AI_VERSION.add(m.group(1))


_pending_per_seed = []
AI_VERSION = set()
DROPPED = []
# Per-seat raw seed values, so report() can tell a stable seat from a bistable
# one. See BISTABLE below.
SPREAD = {}


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
    note_ai_version(out)
    note_capacity(out)
    note_goods(out)
    note_pop(out)
    note_width(out)
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


# Seats measured to have no middle: a run either holds the country or is
# annihilated. Established for 1914:FRA:rush over 22 runs, 2026-09-10.
# A mean over fewer than ~10 seeds here is a coin-flip estimate.
KNOWN_BISTABLE = {"1914:FRA:rush", "modern:CHN:rung"}
# modern:CHN:rung added 2026-09-12 (journal 287). Its par is 2.5, so the 5x cap
# sits at a 12.5% share -- and China either holds well above that or is wiped
# out. Across 24 observations on one model (journals 282 and 285, eight fresh
# seeds, three arms) the seat scored EXACTLY 500 or EXACTLY 0 on 22, with two
# intermediate readings in one arm. It is a survival bit worth a third of the
# rating, not a graded seat, and it was on the "reliable seats" list for six
# iterations. Dropping it from those runs moves the shipped change from
# +89 (CI spanning zero, 6/8 seeds) to +122 (CI [+65,+179], 8/8).
BISTABLE_SEATS = []


def report(label, scores):
    """scores: {"map:iso": world share}. Returns the rating."""
    print(f"\n  {'seat':<18} {'held':>6} {'par':>6} {'score':>7}   ")
    print(f"  {'-'*18} {'-'*6} {'-'*6} {'-'*7}")
    vals = []
    pinned_seats = []
    for mapname, iso, world, par, _why in SEATS:
        key = f"{mapname}:{iso}:{world}"
        v = scores.get(key)
        label_ = f"{mapname}:{iso} {world}"
        if v is None:
            print(f"  {label_:<18} {'--':>6} {par:>6.1f} {'--':>7}")
            continue
        sc = seat_score(v, par)
        vals.append(sc)
        # BISTABLE seats. `1914:FRA:rush` does not have a middle: across 22
        # runs in one day (two models, 11 paired seeds) every result was
        # either 5.7-11.3 land or below 1.5, with NOTHING between. Collapse
        # rate was 5/11 and 3/11. A 3-seed mean of that is a Bernoulli
        # estimate from three coin flips, and it LOOKS like a measurement
        # because it prints to two decimals.
        #
        # It cost a full day: a "5.6x capability gap" between two models was
        # one of them drawing three collapse worlds (N24 read 1.17 on those
        # three seeds and 4.24 over eleven), and a knob finding that
        # "failed to replicate" across two seed sets was three flips, twice.
        #
        # This is keyed off the SEAT, not off the sample, because three seeds
        # usually CANNOT show it -- set C was [3.1, 0.2, 0.2], which never
        # reaches the holding band at all. Detecting it from the spread only
        # works when the sample happens to straddle, which is the same
        # small-n problem the warning exists to flag.
        raw = SPREAD.get(key) or []
        straddles = (len(raw) > 1 and min(raw) < 0.25 * par and max(raw) > 0.75 * par)
        bistable = (key in KNOWN_BISTABLE and len(raw) < 10) or straddles
        if v / par > CAP:
            pinned_seats.append(label_)
        note = "  wiped out" if v <= 0.05 else ("  capped" if v / par > CAP else "")
        if bistable:
            # Show every value while that is readable, and a summary past it.
            # The power line below tells you to use 64-128 seeds on a seat like
            # this; printing 128 floats on one line makes the advice unusable.
            if not raw:
                spread_s = "?"
            elif len(raw) <= 12:
                spread_s = "/".join(f"{g:.1f}" for g in raw)
            else:
                hi = [g for g in raw if g >= 0.75 * par]
                lo = [g for g in raw if g < 0.25 * par]
                mid = len(raw) - len(hi) - len(lo)
                spread_s = (f"{len(raw)} seeds: {len(hi)} holding "
                            f"(median {statistics.median(hi):.1f}), {len(lo)} collapsed"
                            + (f", {mid} between" if mid else ", none between"))
            # n matters for the ADVICE, not for whether it is bimodal. The mean
            # of a two-regime seat describes no run that happened at any n; but
            # with enough seeds the collapse RATE is a real quantity, and below
            # that it is a coin-flip estimate.
            if len(raw) >= 10:
                coll = sum(1 for g in raw if g < 0.25 * par)
                note += (f"  [BIMODAL {spread_s} -- read collapse rate "
                         f"{coll}/{len(raw)}, not the mean]")
            else:
                note += f"  [BISTABLE {spread_s} -- only {len(raw)} seeds, mean is not a measurement]"
            BISTABLE_SEATS.append((label_, len(raw)))
        print(f"  {label_:<18} {v:>6.1f} {par:>6.1f} {sc:>7.0f}{note}")
    # PINNED seats carry no information about the arm. A seat whose share
    # exceeds CAP x par scores exactly CAP*100 however well it actually did,
    # so if BOTH arms of an A/B pin it, it contributes an identical constant
    # to both and the rating is blind to any change there. Measured
    # 2026-09-10: with a strong model, 2 of 6 seats pinned in both arms on
    # hold-out C (SWE par 1.0 ran [24.2, 9.0, 6.3] -- a 4x spread, entirely
    # invisible) and 1 of 6 on D. That is a third of the rating that cannot
    # move. It also DAMPS the rating's variance, so the standard error above
    # understates how noisy the discriminating part is.
    if pinned_seats:
        print(f"\n  [BENCH] pinned at CAP: {', '.join(pinned_seats)} -- scores the same "
              f"however well it did")
        if VERBOSE:
            print("  [BENCH] a pinned seat scores the same however well it did -- the rating "
                  "cannot see\n  [BENCH] improvement there, and if the other arm pins it too "
                  "it is a shared constant.")
    # HOW MUCH OF THE INSTRUMENT CAN MOVE. A seat-seed observation that is
    # wiped out (0) or at/over the cap (CAP*100) is a constant: it scores the
    # same however the arm played. Counting them is the only way to tell that
    # two arms were not measured by the same instrument.
    #
    # Journal 288 measured this across the 281-286 arc. On N24, 14 of 24
    # seat-seeds were graded; on N35, 9 to 11 -- and the seats differed, with
    # CHN a dead coin on N24 and USA pinned at CAP on 8 of 8 seeds on N35.
    # Those two models were compared to each other for three iterations and
    # read as DISAGREEING about two rules. They were saturated in different
    # places, which makes the comparison meaningless rather than negative:
    # N35's arms carried se ~50 and confidence intervals of +/-100.
    #
    # So: before comparing two models, compare THIS LINE for both. A model
    # that pins different seats is being scored by a different instrument.
    graded = total = 0
    for mapname, iso, world, par, _why in SEATS:
        for g in (SPREAD.get(f"{mapname}:{iso}:{world}") or []):
            total += 1
            if g > 0.05 and g / par < CAP:
                graded += 1
    if total and graded < total:
        print(f"  [BENCH] {graded}/{total} observations GRADED ({total - graded} pinned at "
              f"0 or CAP) -- compare this figure across arms before trusting a difference")
        if VERBOSE:
            print("  [BENCH] a saturated observation scores the same however the arm played. Two "
                  "models\n  [BENCH] that saturate DIFFERENT seats are not comparable -- check this "
                  "line on both\n  [BENCH] before reading a cross-model agreement or disagreement.")

    if BISTABLE_SEATS:
        # One figure per distinct seed count -- the seats almost always share n,
        # and printing it once per seat reads as two different results.
        ns = sorted({n for _, n in BISTABLE_SEATS if n >= 2})
        pw = "; ".join(f"{n} seeds resolve a rate difference of ~"
                       f"{1.96 * math.sqrt(2 * 0.25 / n):.2f}" for n in ns)
        print(f"\n  [BENCH] bistable: {', '.join(l for l, _ in BISTABLE_SEATS)} -- no meaningful "
              f"mean, read the collapse RATE;\n  [BENCH] UNPAIRED even on matched seeds"
              + (f"; {pw}" if pw else ""))
        if not VERBOSE:
            print("  [BENCH] OD_BENCH_VERBOSE=1 explains each of the lines above.")
        if VERBOSE:
            print("  [BENCH] a two-regime seat has no meaningful mean. Read its collapse RATE.")
        # PAIRING DOES NOT HELP HERE, and this used to say the opposite.
        #
        # The old text read "paired within-seed arms are the reliable comparison
        # either way -- both sides draw the same worlds". Journal 291 measured
        # it on 1914:FRA:rush: both arms drew the same 8 worlds and disagreed
        # about SEVEN of them (1 agreement where independent coins predict 4.1,
        # P(<=1) = 0.035). Fixing the seed fixes the map and the starting
        # position; it does not fix the outcome, because the intervention
        # re-rolls the trajectory and the seat's result is a knife-edge.
        #
        # So a rate difference on such a seat is an UNPAIRED two-proportion
        # problem however the seeds are arranged, and that is what the power
        # line below computes. It exists because the old advice cost 32 runs on
        # a question needing ~256: the harness believed pairing made 8 seeds
        # enough, so nobody did this arithmetic first.
        if VERBOSE:
            print("  [BENCH] PAIRING DOES NOT HELP: journal 291 toggled one reflex over the same "
                  "8\n  [BENCH] worlds and 7 of 8 outcomes flipped. The seed fixes the map, not "
                  "the\n  [BENCH] outcome -- so treat this as an UNPAIRED rate difference.")
            for label_, n in BISTABLE_SEATS:
                if n >= 2:
                    # Worst-case (p=0.5) se of a difference of two proportions.
                    det = 1.96 * math.sqrt(2 * 0.25 / n)
                    verdict = ("can only see a near-total swing" if det >= 0.40 else
                               "coarse" if det >= 0.20 else "usable")
                    print(f"  [BENCH] {label_}: {n} seeds resolve a rate difference of "
                          f"~{det:.2f} at 95% -- {verdict}.")
            print("  [BENCH] n per arm:   8     16    32    64   128   256")
            print("  [BENCH] resolves: 0.49  0.35  0.24  0.17  0.12  0.09   -- pick n BEFORE "
                  "the runs.")
        BISTABLE_SEATS.clear()
    if not vals:
        return None
    rating = statistics.mean(vals)
    # THE RATING'S OWN ERROR BAR, from the per-seed spreads this run recorded.
    # Measured 2026-09-10 across three independent full runs: the per-seed
    # rating has sd 15-21, so a 3-seed mean carries se 9-12. An UNPAIRED
    # difference below about 28 points (2 x sqrt(2) x se) is not distinguishable
    # from noise, and a whole day was spent on a "+24" that was inside it.
    # Paired arms -- same seeds both sides -- cancel most of this and are the
    # reason a replicated paired result can be trusted at a smaller margin.
    se_note = ""
    per_seed_line = ""
    try:
        per_seed = (per_seed_ratings(SPREAD)
                    if len(SPREAD) >= len(SEATS) else [])
        if len(per_seed) >= 2:
            ps = statistics.mean(per_seed)
            se = statistics.stdev(per_seed) / math.sqrt(len(per_seed))
            # The error bar goes on the number it was computed from, and the
            # headline gets a pointer instead of a figure it does not own.
            se_note = "   [see the per-seed line below for the error bar]"
            per_seed_line = (
                f"  {label}: PER-SEED rating {ps:.0f}  +/- {se:.0f} se   "
                f"(unpaired diffs under ~{2 * math.sqrt(2) * se:.0f} are noise)\n"
                f"        this is the mean of the {len(per_seed)} per-seed ratings, NOT the "
                f"rating of the mean shares above;\n"
                f"        they differ when a seat is capped or bistable "
                f"(journal 351: +45 on the 3-seat control).\n"
                f"        COMPARE THIS ONE with an se or with another arm.")
    except (ValueError, KeyError, IndexError, ZeroDivisionError, statistics.StatisticsError):
        se_note = ""
        per_seed_line = ""
    ver = sorted(AI_VERSION)
    # SAY HOW MANY SEATS THE MEAN IS OVER. A seat that produced no [BENCH]
    # line prints "--" in the table above and is skipped here, so the rating
    # is a mean over the SURVIVORS. On set C, dropping the rusher seat alone
    # moves the 0.15 arm from 311 to 370 -- larger than the entire 0.15->0.05
    # effect of +25, and pointing the other way. It also drops preferentially:
    # the seat likeliest to die is the one being overrun. The seed-loss guard
    # upstream already refuses to call such a run comparable, but the headline
    # number gets read out of logs and out of the store without it, so it must
    # carry its own denominator.
    seat_note = "" if len(vals) == len(SEATS) else \
        f"   [!! {len(vals)} of {len(SEATS)} SEATS -- NOT COMPARABLE]"
    if per_seed_line:
        _pending_per_seed.append(per_seed_line)
    print(f"\n  {label}: OD BENCH {rating:.0f} over {len(vals)}/{len(SEATS)} seats{seat_note}{se_note}   "
          f"(100 = held every seat; 0 = annihilated everywhere)"
          + (f"   [{ver[0]}]" if len(ver) == 1 else
             f"   [MIXED VERSIONS {ver} -- the binary changed mid-run]" if ver else ""))
    # ── AND THE SAME SEATS AS PLAIN LAND ──
    #
    # The sum of the raw shares, which is the one column measured 2026-09-07
    # not to be broken. The rating is a mean of held/par, so it weights a seat
    # by how SMALL it is: Norway's par is 1.3% of the world and France's 6.7,
    # so the same tenth of a percent of land moves Norway five times further.
    # The worst-seat column is the same effect at its extreme -- it reports
    # whichever seat is nearest zero, magnified. Norway carries the floor in
    # 531 of 688 stored runs at a median 0.37% of the world, where a movement
    # of 0.2% of the map reads as 15 points of score.
    #
    # Measured consequences, same day: a "-20 worst seat" that was 0.27% of the
    # map; a rule rejected on a floor collapse that was 0.2%; an ablation table
    # whose two modes disagreed on six of eight features. Land settled all
    # three. It is NOT a replacement rating -- it weights a seat by how large
    # it is, which is its own bias -- but it is not a magnifier, and for "did
    # this change help the AI hold ground" it is the closer question.
    #
    # Printed, not stored: the stored seats already carry it exactly.
    # OVER THE SAME SEATS AS THE RATING ABOVE, which it did not used to be.
    # This summed the whole stored dict while the rating iterates SEATS, so a
    # filtered --compare printed "OD BENCH 381 over 3/3 seats" and "land 56.62%
    # across 4 seats" on adjacent lines, the second including a seat the filter
    # had excluded (journal 340, seen in journal 338's own output).
    _lv = [scores.get(f"{m}:{i}:{w}") for m, i, w, _, _ in SEATS]
    while _pending_per_seed:
        print(_pending_per_seed.pop(0))
    _land = sum(v for v in _lv if v is not None)
    _seats_n = sum(1 for v in _lv if v is not None)
    print(f"  {label}: land {_land:.2f}% of the world across {_seats_n} seats"
          f"   (raw shares, unweighted -- see the note in report())")

    # ── AND THE SAME SEATS WITH RUNAWAY GROWTH TAKEN OUT ──
    #
    # NOT a second rating and deliberately not stored: the seat set and the
    # rating must not move, or scores stop being comparable, which is the
    # disease this file exists to cure. This is a READING of the same numbers.
    #
    # The rating is a mean of ratios capped at 500, so a seat that runs away can
    # BUY the average -- and one that dies can only spend 100. Measured
    # 2026-09-04 on two checkpoints of one training run: the later one rated 177
    # against the earlier one's 131 while its worst seat fell from 26 to 7,
    # because three seats had gone to 2.6-3.3x par and drowned a Sweden that was
    # annihilated. Capping each seat at its own par answers the other question --
    # did it HOLD what it was given -- and on that reading the earlier model is
    # the better one, 87 to 74.
    #
    # Both numbers are wanted. A model that only ever holds is not winning; a
    # model that wins four seats and loses two has not necessarily improved.
    # Print them together and the trade is visible instead of hidden in a mean.
    survival = statistics.mean(min(v, 100.0) for v in vals)
    worst = min(vals)
    # HOW MANY SEATS SURVIVAL IS ACTUALLY MADE OF. Every seat at or above par
    # caps at 100 and contributes an identical constant, so survival varies
    # ONLY over the seats below par. On 2026-09-10 a research-bar sweep moved
    # survival 77 -> 89 with FOUR of six seats sitting at exactly 100 in both
    # arms: the whole difference was 1914:FRA:rush (bistable) and 1939:NOR:hood
    # (par 1.3, magnifier territory). Read as a six-seat statistic it looked
    # like the AI becoming harder to kill. It was two unmeasurable seats.
    live = [v for v in vals if v < 100.0]
    surv_note = ""
    if len(live) <= 2:
        surv_note = (f"   [!! survival varies over only {len(live)} of {len(vals)} seats "
                     f"-- the rest are at or above par and constant]")
    # WHICH SEAT IS THE WORST, because it changes identity. Fixing the worst
    # seat promotes the next one, and "worst seat 17 -> 59" then compares two
    # DIFFERENT seats while reading as a floor lift.
    worst_seat = ""
    for mapname, iso, world, par, _why in SEATS:
        key = f"{mapname}:{iso}:{world}"
        v = scores.get(key)
        if v is not None and abs(seat_score(v, par) - worst) < 1e-6:
            worst_seat = f" ({mapname}:{iso} {world})"
            break
    print(f"  {' ' * len(label)}  survival {survival:.0f}   "
          f"worst seat {worst:.0f}{worst_seat}   "
          f"(survival = mean of min(seat,100): growth above par earns nothing)"
          f"{surv_note}")
    return rating


# ── THE COMPARISON THE RATING CANNOT MAKE ──
#
# The rating is a mean of ratios capped at 5x par, so a seat whose par is small
# scores the SAME at 13% and at 23% of the world. Journal 338 benched a change
# whose entire effect landed on such a seat -- modern:CHN, median share
# 14.3 -> 26.0, the only seat near significance -- and the rating moved 42
# against a ~60 floor, which read as a null. The land share is the quantity the
# change actually moved, and nothing printed it.
#
# So: raw per-seat share, both medians, the difference, and a two-sided
# UNPAIRED permutation test (unpaired because journal 296 measured seed-pairing
# as worth zero here). The shuffle count and seed are fixed so two people
# quoting this line quote the same number.
def per_seed_ratings(spread):
    """The rating computed PER SEED, then averaged -- which is NOT what the
    headline is. The headline applies seat_score to each seat's MEAN share;
    this applies it per seed and averages after. min(x, CAP) is concave, so by
    Jensen the headline is >= this, and journal 351 measured the gap at +45
    points on the standard 3-seat control (381 against 336) because modern:CHN
    scores 500 on six seeds and 0 on two while its mean share is above the cap.

    Everything with an error bar must use THIS one: the se has always been
    computed from per-seed ratings, so for twelve entries it was printed beside
    a number it does not describe."""
    if not spread:
        return []
    n = min((len(v) for v in spread.values() if v), default=0)
    if n < 2:
        return []
    out = []
    for s_ in range(n):
        sv = [min(seat_score(spread[f"{m}:{i}:{w}"][s_], par), CAP * 100.0)
              for m, i, w, par, _ in SEATS if spread.get(f"{m}:{i}:{w}")]
        if sv:
            out.append(statistics.mean(sv))
    return out


PERM_N = 20000
PERM_SEED = 20260912


def _perm_p(a, b):
    """Two-sided unpaired permutation p for mean(b) - mean(a)."""
    import random
    rng = random.Random(PERM_SEED)
    obs = abs(statistics.mean(b) - statistics.mean(a))
    pool = list(a) + list(b)
    n = len(a)
    hits = 0
    for _ in range(PERM_N):
        rng.shuffle(pool)
        if abs(statistics.mean(pool[n:]) - statistics.mean(pool[:n])) >= obs - 1e-12:
            hits += 1
    return (hits + 1) / (PERM_N + 1)


def compare_ratings(a, b, store):
    """The per-seed rating difference against the floor built from BOTH arms.

    Journal 336 compared the HEADLINE difference with a floor computed from
    per-seed ratings and called a -88 a clear; journal 351 redid it correctly
    and it misses by two points. Nobody should have to do this by hand again.
    """
    pa = per_seed_ratings((store[a] or {}).get("spread") or {})
    pb = per_seed_ratings((store[b] or {}).get("spread") or {})
    if len(pa) < 2 or len(pb) < 2:
        return
    ma, mb = statistics.mean(pa), statistics.mean(pb)
    sa = statistics.stdev(pa) / math.sqrt(len(pa))
    sb = statistics.stdev(pb) / math.sqrt(len(pb))
    floor = 1.96 * math.sqrt(sa * sa + sb * sb)
    d = mb - ma
    print(f"\n  PER-SEED rating: {a} {ma:.0f} (se {sa:.0f})   {b} {mb:.0f} (se {sb:.0f})")
    print(f"  difference {d:+.0f}   floor from these two arms {floor:.0f}   "
          f"{'CLEARS' if abs(d) > floor else 'DOES NOT CLEAR'}")
    print(f"  (the headline difference is computed from the MEAN shares and is "
          f"a different quantity -- journal 351)")


def compare_shares(a, b, store):
    sa = (store[a] or {}).get("spread") or {}
    sb = (store[b] or {}).get("spread") or {}
    # A MISSING SPREAD IS SAID OUT LOUD. Every result stored before this
    # existed has none, and an empty table reads as "no difference".
    missing = [n for n, sp in ((a, sa), (b, sb)) if not sp]
    if missing:
        print(f"\n  [BENCH] no per-seed data stored for {', '.join(missing)} -- "
              f"land-share statistics need a run from after journal 339; "
              f"the seat scores above are all these rows carry")
        return
    shared = [k for k in sa if k in sb and len(sa[k]) > 1 and len(sb[k]) > 1]
    if not shared:
        print("\n  [BENCH] the two runs share no seat with per-seed data")
        return
    print(f"\n  per-seat LAND SHARE -- the raw quantity, before the cap and the ratio")
    wa, wb = max(9, len(a[-14:])), max(9, len(b[-14:]))
    print(f"  {'seat':<18}{a[-14:]:>{wa + 1}}{b[-14:]:>{wb + 1}}{'d':>8}"
          f"{'med A':>8}{'med B':>8}{'p':>8}")
    capped = []
    shown = 0
    for m, i, w, par, _why in SEATS:
        k = f"{m}:{i}:{w}"
        if k not in shared:
            continue
        shown += 1
        va, vb = sa[k], sb[k]
        ma, mb = statistics.mean(va), statistics.mean(vb)
        print(f"  {m + ':' + i + ' ' + w:<18}{ma:{wa + 1}.2f}{mb:{wb + 1}.2f}{mb - ma:+8.2f}"
              f"{statistics.median(va):8.2f}{statistics.median(vb):8.2f}"
              f"{_perm_p(va, vb):8.3f}")
        if (seat_score(ma, par) >= CAP * 100.0 - 1e-9 and
                seat_score(mb, par) >= CAP * 100.0 - 1e-9):
            capped.append(k)
    print(f"  [BENCH] p is two-sided, unpaired, {PERM_N} shuffles, seed "
          f"{PERM_SEED}; {shown} seat(s) compared, so expect "
          f"{0.05 * shown:.1f} under 0.05 by chance -- LOOP.md STANDING 5")
    if capped:
        print(f"  [BENCH] PINNED AT CAP IN BOTH ARMS: {', '.join(capped)} -- "
              f"this seat's land moves HERE and not in the rating, which scores "
              f"it identically at any share above {CAP:.0f}x par (journal 338)")


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
        save_label(args.label, store[args.label])
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
        compare_ratings(a, b, store)
        compare_shares(a, b, store)
        if ca is not None and cb is not None:
            d = cb - ca
            print(f"\n  {b} is {abs(d):.0f} {'above' if d > 0 else 'below'} {a}")
            # THE DENOMINATOR IS WHAT BOTH RUNS MEASURED, not the full seat
            # set. A 4-seat run against the 6-seat default printed "better on
            # 3/6", counting two seats that were never run as losses.
            both = [f"{m}:{i}:{w}" for m, i, w, _, _ in SEATS
                    if f"{m}:{i}:{w}" in store[a]["seats"]
                    and f"{m}:{i}:{w}" in store[b]["seats"]]
            better = sum(1 for k in both
                         if store[b]["seats"][k] > store[a]["seats"][k])
            print(f"  better on {better}/{len(both)} seats measured by both")
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
        # A DEAD CHILD USED TO VANISH HERE. run_seat returns None when the
        # subprocess is killed or prints no [BENCH] line, and this filter then
        # averaged whatever survived -- so a seat could be a mean of two seeds
        # while the stored row still claimed three, and nothing said so. A
        # killed bench looks exactly like a real effect. Count them instead.
        missing = sum(1 for g in got if g is None)
        if missing:
            DROPPED.append(f"{mapname}:{iso}:{world} lost {missing} of {len(seeds)} seed(s)")
        got = [g for g in got if g is not None]
        if not got:
            continue
        mean = statistics.mean(got)
        scores[f"{mapname}:{iso}:{world}"] = mean
        SPREAD[f"{mapname}:{iso}:{world}"] = list(got)
        spread = "  ".join(f"{g:.1f}" for g in got)
        print(f"  {mapname}:{iso} {world:<5} {mean:5.1f}  (par {par:.1f})   [{spread}]")

    if DROPPED:
        print("[BENCH] WARNING: seeds were lost -- this run is NOT comparable:")
        for d in DROPPED:
            print(f"[BENCH]   {d}")
    rating = report(label, scores)
    print_capacity()
    print_goods()
    print_pop()
    print_width()
    if rating is None:
        sys.exit("[BENCH] nothing measured")

    # ── PROVENANCE ──
    #
    # Four things decide what a stored number means and none of them were
    # recorded: which model file, which binary, which worlds, how long. Two
    # sessions spent a day re-deriving those from timestamps and memory, and
    # a run whose model file was REFUSED by the loader would have been stored
    # here as an ordinary result of a fresh untrained net (fixed in the
    # loader, but the archive cannot be audited backwards for it).
    #
    # "note" is for the one thing provenance still cannot answer: what the run
    # was FOR. i18-B sits in this file at 3% of the world and nothing records
    # whether that was a deliberately bad arm or a surprise.
    _mp = os.path.abspath(args.model) if args.model else None
    # ── THE PER-SEED VALUES, WHICH USED TO DIE AT EXIT ──
    #
    # SPREAD was built for the BISTABLE warnings and thrown away, so the store
    # kept one mean per seat and 863 archived results carried no statistics at
    # all. Journal 338 had to recompute a comparison in a scratch file for want
    # of this, one iteration after journal 287 built the seat filter to stop
    # exactly that. A mean is not a measurement; the seeds behind it are.
    store[label] = {"seats": scores, "spread": dict(SPREAD), "turns": TURNS,
                    "difficulty": DIFFICULTY, "seeds": SEEDS,
                    "model_path": _mp,
                    "model_size": (os.path.getsize(_mp) if _mp and os.path.exists(_mp) else None),
                    "binary_mtime": (os.path.getmtime(binary) if os.path.exists(binary) else None),
                    "note": os.environ.get("OD_BENCH_NOTE"),
                    "seat_set": [f"{m}:{i}:{w}" for m, i, w, _, _ in SEATS],
                    "capacity": cap_summary(),
                    "goods": goods_summary(),
                    "people": pop_summary(),
                    "width": width_summary(),
                    # Which ParrotZero produced these seats. A number is only
                    # comparable with another taken by the same version; see
                    # src/ai/AIVersion.h.
                    "ai_version": sorted(AI_VERSION)[0] if len(AI_VERSION) == 1 else sorted(AI_VERSION)}
    save_label(label, store[label])
    print(f"  stored as {label!r} in {os.path.relpath(RESULTS, ROOT)}")

    # Left for the training HUD; see SCORE_FILE.
    #
    # ── AND ONLY WHEN IT IS A RATING ──
    #
    # This file is the answer to "what does the AI score", so a number that is
    # not a rating must not land in it. Journal 339's writer test -- ONE seat,
    # two seeds, thirty turns -- published itself here over a 3-seat 8-seed
    # figure, and nothing in the line said 30 turns, because the line carried
    # no turns field. A --quick run did the same, to the file whose own QUICK
    # comment says it "must not be stored as one".
    #
    # So: a filtered seat set or --quick does not publish, and says so; and the
    # line now carries turns and difficulty, which are the two fields that
    # decide whether a rating means anything. Consequence, deliberately: during
    # loop work -- which nearly always filters -- this file goes STALE rather
    # than fresh-and-wrong, and its epoch field is what says how stale.
    _why_not = ("--quick is an estimate, not a rating" if args.quick else
                f"filtered to {len(SEATS)} of {FULL_SEAT_COUNT} seats"
                if len(SEATS) != FULL_SEAT_COUNT else None)
    if _why_not:
        print(f"  not published to {os.path.relpath(SCORE_FILE, ROOT)}: {_why_not} "
              f"(the file keeps the last full rating)")
    else:
        try:
            os.makedirs(os.path.dirname(SCORE_FILE), exist_ok=True)
            with open(SCORE_FILE, "w") as f:
                # Trailing fields so an older reader still parses the first four.
                f.write(f"{rating:.0f} {len(seats)} {len(seeds)} "
                        f"{int(time.time())} {len(SEATS)} {TURNS} {DIFFICULTY}\n")
        except OSError:
            pass


if __name__ == "__main__":
    main()
