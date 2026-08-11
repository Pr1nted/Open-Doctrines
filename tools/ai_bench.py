#!/usr/bin/env python3
"""Measure a model, with error bars.

WHY THIS EXISTS. tools/ai_benchmark.sh reports a mean ADVANTAGE over a few
repeats of ONE seed at ONE horizon. Three separate conclusions were drawn from
it in a single day that the data did not support:

  * a change was called "no effect" because 300 turns was measured, when the
    same models differed fourfold at 400 -- the model's failure was that it
    collapsed LATE, and the benchmark stopped before the collapse;
  * a run was called "above parity for the first time" on a single seed
    reading 1.20x, when the four-seed mean was 0.75x;
  * a hypothesis was called dead on a counter that could not have been
    non-zero, because nothing re-measured it.

Seed spread on ADVANTAGE is roughly 0.67-1.24. That is wider than most effects
worth chasing, so any single number without an interval around it is noise with
a decimal point. This tool refuses to print one.

WHAT IT DOES DIFFERENTLY.

  Paired.      A comparison runs both models over the SAME seeds and maps and
               reports the per-seed DIFFERENCE. Map difficulty varies far more
               than models do, and pairing cancels it: the paired interval is
               typically several times tighter than either arm's own spread.
  Intervals.   Bootstrap percentile CI, not a t-interval. Four to eight seeds is
               too few to trust normality, and ADVANTAGE is a ratio -- bounded
               below by zero and skewed.
  Both horizons. Always. A model that wins early and collapses late is a
               different animal from one that grinds, and one number cannot
               tell them apart.
  Everything.  Every [EVAL] counter is collected, not just ADVANTAGE, so a
               regression in solvency or research shows up next to the headline
               rather than being discovered a week later.

WHAT THE CONTROL IS. By default the other half of every map picks uniformly at
random, which answers one question -- "better than a coin flip" -- and stops
being informative the moment the answer is yes, because random never improves.
--vs-model hands that half a named model file instead. The split, the counters
and the report are identical; only the opponent changes. That turns a target
like "as good as an intermediate player" into something a run can pass or fail:
pin the file, measure against it, and pin a better one when it is beaten.

USAGE
    tools/ai_bench.py                          # measure data/ai/model.bin
    tools/ai_bench.py --seeds 8                # more seeds, tighter interval
    tools/ai_bench.py --compare old.bin        # paired A/B against a checkpoint
    tools/ai_bench.py --vs-model rung1.bin     # play a named opponent, not dice
    tools/ai_bench.py --scenarios --maps 6     # the worlds a player opens
    tools/ai_bench.py --turns 300 400 600      # custom horizons
    tools/ai_bench.py --json out.json          # machine-readable

WHICH WORLDS. By default, procedurally generated archetypes -- and for most of
this project's life that was the ONLY thing trained or measured on, while every
player opens one of the six maps in data/STDmaps. Those have historical alliance
networks, real claims, five great powers among forty small states and, in one
case, 185 countries; none of it resembles a generated map, and all of it is
something buildFeatures reads. --scenarios measures there instead. Runs with and
without it are not comparable and are not meant to be: they are two questions.
A per-world ADVANTAGE table is reported whenever a run covers more than one, so
a model that is fine on pangaea and hopeless on 1939 cannot hide in the mean.

Exit status is 1 if a comparison shows a SIGNIFICANT REGRESSION in ADVANTAGE
(paired 95% CI entirely below zero), so this can gate a training run.
"""

import argparse
import json
import os
import random
import re
import shutil
import statistics
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data", "ai")
SHARED = os.path.join(DATA, "model.bin")

# Seeds are FIXED, not drawn from the clock: map N must be the same world in
# every run or nothing is comparable across invocations. Drawn once from a
# seeded generator so the list is arbitrary rather than hand-picked -- a
# hand-picked seed set is a way to accidentally select for a flattering map.
_rng = random.Random(20260803)
SEED_POOL = [4242, 777, 20260801, 31337] + [_rng.randrange(1000, 10**9) for _ in range(28)]

# Pinned so a stored result carries its own configuration rather than depending
# on whoever's shell history produced it.
#
# This flag was briefly BLAMED for the run-to-run spread that motivated this
# tool -- the same model measured 0.75 and 0.32 on consecutive runs that also
# differed here. It was innocent. The real cause was that turn logic calls
# rand() while raylib seeds it from the wall clock (fixed in Game_AITrain.cpp),
# and once that was seeded per map, --resource-limit 20 and 90 returned exactly
# the same ADVANTAGE. Pinning it is still right -- an experiment should not
# leave its knobs to chance -- but it is hygiene, not a correction.
RESOURCE_LIMIT = "90"

# NOTE ON SAMPLE SIZE. Three repeats is not enough. On 2026-08-03 an
# intermittent divergence was found firing about one run in five: five runs read
# 0.65, 0.71, 0.65, 0.65, 0.65. Every three-run check that day passed, which is
# exactly what a 1-in-5 event does to a 3-run test roughly half the time. The
# default is six, and even that only makes a miss unlikely rather than
# impossible -- raise it before trusting a small effect.
#
# How far apart two identical runs are allowed to be before the tool says so.
# Since the rand() seeding fix this should be exactly ZERO: three consecutive
# 400-turn runs returned 0.59, 0.59, 0.59. Any non-zero reading now means
# nondeterminism has been REINTRODUCED, which is worth failing loudly over --
# it silently inflated every interval for as long as it existed.
NOISE_WARN_FRACTION = 0.10


def find_binary():
    """The NEWEST build tree, not the first one in an arbitrary list.

    This measured an entropy change against a binary an hour out of date,
    because the old version returned `build/` whenever it existed and this
    project routinely keeps several trees -- build/ for the training pool,
    build-gates/ for whatever is being tested, build-release/ for packaging.
    The stale tree predated a determinism fix, so the run reported a 20% noise
    floor that had already been eliminated.

    Freshest wins, and the choice is PRINTED with its timestamp: a benchmark
    that silently measures yesterday's code is worse than one that refuses to
    run, because its numbers look exactly like real ones.
    """
    cands = []
    for d in ("build", "build-gates", "build-release", "build-classic"):
        for tail in (("OpenDoctrines.app", "Contents", "MacOS", "OpenDoctrines"),
                     ("OpenDoctrines",)):
            p = os.path.join(ROOT, d, *tail)
            if os.path.exists(p):
                cands.append((os.path.getmtime(p), p))
                break
    if not cands:
        sys.exit("no built binary found. Run: cmake --build build -j")
    cands.sort()
    return cands[-1][1]


# ── Parsing ────────────────────────────────────────────────────────────────
# Two shapes matter in the [EVAL] block: the headline ADVANTAGE line, and the
# two-column "metric  model  random" rows. Histogram rows (which carry a '/' or
# a '%)') are skipped -- they are per-action detail, not a per-run scalar.
ADV_RE = re.compile(r"ADVANTAGE\s+([\d.]+)x")
LAND_RE = re.compile(r"land held\s+([\d.]+)%")
ROW_RE = re.compile(r"^\[EVAL\]\s{4,}(.+?)\s{2,}(-?[\d.]+)\s+(-?[\d.]+)\s*$")
# "calls answered/issued  0/3   0/4" -- ROW_RE cannot take this (the '/' guard
# that keeps histogram rows out also excludes it), and it is the single most
# player-visible number there is: an ally who never comes when called.
CALLS_RE = re.compile(r"calls answered/issued\s+(\d+)/(\d+)\s+(\d+)/(\d+)")
# "said yes/was asked  0/28  30/32" -- how often anything was AGREED to.
#
# This is the counter that would have caught a live bug for months. Every AI
# answer to every request was an unconditional reject, so no alliance ever
# formed; with no alliances nobody could issue a call to arms; and with no calls
# issued the coalition gate saw "0 of 0" and correctly declined to judge an
# empty denominator. A gate cannot protect a behaviour nothing counts.
AGREE_RE = re.compile(r"said yes/was asked\s+(\d+)/(\d+)\s+(\d+)/(\d+)")
# "training      econ 21024323  politics ...  war 0  navy ..."
TRAIN_RE = re.compile(r"training\s+econ (\d+)\s+politics (\d+)\s+war (\d+)\s+navy (\d+)")
# The per-world table, e.g. "[EVAL] 1939           1      650      462     1.41x".
# ONE space after [EVAL], which is what keeps ROW_RE (four or more) from taking
# these as global metrics. The header row ends in the word ADVANTAGE rather than
# a number and so does not match.
WORLD_RE = re.compile(r"^\[EVAL\] (\S+)\s+(\d+)\s+(\d+)\s+(\d+)\s+([\d.]+)x\s*$")
# "  save   1650/13   ( 0.8%)   1650/786   (47.6%)" -- offered/chosen per action.
#
# THE COUNTERS THE REWARD CONSTANTS ARE ACTUALLY ABOUT. Every documented
# collapse in this project was a take rate going to an extreme: recruit at 98.5%
# of the turns it was offered, declare-war at 0.0% of 1827 opportunities, attack
# at 0.6%. Each was found by hand, weeks later, after the outcome numbers had
# already been blamed on something else -- because nothing parsed these rows,
# so nothing could gate them.
# "P war:recruit       15.9" -- the policy's own probability for an action, at a
# neutral temperature, over the turns the action was OFFERED. Same denominator
# as the take rate beside it, so the two are directly comparable.
#
# THIS IS WHAT THE REWARD-TERM GATES JUDGE. A take rate is what came out of the
# dice after temperature and epsilon, and measurement runs at temperature 0.35
# where the bottom of the range is crushed: measured on one model, an action the
# policy gave 2.7% was taken 0.1% of the time. Every "collapsed low" verdict
# read off a take rate was therefore overstated by more than an order of
# magnitude, and the high end was flattered too. The shape is the policy; the
# take rate is the policy seen through the sampler.
SHAPE_RE = re.compile(r"^\[EVAL\]\s+P (war|econ):(.+?)\s+([\d.]+)\s+\(n=(\d+)\)\s*$")
TAKE_SECTION_RE = re.compile(r"--\s+(econ|war) action:")
TAKE_RE = re.compile(
    r"^\[EVAL\]\s{4,}(.+?)\s+(\d+)/(\d+)\s+\(\s*([\d.]+)%\)"
    r"\s+(\d+)/(\d+)\s+\(\s*([\d.]+)%\)\s*$")


def parse(text):
    """One run -> {metric: (model, random)} plus scalars."""
    out, adv, land = {}, [], []
    ca = ci = ra = ri = 0
    ya = yi = za = zi = 0
    # Which "offered / chosen" block we are inside. The two share a row shape
    # and their action names do not overlap, but naming the section keeps a
    # future "hold" in both from silently merging into one metric.
    section = None
    # Take rates are per MAP in the output and have to be recombined over the
    # run by their own counts, not averaged: a map with four country-turns and
    # one with four hundred are not equal evidence about a preference.
    takes = {}
    shapes = {}
    for ln in text.splitlines():
        m = SHAPE_RE.match(ln.rstrip())
        if m:
            shapes[f"shape {m.group(1)}:{m.group(2).strip()}"] = float(m.group(3))
            continue
        m = TAKE_SECTION_RE.search(ln)
        if m:
            section = m.group(1)
            continue
        m = TAKE_RE.match(ln.rstrip())
        if m and section:
            key = f"take {section}:{m.group(1).strip()}"
            # Printed "offered/chosen", so groups 2 and 5 are the denominators.
            off, cho = int(m.group(2)), int(m.group(3))
            roff, rcho = int(m.group(5)), int(m.group(6))
            a = takes.setdefault(key, [0, 0, 0, 0])
            a[0] += off; a[1] += cho; a[2] += roff; a[3] += rcho
            continue
        m = CALLS_RE.search(ln)
        if m:
            ca += int(m.group(1)); ci += int(m.group(2))
            ra += int(m.group(3)); ri += int(m.group(4))
        m = AGREE_RE.search(ln)
        if m:
            ya += int(m.group(1)); yi += int(m.group(2))
            za += int(m.group(3)); zi += int(m.group(4))
        m = TRAIN_RE.search(ln)
        if m:
            for i, mod in enumerate(("econ", "politics", "war", "navy")):
                out[f"updates {mod}"] = (float(m.group(i + 1)), 0.0)
            continue
        m = WORLD_RE.match(ln.rstrip())
        if m:
            # Filed as an ordinary metric so it inherits the seed aggregation
            # and the bootstrap interval for free: "world 1939 ADVANTAGE" then
            # gets error bars across seeds exactly as the headline does.
            out[f"world {m.group(1)} ADVANTAGE"] = (float(m.group(5)), 1.0)
        m = ADV_RE.search(ln)
        if m:
            adv.append(float(m.group(1)))
        m = LAND_RE.search(ln)
        if m:
            land.append(float(m.group(1)))
        m = ROW_RE.match(ln.rstrip())
        if m:
            key = m.group(1).strip()
            if "/" in key or key.endswith("%)"):
                continue
            a, b = float(m.group(2)), float(m.group(3))
            prev = out.get(key, (0.0, 0.0))
            out[key] = (prev[0] + a, prev[1] + b)
    if adv:
        out["ADVANTAGE"] = (sum(adv) / len(adv), 1.0)
    if land:
        out["land held %"] = (sum(land) / len(land), 100.0 - sum(land) / len(land))
    if ci or ri:
        out["calls answered %"] = (100.0 * ca / ci if ci else 0.0,
                                   100.0 * ra / ri if ri else 0.0)
        # The DENOMINATOR, kept because the ratio above is meaningless without
        # it: a run that issues one call and sees it refused reports "0%
        # answered", which reads as a policy and is one coin landing tails.
        out["calls issued"] = (float(ci), float(ri))
    if yi or zi:
        out["agreements accepted %"] = (100.0 * ya / yi if yi else 0.0,
                                        100.0 * za / zi if zi else 0.0)
        out["agreements asked"] = (float(yi), float(zi))
    for key, v in shapes.items():
        # One column only: a control cohort is dice or a script and holds no
        # belief to report, so the second slot mirrors the first rather than
        # inventing a comparison.
        out[key + " %"] = (v, v)
    for key, (off, cho, roff, rcho) in takes.items():
        out[key + " %"] = (100.0 * cho / off if off else 0.0,
                           100.0 * rcho / roff if roff else 0.0)
        # The denominator, for the same reason the calls one is kept: a take
        # rate over three opportunities is not a preference.
        out[key + " offered"] = (float(off), float(roff))
    return out



class KeepAwake:
    """Hold the display awake for the lifetime of the benchmark.

    --eval-ai opens a real window: raylib's InitWindow calls into GLFW, and when
    macOS sleeps the display that call SEGFAULTS. Observed live -- a paired A/B
    measured its first arm cleanly, the screen slept, and all twelve runs of the
    second arm died inside InitWindow before a line of AI code executed. The
    tool reported two empty tables, which is a confusing way to say "your
    monitor turned off".

    caffeinate is held for the WHOLE run rather than per invocation, because the
    gaps between runs are just as capable of letting the display sleep as the
    runs themselves.
    """

    def __init__(self):
        self.proc = None

    def __enter__(self):
        if sys.platform != "darwin" or not shutil.which("caffeinate"):
            return self
        try:
            self.proc = subprocess.Popen(["caffeinate", "-dims"],
                                         stdout=subprocess.DEVNULL,
                                         stderr=subprocess.DEVNULL)
        except OSError:
            self.proc = None
        return self

    def __exit__(self, *exc):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        return False


class RunFailed(RuntimeError):
    pass


def run_one(binary, maps, turns, seed, difficulty, flags=("--vs-random",), extra=()):
    cmd = [binary, "--resource-limit", RESOURCE_LIMIT, "--eval-ai",
           str(maps), str(turns), str(seed), str(difficulty), *flags, *extra]
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="ignore")
    out = p.stdout + p.stderr
    got = parse(out)
    if not got:
        # Silently skipping this is how twelve dead runs became two empty
        # tables instead of an error. A measurement that did not happen must
        # never look like a measurement that came back uninteresting.
        tail = "\n      ".join(out.strip().splitlines()[-6:]) or "(no output)"
        hint = ""
        if p.returncode in (-11, 139):
            hint = ("\n    Segfault. If the last lines mention GLFW or Monitor, the display "
                    "slept:\n    --eval-ai opens a window and InitWindow dies when it does.")
        raise RunFailed(
            f"eval produced no [EVAL] output (exit {p.returncode})\n"
            f"    cmd: {' '.join(cmd)}{hint}\n      {tail}")
    return got


def noise_floor(binary, maps, turns, seed, difficulty, runs, flags=("--vs-random",)):
    """Run ONE configuration several times and report how much it moves.

    The eval IS reproducible now -- rand() is seeded per map rather than from
    the clock -- so this should report a spread of exactly zero, and the check
    stays because that guarantee is worth verifying rather than assuming.
    Before the fix, identical inputs produced 0.32 and 0.54, an error larger
    than most effects worth chasing, and nothing in the output said so.

    Reported before any comparison, on purpose: the reader should see the
    instrument's own error before they see the measurement. Three conclusions
    were drawn from this benchmark in a single day that its noise floor did not
    support, and none of them would have survived being printed next to it.
    """
    vals = []
    for i in range(runs):
        print(f"  [noise] repeat {i + 1}/{runs} (same seed {seed}, same everything)", flush=True)
        got = run_one(binary, maps, turns, seed, difficulty, flags)
        if "ADVANTAGE" in got:
            vals.append(got["ADVANTAGE"][0])
    if len(vals) < 2:
        return None
    mean = statistics.fmean(vals)
    spread = max(vals) - min(vals)
    sd = statistics.stdev(vals)
    return {"values": vals, "mean": mean, "sd": sd, "spread": spread,
            "rel": (spread / mean if mean else float("inf"))}


# ── Statistics ─────────────────────────────────────────────────────────────
# Below this the bootstrap stops meaning anything. Resampling from three
# numbers can only ever report their own spread, so the interval looks tight
# precisely because it has almost no information -- the failure mode is a
# CONFIDENT wrong answer, which is worse than a wide one. Observed live: two
# seeds produced [0.480, 0.590] on a measurement whose own noise floor is 0.22.
MIN_SEEDS_FOR_CI = 4


def bootstrap_ci(xs, iters=10000, alpha=0.05, seed=12345):
    """Percentile bootstrap. Small n and a skewed, ratio-valued statistic are
    exactly where a normal-theory interval quietly lies."""
    xs = list(xs)
    if len(xs) < 2:
        return (float("nan"), float("nan"))
    rng = random.Random(seed)
    n = len(xs)
    means = []
    for _ in range(iters):
        means.append(sum(xs[rng.randrange(n)] for _ in range(n)) / n)
    means.sort()
    lo = means[int(alpha / 2 * iters)]
    hi = means[min(iters - 1, int((1 - alpha / 2) * iters))]
    return (lo, hi)


def fmt_ci(mean, ci):
    return f"{mean:8.3f}  [{ci[0]:7.3f}, {ci[1]:7.3f}]"


# ── Model swapping ─────────────────────────────────────────────────────────
# --eval-ai reads data/ai/model.bin and nothing else, so measuring an arbitrary
# file means swapping it in. Always restored, including on crash or Ctrl-C --
# leaving someone else's weights in model.bin would silently corrupt every
# later run and a training pool's final merge.
class SwapModel:
    def __init__(self, path):
        self.path = path
        self.backup = None

    def __enter__(self):
        if self.path is None or os.path.abspath(self.path) == os.path.abspath(SHARED):
            return self
        if not os.path.exists(self.path):
            sys.exit(f"no such model: {self.path}")
        fd, self.backup = tempfile.mkstemp(prefix="odbench-", suffix=".bin")
        os.close(fd)
        shutil.copy2(SHARED, self.backup)
        shutil.copy2(self.path, SHARED)
        return self

    def __exit__(self, *exc):
        if self.backup:
            shutil.copy2(self.backup, SHARED)
            os.unlink(self.backup)
            self.backup = None
        return False



# ── The blunder checklist ──────────────────────────────────────────────────
#
# ADVANTAGE is the right training signal and the wrong release gate. It scores
# land held against a random-action control, and a model can hold twice that
# land by being a bankrupt warmonger that never makes peace and never
# researches -- which is a fair description of what this project measured on
# 2026-08-03 while ADVANTAGE was climbing. That reads to a player as BROKEN,
# not as strong.
#
# These are the things a player names unprompted. Each is scored against the
# RANDOM cohort from the same run, because random is the floor: it plays by
# coin flip and manages nothing. A trained model doing WORSE than a coin flip
# at keeping its books, its people, or its promises is not making a trade-off,
# it is malfunctioning -- and no ADVANTAGE figure excuses it.
#
#   name            metric                 rule              threshold
#   min_abs         model >= threshold
#   frac_of_random  model >= threshold * random
#   max_x_random    model <= threshold * random
#   band            lo <= model <= hi      (threshold is a (lo, hi) pair)
# A gate may name a metric that must reach a minimum before it is allowed to
# judge. Without that, "allies get answered 0.0%" fails loudly on a single
# refused call -- which happened, and looked like a regression from a working
# 63% until the denominator was checked. A gate that cannot tell "bad" from
# "no data" is worse than no gate: it spends attention on nothing.
BLUNDER_GATES = [
    # FIRST, because it is upstream of the coalition gate below and of half the
    # politics reward. A country that agrees to nothing has no allies, is never
    # asked to a war, and can never end one except by conquest -- and to a
    # player that is not a hard opponent, it is a broken one. The bar is
    # deliberately low: this asks whether agreement is POSSIBLE, not whether the
    # AI negotiates well.
    ("agreements are possible", "agreements accepted %", "min_abs",     15.0,
     "agreements asked", 10.0),
    ("allies get answered",     "calls answered %",   "min_abs",        20.0,
     "calls issued", 5.0),
    ("wars can end",            "ceasefires offered", "frac_of_random", 0.33),
    ("stays solvent",           "turns bankrupt",     "max_x_random",   1.0),
    ("keeps up in research",    "research completed", "frac_of_random", 0.50),
    ("holds itself together",   "lost to a revolt",   "max_x_random",   1.0),
    # NOT attacking into a fight it loses is the single most defining trait of a
    # player who has stopped being a beginner, and until the repulse counter
    # existed it could not be measured at all: attacks issued minus provinces
    # won lumps "still in progress" together with "army thrown away". Absolute,
    # because this one is a claim about competence rather than about beating a
    # control -- random loses roughly a third of its assaults, and matching
    # random here is not a defence.
    ("picks its fights",        "% of assaults that lost", "max_abs",   35.0,
     "attacks repulsed", 10.0),
    # AN INVARIANT, not a quality bar. The AI may lie about why it refused --
    # that is the mechanic -- but it must never state something the country it
    # is talking to can disprove by looking at the map. One of those is an
    # opponent keeping something back; the other is an opponent that has not
    # noticed what you can see, which reads as broken rather than as devious.
    # Must be exactly zero; anything else means the believability rule is off.
    ("lies hold up",            "refusals: caught out",    "max_abs",    0.0,
     "refusals: lied", 3.0),
    # The same invariant for declarations. A pretext the victim can disprove by
    # looking at the claims map is not cunning, it is the AI not having looked.
    ("pretexts hold up",        "wars: caught out",        "max_abs",    0.0,
     "wars: pretext", 3.0),
]

# ── One gate per reward term that has already broken once ──────────────────
#
# WHY THESE EXIST. The reward is about twenty hand-set constants, and the
# comments beside them record a cycle: a term is added, the policy collapses
# onto whatever it overpays for, the term is reshaped, and the reshaping breaks
# an earlier one. Army growth rewarded unconditionally -> recruit 14,849 times
# and attack 214. A gate added -> the gate was true every turn for a defender ->
# recruit on 98.5% of offers. A flat charge for declaring war -> zero
# declarations out of 1,827 opportunities. An entropy bump to "fix" a low attack
# rate -> more attacks winning less ground.
#
# Every one of those was a TAKE RATE at an extreme, every one was found by hand
# weeks later, and none of them was visible in ADVANTAGE at the time. So each
# constant now owns a counter, and each counter has a band.
#
# BANDS, not minimums, because these fail in both directions and the failure is
# the same failure: a policy that always picks an action has stopped choosing
# just as surely as one that never picks it. The bands are deliberately wide --
# this asks whether a module is still making a decision, not whether it is
# making a good one. A number in-band is not praise; a number out of band is a
# collapsed distribution and no amount of further training walks it back
# without a --reset-ai-head.
#
# The denominator guard matters as much here as anywhere: an action offered
# twice in a run has no take rate worth reading.
REWARD_TERM_GATES = [
    # JUDGED ON THE POLICY SHAPE, not the take rate. See SHAPE_RE: the take rate
    # is read at temperature 0.35, which crushes the bottom of the range -- an
    # action the policy gave 2.7% was taken 0.1% of the time on a measured model
    # -- so every low-end verdict taken off one was overstated by more than an
    # order of magnitude. The shape is what the net believes; the take rate is
    # that belief after the sampler has had its say. Both are reported; only
    # this one is gated.
    #
    # Bands are WIDE and deliberately so. This asks whether a module is still
    # making a decision, not whether it is making a good one.
    #
    # name                    metric                     rule    band          denominator                     min n   owning constant
    ("army is a means",        "shape war:recruit %",     "band", (2.0, 80.0),  "take war:recruit offered",     200.0, "ARMY_SUFFICIENCY / ARMY_PROGRESS_WEIGHT"),
    ("still presses attacks",  "shape war:attack %",      "band", (2.0, 90.0),  "take war:attack offered",      200.0, "dProv weight, PPO_ENTROPY"),
    ("wars still get started", "shape war:declare war %", "band", (0.5, 70.0),  "take war:declare war offered", 200.0, "IDLE_CHARGE, WAR_AGGRESSION_CHARGE"),
    ("wars still get ended",   "shape war:ceasefire %",   "band", (1.0, 85.0),  "take war:ceasefire offered",   100.0, "WAR_END_REWARD, IDLE_CHARGE"),
    ("economy does something", "shape econ:save %",       "band", (0.0, 85.0),  "take econ:save offered",       200.0, "the econIdle charge"),
    ("research keeps funding", "shape econ:fund down %",  "band", (0.0, 70.0),  "take econ:fund down offered",  200.0, "dNet with research added back"),
]


def check_blunders(model_vals, random_vals, gates=None):
    """-> list of (name, ok, detail). Missing metrics are reported, not skipped:
    a gate that silently vanishes is a gate that stops protecting anything."""
    rows = []
    for gate in (BLUNDER_GATES if gates is None else gates):
        name, metric, rule, thr = gate[:4]
        need_metric, need_n = (gate[4], gate[5]) if len(gate) > 5 else (None, 0.0)
        mv, rv = model_vals.get(metric), random_vals.get(metric)
        if not mv:
            rows.append((name, None, f"{metric}: not reported by this build"))
            continue
        if need_metric:
            seen = model_vals.get(need_metric)
            n = statistics.fmean(seen) if seen else 0.0
            if n < need_n:
                rows.append((name, None,
                             f"only {n:.1f} {need_metric} per run (need {need_n:.0f}"
                             f" to judge) -- no data, not a failure"))
                continue
        m = statistics.fmean(mv)
        r = statistics.fmean(rv) if rv else 0.0
        if rule == "band":
            lo, hi = thr
            ok = lo <= m <= hi
            where = "collapsed low" if m < lo else ("collapsed high" if m > hi else "")
            detail = f"{metric} {m:.1f} (want {lo:.0f}-{hi:.0f})"
            if where:
                detail += f" -- {where}"
        elif rule == "min_abs":
            ok = m >= thr
            detail = f"{metric} {m:.1f} (need >= {thr:.0f})"
        elif rule == "max_abs":
            ok = m <= thr
            detail = f"{metric} {m:.1f} (need <= {thr:.0f})"
        elif rule == "frac_of_random":
            ok = m >= thr * r
            detail = f"{metric} {m:.1f} vs random {r:.1f} (need >= {thr:.0%} of it)"
        else:
            ok = m <= thr * r
            detail = f"{metric} {m:.1f} vs random {r:.1f} (need <= {thr:.0%} of it)"
        rows.append((name, ok, detail))
    return rows


def measure(binary, model, seeds, turns_list, maps, difficulty, label, repeat=1,
            flags=("--vs-random",)):
    """-> {turns: {metric: [per-seed values]}}

    With repeat > 1 each seed is run several times and AVERAGED before it
    becomes a data point. That is the only defence against the run-to-run
    nondeterminism documented in noise_floor(): averaging k runs shrinks that
    component by sqrt(k), while leaving the seed-to-seed spread -- which is
    real signal about generalisation -- untouched.
    """
    res = {t: {} for t in turns_list}
    rnd = {t: {} for t in turns_list}
    with SwapModel(model):
        for t in turns_list:
            for i, s in enumerate(seeds, 1):
                runs = []
                for r in range(repeat):
                    tag = f" rep {r + 1}/{repeat}" if repeat > 1 else ""
                    print(f"  [{label}] {t}t seed {s} ({i}/{len(seeds)}){tag}", flush=True)
                    runs.append(run_one(binary, maps, t, s, difficulty, flags))
                keys = set().union(*(r.keys() for r in runs))
                for k in keys:
                    vals = [r[k][0] for r in runs if k in r]
                    rv = [r[k][1] for r in runs if k in r]
                    if vals:
                        res[t].setdefault(k, []).append(statistics.fmean(vals))
                    if rv:
                        rnd[t].setdefault(k, []).append(statistics.fmean(rv))
    return res, rnd


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seeds", type=int, default=6, help="number of seeds (default 6)")
    ap.add_argument("--turns", type=int, nargs="+", default=[300, 400],
                    help="horizons to measure (default: 300 400)")
    ap.add_argument("--maps", type=int, default=2)
    ap.add_argument("--difficulty", type=int, default=2)
    ap.add_argument("--model", default=None, help="model to measure (default data/ai/model.bin)")
    ap.add_argument("--compare", default=None, help="baseline model for a paired A/B")
    ap.add_argument("--json", default=None, help="write full results here")
    ap.add_argument("--repeat", type=int, default=1,
                    help="runs per seed, averaged (shrinks run noise by sqrt(n))")
    ap.add_argument("--gate", action="store_true",
                    help="exit 2 if any blunder check fails (for release gating)")
    ap.add_argument("--binary", default=None,
                    help="explicit binary path (default: the newest build tree)")
    ap.add_argument("--noise-runs", type=int, default=6,
                    help="repeats of one config to measure the noise floor (0 to skip)")
    ap.add_argument("--vs-model", default=None, metavar="PATH",
                    help="control cohort plays this model instead of picking at "
                         "random (a rung, where random is only a floor)")
    ap.add_argument("--vs-script", action="store_true",
                    help="control cohort plays the scripted rung -- a competent "
                         "hand-written player. The first control that stands for "
                         "a LEVEL rather than a floor; parity with it is the target")
    ap.add_argument("--scenarios", action="store_true",
                    help="measure on the maps data/STDmaps ships (1914 .. modern) "
                         "instead of generated archetypes")
    args = ap.parse_args()

    # Everything that decides WHAT is being measured, threaded as one tuple and
    # printed in the header, because two runs that differ in either the control
    # or the worlds are not comparable however similar their headers look.
    if args.vs_model and not os.path.exists(args.vs_model):
        sys.exit(f"no such opponent model: {args.vs_model}")
    if args.vs_model and args.vs_script:
        sys.exit("--vs-model and --vs-script name different control groups; choose one")
    flags = (("--vs-model", args.vs_model) if args.vs_model
             else ("--vs-script",) if args.vs_script
             else ("--vs-random",))
    if args.scenarios:
        flags += ("--scenarios",)

    if args.seeds > len(SEED_POOL):
        sys.exit(f"--seeds max is {len(SEED_POOL)}")
    seeds = SEED_POOL[:args.seeds]
    binary = args.binary or find_binary()
    if not os.path.exists(binary):
        sys.exit(f"no such binary: {binary}")
    import datetime
    built = datetime.datetime.fromtimestamp(os.path.getmtime(binary)).strftime("%Y-%m-%d %H:%M:%S")
    print(f"binary : {os.path.relpath(binary, ROOT)}  (built {built})")
    print(f"seeds  : {seeds}")
    print(f"turns  : {args.turns}   maps/seed: {args.maps}   difficulty: {args.difficulty}")
    print(f"limit  : --resource-limit {RESOURCE_LIMIT} (pinned)")
    print("control: " + (os.path.relpath(args.vs_model, ROOT) if args.vs_model
                         else "the scripted rung" if args.vs_script
                         else "random moves"))
    # The shipped list is six worlds and a run walks it with `--maps`, so
    # anything under six measures a PREFIX of it -- 1914 and 1918 only, at the
    # default of two. Saying so beats letting "--scenarios" read as "all of
    # them" in a log somebody reads next month.
    if args.scenarios:
        print(f"worlds : shipped scenarios, first {args.maps} of 6 per seed "
              f"(--maps 6 covers the list)")
    else:
        print(f"worlds : generated archetypes")
    if len(seeds) < MIN_SEEDS_FOR_CI:
        print(f"\n  !! {len(seeds)} seeds. Every interval below is decorative: a bootstrap over")
        print(f"  !! fewer than {MIN_SEEDS_FOR_CI} points reports the spread of those points and calls it")
        print(f"  !! a confidence interval. Use --seeds {MIN_SEEDS_FOR_CI} or more for anything you intend")
        print(f"  !! to act on.")
    print(f"runs   : {len(seeds) * len(args.turns) * args.repeat * (2 if args.compare else 1)}"
          f" + {args.noise_runs} noise\n")

    # THE INSTRUMENT'S OWN ERROR, BEFORE THE MEASUREMENT. Everything below is
    # read against this number.
    noise = None
    if args.noise_runs >= 2:
        try:
            with KeepAwake(), SwapModel(args.model):
                noise = noise_floor(binary, args.maps, max(args.turns), seeds[0],
                                    args.difficulty, args.noise_runs, flags)
        except RunFailed as e:
            print(f"\nABORTED during the noise probe: {e}", file=sys.stderr)
            return 3
        if noise:
            print(f"\n  NOISE FLOOR ({args.noise_runs} identical runs, seed {seeds[0]}, "
                  f"{max(args.turns)}t)")
            print(f"    ADVANTAGE {['%.3f' % v for v in noise['values']]}")
            print(f"    mean {noise['mean']:.3f}  sd {noise['sd']:.3f}  "
                  f"spread {noise['spread']:.3f} ({100 * noise['rel']:.0f}% of mean)")
            if noise["rel"] > NOISE_WARN_FRACTION:
                print(f"    !! NOT REPRODUCIBLE. Identical inputs disagree by "
                      f"{100 * noise['rel']:.0f}%.")
                print(f"    !! Treat any difference below ~{noise['spread']:.2f} ADVANTAGE as "
                      f"nothing, whatever the interval says,")
                print(f"    !! and raise --repeat to average it down.")
            print()

    try:
        with KeepAwake():
            curr, curr_rnd = measure(binary, args.model, seeds, args.turns, args.maps,
                                     args.difficulty, "curr", args.repeat, flags)
            base = None
            if args.compare:
                # The SAME opponent for both arms. Pairing already cancels map
                # difficulty; holding the control fixed is what makes the two
                # arms' ADVANTAGE figures differences in play rather than in who
                # they played.
                base, _ = measure(binary, args.compare, seeds, args.turns, args.maps,
                                  args.difficulty, "base", args.repeat, flags)
    except RunFailed as e:
        print(f"\nABORTED: {e}", file=sys.stderr)
        return 3

    regressed = False
    payload = {"seeds": seeds, "turns": args.turns, "repeat": args.repeat,
               "resource_limit": RESOURCE_LIMIT, "noise": noise,
               "control": args.vs_model or ("script" if args.vs_script else "random"),
               "worlds": "shipped" if args.scenarios else "generated",
               "current": {}, "baseline": {}, "paired": {}}

    for t in args.turns:
        print(f"\n{'=' * 78}\n{t} TURNS   ({len(seeds)} seeds x {args.maps} maps)\n{'=' * 78}")
        if not base:
            print(f"{'metric':<28}{'mean':>10}  {'95% CI':>18}")
            for k in sorted(curr[t], key=lambda x: (x != "ADVANTAGE", x)):
                vals = curr[t][k]
                if len(vals) < 2:
                    continue
                m = statistics.fmean(vals)
                print(f"{k:<28}{fmt_ci(m, bootstrap_ci(vals))}")
                payload["current"].setdefault(str(t), {})[k] = vals
        else:
            print(f"{'metric':<26}{'base':>9}{'curr':>9}{'delta':>9}  {'paired 95% CI':>20}")
            keys = [k for k in curr[t] if k in base[t]]
            for k in sorted(keys, key=lambda x: (x != "ADVANTAGE", x)):
                cv, bv = curr[t][k], base[t][k]
                n = min(len(cv), len(bv))
                if n < 2:
                    continue
                # PAIRED: difference on the same seed, same maps. Map difficulty
                # is the dominant variance term and this removes it entirely.
                diffs = [cv[i] - bv[i] for i in range(n)]
                d = statistics.fmean(diffs)
                ci = bootstrap_ci(diffs)
                sig = "  *" if (ci[0] > 0 or ci[1] < 0) else ""
                # ADVANTAGE is additionally held against the instrument's own
                # error. An interval can exclude zero while the effect is
                # smaller than the gap between two identical runs -- that is
                # not a finding, it is the noise floor with a p-value stapled
                # to it.
                if k == "ADVANTAGE" and noise and abs(d) < noise["spread"]:
                    sig = "  ~"
                print(f"{k:<26}{statistics.fmean(bv):>9.2f}{statistics.fmean(cv):>9.2f}"
                      f"{d:>9.2f}  [{ci[0]:8.2f},{ci[1]:8.2f}]{sig}")
                payload["paired"].setdefault(str(t), {})[k] = {
                    "base": bv[:n], "curr": cv[:n], "delta": d, "ci": list(ci)}
                if k == "ADVANTAGE" and ci[1] < 0 and (not noise or abs(d) >= noise["spread"]):
                    regressed = True
            print("\n  * = paired 95% CI excludes zero (change is larger than seed noise)")
            if noise:
                print(f"  ~ = ADVANTAGE moved less than the noise floor "
                      f"({noise['spread']:.2f}); not evidence of anything")

    # THE BLUNDER CHECKLIST, at the longest horizon measured -- the failures it
    # looks for compound, and a 300-turn run ends before most of them bite.
    longest = max(args.turns)
    rows = check_blunders(curr[longest], curr_rnd[longest])
    print(f"\n{'=' * 78}\nBLUNDER CHECKLIST ({longest} turns)   "
          f"-- what a player notices, regardless of ADVANTAGE\n{'=' * 78}")
    if args.vs_model:
        # Four of the five gates are RELATIVE to the control column, and the
        # control is no longer a coin flip. "at least a third of random's
        # ceasefires" is a floor because random manages nothing; "at least a
        # third of that model's" is a comparison with a player, and a run can
        # pass it by facing a bad opponent. Only "allies get answered" is
        # absolute and means the same thing in both modes.
        print(f"  NOTE: the control is {os.path.relpath(args.vs_model, ROOT)}, not random.")
        print(f"  Every gate below phrased 'vs random' is really 'vs that model',")
        print(f"  which is a comparison rather than a floor. Re-read them accordingly.")
    for name, ok, detail in rows:
        mark = "PASS" if ok else ("FAIL" if ok is False else "  ? ")
        print(f"  [{mark}]  {name:<24} {detail}")
    failed = [n for n, ok, _ in rows if ok is False]
    payload["blunders"] = [{"name": n, "ok": ok, "detail": d} for n, ok, d in rows]
    if failed:
        print(f"\n  {len(failed)} of {len(rows)} failed: {', '.join(failed)}")
        print("  A model can score well on ADVANTAGE and still fail these. They are")
        print("  what makes an AI read as broken rather than merely weak.")
    else:
        print("\n  All clear.")

    # ── One gate per reward term ──
    # The checklist above is what a PLAYER notices. This is what a reward
    # constant does when it goes wrong, caught at the level it goes wrong at: a
    # take rate pinned to an extreme, which is what every documented collapse in
    # this project turned out to be, and which none of them was visible as until
    # somebody went looking weeks later.
    # A FRESHLY RESET HEAD IS NOT A COLLAPSED ONE.
    #
    # An untrained head has arbitrary logits, and at the hard-difficulty
    # temperature of 0.35 it will still concentrate on whichever action
    # initialisation happened to favour -- so it reads exactly like a converged
    # one in the take rate, and the two need opposite responses. Below this many
    # updates the module's gates are reported as "no data" rather than as
    # failures. Roughly the point at which the epsilon schedule has annealed
    # meaningfully; before it, the take rate is mostly initialisation.
    MIN_UPDATES_TO_JUDGE = 2_000_000

    def head_updates(metric):
        """Which policy head owns this metric, and how trained it is."""
        for mod in ("war", "econ"):
            if metric.startswith(f"take {mod}:") or metric.startswith(f"shape {mod}:"):
                vals = curr[longest].get(f"updates {mod}")
                return mod, (statistics.fmean(vals) if vals else 0.0)
        return None, None

    trows = []
    for gate, (name, ok, detail) in zip(
            REWARD_TERM_GATES,
            check_blunders(curr[longest], curr_rnd[longest], REWARD_TERM_GATES)):
        mod, upd = head_updates(gate[1])
        if mod is not None and upd < MIN_UPDATES_TO_JUDGE:
            trows.append((name, None,
                          f"{mod} head has {upd / 1e6:.1f}M updates (need "
                          f"{MIN_UPDATES_TO_JUDGE / 1e6:.0f}M to tell a collapse "
                          f"from a fresh reset) -- no data, not a failure"))
        else:
            trows.append((name, ok, detail))
    owners = {g[0]: g[6] for g in REWARD_TERM_GATES}
    print(f"\n{'=' * 78}\nREWARD TERMS ({longest} turns)   "
          f"-- is each module still making a choice\n{'=' * 78}")
    for name, ok, detail in trows:
        mark = "PASS" if ok else ("FAIL" if ok is False else "  ? ")
        print(f"  [{mark}]  {name:<24} {detail}")
        if ok is False:
            print(f"           owned by: {owners.get(name, '?')}")
    tfailed = [n for n, ok, _ in trows if ok is False]
    payload["reward_terms"] = [{"name": n, "ok": ok, "detail": d, "owner": owners.get(n)}
                               for n, ok, d in trows]
    if tfailed:
        print(f"\n  {len(tfailed)} of {len(trows)} collapsed: {', '.join(tfailed)}")
        print("  A collapsed distribution does not recover with more training --")
        print("  the constant needs correcting and the head needs --reset-ai-head.")
    else:
        print("\n  Every module is still choosing.")
    failed = failed + tfailed

    if args.json:
        with open(args.json, "w") as f:
            json.dump(payload, f, indent=1)
        print(f"\nwrote {args.json}")

    if regressed:
        print("\nREGRESSION: ADVANTAGE is significantly worse than the baseline.")
        return 1
    if failed and args.gate:
        print("\nGATED: blunder checks failed.")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
